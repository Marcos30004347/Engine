#include "procedural/PlanetGeology.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"

#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <thread>

namespace voxel::svraster
{
namespace
{

uint32_t graphChannel(const procedural::PlanetGraphChannel channel)
{
  return static_cast<uint32_t>(channel);
}

uint32_t rasterChannel(const procedural::PlanetRasterChannel channel)
{
  return static_cast<uint32_t>(channel);
}

double smoothstep(const double low, const double high, const double value)
{
  const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

bool waterMaterial(const uint8_t material)
{
  using Material = procedural::PolygonTerrainMaterial;
  switch (static_cast<Material>(material))
  {
  case Material::Ocean:
  case Material::Lake:
  case Material::River:
  case Material::Ice:
    return true;
  default:
    return false;
  }
}

float linearChannel(const uint32_t packed, const uint32_t shift)
{
  const float value = static_cast<float>((packed >> shift) & 0xffu) / 255.0f;
  return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::array<float, 3> linearColor(const uint32_t packed)
{
  return {linearChannel(packed, 0u), linearChannel(packed, 8u), linearChannel(packed, 16u)};
}

uint16_t readU16(const uint8_t *bytes)
{
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8u;
}

int16_t readS16(const uint8_t *bytes)
{
  return static_cast<int16_t>(readU16(bytes));
}

uint32_t readU32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8u | static_cast<uint32_t>(bytes[2]) << 16u | static_cast<uint32_t>(bytes[3]) << 24u;
}

float readF32(const uint8_t *bytes)
{
  const uint32_t bits = readU32(bytes);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool readGraphBlock(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetGraphChannel channel,
    const procedural::PlanetPackageFormat format,
    std::vector<uint8_t> &bytes,
    const procedural::PlanetPackageChannel **descriptor,
    std::string *error)
{
  const uint32_t id = graphChannel(channel);
  const auto *found = package.findChannel(procedural::PlanetPackageDomain::Graph, id);
  if (found == nullptr || found->format != format)
    return false;
  if (!package.readBlock(procedural::planetGraphBlockKey(id), bytes, error))
    return false;
  if (descriptor != nullptr)
    *descriptor = found;
  return true;
}

bool buildGraphMesh(const procedural::PlanetPackageReader &package, PlanetOverviewMesh &mesh, std::string *error)
{
  std::vector<uint8_t> directionBytes;
  std::vector<uint8_t> elevationBytes;
  std::vector<uint8_t> indexBytes;
  const procedural::PlanetPackageChannel *elevationDescriptor = nullptr;
  if (!readGraphBlock(package, procedural::PlanetGraphChannel::SurfaceDirections, procedural::PlanetPackageFormat::RGB32Float, directionBytes, nullptr, error) ||
      !readGraphBlock(package, procedural::PlanetGraphChannel::SurfaceElevation, procedural::PlanetPackageFormat::R16UInt, elevationBytes, &elevationDescriptor, error) ||
      !readGraphBlock(package, procedural::PlanetGraphChannel::SurfaceIndices, procedural::PlanetPackageFormat::R32UInt, indexBytes, nullptr, error))
    return false;

  if (directionBytes.empty() || directionBytes.size() % 12u != 0u || elevationBytes.size() / 2u != directionBytes.size() / 12u || indexBytes.empty() || indexBytes.size() % 12u != 0u)
  {
    if (error != nullptr)
      *error = "Planet overview graph surface blocks have inconsistent sizes";
    return false;
  }

  const size_t vertexCount = directionBytes.size() / 12u;
  std::vector<uint8_t> materials(vertexCount, static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass));
  std::vector<uint8_t> mountains(vertexCount, 0u);
  std::vector<uint8_t> temperatures(vertexCount, 127u);
  std::vector<uint8_t> precipitation(vertexCount, 127u);
  std::vector<uint8_t> rivers(vertexCount, 0u);
  const auto optionalByteChannel = [&](const procedural::PlanetGraphChannel channel, const procedural::PlanetPackageFormat format, std::vector<uint8_t> &destination)
  {
    std::vector<uint8_t> bytes;
    if (!readGraphBlock(package, channel, format, bytes, nullptr, nullptr))
      return;
    if (bytes.size() == vertexCount)
      destination = std::move(bytes);
  };
  optionalByteChannel(procedural::PlanetGraphChannel::SurfaceMaterial, procedural::PlanetPackageFormat::R8UInt, materials);
  optionalByteChannel(procedural::PlanetGraphChannel::SurfaceMountain, procedural::PlanetPackageFormat::R8UNorm, mountains);
  optionalByteChannel(procedural::PlanetGraphChannel::SurfaceTemperature, procedural::PlanetPackageFormat::R8UNorm, temperatures);
  optionalByteChannel(procedural::PlanetGraphChannel::SurfacePrecipitation, procedural::PlanetPackageFormat::R8UNorm, precipitation);
  optionalByteChannel(procedural::PlanetGraphChannel::SurfaceRiver, procedural::PlanetPackageFormat::R8UNorm, rivers);

  mesh.vertices.resize(vertexCount);
  for (size_t vertex = 0u; vertex < vertexCount; ++vertex)
  {
    procedural::SpherePoint direction{readF32(directionBytes.data() + vertex * 12u), readF32(directionBytes.data() + vertex * 12u + 4u), readF32(directionBytes.data() + vertex * 12u + 8u)};
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (!(length > 0.0) || !std::isfinite(length))
    {
      if (error != nullptr)
        *error = "Planet overview graph contains an invalid direction";
      return false;
    }
    direction.x /= length;
    direction.y /= length;
    direction.z /= length;
    const double elevation = static_cast<double>(readU16(elevationBytes.data() + vertex * 2u)) * elevationDescriptor->scale + elevationDescriptor->bias;
    const double mountain = mountains[vertex] / 255.0;
    const double temperature = temperatures[vertex] / 255.0;
    const double rain = precipitation[vertex] / 255.0;
    const double river = rivers[vertex] / 255.0;
    const SparseVoxelMaterialGPU material = planetVoxelSurfaceMaterial(materials[vertex], direction, package.metadata(), 0.0, mountain);
    const auto base = linearColor(material.albedoRGBA8);
    const auto soil = linearColor(material.reserved0);
    PlanetOverviewVertex &output = mesh.vertices[vertex];
    output.directionElevation = {static_cast<float>(direction.x), static_cast<float>(direction.y), static_cast<float>(direction.z), static_cast<float>(elevation)};
    output.baseColorForest = {base[0], base[1], base[2], static_cast<float>(planetOverviewForestWeight(materials[vertex], temperature, rain, mountain))};
    output.soilColorMountain = {soil[0], soil[1], soil[2], static_cast<float>(mountain)};
    output.climateRiverWater = {static_cast<float>(temperature), static_cast<float>(rain), static_cast<float>(river), waterMaterial(materials[vertex]) ? 1.0f : 0.0f};
  }

  mesh.indices.resize(indexBytes.size() / 4u);
  for (size_t index = 0u; index < mesh.indices.size(); ++index)
  {
    mesh.indices[index] = readU32(indexBytes.data() + index * 4u);
    if (mesh.indices[index] >= vertexCount)
    {
      if (error != nullptr)
        *error = "Planet overview graph contains an out-of-range index";
      return false;
    }
  }
  mesh.graphNative = true;
  return true;
}

struct RasterSampler
{
  const procedural::PlanetPackageReader &package;
  std::map<procedural::PlanetPackageBlockKey, std::vector<uint8_t>> cache;

  bool sample(
      const procedural::PlanetRasterChannel channel,
      const procedural::SpherePoint &direction,
      double &value,
      const bool interpolate = true)
  {
    const uint32_t id = rasterChannel(channel);
    const auto *descriptor = package.findChannel(procedural::PlanetPackageDomain::Raster, id);
    if (descriptor == nullptr)
      return false;
    procedural::PlanetTileAddress requested{};
    if (!procedural::planetCubeTile(direction, package.metadata().maximumTileLevel, requested))
      return false;
    const auto *record = package.findBestRasterBlock(id, requested);
    if (record == nullptr)
      return false;
    const auto cacheResult = cache.try_emplace(record->key);
    auto cached = cacheResult.first;
    const bool inserted = cacheResult.second;
    if (inserted && !package.readBlock(*record, cached->second, nullptr))
    {
      cache.erase(cached);
      return false;
    }
    double u = 0.0;
    double v = 0.0;
    procedural::PlanetTileAddress sampledTile{};
    if (!procedural::planetCubeTile(direction, record->key.tile.level, sampledTile, &u, &v))
      return false;
    const uint32_t tileSide = package.metadata().tileSizePixels;
    const uint32_t gutter = package.metadata().gutterPixels;
    const uint32_t side = tileSide + 2u * gutter;
    const uint32_t bytesPerTexel =
        descriptor->format == procedural::PlanetPackageFormat::R8UInt ||
            descriptor->format == procedural::PlanetPackageFormat::R8UNorm
        ? 1u
        : descriptor->format == procedural::PlanetPackageFormat::R16UInt ||
                descriptor->format == procedural::PlanetPackageFormat::R16SInt
            ? 2u
            : 0u;
    if (bytesPerTexel == 0u ||
        cached->second.size() < static_cast<size_t>(side) * side * bytesPerTexel)
      return false;
    const auto rawTexel = [&](const uint32_t x, const uint32_t y)
    {
      const size_t offset =
          (static_cast<size_t>(y) * side + x) * bytesPerTexel;
      if (bytesPerTexel == 1u)
        return static_cast<double>(cached->second[offset]);
      return descriptor->format == procedural::PlanetPackageFormat::R16SInt
          ? static_cast<double>(readS16(cached->second.data() + offset))
          : static_cast<double>(readU16(cached->second.data() + offset));
    };
    double raw = 0.0;
    if (!interpolate)
    {
      const uint32_t x = gutter + std::min(static_cast<uint32_t>(
          std::floor(std::clamp(u, 0.0, 1.0) * tileSide)), tileSide - 1u);
      const uint32_t y = gutter + std::min(static_cast<uint32_t>(
          std::floor(std::clamp(v, 0.0, 1.0) * tileSide)), tileSide - 1u);
      raw = rawTexel(x, y);
    }
    else
    {
      // Texel-centre coordinates plus the package gutter reproduce the
      // continuous cube-raster reconstruction used by the local horizon cap.
      // The old nearest lookup enlarged every source texel into a square in
      // the overview atlas and turned height gradients into visible steps.
      const double sampleX = std::clamp(
          static_cast<double>(gutter) + std::clamp(u, 0.0, 1.0) * tileSide - 0.5,
          0.0, static_cast<double>(side - 1u));
      const double sampleY = std::clamp(
          static_cast<double>(gutter) + std::clamp(v, 0.0, 1.0) * tileSide - 0.5,
          0.0, static_cast<double>(side - 1u));
      const uint32_t x0 = static_cast<uint32_t>(std::floor(sampleX));
      const uint32_t y0 = static_cast<uint32_t>(std::floor(sampleY));
      const uint32_t x1 = std::min(x0 + 1u, side - 1u);
      const uint32_t y1 = std::min(y0 + 1u, side - 1u);
      const double tx = sampleX - x0;
      const double ty = sampleY - y0;
      const double top = rawTexel(x0, y0) * (1.0 - tx) +
          rawTexel(x1, y0) * tx;
      const double bottom = rawTexel(x0, y1) * (1.0 - tx) +
          rawTexel(x1, y1) * tx;
      raw = top * (1.0 - ty) + bottom * ty;
    }
    if (descriptor->format == procedural::PlanetPackageFormat::R8UNorm)
      raw /= 255.0;
    value = raw * descriptor->scale + descriptor->bias;
    return true;
  }
};

bool buildSurfaceMaps(
    const procedural::PlanetPackageReader &package,
    const PlanetOverviewMeshSettings &settings,
    PlanetOverviewMesh::SurfaceMaps &maps)
{
  const auto channel = [](const procedural::PlanetRasterChannel value)
  {
    return static_cast<uint32_t>(value);
  };
  if (package.findChannel(procedural::PlanetPackageDomain::Raster,
          channel(procedural::PlanetRasterChannel::Material)) == nullptr ||
      package.findChannel(procedural::PlanetPackageDomain::Raster,
          channel(procedural::PlanetRasterChannel::Height)) == nullptr)
    return false;

  maps = {};
  const uint32_t requestedResolution =
      std::clamp(settings.surfaceFaceResolution, 32u, 8192u);
  maps.faceResolution = 1u;
  while (maps.faceResolution <= requestedResolution / 2u)
    maps.faceResolution *= 2u;
  maps.width = maps.faceResolution * 3u;
  maps.height = maps.faceResolution * 2u;
  const size_t pixelCount = static_cast<size_t>(maps.width) * maps.height;
  maps.albedoRoughnessRGBA8.assign(pixelCount * 4u, 0u);
  maps.terrainFeaturesRGBA8.assign(pixelCount * 4u, 0u);
  std::vector<double> heights(pixelCount, 0.0);
  std::vector<double> oceanFloors(pixelCount, 0.0);
  RasterSampler sampler{package};
  double minimumDetail = std::numeric_limits<double>::max();
  double maximumDetail = std::numeric_limits<double>::lowest();
  double minimumOceanFloor = 0.0;

  // One row of one cube face per work item. At a 2048 face this loop evaluates
  // 25 million texels, each sampling the package and running the full landform
  // octave stack, so serial generation would dominate planet load time.
  //
  // Every worker keeps its own RasterSampler: the sampler memoizes decompressed
  // tiles in a std::map, which is not safe to share, and the duplicated caches
  // cost only a few megabytes each.
  struct SurfaceRow
  {
    uint8_t face;
    uint32_t y;
  };
  std::vector<SurfaceRow> rows;
  rows.reserve(static_cast<size_t>(procedural::kPlanetPackageCubeFaceCount) * maps.faceResolution);
  for (uint8_t face = 0u; face < procedural::kPlanetPackageCubeFaceCount; ++face)
    for (uint32_t y = 0u; y < maps.faceResolution; ++y)
      rows.push_back({face, y});

  const uint32_t workerCount = std::max(1u, std::min<uint32_t>(
      std::thread::hardware_concurrency() == 0u ? 4u : std::thread::hardware_concurrency(),
      static_cast<uint32_t>(rows.size())));
  std::vector<double> workerMinimumDetail(workerCount, std::numeric_limits<double>::max());
  std::vector<double> workerMaximumDetail(workerCount, std::numeric_limits<double>::lowest());
  std::vector<double> workerMinimumOceanFloor(workerCount, 0.0);
  std::atomic<size_t> nextRow{0u};
  const auto generateRows = [&](const uint32_t worker)
  {
    RasterSampler localSampler{package};
    double localMinimumDetail = std::numeric_limits<double>::max();
    double localMaximumDetail = std::numeric_limits<double>::lowest();
    double localMinimumOceanFloor = 0.0;
    for (;;)
    {
      const size_t index = nextRow.fetch_add(1u);
      if (index >= rows.size())
        break;
      const uint8_t face = rows[index].face;
      const uint32_t y = rows[index].y;
      for (uint32_t x = 0u; x < maps.faceResolution; ++x)
      {
        procedural::SpherePoint direction{};
        if (!procedural::planetCubeDirection(
                {static_cast<procedural::PlanetCubeFace>(face),
                    (x + 0.5) / maps.faceResolution,
                    (y + 0.5) / maps.faceResolution},
                direction))
          continue;
        double materialValue = 0.0;
        double height = 0.0;
        double waterHeight = 0.0;
        double oceanFloorHeight = 0.0;
        double oceanMask = 0.0;
        double lakeMask = 0.0;
        double mountain = 0.0;
        double river = 0.0;
        double tectonics = 0.0;
        localSampler.sample(procedural::PlanetRasterChannel::Material,
            direction, materialValue, false);
        localSampler.sample(procedural::PlanetRasterChannel::Height,
            direction, height);
        const bool hasWaterHeight = localSampler.sample(
            procedural::PlanetRasterChannel::WaterHeight, direction,
            waterHeight, false);
        const bool hasOceanMask = localSampler.sample(
            procedural::PlanetRasterChannel::OceanMask, direction,
            oceanMask, false);
        const bool hasLakeMask = localSampler.sample(
            procedural::PlanetRasterChannel::LakeMask, direction, lakeMask,
            false);
        const bool hasOceanFloor = localSampler.sample(
            procedural::PlanetRasterChannel::OceanFloorHeight, direction,
            oceanFloorHeight);
        localSampler.sample(procedural::PlanetRasterChannel::Mountain,
            direction, mountain);
        localSampler.sample(procedural::PlanetRasterChannel::River,
            direction, river);
        localSampler.sample(procedural::PlanetRasterChannel::Tectonics,
            direction, tectonics);
        // Rock type, nearest so a class index is never blended. Packages
        // written before the geology channel existed leave it at the default.
        double lithologyValue = static_cast<double>(procedural::PlanetLithology::Sedimentary);
        localSampler.sample(procedural::PlanetRasterChannel::Lithology,
            direction, lithologyValue, false);
        const uint8_t lithology = static_cast<uint8_t>(std::clamp(
            std::lround(lithologyValue), 0l, 255l));
        const uint8_t material = static_cast<uint8_t>(std::clamp(
            std::lround(materialValue), 0l, 255l));
        const bool ocean = hasOceanMask ? oceanMask >= 0.5 :
            material == static_cast<uint8_t>(
                procedural::PolygonTerrainMaterial::Ocean);
        const bool lake = hasLakeMask ? lakeMask >= 0.5 :
            material == static_cast<uint8_t>(
                procedural::PolygonTerrainMaterial::Lake);
        const uint32_t atlasX = (face % 3u) * maps.faceResolution + x;
        const uint32_t atlasY = (face / 3u) * maps.faceResolution + y;
        const size_t pixel = static_cast<size_t>(atlasY) * maps.width + atlasX;
        const size_t byte = pixel * 4u;
        maps.terrainFeaturesRGBA8[byte] = material;
        maps.terrainFeaturesRGBA8[byte + 1u] = static_cast<uint8_t>(
            std::clamp(std::lround(mountain * 255.0), 0l, 255l));
        maps.terrainFeaturesRGBA8[byte + 2u] = static_cast<uint8_t>(
            std::clamp(std::lround(river * 255.0), 0l, 255l));
        // Solid height never supplies ocean or lake surfaces. Water occupancy
        // selects the separate water-height channel; dry terrain alone gets
        // deterministic sub-texel relief.
        const double surfaceHeight = (ocean || lake) && hasWaterHeight ?
            waterHeight : height;
        const double detail = surfaceHeight +
            ((ocean || lake) ? 0.0 : planetVoxelTerrainDetail(direction,
                package.metadata(), mountain, tectonics, material, 2.0,
                lithology));
        heights[pixel] = detail;
        oceanFloors[pixel] = ocean && hasOceanFloor
            ? std::min(oceanFloorHeight, 0.0) : 0.0;
        localMinimumDetail = std::min(localMinimumDetail, detail);
        localMaximumDetail = std::max(localMaximumDetail, detail);
        localMinimumOceanFloor = std::min(localMinimumOceanFloor, oceanFloors[pixel]);
      }
    }
    workerMinimumDetail[worker] = localMinimumDetail;
    workerMaximumDetail[worker] = localMaximumDetail;
    workerMinimumOceanFloor[worker] = localMinimumOceanFloor;
  };
  if (workerCount == 1u)
  {
    generateRows(0u);
  }
  else
  {
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (uint32_t worker = 0u; worker < workerCount; ++worker)
      workers.emplace_back(generateRows, worker);
    for (std::thread &worker : workers)
      worker.join();
  }
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
  {
    minimumDetail = std::min(minimumDetail, workerMinimumDetail[worker]);
    maximumDetail = std::max(maximumDetail, workerMaximumDetail[worker]);
    minimumOceanFloor = std::min(minimumOceanFloor, workerMinimumOceanFloor[worker]);
  }

  maps.detailMinimumMetres = std::isfinite(minimumDetail) ? minimumDetail : 0.0;
  maps.detailRangeMetres = std::isfinite(maximumDetail) &&
          maximumDetail > maps.detailMinimumMetres
      ? maximumDetail - maps.detailMinimumMetres
      : 1.0;
  maps.oceanFloorMinimumMetres = minimumOceanFloor;
  maps.oceanFloorRangeMetres = minimumOceanFloor < 0.0
      ? -minimumOceanFloor : 1.0;
  maps.heightR16Float.resize(pixelCount);
  maps.oceanFloorR16Float.resize(pixelCount);
  maps.heightMinMaxRG16Float.resize(pixelCount * 2u);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const double heightNormalized = std::clamp(
        (heights[pixel] - maps.detailMinimumMetres) /
            maps.detailRangeMetres,
        0.0, 1.0);
    const double oceanFloorNormalized = std::clamp(
        (oceanFloors[pixel] - maps.oceanFloorMinimumMetres) /
            maps.oceanFloorRangeMetres,
        0.0, 1.0);
    maps.heightR16Float[pixel] = planetOverviewFloatToHalf(
        static_cast<float>(heightNormalized));
    maps.oceanFloorR16Float[pixel] = planetOverviewFloatToHalf(
        static_cast<float>(oceanFloorNormalized));
    maps.heightMinMaxRG16Float[pixel * 2u] = maps.heightR16Float[pixel];
    maps.heightMinMaxRG16Float[pixel * 2u + 1u] =
        maps.heightR16Float[pixel];
  }

  // Derive a stable physical slope from the completed visible height map.
  // Keeping this as an atlas feature (instead of relying only on fragment
  // derivatives of an adaptively changing triangle) gives biome blending,
  // exposed-soil selection, and distant relief shading the same input. Face
  // edge samples use a one-sided derivative; only the outermost texel is
  // affected and every shared edge still owns an independently valid normal.
  constexpr double halfPi = 1.57079632679489661923;
  const double nominalTexelMetres = std::max(
      package.metadata().planetRadiusMetres * 2.0 /
          static_cast<double>(maps.faceResolution),
      1.0e-6);
  const auto facePixel = [&](const uint8_t face, const uint32_t x,
                             const uint32_t y)
  {
    const uint32_t atlasX = (face % 3u) * maps.faceResolution + x;
    const uint32_t atlasY = (face / 3u) * maps.faceResolution + y;
    return static_cast<size_t>(atlasY) * maps.width + atlasX;
  };
  for (uint8_t face = 0u;
       face < procedural::kPlanetPackageCubeFaceCount; ++face)
    for (uint32_t y = 0u; y < maps.faceResolution; ++y)
      for (uint32_t x = 0u; x < maps.faceResolution; ++x)
      {
        const uint32_t x0 = x == 0u ? x : x - 1u;
        const uint32_t x1 = std::min(x + 1u, maps.faceResolution - 1u);
        const uint32_t y0 = y == 0u ? y : y - 1u;
        const uint32_t y1 = std::min(y + 1u, maps.faceResolution - 1u);
        const double dx = (heights[facePixel(face, x1, y)] -
                              heights[facePixel(face, x0, y)]) /
            (nominalTexelMetres * std::max(x1 - x0, 1u));
        const double dy = (heights[facePixel(face, x, y1)] -
                              heights[facePixel(face, x, y0)]) /
            (nominalTexelMetres * std::max(y1 - y0, 1u));
        const double slope = std::sqrt(dx * dx + dy * dy);
        const double normalizedSlope =
            std::atan(std::max(slope, 0.0)) / halfPi;
        procedural::SpherePoint direction{};
        procedural::planetCubeDirection(
            {static_cast<procedural::PlanetCubeFace>(face),
                (x + 0.5) / maps.faceResolution,
                (y + 0.5) / maps.faceResolution},
            direction);
        const size_t pixel = facePixel(face, x, y);
        const size_t byte = pixel * 4u;
        const uint8_t material = maps.terrainFeaturesRGBA8[byte];
        const double mountain =
            maps.terrainFeaturesRGBA8[byte + 1u] / 255.0;
        const SparseVoxelMaterialGPU surface = planetVoxelSurfaceMaterial(
            material, direction, package.metadata(), slope, mountain);
        maps.albedoRoughnessRGBA8[byte] =
            static_cast<uint8_t>(surface.albedoRGBA8 & 0xffu);
        maps.albedoRoughnessRGBA8[byte + 1u] =
            static_cast<uint8_t>((surface.albedoRGBA8 >> 8u) & 0xffu);
        maps.albedoRoughnessRGBA8[byte + 2u] =
            static_cast<uint8_t>((surface.albedoRGBA8 >> 16u) & 0xffu);
        maps.albedoRoughnessRGBA8[byte + 3u] =
            static_cast<uint8_t>(surface.roughnessMetallicUNorm8 & 0xffu);
        maps.terrainFeaturesRGBA8[byte + 3u] = static_cast<uint8_t>(
            std::clamp(std::lround(normalizedSlope * 255.0), 0l, 255l));
      }

  uint32_t previousResolution = maps.faceResolution;
  uint32_t previousWidth = maps.width;
  const std::vector<uint8_t> *previousAlbedo =
      &maps.albedoRoughnessRGBA8;
  const std::vector<uint8_t> *previousFeatures =
      &maps.terrainFeaturesRGBA8;
  const std::vector<uint16_t> *previousHeight = &maps.heightR16Float;
  const std::vector<uint16_t> *previousOceanFloor =
      &maps.oceanFloorR16Float;
  const std::vector<uint16_t> *previousMinMax =
      &maps.heightMinMaxRG16Float;
  while (previousResolution > 1u)
  {
    PlanetOverviewMesh::SurfaceMaps::MipLevel mip{};
    mip.faceResolution = previousResolution / 2u;
    mip.width = mip.faceResolution * 3u;
    mip.height = mip.faceResolution * 2u;
    const size_t mipPixels = static_cast<size_t>(mip.width) * mip.height;
    mip.albedoRoughnessRGBA8.resize(mipPixels * 4u);
    mip.terrainFeaturesRGBA8.resize(mipPixels * 4u);
    mip.heightR16Float.resize(mipPixels);
    mip.oceanFloorR16Float.resize(mipPixels);
    mip.heightMinMaxRG16Float.resize(mipPixels * 2u);

    for (uint32_t face = 0u;
         face < procedural::kPlanetPackageCubeFaceCount; ++face)
      for (uint32_t y = 0u; y < mip.faceResolution; ++y)
        for (uint32_t x = 0u; x < mip.faceResolution; ++x)
        {
          const uint32_t destinationX =
              (face % 3u) * mip.faceResolution + x;
          const uint32_t destinationY =
              (face / 3u) * mip.faceResolution + y;
          const size_t destination =
              static_cast<size_t>(destinationY) * mip.width + destinationX;
          std::array<size_t, 4> sources{};
          for (uint32_t sampleY = 0u; sampleY < 2u; ++sampleY)
            for (uint32_t sampleX = 0u; sampleX < 2u; ++sampleX)
            {
              const uint32_t sourceX = (face % 3u) * previousResolution +
                  x * 2u + sampleX;
              const uint32_t sourceY = (face / 3u) * previousResolution +
                  y * 2u + sampleY;
              sources[sampleY * 2u + sampleX] =
                  static_cast<size_t>(sourceY) * previousWidth + sourceX;
            }
          for (uint32_t component = 0u; component < 4u; ++component)
          {
            uint32_t total = 0u;
            for (const size_t source : sources)
              total += (*previousAlbedo)[source * 4u + component];
            mip.albedoRoughnessRGBA8[destination * 4u + component] =
                static_cast<uint8_t>((total + 2u) / 4u);
          }
          std::array<uint8_t, 4> materials{};
          for (uint32_t sample = 0u; sample < 4u; ++sample)
            materials[sample] =
                (*previousFeatures)[sources[sample] * 4u];
          uint8_t mode = materials[0];
          uint32_t modeCount = 0u;
          for (const uint8_t candidate : materials)
          {
            const uint32_t count = static_cast<uint32_t>(std::count(
                materials.begin(), materials.end(), candidate));
            if (count > modeCount || (count == modeCount && candidate < mode))
            {
              mode = candidate;
              modeCount = count;
            }
          }
          mip.terrainFeaturesRGBA8[destination * 4u] = mode;
          uint8_t maximumMountain = 0u;
          uint32_t riverCoverage = 0u;
          for (const size_t source : sources)
          {
            maximumMountain = std::max(maximumMountain,
                (*previousFeatures)[source * 4u + 1u]);
            riverCoverage += (*previousFeatures)[source * 4u + 2u];
          }
          mip.terrainFeaturesRGBA8[destination * 4u + 1u] =
              maximumMountain;
          // River is a coverage field, not a presence flag. Maximum
          // reduction doubled thin channels at every mip and made a river
          // several pixels wide in a whole-globe view.
          mip.terrainFeaturesRGBA8[destination * 4u + 2u] =
              static_cast<uint8_t>((riverCoverage + 2u) / 4u);
          uint8_t maximumSlope = 0u;
          float height = 0.0f;
          float oceanFloor = 0.0f;
          float minimum = 1.0f;
          float maximum = 0.0f;
          for (const size_t source : sources)
          {
            maximumSlope = std::max(maximumSlope,
                (*previousFeatures)[source * 4u + 3u]);
            height += planetOverviewHalfToFloat((*previousHeight)[source]);
            oceanFloor += planetOverviewHalfToFloat(
                (*previousOceanFloor)[source]);
            minimum = std::min(minimum, planetOverviewHalfToFloat(
                (*previousMinMax)[source * 2u]));
            maximum = std::max(maximum, planetOverviewHalfToFloat(
                (*previousMinMax)[source * 2u + 1u]));
          }
          mip.terrainFeaturesRGBA8[destination * 4u + 3u] =
              maximumSlope;
          mip.heightR16Float[destination] =
              planetOverviewFloatToHalf(height * 0.25f);
          mip.oceanFloorR16Float[destination] =
              planetOverviewFloatToHalf(oceanFloor * 0.25f);
          mip.heightMinMaxRG16Float[destination * 2u] =
              planetOverviewFloatToHalf(minimum);
          mip.heightMinMaxRG16Float[destination * 2u + 1u] =
              planetOverviewFloatToHalf(maximum);
        }

    maps.mipLevels.push_back(std::move(mip));
    auto &stored = maps.mipLevels.back();
    previousResolution = stored.faceResolution;
    previousWidth = stored.width;
    previousAlbedo = &stored.albedoRoughnessRGBA8;
    previousFeatures = &stored.terrainFeaturesRGBA8;
    previousHeight = &stored.heightR16Float;
    previousOceanFloor = &stored.oceanFloorR16Float;
    previousMinMax = &stored.heightMinMaxRG16Float;
  }
  return maps.valid();
}

bool buildRasterFallback(const procedural::PlanetPackageReader &package, const PlanetOverviewMeshSettings &settings, PlanetOverviewMesh &mesh, std::string *error)
{
  if (package.findChannel(procedural::PlanetPackageDomain::Raster, rasterChannel(procedural::PlanetRasterChannel::Height)) == nullptr ||
      package.findChannel(procedural::PlanetPackageDomain::Raster, rasterChannel(procedural::PlanetRasterChannel::Material)) == nullptr)
  {
    if (error != nullptr)
      *error = "Planet package has neither a graph surface nor height/material raster fallback";
    return false;
  }
  const uint32_t resolution =
      std::clamp(settings.fallbackFaceResolution, 4u, 512u);
  const uint32_t side = resolution + 1u;
  mesh.vertices.reserve(6ull * side * side);
  mesh.indices.reserve(6ull * resolution * resolution * 6u);
  RasterSampler sampler{package};
  for (uint8_t face = 0u; face < procedural::kPlanetPackageCubeFaceCount; ++face)
  {
    const uint32_t vertexBase = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t y = 0u; y <= resolution; ++y)
      for (uint32_t x = 0u; x <= resolution; ++x)
      {
        procedural::SpherePoint direction{};
        if (!procedural::planetCubeDirection({static_cast<procedural::PlanetCubeFace>(face), static_cast<double>(x) / resolution, static_cast<double>(y) / resolution}, direction))
          return false;
        double elevation = 0.0;
        double waterElevation = 0.0;
        double oceanMask = 0.0;
        double lakeMask = 0.0;
        double materialValue = static_cast<double>(static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass));
        double mountain = 0.0;
        double temperature = 0.5;
        double rain = 0.5;
        double river = 0.0;
        if (!sampler.sample(procedural::PlanetRasterChannel::Height, direction, elevation) || !sampler.sample(procedural::PlanetRasterChannel::Material, direction, materialValue))
        {
          if (error != nullptr)
            *error = "Could not sample the planet overview raster fallback";
          return false;
        }
        sampler.sample(procedural::PlanetRasterChannel::Mountain, direction, mountain);
        sampler.sample(procedural::PlanetRasterChannel::Temperature, direction, temperature);
        sampler.sample(procedural::PlanetRasterChannel::Precipitation, direction, rain);
        sampler.sample(procedural::PlanetRasterChannel::River, direction, river);
        mountain = std::clamp(mountain, 0.0, 1.0);
        temperature = std::clamp(temperature, 0.0, 1.0);
        rain = std::clamp(rain, 0.0, 1.0);
        river = std::clamp(river, 0.0, 1.0);
        const uint8_t material = static_cast<uint8_t>(std::clamp(materialValue, 0.0, 255.0));
        const bool hasWaterElevation = sampler.sample(
            procedural::PlanetRasterChannel::WaterHeight, direction,
            waterElevation);
        const bool hasOceanMask = sampler.sample(
            procedural::PlanetRasterChannel::OceanMask, direction,
            oceanMask);
        const bool hasLakeMask = sampler.sample(
            procedural::PlanetRasterChannel::LakeMask, direction, lakeMask);
        const bool ocean = hasOceanMask ? oceanMask >= 0.5 :
            material == static_cast<uint8_t>(
                procedural::PolygonTerrainMaterial::Ocean);
        const bool lake = hasLakeMask ? lakeMask >= 0.5 :
            material == static_cast<uint8_t>(
                procedural::PolygonTerrainMaterial::Lake);
        if ((ocean || lake) && hasWaterElevation)
          elevation = waterElevation;
        const SparseVoxelMaterialGPU surface = planetVoxelSurfaceMaterial(material, direction, package.metadata(), 0.0, mountain);
        const auto base = linearColor(surface.albedoRGBA8);
        const auto soil = linearColor(surface.reserved0);
        mesh.vertices.push_back(
            {{static_cast<float>(direction.x), static_cast<float>(direction.y), static_cast<float>(direction.z), static_cast<float>(elevation)},
             {base[0], base[1], base[2], static_cast<float>(planetOverviewForestWeight(material, temperature, rain, mountain))},
             {soil[0], soil[1], soil[2], static_cast<float>(mountain)},
             {static_cast<float>(temperature), static_cast<float>(rain), static_cast<float>(river), waterMaterial(material) ? 1.0f : 0.0f}});
      }
    for (uint32_t y = 0u; y < resolution; ++y)
      for (uint32_t x = 0u; x < resolution; ++x)
      {
        const uint32_t a = vertexBase + y * side + x;
        const uint32_t b = a + 1u;
        const uint32_t c = a + side;
        const uint32_t d = c + 1u;
        const auto outward = [&](uint32_t i0, uint32_t i1, uint32_t i2)
        {
          const auto &p0 = mesh.vertices[i0].directionElevation;
          const auto &p1 = mesh.vertices[i1].directionElevation;
          const auto &p2 = mesh.vertices[i2].directionElevation;
          const double abx = p1[0] - p0[0];
          const double aby = p1[1] - p0[1];
          const double abz = p1[2] - p0[2];
          const double acx = p2[0] - p0[0];
          const double acy = p2[1] - p0[1];
          const double acz = p2[2] - p0[2];
          const double nx = aby * acz - abz * acy;
          const double ny = abz * acx - abx * acz;
          const double nz = abx * acy - aby * acx;
          if (nx * p0[0] + ny * p0[1] + nz * p0[2] < 0.0)
            std::swap(i1, i2);
          mesh.indices.insert(mesh.indices.end(), {i0, i1, i2});
        };
        outward(a, b, d);
        outward(a, d, c);
      }
  }
  mesh.graphNative = false;
  return true;
}

} // namespace

uint16_t planetOverviewFloatToHalf(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = static_cast<uint16_t>((bits >> 16u) & 0x8000u);
  const uint32_t exponentBits = (bits >> 23u) & 0xffu;
  uint32_t mantissa = bits & 0x7fffffu;
  if (exponentBits == 0xffu)
    return static_cast<uint16_t>(sign | 0x7c00u |
        (mantissa == 0u ? 0u : 0x0200u));
  int32_t exponent = static_cast<int32_t>(exponentBits) - 127 + 15;
  if (exponent <= 0)
  {
    if (exponent < -10)
      return sign;
    mantissa |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    const uint32_t rounded = (mantissa + (1u << (shift - 1u))) >> shift;
    return static_cast<uint16_t>(sign | rounded);
  }
  if (exponent >= 31)
    return static_cast<uint16_t>(sign | 0x7c00u);
  mantissa += 0x00001000u;
  if ((mantissa & 0x00800000u) != 0u)
  {
    mantissa = 0u;
    ++exponent;
    if (exponent >= 31)
      return static_cast<uint16_t>(sign | 0x7c00u);
  }
  return static_cast<uint16_t>(sign |
      static_cast<uint16_t>(exponent << 10u) |
      static_cast<uint16_t>(mantissa >> 13u));
}

float planetOverviewHalfToFloat(const uint16_t value)
{
  const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
  uint32_t exponent = (value >> 10u) & 0x1fu;
  uint32_t mantissa = value & 0x03ffu;
  uint32_t bits = 0u;
  if (exponent == 0u)
  {
    if (mantissa == 0u)
      bits = sign;
    else
    {
      int32_t unbiased = -14;
      while ((mantissa & 0x0400u) == 0u)
      {
        mantissa <<= 1u;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign |
          static_cast<uint32_t>(unbiased + 127) << 23u |
          mantissa << 13u;
    }
  }
  else if (exponent == 0x1fu)
    bits = sign | 0x7f800000u | mantissa << 13u;
  else
    bits = sign | (exponent - 15u + 127u) << 23u | mantissa << 13u;
  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

double planetOverviewForestWeight(const uint8_t material, const double temperature, const double precipitation, const double mountain)
{
  if (waterMaterial(material))
    return 0.0;
  const double temperateLow = smoothstep(0.18, 0.42, temperature);
  const double temperateHigh = 1.0 - smoothstep(0.82, 0.98, temperature);
  const double wet = smoothstep(0.28, 0.72, precipitation);
  const double mountainSuppression = 1.0 - smoothstep(0.45, 0.92, mountain) * 0.85;
  return std::clamp(temperateLow * temperateHigh * wet * mountainSuppression, 0.0, 1.0);
}

uint64_t planetOverviewMeshSignature(const PlanetOverviewMesh &mesh)
{
  uint64_t hash = 1469598103934665603ull;
  const auto byte = [&hash](const uint8_t value)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto word = [&byte](const uint32_t value)
  {
    byte(static_cast<uint8_t>(value));
    byte(static_cast<uint8_t>(value >> 8u));
    byte(static_cast<uint8_t>(value >> 16u));
    byte(static_cast<uint8_t>(value >> 24u));
  };
  word(static_cast<uint32_t>(mesh.vertices.size()));
  word(static_cast<uint32_t>(mesh.indices.size()));
  word(mesh.graphNative ? 1u : 0u);
  for (const PlanetOverviewVertex &vertex : mesh.vertices)
    for (const auto &attribute : {
             &vertex.directionElevation,
             &vertex.baseColorForest,
             &vertex.soilColorMountain,
             &vertex.climateRiverWater})
      for (const float value : *attribute)
      {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        word(bits);
      }
  for (const uint32_t index : mesh.indices)
    word(index);
  return hash;
}

bool loadPlanetOverviewMesh(const procedural::PlanetPackageReader &package, const PlanetOverviewMeshSettings &settings, PlanetOverviewMesh &mesh, std::string *error)
{
  mesh = {};
  if (!package.isOpen() || !(package.metadata().planetRadiusMetres > 0.0))
  {
    if (error != nullptr)
      *error = "Planet overview package is not open";
    return false;
  }
  mesh.sourceRadiusMetres = package.metadata().planetRadiusMetres;
  const auto finish = [&package, &settings, &mesh]()
  {
    for (const PlanetOverviewVertex &vertex : mesh.vertices)
      mesh.maximumElevationMetres = std::max(
          mesh.maximumElevationMetres,
          static_cast<double>(vertex.directionElevation[3]));
    if (buildSurfaceMaps(package, settings, mesh.surfaceMaps))
      mesh.maximumElevationMetres = std::max(
          mesh.maximumElevationMetres,
          mesh.surfaceMaps.detailMinimumMetres +
              mesh.surfaceMaps.detailRangeMetres);
    return true;
  };
  std::string graphError;
  if (!settings.preferRasterFallbackGeometry &&
      buildGraphMesh(package, mesh, &graphError))
    return finish();
  if (settings.preferRasterFallbackGeometry &&
      buildRasterFallback(package, settings, mesh, nullptr))
    return finish();
  mesh = {};
  mesh.sourceRadiusMetres = package.metadata().planetRadiusMetres;
  if (settings.preferRasterFallbackGeometry &&
      buildGraphMesh(package, mesh, &graphError))
    return finish();
  mesh = {};
  mesh.sourceRadiusMetres = package.metadata().planetRadiusMetres;
  if (buildRasterFallback(package, settings, mesh, error))
    return finish();
  if (error != nullptr && error->empty())
    *error = graphError.empty() ? "Could not build a planet overview mesh" : graphError;
  return false;
}

} // namespace voxel::svraster
