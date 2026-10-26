#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace voxel::svraster
{

// Logical identity only. Physical scratch/output leases are deliberately not
// part of the key, so an evicted chunk can be rebuilt into different storage
// without changing its content identity.
struct TerrainGPUChunkKey
{
  uint64_t worldBundleRevision = 0u;
  int32_t chunkX = 0;
  int32_t chunkY = 0;
  int32_t chunkZ = 0;
  uint16_t clipmapLOD = 0u;
  uint16_t generatorRevision = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t expectedGeneration = 0u;

  bool operator==(const TerrainGPUChunkKey &other) const;
  bool operator<(const TerrainGPUChunkKey &other) const;
};

static_assert(sizeof(TerrainGPUChunkKey) == 32u);

struct TerrainGPUChunkShape
{
  uint16_t bricksX = 0u;
  uint16_t bricksY = 0u;
  uint16_t bricksZ = 0u;
  uint16_t brickSide = 8u;

  bool operator==(const TerrainGPUChunkShape &other) const
  {
    return bricksX == other.bricksX && bricksY == other.bricksY &&
        bricksZ == other.bricksZ && brickSide == other.brickSide;
  }
};

static_assert(sizeof(TerrainGPUChunkShape) == 8u);

// All bounds are expressed in voxels at key.clipmapLOD and are half-open.
struct TerrainGPUChunkBounds
{
  std::array<int32_t, 3u> minimum{};
  std::array<int32_t, 3u> maximum{};
};

static_assert(sizeof(TerrainGPUChunkBounds) == 24u);

struct TerrainGPUChunkShapeCounts
{
  std::array<uint32_t, 3u> voxelDimensions{};
  uint32_t denseBrickCount = 0u;
  uint32_t denseVoxelCount = 0u;
  uint32_t columnCount = 0u;
};

enum class TerrainGPUChunkFace : uint32_t
{
  NegativeX,
  PositiveX,
  NegativeY,
  PositiveY,
  NegativeZ,
  PositiveZ,
};

// Values are stable GPU ABI values. State transitions are monotonic except
// that any active state may terminate in Cancelled or Failed.
enum class TerrainGPUJobStage : uint32_t
{
  WaitingForAtlas = 0u,
  SampleColumns,
  ClassifyFineBricks,
  ScanFineBricks,
  EmitFineBricks,
  BuildRasterParents,
  BuildTree64Masks,
  ScanTree64,
  LinkTree64,
  Validate,
  ReadyToPublish,
  SubmittedForPublication,
  Complete,
  Cancelled,
  Failed,
};

// Direct GPU representation. It avoids 16-bit shader fields even though the
// compact logical key and shape use them on the CPU.
struct TerrainGPUChunkKeyGPU
{
  uint64_t worldBundleRevision = 0u;
  int32_t chunkX = 0;
  int32_t chunkY = 0;
  int32_t chunkZ = 0;
  uint32_t packedLODAndGeneratorRevision = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t expectedGeneration = 0u;
};

static_assert(sizeof(TerrainGPUChunkKeyGPU) == 32u);

struct alignas(16) TerrainGPUChunkJobGPU
{
  TerrainGPUChunkKeyGPU key{};
  TerrainGPUChunkBounds bounds{};
  uint32_t bricksX = 0u;
  uint32_t bricksY = 0u;
  uint32_t bricksZ = 0u;
  uint32_t brickSide = 0u;
  uint32_t stage = 0u;
  uint32_t stageItemCount = 0u;
  uint32_t estimatedMicroseconds = 0u;
  uint32_t measuredMicroseconds = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
  uint64_t inputLease = 0u;
  uint64_t outputLease = 0u;
  uint64_t completionFence = 0u;
  std::array<uint32_t, 2u> reserved1{};
};

static_assert(sizeof(TerrainGPUChunkJobGPU) == 128u);
static_assert(alignof(TerrainGPUChunkJobGPU) == 16u);
static_assert(std::is_standard_layout<TerrainGPUChunkJobGPU>::value);
static_assert(std::is_trivially_copyable<TerrainGPUChunkJobGPU>::value);
static_assert(offsetof(TerrainGPUChunkJobGPU, bounds) == 32u);
static_assert(offsetof(TerrainGPUChunkJobGPU, stage) == 72u);
static_assert(offsetof(TerrainGPUChunkJobGPU, inputLease) == 96u);

struct TerrainGPUCanonicalVoxel
{
  std::array<int64_t, 3u> coordinate{};
};

struct TerrainGPUClipmapVoxel
{
  std::array<int32_t, 3u> coordinate{};
  uint16_t clipmapLOD = 0u;
};

struct TerrainGPUColumnTileCoordinate
{
  int32_t tileX = 0;
  int32_t tileZ = 0;
  uint16_t clipmapLOD = 0u;
};

struct TerrainGPUBrickCoordinate
{
  std::array<int32_t, 3u> coordinate{};
  uint16_t clipmapLOD = 0u;
};

struct TerrainGPUColumnRun
{
  int32_t firstSolidY = 0;
  int32_t topY = -1;
};

struct TerrainGPUBrickCandidate
{
  std::array<int32_t, 3u> canonicalBrick{};
  std::array<int32_t, 3u> minimumVoxel{};
  std::array<uint32_t, 3u> localBrick{};
  uint32_t linearIndex = 0u;
};

struct TerrainGPUBoundarySample
{
  // Finest-grid world coordinate. Adjacent LODs therefore name the same
  // physical sample with identical integers.
  std::array<int64_t, 3u> canonicalCoordinate{};
  int32_t value = 0;
  uint32_t flags = 0u;
};

struct TerrainGPUBoundarySignature
{
  uint64_t hash = 0u;
  int64_t canonicalPlane = 0;
  uint32_t sampleCount = 0u;
  uint32_t axis = 0u;

  bool operator==(const TerrainGPUBoundarySignature &other) const
  {
    return hash == other.hash && canonicalPlane == other.canonicalPlane &&
        sampleCount == other.sampleCount && axis == other.axis;
  }
};

int64_t terrainGPUFloorDivide(int64_t numerator, int64_t positiveDenominator);

bool terrainGPUChunkShapeCounts(
    const TerrainGPUChunkShape &shape,
    TerrainGPUChunkShapeCounts &counts,
    std::string *error = nullptr);

bool terrainGPUChunkBounds(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    TerrainGPUChunkBounds &bounds,
    std::string *error = nullptr);

bool terrainGPUWorldToCanonicalVoxel(
    const std::array<double, 3u> &worldPositionMetres,
    const std::array<double, 3u> &worldOriginMetres,
    double finestVoxelSizeMetres,
    TerrainGPUCanonicalVoxel &voxel,
    std::string *error = nullptr);

bool terrainGPUCanonicalToClipmapVoxel(
    const TerrainGPUCanonicalVoxel &canonical,
    uint16_t clipmapLOD,
    TerrainGPUClipmapVoxel &voxel,
    std::string *error = nullptr);

bool terrainGPUClipmapVoxelMinimum(
    const TerrainGPUClipmapVoxel &voxel,
    TerrainGPUCanonicalVoxel &canonical,
    std::string *error = nullptr);

bool terrainGPUClipmapVoxelWorldBounds(
    const TerrainGPUClipmapVoxel &voxel,
    const std::array<double, 3u> &worldOriginMetres,
    double finestVoxelSizeMetres,
    std::array<double, 3u> &minimumMetres,
    std::array<double, 3u> &maximumMetres,
    std::string *error = nullptr);

bool terrainGPUColumnTileForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    uint32_t tileVoxelsX,
    uint32_t tileVoxelsZ,
    TerrainGPUColumnTileCoordinate &tile,
    std::string *error = nullptr);

bool terrainGPUColumnTileVoxelBounds(
    const TerrainGPUColumnTileCoordinate &tile,
    uint32_t tileVoxelsX,
    uint32_t tileVoxelsZ,
    std::array<int32_t, 2u> &minimumXZ,
    std::array<int32_t, 2u> &maximumXZ,
    std::string *error = nullptr);

bool terrainGPUChunkKeyForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUChunkKey &identity,
    TerrainGPUChunkKey &key,
    std::string *error = nullptr);

bool terrainGPUBrickForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    uint32_t brickSide,
    TerrainGPUBrickCoordinate &brick,
    std::string *error = nullptr);

bool terrainGPUBrickVoxelBounds(
    const TerrainGPUBrickCoordinate &brick,
    uint32_t brickSide,
    TerrainGPUChunkBounds &bounds,
    std::string *error = nullptr);

bool terrainGPUCanonicalChunkBounds(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    std::array<int64_t, 3u> &minimum,
    std::array<int64_t, 3u> &maximum,
    std::string *error = nullptr);

bool terrainGPUChunkContainsVoxel(
    const TerrainGPUChunkBounds &bounds,
    const std::array<int32_t, 3u> &voxel);

bool terrainGPUChunkBoundsOverlap(
    const TerrainGPUChunkBounds &left,
    const TerrainGPUChunkBounds &right);

bool terrainGPUChunkOwnsCanonicalPoint(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUCanonicalVoxel &point,
    std::string *error = nullptr);

bool terrainGPUColumnRunIntersectsChunk(
    const TerrainGPUColumnRun &run,
    const TerrainGPUChunkBounds &bounds);

bool terrainGPUEnumerateVerticalChunks(
    const TerrainGPUChunkKey &horizontalIdentity,
    const TerrainGPUChunkShape &shape,
    const std::vector<TerrainGPUColumnRun> &columns,
    std::vector<TerrainGPUChunkKey> &chunks,
    std::string *error = nullptr);

bool terrainGPUEnumerateDenseBrickCandidates(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    std::vector<TerrainGPUBrickCandidate> &candidates,
    std::string *error = nullptr);

bool terrainGPUBoundarySignatureForChunk(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    TerrainGPUChunkFace face,
    const std::vector<TerrainGPUBoundarySample> &samples,
    TerrainGPUBoundarySignature &signature,
    std::string *error = nullptr);

TerrainGPUChunkKeyGPU terrainGPUChunkKeyGPU(const TerrainGPUChunkKey &key);

bool terrainGPUChunkJobGPU(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    TerrainGPUJobStage stage,
    uint32_t stageItemCount,
    TerrainGPUChunkJobGPU &job,
    std::string *error = nullptr);

} // namespace voxel::svraster
