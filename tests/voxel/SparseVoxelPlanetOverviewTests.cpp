#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PlanetPackage.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

uint32_t graph(const procedural::PlanetGraphChannel channel)
{
  return static_cast<uint32_t>(channel);
}

uint32_t raster(const procedural::PlanetRasterChannel channel)
{
  return static_cast<uint32_t>(channel);
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void appendU32(std::vector<uint8_t> &bytes, const uint32_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void appendF32(std::vector<uint8_t> &bytes, const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  appendU32(bytes, bits);
}

procedural::PlanetPackageChannel graphChannel(const procedural::PlanetGraphChannel channel, const procedural::PlanetPackageFormat format, const double scale = 1.0, const double bias = 0.0)
{
  return {procedural::PlanetPackageDomain::Graph, graph(channel), format, procedural::PlanetPackageMipPolicy::None, 0u, scale, bias, "graph"};
}

procedural::PlanetPackageChannel
rasterChannel(const procedural::PlanetRasterChannel channel, const procedural::PlanetPackageFormat format, const procedural::PlanetPackageMipPolicy mipPolicy, const double scale = 1.0, const double bias = 0.0)
{
  return {procedural::PlanetPackageDomain::Raster, raster(channel), format, mipPolicy, 0u, scale, bias, "raster"};
}

uint16_t atlasHeight(
    const voxel::svraster::PlanetOverviewMesh::SurfaceMaps &maps,
    const procedural::SpherePoint &input,
    const uint32_t mip)
{
  const double length = std::sqrt(
      input.x * input.x + input.y * input.y + input.z * input.z);
  const std::array<float, 3> direction{
    static_cast<float>(input.x / length),
    static_cast<float>(input.y / length),
    static_cast<float>(input.z / length),
  };
  const std::array<float, 3> absolute{
    std::abs(direction[0]), std::abs(direction[1]),
    std::abs(direction[2])};
  uint32_t face = 0u;
  float u = 0.0f;
  float v = 0.0f;
  if (absolute[0] >= absolute[1] && absolute[0] >= absolute[2])
  {
    face = direction[0] >= 0.0f ? 0u : 1u;
    u = direction[0] >= 0.0f ? -direction[2] / absolute[0]
                                : direction[2] / absolute[0];
    v = -direction[1] / absolute[0];
  }
  else if (absolute[1] >= absolute[2])
  {
    face = direction[1] >= 0.0f ? 2u : 3u;
    u = direction[0] / absolute[1];
    v = direction[1] >= 0.0f ? direction[2] / absolute[1]
                                : -direction[2] / absolute[1];
  }
  else
  {
    face = direction[2] >= 0.0f ? 4u : 5u;
    u = direction[2] >= 0.0f ? direction[0] / absolute[2]
                                : -direction[0] / absolute[2];
    v = -direction[1] / absolute[2];
  }
  const uint32_t side = std::max(maps.faceResolution >> mip, 1u);
  const uint32_t x = std::min(static_cast<uint32_t>(
      std::clamp(u * 0.5f + 0.5f, 0.0f, 0.999999f) * side), side - 1u);
  const uint32_t y = std::min(static_cast<uint32_t>(
      std::clamp(v * 0.5f + 0.5f, 0.0f, 0.999999f) * side), side - 1u);
  const uint32_t width = side * 3u;
  const size_t texel = static_cast<size_t>((face / 3u) * side + y) *
      width + (face % 3u) * side + x;
  return mip == 0u ? maps.heightR16Float[texel]
                   : maps.mipLevels[mip - 1u].heightR16Float[texel];
}

} // namespace

int main()
{
  namespace svr = voxel::svraster;
  using Material = procedural::PolygonTerrainMaterial;
  std::cout << "sparse voxel planet overview\n";

  // The high-density geometric fallback this used to assert is gone; the
  // overview no longer keeps a 512/1024 face-resolution mesh in reserve, so
  // pinning those two constants only recorded a decision that has been
  // reversed.

  const double forest = svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Grass), 0.55, 0.9, 0.0);
  const double dry = svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Grass), 0.55, 0.05, 0.0);
  const double alpine = svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Grass), 0.55, 0.9, 1.0);
  check(forest > dry && forest > alpine && svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Ocean), 0.55, 0.9, 0.0) == 0.0, "forest coverage follows climate and excludes water");
  bool boundedForest = true;
  for (uint32_t temperature = 0u; temperature <= 10u; ++temperature)
    for (uint32_t rain = 0u; rain <= 10u; ++rain)
    {
      const double weight = svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Grass), temperature / 10.0, rain / 10.0, 0.4);
      boundedForest &= weight >= 0.0 && weight <= 1.0 && weight == svr::planetOverviewForestWeight(static_cast<uint8_t>(Material::Grass), temperature / 10.0, rain / 10.0, 0.4);
    }
  check(boundedForest, "forest coverage is bounded and deterministic");

  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 6371000.0;
  metadata.tileSizePixels = 2u;
  metadata.gutterPixels = 0u;
  metadata.maximumTileLevel = 0u;

  std::vector<uint8_t> directions;
  constexpr float d = 0.57735026919f;
  for (const procedural::SpherePoint direction : {procedural::SpherePoint{d, d, d}, procedural::SpherePoint{-d, -d, d}, procedural::SpherePoint{-d, d, -d}, procedural::SpherePoint{d, -d, -d}})
  {
    appendF32(directions, static_cast<float>(direction.x));
    appendF32(directions, static_cast<float>(direction.y));
    appendF32(directions, static_cast<float>(direction.z));
  }
  std::vector<uint8_t> elevations;
  for (const uint16_t value : {100u, 200u, 300u, 400u})
    appendU16(elevations, value);
  std::vector<uint8_t> indices;
  for (const uint32_t index : {0u, 2u, 1u, 0u, 1u, 3u, 0u, 3u, 2u, 1u, 2u, 3u})
    appendU32(indices, index);
  const std::vector<uint8_t> materials{static_cast<uint8_t>(Material::Grass), static_cast<uint8_t>(Material::Ocean), static_cast<uint8_t>(Material::TemperateRainForest), static_cast<uint8_t>(Material::Rock)};
  const std::vector<procedural::PlanetPackageChannel> graphChannels{
    graphChannel(procedural::PlanetGraphChannel::SurfaceDirections, procedural::PlanetPackageFormat::RGB32Float),
    graphChannel(procedural::PlanetGraphChannel::SurfaceElevation, procedural::PlanetPackageFormat::R16UInt, 2.0, -100.0),
    graphChannel(procedural::PlanetGraphChannel::SurfaceIndices, procedural::PlanetPackageFormat::R32UInt),
    graphChannel(procedural::PlanetGraphChannel::SurfaceMaterial, procedural::PlanetPackageFormat::R8UInt),
    graphChannel(procedural::PlanetGraphChannel::SurfaceMountain, procedural::PlanetPackageFormat::R8UNorm),
    graphChannel(procedural::PlanetGraphChannel::SurfaceTemperature, procedural::PlanetPackageFormat::R8UNorm),
    graphChannel(procedural::PlanetGraphChannel::SurfacePrecipitation, procedural::PlanetPackageFormat::R8UNorm),
    graphChannel(procedural::PlanetGraphChannel::SurfaceRiver, procedural::PlanetPackageFormat::R8UNorm),
  };
  const auto graphBlock = [](const procedural::PlanetGraphChannel channel, std::vector<uint8_t> payload)
  {
    return procedural::PlanetPackageBlockInput{procedural::planetGraphBlockKey(graph(channel)), std::move(payload)};
  };
  const std::vector<procedural::PlanetPackageBlockInput> graphBlocks{
    graphBlock(procedural::PlanetGraphChannel::SurfaceDirections, directions),
    graphBlock(procedural::PlanetGraphChannel::SurfaceElevation, elevations),
    graphBlock(procedural::PlanetGraphChannel::SurfaceIndices, indices),
    graphBlock(procedural::PlanetGraphChannel::SurfaceMaterial, materials),
    graphBlock(procedural::PlanetGraphChannel::SurfaceMountain, {0u, 0u, 40u, 255u}),
    graphBlock(procedural::PlanetGraphChannel::SurfaceTemperature, {150u, 180u, 140u, 80u}),
    graphBlock(procedural::PlanetGraphChannel::SurfacePrecipitation, {220u, 255u, 245u, 40u}),
    graphBlock(procedural::PlanetGraphChannel::SurfaceRiver, {0u, 0u, 200u, 0u}),
  };
  const auto graphPath = std::filesystem::temp_directory_path() / "sparse-voxel-planet-overview-graph.planetpkg";
  std::string error;
  check(procedural::writePlanetPackage(graphPath, metadata, graphChannels, graphBlocks, {}, nullptr, &error), "graph-native overview package is written");
  procedural::PlanetPackageReader graphReader;
  svr::PlanetOverviewMesh graphMesh;
  check(graphReader.open(graphPath, &error) && svr::loadPlanetOverviewMesh(graphReader, {}, graphMesh, &error), "graph-native surface channels load into the overview mesh");
  check(
      graphMesh.graphNative && graphMesh.vertices.size() == 4u && graphMesh.indices.size() == 12u && graphMesh.vertices[0].directionElevation[3] == 100.0f && graphMesh.vertices[1].climateRiverWater[3] == 1.0f && graphMesh.maximumElevationMetres == 700.0,
      "graph topology, decoded relief, and water classification are preserved");
  const uint64_t graphVisualSignature =
      svr::planetOverviewMeshSignature(graphMesh);
  std::cout << "  graph visual signature: " << std::hex
            << graphVisualSignature << std::dec << '\n';

  const std::vector<procedural::PlanetPackageChannel> rasterChannels{
    rasterChannel(procedural::PlanetRasterChannel::Height, procedural::PlanetPackageFormat::R16UInt, procedural::PlanetPackageMipPolicy::Average, 1.0, -50.0),
    rasterChannel(procedural::PlanetRasterChannel::WaterHeight, procedural::PlanetPackageFormat::R16UInt, procedural::PlanetPackageMipPolicy::Nearest, 1.0, -50.0),
    rasterChannel(procedural::PlanetRasterChannel::OceanMask, procedural::PlanetPackageFormat::R8UNorm, procedural::PlanetPackageMipPolicy::Nearest),
    rasterChannel(procedural::PlanetRasterChannel::OceanFloorHeight, procedural::PlanetPackageFormat::R16UInt, procedural::PlanetPackageMipPolicy::Average, -1.0, 0.0),
    rasterChannel(procedural::PlanetRasterChannel::Material, procedural::PlanetPackageFormat::R8UInt, procedural::PlanetPackageMipPolicy::Mode),
  };
  std::vector<uint8_t> rasterHeights;
  for (const uint16_t height : {50u, 60u, 70u, 80u})
    appendU16(rasterHeights, height);
  const std::vector<uint8_t> rasterMaterials(4u, static_cast<uint8_t>(Material::Grass));
  std::vector<uint8_t> waterHeights;
  std::vector<uint8_t> oceanFloorHeights;
  for (const uint16_t height : {50u, 0u, 0u, 0u})
    appendU16(waterHeights, height);
  for (const uint16_t depth : {100u, 0u, 0u, 0u})
    appendU16(oceanFloorHeights, depth);
  const std::vector<uint8_t> oceanMask{255u, 0u, 0u, 0u};
  std::vector<procedural::PlanetPackageBlockInput> rasterBlocks;
  for (uint8_t face = 0u; face < procedural::kPlanetPackageCubeFaceCount; ++face)
  {
    const procedural::PlanetTileAddress tile{face, 0u, 0u, 0u};
    rasterBlocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), tile), rasterHeights});
    rasterBlocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::WaterHeight), tile), waterHeights});
    rasterBlocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::OceanMask), tile), oceanMask});
    rasterBlocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::OceanFloorHeight), tile), oceanFloorHeights});
    rasterBlocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Material), tile), rasterMaterials});
  }
  const auto rasterPath = std::filesystem::temp_directory_path() / "sparse-voxel-planet-overview-raster.planetpkg";
  check(procedural::writePlanetPackage(rasterPath, metadata, rasterChannels, rasterBlocks, {}, nullptr, &error), "raster-only overview package is written");
  procedural::PlanetPackageReader rasterReader;
  svr::PlanetOverviewMesh rasterMesh;
  check(rasterReader.open(rasterPath, &error) && svr::loadPlanetOverviewMesh(
      rasterReader,
      {.fallbackFaceResolution = 4u, .surfaceFaceResolution = 32u},
      rasterMesh, &error),
      "cube-sphere fallback loads when graph channels are absent");
  const bool indicesInRange = std::all_of(
      rasterMesh.indices.begin(),
      rasterMesh.indices.end(),
      [&rasterMesh](const uint32_t index)
      {
        return index < rasterMesh.vertices.size();
      });
  check(!rasterMesh.graphNative && rasterMesh.vertices.size() == 150u && rasterMesh.indices.size() == 576u && indicesInRange, "fallback mesh covers all six faces with valid immutable topology");
  bool materialMapIsGrass = rasterMesh.surfaceMaps.valid();
  for (size_t byte = 0u;
       materialMapIsGrass &&
       byte < rasterMesh.surfaceMaps.terrainFeaturesRGBA8.size(); byte += 4u)
    materialMapIsGrass =
        rasterMesh.surfaceMaps.terrainFeaturesRGBA8[byte] ==
        static_cast<uint8_t>(Material::Grass);
  check(
      rasterMesh.surfaceMaps.valid() &&
          rasterMesh.surfaceMaps.faceResolution == 32u &&
          rasterMesh.surfaceMaps.width == 96u &&
          rasterMesh.surfaceMaps.height == 64u && materialMapIsGrass &&
          rasterMesh.surfaceMaps.detailRangeMetres > 0.0,
      "overview bakes a high-resolution albedo/roughness and terrain-feature atlas");
  check(
      rasterMesh.surfaceMaps.mipCount() == 6u &&
          rasterMesh.surfaceMaps.mipLevels.back().faceResolution == 1u &&
          rasterMesh.surfaceMaps.heightR16Float.size() == 96u * 64u &&
          rasterMesh.surfaceMaps.heightMinMaxRG16Float.size() == 96u * 64u * 2u,
      "overview builds aligned R16F height and conservative min/max mip chains");
  bool separateBathymetry = false;
  bool oceanUsesWaterSurface = true;
  for (size_t pixel = 0u;
       pixel < rasterMesh.surfaceMaps.heightR16Float.size(); ++pixel)
  {
    const double visibleHeight =
        rasterMesh.surfaceMaps.detailMinimumMetres +
        svr::planetOverviewHalfToFloat(
            rasterMesh.surfaceMaps.heightR16Float[pixel]) *
            rasterMesh.surfaceMaps.detailRangeMetres;
    const double oceanFloor =
        rasterMesh.surfaceMaps.oceanFloorMinimumMetres +
        svr::planetOverviewHalfToFloat(
            rasterMesh.surfaceMaps.oceanFloorR16Float[pixel]) *
            rasterMesh.surfaceMaps.oceanFloorRangeMetres;
    if (oceanFloor < -0.01)
    {
      separateBathymetry = true;
      oceanUsesWaterSurface &= std::abs(visibleHeight) < 0.01;
    }
    else
      oceanUsesWaterSurface &= std::abs(oceanFloor) < 0.01;
  }
  check(separateBathymetry && oceanUsesWaterSurface,
      "negative ocean-floor elevation stays separate while visible ocean uses the water datum");
  bool conservativeMips = true;
  for (const auto &mip : rasterMesh.surfaceMaps.mipLevels)
    for (size_t texel = 0u;
         texel < mip.heightR16Float.size(); ++texel)
    {
      const float average =
          svr::planetOverviewHalfToFloat(mip.heightR16Float[texel]);
      const float minimum = svr::planetOverviewHalfToFloat(
          mip.heightMinMaxRG16Float[texel * 2u]);
      const float maximum = svr::planetOverviewHalfToFloat(
          mip.heightMinMaxRG16Float[texel * 2u + 1u]);
      conservativeMips &= minimum <= average + 0.001f &&
          average <= maximum + 0.001f;
    }
  check(conservativeMips,
      "checker/step height mips converge inside conservative bounds");
  bool seamSafe = true;
  uint32_t sharedSamples = 0u;
  for (uint32_t mip = 0u; mip < rasterMesh.surfaceMaps.mipCount(); ++mip)
  {
    std::map<std::array<int32_t, 3>, uint16_t> shared;
    for (uint8_t face = 0u;
         face < procedural::kPlanetPackageCubeFaceCount; ++face)
      for (uint32_t edge = 0u; edge < 4u; ++edge)
        for (uint32_t step = 0u; step <= 8u; ++step)
        {
          const double t = step / 8.0;
          const double u = edge < 2u ? static_cast<double>(edge) : t;
          const double v = edge < 2u ? t : static_cast<double>(edge - 2u);
          procedural::SpherePoint direction{};
          if (!procedural::planetCubeDirection(
                  {static_cast<procedural::PlanetCubeFace>(face), u, v},
                  direction))
          {
            seamSafe = false;
            continue;
          }
          const std::array<int32_t, 3> key{
            static_cast<int32_t>(std::lround(direction.x * 100000.0)),
            static_cast<int32_t>(std::lround(direction.y * 100000.0)),
            static_cast<int32_t>(std::lround(direction.z * 100000.0)),
          };
          const uint16_t height = atlasHeight(
              rasterMesh.surfaceMaps, direction, mip);
          const auto [found, inserted] = shared.emplace(key, height);
          if (!inserted)
          {
            ++sharedSamples;
            seamSafe &= found->second == height;
          }
        }
  }
  check(seamSafe && sharedSamples > 100u,
      "all shared cube-face edge directions sample bit-identical height mips");
  svr::PlanetOverviewMesh repeatedRasterMesh;
  check(
      svr::loadPlanetOverviewMesh(rasterReader,
          {.fallbackFaceResolution = 4u, .surfaceFaceResolution = 32u},
          repeatedRasterMesh, &error) &&
          repeatedRasterMesh.surfaceMaps.albedoRoughnessRGBA8 ==
              rasterMesh.surfaceMaps.albedoRoughnessRGBA8 &&
          repeatedRasterMesh.surfaceMaps.terrainFeaturesRGBA8 ==
              rasterMesh.surfaceMaps.terrainFeaturesRGBA8,
      "overview material maps are deterministic for a package and resolution");
  const uint64_t rasterVisualSignature =
      svr::planetOverviewMeshSignature(rasterMesh);
  std::cout << "  raster visual signature: " << std::hex
            << rasterVisualSignature << std::dec << '\n';
  check(
      graphVisualSignature == 0x473856f5688a9880ull &&
          rasterVisualSignature == 0x5fbfa273d49066b2ull,
      "overview graph-native and fallback visual baselines remain stable");

  std::error_code removeError;
  std::filesystem::remove(graphPath, removeError);
  std::filesystem::remove(rasterPath, removeError);
  return failures == 0 ? 0 : 1;
}
