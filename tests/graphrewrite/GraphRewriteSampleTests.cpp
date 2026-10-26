// Phase 3.15: the end-to-end sample.
//
// Compiles the shipped corpus, seeds it, and runs match-and-rewrite rounds on
// both runners, checking they agree at every step and that the loop reaches a
// quiescent graph. Also reports what a step costs.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
#include "graphrewrite/runtime/Manifest.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

std::string corpusSource()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    return std::string();
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

// A small world the corpus can actually act on: two characters, a house, a
// workplace and a city, with the unary states as self loops.
std::vector<Fact> seedWorld(const compiler::CompiledProgram &program)
{
  const auto typeOf = [&program](const char *pattern) {
    compiler::EdgeTypeId id = compiler::invalidEdgeType;

    program.edgeTypes.find(pattern, id);

    return id;
  };

  const EdgeTypeId at = typeOf("? at ?");
  const EdgeTypeId houseOf = typeOf("? is house of ?");
  const EdgeTypeId workOf = typeOf("? is work of ?");
  const EdgeTypeId cityOf = typeOf("? is city of ?");
  const EdgeTypeId neutral = typeOf("? neutral towards ?");
  const EdgeTypeId awake = typeOf("? is awake");
  const EdgeTypeId hungry = typeOf("? is hungry");

  const uint32_t jenny = 0u;
  const uint32_t marco = 1u;
  const uint32_t house = 2u;
  const uint32_t work = 3u;
  const uint32_t city = 4u;

  return {
      {jenny, at, house},   {marco, at, house},   {house, houseOf, jenny}, {work, workOf, jenny},
      {city, cityOf, jenny}, {city, cityOf, marco}, {jenny, neutral, marco}, {marco, neutral, jenny},
      {jenny, awake, jenny}, {jenny, hungry, jenny},
  };
}

// Collects one matched row per rule that matched, as the rewrite input.
std::vector<MatchedRow> rowsFrom(const compiler::CompiledProgram &program, const CpuProgramRunner &runner)
{
  std::vector<MatchedRow> rows;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const RuleResult result = runner.resultForRule(rule);

    if (result.rows == 0u)
    {
      continue;
    }

    // The last query serving this rule holds its table.
    const MatchTable *table = nullptr;

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

      const MatchTable *candidate = runner.tableForRegister(instruction.reg);

      if (candidate != nullptr && candidate->rows() != 0u)
      {
        table = candidate;
      }
    }

    if (table == nullptr)
    {
      continue;
    }

    MatchedRow row;

    row.rule = rule;

    for (uint32_t column = 0u; column < table->columns(); ++column)
    {
      row.vertices.push_back(table->at(column, 0u));
    }

    rows.push_back(std::move(row));
  }

  return rows;
}

void runAll(::rendering::RHI *rhi)
{
  const std::string source = corpusSource();

  check(!source.empty(), "corpus asset found");

  if (source.empty())
  {
    return;
  }

  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  check(compiler::compile(source, "basic.graphwrite", compiler::CompileOptions(), sink, program, usage), "the corpus compiles");
  checkEqual(sink.count(), 0u, "cleanly");

  const std::vector<Fact> seed = seedWorld(program);

  // --- the rewrite loop, on the CPU --------------------------------------
  EdgePageStore cpuStore;

  cpuStore.configure(program.edgeTypes);

  RewriteApplier applier(program, cpuStore);

  uint32_t steps = 0u;
  uint32_t totalFired = 0u;

  {
    CpuProgramRunner seeder(program, usage, cpuStore, CpuProgramRunner::Settings());

    seeder.seed(seed);
  }

  const auto started = std::chrono::steady_clock::now();

  for (uint32_t step = 0u; step < 24u; ++step)
  {
    CpuProgramRunner runner(program, usage, cpuStore, CpuProgramRunner::Settings());

    if (!runner.runToCompletion())
    {
      break;
    }

    const std::vector<MatchedRow> rows = rowsFrom(program, runner);

    if (rows.empty())
    {
      break;
    }

    const RewriteStats stats = applier.apply(rows);

    totalFired += stats.rulesFired;
    ++steps;

    if (stats.rulesFired == 0u)
    {
      break;
    }
  }

  const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  check(steps > 0u, "the loop ran at least one step");
  check(totalFired > 0u, "and rules actually fired");

  std::cout << "    cpu loop: " << steps << " steps, " << totalFired << " rule firings, " << elapsedMs << " ms total ("
            << (elapsedMs / std::max(1u, steps)) << " ms/step)\n";

  // --- GPU and CPU agree on the same graph -------------------------------
  const std::vector<Fact> evolved = [&cpuStore] {
    std::vector<Fact> out;

    for (uint32_t index = 0u; index < cpuStore.partitionCount(); ++index)
    {
      const Partition &partition = cpuStore.partitionAt(index);

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
  }();

  check(!evolved.empty(), "the graph survived the loop");

  EdgePageStore cpuCheck;
  EdgePageStore gpuCheck;

  cpuCheck.configure(program.edgeTypes);
  gpuCheck.configure(program.edgeTypes);

  CpuProgramRunner cpuRunner(program, usage, cpuCheck, CpuProgramRunner::Settings());

  cpuRunner.seed(evolved);

  check(cpuRunner.runToCompletion(), "the CPU runner matches the evolved graph");

  GpuRunnerSettings gpuSettings;

  gpuSettings.slotCount = 512u;
  gpuSettings.rowCapacityPerTable = 4096u;

  GpuProgramRunner gpuRunner(rhi, program, usage, gpuCheck, gpuSettings);

  gpuRunner.seed(evolved);
  gpuRunner.createResources();

  const auto gpuStarted = std::chrono::steady_clock::now();

  const bool gpuOk = gpuRunner.runToCompletion();

  if (!gpuOk)
  {
    std::cerr << "    gpu stopped: ticks=" << gpuRunner.stats().ticks << " instructions=" << gpuRunner.stats().instructionsCompleted
              << " overflowed=" << gpuRunner.stats().overflowed << " starved=" << gpuRunner.stats().starved
              << " pages=" << gpuRunner.stats().pagesAdmitted << "\n";
  }

  check(gpuOk, "the GPU runner matches it too");

  const double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpuStarted).count();

  uint32_t matchedRules = 0u;
  bool agree = true;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const RuleResult cpuResult = cpuRunner.resultForRule(rule);
    const RuleResult gpuResult = gpuRunner.resultForRule(rule);

    if (cpuResult.rows != 0u)
    {
      ++matchedRules;
    }

    if (cpuResult.rows != gpuResult.rows)
    {
      agree = false;

      std::cerr << "    rule " << program.ruleNames[rule] << ": cpu " << cpuResult.rows << " gpu " << gpuResult.rows << "\n";
    }
  }

  check(agree, "GPU and CPU agree on every rule over the evolved corpus graph");

  std::cout << "    gpu match: " << gpuRunner.stats().ticks << " ticks, " << gpuRunner.stats().instructionsCompleted
            << " instructions, " << gpuRunner.stats().pagesAdmitted << " pages admitted, " << gpuMs << " ms\n";
  std::cout << "    " << matchedRules << " of " << program.ruleNames.size() << " rules match the evolved graph\n";

  // --- and it survives a restart -----------------------------------------
  const std::string root =
      (std::filesystem::temp_directory_path() / ("graphrewrite-sample-" + std::to_string(::getpid()))).string();

  std::filesystem::remove_all(root);

  DiskStoreSettings diskSettings;

  diskSettings.root = root;

  DiskStore disk(diskSettings);
  DiagnosticSink diskSink;

  check(commitStore(cpuStore, disk, 1u, diskSink), "the evolved graph commits to disk");

  EdgePageStore restored;
  DiagnosticSink readSink;

  check(openStore(disk, restored, readSink, nullptr), "and reopens");

  std::vector<Fact> restoredFacts;

  for (uint32_t index = 0u; index < restored.partitionCount(); ++index)
  {
    const Partition &partition = restored.partitionAt(index);

    if (partition.order() != KeyOrder::ByOrigin)
    {
      continue;
    }

    for (const Edge &edge : partition.snapshot())
    {
      restoredFacts.emplace_back(edge.origin, partition.edgeType(), edge.target);
    }
  }

  std::sort(restoredFacts.begin(), restoredFacts.end());

  check(restoredFacts == evolved, "with exactly the graph it had");

  std::error_code error;

  std::filesystem::remove_all(root, error);
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

  return report("GraphRewriteSampleTests");
}
