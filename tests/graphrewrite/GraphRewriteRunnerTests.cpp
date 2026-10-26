// Phase 3.11: the programme runners.
//
// Two implementations of one interface - a GPU runner that owns its render graph
// and ticks it, and a CPU runner that does the same work in memory. Running both
// over the same programme and comparing per rule is the point: it turns "the GPU
// path looks plausible" into "the GPU path agrees with the reference".

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
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

std::vector<Fact> randomFacts(const uint32_t count, const uint32_t vertices, const uint32_t types, const uint32_t seed)
{
  std::mt19937 rng(seed);

  std::set<Fact> unique;

  while (unique.size() < count)
  {
    const uint32_t origin = rng() % vertices;
    const uint32_t target = rng() % vertices;

    if (origin != target)
    {
      unique.emplace(origin, static_cast<EdgeTypeId>(rng() % types), target);
    }
  }

  return std::vector<Fact>(unique.begin(), unique.end());
}

void cpuRunnerMatchesItsOwnOracle()
{
  Program compiled;

  compile("let A likes B.\n"
          "rule r: when A likes B then A likes B.\n",
          compiled);

  check(compiled.ok, "single-rule programme compiles");

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  CpuProgramRunner runner(compiled.program, compiled.usage, store, CpuProgramRunner::Settings());

  const std::vector<Fact> facts = randomFacts(300u, 40u, 1u, 11u);

  runner.seed(facts);

  check(runner.runToCompletion(), "the CPU runner completes");
  check(runner.complete(), "and reports itself complete");

  const RuleResult result = runner.resultForRule(0u);

  checkEqual(result.rows, static_cast<uint32_t>(facts.size()), "a one-edge rule matches every edge of its type");
}

void tickAdvancesOneInstructionAtATime()
{
  Program compiled;

  compile("let A likes B.\n"
          "let A knows B.\n"
          "rule r: when A likes B, B knows A then A likes B.\n",
          compiled);

  EdgePageStore store;

  store.configure(compiled.program.edgeTypes);

  CpuProgramRunner runner(compiled.program, compiled.usage, store, CpuProgramRunner::Settings());

  runner.seed(randomFacts(120u, 20u, 2u, 3u));

  uint32_t completedInstructions = 0u;

  for (uint32_t i = 0u; i < 64u; ++i)
  {
    const TickResult result = runner.tick();

    if (result.instructionComplete)
    {
      ++completedInstructions;
    }

    if (result.programComplete)
    {
      break;
    }
  }

  checkEqual(completedInstructions, 2u, "a two-edge rule takes two instruction completions");
  check(runner.complete(), "and the programme is then complete");
}

void gpuAndCpuAgree(::rendering::RHI *rhi, const std::string &source, const std::vector<Fact> &facts, const uint32_t slots,
                    const std::string &label)
{
  Program compiled;

  compile(source, compiled);

  if (!compiled.ok)
  {
    check(false, label + ": programme compiles");

    return;
  }

  EdgePageStore cpuStore;
  EdgePageStore gpuStore;

  cpuStore.configure(compiled.program.edgeTypes);
  gpuStore.configure(compiled.program.edgeTypes);

  CpuProgramRunner cpuRunner(compiled.program, compiled.usage, cpuStore, CpuProgramRunner::Settings());

  cpuRunner.seed(facts);

  check(cpuRunner.runToCompletion(), label + ": the CPU runner completes");

  GpuRunnerSettings gpuSettings;

  gpuSettings.slotCount = slots;
  gpuSettings.rowCapacityPerTable = 4096u;

  GpuProgramRunner gpuRunner(rhi, compiled.program, compiled.usage, gpuStore, gpuSettings);

  gpuRunner.seed(facts);
  gpuRunner.createResources();

  check(gpuRunner.runToCompletion(), label + ": the GPU runner completes");

  bool agree = true;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(compiled.program.ruleNames.size()); ++rule)
  {
    const RuleResult cpuResult = cpuRunner.resultForRule(rule);
    const RuleResult gpuResult = gpuRunner.resultForRule(rule);

    if (cpuResult.rows != gpuResult.rows)
    {
      agree = false;

      std::cerr << "    " << label << " rule " << compiled.program.ruleNames[rule] << ": cpu " << cpuResult.rows << " gpu "
                << gpuResult.rows << "\n";
    }
  }

  check(agree, label + ": every rule produces the same row count on GPU and CPU");

  std::cout << "    " << label << " (" << slots << " slots): " << gpuRunner.stats().ticks << " ticks, "
            << gpuRunner.stats().instructionsCompleted << " instructions, " << gpuRunner.stats().stalls << " stalls, "
            << gpuRunner.stats().pagesAdmitted << " pages admitted\n";
}

void runAll(::rendering::RHI *rhi)
{
  cpuRunnerMatchesItsOwnOracle();
  tickAdvancesOneInstructionAtATime();

  const char *oneRule = "let A likes B.\n"
                        "rule r: when A likes B then A likes B.\n";

  const char *twoEdges = "let A likes B.\n"
                         "let A knows B.\n"
                         "rule r: when A likes B, B knows A then A likes B.\n";

  const char *shared = "let C at L.\n"
                       "let C anger towards D.\n"
                       "let C philia towards D.\n"
                       "rule insult: when C at L, D at L, C anger towards D then C at L.\n"
                       "rule compliment: when C at L, D at L, C philia towards D then C at L.\n";

  gpuAndCpuAgree(rhi, oneRule, randomFacts(400u, 40u, 1u, 5u), 4096u, "one rule, whole pool");
  gpuAndCpuAgree(rhi, oneRule, randomFacts(400u, 40u, 1u, 5u), 2u, "one rule, two slots");
  gpuAndCpuAgree(rhi, twoEdges, randomFacts(200u, 25u, 2u, 9u), 4096u, "two edges, whole pool");
  gpuAndCpuAgree(rhi, twoEdges, randomFacts(200u, 25u, 2u, 9u), 1u, "two edges, one slot");
  gpuAndCpuAgree(rhi, shared, randomFacts(150u, 18u, 3u, 13u), 4096u, "shared prefix, whole pool");
  gpuAndCpuAgree(rhi, shared, randomFacts(150u, 18u, 3u, 13u), 3u, "shared prefix, three slots");
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

  return report("GraphRewriteRunnerTests");
}
