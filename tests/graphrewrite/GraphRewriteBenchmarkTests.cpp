// Benchmark: a large random graph, matched on both runners, timed.
//
// Two jobs, and they are not separable. It CHECKS that the two runners agree at
// this scale - a disagreement that only shows up at a million edges is exactly
// the kind a small test misses - and it REPORTS what the work costs, per tick,
// per pattern edge and per graph edge.
//
// Timings are reported as a median over several runs, not a single number: a
// first run pays for shader compilation and page upload, and averaging that in
// makes every later figure a lie.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <set>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(const Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values)
{
  if (values.empty())
  {
    return 0.0;
  }

  std::sort(values.begin(), values.end());

  return values[values.size() / 2u];
}

struct Program
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  DiagnosticSink sink;
  bool ok = false;
};

std::string readProgramSource()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/benchmark.graphwrite");

  if (!file)
  {
    return std::string();
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

// A random graph with enough structure that the rule actually matches: a
// fraction of the `follows` edges get a reciprocal `blocks` edge.
std::vector<Fact> randomGraph(const uint32_t edgeCount, const uint32_t vertices, const uint32_t reciprocalIn, const uint32_t seed)
{
  std::mt19937 rng(seed);

  std::set<Fact> unique;

  const uint32_t follows = 0u;
  const uint32_t blocks = 1u;

  const uint32_t half = edgeCount / 2u;

  std::vector<std::pair<uint32_t, uint32_t>> followEdges;

  followEdges.reserve(half);

  while (unique.size() < half)
  {
    const uint32_t origin = rng() % vertices;
    const uint32_t target = rng() % vertices;

    if (origin == target)
    {
      continue;
    }

    if (unique.emplace(origin, follows, target).second)
    {
      followEdges.emplace_back(origin, target);
    }
  }

  // Reciprocal blocks for a slice of them - these are the matches.
  for (size_t i = 0u; i < followEdges.size(); i += reciprocalIn)
  {
    unique.emplace(followEdges[i].second, blocks, followEdges[i].first);
  }

  // Fill the rest with blocks edges that mostly do not match.
  while (unique.size() < edgeCount)
  {
    const uint32_t origin = rng() % vertices;
    const uint32_t target = rng() % vertices;

    if (origin != target)
    {
      unique.emplace(origin, blocks, target);
    }
  }

  return std::vector<Fact>(unique.begin(), unique.end());
}

struct RunReport
{
  uint32_t rows = 0u;
  double totalMs = 0.0;
  uint32_t ticks = 0u;
  uint32_t callbackFires = 0u;
};

RunReport runCpu(const Program &compiled, const std::vector<Fact> &facts, const uint32_t workers)
{
  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  CpuProgramRunner::Settings settings;

  settings.rowCapacityPerTable = 1u << 21u;
  settings.workers = workers;

  CpuProgramRunner runner(compiled.program, compiled.usage, store, settings);

  runner.seed(facts);

  RunReport report;

  runner.setMatchCallback([&report](const uint32_t, const RuleResult &result) {
    ++report.callbackFires;
    report.rows = result.rows;
  });

  const Clock::time_point start = Clock::now();

  runner.runToCompletion();

  report.totalMs = millisSince(start);
  report.ticks = runner.stats().ticks;

  return report;
}

struct GpuReport : RunReport
{
  std::vector<double> perTickMs;
  std::vector<std::pair<std::string, double>> passMs;
  uint32_t pagesAdmitted = 0u;
  uint64_t uploadWrites = 0u;
  uint64_t metadataFlushes = 0u;
  double msSubmitWait = 0.0;
  double msStatusRead = 0.0;
  double msService = 0.0;
};

GpuReport runGpu(::rendering::RHI *rhi, const Program &compiled, const std::vector<Fact> &facts, const uint32_t slots,
                 const bool timers, const uint32_t edgesPerThread = 8u)
{
  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  GpuRunnerSettings settings;

  settings.slotCount = slots;
  settings.rowCapacityPerTable = 1u << 21u;
  settings.maxRows = 1u << 21u;
  settings.maxPagesPerPartition = 1u << 16u;
  settings.tableWords = 1ull << 24u;
  settings.enableTimers = timers;
  settings.edgesPerThread = edgesPerThread;

  GpuProgramRunner runner(rhi, compiled.program, compiled.usage, store, settings);

  runner.seed(facts);
  runner.createResources();

  GpuReport report;

  runner.setMatchCallback([&report](const uint32_t, const RuleResult &result) {
    ++report.callbackFires;
    report.rows = result.rows;
  });

  const Clock::time_point start = Clock::now();

  while (!runner.complete())
  {
    const Clock::time_point tickStart = Clock::now();

    const TickResult tick = runner.tick();

    report.perTickMs.push_back(millisSince(tickStart));

    if (tick.overflowed || tick.starved)
    {
      break;
    }
  }

  report.totalMs = millisSince(start);
  report.ticks = runner.stats().ticks;
  report.pagesAdmitted = runner.stats().pagesAdmitted;
  report.uploadWrites = runner.stats().uploadWrites;
  report.metadataFlushes = runner.stats().metadataFlushes;
  report.msSubmitWait = runner.stats().msSubmitWait;
  report.msStatusRead = runner.stats().msStatusRead;
  report.msService = runner.stats().msService;

  if (timers)
  {
    for (const ::rendering::RenderGraph::PassTimingSummary &summary : runner.passTimings())
    {
      report.passMs.emplace_back(summary.name, summary.gpuTimeMs);
    }
  }

  return report;
}

void benchmark(::rendering::RHI *rhi, const uint32_t edgeCount)
{
  Program compiled;

  const std::string source = readProgramSource();

  check(!source.empty(), "benchmark programme found");

  if (source.empty())
  {
    return;
  }

  compiled.ok = compiler::compile(source, "benchmark.graphwrite", compiler::CompileOptions(), compiled.sink, compiled.program,
                                  compiled.usage);

  check(compiled.ok, "benchmark programme compiles");

  const uint32_t vertices = std::max(1000u, edgeCount / 8u);
  const std::vector<Fact> facts = randomGraph(edgeCount, vertices, 8u, 20260919u);

  std::cout << "\n  === " << facts.size() << " edges, " << vertices << " vertices ===\n";

  // --- correctness first: the numbers are worthless if they disagree -----
  // Pages needed for the whole graph, so the pool can be sized to fit it - or
  // deliberately not, to measure what thrashing costs.
  const uint32_t pagesNeeded = static_cast<uint32_t>(facts.size() / 128u) + 64u;

  const RunReport cpu = runCpu(compiled, facts, 0u);
  const GpuReport gpu = runGpu(rhi, compiled, facts, pagesNeeded, false);

  checkEqual(gpu.rows, cpu.rows, "GPU and CPU agree on the match count at " + std::to_string(facts.size()) + " edges");
  check(cpu.rows > 0u, "and the graph actually contains matches");
  checkEqual(cpu.callbackFires, 1u, "the match callback fires once per rule on the CPU");
  checkEqual(gpu.callbackFires, 1u, "and once on the GPU");

  // --- workers must not change the answer -------------------------------
  const RunReport threaded = runCpu(compiled, facts, WorkerPool::hardwareWorkers());

  checkEqual(threaded.rows, cpu.rows, "worker threads do not change the CPU result");

  // --- timings ----------------------------------------------------------
  constexpr uint32_t repeats = 5u;

  std::vector<double> gpuTotals;
  std::vector<double> cpuTotals;
  std::vector<double> threadedTotals;

  for (uint32_t run = 0u; run < repeats; ++run)
  {
    gpuTotals.push_back(runGpu(rhi, compiled, facts, pagesNeeded, false).totalMs);
    cpuTotals.push_back(runCpu(compiled, facts, 0u).totalMs);
    threadedTotals.push_back(runCpu(compiled, facts, WorkerPool::hardwareWorkers()).totalMs);
  }

  const double gpuMs = median(gpuTotals);
  const double cpuMs = median(cpuTotals);
  const double threadedMs = median(threadedTotals);

  const double edges = static_cast<double>(facts.size());
  const uint32_t patternEdges = compiled.program.ruleEdgeCounts.empty() ? 1u : compiled.program.ruleEdgeCounts[0];

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "  matches:              " << cpu.rows << "\n";
  std::cout << "  cpu  (serial):        " << cpuMs << " ms   " << (cpuMs * 1e6 / edges) << " ns/edge\n";
  std::cout << "  cpu  (" << WorkerPool::hardwareWorkers() << " workers):     " << threadedMs << " ms   "
            << (threadedMs * 1e6 / edges) << " ns/edge   " << (cpuMs / std::max(0.001, threadedMs)) << "x\n";
  std::cout << "  gpu:                  " << gpuMs << " ms   " << (gpuMs * 1e6 / edges) << " ns/edge\n";
  std::cout << "  gpu ticks:            " << gpu.ticks << "   " << (gpuMs / std::max(1u, gpu.ticks)) << " ms/tick\n";
  std::cout << "  gpu per pattern edge: " << (gpuMs / std::max(1u, patternEdges)) << " ms   (" << patternEdges << " edges in the pattern)\n";
  std::cout << "  pool slots:           " << pagesNeeded << "\n";
  std::cout << "  pages admitted:       " << gpu.pagesAdmitted << "\n";

  // Per tick, so setup is separable from steady state. Averaging a first tick
  // that paid for uploads into four that did not is how a number stops meaning
  // anything.
  std::cout << "  per-tick ms:         ";

  for (const double tickMs : gpu.perTickMs)
  {
    std::cout << " " << tickMs;
  }

  std::cout << "\n";

  // Where the tick actually goes. A tick submits the graph and then blocks on
  // it, so if submit+wait is the whole tick then no amount of kernel or
  // dispatch tuning can move it - the round trip is the cost.
  std::cout << "  tick breakdown:       submit+wait " << gpu.msSubmitWait << " ms, status read " << gpu.msStatusRead
            << " ms, service " << gpu.msService << " ms  (total " << gpuMs << " ms over " << gpu.ticks << " ticks)\n";

  // --- per-pass GPU timings ---------------------------------------------
  const GpuReport timed = runGpu(rhi, compiled, facts, pagesNeeded, true);

  if (!timed.passMs.empty())
  {
    std::cout << "  per-pass gpu time:\n";

    for (const auto &entry : timed.passMs)
    {
      if (entry.second > 0.0)
      {
        std::cout << "    " << std::setw(34) << std::left << entry.first << std::right << entry.second << " ms\n";
      }
    }
  }

  check(gpuMs > 0.0, "the GPU run took measurable time");

  // --- edges per thread -------------------------------------------------
  std::cout << "  edges/thread sweep:\n";

  for (const uint32_t ept : {1u, 8u, 16u, 32u})
  {
    std::vector<double> runs;

    for (uint32_t run = 0u; run < 3u; ++run)
    {
      runs.push_back(runGpu(rhi, compiled, facts, pagesNeeded, false, ept).totalMs);
    }

    const GpuReport check1 = runGpu(rhi, compiled, facts, pagesNeeded, false, ept);

    checkEqual(check1.rows, cpu.rows, "edges/thread = " + std::to_string(ept) + " produces the same matches");

    std::cout << "    " << std::setw(2) << ept << " edges/thread: " << median(runs) << " ms   " << (median(runs) * 1e6 / edges)
              << " ns/edge\n";
  }

  // What it costs when the working set does NOT fit. This is the difference
  // between streaming and thrashing, and it is worth having as a number rather
  // than as an intuition.
  const GpuReport squeezed = runGpu(rhi, compiled, facts, pagesNeeded / 4u, false);

  std::cout << "  gpu, quarter pool:    " << squeezed.totalMs << " ms   " << squeezed.ticks << " ticks, "
            << squeezed.pagesAdmitted << " pages admitted, " << squeezed.uploadWrites << " buffer writes, "
            << squeezed.metadataFlushes << " metadata flushes\n";
  std::cout << "    per-tick:          ";

  for (const double tickMs : squeezed.perTickMs)
  {
    std::cout << " " << tickMs;
  }

  std::cout << "\n";
  std::cout << "  gpu, full pool:       " << gpu.uploadWrites << " buffer writes for " << gpu.pagesAdmitted << " pages\n";

  checkEqual(squeezed.rows, cpu.rows, "a pool too small to hold the graph still produces the same matches");
}

} // namespace

int main(int argc, char **argv)
{
  // Default is the small end so the suite stays fast; pass an edge count to
  // push it to a million.
  uint32_t edgeCount = 500000u;

  if (argc > 1)
  {
    edgeCount = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
  }

  ::rendering::DeviceRequiredLimits limits = {
      .minimumMemory = 0,
      .minimumComputeSharedMemory = 0,
      .minimumComputeWorkGroupInvocations = 0,
  };

  ::rendering::backend::vulkan::VulkanRHI *rhi = new ::rendering::backend::vulkan::VulkanRHI(
      ::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  rhi->init(surfaces);

  benchmark(rhi, edgeCount);

  delete rhi;

  return report("GraphRewriteBenchmarkTests");
}
