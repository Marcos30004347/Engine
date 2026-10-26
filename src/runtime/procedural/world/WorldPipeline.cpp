#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldPipelineStages.hpp"
#include "procedural/world/stages/WorldStage.hpp"

#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/MapLandmass.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/Raisers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace procedural::world {

namespace {

// Rounds up to the next 1, 2, 2.5 or 5 times a power of ten. Contour intervals
// are read off a map by eye, so they have to be numbers a person would choose.
double niceInterval(const double raw)
{
  if (!(raw > 0.0) || !std::isfinite(raw))
  {
    return 100.0;
  }
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  const double normalised = raw / magnitude;
  double step = 10.0;
  for (const double candidate : {1.0, 2.0, 2.5, 5.0})
  {
    if (normalised <= candidate)
    {
      step = candidate;
      break;
    }
  }
  return std::max(0.5, step * magnitude);
}

double secondsSince(const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

bool executeWorldPipelineStage(size_t stage, const WorldPipelineSettings &settings, GeneratedWorld &result, const WorldPipelineCallbacks &callbacks, std::string *error)
{
  // Dispatch, not a switch. Each stage is a small class in
  // procedural/world/stages; this entry point exists only so the existing
  // callers - the direct runner and the DAG adapter - keep working unchanged.
  const auto &stages = stages::worldMapStages();

  if (stage >= stages.size())
  {
    if (error)
    {
      *error = "unknown world pipeline stage";
    }
    return false;
  }

  const stages::Context context = stages::makeContext(settings, callbacks);

  // Cancellation is checked between stages rather than inside them: no stage
  // runs long enough that a mid-stage abort is worth the complexity, and a
  // half-cancelled graph is a graph that fails validate().
  if (context.cancelled())
  {
    if (error)
    {
      *error = "cancelled";
    }
    return false;
  }

  if (!stages[stage]->run(context, result, error))
  {
    return false;
  }

  if (context.cancelled())
  {
    if (error)
    {
      *error = "cancelled";
    }
    return false;
  }

  if (error)
  {
    error->clear();
  }
  return true;
}

const std::vector<std::string> &worldPipelineStageNames()
{
  // Derived from the stages themselves. The old parallel array of strings
  // could disagree with the switch beside it; a list of objects cannot.
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const auto &stage : stages::worldMapStages())
    {
      out.push_back(stage->name());
    }
    return out;
  }();
  return names;
}

bool generateWorld(const WorldPipelineSettings &settings, GeneratedWorld &world, const WorldPipelineCallbacks &callbacks, std::string *error)
{
  return generateWorldDirected(settings, world, callbacks, error);
}
} // namespace procedural::world
