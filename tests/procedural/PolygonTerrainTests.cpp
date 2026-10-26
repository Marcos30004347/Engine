#include "procedural/PolygonTerrain.hpp"
#include "procedural/TerrainPackage.hpp"

#include "lz4.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  if (condition)
    std::cout << "  PASSED " << message << "\n";
  else
  {
    std::cerr << "  FAILED " << message << "\n";
    ++failures;
  }
}

procedural::PolygonTerrainMaps generate(const uint64_t seed, procedural::PolygonTerrainStatistics &statistics)
{
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 256.0;
  settings.heightMetres = 256.0;
  settings.siteRadiusMetres = 18.0;
  settings.seed = seed;
  settings.riverSourceCount = 0u;
  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 192u;
  raster.height = 192u;
  procedural::PolygonTerrainMaps maps{};
  procedural::PolygonTerrainGenerator generator;
  std::string error;
  check(generator.generate(settings, raster, maps, &statistics, &error), error.empty() ? "terrain generation succeeds" : error.c_str());
  return maps;
}

uint32_t nonZeroCount(const std::vector<uint8_t> &values)
{
  return static_cast<uint32_t>(std::count_if(values.begin(), values.end(), [](const uint8_t value) { return value != 0u; }));
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path)
{
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

uint32_t readU32(const std::vector<uint8_t> &bytes, const size_t offset)
{
  return static_cast<uint32_t>(bytes[offset]) |
      (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
      (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
      (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

void testMapsHaveExpectedShape()
{
  std::cout << "polygon terrain map shape\n";
  procedural::PolygonTerrainStatistics statistics{};
  const procedural::PolygonTerrainMaps maps = generate(42u, statistics);
  check(maps.width == 192u && maps.height == 192u, "requested raster dimensions are retained");
  check(maps.heightmap.size() == 192u * 192u, "heightmap contains every raster cell");
  check(maps.rivers.size() == maps.heightmap.size(), "river map matches heightmap dimensions");
  check(maps.regions.size() == maps.heightmap.size(), "region map matches heightmap dimensions");
  check(maps.materials.size() == maps.heightmap.size(), "material map matches heightmap dimensions");
  check(maps.terrainForms.size() == maps.heightmap.size(), "terrain form map matches heightmap dimensions");
  check(maps.temperature.size() == maps.heightmap.size(), "temperature map matches heightmap dimensions");
  check(maps.precipitation.size() == maps.heightmap.size(), "precipitation map matches heightmap dimensions");
  check(maps.wind.size() == maps.heightmap.size(), "wind map matches heightmap dimensions");
  check(maps.forests.size() == maps.heightmap.size(), "forest map matches heightmap dimensions");
  check(maps.settlements.size() == maps.heightmap.size(), "city suitability map matches heightmap dimensions");
  check(maps.continents.size() == maps.heightmap.size(), "continent map matches heightmap dimensions");
  check(maps.tectonics.size() == maps.heightmap.size(), "tectonic map matches heightmap dimensions");
  check(statistics.siteCount > 20u && statistics.triangleCount > 20u, "constructed a non-trivial polygon graph");
  check(statistics.voronoiCornerCount == statistics.triangleCount, "every finite triangle creates one Voronoi corner");
}

void testMapsContainTerrainFeatures()
{
  std::cout << "polygon terrain features\n";
  procedural::PolygonTerrainStatistics statistics{};
  const procedural::PolygonTerrainMaps maps = generate(99u, statistics);
  if (maps.heightmap.empty()) return;
  const auto [minimumHeight, maximumHeight] = std::minmax_element(maps.heightmap.begin(), maps.heightmap.end());
  check(*minimumHeight == 0u, "heightmap contains ocean");
  check(*maximumHeight > 30000u, "heightmap contains high land");
  check(nonZeroCount(maps.rivers) > 0u, "river map contains rivers");
  check(nonZeroCount(maps.regions) > maps.regions.size() / 4u, "region map contains land regions");
  check(nonZeroCount(maps.terrainForms) > maps.terrainForms.size() / 4u, "terrain form map contains land forms");
  check(nonZeroCount(maps.temperature) > 0u, "temperature map contains climate values");
  check(nonZeroCount(maps.precipitation) > 0u, "precipitation map contains climate values");
  check(nonZeroCount(maps.wind) > 0u, "wind map contains wind exposure values");
  check(nonZeroCount(maps.continents) > 0u, "continent map contains macro terrain values");
  check(nonZeroCount(maps.tectonics) > 0u, "tectonic map contains uplift values");
  const bool hasLandMaterial = std::any_of(
      maps.materials.begin(), maps.materials.end(), [](const uint8_t material)
      {
        return material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Beach) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Rock) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Snow) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grassland) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::TemperateDeciduousForest) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::TemperateRainForest) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::TropicalSeasonalForest) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::TropicalRainForest) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Shrubland) ||
            material == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Taiga);
      });
  check(hasLandMaterial, "material map contains terrain materials");
  check(statistics.riverSourceCount > 0u, "automatic river source selection chose headwaters");
  check(statistics.riverEdgeCount > 0u, "river graph contains edges");
}

void testDeterminism()
{
  std::cout << "polygon terrain determinism\n";
  procedural::PolygonTerrainStatistics firstStatistics{};
  procedural::PolygonTerrainStatistics secondStatistics{};
  const procedural::PolygonTerrainMaps first = generate(777u, firstStatistics);
  const procedural::PolygonTerrainMaps second = generate(777u, secondStatistics);
  check(first.heightmap == second.heightmap, "heightmap repeats exactly for the same seed");
  check(first.rivers == second.rivers, "river map repeats exactly for the same seed");
  check(first.regions == second.regions, "region map repeats exactly for the same seed");
  check(first.materials == second.materials, "material map repeats exactly for the same seed");
  check(first.terrainForms == second.terrainForms, "terrain form map repeats exactly for the same seed");
  check(first.temperature == second.temperature, "temperature map repeats exactly for the same seed");
  check(first.precipitation == second.precipitation, "precipitation map repeats exactly for the same seed");
  check(first.wind == second.wind, "wind map repeats exactly for the same seed");
  check(first.forests == second.forests, "forest map repeats exactly for the same seed");
  check(first.settlements == second.settlements, "city suitability map repeats exactly for the same seed");
  check(first.continents == second.continents, "continent map repeats exactly for the same seed");
  check(first.tectonics == second.tectonics, "tectonic map repeats exactly for the same seed");
  check(firstStatistics.siteCount == secondStatistics.siteCount &&
            firstStatistics.riverEdgeCount == secondStatistics.riverEdgeCount &&
            firstStatistics.riverSourceCount == secondStatistics.riverSourceCount,
        "graph statistics repeat exactly for the same seed");
}

void testRasterWorkerDeterminism()
{
  std::cout << "polygon terrain raster worker determinism\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 256.0;
  settings.heightMetres = 256.0;
  settings.siteRadiusMetres = 18.0;
  settings.seed = 2024u;
  settings.riverSourceCount = 12u;

  procedural::PolygonTerrainRasterSettings singleThreadedRaster{};
  singleThreadedRaster.width = 192u;
  singleThreadedRaster.height = 192u;
  singleThreadedRaster.workerCount = 1u;
  procedural::PolygonTerrainRasterSettings threadedRaster = singleThreadedRaster;
  threadedRaster.workerCount = 4u;

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps singleThreaded{};
  procedural::PolygonTerrainMaps threaded{};
  std::string error;
  check(generator.generate(settings, singleThreadedRaster, singleThreaded, nullptr, &error), "single-threaded terrain generation succeeds");
  error.clear();
  check(generator.generate(settings, threadedRaster, threaded, nullptr, &error), "threaded terrain generation succeeds");
  check(singleThreaded.heightmap == threaded.heightmap, "heightmap is independent of raster worker count");
  check(singleThreaded.rivers == threaded.rivers, "river map is independent of raster worker count");
  check(singleThreaded.regions == threaded.regions, "region map is independent of raster worker count");
  check(singleThreaded.materials == threaded.materials, "material map is independent of raster worker count");
  check(singleThreaded.terrainForms == threaded.terrainForms, "terrain form map is independent of raster worker count");
  check(singleThreaded.temperature == threaded.temperature, "temperature map is independent of raster worker count");
  check(singleThreaded.precipitation == threaded.precipitation, "precipitation map is independent of raster worker count");
  check(singleThreaded.wind == threaded.wind, "wind map is independent of raster worker count");
  check(singleThreaded.forests == threaded.forests, "forest map is independent of raster worker count");
  check(singleThreaded.settlements == threaded.settlements, "city suitability map is independent of raster worker count");
  check(singleThreaded.continents == threaded.continents, "continent map is independent of raster worker count");
  check(singleThreaded.tectonics == threaded.tectonics, "tectonic map is independent of raster worker count");
}

void testTerrainPackageWritesCompressedTiles()
{
  std::cout << "polygon terrain package lz4 tiles\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 256.0;
  settings.heightMetres = 256.0;
  settings.siteRadiusMetres = 18.0;
  settings.seed = 313u;
  settings.riverSourceCount = 12u;

  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 192u;
  raster.height = 192u;
  raster.workerCount = 2u;

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps maps{};
  std::string error;
  check(generator.generate(settings, raster, maps, nullptr, &error), error.empty() ? "terrain generation succeeds for package test" : error.c_str());

  procedural::TerrainPackageSettings package{};
  package.tileSizePixels = 64u;
  package.voxelSizeMetres = 0.2;
  package.heightRangeMetres = 220.0;

  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::filesystem::path outputDirectory = std::filesystem::temp_directory_path() / ("polygon-terrain-package-test-" + unique);
  std::filesystem::create_directories(outputDirectory);

  procedural::TerrainPackageStatistics statistics{};
  error.clear();
  check(procedural::writeTerrainPackage(outputDirectory, settings, raster, maps, package, &statistics, &error),
      error.empty() ? "terrain package write succeeds" : error.c_str());
  check(statistics.tileColumns == 3u && statistics.tileRows == 3u, "terrain package is split into expected tiles");
  check(statistics.uncompressedBytes > 0u && statistics.compressedBytes > 0u, "terrain package reports compression byte counts");

  const std::string manifest = [&outputDirectory]
  {
    std::ifstream file(outputDirectory / "terrain-package.json");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }();
  check(manifest.find("\"compression\": \"lz4-block\"") != std::string::npos, "manifest declares LZ4 block compression");
  check(manifest.find("\"tileRecords\"") != std::string::npos, "manifest contains tile records");
  check(manifest.find(".terrain-tile.lz4") != std::string::npos, "manifest points at compressed tile blobs");

  const std::vector<uint8_t> compressed = readBinaryFile(outputDirectory / "tiles" / "x_00000_z_00000.terrain-tile.lz4");
  check(!compressed.empty(), "first compressed terrain tile exists");
  const uint32_t tileWidth = package.tileSizePixels + 2u;
  const uint32_t tileHeight = package.tileSizePixels + 2u;
  const uint32_t heightBytes = tileWidth * tileHeight * 2u;
  const uint32_t materialBytes = tileWidth * tileHeight;
  const uint32_t regionBytes = tileWidth * tileHeight * sizeof(uint32_t);
  const uint32_t headerBytes = 10u * sizeof(uint32_t);
  std::vector<uint8_t> decompressed(headerBytes + heightBytes + materialBytes + regionBytes);
  const int decompressedBytes = LZ4_decompress_safe(
      reinterpret_cast<const char *>(compressed.data()),
      reinterpret_cast<char *>(decompressed.data()),
      static_cast<int>(compressed.size()),
      static_cast<int>(decompressed.size()));
  check(decompressedBytes == static_cast<int>(decompressed.size()), "first compressed terrain tile decompresses");
  check(readU32(decompressed, 0u) == 0x52524554u, "tile header contains TERR magic");
  check(readU32(decompressed, 4u) == 2u, "tile header version carries stable region IDs");
  check(readU32(decompressed, 8u) == tileWidth && readU32(decompressed, 12u) == tileHeight, "tile header stores apron dimensions");
  check(readU32(decompressed, 16u) == headerBytes, "tile header stores height channel offset");
  check(readU32(decompressed, 20u) == headerBytes + heightBytes, "tile header stores material channel offset");
  check(readU32(decompressed, 24u) == headerBytes + heightBytes + materialBytes, "tile header stores source-region channel offset");
  check(readU32(decompressed, 28u) == heightBytes, "tile header stores height byte count");
  check(readU32(decompressed, 32u) == materialBytes, "tile header stores material byte count");
  check(readU32(decompressed, 36u) == regionBytes, "tile header stores source-region byte count");

  const uint16_t firstHeight = static_cast<uint16_t>(decompressed[headerBytes]) |
      (static_cast<uint16_t>(decompressed[headerBytes + 1u]) << 8u);
  check(firstHeight == maps.heightmap.front(), "tile height channel begins with clamped apron sample");
  check(decompressed[headerBytes + heightBytes] == maps.materials.front(), "tile material channel begins with clamped apron sample");
  check(readU32(decompressed, headerBytes + heightBytes + materialBytes) == maps.sourceRegions.front(), "tile source-region channel begins with clamped apron sample");

  std::filesystem::remove_all(outputDirectory);
}

void testClimateCreatesDesertsAndColdRegions()
{
  std::cout << "polygon terrain climate biomes\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 4096.0;
  settings.heightMetres = 4096.0;
  settings.siteRadiusMetres = 180.0;
  settings.seed = 777u;
  settings.riverSourceCount = 0u;

  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 256u;
  raster.height = 256u;
  raster.workerCount = 4u;

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps maps{};
  std::string error;
  check(generator.generate(settings, raster, maps, nullptr, &error), error.empty() ? "terrain generation succeeds for climate test" : error.c_str());

  uint32_t desertPixels = 0u;
  uint32_t coldPixels = 0u;
  for (const uint8_t material : maps.materials)
  {
    const auto typed = static_cast<procedural::PolygonTerrainMaterial>(material);
    if (typed == procedural::PolygonTerrainMaterial::TemperateDesert ||
        typed == procedural::PolygonTerrainMaterial::SubtropicalDesert)
      ++desertPixels;
    if (typed == procedural::PolygonTerrainMaterial::Ice ||
        typed == procedural::PolygonTerrainMaterial::Snow ||
        typed == procedural::PolygonTerrainMaterial::Tundra)
      ++coldPixels;
  }
  check(desertPixels > 0u, "climate simulation creates desert regions");
  check(coldPixels > 0u, "climate simulation creates ice or tundra regions");
  check(nonZeroCount(maps.temperature) > maps.temperature.size() / 2u, "climate simulation writes temperature diagnostics");
  check(nonZeroCount(maps.precipitation) > maps.precipitation.size() / 2u, "climate simulation writes precipitation diagnostics");
  check(nonZeroCount(maps.forests) > 0u, "climate simulation writes forest diagnostics");
  check(nonZeroCount(maps.settlements) > 0u, "terrain analysis writes city suitability diagnostics");
}

void testIslandProfileCreatesInteriorHighlands()
{
  std::cout << "polygon terrain island highlands\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 4096.0;
  settings.heightMetres = 4096.0;
  settings.siteRadiusMetres = 180.0;
  settings.seed = 777u;
  settings.riverSourceCount = 0u;

  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 256u;
  raster.height = 256u;
  raster.workerCount = 4u;

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps maps{};
  std::string error;
  check(generator.generate(settings, raster, maps, nullptr, &error), error.empty() ? "terrain generation succeeds for island profile test" : error.c_str());

  uint64_t centreTotal = 0u;
  uint64_t outerTotal = 0u;
  uint32_t centreCount = 0u;
  uint32_t outerCount = 0u;
  for (uint32_t y = 0u; y < maps.height; ++y)
    for (uint32_t x = 0u; x < maps.width; ++x)
    {
      const double dx = (static_cast<double>(x) + 0.5) / maps.width - 0.5;
      const double dy = (static_cast<double>(y) + 0.5) / maps.height - 0.5;
      const double radius = std::sqrt(dx * dx + dy * dy);
      const uint16_t height = maps.heightmap[static_cast<size_t>(y) * maps.width + x];
      if (radius < 0.18)
      {
        centreTotal += height;
        ++centreCount;
      }
      else if (radius > 0.36 && radius < 0.49)
      {
        outerTotal += height;
        ++outerCount;
      }
    }
  check(centreCount > 0u && outerCount > 0u, "island profile test sampled both interior and coastal bands");
  const double centreAverage = centreCount == 0u ? 0.0 : static_cast<double>(centreTotal) / centreCount;
  const double outerAverage = outerCount == 0u ? 0.0 : static_cast<double>(outerTotal) / outerCount;
  check(centreAverage > outerAverage * 2.0, "island profile creates high interior terrain above the coastal band");
  check(nonZeroCount(maps.rivers) > 0u, "island profile still routes rivers from the highlands");
}

void testHierarchicalContinentsCreateLandAndOcean()
{
  std::cout << "polygon terrain hierarchical continents\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 24000.0;
  settings.heightMetres = 16000.0;
  settings.siteRadiusMetres = 850.0;
  settings.seed = 424242u;
  settings.riverSourceCount = 0u;

  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 192u;
  raster.height = 128u;
  raster.workerCount = 4u;

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps maps{};
  procedural::PolygonTerrainStatistics statistics{};
  std::string error;
  check(generator.generate(settings, raster, maps, &statistics, &error), error.empty() ? "terrain generation succeeds for continent test" : error.c_str());
  if (maps.continents.empty()) return;
  const auto [minimumContinent, maximumContinent] = std::minmax_element(maps.continents.begin(), maps.continents.end());
  check(*minimumContinent < 110u, "hierarchical continent map contains ocean-scale regions");
  check(*maximumContinent > 145u, "hierarchical continent map contains continent-scale regions");
  check(nonZeroCount(maps.regions) > maps.regions.size() / 6u, "continent-scale terrain has visible land regions");
  check(statistics.riverSourceCount > 0u, "continent-scale terrain chooses automatic river headwaters");
}

void testProgressCanCancelGeneration()
{
  std::cout << "polygon terrain progress cancellation\n";
  procedural::PolygonTerrainSettings settings{};
  settings.widthMetres = 256.0;
  settings.heightMetres = 256.0;
  settings.siteRadiusMetres = 18.0;
  settings.seed = 5150u;

  procedural::PolygonTerrainRasterSettings raster{};
  raster.width = 128u;
  raster.height = 128u;

  procedural::PolygonTerrainProgress progress{};
  bool called = false;
  progress.callback = [&called](const char *, const double)
  {
    called = true;
    return false;
  };

  procedural::PolygonTerrainGenerator generator;
  procedural::PolygonTerrainMaps maps{};
  std::string error;
  check(!generator.generate(settings, raster, maps, nullptr, &error, &progress), "progress callback can cancel generation");
  check(called, "progress callback was invoked");
  check(error.find("cancelled") != std::string::npos, "cancelled generation reports cancellation");
}

} // namespace

int main()
{
  testMapsHaveExpectedShape();
  testMapsContainTerrainFeatures();
  testDeterminism();
  testRasterWorkerDeterminism();
  testTerrainPackageWritesCompressedTiles();
  testClimateCreatesDesertsAndColdRegions();
  testIslandProfileCreatesInteriorHighlands();
  testHierarchicalContinentsCreateLandAndOcean();
  testProgressCanCancelGeneration();
  return failures == 0 ? 0 : 1;
}
