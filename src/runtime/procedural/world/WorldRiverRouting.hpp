#pragma once
#include "WorldRiverPath.hpp"
#include <functional>

namespace procedural::world::rewrite {
struct RiverRoutingSettings
{
  double longitudinalSpacingMetres = 2, lateralSpacingMetres = 1, corridorHalfWidthMetres = 24;
  double maximumIncisionMetres = 12, maximumWaterAboveGroundMetres = .1;
  double incisionWeight = 1, resistanceWeight = 8, adverseGradeWeight = 10, turningWeight = 4;
  double minimumBendRadiusMetres = 8, minimumSeparationMetres = 2, junctionRadiusMetres = 12;
  double meanderAmplitudeMetres = 12, meanderWavelengthMetres = 120, meanderWeight = .02;
  double maximumMeanderSlope = .015;
  uint32_t smoothingPasses = 24;
  uint64_t seed = 7;
};

struct RiverRouteMetrics
{
  uint32_t upstreamNode = 0;
  double chordMetres = 0, lengthMetres = 0, sinuosity = 0, maximumCurvature = 0, maximumIncision = 0, maximumOffset = 0;
  uint64_t statesVisited = 0;
};

// Terrain and resistance are frozen world-space fields. Resistance must be >=0;
// non-finite terrain/resistance denotes forbidden ground. Output is atomic.
// A longitudinal position/lateral position/heading DAG supports forward valley
// bends, not oxbows which reverse direction along the original reach chord.
bool refineRiverPaths(
    const RiverGrowthProduct &,
    const std::function<double(double, double)> &terrain,
    const std::function<double(double, double)> &resistance,
    const RiverRoutingSettings &,
    RiverGrowthProduct &,
    std::vector<RiverRouteMetrics> &,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
