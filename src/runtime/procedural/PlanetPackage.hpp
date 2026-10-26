#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace procedural
{

constexpr uint32_t kPlanetPackageVersion = 1u;
constexpr uint32_t kPlanetPackageEndianMarker = 0x01020304u;
constexpr uint8_t kPlanetPackageCubeFaceCount = 6u;
constexpr uint8_t kPlanetPackageGraphFace = 0xffu;

enum class PlanetPackageDomain : uint32_t
{
  Raster = 1u,
  Graph = 2u,
};

enum class PlanetPackageCompression : uint32_t
{
  None = 0u,
  LZ4 = 1u,
};

enum class PlanetPackageFormat : uint32_t
{
  Unknown = 0u,
  R8UInt = 1u,
  R8UNorm = 2u,
  RG8SNorm = 3u,
  R16UInt = 4u,
  R16SInt = 5u,
  R32UInt = 6u,
  R32SInt = 7u,
  R32Float = 8u,
  RGB32Float = 9u,
  RG8UInt = 10u,
  RG8UNorm = 11u,
};

enum class PlanetPackageMipPolicy : uint32_t
{
  None = 0u,
  Average = 1u,
  Minimum = 2u,
  Maximum = 3u,
  Mode = 4u,
  Nearest = 5u,
  NormalizeAverage = 6u,
};

enum class PlanetRasterChannel : uint32_t
{
  Height = 1u,
  WaterHeight = 2u,
  Material = 3u,
  SourceRegion = 4u,
  River = 5u,
  Temperature = 6u,
  Precipitation = 7u,
  Wind = 8u,
  PlateMotion = 9u,
  Plate = 10u,
  Continent = 11u,
  Tectonics = 12u,
  Mountain = 13u,
  Flags = 14u,
  WindExposure = 15u,
  // Explicit water occupancy. Height contains solid terrain only;
  // WaterHeight is meaningful where one of these masks is non-zero.
  OceanMask = 16u,
  LakeMask = 17u,
  // Solid seabed elevation below the zero-metre ocean datum. Stored as an
  // unsigned depth with a negative channel scale; zero means no ocean floor.
  OceanFloorHeight = 18u,
  // PlanetLithology of the bedrock exposed at the surface. Substrate, not
  // cover: Material says what grows here, Lithology says what it grew on and
  // what a dug column runs into.
  Lithology = 19u,
};

enum class PlanetGraphChannel : uint32_t
{
  Positions = 1u,
  NeighbourOffsets = 2u,
  Neighbours = 3u,
  Elevation = 4u,
  Material = 5u,
  Plate = 6u,
  Continent = 7u,
  Tectonics = 8u,
  Mountain = 9u,
  RiverDischarge = 10u,
  DownstreamRegion = 11u,
  WaterHeight = 12u,
  Temperature = 13u,
  Precipitation = 14u,
  Wind = 15u,
  PlateMotion = 16u,
  Flags = 17u,
  WindExposure = 18u,
  TerrainElevation = 19u,
  // Reusable displaced dual mesh. Directions are unit vectors; consumers
  // reconstruct planet-centred positions as
  // direction * (planetRadiusMetres + decoded surface elevation).
  SurfaceDirections = 20u,
  SurfaceElevation = 21u,
  SurfaceIndices = 22u,
  // One stable Voronoi region and source Delaunay triangle per render face.
  SurfaceFaceRegion = 23u,
  SurfaceFaceTriangle = 24u,
  // Three stable Voronoi region IDs per source Delaunay triangle.
  TriangleRegions = 25u,
  SurfaceMaterial = 26u,
  SurfacePlate = 27u,
  SurfaceContinent = 28u,
  SurfaceTectonics = 29u,
  SurfaceMountain = 30u,
  SurfaceTemperature = 31u,
  SurfacePrecipitation = 32u,
  SurfaceRiver = 33u,
};

constexpr uint8_t kPlanetSurfaceFlagWater = 1u << 0u;
constexpr uint8_t kPlanetSurfaceFlagOcean = 1u << 1u;
constexpr uint8_t kPlanetSurfaceFlagLake = 1u << 2u;
constexpr uint8_t kPlanetSurfaceFlagRiver = 1u << 3u;

struct PlanetTileAddress
{
  uint8_t face = 0u;
  uint8_t level = 0u;
  uint32_t x = 0u;
  uint32_t y = 0u;
};

bool operator==(const PlanetTileAddress &a, const PlanetTileAddress &b);
bool validPlanetTileAddress(const PlanetTileAddress &address);
PlanetTileAddress planetTileChild(const PlanetTileAddress &parent, uint32_t child);
PlanetTileAddress planetTileParent(const PlanetTileAddress &child);
uint64_t planetTileMorton(const PlanetTileAddress &address);

// Returns the first cube-quadtree level whose nominal great-circle edge is no
// larger than targetEdgeMetres. Actual edge lengths vary slightly over a cube
// projection, so render refinement should still use measured patch bounds.
uint8_t planetTileLevelForTargetEdge(double planetRadiusMetres, double targetEdgeMetres);
double planetTileNominalEdgeMetres(double planetRadiusMetres, uint8_t level);

struct PlanetPackageMetadata
{
  uint64_t contentHash = 0u;
  uint64_t buildHash = 0u;
  // Radius of the zero-elevation ocean surface. Terrain and water channel
  // values are signed offsets from this datum after scale/bias decoding.
  double planetRadiusMetres = 0.0;
  double heightScaleMetres = 1.0;
  double heightBiasMetres = 0.0;
  double nominalRenderPatchEdgeMetres = 1000.0;
  uint32_t tileSizePixels = 256u;
  uint16_t gutterPixels = 1u;
  uint8_t maximumTileLevel = 0u;
};

double planetRadiusKilometres(const PlanetPackageMetadata &metadata);
double planetDiameterKilometres(const PlanetPackageMetadata &metadata);
double planetCircumferenceKilometres(const PlanetPackageMetadata &metadata);
double planetSurfaceAreaSquareKilometres(
    const PlanetPackageMetadata &metadata);

struct PlanetPackageChannel
{
  PlanetPackageDomain domain = PlanetPackageDomain::Raster;
  uint32_t id = 0u;
  PlanetPackageFormat format = PlanetPackageFormat::Unknown;
  PlanetPackageMipPolicy mipPolicy = PlanetPackageMipPolicy::None;
  uint32_t flags = 0u;
  double scale = 1.0;
  double bias = 0.0;
  std::string name;
};

struct PlanetPackageBlockKey
{
  PlanetPackageDomain domain = PlanetPackageDomain::Raster;
  uint32_t channelId = 0u;
  PlanetTileAddress tile{};
};

bool operator==(const PlanetPackageBlockKey &a, const PlanetPackageBlockKey &b);
bool operator<(const PlanetPackageBlockKey &a, const PlanetPackageBlockKey &b);

PlanetPackageBlockKey planetRasterBlockKey(uint32_t channelId, const PlanetTileAddress &tile);
PlanetPackageBlockKey planetGraphBlockKey(uint32_t channelId);

struct PlanetPackageBlockInput
{
  PlanetPackageBlockKey key{};
  std::vector<uint8_t> payload;
  bool hasValueRange = false;
  double minimumValue = 0.0;
  double maximumValue = 0.0;
};

struct PlanetPackageBlockRecord
{
  PlanetPackageBlockKey key{};
  PlanetPackageCompression compression = PlanetPackageCompression::None;
  uint64_t fileOffset = 0u;
  uint64_t storedSize = 0u;
  uint64_t uncompressedSize = 0u;
  uint64_t checksum = 0u;
  bool hasValueRange = false;
  double minimumValue = 0.0;
  double maximumValue = 0.0;
};

struct PlanetPackageWriteOptions
{
  PlanetPackageCompression compression = PlanetPackageCompression::LZ4;
  bool highCompression = true;
  uint32_t minimumCompressionSavingsBytes = 16u;
};

struct PlanetPackageStatistics
{
  uint32_t channelCount = 0u;
  uint32_t blockCount = 0u;
  uint32_t compressedBlockCount = 0u;
  uint64_t uncompressedPayloadBytes = 0u;
  uint64_t storedPayloadBytes = 0u;
  uint64_t fileBytes = 0u;
};

uint64_t planetPackageChecksum(const uint8_t *data, size_t size);

bool writePlanetPackage(
    const std::filesystem::path &path,
    const PlanetPackageMetadata &metadata,
    const std::vector<PlanetPackageChannel> &channels,
    const std::vector<PlanetPackageBlockInput> &blocks,
    const PlanetPackageWriteOptions &options = {},
    PlanetPackageStatistics *statistics = nullptr,
    std::string *error = nullptr);

class PlanetPackageReader
{
public:
  bool open(const std::filesystem::path &path, std::string *error = nullptr);
  void close();
  bool isOpen() const
  {
    return !path_.empty();
  }

  const PlanetPackageMetadata &metadata() const
  {
    return metadata_;
  }
  const std::vector<PlanetPackageChannel> &channels() const
  {
    return channels_;
  }
  const std::vector<PlanetPackageBlockRecord> &blocks() const
  {
    return blocks_;
  }

  const PlanetPackageChannel *findChannel(PlanetPackageDomain domain, uint32_t id) const;
  const PlanetPackageBlockRecord *findBlock(const PlanetPackageBlockKey &key) const;
  // Finds the requested raster tile or its closest stored quadtree ancestor.
  // This lets 1 km render chunks refine beyond the source-map resolution while
  // still sampling the best available package data.
  const PlanetPackageBlockRecord *findBestRasterBlock(uint32_t channelId, PlanetTileAddress tile) const;
  bool readBlock(const PlanetPackageBlockKey &key, std::vector<uint8_t> &payload, std::string *error = nullptr) const;
  bool readBlock(const PlanetPackageBlockRecord &record, std::vector<uint8_t> &payload, std::string *error = nullptr) const;

private:
  std::filesystem::path path_;
  PlanetPackageMetadata metadata_{};
  std::vector<PlanetPackageChannel> channels_;
  std::vector<PlanetPackageBlockRecord> blocks_;
};

} // namespace procedural
