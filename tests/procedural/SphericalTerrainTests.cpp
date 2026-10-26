#include "procedural/PolygonTerrain.hpp"
#include "procedural/SphericalTerrain.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>

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

uint32_t nonZeroCount(const std::vector<uint8_t> &values)
{
  return static_cast<uint32_t>(std::count_if(
      values.begin(),
      values.end(),
      [](const uint8_t value)
      {
        return value != 0u;
      }));
}

void testPlanetMapsHaveExpectedShape()
{
  std::cout << "spherical terrain map shape\n";
  procedural::SphericalTerrainSettings settings{};
  settings.width = 128u;
  settings.height = 64u;
  settings.seed = 1234u;
  settings.graphRegionCount = 20000u;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps maps{};
  procedural::SphericalTerrainStatistics statistics{};
  check(generator.generate(settings, maps, &statistics), "spherical terrain generation succeeds");
  const size_t pixelCount = static_cast<size_t>(settings.width) * settings.height;
  check(maps.width == settings.width && maps.height == settings.height, "requested equirectangular dimensions are retained");
  check(maps.heightmap.size() == pixelCount, "heightmap contains every planet preview cell");
  check(maps.oceanFloorHeightmap.size() == pixelCount, "ocean-floor heightmap matches planet dimensions");
  check(maps.waterHeightmap.size() == pixelCount, "water-height map matches planet dimensions");
  check(maps.oceanMask.size() == pixelCount && maps.lakeMask.size() == pixelCount, "ocean and lake masks match planet dimensions");
  check(maps.sourceRegions.size() == pixelCount, "source Voronoi region map matches planet dimensions");
  check(maps.plates.size() == pixelCount, "plate map matches planet dimensions");
  check(maps.continents.size() == pixelCount, "continent map matches planet dimensions");
  check(maps.tectonics.size() == pixelCount, "tectonic map matches planet dimensions");
  check(maps.mountains.size() == pixelCount, "mountain map matches planet dimensions");
  check(maps.rivers.size() == pixelCount, "river map matches planet dimensions");
  check(maps.temperature.size() == pixelCount, "temperature map matches planet dimensions");
  check(maps.precipitation.size() == pixelCount, "precipitation map matches planet dimensions");
  check(maps.wind.size() == pixelCount, "wind exposure map matches planet dimensions");
  check(maps.windEast.size() == pixelCount && maps.windNorth.size() == pixelCount, "wind vector maps match planet dimensions");
  check(maps.materials.size() == pixelCount, "material map matches planet dimensions");
  check(
      std::none_of(
          maps.heightmap.begin(),
          maps.heightmap.end(),
          [](const uint16_t value)
          {
            return value == 65535u;
          }),
      "height encoding preserves peak headroom instead of hard-clamping");
  check(maps.regionElevations.size() == maps.regionPositions.size(), "optional region elevations match optional source regions");
  check(maps.regionRivers.size() == maps.regionPositions.size(), "optional region rivers match optional source regions");
  check(maps.regionNodes.size() == maps.regionPositions.size(), "authoritative graph nodes carry every source region");
  check(maps.regionNeighbourOffsets.size() == maps.regionNodes.size() + 1u, "graph nodes expose CSR neighbour offsets");
  check(!maps.regionNeighbourOffsets.empty() && maps.regionNeighbourOffsets.back() == maps.regionNeighbourIndices.size(), "graph CSR neighbour storage is complete");
  check(maps.surfacePositions.size() > maps.regionPositions.size(), "dual surface includes Delaunay triangle centres");
  check(maps.surfaceElevations.size() == maps.surfacePositions.size(), "dual surface carries region and triangle elevations");
  check(maps.surfaceContinents.size() == maps.surfacePositions.size(), "dual surface carries graph continent values");
  check(maps.surfaceTectonics.size() == maps.surfacePositions.size(), "dual surface carries graph tectonic values");
  check(maps.surfaceMountains.size() == maps.surfacePositions.size(), "dual surface carries post-erosion mountain values");
  check(maps.surfaceTemperature.size() == maps.surfacePositions.size(), "dual surface carries graph temperature values");
  check(maps.surfacePrecipitation.size() == maps.surfacePositions.size(), "dual surface carries advected graph precipitation values");
  check(maps.surfacePlates.size() == maps.surfacePositions.size(), "dual surface carries plate values");
  check(maps.surfaceRivers.size() == maps.surfacePositions.size(), "dual surface carries accumulated drainage values");
  check(maps.surfaceMaterials.size() == maps.surfacePositions.size(), "dual surface carries graph-native biome materials");
  check(!maps.surfaceIndices.empty() && maps.surfaceIndices.size() % 3u == 0u, "dual surface contains complete ridge and valley triangles");
  check(maps.surfaceIndices.empty() || *std::max_element(maps.surfaceIndices.begin(), maps.surfaceIndices.end()) < maps.surfacePositions.size(), "dual surface indices remain in bounds");
  check(statistics.plateCount >= 10u, "automatic plate count creates a non-trivial planet");
}

void testPlanetContainsLandOceanAndTectonics()
{
  std::cout << "spherical terrain features\n";
  procedural::SphericalTerrainSettings settings{};
  settings.width = 160u;
  settings.height = 80u;
  settings.seed = 9876u;
  settings.plateCount = 24u;
  settings.graphRegionCount = 20000u;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps maps{};
  procedural::SphericalTerrainStatistics statistics{};
  check(generator.generate(settings, maps, &statistics), "spherical terrain generation succeeds for feature test");
  const auto [minimumHeight, maximumHeight] = std::minmax_element(maps.heightmap.begin(), maps.heightmap.end());
  check(*minimumHeight == 0u, "planet has ocean");
  check(*maximumHeight > 24000u, "planet has high land");
  bool separatedWaterSurfaces = true;
  uint32_t oceanMaskPixels = 0u;
  uint32_t oceanFloorPixels = 0u;
  uint32_t lakeMaskPixels = 0u;
  for (size_t pixel = 0u; pixel < maps.heightmap.size(); ++pixel)
  {
    const bool ocean = maps.oceanMask[pixel] != 0u;
    const bool lake = maps.lakeMask[pixel] != 0u;
    oceanMaskPixels += ocean ? 1u : 0u;
    oceanFloorPixels += maps.oceanFloorHeightmap[pixel] != 0u ? 1u : 0u;
    lakeMaskPixels += lake ? 1u : 0u;
    separatedWaterSurfaces &= !(ocean && lake);
    if (ocean)
      separatedWaterSurfaces &= maps.heightmap[pixel] == 0u &&
          maps.waterHeightmap[pixel] == 0u;
    else
      separatedWaterSurfaces &= maps.oceanFloorHeightmap[pixel] == 0u;
    if (lake)
      separatedWaterSurfaces &= maps.waterHeightmap[pixel] >=
          maps.heightmap[pixel];
  }
  check(separatedWaterSurfaces && oceanMaskPixels > 0u && lakeMaskPixels > 0u,
      "solid height excludes ocean while water masks own ocean and lake surfaces");
  check(oceanFloorPixels > oceanMaskPixels * 9u / 10u &&
          *std::max_element(maps.oceanFloorHeightmap.begin(),
              maps.oceanFloorHeightmap.end()) > 10000u,
      "ocean occupancy carries a deep, non-flat bathymetric heightfield");
  check(nonZeroCount(maps.tectonics) > maps.tectonics.size() / 20u, "planet has tectonic boundary signal");
  check(nonZeroCount(maps.mountains) > maps.mountains.size() / 60u, "planet has explicit mountain ranges");
  check(nonZeroCount(maps.rivers) > 0u, "planet has downhill runoff rivers");
  const auto [minimumRain, maximumRain] = std::minmax_element(maps.precipitation.begin(), maps.precipitation.end());
  check(*minimumRain < 40u && *maximumRain > 180u, "wind transport creates both rain shadows and wet windward areas");
  check(nonZeroCount(maps.wind) > maps.wind.size() / 30u, "mountain lift creates wind exposure diagnostics");
  uint32_t dryLand = 0u;
  uint32_t wetLand = 0u;
  uint32_t desertNodes = 0u;
  uint32_t lakeNodes = 0u;
  std::array<uint16_t, 256> minimumPlateElevation{};
  std::array<uint16_t, 256> maximumPlateElevation{};
  std::array<uint32_t, 256> plateLandNodes{};
  minimumPlateElevation.fill(std::numeric_limits<uint16_t>::max());
  for (size_t pixel = 0u; pixel < maps.heightmap.size(); ++pixel)
    if (maps.heightmap[pixel] != 0u)
    {
      dryLand += maps.precipitation[pixel] < 62u ? 1u : 0u;
      wetLand += maps.precipitation[pixel] > 112u ? 1u : 0u;
    }
  for (const auto &node : maps.regionNodes)
  {
    using Material = procedural::PolygonTerrainMaterial;
    const Material material = static_cast<Material>(node.material);
    lakeNodes += material == Material::Lake ? 1u : 0u;
    desertNodes += material == Material::TemperateDesert || material == Material::SubtropicalDesert || material == Material::Shrubland ? 1u : 0u;
    if (node.elevation == 0u)
      continue;
    minimumPlateElevation[node.plate] = std::min(minimumPlateElevation[node.plate], node.elevation);
    maximumPlateElevation[node.plate] = std::max(maximumPlateElevation[node.plate], node.elevation);
    ++plateLandNodes[node.plate];
  }
  check(dryLand > statistics.landPixelCount / 30u, "leeward transport creates substantial dry land for deserts");
  check(wetLand > 0u, "windward mountain slopes retain wet land");
  check(lakeNodes > 0u && statistics.lakePixelCount == lakeNodes, "priority-flood depressions retain inland lake surfaces");
  bool validDrainage = true;
  bool retainedLakeSurface = false;
  bool retainedOceanFlag = false;
  for (uint32_t region = 0u; region < maps.regionNodes.size(); ++region)
  {
    const auto &node = maps.regionNodes[region];
    validDrainage =
        validDrainage && std::isfinite(node.riverDischarge) && node.riverDischarge >= 0.0f && (node.downstreamRegion == procedural::SphericalTerrainNode::noRegion || node.downstreamRegion < maps.regionNodes.size());
    if (node.downstreamRegion != procedural::SphericalTerrainNode::noRegion)
      validDrainage =
          validDrainage && std::find(maps.regionNeighbourIndices.begin() + maps.regionNeighbourOffsets[region], maps.regionNeighbourIndices.begin() + maps.regionNeighbourOffsets[region + 1u], node.downstreamRegion) !=
                               maps.regionNeighbourIndices.begin() + maps.regionNeighbourOffsets[region + 1u];
    retainedLakeSurface = retainedLakeSurface || ((node.flags & procedural::SphericalTerrainNode::lakeFlag) != 0u && node.waterElevation > node.terrainElevation);
    retainedOceanFlag = retainedOceanFlag || (node.flags & procedural::SphericalTerrainNode::oceanFlag) != 0u;
  }
  check(validDrainage, "authoritative graph retains a valid directed drainage receiver and discharge per region");
  check(retainedLakeSurface, "authoritative graph retains lake water separately from basin terrain");
  check(retainedOceanFlag, "authoritative graph retains ocean classification flags");
  check(desertNodes > statistics.landPixelCount / 100u, "continentality and subtropical circulation create dry biomes");
  bool variedPlate = false;
  for (size_t plate = 0u; plate < plateLandNodes.size(); ++plate)
    if (plateLandNodes[plate] >= 100u && maximumPlateElevation[plate] - minimumPlateElevation[plate] > 9000u)
      variedPlate = true;
  check(variedPlate, "large plates contain substantial internal elevation variation");
  std::set<uint8_t> landMaterials;
  double coastalElevation = 0.0;
  double inlandElevation = 0.0;
  uint32_t coastalNodes = 0u;
  uint32_t inlandNodes = 0u;
  for (uint32_t region = 0u; region < maps.regionNodes.size(); ++region)
  {
    const auto &node = maps.regionNodes[region];
    if (node.elevation == 0u)
      continue;
    landMaterials.insert(node.material);
    bool touchesOcean = false;
    for (uint32_t edge = maps.regionNeighbourOffsets[region]; edge < maps.regionNeighbourOffsets[region + 1u]; ++edge)
      if (maps.regionNodes[maps.regionNeighbourIndices[edge]].elevation == 0u)
      {
        touchesOcean = true;
        break;
      }
    if (touchesOcean)
    {
      coastalElevation += node.elevation;
      ++coastalNodes;
    }
    else if (node.elevation > 0u)
    {
      inlandElevation += node.elevation;
      ++inlandNodes;
    }
  }
  check(landMaterials.size() >= 8u, "climate classification creates a varied set of land biomes");
  check(coastalNodes > 0u && inlandNodes > 0u && coastalElevation / coastalNodes < inlandElevation / inlandNodes, "coastal land is lower on average than continental interiors");
  check(statistics.continentalPlateCount > 0u && statistics.oceanicPlateCount > 0u, "planet has continental and oceanic plates");
  check(statistics.landPixelCount > 0u && statistics.oceanPixelCount > 0u, "planet statistics contain land and ocean");
  check(statistics.riverSourceCount > 0u && statistics.riverPixelCount > 0u, "planet statistics contain river sources and river pixels");

  using Material = procedural::PolygonTerrainMaterial;
  uint64_t desertSlopeSum = 0u;
  uint32_t desertEdges = 0u;
  uint16_t maximumDesertStep = 0u;
  for (uint32_t y = 0u; y < maps.height; ++y)
    for (uint32_t x = 0u; x < maps.width; ++x)
    {
      const size_t cell = static_cast<size_t>(y) * maps.width + x;
      const size_t neighbour = static_cast<size_t>(y) * maps.width +
          ((x + 1u) % maps.width);
      const auto material = static_cast<Material>(maps.materials[cell]);
      const auto neighbourMaterial =
          static_cast<Material>(maps.materials[neighbour]);
      const uint16_t step = static_cast<uint16_t>(std::abs(
          static_cast<int32_t>(maps.heightmap[cell]) -
          static_cast<int32_t>(maps.heightmap[neighbour])));
      const bool desert =
          (material == Material::TemperateDesert ||
              material == Material::SubtropicalDesert) &&
          material == neighbourMaterial;
      if (desert)
      {
        desertSlopeSum += step;
        maximumDesertStep = std::max(maximumDesertStep, step);
        ++desertEdges;
      }
    }
  const double meanDesertStep = desertSlopeSum /
      static_cast<double>(std::max(1u, desertEdges));
  check(
      desertEdges > 20u && maximumDesertStep > 20u &&
          meanDesertStep > 1.0,
      "desert biomes contain coherent heightfield slopes instead of flat material paint");
}

void testPlanetDeterminism()
{
  std::cout << "spherical terrain determinism\n";
  procedural::SphericalTerrainSettings settings{};
  settings.width = 96u;
  settings.height = 48u;
  settings.seed = 5150u;
  settings.plateCount = 18u;
  settings.graphRegionCount = 20000u;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps first{};
  procedural::SphericalTerrainMaps second{};
  procedural::SphericalTerrainStatistics firstStatistics{};
  procedural::SphericalTerrainStatistics secondStatistics{};
  check(generator.generate(settings, first, &firstStatistics), "first planet generation succeeds");
  check(generator.generate(settings, second, &secondStatistics), "second planet generation succeeds");
  check(first.heightmap == second.heightmap, "heightmap repeats exactly");
  check(first.oceanFloorHeightmap == second.oceanFloorHeightmap &&
          first.waterHeightmap == second.waterHeightmap &&
          first.oceanMask == second.oceanMask &&
          first.lakeMask == second.lakeMask,
      "seabed, separate water surfaces, and masks repeat exactly");
  check(first.sourceRegions == second.sourceRegions, "source Voronoi region IDs repeat exactly");
  check(first.plates == second.plates, "plate map repeats exactly");
  check(first.continents == second.continents, "continent map repeats exactly");
  check(first.tectonics == second.tectonics, "tectonic map repeats exactly");
  check(first.mountains == second.mountains, "mountain map repeats exactly");
  check(first.rivers == second.rivers, "river map repeats exactly");
  check(first.temperature == second.temperature, "temperature map repeats exactly");
  check(first.precipitation == second.precipitation, "precipitation map repeats exactly");
  check(first.wind == second.wind && first.windEast == second.windEast && first.windNorth == second.windNorth, "wind transport repeats exactly");
  check(first.regionElevations == second.regionElevations, "optional region elevations repeat exactly");
  check(first.regionRivers == second.regionRivers, "optional region rivers repeat exactly");
  check(first.surfaceElevations == second.surfaceElevations, "post-erosion graph elevations repeat exactly");
  check(first.surfacePrecipitation == second.surfacePrecipitation && first.surfaceRivers == second.surfaceRivers, "graph climate and drainage repeat exactly");
  check(first.surfaceMaterials == second.surfaceMaterials, "graph biome materials repeat exactly");
  check(first.sphereLines.size() == second.sphereLines.size(), "optional sphere line count repeats exactly");
  check(
      firstStatistics.landPixelCount == secondStatistics.landPixelCount && firstStatistics.mountainPixelCount == secondStatistics.mountainPixelCount && firstStatistics.lakePixelCount == secondStatistics.lakePixelCount &&
          firstStatistics.riverPixelCount == secondStatistics.riverPixelCount,
      "planet statistics repeat exactly");
}

void testErosionFeedsFinalRasterMaps()
{
  std::cout << "spherical terrain erosion export\n";
  procedural::SphericalTerrainSettings unerodedSettings{};
  unerodedSettings.width = 128u;
  unerodedSettings.height = 64u;
  unerodedSettings.seed = 0xe20510ull;
  unerodedSettings.graphRegionCount = 12000u;
  unerodedSettings.hydraulicIterations = 0u;
  unerodedSettings.thermalIterations = 0u;
  unerodedSettings.ridgeSharpeningIterations = 0u;
  unerodedSettings.soilCreepIterations = 0u;
  procedural::SphericalTerrainSettings erodedSettings = unerodedSettings;
  erodedSettings.hydraulicIterations = 10u;
  erodedSettings.thermalIterations = 1u;
  erodedSettings.ridgeSharpeningIterations = 3u;
  erodedSettings.soilCreepIterations = 3u;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps uneroded{};
  procedural::SphericalTerrainMaps eroded{};
  check(generator.generate(unerodedSettings, uneroded, nullptr),
      "un-eroded comparison planet generation succeeds");
  check(generator.generate(erodedSettings, eroded, nullptr),
      "eroded comparison planet generation succeeds");
  check(uneroded.heightmap != eroded.heightmap,
      "hydraulic and thermal passes modify the exported height texture");
  check(nonZeroCount(eroded.rivers) > 0u &&
          eroded.rivers.size() == eroded.heightmap.size(),
      "final runoff is routed again over the eroded height texture");
  bool erodedMaterialsFollowWater = true;
  for (size_t pixel = 0u; pixel < eroded.heightmap.size(); ++pixel)
    if (eroded.heightmap[pixel] == 0u)
      erodedMaterialsFollowWater &=
          eroded.materials[pixel] == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Ocean) ||
          eroded.materials[pixel] == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Lake);
  check(erodedMaterialsFollowWater,
      "post-erosion material classification preserves the final water mask");
}

void testMacroSimulationDoesNotDependOnRasterResolution()
{
  std::cout << "spherical terrain macro/raster separation\n";
  procedural::SphericalTerrainSettings lowSettings{};
  lowSettings.width = 96u;
  lowSettings.height = 48u;
  lowSettings.seed = 0x1843u;
  lowSettings.graphRegionCount = 20000u;
  procedural::SphericalTerrainSettings highSettings = lowSettings;
  highSettings.width = 192u;
  highSettings.height = 96u;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps low{};
  procedural::SphericalTerrainMaps high{};
  procedural::SphericalTerrainStatistics lowStatistics{};
  procedural::SphericalTerrainStatistics highStatistics{};
  check(generator.generate(lowSettings, low, &lowStatistics), "low-resolution hybrid planet generation succeeds");
  check(generator.generate(highSettings, high, &highStatistics), "high-resolution hybrid planet generation succeeds");
  check(lowStatistics.plateCount == highStatistics.plateCount, "raster resolution does not change the automatic plate count");
  check(low.regionPositions.size() == high.regionPositions.size() && low.regionPositions.size() >= 4000u, "raster resolution does not change the macro Voronoi graph");
  bool sameRegions = low.regionPositions.size() == high.regionPositions.size();
  for (size_t region = 0u; sameRegions && region < low.regionPositions.size(); ++region)
    sameRegions = low.regionPositions[region].x == high.regionPositions[region].x && low.regionPositions[region].y == high.regionPositions[region].y && low.regionPositions[region].z == high.regionPositions[region].z;
  check(sameRegions, "a seed retains identical macro region positions at every raster resolution");
  check(low.surfaceElevations == high.surfaceElevations, "post-erosion spherical elevation does not depend on export resolution");
  check(low.surfacePrecipitation == high.surfacePrecipitation && low.surfaceRivers == high.surfaceRivers, "graph climate and drainage do not depend on export resolution");
  check(low.heightmap.size() * 4u == high.heightmap.size(), "higher resolution increases only raster detail storage");
}

void testGraphOnlyGenerationDefersAtlases()
{
  std::cout << "spherical graph-only generation\n";
  procedural::SphericalTerrainSettings settings{};
  settings.width = 1024u;
  settings.height = 512u;
  settings.seed = 0x1843u;
  settings.graphRegionCount = 8000u;
  settings.rasterizeMaps = false;

  procedural::SphericalTerrainGenerator generator;
  procedural::SphericalTerrainMaps maps{};
  procedural::SphericalTerrainStatistics statistics{};
  check(generator.generate(settings, maps, &statistics), "graph-only spherical generation succeeds");
  check(maps.width == settings.width && maps.height == settings.height, "deferred atlas dimensions remain available as export metadata");
  check(maps.heightmap.empty() && maps.sourceRegions.empty() && maps.continents.empty(), "graph-only generation allocates no raster atlases");
  check(maps.regionNodes.size() >= settings.graphRegionCount, "graph-only generation retains authoritative node payloads");
  check(maps.regionNeighbourOffsets.size() == maps.regionNodes.size() + 1u && maps.regionNeighbourOffsets.back() == maps.regionNeighbourIndices.size(), "graph-only generation retains streamable CSR topology");
  check(maps.surfacePositions.size() > maps.regionNodes.size() && !maps.surfaceIndices.empty(), "graph-only generation retains the direct preview mesh");
  check(maps.surfaceMaterials.size() == maps.surfacePositions.size(), "graph-only generation retains biome materials without an atlas");
  check(statistics.riverPixelCount > 0u, "graph-only statistics count flowing graph regions without an atlas");
}

} // namespace

int main()
{
  testPlanetMapsHaveExpectedShape();
  testPlanetContainsLandOceanAndTectonics();
  testPlanetDeterminism();
  testErosionFeedsFinalRasterMaps();
  testMacroSimulationDoesNotDependOnRasterResolution();
  testGraphOnlyGenerationDefersAtlases();
  return failures == 0 ? 0 : 1;
}
