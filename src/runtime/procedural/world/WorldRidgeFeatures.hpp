#pragma once
#include "WorldWatersheds.hpp"

namespace procedural::world::rewrite {
struct ReliefPeak
{
  uint32_t vertex = 0, parentPeak = UINT32_MAX, keySaddle = UINT32_MAX;
  double heightMetres = 0, prominenceMetres = 0;
  bool significant = false;
};

struct ReliefPass
{
  uint32_t vertex = 0, lowerPeak = 0, higherPeak = 0;
  double heightMetres = 0, prominenceMetres = 0;
};

struct RidgeFeatures
{
  std::vector<ReliefPeak> peaks;
  std::vector<ReliefPass> passes;
  uint32_t components = 0;
};

// Descending superlevel-set merge tree. Equal-height connected vertices form a
// single plateau. Prominence is measured on the supplied graph; use mesh edges
// for a sampled surface estimate, crest edges for ridge-network descriptors.
// The surviving highest peak in each component uses the explicit reference level.
bool analyzeReliefTopology(
    const std::vector<Position> &vertices,
    const std::vector<std::array<uint32_t, 2>> &edges,
    double referenceLevelMetres,
    double minimumProminenceMetres,
    RidgeFeatures &,
    std::string *error = nullptr);
bool analyzeRidgeFeatures(const WatershedProduct &, double referenceLevelMetres, double minimumProminenceMetres, RidgeFeatures &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
