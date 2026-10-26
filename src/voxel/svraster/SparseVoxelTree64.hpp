#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{
constexpr uint32_t kSparseVoxelTree64Branching = 4u;
constexpr uint32_t kSparseVoxelTree64ChildCount = 64u;
constexpr uint32_t kSparseVoxelTree64TerminalFlag = 1u;
constexpr uint32_t kSparseVoxelTree64ChunkValid = 1u << 0u;
constexpr uint32_t kSparseVoxelTree64ChunkUnknown = 1u << 1u;
constexpr uint32_t kSparseVoxelTree64ChunkOccupied = 1u << 2u;
constexpr uint32_t kSparseVoxelTree64BuildInvalid = 0u;
constexpr uint32_t kSparseVoxelTree64BuildSuccess = 1u;
constexpr uint32_t kSparseVoxelTree64BuildOverflow = 2u;
constexpr uint32_t kSparseVoxelTree64BuildOverflowPreflight = 1u;
constexpr uint32_t kSparseVoxelTree64BuildJobValid = 1u << 0u;
constexpr uint32_t kSparseVoxelTree64PublishRecordEmpty = 1u << 0u;
constexpr uint32_t kSparseVoxelTree64DynamicValid = 1u << 0u;
constexpr uint32_t kSparseVoxelTree64TerrainPageValid = 1u << 0u;
constexpr uint32_t kSparseVoxelTerrainPublicationValid = 1u << 0u;
// The publication also owns the stable terrain dynamic-instance visibility
// words. GPU-generated pages build their transform and bounds while hidden;
// the final metadata CAS exposes (or clears) the instance in the same dispatch
// as its page state and Tree64 descriptor.
constexpr uint32_t kSparseVoxelTerrainPublicationDynamicInstance = 1u << 1u;
// Rewrite only SparseVoxelTree64DynamicInstanceGPU::flags while preserving the
// instance transform, bounds, identity, and generation already resident on the
// GPU. Terrain cut transitions use this to stop tracing an incumbent parent in
// the same transaction that installs its children, while retaining the parent
// raster page until the runtime cut has switched.
constexpr uint32_t kSparseVoxelTerrainPublicationDynamicFlagsOnly = 1u << 2u;
// Heightfield receivers can be displaced within the conservative Tree64 leaf
// that represents them. Visibility rays may ignore that one containing leaf;
// all following leaves remain ordinary occluders.
constexpr uint32_t kSparseVoxelTree64DynamicSkipReceiverCell = 1u << 2u;
// worldToModel is a pure translation: identity rotation, no scale, no shear.
//
// Every terrain page is placed this way - the model space *is* the page's cell
// cube - so the general 4x4 transform the traversal applies per ray per
// instance reduces to three adds on the origin and nothing at all on the
// direction. Without this the direction transform is sixteen multiplies and
// twelve adds computing its own input. Set only when the transform has been
// checked, since a general dynamic instance may rotate.
constexpr uint32_t kSparseVoxelTree64DynamicTranslationOnly = 1u << 3u;
// The instance remains a valid raster/ownership record but global any-hit
// lighting traversal treats it as empty. This is deliberately not a source
// flag: it is a transient GPU publication state used while replacing a terrain
// LOD parent with its children.
constexpr uint32_t kSparseVoxelTree64DynamicDisableAnyHit = 1u << 4u;
// Flags a caller may set on an instance source. Snapshot validation rejects
// anything outside this mask, so a new flag must be added here or every
// instance carrying it is refused.
constexpr uint32_t kSparseVoxelTree64DynamicSourceFlagMask =
    kSparseVoxelTree64DynamicSkipReceiverCell |
    kSparseVoxelTree64DynamicTranslationOnly;
constexpr uint32_t kSparseVoxelTree64DynamicUnknown = 1u << 1u;

// Children are indexed x + 4 * y + 16 * z. Internal nodes encode
// childBase << 1 in childBaseAndFlags; terminal 4-cubed nodes set bit zero and
// use only their population mask.
struct SparseVoxelTree64NodeGPU
{
  uint32_t childBaseAndFlags = 0u;
  uint32_t populationMaskLo = 0u;
  uint32_t populationMaskHi = 0u;
};

struct SparseVoxelTree64NodePaddedGPU
{
  uint32_t childBaseAndFlags = 0u;
  uint32_t populationMaskLo = 0u;
  uint32_t populationMaskHi = 0u;
  uint32_t reserved0 = 0u;
};

struct SparseVoxelTree64ChunkGPU
{
  std::array<int32_t, 3> worldChunk{};
  uint32_t nodeSlot = 0u;
  uint32_t rootNodeOffset = 0u;
  uint32_t nodeCount = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t flags = 0u;
};

// Stable logical terrain owner -> immutable physical Tree64 page mapping.
//
// Node and material payloads are written while this descriptor still points
// at the previous committed pages. A final publication pass replaces the
// complete descriptor only after both payloads and the matching raster page
// are ready. Page generations reject delayed asynchronous work after a
// physical slot has been recycled.
struct alignas(16) SparseVoxelTerrainTree64DescriptorGPU
{
  uint32_t terrainSlot = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint32_t flags = 0u;

  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  uint32_t rootNodeOffset = 0u;
  uint32_t nodeCount = 0u;

  uint32_t materialPageSlot = 0u;
  uint32_t materialPageGeneration = 0u;
  uint32_t materialCellOffset = 0u;
  uint32_t materialCellCount = 0u;

  uint32_t geometryRevisionLo = 0u;
  uint32_t geometryRevisionHi = 0u;
  // Terrain pages may change their internal lattice without changing their
  // stable dynamic model slot. The descriptor therefore owns the two model
  // fields that vary with page LOD. localMinimum is always zero and terrain
  // pages use a uniform cell size.
  uint32_t modelSide = 0u;
  uint32_t modelCellSizeBits = 0u;
};

inline bool sparseVoxelTerrainTree64DescriptorMatchesLogicalPage(
    const SparseVoxelTerrainTree64DescriptorGPU &descriptor,
    const uint32_t terrainGeneration,
    const uint32_t internalLOD,
    const uint64_t geometryRevision)
{
  const uint64_t committedRevision =
      uint64_t{descriptor.geometryRevisionLo} |
      (uint64_t{descriptor.geometryRevisionHi} << 32u);
  return (descriptor.flags & kSparseVoxelTree64TerrainPageValid) != 0u &&
      descriptor.terrainGeneration == terrainGeneration &&
      descriptor.internalLOD == internalLOD &&
      committedRevision == geometryRevision;
}

struct alignas(16) SparseVoxelTree64DynamicInstanceGPU
{
  std::array<float, 16> worldToModel{};
  std::array<float, 4> worldBoundsMinimum{};
  std::array<float, 4> worldBoundsMaximum{};
  uint32_t modelIndex = 0u;
  uint32_t stableInstanceId = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t flags = 0u;
  uint32_t terrainSlot = std::numeric_limits<uint32_t>::max();
  uint32_t terrainGeneration = 0u;
  uint32_t regionMappingId = 0u;
  // Raw f32 bits. A flagged terrain instance exits one cell of the raster
  // lattice, not one possibly OR-reduced Tree64 leaf. Zero keeps the legacy
  // one-Tree64-leaf behaviour for ordinary diagnostic instances.
  uint32_t receiverCellSizeBits = 0u;
};

// One conditional visibility rewrite. The validation dispatch compares the
// expected raster/Tree64 identity with GPU-resident state and validates the
// replacement physical pages. A barrier then exposes this record to the
// publication dispatch, which writes both replacements or neither.
struct alignas(16) SparseVoxelTerrainPublicationRecordGPU
{
  uint32_t terrainSlot = 0u;
  uint32_t flags = 0u;
  // Used when kSparseVoxelTerrainPublicationDynamicInstance is set.
  // These become SparseVoxelTree64DynamicInstanceGPU::contentGeneration and
  // ::flags; zero values atomically turn an evicted stable slot into a hole.
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  std::array<uint32_t, 4> expectedPageState{};
  std::array<uint32_t, 4> replacementPageState{};
  SparseVoxelTerrainTree64DescriptorGPU expectedTree64{};
  SparseVoxelTerrainTree64DescriptorGPU replacementTree64{};
  // CPU-authored compact install command. Hidden GPU generation never writes
  // the renderer-visible stable instance. After reading only the result
  // records, the CPU supplies the complete replacement instance here and the
  // final publication pass installs it with the page state/descriptor CAS.
  // Keeping the complete typed instance avoids retaining an old transform or
  // bounds when a stable logical slot changes LOD. A typed field also avoids
  // nested fixed-array lowering bugs in Metal's SPIR-V translation.
  SparseVoxelTree64DynamicInstanceGPU replacementDynamicInstance{};
};

struct alignas(16) SparseVoxelTerrainPublicationStatusGPU
{
  uint32_t accepted = 0u;
  uint32_t rejectedRecord = 0xffffffffu;
  uint32_t generation = 0u;
  uint32_t publishedRecordCount = 0u;
  // Zero on acceptance. Non-zero values identify which validation invariant
  // rejected the record while rejectedRecord retains its stable array index.
  uint32_t rejectionReason = 0u;
  std::array<uint32_t, 3> reserved{};
};

struct SparseVoxelTree64StateGPU
{
  std::array<uint32_t, 3> logicalChunkResolution{};
  uint32_t chunkSide = 0u;
  std::array<int32_t, 3> originWorldChunk{};
  uint32_t valid = 0u;
  std::array<uint32_t, 3> wrapOffset{};
  uint32_t contentGeneration = 0u;
  std::array<int32_t, 3> originCell{};
  uint32_t reserved0 = 0u;
  std::array<uint32_t, 3> logicalCellResolution{};
  uint32_t reserved1 = 0u;
  std::array<float, 3> cellSize{};
  uint32_t reserved2 = 0u;
  std::array<float, 3> worldExtent{};
  uint32_t overflowStage = 0u;
};

// Mirrors SparseVoxelTree64PrimaryCamera. Bound in place of the ray array by
// the primary-ray trace variants, which derive each ray from its invocation
// index rather than reading one back from memory.
struct alignas(16) SparseVoxelTree64PrimaryCameraGPU
{
  std::array<float, 16> cameraToWorld{};
  std::array<float, 4> originMaximumDistance{};
  std::array<float, 4> focalPrincipal{};
  std::array<uint32_t, 4> viewport{};
};

// Distance-scaled traversal level of detail. Beyond `startDistance`, a
// populated interior node whose world size falls at or below
//
//   min(worldSize + distanceSlope * (t - startDistance), maximumWorldSize)
//
// is reported as a hit instead of being refined. A non-positive maximum is
// unbounded. This is a conservative voxel-cone trace: it can move an occluder
// nearer, but cannot discard one.
//
// Both zero - the default - reproduces leaf-exact traversal bit for bit, so
// primary rays and the equivalence tests are unaffected until a caller opts in.
// It can only over-report occlusion, never miss it, so shadow and occlusion
// consumers can trade a controlled amount of extra shadowing for fewer node
// visits without introducing light leaks.
struct SparseVoxelTree64TraceLODGPU
{
  float worldSize = 0.0f;
  float distanceSlope = 0.0f;
  float startDistance = 0.0f;
  float maximumWorldSize = 0.0f;

  bool enabled() const { return worldSize > 0.0f || distanceSlope > 0.0f; }

  float cutoffWorldSize(const float rayDistance) const
  {
    if (!enabled() || rayDistance < startDistance)
      return 0.0f;
    const float cutoff =
        std::max(0.0f, worldSize) +
        std::max(0.0f, distanceSlope) *
            std::max(0.0f, rayDistance - std::max(0.0f, startDistance));
    return maximumWorldSize > 0.0f
        ? std::min(cutoff, maximumWorldSize)
        : cutoff;
  }
};

struct alignas(16) SparseVoxelTree64TraceDispatchGPU
{
  uint32_t rayCount = 0u;
  uint32_t maximumQueries = 0u;
  uint32_t chunkCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t nodeSlotCapacity = 0u;
  float lodWorldSize = 0.0f;
  float lodDistanceSlope = 0.0f;
  float lodStartDistance = 0.0f;
  float lodMaximumWorldSize = 0.0f;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

struct alignas(16) SparseVoxelTree64BuildDispatchGPU
{
  uint32_t side = 0u;
  uint32_t cellCount = 0u;
  uint32_t nodeSlot = 0u;
  uint32_t nodeSlotCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTree64BuildResultGPU
{
  uint32_t status = 0u;
  uint32_t nodeCount = 0u;
  uint32_t rootNodeOffset = 0u;
  uint32_t nodeSlot = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t overflowStage = 0u;
  uint32_t maximumStack = 0u;
  uint32_t reserved0 = 0u;
};

struct alignas(16) SparseVoxelTree64BuildJobGPU
{
  std::array<int32_t, 3> worldChunk{};
  uint32_t nodeSlot = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTree64BuildSourceGPU
{
  std::array<float, 3> minimum{};
  uint32_t jobIndex = 0u;
  std::array<float, 3> maximum{};
  uint32_t reserved0 = 0u;
};

struct alignas(16) SparseVoxelTree64BatchBuildDispatchGPU
{
  uint32_t side = 0u;
  uint32_t cellCount = 0u;
  uint32_t chunkCount = 0u;
  uint32_t sourceCount = 0u;
  uint32_t nodeSlotCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t terminalCount = 0u;
  uint32_t middleCount = 0u;
  uint32_t cellCapacity = 0u;
  uint32_t terminalCapacity = 0u;
  uint32_t middleCapacity = 0u;
  uint32_t resultCapacity = 0u;
  uint32_t cellWordCount = 0u;
  uint32_t cellWordCapacity = 0u;
  uint32_t reserved2 = 0u;
  uint32_t reserved3 = 0u;
};

struct alignas(16) SparseVoxelTree64PublishRecordGPU
{
  std::array<int32_t, 3> worldChunk{};
  uint32_t directoryIndex = 0u;
  uint32_t nodeSlot = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t resultIndex = 0u;
  uint32_t reserved0 = 0u;
};

struct alignas(16) SparseVoxelTree64PublishDispatchGPU
{
  uint32_t recordCount = 0u;
  uint32_t recordCapacity = 0u;
  uint32_t resultCapacity = 0u;
  uint32_t directoryCapacity = 0u;
  uint32_t nodeSlotCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTree64SceneTraversalGPU
{
  uint32_t instanceCount = 0u;
  uint32_t hierarchyNodeCount = 0u;
  uint32_t blasCount = 0u;
  uint32_t traversalLevelCount = 0u;
  uint32_t queueCapacity = 0u;
  uint32_t chunkCount = 0u;
  uint32_t chunkSide = 0u;
  uint32_t cellCount = 0u;
  uint32_t cellCapacity = 0u;
  uint32_t depth = 0u;
  uint32_t cellWordCount = 0u;
  uint32_t cellWordCapacity = 0u;
  std::array<float, 3> cellSize{1.0f, 1.0f, 1.0f};
  uint32_t reserved2 = 0u;
};

struct alignas(16) SparseVoxelTree64SceneCountersGPU
{
  uint32_t queueOverflow = 0u;
  uint32_t instancesTested = 0u;
  uint32_t nodesTested = 0u;
  uint32_t leavesReached = 0u;
  uint32_t chunkSplats = 0u;
  uint32_t cellsWritten = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTree64DynamicModelGPU
{
  std::array<float, 3> localMinimum{};
  uint32_t nodeOffset = 0u;
  std::array<float, 3> localCellSize{1.0f, 1.0f, 1.0f};
  uint32_t nodeCount = 0u;
  uint32_t rootNodeOffset = 0u;
  uint32_t side = 0u;
  uint32_t modelId = 0u;
  uint32_t flags = 0u;
};

struct alignas(16) SparseVoxelTree64DynamicMembershipGPU
{
  std::array<int32_t, 3> worldChunk{};
  uint32_t firstReference = 0u;
  uint32_t referenceCount = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
};

struct alignas(16) SparseVoxelTree64DynamicTraceDispatchGPU
{
  uint32_t rayCount = 0u;
  uint32_t maximumQueries = 0u;
  uint32_t chunkCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t nodeSlotCapacity = 0u;
  uint32_t modelCount = 0u;
  uint32_t instanceCount = 0u;
  uint32_t membershipCapacity = 0u;
  uint32_t referenceCount = 0u;
  uint32_t dynamicNodeCount = 0u;
  uint32_t maximumReferencesPerChunk = 0u;
  uint32_t dynamicGeneration = 0u;
  float lodWorldSize = 0.0f;
  float lodDistanceSlope = 0.0f;
  float lodStartDistance = 0.0f;
  float lodMaximumWorldSize = 0.0f;
  // BVH4 broad phase over instances. A zero node count selects the clipmap
  // directory march, so both paths ship in one build and can be compared with
  // a single flag rather than a rebuild.
  uint32_t tlasRootNode = 0xffffffffu;
  uint32_t tlasNodeCount = 0u;
  uint32_t tlasReserved0 = 0u;
  uint32_t tlasReserved1 = 0u;
};

struct SparseVoxelTree64Chunk
{
  uint32_t side = 0u;
  std::vector<SparseVoxelTree64NodeGPU> nodes;
};

struct SparseVoxelTree64CellAddress
{
  std::array<int32_t, 3> worldChunk{};
  std::array<uint32_t, 3> chunkCell{};
};

struct SparseVoxelTree64AABB
{
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
};

bool sparseVoxelTree64SupportedSide(uint32_t side);
// Model-space trees are CPU-built and may follow the complete authored
// octree depth. The GPU world builder remains limited to supportedSide().
bool sparseVoxelTree64SupportedModelSide(uint32_t side);
uint32_t sparseVoxelTree64Depth(uint32_t side);
uint32_t sparseVoxelTree64ModelDepth(uint32_t side);
uint32_t sparseVoxelTree64MaximumNodeCount(uint32_t side);
uint32_t sparseVoxelTree64ChildIndex(uint32_t x, uint32_t y, uint32_t z);
uint64_t sparseVoxelTree64PopulationMask(const SparseVoxelTree64NodeGPU &node);
uint32_t sparseVoxelTree64PopulationCountBefore(const SparseVoxelTree64NodeGPU &node, uint32_t childIndex);
bool sparseVoxelTree64NodeTerminal(const SparseVoxelTree64NodeGPU &node);
uint32_t sparseVoxelTree64NodeChildBase(const SparseVoxelTree64NodeGPU &node);

int32_t sparseVoxelTree64FloorDivide(int32_t value, uint32_t divisor);
uint32_t sparseVoxelTree64PositiveModulo(int32_t value, uint32_t divisor);
SparseVoxelTree64CellAddress sparseVoxelTree64CellAddress(const std::array<int32_t, 3> &globalCell, uint32_t chunkSide);
bool sparseVoxelTree64DirectorySlot(
    const std::array<int32_t, 3> &worldChunk,
    const std::array<int32_t, 3> &originWorldChunk,
    const std::array<uint32_t, 3> &logicalResolution,
    const std::array<uint32_t, 3> &wrapOffset,
    std::array<uint32_t, 3> &physicalSlot);

bool buildSparseVoxelTree64(uint32_t side, const std::vector<uint8_t> &occupiedCells, SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
bool buildSparseVoxelTree64(uint32_t side, const std::vector<SparseVoxelTree64AABB> &aabbs, SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
// Builds an aligned lower-depth tree by OR-reducing source occupancy. Every
// occupied source cell is contained by one occupied target cell, so the result
// is suitable for conservative visibility even when targetSide is lower than
// sourceSide. Both sides use the Tree64 power-of-four lattice and the target
// must divide the source.
bool buildConservativeSparseVoxelTree64(
    uint32_t sourceSide,
    const std::vector<uint8_t> &sourceOccupiedCells,
    uint32_t targetSide,
    SparseVoxelTree64Chunk &chunk,
    std::string *error = nullptr);
// Sparse model builder: unlike the world-chunk builder, this does not allocate
// side^3 temporary cells and encodes coarse occupied voxels as early terminal
// nodes. This keeps 256..65536-cell authored BLAS trees proportional to source
// hierarchy complexity.
bool buildSparseVoxelTree64Model(uint32_t side, const std::vector<SparseVoxelTree64AABB> &aabbs, SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
// Exact sparse unit cells without an intermediate double-precision AABB per
// voxel. This is the offline city geometry-to-Tree64 adapter.
bool buildSparseVoxelTree64ModelCells(uint32_t side,
    const std::vector<std::array<int32_t, 3>> &cells,
    SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
bool validateSparseVoxelTree64(const SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
bool validateSparseVoxelTree64Model(const SparseVoxelTree64Chunk &chunk, std::string *error = nullptr);
bool sparseVoxelTree64Occupied(const SparseVoxelTree64Chunk &chunk, const std::array<uint32_t, 3> &cell);

static_assert(sizeof(SparseVoxelTree64NodeGPU) == 12u);
static_assert(sizeof(SparseVoxelTree64NodePaddedGPU) == 16u);
static_assert(sizeof(SparseVoxelTree64ChunkGPU) == 32u);
static_assert(sizeof(SparseVoxelTerrainTree64DescriptorGPU) == 64u);
static_assert(offsetof(
    SparseVoxelTerrainPublicationRecordGPU,
    replacementDynamicInstance) == 176u);
static_assert(sizeof(SparseVoxelTerrainPublicationRecordGPU) == 304u);
static_assert(sizeof(SparseVoxelTerrainPublicationStatusGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64StateGPU) == 112u);
static_assert(sizeof(SparseVoxelTree64TraceDispatchGPU) == 48u);
static_assert(sizeof(SparseVoxelTree64BuildDispatchGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64BuildResultGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64BuildJobGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64BuildSourceGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64BatchBuildDispatchGPU) == 64u);
static_assert(sizeof(SparseVoxelTree64PublishRecordGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64PublishDispatchGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64SceneTraversalGPU) == 64u);
static_assert(sizeof(SparseVoxelTree64SceneCountersGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64DynamicModelGPU) == 48u);
static_assert(sizeof(SparseVoxelTree64DynamicInstanceGPU) == 128u);
static_assert(sizeof(SparseVoxelTree64DynamicMembershipGPU) == 32u);
static_assert(sizeof(SparseVoxelTree64DynamicTraceDispatchGPU) == 80u);
} // namespace voxel::svraster
