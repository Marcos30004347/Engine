#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapPlateaus.hpp"
#include "procedural/world/MapRivers.hpp"

#include <string>
#include <vector>

namespace procedural::world {

struct StylizeSettings
{
  // How much of the relief to add at the top of the range. The bound below is
  // what keeps the map monotone.
  double gain = 0.55;
  // Where the expansion starts and ends, as fractions of the land relief.
  double lowFraction = 0.25;
  double highFraction = 1.0;
  // Prominence a divide must have before it is worth an edge in the mesh.
  double ridgeProminenceFraction = 0.06;
  double seaLevelMetres = 0.0;
};

struct StylizeReport
{
  double gainApplied = 0.0;
  double maximumGain = 0.0;
  double reliefBefore = 0.0;
  double reliefAfter = 0.0;
  uint32_t ridgeChains = 0u;
  uint32_t peaks = 0u;
  bool monotone = true;
};

// A ridge line: the divide between two drainage basins, in world metres.
struct RidgeLine
{
  std::vector<Point2> points;
  double prominenceMetres = 0.0;
};

// Stage 9C: expand the upper elevation band so ranges read as ranges.
//
// At map scale a country-sized world has under one percent of relief, and
// rendered honestly its mountains are imperceptible bumps. The remap is a
// strictly monotone function of elevation *alone*, which is what makes it safe:
// a monotone map cannot reverse any descent relation, so every river still runs
// downhill and the drainage network survives untouched. The gain is bounded so
// the derivative cannot reach zero.
//
// Runs after drainage and before the surface is published, so the contours and
// everything derived from them describe the stylized world.
bool stylizeRelief(const StylizeSettings &settings, MapGraph &graph, PlateauField &plateaus, StylizeReport *report = nullptr, std::string *error = nullptr);

// Divides, extracted from the drainage network rather than reconstructed.
//
// Follow `downstream` to a sink to label each cell's basin; a cell boundary
// whose two sides belong to different basins *is* a divide. That is exact and
// free, and it is what gives a low-poly mesh a crest edge instead of triangles
// straddling the ridge and rounding it off.
std::vector<RidgeLine> extractRidges(const StylizeSettings &settings, const MapGraph &graph, const RiverNetwork &rivers);

// Local maxima, as points to pin. Without them a summit is clipped flat by
// whichever triangle happens to cover it.
std::vector<Point2> extractPeaks(const MapGraph &graph);

} // namespace procedural::world
