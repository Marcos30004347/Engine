// Frozen pre-refactor algorithm for migration parity; never linked into Engine.
#include "procedural/world/WorldPipeline.hpp"

#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/MapLandmass.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/Raisers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace procedural::world
{

namespace
{

// Rounds up to the next 1, 2, 2.5 or 5 times a power of ten. Contour intervals
// are read off a map by eye, so they have to be numbers a person would choose.
double niceInterval(const double raw)
{
  if (!(raw > 0.0) || !std::isfinite(raw))
    return 100.0;
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  const double normalised = raw / magnitude;
  double step = 10.0;
  for (const double candidate : {1.0, 2.0, 2.5, 5.0})
    if (normalised <= candidate)
    {
      step = candidate;
      break;
    }
  return std::max(0.5, step * magnitude);
}

double secondsSince(const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

} // namespace

bool generateWorldBeforeDirectorRefactor(
    const WorldPipelineSettings &settings,
    GeneratedWorld &world,
    const WorldPipelineCallbacks &callbacks,
    std::string *error)
{
  const auto started = std::chrono::steady_clock::now();
  const WorldBrief &brief = settings.brief;
  const double cellSpacingMetres = settings.cellSpacingMetres;
  const uint32_t hierarchyLevels = settings.hierarchyLevels;

  GeneratedWorld result{};
  result.extentMetres = brief.extentMetres;
  result.requestedShape = shapeTag(brief.tags);
  std::string stageError;

  const auto setPhase = [&callbacks](const char *phase, const double progress)
  {
    if (callbacks.setPhase)
      callbacks.setPhase(phase, progress);
  };
  const auto cancelled = [&callbacks]
  { return callbacks.cancelled ? callbacks.cancelled() : false; };
  const auto fail = [&error, &started, &result](const std::string &message)
  {
    if (error)
      *error = message;
    result.totalSeconds = secondsSince(started);
    return false;
  };

  setPhase("Partitioning", 0.05);
  result.graph.setBrief(brief);
  // A ladder of Voronoi partitions, coarsest first, each cut inside the one
  // above it. The finest is what every later stage measures; the coarser ones
  // are the units a realm or a province is drawn on.
  HierarchySettings hierarchy{};
  hierarchy.extentMetres = brief.extentMetres;
  hierarchy.seed = brief.seed;
  hierarchy.cellSpacingMetres.clear();
  const double coarsestSpacing = cellSpacingMetres *
      std::pow(4.0, static_cast<double>(hierarchyLevels - 1u));
  const double coarseSitesPerAxis = brief.extentMetres / coarsestSpacing;
  if (!(brief.extentMetres > 0.0) || !std::isfinite(brief.extentMetres) ||
      !(coarsestSpacing > 0.0) || !std::isfinite(coarsestSpacing) ||
      !std::isfinite(coarseSitesPerAxis) || coarseSitesPerAxis < 3.0)
  {
    std::ostringstream message;
    message << "hierarchy needs at least 3 coarse sites per axis; extent "
            << brief.extentMetres * 1.0e-3 << " km, spacing "
            << cellSpacingMetres * 1.0e-3 << " km and " << hierarchyLevels
            << " levels provide " << coarseSitesPerAxis;
    return fail(message.str());
  }
  for (double spacing = coarsestSpacing;
       hierarchy.cellSpacingMetres.size() + 1u < hierarchyLevels;
       spacing /= 4.0)
    hierarchy.cellSpacingMetres.push_back(spacing);
  hierarchy.cellSpacingMetres.push_back(cellSpacingMetres);
  auto stageStart = std::chrono::steady_clock::now();
  if (!buildHierarchy(hierarchy, result.graph, &stageError))
    return fail("partition failed: " + stageError);
  result.partitionSeconds = secondsSince(stageStart);
  result.hierarchyLevels = hierarchyLevels;
  result.leafCells = static_cast<uint32_t>(leafNodes(result.graph).size());
  if (cancelled())
    return fail("cancelled");

  setPhase("Raising land", 0.45);
  stageStart = std::chrono::steady_clock::now();
  if (settings.grownLandmasses)
  {
    // Grown over the coarse partition rather than cut from an analytic field.
    // The archetypes cannot make a shape that is not convex; growing can.
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
    if (!growLandmasses(landmass, result.graph, &stageError))
      return fail("landmass growth failed: " + stageError);
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
    if (!raiseLand(result.requestedShape, raise, result.graph, &stageError))
      return fail("raise failed: " + stageError);
  }
  result.raiseSeconds = secondsSince(stageStart);
  if (cancelled())
    return fail("cancelled");

  setPhase("Clipping and tracing the coast", 0.6);
  stageStart = std::chrono::steady_clock::now();
  if (!clipCellsToExtent(result.graph, brief.extentMetres, &stageError))
    return fail("clipping failed: " + stageError);
  result.coastline = extractCoastline(result.graph, cellSpacingMetres * 1e-4);
  result.coastSeconds = secondsSince(stageStart);
  if (!result.graph.validate(&stageError))
    return fail("the generated graph does not validate: " + stageError);
  if (cancelled())
    return fail("cancelled");

  setPhase("Elevation and uplift", 0.7);
  stageStart = std::chrono::steady_clock::now();
  ElevationSettings elevation = settings.elevation;
  elevation.extentMetres = brief.extentMetres;
  elevation.seaLevelMetres = brief.seaLevelMetres;
  elevation.reliefMetres = brief.reliefMetres;
  elevation.seed = brief.seed;
  if (!assignElevation(brief.tags, elevation, result.graph, &stageError))
    return fail("elevation failed: " + stageError);

  // Drain once on a copy to discover the trunk channels, condition broad
  // plains and their floodplains on the real graph, then discard this probe.
  // The definitive river pass below runs against the conditioned elevation, so
  // every drainage invariant remains a property of the final map.
  setPhase("Planarizing terrain and floodplains", 0.72);
  MapGraph drainageProbe = result.graph;
  RiverNetwork preliminaryRivers;
  if (!buildRivers(
          settings.rivers, drainageProbe, preliminaryRivers, {}, &stageError))
    return fail("preliminary drainage failed: " + stageError);
  if (!conditionTerrain(settings.terrain, preliminaryRivers, result.graph,
          &result.terrainConditioning, &stageError))
    return fail("terrain conditioning failed: " + stageError);
  result.elevationSeconds = secondsSince(stageStart);

  // Stage 5A: fluvial relief. The planet generator's split, brought to the
  // map - the graph owns tectonics and climate, and a stream-power solver owns
  // the valleys and the ranges. Runs against the uplift the elevation stage
  // authored, so what is left standing is what uplift defended.
  if (settings.fluvialErosion)
  {
    setPhase("Fluvial relief", 0.725);
    FluvialSettings fluvial = settings.fluvial;
    fluvial.seaLevelMetres = brief.seaLevelMetres;
    if (!erodeFluvial(fluvial, settings.rivers, result.graph, &result.fluvial,
            &stageError))
      return fail("fluvial erosion failed: " + stageError);
    if (!result.graph.validate(&stageError))
      return fail("the graph does not validate after erosion: " + stageError);
  }

  // Stage 5B: plateau blocks, before the definitive drainage pass. Flattening
  // after the rivers makes them run uphill across the block; flattening before
  // means the drainage that follows routes water around it and drops it off the
  // brink.
  if (settings.plateaus)
  {
    setPhase("Plateau blocks", 0.73);
    PlateauSettings plateau = settings.plateau;
    plateau.seed = brief.seed ^ 0x9d2c5680ull;
    if (!(plateau.ladderIntervalMetres > 0.0))
    {
      // The ladder is the contour ladder. Derived the same way, from the relief
      // that exists rather than the relief the brief asked for.
      double highest = brief.seaLevelMetres;
      for (const MapNode &node : result.graph.nodes())
        if (!node.refined() && node.land)
          highest = std::max(highest, node.intent.elevationMetres);
      plateau.ladderIntervalMetres = settings.contourIntervalMetres > 0.0
          ? settings.contourIntervalMetres
          : niceInterval((highest - brief.seaLevelMetres) /
                std::max(1u, settings.contourBandTarget));
    }
    if (!buildPlateaus(plateau, result.graph, result.plateaus, &stageError))
      return fail("plateau blocks failed: " + stageError);
  }

  if (!result.index.build(result.graph, 0u, brief.extentMetres, &stageError))
    return fail("site index failed: " + stageError);
  if (cancelled())
    return fail("cancelled");

  // Climate first, because precipitation is what drives the rivers: a desert
  // basin must not grow the same river a wet one does. The riparian moisture
  // that runs the other way is a second, local pass below.
  setPhase("Climate", 0.74);
  stageStart = std::chrono::steady_clock::now();
  ClimateSettings climate = settings.climate;
  climate.extentMetres = brief.extentMetres;
  climate.seed = brief.seed;
  if (!buildClimate(climate, RiverNetwork{}, result.graph, result.climate, &stageError))
    return fail("climate failed: " + stageError);
  if (cancelled())
    return fail("cancelled");

  setPhase("Rivers", 0.8);
  stageStart = std::chrono::steady_clock::now();
  std::vector<double> runoffScale(result.graph.nodeCount(), 1.0);
  for (size_t index = 0u; index < result.climate.cells.size(); ++index)
    runoffScale[index] = result.climate.cells[index].moisture;
  if (!buildRivers(settings.rivers, result.graph, result.rivers, runoffScale, &stageError))
    return fail("rivers failed: " + stageError);
  // Basins that filled deep enough, in a climate wet enough to keep them full,
  // with something draining into them.
  result.lakesFilled = fillLakes(result.rivers, settings.lakes, runoffScale, result.graph);
  // Newly filled lakes changed the land/water boundary. Re-trace here so their
  // exact shores reach both renderers instead of appearing against a coastline
  // captured before hydrology ran.
  result.coastline = extractCoastline(result.graph, cellSpacingMetres * 1e-4);
  result.riverSeconds = secondsSince(stageStart);
  if (!result.graph.validate(&stageError))
    return fail("the graph does not validate after the river stage: " + stageError);
  if (cancelled())
    return fail("cancelled");

  // Stage 9B/9C: ridges, then stylization - both after drainage, and both
  // before the surface is published so everything derived from it describes the
  // stylized world.
  if (settings.stylizeRanges)
  {
    setPhase("Ridges and relief stylization", 0.82);
    StylizeSettings stylize = settings.stylize;
    stylize.seaLevelMetres = brief.seaLevelMetres;
    result.ridges = extractRidges(stylize, result.graph, result.rivers);
    result.peaks = extractPeaks(result.graph);
    if (!stylizeRelief(stylize, result.graph, result.plateaus,
            &result.stylization, &stageError))
      return fail("relief stylization failed: " + stageError);
    if (!result.graph.validate(&stageError))
      return fail("the graph does not validate after stylization: " + stageError);
  }

  // Lakes may replace a basin floor with its spill elevation. Publish the
  // continuous field only after that last scalar mutation, then derive the
  // contours from it so all downstream consumers see one final surface.
  setPhase("Continuous elevation, uplift and contours", 0.83);
  MapSurfaceSettings surfaceSettings{};
  surfaceSettings.sampleSpacingMetres = cellSpacingMetres * 0.65;
  if (!result.surface.build(result.graph, result.index, settings.blend,
          surfaceSettings, &stageError))
    return fail("continuous surface failed: " + stageError);
  // Measure the relief that actually exists before choosing an interval. The
  // brief's relief is an upper bound the elevation stage may not reach.
  double highestLand = brief.seaLevelMetres;
  for (const MapNode &node : result.graph.nodes())
    if (!node.refined() && node.land)
      highestLand = std::max(highestLand, node.intent.elevationMetres);

  ContourSettings contourSettings{};
  contourSettings.intervalMetres = settings.contourIntervalMetres > 0.0
      ? settings.contourIntervalMetres
      : (result.plateaus.ladderIntervalMetres > 0.0
                ? result.plateaus.ladderIntervalMetres
                : niceInterval((highestLand - brief.seaLevelMetres) /
                      std::max(1u, settings.contourBandTarget)));
  contourSettings.minimumElevationMetres = brief.seaLevelMetres;
  contourSettings.maximumElevationMetres =
      brief.seaLevelMetres + brief.reliefMetres;
  contourSettings.minimumSegmentMetres = cellSpacingMetres * 1.0e-5;
  if (!extractElevationContours(
          result.surface, contourSettings, result.contours, &stageError))
    return fail("contours failed: " + stageError);

  // Second climate pass, now that the rivers and lakes exist: riparian strips
  // and lake shores are local moisture the first pass could not know about.
  setPhase("Climate and land types", 0.84);
  stageStart = std::chrono::steady_clock::now();
  if (!buildClimate(climate, result.rivers, result.graph, result.climate, &stageError))
    return fail("climate failed: " + stageError);
  result.climateSeconds = secondsSince(stageStart);
  result.incompatibleBorders = countIncompatibleBorders(result.graph);
  result.waterfalls = static_cast<uint32_t>(result.rivers.waterfalls.size());
  if (cancelled())
    return fail("cancelled");

  setPhase("Regions and names", 0.86);
  stageStart = std::chrono::steady_clock::now();
  RegionSettings regions = settings.regions;
  regions.seed = brief.seed;
  if (!buildRegions(regions, result.rivers, result.graph, result.regions, &stageError))
    return fail("regions failed: " + stageError);
  result.regionSeconds = secondsSince(stageStart);
  if (!result.graph.validate(&stageError))
    return fail("the graph does not validate after the region stage: " + stageError);
  if (cancelled())
    return fail("cancelled");

  // Summarise the leaves back up the ladder, so a coarse cell knows whether it
  // is land, how high it stands and what is on it - which is what makes it
  // assignable to a realm.
  aggregateToParents(result.graph);
  result.reliefMetres = brief.reliefMetres;

  // The reference generator's own archetype classifier, run against the result.
  // Reporting it beside the request is what makes "does the tag do anything"
  // answerable without squinting at the picture.
  result.borderLand = borderLandFraction(result.graph, brief.extentMetres);
  result.measuredShapeTag = measuredShape(result.graph, brief.extentMetres);
  for (const MapNode &node : result.graph.nodes())
  {
    if (node.refined())
      continue;
    if (node.land)
    {
      ++result.landCells;
      result.landAreaSquareMetres += cellArea(node);
    }
    if (node.type == CellType::Lake)
      ++result.lakeCells;
    if (node.type == CellType::Mountain)
      ++result.mountainCells;
    if (biomeOf(node) == Biome::Glacier)
      ++result.glacierCells;
    if (node.type == CellType::Cliff)
      ++result.cliffCells;
    if (node.type == CellType::Canyon)
      ++result.canyonCells;
    result.highestMetres = std::max(result.highestMetres, node.intent.elevationMetres);
    result.deepestMetres = std::min(result.deepestMetres, node.intent.elevationMetres);
  }

  setPhase("Done", 1.0);
  result.totalSeconds = secondsSince(started);
  world = std::move(result);
  return true;
}

} // namespace procedural::world
