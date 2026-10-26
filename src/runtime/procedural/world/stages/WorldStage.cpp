#include "procedural/world/WorldKingdomHistory.hpp"
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

// A deterministic draw in [0, 1) addressed by purpose rather than by call
// order, so a value does not change when an unrelated loop is reordered.
double hashUnit(const uint64_t seed, const int64_t x, const int64_t y)
{
  const auto mix = [](uint64_t value) {
    value ^= value >> 33u;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33u;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33u;
    return value;
  };

  uint64_t value = mix(seed ^ static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ull);
  value = mix(value ^ static_cast<uint64_t>(y) * 0xbf58476d1ce4e5b9ull);

  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

// Smoothstep, clamped.
double smoothUnit(const double t)
{
  const double c = std::clamp(t, 0.0, 1.0);
  return c * c * (3.0 - 2.0 * c);
}

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

  // Maritime moisture reaches 240 km inland by default, which is a fact about
  // the real atmosphere and useless on a 160 km map: every cell is within reach
  // of the sea, moisture saturates, and the Whittaker classifier answers
  // "temperate rainforest" for half the land. Seed 7 came out with four biomes
  // on fifteen hundred cells.
  //
  // The reach has to be a fraction of the map instead, so the middle of a
  // continent is genuinely continental. At a third of the extent a 110 km
  // continent has a maritime rim and a dry interior, which is what puts
  // grassland and steppe between the coastal forest and the ranges.
  context.climate.coastalMoistureRangeMetres = std::min(context.climate.coastalMoistureRangeMetres, settings.brief.extentMetres * 0.35);

  // Likewise the latitude window. Nine degrees of latitude is one climate band,
  // so every world got the same band and the only variation left was relief.
  // A map is a window onto a globe and we choose where to put it: a wider
  // window crosses the subtropical dry belt and the temperate zone, so one
  // world has desert against savanna and the next has taiga above tundra.
  context.climate.latitudeSpanDegrees = std::max(context.climate.latitudeSpanDegrees, 22.0);

  // Where that window sits on the globe. Four overlapping placements, each
  // crossing at least two climate bands so a world has a gradient rather than
  // one colour: equatorial reaches from rainforest up into the dry belt,
  // subtropical straddles the dry belt itself and so is the one that grows real
  // deserts against savanna, temperate is the familiar forest-and-grassland
  // middle, and boreal runs taiga up into tundra and ice. The boreal edge sits at 44 rather
    // than 48 because at 48 the window was 45 per cent glacier and tundra, which
    // is an ice cap rather than a world to put a settlement on.
  if (context.climate.placeWindowFromSeed)
  {
    static constexpr double kBands[] = {2.0, 16.0, 34.0, 44.0};

    uint64_t pick = settings.brief.seed * 0x9e3779b97f4a7c15ull;
    pick ^= pick >> 29u;
    pick *= 0xbf58476d1ce4e5b9ull;
    pick ^= pick >> 32u;

    context.climate.southLatitudeDegrees = kBands[pick % std::size(kBands)];
  }
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

  // Two geology constants are world-scale and have to be brought down to the
  // size of the map being drawn, or a small world gets neither.
  //
  // The plate lattice spacing decides where mountain ranges go: a range
  // follows a seam between plates, and only when no seam crosses the landmass
  // does the elevation stage fall back to anchoring one range at the most
  // inland cell. At the 220 km default a 160 km map sits inside a single plate,
  // so the seam path never fires and *every* world gets one range in the middle
  // of its continent. Requiring at least a couple of plate widths across the
  // map is what puts ranges somewhere other than the centre.
  elevation.orogenSpacingMetres = std::min(elevation.orogenSpacingMetres, brief.extentMetres / 2.5);

  // Every height term is gated by the coastal ramp, so the plain width decides
  // whether a continent ever reaches its full relief. It is a distance measured
  // *inland*, which makes it a fact about the landmass rather than about the
  // map: sized against the whole map, a lobed continent whose arms are 40 km
  // across never gets more than 30 km from a shore, the ramp never closes, and
  // the world tops out at half the relief the brief asked for with no cell
  // classified as mountain. Sizing it against the continent instead lets a lobe
  // reach full height in its own middle while the shore still rises smoothly.
  const double continentReach = settings.continentSpanMetres > 0.0 ? settings.continentSpanMetres : brief.extentMetres;

  elevation.coastalPlainMetres = std::min(elevation.coastalPlainMetres, continentReach / 8.0);

  // A range is only as wide as the landmass can show. Left at a world-scale
  // width it smears across a small continent instead of reading as a range.
  elevation.rangeWidthMetres = std::min(elevation.rangeWidthMetres, continentReach / 8.0);

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

bool BasinStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;

  context.phase(phase(), progress());

  if (!(settings.lakeBasinsPerThousandCells > 0.0))
  {
    return true;
  }

  // Eligible ground: inland, not already water, and not on a range flank. A
  // basin is a floor between hills, so the sites are drawn from the flatter,
  // lower half of the interior rather than from anywhere on land.
  std::vector<uint32_t> interior;
  double highest = 0.0;

  for (const MapNode &node : world.graph.nodes())
  {
    if (node.refined() || !node.land || node.type == CellType::Lake)
    {
      continue;
    }
    highest = std::max(highest, node.intent.elevationMetres);
  }

  for (const MapNode &node : world.graph.nodes())
  {
    if (node.refined() || !node.land || node.type == CellType::Lake || node.type == CellType::Coast)
    {
      continue;
    }
    // Wet enough to keep a lake full, and low enough to be a floor. Both gates
    // are deliberately loose: the lake pass applies the real ones afterwards,
    // and a basin that stays dry is a salt flat, which is also a thing a world
    // should have.
    const double moisture = node.id < world.climate.cells.size() ? world.climate.cells[node.id].moisture : 1.0;
    if (moisture < 0.18 || node.intent.elevationMetres > highest * 0.55)
    {
      continue;
    }
    interior.push_back(node.id);
  }

  if (interior.size() < 8u)
  {
    return true;
  }

  const size_t wanted = static_cast<size_t>(std::llround(static_cast<double>(interior.size()) * settings.lakeBasinsPerThousandCells / 1000.0));
  if (wanted == 0u)
  {
    return true;
  }

  // Basin centres, spread apart so two do not merge into one sheet of water.
  const double spacing = settings.brief.extentMetres / std::sqrt(static_cast<double>(wanted) + 1.0) * 0.45;
  std::vector<uint32_t> centres;

  for (const uint32_t id : interior)
  {
    if (centres.size() >= wanted)
    {
      break;
    }
    const Point2 &site = world.graph.node(id).site;
    bool clear = true;
    for (const uint32_t other : centres)
    {
      const Point2 &taken = world.graph.node(other).site;
      if (std::hypot(site.x - taken.x, site.y - taken.y) < spacing)
      {
        clear = false;
        break;
      }
    }
    // A purpose-addressed draw, so the centres are not simply the first
    // eligible cells in node order.
    if (clear && hashUnit(settings.brief.seed ^ 0x1ac3b5ull, static_cast<int64_t>(id), 0) < 0.55)
    {
      centres.push_back(id);
    }
  }

  // Sink each basin below its own rim. The floor is dropped to the lowest cell
  // in the disc less the basin depth, and the cells between floor and rim are
  // interpolated, so the result is a bowl rather than a cylinder with a step.
  uint32_t carved = 0u;

  for (const uint32_t centre : centres)
  {
    const Point2 &middle = world.graph.node(centre).site;
    const double radius = spacing * (0.08 + 0.14 * hashUnit(settings.brief.seed ^ 0x51d0b7ull, static_cast<int64_t>(centre), 1));

    std::vector<uint32_t> disc;
    double rim = -1.0e30;
    double floorHeight = 1.0e30;

    for (const uint32_t id : interior)
    {
      const Point2 &site = world.graph.node(id).site;
      if (std::hypot(site.x - middle.x, site.y - middle.y) <= radius)
      {
        disc.push_back(id);
        rim = std::max(rim, world.graph.node(id).intent.elevationMetres);
        floorHeight = std::min(floorHeight, world.graph.node(id).intent.elevationMetres);
      }
    }

    if (disc.size() < 4u)
    {
      continue;
    }

    // The floor has to sit below the *lowest* rim the disc has, not below its
    // own starting height. On sloping ground those are different numbers: a
    // fixed drop from the floor leaves the downhill rim lower than the floor,
    // the water runs straight out, and the basin never holds. Carving eight
    // basins that way still left a 99th-percentile depression depth of zero.
    //
    // So the drop clears the local relief across the disc and then adds a
    // freeboard, which is what actually makes the bowl closed.
    const double freeboard = std::max(settings.lakes.minimumDepthMetres * 1.8, settings.brief.reliefMetres * 0.015);
    // Capped, because clearing the full local relief on a steep site cuts a
    // gorge rather than a lake bed - and a gorge that deep captures the whole
    // drainage network above it, which took one world from 135 river segments
    // to 15.
    const double clearance = std::min(rim - floorHeight, settings.brief.reliefMetres * 0.05);
    const double target = floorHeight - (clearance + freeboard);

    for (const uint32_t id : disc)
    {
      MapNode &node = world.graph.node(id);
      const Point2 &site = node.site;
      const double reach = std::hypot(site.x - middle.x, site.y - middle.y) / std::max(1.0, radius);
      // Smooth at the rim, full depth at the centre, so the basin joins the
      // ground it sits in instead of cutting a cliff around itself.
      const double sink = 1.0 - smoothUnit(reach);
      node.intent.elevationMetres = node.intent.elevationMetres + (target - node.intent.elevationMetres) * sink;
    }

    ++carved;
  }

  world.lakeBasinsCarved = carved;

  return true;
}

bool RiverStage::run(const Context &context, GeneratedWorld &world, std::string *error)
{
  const auto &settings = context.settings;

  context.phase(phase(), progress());

  const auto started = std::chrono::steady_clock::now();
  std::string stageError;

  // Runoff carries the wet-against-dry contrast, normalised so its mean over
  // land is one.
  //
  // Fed the raw moisture it carried the absolute level too, which makes the
  // drainage density depend on the climate model's calibration rather than on
  // the terrain: correcting the maritime reach halved the mean moisture and
  // took a world from 135 river segments to 13 without a single line of the
  // river code changing. Normalising means a wetter world has the same channel
  // density but a drier interior relative to its coast, which is the part that
  // was meant to be expressed.
  std::vector<double> runoffScale(world.graph.nodeCount(), 1.0);

  double total = 0.0;
  size_t counted = 0u;

  for (const MapNode &node : world.graph.nodes())
  {
    if (!node.refined() && node.land && node.id < world.climate.cells.size())
    {
      total += world.climate.cells[node.id].moisture;
      ++counted;
    }
  }

  const double mean = counted == 0u ? 0.0 : total / static_cast<double>(counted);

  if (mean > 0.0)
  {
    for (size_t index = 0u; index < world.climate.cells.size(); ++index)
    {
      runoffScale[index] = world.climate.cells[index].moisture / mean;
    }
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

bool SummaryStage::run(const Context &context, GeneratedWorld &world, std::string *error)
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

  if(context.settings.historyYears)context.phase("Kingdom history",.98);
  return rewrite::populateWorldHistory(world,context.settings,context.callbacks.cancelled,error,[&](uint32_t year) {
    const std::string name = "Kingdom history: year " + std::to_string(year) + "/" + std::to_string(context.settings.historyYears);
    context.phase(name.c_str(), .98 + .019 * double(year) / context.settings.historyYears);
  });
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
    std::make_shared<BasinStage>(),
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
