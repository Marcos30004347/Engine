#include "WorldPipelineDirector.hpp"
#include "WorldPipelineStages.hpp"
#include <chrono>

namespace procedural::world {
std::vector<rewrite::GenerationTask> worldPipelineGenerators(const WorldPipelineCallbacks &callbacks)
{
  using namespace rewrite;
  std::vector<GenerationTask> tasks;
  const auto &names = worldPipelineStageNames();
  for (size_t i = 0; i < names.size(); ++i)
  {
    GenerationKey settings{"world.settings", 1}, previous{i ? "world." + names[i - 1] : "world.initial", 1}, output{"world." + names[i], 1};
    GenerationTask task;
    task.descriptor = {"world." + names[i], 1, 1, GenerationScope::World, {{settings, typeid(WorldPipelineSettings), 1}}, {{output, typeid(GeneratedWorld), 1}}};
    if (i)
    {
      task.descriptor.reads.push_back({previous, typeid(GeneratedWorld), 1});
    }
    task.generate = [=](const GenerationRequest &r, GenerationSnapshot &delta, std::string *e) {
      auto config = r.context.get<WorldPipelineSettings>(settings);
      if (!config)
      {
        return false;
      }
      GeneratedWorld result;
      if (i)
      {
        auto prior = r.context.get<GeneratedWorld>(previous);
        if (!prior)
        {
          return false;
        }
        result = *prior;
      }
      if (!executeWorldPipelineStage(i, *config, result, callbacks, e))
      {
        return false;
      }
      delta.emplace(output, GenerationProduct{std::move(result), 1});
      return true;
    };
    tasks.push_back(std::move(task));
  }
  return tasks;
}

bool generateWorldDirected(const WorldPipelineSettings &settings, GeneratedWorld &world, const WorldPipelineCallbacks &callbacks, std::string *error)
{
  using namespace rewrite;
  const auto start = std::chrono::steady_clock::now();
  GenerationSnapshot result;
  if (!runGeneratorsProjected(worldPipelineGenerators(callbacks), {{{"world.settings", 1}, {settings, 1}}}, settings.brief.seed, result, {{"world.summary", 1}}, 1, callbacks.cancelled, error))
  {
    return false;
  }
  auto candidate = std::move(std::any_cast<GeneratedWorld &>(result.at({"world.summary", 1}).value));
  candidate.totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (callbacks.setPhase)
  {
    callbacks.setPhase("Done", 1.0);
  }
  world = std::move(candidate);
  return true;
}
} // namespace procedural::world
