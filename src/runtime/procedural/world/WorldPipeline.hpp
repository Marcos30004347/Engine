#pragma once

#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapContours.hpp"
#include "procedural/world/MapElevation.hpp"
#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapFluvial.hpp"
#include "procedural/world/MapPlateaus.hpp"
#include "procedural/world/MapStylize.hpp"
#include "procedural/world/MapRegions.hpp"
#include "procedural/world/MapRivers.hpp"
#include "procedural/world/MapSurface.hpp"
#include "procedural/world/MapTerrain.hpp"
#include "procedural/world/WorldBrief.hpp"
#include "procedural/world/land/Blend.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace procedural::world
{

// Everything the map stage needs, as one struct rather than as nineteen
// positional parameters. The stage settings are the same types the individual
// stages take; only the values the driver derives itself - spacing, hierarchy
// depth, the landmass knobs - are named here.
struct WorldPipelineSettings
{
  WorldBrief brief{};

  // Land mask. `grownLandmasses` picks growth over the analytic raisers: the
  // archetypes cannot make a shape that is not convex, and growing can.
  bool grownLandmasses = true;
  double landFraction = 0.75;
  double coastComplexity = 0.25;
  double coastDetail = 0.55;
  // Also the craton count per continent, and therefore how many seams there
  // are to put ranges on.
  uint32_t landmassCores = 9u;
  uint32_t continentCount = 1u;
  double continentSpanMetres = 0.0;
  double cratonSpread = 0.0;

  // Leaf cell spacing, and how many coarser partitions sit above it. Each
  // level up is four times the spacing.
  double cellSpacingMetres = 0.0;
  uint32_t hierarchyLevels = 1u;

  // Vertical spacing of the elevation contours. Zero derives one from the
  // relief actually produced, which matters because the relief a brief asks
  // for and the relief the elevation stage delivers are not the same number at
  // every world size: a fixed 100 m interval finds nothing at all on a 32 km
  // world whose land spans 70 m.
  double contourIntervalMetres = 0.0;
  // Bands to aim for when deriving it.
  uint32_t contourBandTarget = 26u;

  // Stage 5A. Ranges and drainage emerge together from uplift opposed by
  // incision, rather than being placed and then routed around each other.
  bool fluvialErosion = false;
  FluvialSettings fluvial{};

  // Stage 5B. Off by default: flattening changes the map every later stage
  // reads, so it is opt-in until a brief asks for it.
  bool plateaus = false;
  PlateauSettings plateau{};

  // Stage 9C. A monotone remap of elevation alone, so drainage cannot break.
  bool stylizeRanges = false;
  StylizeSettings stylize{};

  BlendSettings blend{};
  ElevationSettings elevation{};
  TerrainConditioningSettings terrain{};
  RiverSettings rivers{};
  LakeSettings lakes{};
  RegionSettings regions{};
  ClimateSettings climate{};
};

// The result of one generation, as a single unit. Everything downstream of the
// graph is derived, so they are produced together and swapped in together: a
// half-updated set - a new graph against the previous derived data - describes
// a world that never existed.
//
// Deliberately carries no view data. The cartographic mesh is a *view* of this
// and belongs to whoever draws it; two samples want the same world and
// different pictures of it.
struct GeneratedWorld
{
  MapGraph graph;
  std::vector<CoastPolyline> coastline;
  RiverNetwork rivers;
  RegionMap regions;
  ClimateMap climate;
  ContourSet contours;
  SiteIndex index;
  MapSurfaceField surface;
  PlateauField plateaus;
  FluvialReport fluvial{};
  StylizeReport stylization{};
  std::vector<RidgeLine> ridges;
  std::vector<Point2> peaks;

  double extentMetres = 0.0;
  double reliefMetres = 3000.0;

  double partitionSeconds = 0.0;
  double raiseSeconds = 0.0;
  double coastSeconds = 0.0;
  double elevationSeconds = 0.0;
  double riverSeconds = 0.0;
  double regionSeconds = 0.0;
  double climateSeconds = 0.0;
  double totalSeconds = 0.0;

  uint32_t hierarchyLevels = 1u;
  uint32_t leafCells = 0u;

  // The requested archetype beside the one measured from the result. Reporting
  // both is what makes "does the tag do anything" answerable without squinting
  // at the picture.
  Tag requestedShape = Tag::Island;
  Tag measuredShapeTag = Tag::Island;
  double borderLand = 0.0;

  uint32_t landCells = 0u;
  uint32_t lakeCells = 0u;
  uint32_t incompatibleBorders = 0u;
  uint32_t lakesFilled = 0u;
  uint32_t waterfalls = 0u;
  uint32_t mountainCells = 0u;
  uint32_t glacierCells = 0u;
  uint32_t cliffCells = 0u;
  uint32_t canyonCells = 0u;
  TerrainConditioningReport terrainConditioning{};
  double highestMetres = 0.0;
  double deepestMetres = 0.0;
  double landAreaSquareMetres = 0.0;
};

// Progress reporting and cancellation, supplied by the caller so the pipeline
// owns no threading. Both may be empty.
//
// Cancellation is checked between stages rather than inside them: no stage runs
// long enough that a mid-stage abort is worth the complexity, and a
// half-cancelled graph is a graph that fails validate().
struct WorldPipelineCallbacks
{
  std::function<void(const char *phase, double progress)> setPhase;
  std::function<bool()> cancelled;
};

// Runs the whole map stage: partition, raise, coast, elevation, conditioning,
// climate, rivers, lakes, the continuous surface and its contours, regions and
// names, then the summary back up the hierarchy.
//
// Returns false on failure or cancellation, filling `error`; `world` is then
// left unchanged. This compatibility entry point uses the shared stage DAG. Deterministic in
// `settings` alone - the same settings produce a graph with the same
// `MapGraph::signature()` on any machine, in any thread.
bool generateWorld(
    const WorldPipelineSettings &settings,
    GeneratedWorld &world,
    const WorldPipelineCallbacks &callbacks = {},
    std::string *error = nullptr);

} // namespace procedural::world
