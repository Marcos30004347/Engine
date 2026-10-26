#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapRivers.hpp"

#include <string>
#include <vector>

namespace procedural::world
{

struct RegionSettings
{
  // Target area of one region, in square metres. A region is the unit a map
  // labels, so this is really "how big is a nameable place". 2,500 km^2 is
  // about a small county; at 900 km^2 a 130,000 km^2 island wanted 29 labels
  // and the render was a wall of text.
  double targetAreaSquareMetres = 2500.0e6;

  // How strongly a range or a river resists being crossed when regions grow.
  // Real regions are bounded by watersheds and ridgelines, and setting these to
  // zero produces regions that straddle a mountain range, which no map does.
  double ridgeResistance = 3.0;
  double riverResistance = 2.0;

  // Prefer consuming siblings inside one hierarchy parent before crossing into
  // the next. This lets the larger partition guide regional silhouettes while
  // the leaf graph still follows rivers, ridges and coastlines. Zero ignores
  // hierarchy; one is the strongest supported grouping.
  double hierarchyCohesion = 0.45;

  // Character tags bias content selection; this is how far they can move it.
  double tagBias = 0.35;

  // Spread between covers a region suits about equally, as a fraction either
  // way. Not small, and deliberately so: with no climate model there is no
  // physical basis for forest against desert, so in a neutral brief this is
  // what distributes them. Terrain suitability still gates out what does not
  // fit, and the tags are what make the choice not a draw.
  double contentJitter = 0.30;

  // A river is named when it carries at least this share of the map's largest
  // discharge. Every mouth getting a name put fifty river labels on one island
  // and made the map unreadable; real maps name the rivers you would navigate.
  double riverNamingFraction = 0.15;

  uint64_t seed = 0u;
};

struct MapRegion
{
  uint32_t id = kInvalidNode;
  std::vector<uint32_t> cells;
  uint32_t seedCell = kInvalidNode;

  // Area-weighted, so a label lands in the bulk of a region rather than being
  // dragged by a tail of small border cells.
  Point2 centroid;
  double areaSquareMetres = 0.0;

  CellType content = CellType::Plain;
  Biome biome = Biome::Grassland;
  std::string name;

  double meanElevationMetres = 0.0;
  double reliefMetres = 0.0;
  double meanUpliftRate = 0.0;
  double coastalFraction = 0.0;
  double riverFraction = 0.0;
  bool water = false;
};

struct NamedRiver
{
  std::string name;
  // Cells of the main stem, mouth first.
  std::vector<uint32_t> stem;
  double dischargeCumecs = 0.0;
  uint32_t strahlerOrder = 0u;
};

struct RegionMap
{
  std::vector<MapRegion> regions;
  std::vector<NamedRiver> rivers;
  // Region id per cell, indexed by node id. kInvalidNode where a cell belongs
  // to no region.
  std::vector<uint32_t> cellRegion;
};

// Stage 7 and 9: group cells into named regions and give each a content type.
//
// Regions grow from spread seeds by least cost over the neighbour graph, where
// crossing a ridge or a river costs extra. That makes the boundaries fall where
// a map's boundaries fall - along watersheds and ranges - instead of wherever a
// flood front happened to meet.
//
// Content is the **dominant biome** of the region's cells, by area.
//
// It used to be scored here from relief, uplift and drainage with a tag bias
// and a jitter. That was a second, worse answer to a question the climate stage
// already answers properly: the two disagreed on the same ground - a region
// named "the Fubrik Desert" sitting on green forest - and the scoring collapsed
// to a single cover whenever the terrain changed under it. The climate fields
// are continuous and physical; this just reads them.
//
// Requires the climate stage to have run first.
//
// Writes `region` and `content` on every cell, so the graph carries the result
// and the render does not need the RegionMap to colour anything.
bool buildRegions(
    const RegionSettings &settings,
    const RiverNetwork &rivers,
    MapGraph &graph,
    RegionMap &regions,
    std::string *error = nullptr);

// Toponymy. Deterministic in (seed, salt) and nothing else, so a region keeps
// its name when an unrelated part of the world changes.
std::string placeName(uint64_t seed, uint64_t salt);

// Dresses a bare name with a generic appropriate to what is being named -
// "the Kelran Hills", "Lake Voreth", "the Ashmere Marshes".
std::string namedPlace(CellType content, uint64_t seed, uint64_t salt);
std::string namedRiver(uint64_t seed, uint64_t salt);

} // namespace procedural::world
