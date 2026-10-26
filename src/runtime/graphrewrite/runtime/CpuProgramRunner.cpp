#include "graphrewrite/runtime/CpuProgramRunner.hpp"

#include "graphrewrite/runtime/PlanCheck.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

bool IProgramRunner::runToCompletion(const uint32_t maxTicks)
{
  for (uint32_t i = 0u; i < maxTicks; ++i)
  {
    const TickResult result = tick();

    if (result.overflowed || result.starved)
    {
      return false;
    }

    if (result.programComplete)
    {
      return true;
    }
  }

  return false;
}

CpuProgramRunner::CpuProgramRunner(const compiler::CompiledProgram &compiled, const compiler::UsageValues &usageValues,
                                   EdgePageStore &pageStore, Settings runnerSettings)
    : program(compiled), usage(usageValues), store(pageStore), settings(runnerSettings)
{
  pool = std::make_unique<WorkerPool>(settings.workers);
}

void CpuProgramRunner::seed(const std::vector<Fact> &facts)
{
  for (const Fact &fact : facts)
  {
    store.addEdge(std::get<1>(fact), Edge{std::get<0>(fact), std::get<2>(fact)});
  }
}

void CpuProgramRunner::setMatchCallback(MatchCallback callback)
{
  matchCallback = std::move(callback);
}

void CpuProgramRunner::reset()
{
  registers.clear();
  boundMasks.clear();
  cursor = 0u;
  runnerStats = RunnerStats();
}

bool CpuProgramRunner::complete() const
{
  return cursor >= program.instructions.size();
}

uint32_t CpuProgramRunner::columnsForThread(const uint32_t thread) const
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

bool CpuProgramRunner::runInstruction(const uint32_t index)
{
  const compiler::Instruction &instruction = program.instructions[index];

  const bool isSelect = instruction.op == compiler::Op::Select;

  const uint32_t columns = columnsForThread(instruction.thread);

  MatchTable output;

  output.reset(columns, settings.rowCapacityPerTable);

  const KeyOrder order = instruction.direction == compiler::StepDirection::Backward ? KeyOrder::ByTarget : KeyOrder::ByOrigin;

  if (!store.has(instruction.label, order))
  {
    registers[instruction.reg] = std::move(output);
    boundMasks[instruction.reg] = 0u;

    return true;
  }

  const Partition &partition = store.partition(instruction.label, order);

  const bool cyclic = instruction.direction == compiler::StepDirection::Cyclic;

  uint32_t mask = 0u;

  if (isSelect)
  {
    // Seed from every edge of the partition.
    for (uint32_t position = 0u; position < partition.pageCount(); ++position)
    {
      for (const Edge &edge : partition.pageAt(position))
      {
        const bool loops = edge.origin == edge.target;

        if (loops != cyclic)
        {
          continue;
        }

        const uint32_t row = output.appendRow();

        if (row == notResident)
        {
          registers[instruction.reg] = std::move(output);

          return false;
        }

        output.set(instruction.fromColumn, row, edge.origin);

        if (!cyclic)
        {
          output.set(instruction.writeColumn, row, edge.target);
        }
      }
    }

    mask = (1u << instruction.fromColumn) | (cyclic ? 0u : (1u << instruction.writeColumn));
  }
  else
  {
    const auto entry = registers.find(instruction.from);

    if (entry == registers.end())
    {
      return false;
    }

    const MatchTable &input = entry->second;
    const uint32_t inputMask = boundMasks[instruction.from];

    // Each chunk matches its own contiguous range of rows into a private
    // buffer. Concatenating those buffers in CHUNK order is what makes the
    // result identical to the serial run regardless of how the threads
    // interleave - a matcher whose row order depends on scheduling cannot be
    // compared against a reference.
    const uint32_t chunks = pool->chunkCountFor(input.rows());

    std::vector<std::vector<uint32_t>> perChunk(chunks);

    pool->parallelFor(input.rows(), chunks, [&](const uint32_t chunk, const uint32_t begin, const uint32_t end) {
      std::vector<uint32_t> &local = perChunk[chunk];

      for (uint32_t row = begin; row < end; ++row)
      {
        const uint32_t bound = input.at(instruction.fromColumn, row);
        const PageRun run = partition.rangeFor(bound);

        if (run.empty())
        {
          continue;
        }

        for (uint32_t position = run.first; position <= run.last; ++position)
        {
          for (const Edge &edge : partition.pageAt(position))
          {
            if (keyOf(edge, order) != bound)
            {
              continue;
            }

            const uint32_t other = otherOf(edge, order);

            if (cyclic)
            {
              if (input.at(instruction.writeColumn, row) != other)
              {
                continue;
              }
            }
            else
            {
              bool duplicate = false;

              for (uint32_t column = 0u; column < input.columns(); ++column)
              {
                if ((inputMask & (1u << column)) != 0u && input.at(column, row) == other)
                {
                  duplicate = true;

                  break;
                }
              }

              if (duplicate)
              {
                continue;
              }
            }

            // A row is staged as its own cells plus the newly bound vertex; the
            // table layout is applied when the chunks are merged.
            for (uint32_t column = 0u; column < input.columns(); ++column)
            {
              local.push_back(input.at(column, row));
            }

            if (!cyclic)
            {
              local[local.size() - input.columns() + instruction.writeColumn] = other;
            }
          }
        }
      }
    });

    for (const std::vector<uint32_t> &local : perChunk)
    {
      for (size_t at = 0u; at + input.columns() <= local.size(); at += input.columns())
      {
        const uint32_t destination = output.appendRow();

        if (destination == notResident)
        {
          registers[instruction.reg] = std::move(output);

          return false;
        }

        for (uint32_t column = 0u; column < input.columns(); ++column)
        {
          output.set(column, destination, local[at + column]);
        }
      }
    }

    mask = inputMask | (1u << instruction.fromColumn) | (cyclic ? 0u : (1u << instruction.writeColumn));
  }

  registers[instruction.reg] = std::move(output);
  boundMasks[instruction.reg] = mask;

  return true;
}

TickResult CpuProgramRunner::tick()
{
  TickResult result;

  ++runnerStats.ticks;

  // Skip bookkeeping instructions.
  while (cursor < program.instructions.size())
  {
    const compiler::Op op = program.instructions[cursor].op;

    if (op == compiler::Op::Select || op == compiler::Op::Where)
    {
      break;
    }

    ++cursor;
  }

  if (cursor >= program.instructions.size())
  {
    result.programComplete = true;

    return result;
  }

  result.instruction = cursor;

  const bool ok = runInstruction(cursor);

  const auto entry = registers.find(program.instructions[cursor].reg);

  result.rowsProduced = entry == registers.end() ? 0u : entry->second.rows();
  result.instructionComplete = true;
  result.overflowed = !ok;

  runnerStats.peakRowsNeeded = std::max(runnerStats.peakRowsNeeded, result.rowsProduced);

  if (!ok)
  {
    runnerStats.overflowed = true;
  }

  ++runnerStats.instructionsCompleted;

  notifyCompletedRules(cursor);

  ++cursor;

  result.programComplete = complete();

  return result;
}

RuleResult CpuProgramRunner::resultForRule(const uint32_t rule) const
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
      result.rows = entry->second.rows();
      result.columns = entry->second.columns();
    }
  }

  return result;
}

// A rule is finished when the instruction just run is the LAST one serving it.
void CpuProgramRunner::notifyCompletedRules(const uint32_t index)
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

PlanCheck CpuProgramRunner::checkPlan() const
{
  return checkProgramPlan(program, store);
}

const RunnerStats &CpuProgramRunner::stats() const
{
  return runnerStats;
}

std::vector<Fact> CpuProgramRunner::snapshot() const
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

const MatchTable *CpuProgramRunner::finalTableForRule(const uint32_t rule) const
{
  const MatchTable *last = nullptr;

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
      last = &entry->second;
    }
  }

  return last;
}

const MatchTable *CpuProgramRunner::tableForRegister(const uint32_t reg) const
{
  const auto entry = registers.find(reg);

  return entry == registers.end() ? nullptr : &entry->second;
}

} // namespace graphrewrite::runtime
