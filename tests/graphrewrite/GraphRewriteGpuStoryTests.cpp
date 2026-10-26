// The story, simulated on the GPU: the same assets as GraphRewriteStoryTests,
// matched and rewritten by GpuProgramRunner with the CPU evaluating clauses
// and narrating on its own thread.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/GpuStoryRunner.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

std::string readAsset(const char *name)
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/" + name);
  std::ostringstream buffer;

  buffer << file.rdbuf();

  return buffer.str();
}

void theStoryRunsOnTheGpu(::rendering::RHI *rhi)
{
  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  const bool compiled =
      compiler::compile(readAsset("story.graphwrite"), "story.graphwrite", compiler::CompileOptions(), sink, program, usage);

  check(compiled, "the story compiles");

  LoadedFacts world;
  const uint32_t dayBase = StorySettings().dayBase;

  world.valueResolver = [dayBase](const uint32_t year, uint32_t &id) {
    id = dayBase + year * 52u;

    return true;
  };

  check(loadFactsText(readAsset("story.graphfacts"), program.edgeTypes, "story.graphfacts", sink, world), "and the cast loads");

  Narrator narrator;

  check(narrator.load(readAsset("story.narration"), "story.narration", sink), "and the narration loads");

  for (const Diagnostic &diagnostic : sink.all())
  {
    std::cerr << "    " << diagnostic.format("story") << "\n";
  }

  StorySettings settings;

  settings.seed = 11u;
  settings.ticks = 60u;

  GpuRunnerSettings gpu;

  gpu.rowCapacityPerTable = 4096u;
  gpu.maxRows = 4096u;
  gpu.tableWords = 1u << 22u;
  gpu.diceSeed = settings.seed;

  GpuStoryRunner story(rhi, program, usage, world, narrator, settings, gpu);

  const uint32_t before = story.population();

  story.run();

  check(story.summaries().size() >= 2u, "several weeks ran (" + std::to_string(story.summaries().size()) + ")");
  check(story.eventCount() > 0u, "something happened (" + std::to_string(story.eventCount()) + " events)");
  // Which buffer ran out matters more than that one did: "raise --rows" is
  // wrong advice when what filled up was the delta or the candidate list.
  const uint32_t kinds = story.overflowKinds();

  std::string what;

  if (kinds & 1u) { what += " dirty-pages"; }
  if (kinds & 2u) { what += " delta"; }
  if (kinds & 4u) { what += " candidates"; }
  if (kinds & 8u) { what += " fired"; }
  if (kinds & 16u) { what += " match-table(peak " + std::to_string(story.peakRowsNeeded()) + " rows)"; }

  checkEqual(story.overflowedWeeks(), 0u, "nothing overflowed" + (what.empty() ? std::string() : " -" + what));

  uint32_t fired = 0u;
  uint32_t scheduled = 0u;
  uint32_t released = 0u;
  uint32_t candidates = 0u;

  for (const GpuTickSummary &tick : story.summaries())
  {
    fired += tick.fired;
    scheduled += tick.scheduled;
    released += tick.released;
    candidates += tick.candidates;
  }

  check(fired > 0u, "rows fired on the GPU (" + std::to_string(fired) + ")");
  check(candidates > 0u, "the CPU judged clauses on candidate rows (" + std::to_string(candidates) + ")");
  check(scheduled > 0u, "and queued schedules (" + std::to_string(scheduled) + ")");

  std::cout << "    released on their day: " << released << "\n";

  const std::string text = story.text();

  check(text.find("-- Week ") != std::string::npos, "the text has week headers");
  check(text.find('{') == std::string::npos, "and leaves no placeholder unresolved");
  check(text.find("is alive") == std::string::npos, "and never mentions bookkeeping");
  check(story.population() + 40u > before,
        "the village did not collapse (" + std::to_string(before) + " -> " + std::to_string(story.population()) + ")");

  std::cout << story.footer();

  const bool married = text.find("were married at") != std::string::npos;

  std::cout << "    a wedding was told: " << (married ? "yes" : "no") << "\n";
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

  std::vector<VkSurfaceKHR> surfaces;

  rhi->init(surfaces);

  theStoryRunsOnTheGpu(rhi);

  delete rhi;

  return report("GraphRewriteGpuStoryTests");
}
