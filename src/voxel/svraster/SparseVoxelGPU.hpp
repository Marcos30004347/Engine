#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace voxel::svraster
{

constexpr uint32_t kMaxOctreeLevels = 16u;
constexpr uint32_t kOctreePathBits = 3u * kMaxOctreeLevels;
constexpr uint64_t kOctreePathMask = (uint64_t{1} << kOctreePathBits) - 1u;
constexpr uint32_t kExteriorShellLevel = 7u;
constexpr uint32_t kExteriorShellResolution = 1u << kExteriorShellLevel;
constexpr uint32_t kExteriorShellVoxelCount =
    kExteriorShellResolution * kExteriorShellResolution * kExteriorShellResolution - (kExteriorShellResolution - 2u) * (kExteriorShellResolution - 2u) * (kExteriorShellResolution - 2u);
constexpr uint32_t kPreprocessWorkgroupSize = 256u;
// Raster candidates are bucketed at 4x4 granularity. The resolver still runs
// 8x8 workgroups and processes independent buckets per wave, keeping full
// occupancy while sharply reducing candidate/pixel rejection work.
constexpr uint32_t kSparseVoxelTileSize = 4u;
constexpr uint32_t kSparseVoxelScanWorkgroupSize = 256u;
constexpr uint32_t kSparseVoxelIndirectSlotCount = 4u;
constexpr uint32_t kInvalidSparseVoxelIndex = std::numeric_limits<uint32_t>::max();
constexpr uint64_t kInvalidOctreePath = std::numeric_limits<uint64_t>::max();
constexpr float kSparseVoxelSurfaceTieEpsilon = 1.0e-5f;
constexpr uint32_t kSparseVoxelSurfaceHit = 1u << 0u;
constexpr uint32_t kSparseVoxelSurfaceCameraInside = 1u << 1u;
constexpr uint32_t kSparseVoxelSurfaceNoMaterial = 1u << 2u;
constexpr uint32_t kSparseVoxelSurfacePackedMaterial = 1u << 3u;
constexpr uint32_t kSparseVoxelGBufferHasPreviousFrame = 1u << 0u;
constexpr uint32_t kSparseVoxelCullingFrustum = 1u << 0u;
constexpr uint32_t kSparseVoxelCullingOcclusion = 1u << 1u;
constexpr uint32_t kSparseVoxelCullingPreviousFrame = 1u << 2u;
constexpr uint32_t kSparseVoxelCullingRecovery = 1u << 3u;
constexpr uint32_t kSparseVoxelCullingReverseZ = 1u << 4u;
// The temporal depth prepass already proved these instances visible. The
// current-frame recovery pass must not test them against depth they generated
// themselves; it only uses the HZB for instances absent from the prepass.
constexpr uint32_t kSparseVoxelCullingPrepassGuarantee = 1u << 6u;
// Selects occupancy-volume AABB queries in the shared TLAS traversal. This
// mode bypasses camera-frustum and HZB decisions entirely.
constexpr uint32_t kSparseVoxelCullingOccupancyQuery = 1u << 7u;
// LOD traversal policy shares cullingFlags with the generic culling bits, so
// policy bits must not alias frustum/occlusion/history/depth-convention bits.
constexpr uint32_t kSparseVoxelLODSeedsFromVisibleInstances = 1u << 5u;
static_assert(
    (kSparseVoxelLODSeedsFromVisibleInstances &
     (kSparseVoxelCullingFrustum | kSparseVoxelCullingOcclusion | kSparseVoxelCullingPreviousFrame | kSparseVoxelCullingRecovery | kSparseVoxelCullingReverseZ | kSparseVoxelCullingPrepassGuarantee)) == 0u);
constexpr uint32_t kSparseVoxelTLASWorkgroupSize = 256u;
constexpr uint32_t kSparseVoxelTLASMaxScanBlocks = 256u;

enum class SparseVoxelIndirectSlot : uint32_t
{
  Preprocess = 0u,
  CandidateEmission = 1u,
  Sort = 2u,
  TileRanges = 3u,
};

enum class SparseVoxelOverflowStage : uint32_t
{
  None = 0u,
  VisibleInstances = 1u,
  VisibleVoxels = 2u,
  ProjectedVoxels = 3u,
  TileCandidates = 4u,
  TileRanges = 5u,
  PageRequests = 6u,
  LODQueue = 7u,
  TLASQueue = 8u,
  OccupancyLayout = 9u,
  OccupancyDirtyWords = 10u,
};

enum class SparseVoxelDebugMode : uint32_t
{
  BaseColor = 0u,
  GeometryNormal = 1u,
  ShadingNormal = 2u,
  Roughness = 3u,
  Metallic = 4u,
  Emissive = 5u,
  MotionVector = 6u,
  HardwareDepth = 7u,
  LinearDepth = 8u,
  Coverage = 9u,
  InstanceId = 10u,
  VoxelId = 11u,
  OccupancyShadow = 12u,
  OccupancyHitCell = 13u,
  SceneRayShading = 14u,
  SceneRayStatus = 15u,
  SceneRayScreenTrace = 16u,
  SceneRayWorldTrace = 17u,
  SceneRayLightingAccumulation = 18u,
  // Fragment invocations per pixel in the brick draw pass. Selecting it rebuilds
  // the render graph so the counter and its extra pass exist only while it is on.
  BrickOverdraw = 19u,
  // Descriptor-hashed tint with face-relative U, V and inward layer channels.
  // Legacy/fallback parent hits are dark, making exact coverage visible.
  MicroSurface = 20u,
  MaterialHandle = 21u,
  MaterialProgram = 22u,
  MaterialContext = 23u,
  MaterialBaseHeight = 24u,
  MaterialModifier = 25u,
  MaterialFinalOccupancy = 26u,
  MaterialLayerWeight = 27u,
  MaterialExactFallback = 28u,
  TerrainChunkOwner = 29u,
  TerrainChunkBoundaries = 30u,
  TerrainChunkLOD = 31u,
  TerrainChunkSlot = 32u,
  TerrainChunkGeneration = 33u,
  TerrainBrickLOD = 34u,
  Lambert = 35u,
};

struct alignas(16) SparseVoxelBLASGPU
{
  std::array<float, 4> rootCenterExtent{};
  uint32_t firstVoxel = 0u;
  uint32_t voxelCount = 0u;
  uint32_t maxLevel = 0u;
  uint32_t flags = 0u;
};

struct alignas(16) SparseVoxelPathLevelGPU
{
  uint32_t pathLo = 0u;
  uint32_t pathHi = 0u;
  uint32_t level = 0u;
  uint32_t flags = 0u;
};

struct alignas(16) SparseVoxelCenterLengthGPU
{
  std::array<float, 3> center{};
  float length = 0.0f;
};

struct alignas(16) SparseVoxelMaterialGPU
{
  uint32_t albedoRGBA8 = 0u;
  uint32_t roughnessMetallicUNorm8 = 0u;
  uint32_t emissiveRG16F = 0u;
  uint32_t emissiveB16F = 0u;
  uint32_t materialId = 0u;
  uint32_t flags = 0u;
  // Optional bottom-layer albedo. Alpha zero disables the layer; an authored
  // layer is opaque RGBA8. This reuses the existing ABI word, so source voxel
  // and scene-file records remain 32 bytes.
  uint32_t reserved0 = 0u;
  // Low two bits select whether top/bottom faces and side faces respectively
  // blend from the primary (top) colour to reserved0 as virtual micro cells
  // recess. A clear bit shows only the bottom colour on that face category.
  uint32_t reserved1 = 0u;
};

// Deduplicated material extension used by exact microvoxel carving. Source
// materials keep their existing 32-byte ABI; their materialId selects one of
// these records through SparseVoxelMicroMaterialLibrary. Descriptor zero is
// always the all-zero Disabled record.
struct SparseVoxelMicroMaterialGPU
{
  uint32_t geometry = 0u;
  uint32_t shading = 0u;
};

inline constexpr uint32_t kSparseVoxelMicroSubdivisionMask = 0x0fu;
inline constexpr uint32_t kSparseVoxelMicroMaximumDepthShift = 4u;
// Q4 fixed-point microcell depth: eight bits retain the old 0..8 range while
// adding 1/16-cell placement. Geometry and shading profiles need only three
// bits each, so the material record remains eight bytes.
inline constexpr uint32_t kSparseVoxelMicroMaximumDepthMask = 0xffu;
inline constexpr uint32_t kSparseVoxelMicroDepthFractionBits = 4u;
inline constexpr uint32_t kSparseVoxelMicroDepthFractionScale =
    1u << kSparseVoxelMicroDepthFractionBits;
inline constexpr uint32_t kSparseVoxelMicroFaceMaskShift = 12u;
inline constexpr uint32_t kSparseVoxelMicroFaceMask = 0x3fu;
inline constexpr uint32_t kSparseVoxelMicroGeometryProfileShift = 18u;
inline constexpr uint32_t kSparseVoxelMicroGeometryProfileMask = 0x07u;
inline constexpr uint32_t kSparseVoxelMicroShadingProfileShift = 21u;
inline constexpr uint32_t kSparseVoxelMicroShadingProfileMask = 0x07u;
inline constexpr uint32_t kSparseVoxelMicroMaterialSeedShift = 24u;

inline constexpr uint32_t kSparseVoxelMaterialBlendTopBottomFaces = 1u << 0u;
inline constexpr uint32_t kSparseVoxelMaterialBlendSideFaces = 1u << 1u;
inline constexpr uint32_t kSparseVoxelMaterialSecondaryFaceMask =
    kSparseVoxelMaterialBlendTopBottomFaces |
    kSparseVoxelMaterialBlendSideFaces;

inline bool sparseVoxelMaterialSecondaryLayerIsCanonical(
    const SparseVoxelMaterialGPU &material)
{
  if (material.reserved0 == 0u)
    return material.reserved1 == 0u;
  return (material.reserved0 >> 24u) == 0xffu &&
      (material.reserved1 & ~kSparseVoxelMaterialSecondaryFaceMask) == 0u;
}

struct alignas(16) SparseVoxelInstanceGPU
{
  std::array<float, 16> modelToWorld{};
  std::array<float, 16> worldToModel{};
  std::array<float, 16> previousModelToWorld{};
  std::array<float, 4> worldBoundsMin{};
  std::array<float, 4> worldBoundsMax{};
  uint32_t blasIndex = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTLASNodeGPU
{
  std::array<float, 4> worldBoundsMin{};
  std::array<float, 4> worldBoundsMax{};
  uint32_t firstChild = 0u;
  uint32_t childCount = 0u;
  uint32_t firstInstance = 0u;
  uint32_t instanceCount = 0u;
};

struct alignas(16) SparseVoxelBLASRootRangeGPU
{
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
  uint32_t maxLevel = 0u;
  uint32_t flags = 0u;
};

struct SparseVoxelVisibilityOffsetGPU
{
  uint32_t visible = 0u;
  uint32_t roots = 0u;
};

struct alignas(16) SparseVoxelTLASScanBlockGPU
{
  uint32_t visible = 0u;
  uint32_t roots = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) TLASQueueElementGPU
{
  uint32_t nodeIndex = 0u;
  uint32_t depth = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) VisibleInstanceGPU
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t firstVisibleVoxel = 0u;
  uint32_t visibleVoxelCount = 0u;
  uint32_t flags = 0u;
};

struct alignas(16) VisibleVoxelGPU
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t voxelIndex = kInvalidSparseVoxelIndex;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelLODNodeGPU
{
  uint32_t firstChild = 0u;
  uint32_t childCount = 0u;
  uint32_t voxelIndex = kInvalidSparseVoxelIndex;
  uint32_t level = 0u;
};

// Indexed by the bounded physical node slot. File-global logical node indices
// are deliberately kept out of child pointers so a page can be placed in any
// contiguous physical range.
struct alignas(16) SparseVoxelResidentNodeMetadataGPU
{
  uint32_t logicalNodeIndex = kInvalidSparseVoxelIndex;
  uint32_t refinementPageIndex = kInvalidSparseVoxelIndex;
  uint32_t owningPageIndex = kInvalidSparseVoxelIndex;
  uint32_t flags = 0u;
};

struct alignas(16) SparseVoxelLODQueueElementGPU
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t nodeIndex = kInvalidSparseVoxelIndex;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

// Frame-local handoff between the temporal BLAS traversal and the
// current-frame recovery traversal.
struct alignas(16) SparseVoxelLODRecoveryStateGPU
{
  uint32_t rejectedNodeCount = 0u;
  uint32_t temporalVisibleVoxelCount = 0u;
  uint32_t recoverySeedCount = 0u;
  uint32_t recoveredVisibleVoxelCount = 0u;
};

struct alignas(16) SparseVoxelLODSeedGPU
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
  uint32_t firstQueueEntry = 0u;
};

// Compact readback representation. The traversal first deduplicates requests
// in a page bitset and performs atomicMax on the per-page priority words.
struct alignas(16) SparseVoxelPageRequestGPU
{
  uint32_t pageIndex = kInvalidSparseVoxelIndex;
  uint32_t priority = 0u;
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t flags = 0u;
};

// A rewrite is applied only after the target page payload is visible to the
// GPU. Uninstall batches use firstChild/childCount == 0 and run before freeing
// the page's physical range.
struct alignas(16) SparseVoxelPhysicalPageRewriteGPU
{
  uint32_t nodeIndex = kInvalidSparseVoxelIndex;
  uint32_t firstChild = 0u;
  uint32_t childCount = 0u;
  uint32_t flags = 0u;
};

struct alignas(16) SparseVoxelPageBoundsGPU
{
  std::array<float, 4> localBoundsMin{};
  std::array<float, 4> localBoundsMax{};
};

struct alignas(16) ProjectedVoxelGPU
{
  std::array<uint32_t, 2> packedInclusivePixelBounds{};
  uint32_t quadrantBitset = 0u;
  uint32_t duplicateCount = 0u;
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t voxelIndex = kInvalidSparseVoxelIndex;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) TileCandidateGPU
{
  uint32_t keyLo = 0u;
  uint32_t keyHi = 0u;
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t voxelIndex = kInvalidSparseVoxelIndex;
  uint32_t quadrant = 0u;
  uint32_t projectedVoxelIndex = kInvalidSparseVoxelIndex;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelTileRangeGPU
{
  uint32_t begin = 0u;
  uint32_t end = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) ResolvedVoxelSurfaceGPU
{
  float surfaceT = 0.0f;
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  uint32_t voxelIndex = kInvalidSparseVoxelIndex;
  uint32_t packedLocalFace = 0u;
  uint32_t hitFlags = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  // Packed brick surfaces: bottom RGB24 | normalized context byte << 24.
  uint32_t reserved2 = 0u;
};

// Stable logical address used by material-field sampling. Coordinates are in
// the selected material's micro grid: canonicalParent * subdivision +
// localMicroCell * canonicalParentScale. regionMappingId is a producer-owned logical identity (zero
// means the material-global mapping), never a physical brick/page/atlas slot.
struct alignas(16) VoxelMaterialSampleAddressGPU
{
  std::array<int32_t, 3> canonicalMicroCoordinate{};
  uint32_t regionMappingId = 0u;

  bool operator==(const VoxelMaterialSampleAddressGPU &other) const
  {
    return canonicalMicroCoordinate == other.canonicalMicroCoordinate &&
        regionMappingId == other.regionMappingId;
  }
};
static_assert(sizeof(VoxelMaterialSampleAddressGPU) == 16u);

// Phase-zero generated-terrain ownership sidecar. The fixed-size open-address
// table is keyed by regionMappingId and is diagnostic only: raster visibility,
// material sampling and Tree64 traversal never consume it.
struct alignas(16) SparseVoxelTerrainOwnershipGPU
{
  uint32_t regionMappingId = 0u;
  int32_t chunkAddressX = 0;
  int32_t chunkAddressY = 0;
  uint32_t spatialLOD = 0u;
  uint32_t physicalSlot = kInvalidSparseVoxelIndex;
  uint32_t publicationGeneration = 0u;
  uint32_t rasterRootCount = 0u;
  uint32_t tree64StableInstanceId = kInvalidSparseVoxelIndex;
};
static_assert(sizeof(SparseVoxelTerrainOwnershipGPU) == 32u);

inline VoxelMaterialSampleAddressGPU voxelMaterialSampleAddress(
    const std::array<int32_t, 3> &canonicalParent,
    const std::array<uint32_t, 3> &localMicroCell,
    const uint32_t canonicalParentScale, const uint32_t subdivision,
    const uint32_t regionMappingId)
{
  VoxelMaterialSampleAddressGPU result{};
  result.regionMappingId = regionMappingId;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t coordinate =
        static_cast<int64_t>(canonicalParent[axis]) * subdivision +
        static_cast<int64_t>(
            std::min(localMicroCell[axis],
                subdivision == 0u ? 0u : subdivision - 1u)) *
            canonicalParentScale;
    result.canonicalMicroCoordinate[axis] = static_cast<int32_t>(std::clamp(
        coordinate,
        static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
        static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
  }
  return result;
}

// Packed-material resolvers use reserved2 for bottom-layer RGB24 plus the
// normalized terrain context byte. Regular scene-material resolvers leave the
// word zero, preserving their existing contract.
inline constexpr uint32_t kSparseVoxelResolvedBottomAlbedoMask = 0x00ffffffu;
inline constexpr uint32_t kSparseVoxelResolvedContextShift = 24u;
inline constexpr uint32_t kSparseVoxelResolvedContextMask = 0xffu;
// Set by packed resolvers when the per-pixel micro payload already describes
// exact geometry. G-buffer passes must not layer their analytic fallback over
// that surface. Bit 31 is outside the packed scalar and blend-mode fields.
inline constexpr uint32_t kSparseVoxelExactMicroMaterialFlag = 1u << 31u;

inline constexpr uint32_t packSparseVoxelResolvedBottomAndContext(
    const uint32_t bottomAlbedo, const uint32_t context)
{
  return (bottomAlbedo & kSparseVoxelResolvedBottomAlbedoMask) |
      ((context & kSparseVoxelResolvedContextMask) <<
       kSparseVoxelResolvedContextShift);
}

inline constexpr uint32_t sparseVoxelResolvedContext(
    const ResolvedVoxelSurfaceGPU &surface)
{
  return (surface.hitFlags & kSparseVoxelSurfacePackedMaterial) != 0u
      ? (surface.reserved2 >> kSparseVoxelResolvedContextShift) &
          kSparseVoxelResolvedContextMask
      : 0u;
}

// Canonical diagnostic/readback representation of the Engine voxel G-buffer.
// Render targets retain their native formats; tests and debug capture pack one
// sampled pixel into this record after attachment quantization.
struct alignas(16) SparseVoxelGBufferPixelGPU
{
  std::array<uint32_t, 4> ids{}; // instance, voxel, material, reserved
  std::array<float, 4> materialUv{};
  std::array<float, 4> baseColor{};
  std::array<float, 4> materialProperties{};
  std::array<float, 4> emissive{};
  std::array<float, 4> geometryNormal{};
  std::array<float, 4> normal{};
  std::array<float, 4> motionDepth{}; // xy motion, z hardware depth
};

// R0 exact-quality capture. This deliberately keeps the upstream resolved
// visibility contract beside the attachment-quantized G-buffer: hashing only
// the latter cannot detect a changed temporal identity, raw packed material,
// bottom layer, micro payload, or canonical material-field address.
struct alignas(16) SparseVoxelExactOutputPixelGPU
{
  ResolvedVoxelSurfaceGPU surface{};
  VoxelMaterialSampleAddressGPU materialSampleAddress{};
  // x: micro payload, y: temporal identity, z: coverage, w: reserved.
  std::array<uint32_t, 4> diagnostics{};
  SparseVoxelGBufferPixelGPU gbuffer{};
};
static_assert(sizeof(SparseVoxelExactOutputPixelGPU) == 192u);
static_assert(offsetof(SparseVoxelExactOutputPixelGPU, surface) == 0u);
static_assert(offsetof(SparseVoxelExactOutputPixelGPU,
                  materialSampleAddress) == 32u);
static_assert(offsetof(SparseVoxelExactOutputPixelGPU, diagnostics) == 48u);
static_assert(offsetof(SparseVoxelExactOutputPixelGPU, gbuffer) == 64u);

struct alignas(16) SparseVoxelDebugPresentUniformsGPU
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t mode = static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayShading);
  float linearDepthScale = 1.0f;
  uint32_t shadowWidth = 0u;
  uint32_t shadowHeight = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  // Analytic environment used by the unlit brick preview. This keeps terrain
  // readable, and gives uncovered pixels a sky, while its full ray-traced
  // lighting representation is still being connected to streamed bricks.
  std::array<float, 4> skyUpExponent{0.0f, 1.0f, 0.0f, 0.35f};
  std::array<float, 4> skyHorizonIntensity{0.55f, 0.72f, 1.0f, 0.0f};
  std::array<float, 4> skyZenith{0.08f, 0.24f, 0.55f, 0.0f};
  std::array<float, 4> skyGround{0.05f, 0.045f, 0.04f, 0.0f};
};

struct alignas(16) IndirectDispatchArgsGPU
{
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t z = 0u;
  uint32_t reserved = 0u;
};

struct alignas(16) SparseVoxelFrameCountersGPU
{
  uint32_t visibleInstanceCount = 0u;
  uint32_t visibleVoxelCount = 0u;
  uint32_t candidateCount = 0u;
  uint32_t duplicateCount = 0u;
  uint32_t overflowStage = 0u;
  uint32_t requiredCount = 0u;
  uint32_t capacity = 0u;
  uint32_t occlusionCulledVoxelCount = 0u;
};

struct alignas(16) SparseVoxelBootstrapUniformsGPU
{
  uint32_t voxelCount = 0u;
  uint32_t preprocessWorkgroupSize = kPreprocessWorkgroupSize;
  uint32_t visibleInstanceCapacity = 1u;
  uint32_t visibleVoxelCapacity = 0u;
};

struct alignas(16) SparseVoxelLODPassUniformsGPU
{
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t depth = 0u;
  uint32_t pageCount = 0u;
  uint32_t requestWordCount = 0u;
  uint32_t streamingEnabled = 0u;
  uint32_t cullingFlags = 0u;
  uint32_t seedCount = 0u;
  uint32_t seedCapacity = 0u;
  // Zero preserves the original eight-entry test ABI. Runtime renderers set
  // maxLevel + 1 explicitly so valid level-16 files cannot overflow queues.
  uint32_t levelCapacity = 0u;
  uint32_t reserved2 = 0u;
  uint32_t hzbWidth = 0u;
  uint32_t hzbHeight = 0u;
  uint32_t hzbMipCount = 0u;
  uint32_t hzbFirstValidMip = 0u;
  float occlusionDepthBias = 1.0e-5f;
  int32_t occlusionMipBias = 0;
  uint32_t historyValid = 0u;
  uint32_t reserved3 = 0u;
  uint32_t rejectionCapacity = 0u;
  uint32_t reserved4 = 0u;
  uint32_t reserved5 = 0u;
  uint32_t reserved6 = 0u;
};

struct alignas(16) SparseVoxelTLASCullingUniformsGPU
{
  uint32_t instanceCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t rootNode = kInvalidSparseVoxelIndex;
  uint32_t maxDepth = 0u;
  uint32_t queueCapacity = 0u;
  uint32_t visibleInstanceCapacity = 0u;
  uint32_t lodQueueCapacity = 0u;
  uint32_t scanBlockCount = 0u;
  uint32_t blasCount = 0u;
  uint32_t cullingFlags = kSparseVoxelCullingFrustum;
  uint32_t depth = 0u;
  uint32_t hzbWidth = 0u;
  uint32_t hzbHeight = 0u;
  uint32_t hzbMipCount = 0u;
  uint32_t hzbFirstValidMip = 0u;
  float occlusionDepthBias = 1.0e-5f;
  int32_t occlusionMipBias = 0;
  uint32_t historyValid = 0u;
  uint32_t reserved = 0u;
  alignas(16) std::array<std::array<float, 4>, 3> occupancyQueryMinimum{};
  alignas(16) std::array<std::array<float, 4>, 3> occupancyQueryMaximum{};
  uint32_t occupancyQueryBoxCount = 0u;
  uint32_t reservedQuery0 = 0u;
  uint32_t reservedQuery1 = 0u;
  uint32_t reservedQuery2 = 0u;
};

enum class SparseVoxelOcclusionReason : uint32_t
{
  Visible = 0u,
  CulledFrustum = 1u,
  CulledOcclusion = 2u,
  VisibleHistoryInvalid = 3u,
  VisibleNearPlane = 4u,
  VisibleViewportEscape = 5u,
  VisibleInvalidProjection = 6u,
  VisibleDegenerateBounds = 7u,
  VisibleMovingPreviousTransform = 8u,
};

struct alignas(16) SparseVoxelStreamingPassUniformsGPU
{
  uint32_t pageCount = 0u;
  uint32_t requestWordCount = 0u;
  uint32_t rewriteCount = 0u;
  uint32_t physicalNodeCapacity = 0u;
};

// Persistent/public metadata for the camera-centered packed occupancy volume.
// The public texture stores one R32Uint word at (physicalX >> 5, y, z).
struct alignas(16) SparseVoxelOccupancyVolumeGPU
{
  std::array<uint32_t, 3> logicalResolution{};
  uint32_t wordsX = 0u;
  std::array<uint32_t, 3> wordResolution{};
  uint32_t flags = 0u;
  std::array<int32_t, 3> cameraCell{};
  uint32_t contentGeneration = 0u;
  std::array<int32_t, 3> originCell{};
  uint32_t sceneRevision = 0u;
  std::array<int32_t, 3> scrollDelta{};
  uint32_t dirtyWordCount = 0u;
  std::array<uint32_t, 3> wrapOffset{};
  uint32_t valid = 0u;
  std::array<float, 3> cameraInnerCell{};
  uint32_t fullRebuild = 0u;
  std::array<float, 3> cellSize{};
  uint32_t reserved0 = 0u;
  std::array<float, 3> worldExtent{};
  uint32_t overflowStage = 0u;
};

struct alignas(8) SparseVoxelOccupancyDirtyWordGPU
{
  uint32_t wordIndex = 0u;
  uint32_t bitMask = 0u;
};

struct alignas(16) SparseVoxelOccupancyDispatchGPU
{
  uint32_t reserved0 = 0u;
  uint32_t sourceAABBCount = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

struct alignas(16) SparseVoxelOccupancyAABBGPU
{
  std::array<float, 4> minimum{};
  std::array<float, 4> maximum{};
};

struct alignas(16) SparseVoxelOccupancyProbeGPU
{
  SparseVoxelOccupancyVolumeGPU state{};
  uint32_t probeCount = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

struct alignas(16) SparseVoxelOccupancySourceGPU
{
  uint32_t sourceCapacity = 0u;
  uint32_t instanceCapacity = 0u;
  uint32_t centerCapacity = 0u;
  uint32_t reserved = 0u;
};

struct alignas(16) SparseVoxelOccupancySceneTraversalGPU
{
  uint32_t instanceCount = 0u;
  uint32_t hierarchyNodeCount = 0u;
  uint32_t blasCount = 0u;
  uint32_t traversalLevelCount = 0u;
  uint32_t queueCapacity = 0u;
  uint32_t queryBoxCount = 0u;
  uint32_t depth = 0u;
  uint32_t reserved = 0u;
};

struct alignas(16) SparseVoxelOccupancyMipLevelGPU
{
  std::array<uint32_t, 3> logicalResolution{};
  uint32_t wordsX = 0u;
  std::array<uint32_t, 3> wordResolution{};
  uint32_t level = 0u;
  std::array<int32_t, 3> originCell{};
  uint32_t scale = 1u;
  std::array<uint32_t, 3> wrapOffset{};
  uint32_t wordOffset = 0u;
  std::array<uint32_t, 3> activeResolution{};
  uint32_t wordCount = 0u;
};

struct alignas(16) SparseVoxelOccupancyMipBuildGPU
{
  SparseVoxelOccupancyVolumeGPU baseState{};
  SparseVoxelOccupancyMipLevelGPU childLevel{};
  SparseVoxelOccupancyMipLevelGPU parentLevel{};
  uint32_t dirtyWordCount = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

struct alignas(16) SparseVoxelOccupancyPyramidGPU
{
  uint32_t contentGeneration = 0u;
  uint32_t levelCount = 0u;
  uint32_t valid = 0u;
  uint32_t baseDirtyWordCount = 0u;
  uint32_t mipDirtyWordCount = 0u;
  uint32_t mipWordCount = 0u;
  uint32_t fullRebuild = 0u;
  uint32_t reserved0 = 0u;
};

struct alignas(16) SparseVoxelOccupancyDDARayGPU
{
  std::array<float, 4> originMaximumDistance{};
  std::array<float, 4> direction{};
};

struct alignas(16) SparseVoxelOccupancyDDAResultGPU
{
  std::array<int32_t, 3> globalCell{};
  uint32_t status = 0u;
  float distance = 0.0f;
  float skippedDistance = 0.0f;
  uint32_t queryCount = 0u;
  uint32_t baseCellSteps = 0u;
  uint32_t highestLevel = 0u;
  uint32_t contentGeneration = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct alignas(16) SparseVoxelOccupancyDDADispatchGPU
{
  uint32_t rayCount = 0u;
  uint32_t maximumQueries = 0u;
  uint32_t levelCapacity = 0u;
  uint32_t reserved0 = 0u;
};

// Shared, hit-only ray ABI for the voxel screen-space and Tree64 backends.
// The ray record stays immutable after generation; each backend writes a
// SparseVoxelSceneRayHitGPU and the merge pass selects the final result.
enum class SparseVoxelSceneRayStatus : uint32_t
{
  Invalid = 0u,
  Pending = 1u,
  Hit = 2u,
  MissTMax = 3u,
  EscapedScene = 4u,
  EscapedTreeVolume = 5u,
  UnknownResidency = 6u,
  QueryLimit = 7u,
};

enum class SparseVoxelSceneRayBackend : uint32_t
{
  None = 0u,
  Screen = 1u,
  Tree64 = 2u,
  Hardware = 3u,
};

constexpr uint32_t kSparseVoxelSceneRayFlagActive = 1u << 0u;
constexpr uint32_t kSparseVoxelSceneRayFlagShadow = 1u << 1u;
// A low-rate ray emitted by an otherwise sleeping screen tile. Completed
// point/directional visibility from these rays updates the regional shadow
// heartbeat and can wake the tile when an occluder changes.
constexpr uint32_t kSparseVoxelSceneRayFlagRegionSentinel = 1u << 2u;

constexpr uint32_t kSparseVoxelRegionStabilityDefaultTileSize = 16u;
// Four header words (binary-light masks, wake deadline, reserved), followed
// by one compact rolling visibility distribution for each supported light.
// The renderer deliberately caps regional light tracking at eight entries.
constexpr uint32_t kSparseVoxelRegionStabilityHeaderWords = 4u;
constexpr uint32_t kSparseVoxelRegionStabilityLightCapacity = 8u;
constexpr uint32_t kSparseVoxelRegionStabilityWordsPerTile =
    kSparseVoxelRegionStabilityHeaderWords +
    kSparseVoxelRegionStabilityLightCapacity;

constexpr uint32_t sparseVoxelRegionStabilityTileCount(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelRegionStabilityDefaultTileSize)
{
  const uint32_t size = std::max(tileSize, 1u);
  return ((std::max(width, 1u) + size - 1u) / size) *
         ((std::max(height, 1u) + size - 1u) / size);
}

constexpr uint64_t sparseVoxelRegionStabilityBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelRegionStabilityDefaultTileSize)
{
  return uint64_t{sparseVoxelRegionStabilityTileCount(
             width, height, tileSize)} *
         kSparseVoxelRegionStabilityWordsPerTile * sizeof(uint32_t);
}

// Adaptive per-tile ray budgeting. The scheduler reduces pixel demand into one
// record per screen tile, derives a quota from that tile's need relative to the
// rest of the frame, and then elects exactly that many receivers inside the
// tile. Everything stays GPU-resident: the only host transfer is the tiny
// statistics header below, and only when statistics collection is enabled.
//
// The global header is double-buffered by frame parity. A frame accumulates
// into bank frame&1 while reading the totals bank (frame-1)&1 produced last
// frame, so redistribution needs no second full-screen pass and no barrier
// beyond the one the schedule chain already has.
constexpr uint32_t kSparseVoxelRayBudgetBankWords = 16u;
constexpr uint32_t kSparseVoxelRayBudgetBankCount = 2u;
constexpr uint32_t kSparseVoxelRayBudgetHeaderWords =
    kSparseVoxelRayBudgetBankWords * kSparseVoxelRayBudgetBankCount;
// Words zero through seven are authored by the scheduler; the last two are
// accumulated afterwards by the light-field resolve, which is the pass that
// sees how many of the tile's rays actually came back with a usable answer and
// how many sentinels reported a changed occluder.
constexpr uint32_t kSparseVoxelRayBudgetWordsPerTile = 10u;
// Per-tile demand is accumulated in 1/256 fixed point so the reduction and the
// cross-frame totals can both use integer atomics.
constexpr uint32_t kSparseVoxelRayBudgetDemandScale = 256u;

// Word offsets inside one statistics bank. Kept in sync with
// SPARSE_VOXEL_RAY_BUDGET_STAT_* in sparse_voxel_scene_ray_types.wgsl.
enum class SparseVoxelRayBudgetStat : uint32_t
{
  // Frame index that produced this bank, biased by one so that zero means
  // "never written" and a freshly cleared buffer reads as neutral.
  FrameStamp = 0u,
  CoveredTiles = 1u,
  TotalDemand = 2u,
  TilesAwake = 3u,
  TilesConverged = 4u,
  TotalBudget = 5u,
  RaysScheduled = 6u,
  HeartbeatRays = 7u,
  DisocclusionRays = 8u,
  MotionRays = 9u,
  LightingChangeRays = 10u,
  NoiseRays = 11u,
  UnstableTileRays = 12u,
  ResolvedRays = 13u,
  VisibilityChanges = 14u,
  // Redistribution normalises by covered receivers rather than by tiles, so a
  // partially covered edge tile competes on equal terms with a full one.
  CoveredPixels = 15u,
};

// Why a tile was awake this frame. Stored in the high byte of tile word five
// and mirrored into the readback header, so a debug view can colour tiles by
// the reason that spent their budget.
constexpr uint32_t kSparseVoxelRayBudgetWakeDisocclusion = 1u << 0u;
constexpr uint32_t kSparseVoxelRayBudgetWakeMotion = 1u << 1u;
constexpr uint32_t kSparseVoxelRayBudgetWakeLighting = 1u << 2u;
constexpr uint32_t kSparseVoxelRayBudgetWakeNoise = 1u << 3u;
constexpr uint32_t kSparseVoxelRayBudgetWakeUnconverged = 1u << 4u;
constexpr uint32_t kSparseVoxelRayBudgetWakeHeartbeat = 1u << 5u;

constexpr uint64_t sparseVoxelRayBudgetBufferSize(
    const uint32_t width, const uint32_t height,
    const uint32_t tileSize = kSparseVoxelRegionStabilityDefaultTileSize)
{
  return (uint64_t{kSparseVoxelRayBudgetHeaderWords} +
          uint64_t{sparseVoxelRegionStabilityTileCount(
              width, height, tileSize)} *
              kSparseVoxelRayBudgetWordsPerTile) *
         sizeof(uint32_t);
}

// Host mirror of one statistics bank. Read back only when the sample is
// collecting scene-ray statistics; the renderer itself never reads it.
struct alignas(16) SparseVoxelRayBudgetStatisticsGPU
{
  uint32_t frameStamp = 0u;
  uint32_t coveredTiles = 0u;
  uint32_t totalDemand = 0u;
  uint32_t tilesAwake = 0u;
  uint32_t tilesConverged = 0u;
  uint32_t totalBudget = 0u;
  uint32_t raysScheduled = 0u;
  uint32_t heartbeatRays = 0u;
  uint32_t disocclusionRays = 0u;
  uint32_t motionRays = 0u;
  uint32_t lightingChangeRays = 0u;
  uint32_t noiseRays = 0u;
  uint32_t unstableTileRays = 0u;
  uint32_t resolvedRays = 0u;
  uint32_t visibilityChanges = 0u;
  uint32_t coveredPixels = 0u;
};
static_assert(
    sizeof(SparseVoxelRayBudgetStatisticsGPU) ==
    kSparseVoxelRayBudgetBankWords * sizeof(uint32_t));

// Direct-mapped, four-probe world-region cache used to learn which lights are
// visible from a surface region. Each entry contains generation + key followed
// by one packed visibility statistic per light.
constexpr uint32_t kSparseVoxelLightReachabilityEntryCount = 4096u;
constexpr uint32_t kSparseVoxelLightReachabilityHeaderWords = 2u;

constexpr uint64_t sparseVoxelLightReachabilityBufferSize(
    const uint32_t lightCapacity,
    const uint32_t entryCount = kSparseVoxelLightReachabilityEntryCount)
{
  return uint64_t{std::max(1u, entryCount)} *
         (kSparseVoxelLightReachabilityHeaderWords +
          std::max(1u, lightCapacity)) *
         sizeof(uint32_t);
}

struct alignas(16) SparseVoxelSceneRayGPU
{
  std::array<float, 4> originMinimumDistance{};
  std::array<float, 4> directionMaximumDistance{};
  // x/y source pixel, ray slot, light index.
  std::array<uint32_t, 4> pixelRayLight{};
  // seed, flags, scene generation, world-region reachability key.
  std::array<uint32_t, 4> seedFlags{};
  // Light-selection PDF, emitter-surface PDF, candidate weight, reserved.
  std::array<float, 4> lightSamplePdfWeight{1.0f, 1.0f, 0.0f, 0.0f};
};

struct alignas(16) SparseVoxelSceneRayHitGPU
{
  // xyz hit point, w ray distance.
  std::array<float, 4> hitPositionDistance{};
  // Cell is populated by Tree64; screen hits leave it invalid.
  std::array<int32_t, 3> globalCell{
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min()};
  uint32_t status = static_cast<uint32_t>(SparseVoxelSceneRayStatus::Invalid);
  // deterministic sample identity, light index, backend, scene generation.
  std::array<uint32_t, 4> identity{};
};

// Per-pixel direct-illumination reservoir.  The selected ray is stored in a
// separate dense buffer; this record is the compact ReSTIR/RIS state needed to
// account for the candidates that competed for it.
struct alignas(16) SparseVoxelDIReservoirGPU
{
  // selected light, selected candidate slot, number of candidates (M), scene generation.
  std::array<uint32_t, 4> identity{};
  // sum(w_i), selected target proxy, selected proposal PDF, W = sum(w_i)/(M*pHat).
  std::array<float, 4> weights{};
  // deterministic sample seed, temporal age, validity flags, reserved.
  std::array<uint32_t, 4> state{};
  // Receiver geometry used for temporal/spatial disocclusion validation.
  // xyz is the world-space geometry normal and w is linear depth.
  std::array<float, 4> receiverNormalDepth{};
  // Stable instance and voxel IDs, followed by coverage and reserved.
  std::array<uint32_t, 4> receiverIdentity{};
};

// Small readback-friendly counters authored by the scene-ray debug pass.
struct alignas(16) SparseVoxelSceneRayStatisticsGPU
{
  uint32_t rayCount = 0u;
  uint32_t screenHits = 0u;
  uint32_t screenMisses = 0u;
  uint32_t worldHits = 0u;
  uint32_t worldMisses = 0u;
  uint32_t escapedScene = 0u;
  uint32_t unresolved = 0u;
  uint32_t reservoirM = 0u;
  uint32_t temporalReservoirs = 0u;
  uint32_t spatialReservoirs = 0u;
  uint32_t invalidRays = 0u;
  // `unresolved` above is the sum of these three, which is not enough to act
  // on: they mean different things and have different fixes. Escaping the tree
  // volume is a clipmap sizing problem, unknown residency is the dynamic
  // layer declining to answer, and the query limit is a traversal budget.
  uint32_t escapedTreeVolume = 0u;
  uint32_t unknownResidency = 0u;
  uint32_t queryLimit = 0u;
  // Dedicated hemispherical sky lane, split out from the aggregate counters
  // above.  Direct-light hits can otherwise make screen tracing look healthy
  // while the samples responsible for ambient occlusion never intersect.
  uint32_t skyRayHits = 0u;
  uint32_t skyRayCount = 0u;
  // Diagnostic same-ray backend parity. These are populated only when the
  // compaction pass is asked to keep tracing rays already resolved by the
  // screen-space backend. Production rendering retains the early-out.
  uint32_t screenWorldBothHit = 0u;
  uint32_t screenHitWorldMiss = 0u;
  uint32_t screenHitWorldUnknown = 0u;
  uint32_t screenMissWorldHit = 0u;
  uint32_t tree64InvalidInstanceFlags = 0u;
  uint32_t tree64InvalidDynamicGeneration = 0u;
  uint32_t tree64InvalidModelIndex = 0u;
  uint32_t tree64InvalidTerrainState = 0u;
  uint32_t tree64InvalidTerrainDescriptorBounds = 0u;
  uint32_t tree64InvalidTerrainDescriptorIdentity = 0u;
  uint32_t tree64InvalidTerrainNodePage = 0u;
  uint32_t tree64InvalidTerrainMaterialPage = 0u;
};

// Diagnostic counters authored by the screen-space scene-ray trace. The
// renderer allocates and reads this only when screen-trace statistics are
// explicitly requested, so the shipping path does not pay the atomic cost.
struct alignas(16) SparseVoxelScreenTraceStatisticsGPU
{
  uint32_t activeRays = 0u;
  uint32_t nodeVisits = 0u;
  uint32_t leafVisits = 0u;
  uint32_t maximumLeafVisits = 0u;
  uint32_t hits = 0u;
  uint32_t missTMax = 0u;
  uint32_t escapedScene = 0u;
  uint32_t shortSegments = 0u;
  uint32_t planeHits = 0u;
  uint32_t slabHits = 0u;
  uint32_t receiverIdentityRejects = 0u;
  uint32_t receiverSheetRejects = 0u;
};

struct alignas(16) SparseVoxelTemporalSceneRayHitGPU
{
  SparseVoxelSceneRayHitGPU hit{};
  // xyz receiver origin, w accumulated visibility mean.
  std::array<float, 4> receiverVisibility{};
  // visibility second moment, confidence, variance, reserved.
  std::array<float, 4> momentsConfidence{};
  // sample count, content generation, stable light handle, reserved.
  std::array<uint32_t, 4> state{};
};

struct alignas(16) SparseVoxelRasterUniformsGPU
{
  std::array<float, 16> worldToCamera{};
  std::array<float, 16> cameraToWorld{};
  std::array<float, 4> cameraWorldPositionNear{};
  std::array<float, 4> focalPrincipal{};
  std::array<uint32_t, 4> viewportTiles{};
  uint32_t visibleVoxelCapacity = 0u;
  uint32_t projectedVoxelCapacity = 0u;
  uint32_t candidateCapacity = 0u;
  uint32_t tileRangeCapacity = 0u;
  std::array<float, 16> worldToClip{};
  std::array<float, 16> previousWorldToClip{};
  uint32_t gbufferFlags = 0u;
  float gbufferClearDepth = 0.0f;
  float lodPixelThreshold = 8.0f;
  uint32_t lodEnabled = 0u;
};

struct alignas(16) SparseVoxelRadixUniformsGPU
{
  uint32_t shift = 0u;
  uint32_t candidateCapacity = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct SparseVoxelRadixDispatchArgsGPU
{
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t z = 0u;
};

static_assert(std::is_standard_layout_v<SparseVoxelBLASGPU>);
static_assert(sizeof(SparseVoxelBLASGPU) == 32u);
static_assert(sizeof(SparseVoxelPathLevelGPU) == 16u);
static_assert(sizeof(SparseVoxelCenterLengthGPU) == 16u);
static_assert(sizeof(SparseVoxelMaterialGPU) == 32u);
static_assert(sizeof(SparseVoxelMicroMaterialGPU) == 8u);
static_assert(sizeof(SparseVoxelInstanceGPU) == 240u);
static_assert(sizeof(SparseVoxelTLASNodeGPU) == 48u);
static_assert(sizeof(SparseVoxelBLASRootRangeGPU) == 16u);
static_assert(sizeof(SparseVoxelVisibilityOffsetGPU) == 8u);
static_assert(sizeof(SparseVoxelTLASScanBlockGPU) == 16u);
static_assert(sizeof(TLASQueueElementGPU) == 16u);
static_assert(sizeof(VisibleInstanceGPU) == 16u);
static_assert(sizeof(VisibleVoxelGPU) == 16u);
static_assert(sizeof(SparseVoxelLODNodeGPU) == 16u);
static_assert(sizeof(SparseVoxelResidentNodeMetadataGPU) == 16u);
static_assert(sizeof(SparseVoxelLODQueueElementGPU) == 16u);
static_assert(sizeof(SparseVoxelLODRecoveryStateGPU) == 16u);
static_assert(sizeof(SparseVoxelLODSeedGPU) == 16u);
static_assert(sizeof(SparseVoxelPageRequestGPU) == 16u);
static_assert(sizeof(SparseVoxelPhysicalPageRewriteGPU) == 16u);
static_assert(sizeof(SparseVoxelPageBoundsGPU) == 32u);
static_assert(sizeof(ProjectedVoxelGPU) == 32u);
static_assert(sizeof(TileCandidateGPU) == 32u);
static_assert(sizeof(SparseVoxelTileRangeGPU) == 16u);
static_assert(sizeof(ResolvedVoxelSurfaceGPU) == 32u);
static_assert(sizeof(SparseVoxelGBufferPixelGPU) == 128u);
static_assert(sizeof(SparseVoxelExactOutputPixelGPU) == 192u);
static_assert(sizeof(SparseVoxelDebugPresentUniformsGPU) == 96u);
static_assert(sizeof(IndirectDispatchArgsGPU) == 16u);
static_assert(sizeof(SparseVoxelFrameCountersGPU) == 32u);
static_assert(sizeof(SparseVoxelBootstrapUniformsGPU) == 16u);
static_assert(sizeof(SparseVoxelLODPassUniformsGPU) == 96u);
static_assert(sizeof(SparseVoxelTLASCullingUniformsGPU) == 192u);
static_assert(sizeof(SparseVoxelStreamingPassUniformsGPU) == 16u);
static_assert(sizeof(SparseVoxelOccupancyVolumeGPU) == 144u);
static_assert(sizeof(SparseVoxelOccupancyDirtyWordGPU) == 8u);
static_assert(sizeof(SparseVoxelOccupancyDispatchGPU) == 16u);
static_assert(sizeof(SparseVoxelOccupancyAABBGPU) == 32u);
static_assert(sizeof(SparseVoxelOccupancyProbeGPU) == 160u);
static_assert(sizeof(SparseVoxelOccupancySourceGPU) == 16u);
static_assert(sizeof(SparseVoxelOccupancySceneTraversalGPU) == 32u);
static_assert(sizeof(SparseVoxelOccupancyMipLevelGPU) == 80u);
static_assert(sizeof(SparseVoxelOccupancyMipBuildGPU) == 320u);
static_assert(sizeof(SparseVoxelOccupancyPyramidGPU) == 32u);
static_assert(sizeof(SparseVoxelOccupancyDDARayGPU) == 32u);
static_assert(sizeof(SparseVoxelOccupancyDDAResultGPU) == 48u);
static_assert(sizeof(SparseVoxelOccupancyDDADispatchGPU) == 16u);
static_assert(sizeof(SparseVoxelSceneRayGPU) == 80u);
static_assert(sizeof(SparseVoxelSceneRayHitGPU) == 48u);
static_assert(sizeof(SparseVoxelDIReservoirGPU) == 80u);
static_assert(sizeof(SparseVoxelSceneRayStatisticsGPU) == 112u);
static_assert(sizeof(SparseVoxelScreenTraceStatisticsGPU) == 48u);
static_assert(sizeof(SparseVoxelTemporalSceneRayHitGPU) == 96u);
static_assert(sizeof(SparseVoxelRasterUniformsGPU) == 336u);
static_assert(sizeof(SparseVoxelRadixUniformsGPU) == 16u);
static_assert(sizeof(SparseVoxelRadixDispatchArgsGPU) == 12u);

static_assert(alignof(SparseVoxelBLASGPU) == 16u);
static_assert(alignof(SparseVoxelInstanceGPU) == 16u);
static_assert(offsetof(SparseVoxelBLASGPU, firstVoxel) == 16u);
static_assert(offsetof(SparseVoxelMaterialGPU, materialId) == 16u);
static_assert(offsetof(SparseVoxelInstanceGPU, worldToModel) == 64u);
static_assert(offsetof(SparseVoxelInstanceGPU, previousModelToWorld) == 128u);
static_assert(offsetof(SparseVoxelInstanceGPU, worldBoundsMin) == 192u);
static_assert(offsetof(SparseVoxelInstanceGPU, worldBoundsMax) == 208u);
static_assert(offsetof(SparseVoxelInstanceGPU, blasIndex) == 224u);
static_assert(offsetof(SparseVoxelTLASCullingUniformsGPU, occupancyQueryMinimum) == 80u);
static_assert(offsetof(SparseVoxelTLASCullingUniformsGPU, occupancyQueryMaximum) == 128u);
static_assert(offsetof(SparseVoxelTLASCullingUniformsGPU, occupancyQueryBoxCount) == 176u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, cameraToWorld) == 64u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, cameraWorldPositionNear) == 128u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, worldToClip) == 192u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, previousWorldToClip) == 256u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, gbufferFlags) == 320u);
static_assert(offsetof(SparseVoxelRasterUniformsGPU, gbufferClearDepth) == 324u);
static_assert(offsetof(SparseVoxelTLASNodeGPU, firstChild) == 32u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, wordResolution) == 16u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, cameraCell) == 32u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, originCell) == 48u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, scrollDelta) == 64u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, wrapOffset) == 80u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, cameraInnerCell) == 96u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, cellSize) == 112u);
static_assert(offsetof(SparseVoxelOccupancyVolumeGPU, worldExtent) == 128u);
static_assert(offsetof(SparseVoxelOccupancyMipLevelGPU, wordResolution) == 16u);
static_assert(offsetof(SparseVoxelOccupancyMipLevelGPU, originCell) == 32u);
static_assert(offsetof(SparseVoxelOccupancyMipLevelGPU, wrapOffset) == 48u);
static_assert(offsetof(SparseVoxelOccupancyMipLevelGPU, activeResolution) == 64u);
static_assert(offsetof(SparseVoxelOccupancyMipBuildGPU, childLevel) == 144u);
static_assert(offsetof(SparseVoxelOccupancyMipBuildGPU, parentLevel) == 224u);
static_assert(offsetof(SparseVoxelOccupancyMipBuildGPU, dirtyWordCount) == 304u);
static_assert(offsetof(SparseVoxelOccupancyPyramidGPU, mipDirtyWordCount) == 16u);
static_assert(offsetof(ProjectedVoxelGPU, instanceIndex) == 16u);
static_assert(offsetof(TileCandidateGPU, quadrant) == 16u);
static_assert(offsetof(ResolvedVoxelSurfaceGPU, hitFlags) == 16u);

} // namespace voxel::svraster
