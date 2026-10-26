#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace procedural
{

// Offline polygon-map generator based on the Red Blob Games pipeline:
// Poisson sites, Delaunay/Voronoi dual graphs, tectonic/continent/ocean
// classification, coast-distance elevation, downhill rivers, lightweight
// climate simulation, and biome-style regions.
//
// It intentionally generates one finite map at a time. Texture tiling and
// runtime chunk streaming consume its raster output later; they do not change
// the map graph or regenerate it per chunk.
struct PolygonTerrainSettings
{
  double widthMetres = 1024.0;
  double heightMetres = 1024.0;
  double siteRadiusMetres = 18.0;
  uint64_t seed = 0x7f4a7c159e3779b9ull;
  // 0 means automatic: choose a deterministic headwater density from the
  // configured world scale, graph spacing, and available highland candidates.
  uint32_t riverSourceCount = 0u;
  // Optional ocean collar, as a fraction of the shorter map side. Applied
  // before drainage and climate so coastlines/rivers agree with the land mask.
  double oceanMarginFraction = 0.0;
  // Smooth reconstruction independent of the nearest region's neighbour list.
  bool continuousElevation = false;
};

struct PolygonTerrainRasterSettings
{
  uint32_t width = 1024u;
  uint32_t height = 1024u;
  // Raster workers used while baking maps. The default favours large offline
  // previews; 0 means auto and currently resolves to the same 64-worker target.
  uint32_t workerCount = 64u;
};

// Values written to PolygonTerrainMaps::materials and the runtime terrain
// package. Keep these compact and stable: voxel terrain stores this index on
// its surface cell instead of carrying the generator's graph attributes.
enum class PolygonTerrainMaterial : uint8_t
{
  Ocean = 0u,
  Lake = 1u,
  Beach = 2u,
  Grass = 3u,
  Rock = 4u,
  Snow = 5u,
  River = 6u,
  Marsh = 7u,
  Ice = 8u,
  Tundra = 9u,
  Bare = 10u,
  Scorched = 11u,
  Taiga = 12u,
  Shrubland = 13u,
  TemperateDesert = 14u,
  TemperateRainForest = 15u,
  TemperateDeciduousForest = 16u,
  Grassland = 17u,
  TropicalRainForest = 18u,
  TropicalSeasonalForest = 19u,
  SubtropicalDesert = 20u,
  // City ground, written by CityGroundOverlay over the world map's own
  // material. Paving first, then the unpaved yard and verge surfaces.
  CityCobble = 21u,
  CityFlagstone = 22u,
  CityPackedEarth = 23u,
  CityGravel = 24u,
  CityCutStone = 25u,
};

struct PolygonTerrainStatistics
{
  uint32_t siteCount = 0u;
  uint32_t triangleCount = 0u;
  uint32_t voronoiCornerCount = 0u;
  uint32_t riverEdgeCount = 0u;
  uint32_t riverSourceCount = 0u;
  // Optional ocean collar, as a fraction of the shorter map side. Applied
  // before drainage and climate so coastlines/rivers agree with the land mask.
  double oceanMarginFraction = 0.0;
  // Smooth reconstruction independent of the nearest region's neighbour list.
  bool continuousElevation = false;
  uint32_t oceanSiteCount = 0u;
  uint32_t lakeSiteCount = 0u;
};

struct PolygonTerrainProgress
{
  // Called from the generator as work advances. The raster phase may call this
  // from worker threads; implementations should be cheap and thread-safe.
  // Return false to request cancellation.
  std::function<bool(const char *phase, double progress)> callback;
};

// All maps have rasterSettings.width * rasterSettings.height elements.
//
// height: normalized land elevation, with zero reserved for water.
// rivers: zero for no river; larger values represent merged downstream flow.
// regions: deterministic colour/index value for the nearest Delaunay site.
// sourceRegions: stable 32-bit Delaunay/Voronoi site identifier used by
// runtime planet patches and streamed chunks. This is data, not a colour map.
// materials: PolygonTerrainMaterial for each raster cell.
// terrainForms: debug terrain-shape class:
//   0 water, 1 plains, 2 valleys, 3 hills, 4 mountains, 5 peaks.
// temperature, precipitation, wind: debug climate fields in normalized 0..255
// texture form.
// forests: forest growth suitability/coverage in normalized 0..255 form.
// settlements: suitability for towns/cities: flat, temperate, near water, away
// from steep mountains/wetlands/deserts.
// continents: coarse hierarchical continent/ocean field.
// tectonics: coarse uplift / mountain-boundary influence.
// These are preview/offline diagnostics, not runtime voxel package channels.
// Directed graph edges retained for geometry consumers. Coordinates are in
// source world metres; flow counts merged headwaters, not raster pixels.
struct PolygonTerrainRiverEdge
{
  uint32_t from = 0u;
  uint32_t to = 0u;
  double fromX = 0.0, fromY = 0.0, toX = 0.0, toY = 0.0;
  uint32_t flow = 0u;
};

struct PolygonTerrainRegionEdge
{
  uint32_t left = 0u, right = 0u;
  double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
};

struct PolygonTerrainMaps
{
  std::vector<PolygonTerrainRegionEdge> regionEdges;
  std::vector<PolygonTerrainRiverEdge> riverEdges;
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<uint16_t> heightmap;
  std::vector<uint32_t> sourceRegions;
  std::vector<uint8_t> rivers;
  std::vector<uint8_t> regions;
  std::vector<uint8_t> materials;
  std::vector<uint8_t> terrainForms;
  std::vector<uint8_t> temperature;
  std::vector<uint8_t> precipitation;
  std::vector<uint8_t> wind;
  std::vector<uint8_t> forests;
  std::vector<uint8_t> settlements;
  std::vector<uint8_t> continents;
  std::vector<uint8_t> tectonics;
};

class PolygonTerrainGenerator
{
public:
  bool generate(
      const PolygonTerrainSettings &settings, const PolygonTerrainRasterSettings &rasterSettings,
      PolygonTerrainMaps &maps, PolygonTerrainStatistics *statistics = nullptr,
      std::string *error = nullptr, const PolygonTerrainProgress *progress = nullptr);
};

} // namespace procedural
