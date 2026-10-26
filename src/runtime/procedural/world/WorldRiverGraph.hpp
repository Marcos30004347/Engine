#pragma once
#include "WorldLandProduct.hpp"
#include <array>

namespace procedural::world::rewrite {
struct RiverMouth
{
  uint64_t coastEdge = 0;
  double parameter = 0.5;
  uint32_t priority = 3;
};

struct RiverGrowthSettings
{
  double edgeLengthMetres = 100;
  double coastClearanceMetres = 30, separationMetres = 20;
  double maximumSlope = 0.25, elevationWindowMetres = 10;
  double continuationProbability = 0.2, symmetricProbability = 0.4;
  uint32_t maximumNodes = 512, attemptsPerChild = 8;
  double maximumElevationPerTerrainLambda = 0; // Optional regional elevation envelope, metres.
  double maximumCoastalSlope = 0;              // Optional sea-level Lipschitz guard; zero disables.
  double mouthClearanceScale = 1;              // First inland node only; (0,1], permits estuary entry through narrow coves.
};

struct RiverGraphNode
{
  Position position;
  uint32_t receiver = UINT32_MAX;
  uint32_t growthPriority = 1, strahlerOrder = 1;
};

struct RiverGrowthProduct
{
  std::vector<RiverGraphNode> nodes;
  // Optional arc-length graded polylines indexed by upstream node; mouths are empty.
  // Refining these preserves the receiver/catchment graph and invalidates physical consumers.
  std::vector<std::vector<Position>> reachPaths;
  // continuation, symmetric, asymmetric, order-one filling productions.
  std::array<uint32_t, 4> productions{};
  // coast/domain, separation/intersection, Lipschitz compatibility failures.
  std::array<uint32_t, 3> rejections{};
  uint32_t attempts = 0, exhaustedChildren = 0;
  // A valid truncated graph is explicit; it is not a completed domain coverage.
  bool budgetReached = false;
};

// Mouth-first growth following Genevaux et al. 2013 section 4. Authored mouths
// reference the frozen physical coastline. Brute-force validation is bounded;
// acceleration and automatic mouth placement are separate work.
bool growRiverGraph(const LandProduct &, const ReliefControls &, const std::vector<RiverMouth> &, const RiverGrowthSettings &, RiverGrowthProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
