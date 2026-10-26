#include "procedural/SphericalTerrainPackage.hpp"

#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PolygonTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace procedural
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint32_t graphChannel(const PlanetGraphChannel channel)
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
  for (uint32_t byte = 0u; byte < 4u; ++byte)
    bytes.push_back(static_cast<uint8_t>((value >> (byte * 8u)) & 0xffu));
}

void appendF32(std::vector<uint8_t> &bytes, const float value)
{
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  appendU32(bytes, bits);
}

void addChannelAndBlock(
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    const PlanetGraphChannel channel,
    const PlanetPackageFormat format,
    const std::string &name,
    std::vector<uint8_t> payload,
    const double scale = 1.0,
    const double bias = 0.0)
{
  const uint32_t id = graphChannel(channel);
  channels.push_back({PlanetPackageDomain::Graph, id, format, PlanetPackageMipPolicy::None, 0u, scale, bias, name});
  blocks.push_back({planetGraphBlockKey(id), std::move(payload)});
}

template <typename Member> std::vector<uint8_t> nodeBytes(const std::vector<SphericalTerrainNode> &nodes, Member member)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(nodes.size());
  for (const SphericalTerrainNode &node : nodes)
    bytes.push_back(node.*member);
  return bytes;
}

template <typename First, typename Second> std::vector<uint8_t> nodePairBytes(const std::vector<SphericalTerrainNode> &nodes, First first, Second second)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(nodes.size() * 2u);
  for (const SphericalTerrainNode &node : nodes)
  {
    bytes.push_back(node.*first);
    bytes.push_back(node.*second);
  }
  return bytes;
}

std::vector<uint8_t> u16Bytes(const std::vector<uint16_t> &values)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(values.size() * sizeof(uint16_t));
  for (const uint16_t value : values)
    appendU16(bytes, value);
  return bytes;
}

std::vector<uint8_t> u32Bytes(const std::vector<uint32_t> &values)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(values.size() * sizeof(uint32_t));
  for (const uint32_t value : values)
    appendU32(bytes, value);
  return bytes;
}

bool appendSurfaceMesh(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    std::string *error)
{
  const size_t regionCount = maps.regionNodes.size();
  const size_t vertexCount = maps.surfacePositions.size();
  const bool anyMeshData = vertexCount != 0u ||
      !maps.surfaceElevations.empty() || !maps.surfaceIndices.empty();
  if (!anyMeshData)
    return true;
  if (vertexCount < regionCount ||
      maps.surfaceElevations.size() != vertexCount ||
      maps.surfaceContinents.size() != vertexCount ||
      maps.surfaceTectonics.size() != vertexCount ||
      maps.surfaceMountains.size() != vertexCount ||
      maps.surfaceTemperature.size() != vertexCount ||
      maps.surfacePrecipitation.size() != vertexCount ||
      maps.surfacePlates.size() != vertexCount ||
      maps.surfaceRivers.size() != vertexCount ||
      maps.surfaceMaterials.size() != vertexCount)
    return fail(error, "Spherical terrain surface mesh attributes are inconsistent");
  const size_t sourceTriangleCount = vertexCount - regionCount;
  if (sourceTriangleCount == 0u ||
      sourceTriangleCount > std::numeric_limits<uint32_t>::max() ||
      maps.surfaceIndices.size() != sourceTriangleCount * 9u)
    return fail(error, "Spherical terrain surface mesh topology is inconsistent");

  std::vector<uint8_t> directions;
  directions.reserve(vertexCount * 3u * sizeof(float));
  for (const SpherePoint &position : maps.surfacePositions)
  {
    const double lengthSquared = position.x * position.x +
        position.y * position.y + position.z * position.z;
    if (!std::isfinite(lengthSquared) ||
        std::abs(lengthSquared - 1.0) > 1.0e-5)
      return fail(error, "Spherical terrain surface mesh contains a non-unit direction");
    appendF32(directions, static_cast<float>(position.x));
    appendF32(directions, static_cast<float>(position.y));
    appendF32(directions, static_cast<float>(position.z));
  }

  std::vector<uint32_t> faceRegions;
  std::vector<uint32_t> faceTriangles;
  std::vector<uint32_t> triangleRegions;
  const size_t faceCount = maps.surfaceIndices.size() / 3u;
  faceRegions.reserve(faceCount);
  faceTriangles.reserve(faceCount);
  triangleRegions.reserve(sourceTriangleCount * 3u);
  for (size_t face = 0u; face < faceCount; ++face)
  {
    const uint32_t first = maps.surfaceIndices[face * 3u];
    const uint32_t second = maps.surfaceIndices[face * 3u + 1u];
    const uint32_t third = maps.surfaceIndices[face * 3u + 2u];
    if (first >= regionCount || second >= vertexCount || third >= vertexCount)
      return fail(error, "Spherical terrain surface mesh contains an invalid index mapping");
    faceRegions.push_back(first);
    faceTriangles.push_back(static_cast<uint32_t>(face / 3u));
    if ((face % 3u) == 0u)
    {
      triangleRegions.push_back(first);
      triangleRegions.push_back(
          maps.surfaceIndices[(face + 1u) * 3u]);
      triangleRegions.push_back(
          maps.surfaceIndices[(face + 2u) * 3u]);
      if (triangleRegions[triangleRegions.size() - 2u] >= regionCount ||
          triangleRegions.back() >= regionCount)
        return fail(error, "Spherical terrain triangle-region mapping is invalid");
    }
  }

  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceDirections,
      PlanetPackageFormat::RGB32Float, "surfaceDirections",
      std::move(directions));
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceElevation,
      PlanetPackageFormat::R16UInt, "surfaceElevation",
      u16Bytes(maps.surfaceElevations), metadata.heightScaleMetres,
      metadata.heightBiasMetres);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceIndices,
      PlanetPackageFormat::R32UInt, "surfaceIndices",
      u32Bytes(maps.surfaceIndices));
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceFaceRegion,
      PlanetPackageFormat::R32UInt, "surfaceFaceRegion",
      u32Bytes(faceRegions));
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceFaceTriangle,
      PlanetPackageFormat::R32UInt, "surfaceFaceTriangle",
      u32Bytes(faceTriangles));
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::TriangleRegions,
      PlanetPackageFormat::R32UInt, "triangleRegions",
      u32Bytes(triangleRegions));
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceMaterial,
      PlanetPackageFormat::R8UInt, "surfaceMaterial",
      maps.surfaceMaterials);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfacePlate,
      PlanetPackageFormat::R8UInt, "surfacePlate", maps.surfacePlates);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceContinent,
      PlanetPackageFormat::R8UNorm, "surfaceContinent",
      maps.surfaceContinents);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceTectonics,
      PlanetPackageFormat::R8UNorm, "surfaceTectonics",
      maps.surfaceTectonics);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceMountain,
      PlanetPackageFormat::R8UNorm, "surfaceMountain",
      maps.surfaceMountains);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceTemperature,
      PlanetPackageFormat::R8UNorm, "surfaceTemperature",
      maps.surfaceTemperature);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfacePrecipitation,
      PlanetPackageFormat::R8UNorm, "surfacePrecip",
      maps.surfacePrecipitation);
  addChannelAndBlock(channels, blocks,
      PlanetGraphChannel::SurfaceRiver,
      PlanetPackageFormat::R8UNorm, "surfaceRiver", maps.surfaceRivers);
  return true;
}

uint32_t rasterChannel(const PlanetRasterChannel channel)
{
  return static_cast<uint32_t>(channel);
}

bool validRasterMaps(const SphericalTerrainMaps &maps)
{
  const size_t texelCount = static_cast<size_t>(maps.width) * maps.height;
  return maps.width != 0u && maps.height != 0u && maps.heightmap.size() == texelCount && maps.oceanFloorHeightmap.size() == texelCount && maps.waterHeightmap.size() == texelCount && maps.oceanMask.size() == texelCount && maps.lakeMask.size() == texelCount && maps.sourceRegions.size() == texelCount && maps.plates.size() == texelCount && maps.continents.size() == texelCount &&
         maps.tectonics.size() == texelCount && maps.mountains.size() == texelCount && maps.rivers.size() == texelCount && maps.temperature.size() == texelCount && maps.precipitation.size() == texelCount &&
         maps.wind.size() == texelCount && maps.windEast.size() == texelCount && maps.windNorth.size() == texelCount && maps.plateMotionEast.size() == texelCount && maps.plateMotionNorth.size() == texelCount &&
         maps.materials.size() == texelCount;
}

struct EquirectangularCoordinate
{
  uint32_t nearestX = 0u;
  uint32_t nearestY = 0u;
  uint32_t x0 = 0u;
  uint32_t x1 = 0u;
  uint32_t y0 = 0u;
  uint32_t y1 = 0u;
  double tx = 0.0;
  double ty = 0.0;
};

EquirectangularCoordinate equirectangularCoordinate(const SpherePoint &direction, const uint32_t width, const uint32_t height)
{
  constexpr double pi = 3.14159265358979323846264338327950288;
  EquirectangularCoordinate coordinate{};
  double u = std::atan2(direction.z, direction.x) / (2.0 * pi) + 0.5;
  u -= std::floor(u);
  const double v = std::clamp(0.5 - std::asin(std::clamp(direction.y, -1.0, 1.0)) / pi, 0.0, 1.0);
  coordinate.nearestX = static_cast<uint32_t>(std::floor(u * width)) % width;
  coordinate.nearestY = std::min(static_cast<uint32_t>(std::floor(v * height)), height - 1u);
  const double fx = u * width - 0.5;
  const double fy = std::clamp(v * height - 0.5, 0.0, static_cast<double>(height - 1u));
  const double floorX = std::floor(fx);
  int64_t signedX0 = static_cast<int64_t>(floorX) % static_cast<int64_t>(width);
  if (signedX0 < 0)
    signedX0 += width;
  coordinate.x0 = static_cast<uint32_t>(signedX0);
  coordinate.x1 = (coordinate.x0 + 1u) % width;
  coordinate.y0 = static_cast<uint32_t>(std::floor(fy));
  coordinate.y1 = std::min(coordinate.y0 + 1u, height - 1u);
  coordinate.tx = fx - floorX;
  coordinate.ty = fy - coordinate.y0;
  return coordinate;
}

template <typename Value> Value bilinearSample(const std::vector<Value> &values, const uint32_t width, const EquirectangularCoordinate &coordinate)
{
  const auto valueAt = [&values, width](const uint32_t x, const uint32_t y)
  {
    return static_cast<double>(values[static_cast<size_t>(y) * width + x]);
  };
  const double top = valueAt(coordinate.x0, coordinate.y0) + (valueAt(coordinate.x1, coordinate.y0) - valueAt(coordinate.x0, coordinate.y0)) * coordinate.tx;
  const double bottom = valueAt(coordinate.x0, coordinate.y1) + (valueAt(coordinate.x1, coordinate.y1) - valueAt(coordinate.x0, coordinate.y1)) * coordinate.tx;
  const double maximum = static_cast<double>(std::numeric_limits<Value>::max());
  return static_cast<Value>(std::lround(std::clamp(top + (bottom - top) * coordinate.ty, 0.0, maximum)));
}

template <typename Value> Value nearestSample(const std::vector<Value> &values, const uint32_t width, const EquirectangularCoordinate &coordinate)
{
  return values[static_cast<size_t>(coordinate.nearestY) * width + coordinate.nearestX];
}

void addRasterChannel(
    std::vector<PlanetPackageChannel> &channels,
    const PlanetRasterChannel channel,
    const PlanetPackageFormat format,
    const PlanetPackageMipPolicy mipPolicy,
    const std::string &name,
    const double scale = 1.0,
    const double bias = 0.0)
{
  channels.push_back({PlanetPackageDomain::Raster, rasterChannel(channel), format, mipPolicy, 0u, scale, bias, name});
}

void addRasterBlock(std::vector<PlanetPackageBlockInput> &blocks, const PlanetRasterChannel channel, const PlanetTileAddress &tile, std::vector<uint8_t> payload)
{
  blocks.push_back({planetRasterBlockKey(rasterChannel(channel), tile), std::move(payload)});
}

struct CubeTilePayloads
{
  std::vector<uint8_t> height;
  std::vector<uint8_t> oceanFloorHeight;
  std::vector<uint8_t> waterHeight;
  std::vector<uint8_t> oceanMask;
  std::vector<uint8_t> lakeMask;
  std::vector<uint8_t> material;
  std::vector<uint8_t> lithology;
  std::vector<uint8_t> sourceRegion;
  std::vector<uint8_t> river;
  std::vector<uint8_t> temperature;
  std::vector<uint8_t> precipitation;
  std::vector<uint8_t> windExposure;
  std::vector<uint8_t> wind;
  std::vector<uint8_t> plateMotion;
  std::vector<uint8_t> plate;
  std::vector<uint8_t> continent;
  std::vector<uint8_t> tectonics;
  std::vector<uint8_t> mountain;
  std::vector<uint8_t> flags;
};

CubeTilePayloads makeCubeTilePayloads(const size_t texelCount)
{
  CubeTilePayloads payloads{};
  payloads.height.reserve(texelCount * 2u);
  payloads.oceanFloorHeight.reserve(texelCount * 2u);
  payloads.waterHeight.reserve(texelCount * 2u);
  payloads.oceanMask.reserve(texelCount);
  payloads.lakeMask.reserve(texelCount);
  payloads.material.reserve(texelCount);
  payloads.lithology.reserve(texelCount);
  payloads.sourceRegion.reserve(texelCount * 4u);
  payloads.river.reserve(texelCount);
  payloads.temperature.reserve(texelCount);
  payloads.precipitation.reserve(texelCount);
  payloads.windExposure.reserve(texelCount);
  payloads.wind.reserve(texelCount * 2u);
  payloads.plateMotion.reserve(texelCount * 2u);
  payloads.plate.reserve(texelCount);
  payloads.continent.reserve(texelCount);
  payloads.tectonics.reserve(texelCount);
  payloads.mountain.reserve(texelCount);
  payloads.flags.reserve(texelCount);
  return payloads;
}

constexpr uint64_t kRasterBytesPerTexel = 26u;

} // namespace

bool buildSphericalTerrainGraphPackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    std::string *error)
{
  channels.clear();
  blocks.clear();
  if (error != nullptr)
    error->clear();
  const size_t nodeCount = maps.regionNodes.size();
  if (nodeCount == 0u || nodeCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "Spherical terrain graph has no representable region nodes");
  if (maps.regionNeighbourOffsets.size() != nodeCount + 1u || maps.regionNeighbourOffsets.front() != 0u || maps.regionNeighbourOffsets.back() != maps.regionNeighbourIndices.size())
    return fail(error, "Spherical terrain graph CSR offsets are invalid");
  for (size_t region = 0u; region < nodeCount; ++region)
    if (maps.regionNeighbourOffsets[region] > maps.regionNeighbourOffsets[region + 1u])
      return fail(error, "Spherical terrain graph CSR offsets are not monotonic");
  if (std::any_of(
          maps.regionNeighbourIndices.begin(),
          maps.regionNeighbourIndices.end(),
          [nodeCount](const uint32_t neighbour)
          {
            return neighbour >= nodeCount;
          }))
    return fail(error, "Spherical terrain graph contains an invalid neighbour ID");
  if (!(metadata.heightScaleMetres > 0.0) || !std::isfinite(metadata.heightScaleMetres) || !std::isfinite(metadata.heightBiasMetres))
    return fail(error, "Spherical terrain graph height encoding is invalid");

  std::vector<uint8_t> positions;
  positions.reserve(nodeCount * 3u * sizeof(float));
  std::vector<uint8_t> elevations;
  elevations.reserve(nodeCount * sizeof(uint16_t));
  std::vector<uint8_t> terrainElevations;
  terrainElevations.reserve(nodeCount * sizeof(uint16_t));
  std::vector<uint8_t> waterElevations;
  waterElevations.reserve(nodeCount * sizeof(uint16_t));
  std::vector<uint8_t> downstreamRegions;
  downstreamRegions.reserve(nodeCount * sizeof(uint32_t));
  std::vector<uint8_t> riverDischarges;
  riverDischarges.reserve(nodeCount * sizeof(float));
  std::vector<uint8_t> surfaceFlags;
  surfaceFlags.reserve(nodeCount);
  for (const SphericalTerrainNode &node : maps.regionNodes)
  {
    if (!std::isfinite(node.position.x) || !std::isfinite(node.position.y) || !std::isfinite(node.position.z))
      return fail(error, "Spherical terrain graph contains an invalid position");
    if ((node.downstreamRegion != SphericalTerrainNode::noRegion && node.downstreamRegion >= nodeCount) || !std::isfinite(node.riverDischarge) || node.riverDischarge < 0.0f)
      return fail(error, "Spherical terrain graph contains invalid drainage data");
    appendF32(positions, static_cast<float>(node.position.x));
    appendF32(positions, static_cast<float>(node.position.y));
    appendF32(positions, static_cast<float>(node.position.z));
    appendU16(elevations, node.elevation);
    appendU16(terrainElevations, node.terrainElevation);
    appendU16(waterElevations, node.waterElevation);
    appendU32(downstreamRegions, node.downstreamRegion);
    appendF32(riverDischarges, node.riverDischarge);
    uint8_t flags = 0u;
    if ((node.flags & SphericalTerrainNode::oceanFlag) != 0u)
      flags |= kPlanetSurfaceFlagWater | kPlanetSurfaceFlagOcean;
    if ((node.flags & SphericalTerrainNode::lakeFlag) != 0u)
      flags |= kPlanetSurfaceFlagWater | kPlanetSurfaceFlagLake;
    if (node.riverDischarge > 0.0f)
      flags |= kPlanetSurfaceFlagRiver;
    surfaceFlags.push_back(flags);
  }

  std::vector<uint8_t> neighbourOffsets;
  neighbourOffsets.reserve(maps.regionNeighbourOffsets.size() * sizeof(uint32_t));
  for (const uint32_t offset : maps.regionNeighbourOffsets)
    appendU32(neighbourOffsets, offset);
  std::vector<uint8_t> neighbours;
  neighbours.reserve(maps.regionNeighbourIndices.size() * sizeof(uint32_t));
  for (const uint32_t neighbour : maps.regionNeighbourIndices)
    appendU32(neighbours, neighbour);

  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Positions, PlanetPackageFormat::RGB32Float, "positions", std::move(positions));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::NeighbourOffsets, PlanetPackageFormat::R32UInt, "neighbourOffsets", std::move(neighbourOffsets));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Neighbours, PlanetPackageFormat::R32UInt, "neighbours", std::move(neighbours));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Elevation, PlanetPackageFormat::R16UInt, "elevation", std::move(elevations), metadata.heightScaleMetres, metadata.heightBiasMetres);
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::TerrainElevation, PlanetPackageFormat::R16UInt, "terrainElevation", std::move(terrainElevations), metadata.heightScaleMetres, metadata.heightBiasMetres);
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::WaterHeight, PlanetPackageFormat::R16UInt, "waterHeight", std::move(waterElevations), metadata.heightScaleMetres, metadata.heightBiasMetres);
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::DownstreamRegion, PlanetPackageFormat::R32UInt, "downstreamRegion", std::move(downstreamRegions));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Material, PlanetPackageFormat::R8UInt, "material", nodeBytes(maps.regionNodes, &SphericalTerrainNode::material));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Plate, PlanetPackageFormat::R8UInt, "plate", nodeBytes(maps.regionNodes, &SphericalTerrainNode::plate));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Continent, PlanetPackageFormat::R8UNorm, "continent", nodeBytes(maps.regionNodes, &SphericalTerrainNode::continent));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Tectonics, PlanetPackageFormat::R8UNorm, "tectonics", nodeBytes(maps.regionNodes, &SphericalTerrainNode::tectonics));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Mountain, PlanetPackageFormat::R8UNorm, "mountain", nodeBytes(maps.regionNodes, &SphericalTerrainNode::mountain));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::RiverDischarge, PlanetPackageFormat::R32Float, "riverDischarge", std::move(riverDischarges));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Temperature, PlanetPackageFormat::R8UNorm, "temperature", nodeBytes(maps.regionNodes, &SphericalTerrainNode::temperature));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Precipitation, PlanetPackageFormat::R8UNorm, "precipitation", nodeBytes(maps.regionNodes, &SphericalTerrainNode::precipitation));
  addChannelAndBlock(
      channels, blocks, PlanetGraphChannel::Wind, PlanetPackageFormat::RG8UNorm, "wind", nodePairBytes(maps.regionNodes, &SphericalTerrainNode::windEast, &SphericalTerrainNode::windNorth), 2.0 / 255.0, -1.0);
  addChannelAndBlock(
      channels,
      blocks,
      PlanetGraphChannel::PlateMotion,
      PlanetPackageFormat::RG8UNorm,
      "plateMotion",
      nodePairBytes(maps.regionNodes, &SphericalTerrainNode::plateMotionEast, &SphericalTerrainNode::plateMotionNorth),
      2.0 / 255.0,
      -1.0);
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::WindExposure, PlanetPackageFormat::R8UNorm, "windExposure", nodeBytes(maps.regionNodes, &SphericalTerrainNode::windExposure));
  addChannelAndBlock(channels, blocks, PlanetGraphChannel::Flags, PlanetPackageFormat::R8UInt, "flags", std::move(surfaceFlags));
  return appendSurfaceMesh(maps, metadata, channels, blocks, error);
}

bool buildSphericalTerrainCubePackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &settings,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    SphericalTerrainCubePackageStatistics *statistics,
    std::string *error)
{
  channels.clear();
  blocks.clear();
  if (error != nullptr)
    error->clear();
  if (!validRasterMaps(maps))
    return fail(error, "Spherical terrain raster maps are missing or inconsistent");
  if (maps.regionNodes.empty() || std::any_of(
                                      maps.sourceRegions.begin(),
                                      maps.sourceRegions.end(),
                                      [&maps](const uint32_t region)
                                      {
                                        return region >= maps.regionNodes.size();
                                      }))
    return fail(error, "Spherical terrain source-region map does not reference the authoritative graph");
  if (metadata.tileSizePixels == 0u || metadata.maximumTileLevel > 31u || settings.minimumTileLevel > metadata.maximumTileLevel || !(metadata.heightScaleMetres > 0.0) || !std::isfinite(metadata.heightScaleMetres) ||
      !std::isfinite(metadata.heightBiasMetres))
    return fail(error, "Spherical terrain cube package settings are invalid");

  const uint64_t storedSize = static_cast<uint64_t>(metadata.tileSizePixels) + static_cast<uint64_t>(metadata.gutterPixels) * 2u;
  if (storedSize > std::numeric_limits<uint32_t>::max() || storedSize * storedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) / 4u)
    return fail(error, "Spherical terrain cube tile exceeds the package block limit");
  const uint64_t texelsPerTile = storedSize * storedSize;
  uint64_t tileCount = 0u;
  for (uint32_t level = settings.minimumTileLevel; level <= metadata.maximumTileLevel; ++level)
  {
    const uint64_t dimension = uint64_t{1u} << level;
    if (dimension > std::numeric_limits<uint64_t>::max() / dimension / kPlanetPackageCubeFaceCount)
      return fail(error, "Spherical terrain cube tile count overflow");
    const uint64_t levelTiles = dimension * dimension * kPlanetPackageCubeFaceCount;
    if (tileCount > std::numeric_limits<uint64_t>::max() - levelTiles)
      return fail(error, "Spherical terrain cube tile count overflow");
    tileCount += levelTiles;
  }
  if (tileCount > std::numeric_limits<uint32_t>::max() / 18u || texelsPerTile > std::numeric_limits<uint64_t>::max() / std::max<uint64_t>(1u, tileCount) / kRasterBytesPerTexel)
    return fail(error, "Spherical terrain cube package exceeds directory or size limits");
  const uint64_t uncompressedBytes = tileCount * texelsPerTile * kRasterBytesPerTexel;
  if (settings.maximumUncompressedBytes != 0u && uncompressedBytes > settings.maximumUncompressedBytes)
    return fail(error, "Spherical terrain cube package exceeds maximumUncompressedBytes");

  addRasterChannel(channels, PlanetRasterChannel::Height, PlanetPackageFormat::R16UInt, PlanetPackageMipPolicy::Average, "height", metadata.heightScaleMetres, metadata.heightBiasMetres);
  addRasterChannel(channels, PlanetRasterChannel::WaterHeight, PlanetPackageFormat::R16UInt, PlanetPackageMipPolicy::Nearest, "waterHeight", metadata.heightScaleMetres, metadata.heightBiasMetres);
  addRasterChannel(channels, PlanetRasterChannel::Material, PlanetPackageFormat::R8UInt, PlanetPackageMipPolicy::Mode, "material");
  addRasterChannel(channels, PlanetRasterChannel::Lithology, PlanetPackageFormat::R8UInt, PlanetPackageMipPolicy::Mode, "lithology");
  addRasterChannel(channels, PlanetRasterChannel::SourceRegion, PlanetPackageFormat::R32UInt, PlanetPackageMipPolicy::Nearest, "sourceRegion");
  addRasterChannel(channels, PlanetRasterChannel::River, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Maximum, "river");
  addRasterChannel(channels, PlanetRasterChannel::Temperature, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Average, "temperature");
  addRasterChannel(channels, PlanetRasterChannel::Precipitation, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Average, "precipitation");
  addRasterChannel(channels, PlanetRasterChannel::Wind, PlanetPackageFormat::RG8UNorm, PlanetPackageMipPolicy::NormalizeAverage, "wind", 2.0 / 255.0, -1.0);
  addRasterChannel(channels, PlanetRasterChannel::PlateMotion, PlanetPackageFormat::RG8UNorm, PlanetPackageMipPolicy::NormalizeAverage, "plateMotion", 2.0 / 255.0, -1.0);
  addRasterChannel(channels, PlanetRasterChannel::Plate, PlanetPackageFormat::R8UInt, PlanetPackageMipPolicy::Nearest, "plate");
  addRasterChannel(channels, PlanetRasterChannel::Continent, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Average, "continent");
  addRasterChannel(channels, PlanetRasterChannel::Tectonics, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Average, "tectonics");
  addRasterChannel(channels, PlanetRasterChannel::Mountain, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Maximum, "mountain");
  addRasterChannel(channels, PlanetRasterChannel::Flags, PlanetPackageFormat::R8UInt, PlanetPackageMipPolicy::Nearest, "flags");
  addRasterChannel(channels, PlanetRasterChannel::WindExposure, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Average, "windExposure");
  addRasterChannel(channels, PlanetRasterChannel::OceanMask, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Nearest, "oceanMask");
  addRasterChannel(channels, PlanetRasterChannel::LakeMask, PlanetPackageFormat::R8UNorm, PlanetPackageMipPolicy::Nearest, "lakeMask");
  addRasterChannel(channels, PlanetRasterChannel::OceanFloorHeight, PlanetPackageFormat::R16UInt, PlanetPackageMipPolicy::Average, "oceanFloorHeight", -metadata.heightScaleMetres, 0.0);
  blocks.reserve(static_cast<size_t>(tileCount) * channels.size());

  // Planet radius is the ocean datum. Encode that explicit zero-metre water
  // surface through the channel transform instead of assuming raw zero means
  // zero metres (it does not when heightBiasMetres is non-zero).
  const double encodedOceanLevel =
      (0.0 - metadata.heightBiasMetres) / metadata.heightScaleMetres;
  if (!std::isfinite(encodedOceanLevel) || encodedOceanLevel < 0.0 ||
      encodedOceanLevel > 65535.0)
    return fail(error, "Zero-metre ocean level is outside the package height encoding");
  const uint16_t oceanLevel =
      static_cast<uint16_t>(std::lround(encodedOceanLevel));

  for (uint32_t level = settings.minimumTileLevel; level <= metadata.maximumTileLevel; ++level)
  {
    const uint32_t dimension = uint32_t{1u} << level;
    for (uint8_t face = 0u; face < kPlanetPackageCubeFaceCount; ++face)
      for (uint32_t tileY = 0u; tileY < dimension; ++tileY)
        for (uint32_t tileX = 0u; tileX < dimension; ++tileX)
        {
          const PlanetTileAddress tile{face, static_cast<uint8_t>(level), tileX, tileY};
          CubeTilePayloads payloads = makeCubeTilePayloads(static_cast<size_t>(texelsPerTile));
          uint16_t minimumHeight = std::numeric_limits<uint16_t>::max();
          uint16_t maximumHeight = 0u;
          for (uint32_t storedY = 0u; storedY < storedSize; ++storedY)
            for (uint32_t storedX = 0u; storedX < storedSize; ++storedX)
            {
              SpherePoint direction{};
              if (!planetCubeTileTexelDirection(tile, metadata.tileSizePixels, metadata.gutterPixels, storedX, storedY, direction))
                return fail(error, "Could not evaluate a cube tile texel direction");
              const EquirectangularCoordinate coordinate = equirectangularCoordinate(direction, maps.width, maps.height);
              const uint8_t material = nearestSample(maps.materials, maps.width, coordinate);
              const uint32_t sourceRegion = nearestSample(maps.sourceRegions, maps.width, coordinate);
              const uint8_t river = bilinearSample(maps.rivers, maps.width, coordinate);
              const bool ocean = nearestSample(maps.oceanMask, maps.width,
                  coordinate) != 0u;
              const bool lake = nearestSample(maps.lakeMask, maps.width,
                  coordinate) != 0u;
              // Height is a solid-terrain field. Ocean has no emitted terrain
              // sample even when bilinear filtering reaches across a coast.
              const uint16_t height = ocean ? 0u :
                  bilinearSample(maps.heightmap, maps.width, coordinate);
              const uint16_t oceanFloorHeight = ocean ?
                  bilinearSample(maps.oceanFloorHeightmap, maps.width,
                      coordinate) :
                  0u;
              const uint16_t waterHeight = ocean ? oceanLevel :
                  (lake ? nearestSample(maps.waterHeightmap, maps.width,
                              coordinate) :
                          0u);
              uint8_t flags = 0u;
              if (ocean || lake)
                flags |= kPlanetSurfaceFlagWater;
              if (ocean)
                flags |= kPlanetSurfaceFlagOcean;
              if (lake)
                flags |= kPlanetSurfaceFlagLake;
              if (river != 0u)
                flags |= kPlanetSurfaceFlagRiver;

              appendU16(payloads.height, height);
              appendU16(payloads.oceanFloorHeight, oceanFloorHeight);
              appendU16(payloads.waterHeight, waterHeight);
              payloads.oceanMask.push_back(ocean ? 255u : 0u);
              payloads.lakeMask.push_back(lake ? 255u : 0u);
              payloads.material.push_back(material);
              // Nearest, like every other class index: an interpolated rock type
              // is a rock type that does not exist.
              payloads.lithology.push_back(
                  maps.lithology.size() == maps.materials.size() ? nearestSample(maps.lithology, maps.width, coordinate) : 0u);
              appendU32(payloads.sourceRegion, sourceRegion);
              payloads.river.push_back(river);
              payloads.temperature.push_back(bilinearSample(maps.temperature, maps.width, coordinate));
              payloads.precipitation.push_back(bilinearSample(maps.precipitation, maps.width, coordinate));
              payloads.windExposure.push_back(bilinearSample(maps.wind, maps.width, coordinate));
              payloads.wind.push_back(bilinearSample(maps.windEast, maps.width, coordinate));
              payloads.wind.push_back(bilinearSample(maps.windNorth, maps.width, coordinate));
              payloads.plateMotion.push_back(bilinearSample(maps.plateMotionEast, maps.width, coordinate));
              payloads.plateMotion.push_back(bilinearSample(maps.plateMotionNorth, maps.width, coordinate));
              payloads.plate.push_back(nearestSample(maps.plates, maps.width, coordinate));
              payloads.continent.push_back(bilinearSample(maps.continents, maps.width, coordinate));
              payloads.tectonics.push_back(bilinearSample(maps.tectonics, maps.width, coordinate));
              payloads.mountain.push_back(bilinearSample(maps.mountains, maps.width, coordinate));
              payloads.flags.push_back(flags);
              minimumHeight = std::min(minimumHeight, height);
              maximumHeight = std::max(maximumHeight, height);
            }

          PlanetPackageBlockInput heightBlock{};
          heightBlock.key = planetRasterBlockKey(rasterChannel(PlanetRasterChannel::Height), tile);
          heightBlock.payload = std::move(payloads.height);
          heightBlock.hasValueRange = true;
          heightBlock.minimumValue = metadata.heightBiasMetres + minimumHeight * metadata.heightScaleMetres;
          heightBlock.maximumValue = metadata.heightBiasMetres + maximumHeight * metadata.heightScaleMetres;
          blocks.push_back(std::move(heightBlock));
          addRasterBlock(blocks, PlanetRasterChannel::WaterHeight, tile, std::move(payloads.waterHeight));
          addRasterBlock(blocks, PlanetRasterChannel::Material, tile, std::move(payloads.material));
          addRasterBlock(blocks, PlanetRasterChannel::Lithology, tile, std::move(payloads.lithology));
          addRasterBlock(blocks, PlanetRasterChannel::SourceRegion, tile, std::move(payloads.sourceRegion));
          addRasterBlock(blocks, PlanetRasterChannel::River, tile, std::move(payloads.river));
          addRasterBlock(blocks, PlanetRasterChannel::Temperature, tile, std::move(payloads.temperature));
          addRasterBlock(blocks, PlanetRasterChannel::Precipitation, tile, std::move(payloads.precipitation));
          addRasterBlock(blocks, PlanetRasterChannel::WindExposure, tile, std::move(payloads.windExposure));
          addRasterBlock(blocks, PlanetRasterChannel::Wind, tile, std::move(payloads.wind));
          addRasterBlock(blocks, PlanetRasterChannel::PlateMotion, tile, std::move(payloads.plateMotion));
          addRasterBlock(blocks, PlanetRasterChannel::Plate, tile, std::move(payloads.plate));
          addRasterBlock(blocks, PlanetRasterChannel::Continent, tile, std::move(payloads.continent));
          addRasterBlock(blocks, PlanetRasterChannel::Tectonics, tile, std::move(payloads.tectonics));
          addRasterBlock(blocks, PlanetRasterChannel::Mountain, tile, std::move(payloads.mountain));
          addRasterBlock(blocks, PlanetRasterChannel::Flags, tile, std::move(payloads.flags));
          addRasterBlock(blocks, PlanetRasterChannel::OceanMask, tile, std::move(payloads.oceanMask));
          addRasterBlock(blocks, PlanetRasterChannel::LakeMask, tile, std::move(payloads.lakeMask));
          addRasterBlock(blocks, PlanetRasterChannel::OceanFloorHeight, tile, std::move(payloads.oceanFloorHeight));
        }
  }

  if (statistics != nullptr)
  {
    statistics->tileCount = tileCount;
    statistics->storedTexelCount = tileCount * texelsPerTile;
    statistics->uncompressedBytes = uncompressedBytes;
    statistics->channelCount = static_cast<uint32_t>(channels.size());
    statistics->blockCount = static_cast<uint32_t>(blocks.size());
  }
  return true;
}

bool buildSphericalTerrainPackage(
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &settings,
    std::vector<PlanetPackageChannel> &channels,
    std::vector<PlanetPackageBlockInput> &blocks,
    SphericalTerrainCubePackageStatistics *statistics,
    std::string *error)
{
  std::vector<PlanetPackageChannel> graphChannels;
  std::vector<PlanetPackageBlockInput> graphBlocks;
  std::vector<PlanetPackageChannel> rasterChannels;
  std::vector<PlanetPackageBlockInput> rasterBlocks;
  if (!buildSphericalTerrainGraphPackage(maps, metadata, graphChannels, graphBlocks, error) || !buildSphericalTerrainCubePackage(maps, metadata, settings, rasterChannels, rasterBlocks, statistics, error))
    return false;
  channels = std::move(graphChannels);
  channels.insert(channels.end(), std::make_move_iterator(rasterChannels.begin()), std::make_move_iterator(rasterChannels.end()));
  blocks = std::move(graphBlocks);
  blocks.insert(blocks.end(), std::make_move_iterator(rasterBlocks.begin()), std::make_move_iterator(rasterBlocks.end()));
  return true;
}

bool writeSphericalTerrainGraphPackage(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const PlanetPackageWriteOptions &options,
    PlanetPackageStatistics *statistics,
    std::string *error)
{
  std::vector<PlanetPackageChannel> channels;
  std::vector<PlanetPackageBlockInput> blocks;
  return buildSphericalTerrainGraphPackage(maps, metadata, channels, blocks, error) && writePlanetPackage(path, metadata, channels, blocks, options, statistics, error);
}

bool writeSphericalTerrainPackage(
    const std::filesystem::path &path,
    const SphericalTerrainMaps &maps,
    const PlanetPackageMetadata &metadata,
    const SphericalTerrainCubePackageSettings &cubeSettings,
    const PlanetPackageWriteOptions &writeOptions,
    PlanetPackageStatistics *statistics,
    SphericalTerrainCubePackageStatistics *cubeStatistics,
    std::string *error)
{
  std::vector<PlanetPackageChannel> channels;
  std::vector<PlanetPackageBlockInput> blocks;
  return buildSphericalTerrainPackage(maps, metadata, cubeSettings, channels, blocks, cubeStatistics, error) && writePlanetPackage(path, metadata, channels, blocks, writeOptions, statistics, error);
}

} // namespace procedural
