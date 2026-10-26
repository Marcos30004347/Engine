#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <vector>

namespace voxel::svraster
{

// City scene index v1. All disk integers are little endian and explicitly
// encoded; these C++ records are an intermediate representation, not a disk ABI.
constexpr uint16_t kCityVoxelSceneMajor = 1;
constexpr uint16_t kCityVoxelSceneMinor = 1;
// Local coordinates use a per-cell millimetre origin and a 16-bit turn.
constexpr uint8_t kCityVoxelInstanceLocalFrame = 1u;
constexpr uint32_t kCityVoxelInvalidPage = 0xffffffffu;
constexpr uint32_t kCityVoxelFeatureSharedGeometry = 1u << 0;
constexpr uint32_t kCityVoxelFeatureTracePages = 1u << 1;
constexpr uint32_t kCityVoxelFeatureVirtualGeometry = 1u << 2;
constexpr uint32_t kCityVoxelSupportedFeatures =
    kCityVoxelFeatureSharedGeometry | kCityVoxelFeatureTracePages |
    kCityVoxelFeatureVirtualGeometry;

enum class CityVoxelPageKind : uint16_t
{
  Geometry = 1,
  Appearance = 2,
  Trace = 3,
  Fallback = 4,
  VirtualGeometry = 5,
  // A trained LZ4 prefix for the LZ4 pages of one kind (its assetId).
  Dictionary = 6,
};

enum class CityVoxelCodec : uint16_t
{
  Raw = 0,
  LZ4 = 1,
  Palette8 = 2,
  Palette16 = 3,
  SparseOverride = 4,
};

struct CityVoxelGeometryAsset
{
  uint64_t id = 0;
  uint64_t contentHash = 0;
  uint64_t voxelAddressHash = 0;
  uint64_t connectorHash = 0;
  uint32_t rootPage = kCityVoxelInvalidPage;
  uint32_t firstPage = 0;
  uint32_t pageCount = 0;
  uint32_t pitchMicrometres = 100000;
  std::array<int32_t, 3> minimumCell{};
  std::array<int32_t, 3> maximumCell{}; // Exclusive bounds.
  uint32_t flags = 0;
};

struct CityVoxelAppearanceAsset
{
  uint64_t id = 0;
  uint64_t contentHash = 0;
  uint64_t geometryAddressHash = 0;
  uint64_t geometryId = 0;
  uint64_t baseAppearanceId = 0; // Zero means no base.
  uint32_t rootPage = kCityVoxelInvalidPage;
  uint32_t firstPage = 0;
  uint32_t pageCount = 0;
  CityVoxelCodec codec = CityVoxelCodec::Raw;
  uint16_t flags = 0;
};

struct CityVoxelInstance
{
  uint64_t stableId = 0;
  uint64_t geometryId = 0;
  uint64_t appearanceId = 0;
  uint64_t ownerId = 0;
  uint32_t spatialCellId = 0;
  std::array<int32_t, 3> gridPosition{};
  uint8_t quarterTurn = 0;
  uint8_t flags = 0;
  uint16_t frameId = 0;
};

struct CityVoxelSpatialCell
{
  uint64_t id = 0;
  std::array<int64_t, 3> originMillimetres{};
  uint32_t firstInstance = 0;
  uint32_t instanceCount = 0;
  uint32_t fallbackPage = kCityVoxelInvalidPage;
  uint32_t flags = 0;
};

struct CityVoxelPageRecord
{
  uint32_t id = 0;
  CityVoxelPageKind kind = CityVoxelPageKind::Geometry;
  CityVoxelCodec codec = CityVoxelCodec::Raw;
  uint64_t assetId = 0;
  uint64_t fileOffset = 0; // Zero until page payloads are installed.
  uint32_t storedBytes = 0;
  uint32_t decodedBytes = 0;
  uint64_t checksum = 0;
  uint32_t firstDependency = 0;
  uint32_t dependencyCount = 0;
  uint32_t flags = 0;
};

struct CityVoxelMaterialReference
{
  uint32_t profileId = 0;
  uint32_t flags = 0;
  uint64_t assetHash = 0;
};

struct CityVoxelSceneIndex
{
  uint64_t sourceHash = 0;
  uint64_t settingsHash = 0;
  std::array<int64_t, 3> cityOriginMillimetres{};
  uint32_t nominalPitchMicrometres = 100000;
  uint32_t requiredFeatures = 0;
  std::vector<CityVoxelGeometryAsset> geometry;
  std::vector<CityVoxelAppearanceAsset> appearances;
  std::vector<CityVoxelInstance> instances;
  std::vector<CityVoxelSpatialCell> spatialCells;
  std::vector<CityVoxelPageRecord> pages;
  std::vector<uint32_t> dependencies;
  std::vector<CityVoxelMaterialReference> materials;
};

struct CityVoxelSceneLimits
{
  uint64_t maximumIndexBytes = 256ull * 1024ull * 1024ull;
  uint32_t maximumGeometry = 1000000u;
  uint32_t maximumAppearances = 1000000u;
  uint32_t maximumInstances = 4000000u;
  uint32_t maximumSpatialCells = 1000000u;
  uint32_t maximumPages = 4000000u;
  uint32_t maximumDependencies = 16000000u;
  uint32_t maximumMaterials = 65536u;
  uint32_t maximumStoredPageBytes = 64u * 1024u * 1024u;
  uint32_t maximumDecodedPageBytes = 256u * 1024u * 1024u;
  uint64_t maximumFileBytes = 64ull * 1024ull * 1024ull * 1024ull;
};

bool validateCityVoxelSceneIndex(const CityVoxelSceneIndex &, const CityVoxelSceneLimits &, std::string *error = nullptr);
bool serializeCityVoxelSceneIndex(const CityVoxelSceneIndex &, const CityVoxelSceneLimits &, std::vector<uint8_t> &, std::string *error = nullptr);
bool deserializeCityVoxelSceneIndex(const uint8_t *, uint64_t, const CityVoxelSceneLimits &, CityVoxelSceneIndex &, std::string *error = nullptr);
bool writeCityVoxelSceneIndex(const std::string &, const CityVoxelSceneIndex &, const CityVoxelSceneLimits &, std::string *error = nullptr);
bool readCityVoxelSceneIndex(const std::string &, const CityVoxelSceneLimits &, CityVoxelSceneIndex &, std::string *error = nullptr);

// The producer supplies one stored page at a time. The writer never retains
// more than one payload and verifies the temporary file before publishing it.
using CityVoxelPageProducer = std::function<bool(uint32_t, std::vector<uint8_t> &, std::string *)>;
// Compression writes the pages raw first, trains one dictionary per page
// kind from a sample of them, then rewrites every page that shrinks as an
// LZ4 block against its kind's dictionary. Readers decode transparently.
struct CityVoxelSceneCompression
{
  bool enabled = false;
  uint32_t dictionaryBytes = 65536u;
  uint32_t samplesPerKind = 2048u;
  int level = 9; // LZ4HC
  uint32_t workers = 1u;
};
bool writeCityVoxelSceneFile(const std::string &, CityVoxelSceneIndex,
                             const CityVoxelSceneLimits &, const CityVoxelPageProducer &,
                             std::string *error = nullptr,
                             const CityVoxelSceneCompression &compression = {});

class CityVoxelSceneFileReader
{
public:
  bool open(const std::string &, const CityVoxelSceneLimits &, std::string *error = nullptr);
  // Decoded page bytes, whatever the stored codec.
  bool readPage(uint32_t, std::vector<uint8_t> &, std::string *error = nullptr) const;
  // Stored bytes as written, before decoding.
  bool readStoredPage(uint32_t, std::vector<uint8_t> &, std::string *error = nullptr) const;
  const CityVoxelSceneIndex &index() const { return index_; }
  uint64_t fileBytes() const { return fileBytes_; }
  uint64_t indexBytes() const { return indexBytes_; }
private:
  std::string path_;
  CityVoxelSceneIndex index_;
  CityVoxelSceneLimits limits_;
  uint64_t fileBytes_ = 0;
  uint64_t indexBytes_ = 0;
  std::vector<std::vector<uint8_t>> dictionaries_; // By page kind.
  // One descriptor for the reader's life; pages are read with pread so
  // threads can share it. Opening the file per page cost 11 s of an export.
  std::shared_ptr<int> descriptor_;
};

} // namespace voxel::svraster
