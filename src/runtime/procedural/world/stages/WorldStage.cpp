#include "procedural/world/stages/WorldStage.hpp"

#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/MapLandmass.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/Raisers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace procedural::world::stages {

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

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
  {
    *error = message;
  }

  return false;
}

// The highest land the graph actually carries. Measured rather than taken from
// the brief, because the relief a brief asks for and the relief the elevation
// stage delivers are not the same number.
double highestLandMetres(const GeneratedWorld &world, const double seaLevelMetres)
{
  double highest = seaLevelMetres;

  for (const MapNode &node : world.graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      highest = std::max(highest, node.intent.elevationMetres);
    }
  }

  return highest;
}

} // namespace

void Context::phase(const char *name, const double progress) const
{
  if (callbacks.setPhase)
  {
    callbacks.setPhase(name, progress);
  }
}

bool Context::cancelled() const
{
  return callbacks.cancelled && callbacks.cancelled();
}

Context makeContext(const WorldPipelineSettings &settings, const WorldPipelineCallbacks &callbacks)
{
  Context context{settings};
  context.cellSpacingMetres = settings.cellSpacingMetres;
  context.hierarchyLevels = settings.hierarchyLevels;
  context.climate = settings.climate;
  context.climate.extentMetres = settings.brief.extentMetres;
  context.climate.seed = settings.brief.seed;
  context.callbacks = callbacks;

  return context;
}

bool PartitionStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &brief = context.settings.brief;

  // The first stage owns the reset, because it is the only one that can know
  // the world is starting over.
  world = GeneratedWorld{};
  world.extentMetres = brief.extentMetres;
  world.requestedShape = shapeTag(brief.tags);

  context.phase(phase(), progress());
  world.graph.setBrief(brief);

  HierarchySettings hierarchy{};
  hierarchy.extentMetres = brief.extentMetres;
  hierarchy.seed = brief.seed;
  hierarchy.cellSpacingMetres.clear();

  const double coarsestSpacing = context.cellSpacingMetres * std::pow(4.0, static_cast<double>(context.hierarchyLevels - 1u));
  const double coarseSitesPerAxis = brief.extentMetres / coarsestSpacing;

  if (!(brief.extentMetres > 0.0) || !std::isfinite(brief.extentMetres) || !(coarsestSpacing > 0.0) || !std::isfinite(coarsestSpacing) || !std::isfinite(coarseSitesPerAxis) || coarseSitesPerAxis < 3.0)
  {
    std::ostringstream message;
    message << "hierarchy needs at least 3 coarse sites per axis; extent " << brief.extentMetres * 1.0e-3 << " km, spacing " << context.cellSpacingMetres * 1.0e-3 << " km and " << context.hierarchyLevels
            << " levels provide " << coarseSitesPerAxis;

    return fail(error, message.str());
  }

  for (double spacing = coarsestSpacing; hierarchy.cellSpacingMetres.size() + 1u < context.hierarchyLevels; spacing /= 4.0)
  {
    hierarchy.cellSpacingMetres.push_back(spacing);
  }

  hierarchy.cellSpacingMetres.push_back(context.cellSpacingMetres);

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  if (!buildHierarchy(hierarchy, world.graph, &stageError))
  {
    return fail(error, "partition failed: " + stageError);
  }

  world.partitionSeconds = secondsSince(started);
  world.hierarchyLevels = context.hierarchyLevels;
  world.leafCells = static_cast<uint32_t>(leafNodes(world.graph).size());

  return true;
}

bool LandmassStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;
  const auto &brief = settings.brief;

  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  if (settings.grownLandmasses)
  {
    LandmassSettings landmass{};
    landmass.landFraction = settings.landFraction;
    landmass.extentMetres = brief.extentMetres;
    landmass.continents = settings.continentCount;
    landmass.cratonsPerContinent = settings.landmassCores;
    landmass.cratonSpreadFraction = settings.cratonSpread;
    landmass.raggedness = settings.coastComplexity;
    landmass.coastDetail = settings.coastDetail;
    landmass.continentSpanMetres = settings.continentSpanMetres;
    landmass.seed = brief.seed;

    if (!growLandmasses(landmass, world.graph, &stageError))
    {
      return fail(error, "landmass growth failed: " + stageError);
    }
  }
  else
  {
    RaiseSettings raise{};
    raise.landFraction = settings.landFraction;
    raise.coastComplexity = settings.coastComplexity;
    raise.coastDetail = settings.coastDetail;
    raise.landmassCores = settings.landmassCores;
    raise.extentMetres = brief.extentMetres;
    raise.seed = brief.seed;

    if (!raiseLand(world.requestedShape, raise, world.graph, &stageError))
    {
      return fail(error, "raise failed: " + stageError);
    }
  }

  world.raiseSeconds = secondsSince(started);

  return true;
}

bool CoastStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  if (!clipCellsToExtent(world.graph, context.settings.brief.extentMetres, &stageError))
  {
    return fail(error, "clipping failed: " + stageError);
  }

  world.coastline = extractCoastline(world.graph, context.cellSpacingMetres * 1e-4);
  world.coastSeconds = secondsSince(started);

  if (!world.graph.validate(&stageError))
  {
    return fail(error, "the generated graph does not validate: " + stageError);
  }

  return true;
}

bool ElevationStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;
  const auto &brief = settings.brief;

  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  ElevationSettings elevation = settings.elevation;
  elevation.extentMetres = brief.extentMetres;
  elevation.seaLevelMetres = brief.seaLevelMetres;
  elevation.reliefMetres = brief.reliefMetres;
  elevation.seed = brief.seed;

  if (!assignElevation(brief.tags, elevation, world.graph, &stageError))
  {
    return fail(error, "elevation failed: " + stageError);
  }

  // Drain once on a copy to discover the trunk channels, condition broad
  // plains and their floodplains on the real graph, then discard this probe.
  // The definitive river pass runs against the conditioned elevation, so every
  // drainage invariant remains a property of the final map.
  context.phase("Planarizing terrain and floodplains", 0.72);

  MapGraph drainageProbe = world.graph;
  RiverNetwork preliminaryRivers;

  if (!buildRivers(settings.rivers, drainageProbe, preliminaryRivers, {}, &stageError))
  {
    return fail(error, "preliminary drainage failed: " + stageError);
  }

  if (!conditionTerrain(settings.terrain, preliminaryRivers, world.graph, &world.terrainConditioning, &stageError))
  {
    return fail(error, "terrain conditioning failed: " + stageError);
  }

  world.elevationSeconds = secondsSince(started);

  return true;
}

bool FluvialStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;

  if (!settings.fluvialErosion)
  {
    return true;
  }

  context.phase(phase(), progress());

  std::string stageError;
  FluvialSettings fluvial = settings.fluvial;
  fluvial.seaLevelMetres = settings.brief.seaLevelMetres;

  if (!erodeFluvial(fluvial, settings.rivers, world.graph, &world.fluvial, &stageError))
  {
    return fail(error, "fluvial erosion failed: " + stageError);
  }

  if (!world.graph.validate(&stageError))
  {
    return fail(error, "the graph does not validate after erosion: " + stageError);
  }

  return true;
}

bool PlateauStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;
  const auto &brief = settings.brief;

  std::string stageError;

  // Flattening after the rivers makes them run uphill across the block;
  // flattening before means the drainage that follows routes water around it
  // and drops it off the brink.
  if (settings.plateaus)
  {
    context.phase(phase(), progress());

    PlateauSettings plateau = settings.plateau;
    plateau.seed = brief.seed ^ 0x9d2c5680ull;

    if (!(plateau.ladderIntervalMetres > 0.0))
    {
      const double highest = highestLandMetres(world, brief.seaLevelMetres);

      plateau.ladderIntervalMetres = settings.contourIntervalMetres > 0.0 ? settings.contourIntervalMetres : niceInterval((highest - brief.seaLevelMetres) / std::max(1u, settings.contourBandTarget));
    }

    if (!buildPlateaus(plateau, world.graph, world.plateaus, &stageError))
    {
      return fail(error, "plateau blocks failed: " + stageError);
    }
  }

  if (!world.index.build(world.graph, 0u, brief.extentMetres, &stageError))
  {
    return fail(error, "site index failed: " + stageError);
  }

  return true;
}

bool BaseClimateStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  context.phase(phase(), progress());

  std::string stageError;

  if (!buildClimate(context.climate, RiverNetwork{}, world.graph, world.climate, &stageError))
  {
    return fail(error, "climate failed: " + stageError);
  }

  return true;
}

bool RiverStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;

  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  std::vector<double> runoffScale(world.graph.nodeCount(), 1.0);

  for (size_t index = 0u; index < world.climate.cells.size(); ++index)
  {
    runoffScale[index] = world.climate.cells[index].moisture;
  }

  if (!buildRivers(settings.rivers, world.graph, world.rivers, runoffScale, &stageError))
  {
    return fail(error, "rivers failed: " + stageError);
  }

  // Basins that filled deep enough, in a climate wet enough to keep them full,
  // with something draining into them.
  world.lakesFilled = fillLakes(world.rivers, settings.lakes, runoffScale, world.graph);

  // Newly filled lakes changed the land/water boundary. Re-trace here so their
  // exact shores reach both renderers instead of appearing against a coastline
  // captured before hydrology ran.
  world.coastline = extractCoastline(world.graph, context.cellSpacingMetres * 1e-4);
  world.riverSeconds = secondsSince(started);

  if (!world.graph.validate(&stageError))
  {
    return fail(error, "the graph does not validate after the river stage: " + stageError);
  }

  return true;
}

bool StylizeStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;

  if (!settings.stylizeRanges)
  {
    return true;
  }

  context.phase(phase(), progress());

  std::string stageError;
  StylizeSettings stylize = settings.stylize;
  stylize.seaLevelMetres = settings.brief.seaLevelMetres;

  world.ridges = extractRidges(stylize, world.graph, world.rivers);
  world.peaks = extractPeaks(world.graph);

  if (!stylizeRelief(stylize, world.graph, world.plateaus, &world.stylization, &stageError))
  {
    return fail(error, "relief stylization failed: " + stageError);
  }

  if (!world.graph.validate(&stageError))
  {
    return fail(error, "the graph does not validate after stylization: " + stageError);
  }

  return true;
}

bool SurfaceStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;
  const auto &brief = settings.brief;

  context.phase(phase(), progress());

  std::string stageError;

  MapSurfaceSettings surfaceSettings{};
  surfaceSettings.sampleSpacingMetres = context.cellSpacingMetres * 0.65;

  if (!world.surface.build(world.graph, world.index, settings.blend, surfaceSettings, &stageError))
  {
    return fail(error, "continuous surface failed: " + stageError);
  }

  const double highestLand = highestLandMetres(world, brief.seaLevelMetres);

  ContourSettings contourSettings{};
  contourSettings.intervalMetres = settings.contourIntervalMetres > 0.0
                                       ? settings.contourIntervalMetres
                                       : (world.plateaus.ladderIntervalMetres > 0.0 ? world.plateaus.ladderIntervalMetres : niceInterval((highestLand - brief.seaLevelMetres) / std::max(1u, settings.contourBandTarget)));
  contourSettings.minimumElevationMetres = brief.seaLevelMetres;
  contourSettings.maximumElevationMetres = brief.seaLevelMetres + brief.reliefMetres;
  contourSettings.minimumSegmentMetres = context.cellSpacingMetres * 1.0e-5;

  if (!extractElevationContours(world.surface, contourSettings, world.contours, &stageError))
  {
    return fail(error, "contours failed: " + stageError);
  }

  return true;
}

bool FinalClimateStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  if (!buildClimate(context.climate, world.rivers, world.graph, world.climate, &stageError))
  {
    return fail(error, "climate failed: " + stageError);
  }

  world.climateSeconds = secondsSince(started);
  world.incompatibleBorders = countIncompatibleBorders(world.graph);
  world.waterfalls = static_cast<uint32_t>(world.rivers.waterfalls.size());

  return true;
}

bool RegionStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  RegionSettings regions = context.settings.regions;
  regions.seed = context.settings.brief.seed;

  if (!buildRegions(regions, world.rivers, world.graph, world.regions, &stageError))
  {
    return fail(error, "regions failed: " + stageError);
  }

  world.regionSeconds = secondsSince(started);

  if (!world.graph.validate(&stageError))
  {
    return fail(error, "the graph does not validate after the region stage: " + stageError);
  }

  return true;
}

bool SummaryStage::run(const Context &context, GeneratedWorld &world, std::string *)
{
  const auto &brief = context.settings.brief;

  // Summarise the leaves back up the ladder, so a coarse cell knows whether it
  // is land, how high it stands and what is on it - which is what makes it
  // assignable to a realm.
  aggregateToParents(world.graph);
  world.reliefMetres = brief.reliefMetres;

  // The reference generator's own archetype classifier, run against the
  // result. Reporting it beside the request is what makes "does the tag do
  // anything" answerable without squinting at the picture.
  world.borderLand = borderLandFraction(world.graph, brief.extentMetres);
  world.measuredShapeTag = measuredShape(world.graph, brief.extentMetres);

  for (const MapNode &node : world.graph.nodes())
  {
    if (node.refined())
    {
      continue;
    }

    if (node.land)
    {
      ++world.landCells;
      world.landAreaSquareMetres += cellArea(node);
    }

    if (node.type == CellType::Lake)
    {
      ++world.lakeCells;
    }

    if (node.type == CellType::Mountain)
    {
      ++world.mountainCells;
    }

    if (biomeOf(node) == Biome::Glacier)
    {
      ++world.glacierCells;
    }

    if (node.type == CellType::Cliff)
    {
      ++world.cliffCells;
    }

    if (node.type == CellType::Canyon)
    {
      ++world.canyonCells;
    }

    world.highestMetres = std::max(world.highestMetres, node.intent.elevationMetres);
    world.deepestMetres = std::min(world.deepestMetres, node.intent.elevationMetres);
  }

  return true;
}

const std::vector<StagePtr> &worldMapStages()
{
  static const std::vector<StagePtr> stages = {
    std::make_shared<PartitionStage>(),
    std::make_shared<LandmassStage>(),
    std::make_shared<CoastStage>(),
    std::make_shared<ElevationStage>(),
    std::make_shared<FluvialStage>(),
    std::make_shared<PlateauStage>(),
    std::make_shared<BaseClimateStage>(),
    std::make_shared<RiverStage>(),
    std::make_shared<StylizeStage>(),
    std::make_shared<SurfaceStage>(),
    std::make_shared<FinalClimateStage>(),
    std::make_shared<RegionStage>(),
    std::make_shared<SummaryStage>(),
  };

  return stages;
}

} // namespace procedural::world::stages
