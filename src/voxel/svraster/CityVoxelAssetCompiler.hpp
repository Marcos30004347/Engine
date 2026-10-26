#pragma once

#include "voxel/svraster/CityVoxelSceneFormat.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster {

// The compiler owns only one part's uncompressed cells at a time. Coordinates
// are integer cells in the part's authored lattice; RGBA is byte-exact and the
// profile is a stable shared material ID, never a palette or shader index.
struct CityVoxelCell {
  std::array<int32_t, 3> position{};
  uint32_t rgba = 0;
  uint32_t materialProfile = 0;
};

struct CityVoxelAddress {
  uint32_t brick = 0;
  uint16_t rank = 0;
};

struct CityVoxelCompiledGeometry {
  uint64_t id = 0;
  uint64_t contentHash = 0;
  uint64_t addressHash = 0;
  uint32_t pitchMicrometres = 100000;
  std::array<int32_t, 3> extent{}; // Exclusive, relative to minimum input cell.
  uint32_t occupiedCount = 0;
  std::vector<uint8_t> bytes; // Canonical sparse brick stream.
};

struct CityVoxelCompiledAppearance {
  uint64_t id = 0;
  uint64_t contentHash = 0;
  uint64_t geometryAddressHash = 0;
  uint64_t geometryId = 0;
  CityVoxelCodec codec = CityVoxelCodec::Raw;
  uint32_t occupiedCount = 0;
  std::vector<uint8_t> bytes; // Exact, ordered by (brick, occupancy rank).
};

struct CityVoxelCompiledPlacement {
  uint64_t stableId = 0;
  uint64_t geometryId = 0;
  uint64_t appearanceId = 0;
  std::array<int32_t, 3> originCell{};
};

struct CityVoxelCompilerLimits {
  uint32_t maximumPartCells = 1'000'000;
  uint64_t maximumScratchBytes = 128ull * 1024ull * 1024ull;
  uint32_t maximumBricks = 100'000;
};

enum CityVoxelGeometryCause : uint8_t {
  CityVoxelCanonicalShape = 0,
  CityVoxelBakedSubcellOffset = 1u << 0,
  CityVoxelBakedRotation = 1u << 1,
  CityVoxelUniqueSourceRecipe = 1u << 2,
};

struct CityVoxelCompilerReport {
  uint64_t inputPlacements = 0;
  uint64_t occupiedVoxels = 0; // Sum over submitted placements.
  uint64_t geometryBytes = 0; // Unique encoded bytes.
  uint64_t appearanceBytes = 0;
  uint64_t distinctGeometry = 0;
  uint64_t distinctAppearances = 0;
  uint64_t geometryReuses = 0;
  uint64_t appearanceReuses = 0;
  uint64_t uniqueGeometryFromOccupancy = 0;
  uint64_t uniqueAppearanceFromValues = 0;
  uint64_t appearanceVariantsOnSharedGeometry = 0;
  // Index 0: canonical shape. Indices 1..3 correspond to the cause bits;
  // multiple reasons may apply to one newly encoded geometry asset.
  std::array<uint64_t, 4> newGeometryCauses{};
  uint64_t peakScratchBytes = 0;
  std::array<uint64_t, 5> codecCounts{};
};

// Synchronous submission provides backpressure: the caller may discard its
// source part after each return. Asset vectors hold only encoded unique data.
class CityVoxelAssetCompiler {
public:
  explicit CityVoxelAssetCompiler(CityVoxelCompilerLimits limits = {});
  bool submit(uint64_t stableId, uint32_t pitchMicrometres,
              const std::vector<CityVoxelCell> &cells,
              CityVoxelCompiledPlacement &placement, std::string *error = nullptr,
              uint8_t geometryCauseFlags = CityVoxelCanonicalShape);
  const std::vector<CityVoxelCompiledGeometry> &geometry() const { return geometry_; }
  const std::vector<CityVoxelCompiledAppearance> &appearances() const { return appearances_; }
  const CityVoxelCompilerReport &report() const { return report_; }
private:
  CityVoxelCompilerLimits limits_;
  CityVoxelCompilerReport report_;
  std::vector<CityVoxelCompiledGeometry> geometry_;
  std::vector<CityVoxelCompiledAppearance> appearances_;
  std::unordered_map<uint64_t, uint32_t> geometryById_;
  std::unordered_map<uint64_t, uint32_t> appearanceById_;
};

bool decodeCityVoxelGeometry(const CityVoxelCompiledGeometry &, std::vector<std::array<int32_t, 3>> &,
                             std::vector<CityVoxelAddress> *, std::string *error = nullptr);
bool decodeCityVoxelAppearance(const CityVoxelCompiledAppearance &, std::vector<CityVoxelCell> &,
                               std::string *error = nullptr);

// A small checked phase-4 cache record. The full .cvscene page writer is phase 9.
bool writeCityVoxelCompiledAssets(const std::string &path, const CityVoxelCompiledGeometry &,
                                  const CityVoxelCompiledAppearance &, std::string *error = nullptr);
bool readCityVoxelCompiledAssets(const std::string &path, const CityVoxelCompilerLimits &,
                                 CityVoxelCompiledGeometry &, CityVoxelCompiledAppearance &,
                                 std::string *error = nullptr);

} // namespace voxel::svraster
