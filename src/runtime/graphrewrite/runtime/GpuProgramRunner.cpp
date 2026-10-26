#include "graphrewrite/runtime/GpuProgramRunner.hpp"

#include "graphrewrite/runtime/Dice.hpp"
#include "graphrewrite/runtime/RuleRows.hpp"

#include "rendering/gpgpu/CopyBufferPass.hpp"

#include "graphrewrite/runtime/PlanCheck.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <set>
#include <cstring>

namespace graphrewrite::runtime {

GpuProgramRunner::GpuProgramRunner(::rendering::RHI *device, const compiler::CompiledProgram &compiled,
                                   const compiler::UsageValues &usageValues, EdgePageStore &pageStore, GpuRunnerSettings runnerSettings)
    : rhi(device), program(compiled), usage(usageValues), store(pageStore), settings(runnerSettings)
{
}

GpuProgramRunner::~GpuProgramRunner() = default;

void GpuProgramRunner::seed(const std::vector<Fact> &facts)
{
  for (const Fact &fact : facts)
  {
    store.addEdge(std::get<1>(fact), Edge{std::get<0>(fact), std::get<2>(fact)});
  }
}

void GpuProgramRunner::createResources()
{
  ::rendering::RenderGraph::Settings graphSettings;

  graphSettings.enableAutomaticNodeTimers = settings.enableTimers;

  graph = std::make_unique<::rendering::RenderGraph>(rhi, graphSettings);

  GpuPagePoolSettings poolSettings;

  // The budget is a ceiling, not an allocation. A graph whose pages all fit in
  // far less than the budget should not reserve the budget: the pool is the
  // largest buffer this system owns, and on a machine that has been driven into
  // memory starvation before, reserving two gigabytes to hold four megabytes of
  // edges is how a run takes the compositor down with it.
  //
  // Headroom above the current page count is for what a rewrite inserts, which
  // allocates pages as it goes.
  uint64_t storePages = 0u;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    storePages += store.partitionAt(partition).pageCount();
  }

  const uint64_t wanted = std::max<uint64_t>(64u, storePages + storePages / 2u);

  poolSettings.slotCount = static_cast<uint32_t>(std::min<uint64_t>(settings.slotCount, wanted));

  pagePool = std::make_unique<GpuPagePool>(*graph, poolSettings);
  pagePool->configure(store);

  streamingManager = std::make_unique<StreamingManager>(*pagePool, store, usage);

  std::vector<EdgeTypeId> partitionTypes;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    partitionTypes.push_back(store.partitionAt(partition).edgeType());
  }

  streamingManager->configure(partitionTypes);

  // Which registers hold a rule's final table: the last Select/Where that
  // serves the rule. Their regions are never reused within a match.
  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    uint32_t finalRegister = ~0u;

    for (const compiler::Instruction &instruction : program.instructions)
    {
      if ((instruction.op == compiler::Op::Select || instruction.op == compiler::Op::Where) &&
          std::find(instruction.rules.begin(), instruction.rules.end(), rule) != instruction.rules.end())
      {
        finalRegister = instruction.reg;
      }
    }

    if (finalRegister != ~0u)
    {
      pinnedRegisters.insert(finalRegister);
    }
  }

  // How many table regions can be live at once: a region is freed by
  // FreeTable and reused, so the buffer needs the PEAK, not one per
  // instruction. Asking for more is not free - the first frame pays for
  // first-touch of the whole buffer (1 GB costs ~220 ms, 64 MB ~20 ms), and
  // that was most of the run's slowest frame.
  {
    uint32_t widest = 2u;

    for (uint32_t thread = 0u; thread < static_cast<uint32_t>(program.threadStarts.size()); ++thread)
    {
      widest = std::max(widest, columnsForThread(thread));
    }

    std::set<uint32_t> live;
    uint32_t peak = 1u;

    for (const compiler::Instruction &instruction : program.instructions)
    {
      if (instruction.op == compiler::Op::Select || instruction.op == compiler::Op::Where)
      {
        live.insert(instruction.reg);
        peak = std::max(peak, static_cast<uint32_t>(live.size()));
      }
      else if (instruction.op == compiler::Op::FreeTable && (!pinFinalTables || pinnedRegisters.count(instruction.from) == 0u))
      {
        live.erase(instruction.from);
      }
    }

    // Four spare regions. Two was the static peak plus a working slot, and a
    // run at two hundred thousand still walked past it - the static walk is
    // a lower bound on what the free list hands out, not the truth.
    const uint64_t needed = static_cast<uint64_t>(widest) * settings.rowCapacityPerTable * (peak + 4u);

    // Zero means "size it from the programme". Anything else is taken at face
    // value: a caller capping memory on purpose gets what it asked for, and
    // the overflow it may cause is reported rather than silent. Taking the
    // SMALLER of the two was a silent catastrophe - a 67 MB buffer against a
    // 616 MB need overflowed every register, and a whole 200k year ran in
    // 0.02 s with nothing happening.
    if (settings.tableWords == 0u)
    {
      settings.tableWords = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(needed, 1u << 16u), 0xFFFFFFFFull));
    }
  }

  graphrewrite::rendering::GraphMatchPass::Settings passSettings;

  passSettings.pool = pagePool.get();
  passSettings.maxRows = settings.maxRows;
  passSettings.tableWords = settings.tableWords;
  passSettings.maxPagesPerPartition = settings.maxPagesPerPartition;
  passSettings.maxRegisters = settings.maxRegisters;
  passSettings.edgesPerThread = settings.edgesPerThread;

  matchPass = graph->registerPass<graphrewrite::rendering::GraphMatchPass>("graphrewrite.match", 0u, passSettings);

  statusStaging = graph->createBuffer(::rendering::BufferInfo{
      .name = "graphrewrite.statusStaging",
      .size = sizeof(graphrewrite::rendering::MatchStatus),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  requestStaging = graph->createBuffer(::rendering::BufferInfo{
      .name = "graphrewrite.requestStaging",
      .size = pagePool->requestBytes(),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  tableStaging = graph->createBuffer(::rendering::BufferInfo{
      .name = "graphrewrite.tableStaging",
      .size = matchPass->tableBytes(),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  graph->registerPass<::rendering::gpgpu::CopyBufferPass>("graphrewrite.copyStatus", 1u, matchPass->statusBuffer(), 0u,
                                                          sizeof(graphrewrite::rendering::MatchStatus), statusStaging, 0u,
                                                          sizeof(graphrewrite::rendering::MatchStatus));

  graph->registerPass<::rendering::gpgpu::CopyBufferPass>("graphrewrite.copyRequests", 2u, pagePool->requestBuffer(), 0u,
                                                          pagePool->requestBytes(), requestStaging, 0u, pagePool->requestBytes());

  graphrewrite::rendering::GraphRewritePass::Settings rewriteSettings;

  rewriteSettings.pool = pagePool.get();
  rewriteSettings.tableBuffer = matchPass->readTable();
  rewriteSettings.tableBytes = matchPass->tableBytes();
  rewriteSettings.maxRows = settings.maxRows;
  rewriteSettings.deltaCapacity = settings.deltaCapacity;
  rewriteSettings.dirtyCapacity = settings.dirtyCapacity;
  rewriteSettings.candidateCapacity = settings.candidateCapacity;
  rewriteSettings.firedCapacity = settings.firedCapacity;
  rewriteSettings.claimEntries = settings.claimEntries;
  rewriteSettings.ctrlStride = 1u + settings.maxFreshVertices;

  rewritePass = graph->registerPass<graphrewrite::rendering::GraphRewritePass>("graphrewrite.rewrite", 3u, rewriteSettings);

  const auto pull = [this](const char *name, const uint64_t size) {
    return graph->createBuffer(::rendering::BufferInfo{
        .name = name,
        .size = size,
        .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
  };

  countersStaging = pull("graphrewrite.countersStaging", sizeof(graphrewrite::rendering::RewriteCounters));
  dirtyStaging = pull("graphrewrite.dirtyStaging", rewritePass->dirtyPagesBytes());
  deltaStaging = pull("graphrewrite.deltaStaging", rewritePass->deltaBytes());
  poolStaging = pull("graphrewrite.poolStaging", rewritePass->dirtyEdgeBytes());

  copyTablesPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyTables", 4u, matchPass->readTable(), 0u, matchPass->tableBytes(), tableStaging, 0u, matchPass->tableBytes());

  // The status word is small and read every tick; everything else is large and
  // only wanted around a rewrite.
  graph->registerPass<::rendering::gpgpu::CopyBufferPass>("graphrewrite.copyCounters", 5u, rewritePass->countersBuffer(), 0u,
                                                          sizeof(graphrewrite::rendering::RewriteCounters), countersStaging, 0u,
                                                          sizeof(graphrewrite::rendering::RewriteCounters));

  copyDirtyPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyDirty", 6u, rewritePass->dirtyPagesBuffer(), 0u, rewritePass->dirtyPagesBytes(), dirtyStaging, 0u,
      rewritePass->dirtyPagesBytes());

  copyDeltaPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyDelta", 7u, rewritePass->deltaBuffer(), 0u, rewritePass->deltaBytes(), deltaStaging, 0u, rewritePass->deltaBytes());

  copyPoolPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyPool", 8u, rewritePass->dirtyEdgesBuffer(), 0u, rewritePass->dirtyEdgeBytes(), poolStaging, 0u,
      rewritePass->dirtyEdgeBytes());

  candidatesStaging = pull("graphrewrite.candidatesStaging", rewritePass->candidatesBytes());
  firedStaging = pull("graphrewrite.firedStaging", rewritePass->firedBytes());

  copyCandidatesPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyCandidates", 9u, rewritePass->candidatesBuffer(), 0u, rewritePass->candidatesBytes(), candidatesStaging, 0u,
      rewritePass->candidatesBytes());

  copyFiredPass = graph->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "graphrewrite.copyFired", 10u, rewritePass->firedBuffer(), 0u, rewritePass->firedBytes(), firedStaging, 0u,
      rewritePass->firedBytes());

  copyTablesPass->setEnabled(settings.enableTableReadback);

  setRewriteReadbackEnabled(false, false);

  graph->compile();

  // The rewrite pass is recorded in the same graph as the match pass, so it runs
  // on every tick. A zero row count makes every one of its stages inert.
  rewritePass->setParams(graphrewrite::rendering::RewriteParams{});

  uint32_t widest = 2u;

  for (uint32_t thread = 0u; thread < static_cast<uint32_t>(program.threadStarts.size()); ++thread)
  {
    widest = std::max(widest, columnsForThread(thread));
  }

  regionWords = widest * settings.rowCapacityPerTable;

  {
    const std::vector<uint32_t> zeroRows(settings.maxRegisters, 0u);

    graph->bufferWrite(matchPass->tableRowsBuffer(), 0u, zeroRows.size() * sizeof(uint32_t),
                       const_cast<uint32_t *>(zeroRows.data()));
  }

  zeroRequests.assign(pagePool->requestBytes() / sizeof(uint32_t), 0u);
  zeroRowState.assign(matchPass->rowStateBytes() / sizeof(uint32_t), 0u);
}

uint32_t GpuProgramRunner::partitionIndexOf(const Partition &partition) const
{
  for (uint32_t i = 0u; i < store.partitionCount(); ++i)
  {
    if (&store.partitionAt(i) == &partition)
    {
      return i;
    }
  }

  return 0u;
}

void GpuProgramRunner::setMatchCallback(MatchCallback callback)
{
  matchCallback = std::move(callback);
}

void GpuProgramRunner::setRewriteReadbackEnabled(const bool candidates, const bool fold)
{
  if (copyDirtyPass != nullptr)
  {
    copyDirtyPass->setEnabled(fold);
    copyDeltaPass->setEnabled(fold);
    copyPoolPass->setEnabled(fold);
    copyFiredPass->setEnabled(fold);
    copyCandidatesPass->setEnabled(candidates);
  }
}

void GpuProgramRunner::reset()
{
  registers.clear();
  freeRegions.clear();
  cursor = 0u;
  nextRegion = 0u;
  instructionStarted = false;
  producedRows = 0u;
  runnerStats = RunnerStats();
}

bool GpuProgramRunner::complete() const
{
  return cursor >= program.instructions.size();
}

uint32_t GpuProgramRunner::columnsForThread(const uint32_t thread) const
{
  uint32_t highest = 1u;

  for (const compiler::Instruction &other : program.instructions)
  {
    if (other.thread != thread || (other.op != compiler::Op::Select && other.op != compiler::Op::Where))
    {
      continue;
    }

    highest = std::max(highest, std::max(other.fromColumn, other.writeColumn) + 1u);
  }

  return highest;
}

void GpuProgramRunner::beginInstruction(const uint32_t index)
{
  const compiler::Instruction &instruction = program.instructions[index];

  const bool isSelect = instruction.op == compiler::Op::Select;

  const uint32_t columns = columnsForThread(instruction.thread);

  RegisterTable output;

  if (!freeRegions.empty())
  {
    output.wordBase = freeRegions.back();
    freeRegions.pop_back();
  }
  else
  {
    output.wordBase = nextRegion++ * regionWords;
  }

  runnerStats.regionsAvailable = static_cast<uint32_t>(matchPass->tableBytes() / sizeof(uint32_t) / std::max(1u, regionWords));
  runnerStats.peakRegionsUsed = std::max(runnerStats.peakRegionsUsed, nextRegion);

  if (static_cast<uint64_t>(output.wordBase) + regionWords > matchPass->tableBytes() / sizeof(uint32_t))
  {
    // Past the table buffer: the kernel would write beyond its binding. Say
    // so rather than let the match silently lose every row of this register.
    runnerStats.overflowed = true;
    output.overflowed = true;
    output.wordBase = 0u;
  }

  output.columns = columns;

  RegisterTable input;

  if (!isSelect)
  {
    const auto entry = registers.find(instruction.from);

    if (entry != registers.end())
    {
      input = entry->second;
    }
  }

  const KeyOrder order = instruction.direction == compiler::StepDirection::Backward ? KeyOrder::ByTarget : KeyOrder::ByOrigin;

  params = graphrewrite::rendering::MatchParams();

  params.mode = isSelect ? 0u : 1u;
  params.direction = static_cast<uint32_t>(instruction.direction);
  params.fromColumn = instruction.fromColumn;
  params.writeColumn = instruction.writeColumn;
  params.writeCols = columns;
  params.writeCapacity = settings.rowCapacityPerTable;
  params.readBase = input.wordBase;
  params.writeWordBase = output.wordBase;
  params.readRegister = isSelect ? instruction.reg : instruction.from;
  params.writeRegister = instruction.reg;
  params.firstFrame = 1u;
  params.pageEdges = pageEdges;
  params.blockSumsBase = matchPass->blockSumsBase();

  const bool cyclic = instruction.direction == compiler::StepDirection::Cyclic;

  output.boundMask = input.boundMask | (1u << instruction.fromColumn) | (cyclic ? 0u : (1u << instruction.writeColumn));

  params.boundMask = input.boundMask;

  if (store.has(instruction.label, order))
  {
    params.partition = partitionIndexOf(store.partition(instruction.label, order));
  }
  else
  {
    // No partition for this type: leave the instruction pointing at an empty
    // register so fill_dispatch_args launches nothing.
    params.mode = 1u;
    params.readRegister = settings.maxRegisters - 1u;
  }

  params.readCols = isSelect ? 0u : input.columns;
  params.readStride = settings.rowCapacityPerTable;

  // Writing a register that still holds a region - one reused before its
  // FreeTable - abandoned that region, so the allocator outgrew the peak the
  // table buffer was sized for and the far end of the programme overflowed.
  {
    const auto previous = registers.find(instruction.reg);

    if (previous != registers.end() && !previous->second.overflowed && previous->second.wordBase != input.wordBase &&
        previous->second.wordBase != output.wordBase)
    {
      freeRegions.push_back(previous->second.wordBase);
    }
  }

  registers[instruction.reg] = output;

  // How many rows this instruction will iterate: pages for a Select, the input
  // table's rows for a Where.
  uint32_t plannedRows = input.rows;

  if (isSelect && store.has(instruction.label, order))
  {
    plannedRows = store.partition(instruction.label, order).pageCount();
  }

  producedRows = 0u;
  instructionStarted = true;
  plannedRowsThisInstruction = output.overflowed ? 0u : plannedRows;

  // Only the rows this instruction will touch, plus their block sums. Clearing
  // the whole capacity is tens of megabytes of writes for a table that may hold
  // a handful of rows.
  const uint32_t usedRows = std::min(plannedRows, settings.maxRows);
  const uint32_t stateWords = usedRows * 8u;
  const uint32_t blockWords = (usedRows + 255u) / 256u + 1u;

  // An instruction over an empty input has no rows to clear; a zero-byte map
  // is what MoltenVK reports as "memory is not mapped" on the unmap.
  if (stateWords != 0u)
  {
    graph->bufferWrite(matchPass->rowState(), 0u, stateWords * sizeof(uint32_t), zeroRowState.data());
  }

  graph->bufferWrite(matchPass->rowState(), matchPass->blockSumsBase() * sizeof(uint32_t), blockWords * sizeof(uint32_t),
                     zeroRowState.data());
  graph->bufferWrite(pagePool->requestBuffer(), 0u, pagePool->requestBytes(), zeroRequests.data());
}

void GpuProgramRunner::serviceRequests()
{
  std::vector<uint32_t> bits(zeroRequests.size(), 0u);

  graph->bufferRead(requestStaging, 0u, pagePool->requestBytes(),
                    [&bits](const void *data) { std::memcpy(bits.data(), data, bits.size() * sizeof(uint32_t)); });

  bool any = false;

  for (size_t word = 0u; word < bits.size(); ++word)
  {
    uint32_t value = bits[word];

    while (value != 0u)
    {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(value));

      value &= value - 1u;

      streamingManager->requestByMetaIndex(static_cast<uint32_t>(word) * 32u + bit);

      any = true;
    }
  }

  if (!any)
  {
    return;
  }

  runnerStats.pagesAdmitted += streamingManager->service(0u);
  runnerStats.uploadWrites = pagePool->uploadWriteCount();
  ++runnerStats.metadataFlushes;

  if (streamingManager->lastCallStarved())
  {
    runnerStats.starved = true;
  }

  graph->bufferWrite(pagePool->requestBuffer(), 0u, pagePool->requestBytes(), zeroRequests.data());
}

TickResult GpuProgramRunner::tick()
{
  TickResult result;

  ++runnerStats.ticks;

  while (cursor < program.instructions.size())
  {
    const compiler::Instruction &skipped = program.instructions[cursor];

    if (skipped.op == compiler::Op::Select || skipped.op == compiler::Op::Where)
    {
      break;
    }

    if (skipped.op == compiler::Op::FreeTable && (!pinFinalTables || pinnedRegisters.count(skipped.from) == 0u))
    {
      const auto entry = registers.find(skipped.from);

      if (entry != registers.end() && !entry->second.overflowed)
      {
        freeRegions.push_back(entry->second.wordBase);
      }
    }

    ++cursor;
  }

  if (cursor >= program.instructions.size())
  {
    result.programComplete = true;

    return result;
  }

  result.instruction = cursor;

  if (!instructionStarted)
  {
    beginInstruction(cursor);

    // A register whose region does not fit the table buffer produced no rows
    // because there was nowhere to put them. Say so: this path returns
    // without a frame, and a silent zero is indistinguishable from a quiet
    // week.
    if (registers[program.instructions[cursor].reg].overflowed)
    {
      result.overflowed = true;
      runnerStats.overflowed = true;
    }

    // An instruction with nothing to iterate - a Where over an empty
    // register, a Select over a type with no pages - completes here, without
    // a frame. At twenty thousand people 1509 of a week's 1536 match frames
    // produced no rows and cost 3.5 ms of submit each; most patterns die on
    // their first or second edge, and the row counts are already on the CPU.
    if (plannedRowsThisInstruction == 0u)
    {
      registers[program.instructions[cursor].reg].rows = 0u;
      instructionStarted = false;
      result.instructionComplete = true;
      result.rowsProduced = 0u;

      ++runnerStats.instructionsCompleted;

      notifyCompletedRules(cursor);

      ++cursor;

      result.programComplete = complete();

      return result;
    }
  }

  matchPass->setParams(params);
  matchPass->setActive(true);
  rewritePass->setActive(false);

  ::rendering::RenderGraph::Frame frame;
  ::rendering::RenderGraph::Overrides overrides;

  graph->setCurrentFrameIndex(runnerStats.ticks % graph->getMaxFramesInFlight());

  const auto submitStart = std::chrono::steady_clock::now();

  graph->run(frame, overrides);
  graph->waitFrame(frame);

  const auto submitEnd = std::chrono::steady_clock::now();

  runnerStats.msSubmitWait += std::chrono::duration<double, std::milli>(submitEnd - submitStart).count();

  recordFrame(0u, cursor, ~0u, producedRows, std::chrono::duration<double, std::milli>(submitEnd - submitStart).count());

  graphrewrite::rendering::MatchStatus status;

  graph->bufferRead(statusStaging, 0u, sizeof(status), [&status](const void *data) { std::memcpy(&status, data, sizeof(status)); });

  runnerStats.msStatusRead += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitEnd).count();

  producedRows += status.writtenRows;

  runnerStats.peakRowsNeeded = std::max(runnerStats.peakRowsNeeded, producedRows);

  result.rowsProduced = status.writtenRows;
  result.stalls = status.stalledRows;

  runnerStats.stalls += status.stalledRows;

  params.firstFrame = 0u;

  if (status.overflowed != 0u)
  {
    result.overflowed = true;
    runnerStats.overflowed = true;

    registers[program.instructions[cursor].reg].overflowed = true;
  }

  if (status.incompleteRows == 0u || result.overflowed)
  {
    // `writtenRows` is what the scan COUNTED, which is what the writes would have
    // needed. When the table overflowed, the writes stopped at its capacity, so
    // the real row count is the capacity and not the count.
    registers[program.instructions[cursor].reg].rows =
        result.overflowed ? std::min(producedRows, settings.rowCapacityPerTable) : producedRows;

    instructionStarted = false;
    result.instructionComplete = true;

    ++runnerStats.instructionsCompleted;

    notifyCompletedRules(cursor);

    ++cursor;

    result.programComplete = complete();

    return result;
  }

  const auto serviceStart = std::chrono::steady_clock::now();

  serviceRequests();

  runnerStats.msService += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - serviceStart).count();

  result.starved = runnerStats.starved;

  return result;
}

void GpuProgramRunner::notifyCompletedRules(const uint32_t index)
{
  if (!matchCallback)
  {
    return;
  }

  for (const uint32_t rule : program.instructions[index].rules)
  {
    bool isLast = true;

    for (uint32_t later = index + 1u; later < static_cast<uint32_t>(program.instructions.size()); ++later)
    {
      const compiler::Instruction &instruction = program.instructions[later];

      if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
      {
        continue;
      }

      if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) != instruction.rules.end())
      {
        isLast = false;

        break;
      }
    }

    if (isLast)
    {
      matchCallback(rule, resultForRule(rule));
    }
  }
}

RuleResult GpuProgramRunner::resultForRule(const uint32_t rule) const
{
  RuleResult result;

  for (const compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) == instruction.rules.end())
    {
      continue;
    }

    const auto entry = registers.find(instruction.reg);

    if (entry != registers.end())
    {
      result.rows = entry->second.rows;
      result.columns = entry->second.columns;
      result.overflowed = entry->second.overflowed;
    }
  }

  return result;
}

PlanCheck GpuProgramRunner::checkPlan() const
{
  return checkProgramPlan(program, store);
}

const RunnerStats &GpuProgramRunner::stats() const
{
  return runnerStats;
}

std::vector<Fact> GpuProgramRunner::snapshot() const
{
  std::vector<Fact> out;

  for (uint32_t index = 0u; index < store.partitionCount(); ++index)
  {
    const Partition &partition = store.partitionAt(index);

    if (partition.order() != KeyOrder::ByOrigin)
    {
      continue;
    }

    for (const Edge &edge : partition.snapshot())
    {
      out.emplace_back(edge.origin, partition.edgeType(), edge.target);
    }
  }

  std::sort(out.begin(), out.end());

  return out;
}

const RegisterTable *GpuProgramRunner::tableForRegister(const uint32_t reg) const
{
  const auto entry = registers.find(reg);

  return entry == registers.end() ? nullptr : &entry->second;
}

std::vector<uint32_t> GpuProgramRunner::readRegister(const uint32_t reg) const
{
  std::vector<uint32_t> out;

  const auto entry = registers.find(reg);

  if (entry == registers.end())
  {
    return out;
  }

  std::vector<uint32_t> all(matchPass->tableBytes() / sizeof(uint32_t), 0u);

  const_cast<::rendering::RenderGraph &>(*graph).bufferRead(
      tableStaging, 0u, matchPass->tableBytes(),
      [&all](const void *data) { std::memcpy(all.data(), data, all.size() * sizeof(uint32_t)); });

  const RegisterTable &table = entry->second;

  for (uint32_t column = 0u; column < table.columns; ++column)
  {
    for (uint32_t row = 0u; row < table.rows; ++row)
    {
      out.push_back(all[table.wordBase + column * settings.rowCapacityPerTable + row]);
    }
  }

  return out;
}

GpuRewriteStats GpuProgramRunner::applyRewrites()
{
  GpuRewriteStats total;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const GpuRewriteStats stats = applyRule(rule, RuleApplyOptions{});

    total.rulesApplied += stats.rulesApplied;
    total.edgesRemoved += stats.edgesRemoved;
    total.edgesInserted += stats.edgesInserted;
    total.pagesCompacted += stats.pagesCompacted;
    total.verticesCreated += stats.verticesCreated;
    total.rowsFired += stats.rowsFired;
    total.claimsRejected += stats.claimsRejected;
    total.declined += stats.declined;
    total.stale += stats.stale;
    total.overflowed = total.overflowed || stats.overflowed;
  }

  return total;
}

void GpuProgramRunner::runRewriteFrame(const uint32_t kind, const uint32_t rule, const uint32_t rows)
{
  ::rendering::RenderGraph::Frame frame;
  ::rendering::RenderGraph::Overrides overrides;

  const auto started = std::chrono::steady_clock::now();

  graph->setCurrentFrameIndex(runnerStats.ticks % graph->getMaxFramesInFlight());
  graph->run(frame, overrides);
  graph->waitFrame(frame);

  const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  runnerStats.msSubmitWait += wallMs;

  ++runnerStats.ticks;

  recordFrame(kind, cursor, rule, rows, wallMs);
}

void GpuProgramRunner::recordFrame(const uint32_t kind, const uint32_t instruction, const uint32_t rule, const uint32_t rows,
                                   const double wallMs)
{
  if (!settings.recordFrames)
  {
    return;
  }

  FrameTiming timing;

  timing.frame = runnerStats.ticks;
  timing.kind = kind;
  timing.instruction = instruction;
  timing.rule = rule;
  timing.rows = rows;
  timing.wallMs = wallMs;

  if (settings.enableTimers)
  {
    for (const ::rendering::RenderGraph::PassTimingSummary &summary : graph->getPassTimingSummaries())
    {
      timing.gpuMs += summary.gpuTimeMs;

      if (summary.name == "graphrewrite.match")
      {
        timing.matchGpuMs += summary.gpuTimeMs;
      }
      else if (summary.name == "graphrewrite.rewrite")
      {
        timing.rewriteGpuMs += summary.gpuTimeMs;
      }
    }
  }

  frameTimings.push_back(timing);
}

void GpuProgramRunner::readCounters(graphrewrite::rendering::RewriteCounters &counters)
{
  graph->bufferRead(countersStaging, 0u, sizeof(counters),
                    [&counters](const void *data) { std::memcpy(&counters, data, sizeof(counters)); });
}

void GpuProgramRunner::snapshotResidency()
{
  residencyBefore = pagePool->snapshotResidency(store);
}

void GpuProgramRunner::rebuildResidency()
{
  // Rebuild rather than reconfigure: the buffers already exist, and a split
  // has renumbered directory positions. Pages that did not change keep their
  // slots; changed ones are re-uploaded in place.
  pagePool->rebuildMetadataKeeping(store, residencyBefore);
  residencyBefore.clear();

  // Residency was dropped, so the streaming manager has to be told too.
  streamingManager = std::make_unique<StreamingManager>(*pagePool, store, usage);

  std::vector<EdgeTypeId> partitionTypes;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    partitionTypes.push_back(store.partitionAt(partition).edgeType());
  }

  streamingManager->configure(partitionTypes);
}

void GpuProgramRunner::resyncStore()
{
  rebuildResidency();
}

GpuRewriteStats GpuProgramRunner::applyRule(const uint32_t rule, const RuleApplyOptions &options, std::vector<FiredRecord> *firedOut)
{
  GpuRewriteStats stats;

  if (rule >= program.ruleRewrites.size())
  {
    return stats;
  }

  const RuleResult result = resultForRule(rule);

  if (result.rows == 0u)
  {
    return stats;
  }

  const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

  if (rewrite.removals.empty() && rewrite.insertions.empty())
  {
    return stats;
  }

  // Find the register holding this rule's table.
  const RegisterTable *table = nullptr;

  for (const compiler::Instruction &instruction : program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) == instruction.rules.end())
    {
      continue;
    }

    const auto entry = registers.find(instruction.reg);

    if (entry != registers.end())
    {
      table = &entry->second;
    }
  }

  if (table == nullptr)
  {
    return stats;
  }

  // One round per rule application, folded into the dice seed - see Dice.hpp.
  // The rule is folded in too, so two rules matching the same row do not roll
  // the same number.
  ++rewriteRound;

  // The claim generation: wraps at 256, and a wrap clears the table.
  claimGen = (claimGen % 255u) + 1u;

  if (claimGen == 1u)
  {
    const std::vector<uint32_t> zeros(settings.claimEntries, 0u);

    graph->bufferWrite(rewritePass->claimsBuffer(), 0u, zeros.size() * sizeof(uint32_t), const_cast<uint32_t *>(zeros.data()));
  }

  std::vector<graphrewrite::rendering::RuleEdgeDesc> descriptors;

  // Rule vertices are not table columns. The table is the merged graph's, and
  // which column holds this rule's vertex 0 depends on where the rule was
  // merged in - the compiler records it per instruction, and until this was
  // read the shader assumed column i was vertex i, which held only for the
  // first rule of every thread.
  const std::vector<uint32_t> columns = ruleVertexColumns(program, rule);
  const uint32_t matched = rewrite.matchedVertexCount;

  const auto describe = [this, &descriptors, &columns, matched](const std::vector<compiler::PatternEdge> &edges,
                                                                 const std::vector<bool> &deferred) {
    const auto place = [&columns, matched](const uint32_t vertex, uint32_t &column, uint32_t &fresh) {
      if (vertex >= matched)
      {
        column = 0u;
        fresh = vertex - matched + 1u;

        return;
      }

      column = vertex < columns.size() && columns[vertex] != compiler::noRuleColumn ? columns[vertex] : vertex;
      fresh = 0u;
    };

    for (size_t i = 0u; i < edges.size(); ++i)
    {
      const compiler::PatternEdge &edge = edges[i];
      graphrewrite::rendering::RuleEdgeDesc desc;
      uint32_t originFresh = 0u;
      uint32_t targetFresh = 0u;

      place(edge.origin, desc.originColumn, originFresh);
      place(edge.target, desc.targetColumn, targetFresh);

      desc.fresh = graphrewrite::rendering::freshCode(originFresh, targetFresh);
      desc.partition = store.has(edge.label, KeyOrder::ByOrigin)
                           ? partitionIndexOf(store.partition(edge.label, KeyOrder::ByOrigin))
                           : 0u;

      if (i < deferred.size() && deferred[i])
      {
        desc.partition |= graphrewrite::rendering::deferredEdgeBit;
      }

      descriptors.push_back(desc);
    }
  };

  describe(rewrite.removals, {});
  describe(rewrite.insertions, options.deferredInsertions);

  rewritePass->uploadRuleEdges(descriptors);
  rewritePass->clearMarks();

  // Where each of the rule's matched vertices sits in the table, for the
  // records the kernels write in rule order.
  {
    std::vector<uint32_t> vertexColumns(matched, 0u);

    for (uint32_t vertex = 0u; vertex < matched; ++vertex)
    {
      vertexColumns[vertex] = vertex < columns.size() && columns[vertex] != compiler::noRuleColumn ? columns[vertex] : vertex;
    }

    rewritePass->uploadVertexColumns(vertexColumns);
  }

  graphrewrite::rendering::RewriteParams params;

  params.rowCount = table->rows;
  params.tableWordBase = table->wordBase;
  params.tableStride = settings.rowCapacityPerTable;
  params.removalCount = static_cast<uint32_t>(rewrite.removals.size());
  params.removalBase = 0u;
  params.insertionCount = static_cast<uint32_t>(rewrite.insertions.size());
  params.insertionBase = static_cast<uint32_t>(rewrite.removals.size());
  params.matchedVertices = rewrite.matchedVertexCount;
  params.totalVertices = rewrite.totalVertexCount;
  params.deltaCapacity = settings.deltaCapacity;
  params.dirtyCapacity = settings.dirtyCapacity;
  params.vertexBase = store.vertexCount();
  params.dirtyBase = rewritePass->dirtyBase();
  params.rowBase = rewritePass->rowBase();
  params.seed = dice::seedFor(dice::roundSeed(settings.diceSeed, rewriteRound), rule);
  params.chance = options.chance == ~0u ? rewrite.policy.permille() : options.chance;
  params.claimGen = claimGen;
  params.ruleId = rule;
  params.phase = options.decide ? 1u : 0u;

  // Every page of every partition this rule touches has to be RESIDENT when
  // its kernels run: filter_rows and mark_removals skip pages that are not,
  // on the reasoning that a page the match read must still be in the pool.
  // That held for the first rule of a round. The previous rule's compaction
  // rebuilt the metadata and dropped residency wholesale, so the second rule
  // found no page resident, judged every one of its rows stale, and removed
  // nothing - silently, and only in programmes with more than one rule.
  {
    std::set<uint32_t> partitions;

    for (const graphrewrite::rendering::RuleEdgeDesc &desc : descriptors)
    {
      partitions.insert(desc.partition & ~graphrewrite::rendering::deferredEdgeBit);
    }

    for (const uint32_t partition : partitions)
    {
      const uint32_t pages = store.partitionAt(partition).pageCount();

      for (uint32_t position = 0u; position < pages; ++position)
      {
        streamingManager->request(partition, position);
      }
    }

    runnerStats.pagesAdmitted += streamingManager->service(0u);
    runnerStats.uploadWrites = pagePool->uploadWriteCount();
    ++runnerStats.metadataFlushes;

    if (streamingManager->lastCallStarved())
    {
      // The pool cannot hold this rule's pages: its rewrite would be partial.
      runnerStats.starved = true;
      stats.overflowed = true;
    }
  }

  // The match stages share this graph, so they are told to do nothing, and the
  // large readbacks are switched on for exactly these runs.
  matchPass->setIdle(settings.maxRegisters - 1u);
  matchPass->setActive(false);
  rewritePass->setActive(true);

  graphrewrite::rendering::RewriteCounters counters;

  if (options.decide)
  {
    // Phase 1: the GPU judges dice and staleness and hands the live rows over.
    setRewriteReadbackEnabled(true, false);
    rewritePass->setParams(params);
    runRewriteFrame(1u, rule, table->rows);
    readCounters(counters);

    stats.declined += counters.declined;
    stats.stale += counters.stale;
    stats.overflowed = stats.overflowed || counters.overflow != 0u;
    stats.overflowKinds |= counters.overflow;

    const uint32_t count = std::min(counters.candidates, settings.candidateCapacity);
    std::vector<uint32_t> raw(1u + static_cast<size_t>(count) * graphrewrite::rendering::recordWords, 0u);

    if (count != 0u)
    {
      graph->bufferRead(candidatesStaging, 0u, raw.size() * sizeof(uint32_t),
                        [&raw](const void *data) { std::memcpy(raw.data(), data, raw.size() * sizeof(uint32_t)); });
    }

    std::vector<CandidateRow> candidates(count);

    for (uint32_t i = 0u; i < count; ++i)
    {
      const uint32_t *record = raw.data() + 1u + static_cast<size_t>(i) * graphrewrite::rendering::recordWords;

      candidates[i].row = record[0];

      const uint32_t vertices = std::min(record[1], graphrewrite::rendering::recordVertices);

      candidates[i].vertices.assign(record + 2u, record + 2u + vertices);
    }

    const uint32_t stride = 1u + settings.maxFreshVertices;
    std::vector<uint32_t> control(static_cast<size_t>(table->rows) * stride, 0u);

    options.decide(candidates, control);

    rewritePass->uploadRowControl(control);

    params.phase = 2u;
  }

  setRewriteReadbackEnabled(false, true);
  rewritePass->setParams(params);
  runRewriteFrame(2u, rule, table->rows);
  readCounters(counters);

  stats.edgesRemoved += counters.removed;
  // Counted where they land, below: a delta entry that duplicates an existing
  // edge is not an insertion.
  stats.pagesCompacted += counters.dirtyCount;
  stats.overflowed = stats.overflowed || counters.overflow != 0u;
  stats.overflowKinds |= counters.overflow;
  stats.rowsFired += counters.fired;
  stats.claimsRejected += counters.rejected;

  if (!options.decide)
  {
    stats.declined += counters.declined;
    stats.stale += counters.stale;
  }

  if (counters.vertexCount > store.vertexCount())
  {
    stats.verticesCreated += counters.vertexCount - store.vertexCount();

    store.reserveVertices(counters.vertexCount);
  }

  ++stats.rulesApplied;

  // The fired records: what the narrator wants, read before anything else so
  // the caller can hand them off while the fold below runs.
  if (firedOut != nullptr && counters.fired != 0u)
  {
    const uint32_t count = std::min(counters.fired, settings.firedCapacity);
    std::vector<uint32_t> raw(1u + static_cast<size_t>(count) * graphrewrite::rendering::recordWords, 0u);

    graph->bufferRead(firedStaging, 0u, raw.size() * sizeof(uint32_t),
                      [&raw](const void *data) { std::memcpy(raw.data(), data, raw.size() * sizeof(uint32_t)); });

    for (uint32_t i = 0u; i < count; ++i)
    {
      const uint32_t *record = raw.data() + 1u + static_cast<size_t>(i) * graphrewrite::rendering::recordWords;

      FiredRecord fired;

      fired.rule = record[0];
      fired.row = record[1];

      const uint32_t total = std::min(record[3], graphrewrite::rendering::recordVertices);

      fired.bound.assign(record + 4u, record + 4u + total);

      firedOut->push_back(std::move(fired));
    }
  }

  // Fold the compacted pages back into the CPU store. Reading back only the
  // pages on the dirty list is what keeps this proportional to what changed
  // rather than to what is resident.
  //
  // Every page the fold touches is remembered, and unless an insertion split
  // a page (which renumbers directory positions), each touched page is
  // re-uploaded IN PLACE and residency is kept. Rebuilding the metadata after
  // every rule dropped residency wholesale, and the next rule - and the next
  // week's match - streamed every page of every touched partition back in.
  std::set<std::pair<uint32_t, uint32_t>> touched;
  std::vector<uint32_t> pagesBefore(store.partitionCount(), 0u);

  snapshotResidency();

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    pagesBefore[partition] = store.partitionAt(partition).pageCount();
  }

  if (counters.dirtyCount != 0u)
  {
    std::vector<uint32_t> dirty(rewritePass->dirtyPagesBytes() / sizeof(uint32_t), 0u);

    graph->bufferRead(dirtyStaging, 0u, rewritePass->dirtyPagesBytes(),
                      [&dirty](const void *data) { std::memcpy(dirty.data(), data, dirty.size() * sizeof(uint32_t)); });

    // Only the slice the rewrite actually touched. `compact_pages` gathered
    // survivors by dirty index precisely so this is dirtyCount pages long -
    // at the default pool budget the whole-pool alternative is two gigabytes
    // copied to decide a few thousand edges.
    const uint32_t dirtyPages = std::min(counters.dirtyCount, settings.dirtyCapacity);
    const uint64_t gatheredBytes = static_cast<uint64_t>(dirtyPages) * pageEdges * sizeof(Edge);

    std::vector<Edge> pooled(static_cast<size_t>(dirtyPages) * pageEdges);

    graph->bufferRead(poolStaging, 0u, gatheredBytes,
                      [&pooled, gatheredBytes](const void *data) { std::memcpy(pooled.data(), data, gatheredBytes); });

    for (uint32_t i = 0u; i < dirtyPages; ++i)
    {
      const uint32_t page = dirty[i];
      const uint32_t survivors = dirty[settings.dirtyCapacity + i];

      if (page >= pagePool->totalPages())
      {
        continue;
      }

      const uint32_t partition = pagePool->metadata()[page].partition;
      const uint32_t position = page - pagePool->partitionFirst()[partition];
      if (pagePool->metadata()[page].slot == notResident)
      {
        continue;
      }

      // Indexed by dirty index, not by slot: that is what the gather bought.
      std::vector<Edge> contents(pooled.begin() + static_cast<long>(i) * pageEdges,
                                 pooled.begin() + static_cast<long>(i) * pageEdges + survivors);

      store.partitionAt(partition).replacePage(position, std::move(contents));
      touched.emplace(partition, position);
    }
  }

  // The delta holds what the kernel produced. Merging it into the sorted runs
  // is a CPU job: an insert can split a page, and a split rewrites the
  // directory.
  if (counters.deltaCount != 0u)
  {
    std::vector<graphrewrite::rendering::DeltaEdge> delta(rewritePass->deltaBytes() /
                                                          sizeof(graphrewrite::rendering::DeltaEdge));

    graph->bufferRead(deltaStaging, 0u, rewritePass->deltaBytes(),
                      [&delta](const void *data) { std::memcpy(delta.data(), data, delta.size() * sizeof(delta[0])); });

    for (uint32_t i = 0u; i < std::min(counters.deltaCount, settings.deltaCapacity); ++i)
    {
      const graphrewrite::rendering::DeltaEdge &entry = delta[i];

      if (entry.partition >= store.partitionCount())
      {
        continue;
      }

      // Through the store, not the one partition the descriptor named: a type
      // walked in both directions has two partitions and both must see the
      // edge, or every later backward match misses it. And never twice - the
      // graph is a set, and the CPU applier drops the same duplicates.
      const EdgeTypeId type = store.partitionAt(entry.partition).edgeType();
      const Edge inserted{entry.origin, entry.dest};

      if (store.containsEdge(type, inserted))
      {
        continue;
      }

      // What addEdge does, but remembering which page took the edge.
      for (const KeyOrder order : {KeyOrder::ByOrigin, KeyOrder::ByTarget})
      {
        if (!store.has(type, order))
        {
          continue;
        }

        Partition &target = store.partition(type, order);
        const uint32_t position = target.insert(inserted);

        touched.emplace(partitionIndexOf(target), position);
      }

      store.reserveVertices(std::max(inserted.origin, inserted.target) + 1u);

      ++stats.edgesInserted;
    }
  }

  if (!touched.empty())
  {
    bool split = false;

    for (uint32_t partition = 0u; partition < store.partitionCount() && !split; ++partition)
    {
      split = store.partitionAt(partition).pageCount() != pagesBefore[partition];
    }

    if (split)
    {
      rebuildResidency();
    }
    else
    {
      for (const auto &page : touched)
      {
        if (pagePool->isResident(page.first, page.second))
        {
          pagePool->upload(store, page.first, page.second, pagePool->slotOf(page.first, page.second));
        }
      }

      pagePool->flushMetadata();
    }
  }

  rewritePass->setParams(graphrewrite::rendering::RewriteParams{});

  setRewriteReadbackEnabled(false, false);

  return stats;
}

const std::vector<::rendering::RenderGraph::PassTimingSummary> &GpuProgramRunner::passTimings() const
{
  return graph->getPassTimingSummaries();
}

GpuPagePool &GpuProgramRunner::pool()
{
  return *pagePool;
}

StreamingManager &GpuProgramRunner::streaming()
{
  return *streamingManager;
}

::rendering::RenderGraph &GpuProgramRunner::renderGraph()
{
  return *graph;
}

} // namespace graphrewrite::runtime
