// Phase 3.14: the capacity cases, each forced by an undersized configuration.
//
// Every one of these is a state the system has to RECOVER from, not merely
// survive. The original reports an overflow flag and acts on none of it.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <memory>
#include <random>
#include <set>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Program
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  DiagnosticSink sink;
  bool ok = false;
};

void compile(const std::string &source, Program &out)
{
  out.ok = compiler::compile(source, "t.graphwrite", compiler::CompileOptions(), out.sink, out.program, out.usage);
}

std::vector<Fact> facts(const uint32_t count, const uint32_t vertices, const uint32_t seed)
{
  std::mt19937 rng(seed);

  std::set<Fact> unique;

  while (unique.size() < count)
  {
    const uint32_t origin = rng() % vertices;
    const uint32_t target = rng() % vertices;

    if (origin != target)
    {
      unique.emplace(origin, EdgeTypeId(0u), target);
    }
  }

  return std::vector<Fact>(unique.begin(), unique.end());
}

const char *oneRule()
{
  return "let A likes B.\n"
         "rule r: when A likes B then A likes B.\n";
}

// --- an edge type larger than the whole pool -----------------------------
void typeLargerThanThePool(::rendering::RHI *rhi)
{
  Program compiled;

  compile(oneRule(), compiled);

  const std::vector<Fact> seeded = facts(3000u, 200u, 17u);

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = 1u; // one page of a many-page type
  settings.rowCapacityPerTable = 8192u;

  GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

  runner.seed(seeded);
  runner.createResources();

  check(store.partitionAt(0u).pageCount() > settings.slotCount, "the type really is larger than the pool");

  check(runner.runToCompletion(), "a type larger than the whole pool still completes");
  check(!runner.stats().starved, "without starving");

  checkEqual(runner.resultForRule(0u).rows, static_cast<uint32_t>(seeded.size()),
             "and finds every edge - the row advances a page per frame");

  std::cout << "    type larger than pool: " << runner.stats().ticks << " ticks for "
            << store.partitionAt(0u).pageCount() << " pages through 1 slot\n";
}

// --- a table smaller than its matches ------------------------------------
void tableOverflowIsReportedAndRecoverable(::rendering::RHI *rhi)
{
  Program compiled;

  compile(oneRule(), compiled);

  const std::vector<Fact> seeded = facts(600u, 60u, 23u);

  // Undersized on purpose: the table cannot hold every match.
  {
    EdgePageStore store;

    store.configure(compiled.program.edgeTypes);

    GpuRunnerSettings settings;

    settings.slotCount = 256u;
    settings.rowCapacityPerTable = 64u;

    GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

    runner.seed(seeded);
    runner.createResources();

    runner.runToCompletion();

    check(runner.stats().overflowed, "an undersized table reports overflow");
    check(runner.resultForRule(0u).overflowed, "and the rule's own result says so");
    check(runner.resultForRule(0u).rows <= settings.rowCapacityPerTable, "and it stopped at its capacity rather than running past it");
  }

  // Grow and replay: the same programme and the same facts, with room.
  {
    EdgePageStore store;

    store.configure(compiled.program.edgeTypes);

    GpuRunnerSettings settings;

    settings.slotCount = 256u;
    settings.rowCapacityPerTable = 4096u;

    GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

    runner.seed(seeded);
    runner.createResources();

    check(runner.runToCompletion(), "growing the table and replaying completes");
    check(!runner.stats().overflowed, "without overflow");
    checkEqual(runner.resultForRule(0u).rows, static_cast<uint32_t>(seeded.size()), "and recovers every match");
  }
}

void cpuRunnerReportsTheSameOverflow()
{
  Program compiled;

  compile(oneRule(), compiled);

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  CpuProgramRunner::Settings settings;

  settings.rowCapacityPerTable = 32u;

  CpuProgramRunner runner(compiled.program, compiled.usage, store, settings);

  runner.seed(facts(400u, 50u, 31u));

  check(!runner.runToCompletion(), "the CPU runner also refuses to claim success on overflow");
  check(runner.stats().overflowed, "and records it");
  check(runner.resultForRule(0u).rows <= settings.rowCapacityPerTable, "stopping at capacity");
}

// --- a rewrite that grows the graph past its pages -----------------------
void rewriteGrowingBeyondItsPages()
{
  Program compiled;

  compile("let A likes B.\n"
          "let A knows B.\n"
          "rule r: when A likes B then A likes B, A knows B.\n",
          compiled);

  check(compiled.ok, "the growing programme compiles");

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  compiler::EdgeTypeId likes = compiler::invalidEdgeType;

  compiled.program.edgeTypes.find("? likes ?", likes);

  // More insertions than one page can hold, so the partition must split.
  for (uint32_t i = 0u; i < pageEdges * 3u; ++i)
  {
    store.addEdge(likes, Edge{i % 90u, i});
  }

  RewriteApplier applier(compiled.program, store);

  std::vector<MatchedRow> rows;

  for (uint32_t i = 0u; i < 400u; ++i)
  {
    MatchedRow row;

    row.rule = 0u;
    row.vertices = {i % 90u, 100000u + i};

    rows.push_back(row);
  }

  const RewriteStats stats = applier.apply(rows);

  checkEqual(stats.rulesFired, 400u, "every row fires - insertions need no claim");
  checkEqual(stats.edgesInserted, 400u, "and every insertion lands");

  compiler::EdgeTypeId knows = compiler::invalidEdgeType;

  compiled.program.edgeTypes.find("? knows ?", knows);

  const Partition &inserted = store.partition(knows, KeyOrder::ByOrigin);

  check(inserted.pageCount() > 1u, "the inserted edges forced the partition to split");

  std::string problem;

  check(inserted.checkInvariants(problem), "and it is still well formed: " + problem);
  checkEqual(inserted.snapshot().size(), size_t(400), "with every inserted edge present");
}

void emptyGraphIsNotAnError()
{
  Program compiled;

  compile(oneRule(), compiled);

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  CpuProgramRunner runner(compiled.program, compiled.usage, store, CpuProgramRunner::Settings());

  check(runner.runToCompletion(), "an empty graph completes");
  checkEqual(runner.resultForRule(0u).rows, 0u, "with no matches, and no diagnostics");
}

void runAll(::rendering::RHI *rhi)
{
  typeLargerThanThePool(rhi);
  tableOverflowIsReportedAndRecoverable(rhi);
  cpuRunnerReportsTheSameOverflow();
  rewriteGrowingBeyondItsPages();
  emptyGraphIsNotAnError();
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

  runAll(rhi);

  delete rhi;

  return report("GraphRewriteCapacityTests");
}
