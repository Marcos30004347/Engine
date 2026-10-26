// The whole loop on the GPU: match, rewrite, read back, repeat - checked against
// the CPU runner plus RewriteApplier doing the same thing in memory.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"
#include "graphrewrite/runtime/RuleRows.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <memory>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Compiled
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  DiagnosticSink sink;
  bool ok = false;
};

void compile(const std::string &source, Compiled &out)
{
  out.ok = compiler::compile(source, "t.graphwrite", compiler::CompileOptions(), out.sink, out.program, out.usage);
}

EdgeTypeId typeOf(const Compiled &compiled, const char *pattern)
{
  compiler::EdgeTypeId id = compiler::invalidEdgeType;

  compiled.program.edgeTypes.find(pattern, id);

  return id;
}

std::vector<Fact> snapshot(const EdgePageStore &store)
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

// A rule that consumes what it matches: `B knows A` is dropped by THEN.
const char *consumingRule()
{
  return "let A likes B.\n"
         "let A knows B.\n"
         "rule r: when A likes B, B knows A then A likes B.\n";
}

void gpuRewriteRemovesWhatTheRuleConsumes(::rendering::RHI *rhi)
{
  Compiled compiled;

  compile(consumingRule(), compiled);

  check(compiled.ok, "programme compiles");

  const EdgeTypeId likes = typeOf(compiled, "? likes ?");
  const EdgeTypeId knows = typeOf(compiled, "? knows ?");

  std::vector<Fact> seed;

  for (uint32_t i = 0u; i < 6u; ++i)
  {
    seed.emplace_back(i, likes, i + 100u);
    seed.emplace_back(i + 100u, knows, i);
  }

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = 256u;
  settings.rowCapacityPerTable = 2048u;
  settings.enableTableReadback = true;

  GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

  runner.seed(seed);
  runner.createResources();

  check(runner.runToCompletion(), "the GPU match completes");

  const RuleResult matched = runner.resultForRule(0u);

  checkEqual(matched.rows, 6u, "six pairs match");

  const GpuRewriteStats stats = runner.applyRewrites();

  checkEqual(stats.rulesApplied, 1u, "one rule applied");
  checkEqual(stats.edgesRemoved, 6u, "and it removed the six edges its THEN drops");
  checkEqual(stats.edgesInserted, 0u, "inserting nothing");
  check(stats.pagesCompacted >= 1u, "compacting the pages that changed");
  check(!stats.overflowed, "without overflowing");

  const std::vector<Fact> after = snapshot(store);

  uint32_t remainingKnows = 0u;
  uint32_t remainingLikes = 0u;

  for (const Fact &fact : after)
  {
    remainingKnows += std::get<1>(fact) == knows ? 1u : 0u;
    remainingLikes += std::get<1>(fact) == likes ? 1u : 0u;
  }

  checkEqual(remainingKnows, 0u, "every consumed edge is gone from the store");
  checkEqual(remainingLikes, 6u, "and every preserved edge survives");

  // And the graph is now quiescent: nothing matches any more.
  EdgePageStore recheck;

  recheck.configure(compiled.program.edgeTypes);

  CpuProgramRunner verifier(compiled.program, compiled.usage, recheck, CpuProgramRunner::Settings());

  verifier.seed(after);
  verifier.runToCompletion();

  checkEqual(verifier.resultForRule(0u).rows, 0u, "and the rewritten graph matches nothing further");
}

void gpuAndCpuRewriteAgree(::rendering::RHI *rhi)
{
  Compiled compiled;

  compile(consumingRule(), compiled);

  const EdgeTypeId likes = typeOf(compiled, "? likes ?");
  const EdgeTypeId knows = typeOf(compiled, "? knows ?");

  std::vector<Fact> seed;

  for (uint32_t i = 0u; i < 40u; ++i)
  {
    seed.emplace_back(i, likes, i + 500u);
    seed.emplace_back(i + 500u, knows, i);
  }

  // --- CPU: match, then apply every matched row -------------------------
  EdgePageStore cpuStore;

  cpuStore.configure(compiled.program.edgeTypes);

  CpuProgramRunner cpuRunner(compiled.program, compiled.usage, cpuStore, CpuProgramRunner::Settings());

  cpuRunner.seed(seed);
  cpuRunner.runToCompletion();

  RewriteApplier applier(compiled.program, cpuStore);

  std::vector<MatchedRow> rows;

  const MatchTable *table = nullptr;

  for (const compiler::Instruction &instruction : compiled.program.instructions)
  {
    if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
    {
      continue;
    }

    const MatchTable *candidate = cpuRunner.tableForRegister(instruction.reg);

    if (candidate != nullptr && candidate->rows() != 0u)
    {
      table = candidate;
    }
  }

  check(table != nullptr, "the CPU produced a table");

  if (table != nullptr)
  {
    for (uint32_t row = 0u; row < table->rows(); ++row)
    {
      MatchedRow matched;

      matched.rule = 0u;

      for (uint32_t column = 0u; column < table->columns(); ++column)
      {
        matched.vertices.push_back(table->at(column, row));
      }

      rows.push_back(std::move(matched));
    }
  }

  const RewriteStats cpuStats = applier.apply(rows);

  // --- GPU: the same thing ----------------------------------------------
  EdgePageStore gpuStore;

  gpuStore.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = 256u;
  settings.rowCapacityPerTable = 2048u;
  settings.enableTableReadback = true;

  GpuProgramRunner gpuRunner(rhi, compiled.program, compiled.usage, gpuStore, settings);

  gpuRunner.seed(seed);
  gpuRunner.createResources();
  gpuRunner.runToCompletion();

  const GpuRewriteStats gpuStats = gpuRunner.applyRewrites();

  checkEqual(gpuStats.edgesRemoved, cpuStats.edgesRemoved, "GPU and CPU remove the same number of edges");

  const std::vector<Fact> cpuAfter = snapshot(cpuStore);
  const std::vector<Fact> gpuAfter = snapshot(gpuStore);

  checkEqual(gpuAfter.size(), cpuAfter.size(), "and leave graphs of the same size");
  check(gpuAfter == cpuAfter, "and in fact exactly the same graph");

  std::cout << "    rewrite agreement: " << cpuStats.edgesRemoved << " edges removed, " << cpuAfter.size() << " remaining\n";
}

void gpuRewriteInserts(::rendering::RHI *rhi)
{
  Compiled compiled;

  // Consumes `knows` and produces `hates`.
  compile("let A likes B.\n"
          "let A knows B.\n"
          "let A hates B.\n"
          "rule r: when A likes B, B knows A then A likes B, A hates B.\n",
          compiled);

  check(compiled.ok, "the producing programme compiles");

  const EdgeTypeId likes = typeOf(compiled, "? likes ?");
  const EdgeTypeId knows = typeOf(compiled, "? knows ?");
  const EdgeTypeId hates = typeOf(compiled, "? hates ?");

  std::vector<Fact> seed;

  for (uint32_t i = 0u; i < 5u; ++i)
  {
    seed.emplace_back(i, likes, i + 200u);
    seed.emplace_back(i + 200u, knows, i);
  }

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = 256u;

  GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

  runner.seed(seed);
  runner.createResources();
  runner.runToCompletion();

  const GpuRewriteStats stats = runner.applyRewrites();

  checkEqual(stats.edgesRemoved, 5u, "five consumed edges removed");
  checkEqual(stats.edgesInserted, 5u, "five produced edges emitted to the delta");

  const std::vector<Fact> after = snapshot(store);

  uint32_t hatesCount = 0u;
  uint32_t knowsCount = 0u;

  for (const Fact &fact : after)
  {
    hatesCount += std::get<1>(fact) == hates ? 1u : 0u;
    knowsCount += std::get<1>(fact) == knows ? 1u : 0u;
  }

  checkEqual(hatesCount, 5u, "and the produced edges are in the store, merged out of the delta");
  checkEqual(knowsCount, 0u, "with the consumed ones gone");

  // The inserted edges must be findable, not merely present - the directory has
  // to have been updated by the merge.
  const Partition &partition = store.partition(hates, KeyOrder::ByOrigin);

  check(!partition.rangeFor(0u).empty(), "an inserted edge is reachable through the directory");

  std::string problem;

  check(partition.checkInvariants(problem), "and the partition is well formed: " + problem);
}


// A rule whose own vertex 0 is NOT the merged table's column 0. Rule `two` is
// written target-first, so its vertex 0 is B, while the shared seed puts A in
// column 0. Read naively, its rewrite removes an edge that does not exist and
// inserts one backwards; read through the column mapping, both devices agree.
void nonIdentityColumnsAgree(::rendering::RHI *rhi)
{
  Compiled compiled;

  compile("let A likes B.\n"
          "let A knows B.\n"
          "let A fears B.\n"
          "let A hates B.\n"
          "let A trusts B.\n"
          "rule one: when A likes B, B knows A then A likes B, A hates B.\n"
          "rule two: when B fears A, A likes B then A likes B, B trusts A.\n",
          compiled);

  check(compiled.ok, "the two-rule programme compiles");
  check(!ruleColumnsAreIdentity(compiled.program), "and its rule columns are not identity - the case under test");

  const EdgeTypeId likes = typeOf(compiled, "? likes ?");
  const EdgeTypeId knows = typeOf(compiled, "? knows ?");
  const EdgeTypeId fears = typeOf(compiled, "? fears ?");

  std::vector<Fact> seed;

  for (uint32_t i = 0u; i < 6u; ++i)
  {
    seed.emplace_back(i, likes, i + 300u);
    seed.emplace_back(i + 300u, knows, i);
    seed.emplace_back(i + 300u, fears, i);
  }

  EdgePageStore cpuStore;

  cpuStore.configure(compiled.program.edgeTypes);

  CpuProgramRunner cpuRunner(compiled.program, compiled.usage, cpuStore, CpuProgramRunner::Settings());

  cpuRunner.seed(seed);
  cpuRunner.runToCompletion();

  std::vector<MatchedRow> rows;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(compiled.program.ruleNames.size()); ++rule)
  {
    std::vector<MatchedRow> matched = gatherRuleRows(compiled.program, cpuRunner, rule);

    const MatchTable *table = cpuRunner.finalTableForRule(rule);
    const std::vector<uint32_t> columns = ruleVertexColumns(compiled.program, rule);

    std::cout << "    rule " << compiled.program.ruleNames[rule] << ": result rows " << cpuRunner.resultForRule(rule).rows
              << ", final table " << (table == nullptr ? 0u : table->rows()) << "x" << (table == nullptr ? 0u : table->columns())
              << ", gathered " << matched.size() << ", columns";

    for (const uint32_t column : columns)
    {
      std::cout << ' ' << (column == compiler::noRuleColumn ? std::string("-") : std::to_string(column));
    }

    std::cout << "\n";

    rows.insert(rows.end(), matched.begin(), matched.end());
  }

  checkEqual(rows.size(), size_t(12), "both rules matched every pair");

  RewriteApplier applier(compiled.program, cpuStore);

  applier.setRound(1u); // the GPU's first round is 1

  const RewriteStats cpuStats = applier.apply(rows);

  checkEqual(cpuStats.edgesRemoved, 12u, "the CPU removed both consumed edges of every pair");

  EdgePageStore gpuStore;

  gpuStore.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = 256u;

  GpuProgramRunner gpuRunner(rhi, compiled.program, compiled.usage, gpuStore, settings);

  gpuRunner.seed(seed);
  gpuRunner.createResources();
  gpuRunner.runToCompletion();

  const GpuRewriteStats gpuStats = gpuRunner.applyRewrites();

  checkEqual(gpuStats.edgesRemoved, cpuStats.edgesRemoved, "the GPU removed the same number");
  check(snapshot(gpuStore) == snapshot(cpuStore), "and the two graphs are identical afterwards");
}

} // namespace

int main()
{
  ::rendering::DeviceRequiredLimits limits = {
      .minimumMemory = 0,
      .minimumComputeSharedMemory = 0,
      .minimumComputeWorkGroupInvocations = 0,
  };

  ::rendering::backend::vulkan::VulkanRHI *rhi = new ::rendering::backend::vulkan::VulkanRHI(
      ::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  rhi->init(surfaces);

  // Order matters, and not for a good reason: the FOURTH RenderGraph created in
  // one process crashes inside RenderGraph's constructor with a guard-page read
  // (libgmalloc), whichever test creates it. Until that is fixed in the render
  // graph, the newest test goes first so the crash does not hide its result.
  nonIdentityColumnsAgree(rhi);
  gpuRewriteRemovesWhatTheRuleConsumes(rhi);
  gpuAndCpuRewriteAgree(rhi);
  gpuRewriteInserts(rhi);

  delete rhi;

  return report("GraphRewriteEndToEndTests");
}
