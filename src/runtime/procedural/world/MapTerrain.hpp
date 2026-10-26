#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapRivers.hpp"

#include <string>

namespace procedural::world {

// Post-processes the authored elevation intent before the definitive drainage
// pass. This operates at map scale: it creates broad buildable ground, not the
// metre-scale terraces and channel cross-sections owned by terrain synthesis.
struct TerrainConditioningSettings
{
  // Conservative Laplacian relaxation inside plain and plateau cells. Type
  // boundaries are excluded, preserving escarpments and mountain fronts.
  uint32_t planarPasses = 4u;
  double planarStrength = 0.48;
  double maximumPlanarChangeMetres = 90.0;

  // Major rivers grade nearby ground toward a shallow cross-valley profile.
  // The preliminary drainage network is discarded and rebuilt afterwards, so
  // the definitive rivers still obey the conditioned surface.
  double floodplainWidthMetres = 26000.0;
  double floodplainStrength = 0.72;
  double floodplainBankGradient = 0.0012;
  double maximumFloodplainChangeMetres = 420.0;
  double majorRiverDischargeFraction = 0.035;
};

struct TerrainConditioningReport
{
  uint32_t planarCells = 0u;
  uint32_t floodplainCells = 0u;
  double totalElevationChangeMetres = 0.0;
};

// `rivers` is a preliminary network over the unconditioned graph. Node ids
// must correspond to `graph`. The caller must rebuild drainage afterwards.
bool conditionTerrain(const TerrainConditioningSettings &settings, const RiverNetwork &rivers, MapGraph &graph, TerrainConditioningReport *report = nullptr, std::string *error = nullptr);

} // namespace procedural::world
