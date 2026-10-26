#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace voxel::material
{
class VoxelMaterialManager;
}

namespace voxel::svraster
{

// Brick hierarchy used by the G-buffer resolver.
//
// A brick is an 8x8x8 block of voxels stored as 512 occupancy bits plus one
// material index per occupied voxel, addressed by the popcount of the bits
// before it. That keeps appearance exact - unlike the Tree64 clipmap, which
// stores occupancy only - while costing the same one word per voxel the octree
// material array already costs.
//
// Bricks form an octree: a node at level L+1 covers the same world volume as
// eight level-L children at half the voxel size, and its occupancy is their
// OR-reduction. That is a mip chain, not a simplification chain, so the LOD cut
// a culling pass selects is crack-free by construction: any cut through the
// tree covers the volume exactly once.
//
// This is built in memory either from resident scene leaves or directly from
// independently streamed planet-package patches. The first terrain path keeps
// a camera-centered multi-root chunk window resident and republishes entering
// rows through the existing GPU buffers whenever their capacity is sufficient.

inline constexpr uint32_t kSparseVoxelBrickSide = 8u;
inline constexpr uint32_t kSparseVoxelBrickVoxelCount =
    kSparseVoxelBrickSide * kSparseVoxelBrickSide * kSparseVoxelBrickSide;
inline constexpr uint32_t kSparseVoxelBrickOccupancyWords = kSparseVoxelBrickVoxelCount / 32u;
// One coarse bit per 2x2x2 block of voxels: 4^3 = 64 bits for the whole brick.
inline constexpr uint32_t kSparseVoxelBrickCoarseSide = kSparseVoxelBrickSide / 2u;
inline constexpr uint32_t kSparseVoxelBrickInvalidIndex = 0xffffffffu;
inline constexpr uint32_t kSparseVoxelBrickTerrainClipFlag = 1u << 31u;
inline constexpr uint32_t kSparseVoxelBrickTerrainClipMask = 0x7fu;
inline constexpr uint32_t kSparseVoxelBrickTerrainClipQuantization = 8u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupInvalidIndex = 0xffffffffu;
inline constexpr uint32_t kSparseVoxelBrickLODGroupDefaultSideBricks = 2u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryMinusX = 1u << 0u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryPlusX = 1u << 1u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryMinusY = 1u << 2u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryPlusY = 1u << 3u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryMinusZ = 1u << 4u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryPlusZ = 1u << 5u;
inline constexpr uint32_t kSparseVoxelBrickLODGroupBoundaryMask = 0x3fu;
inline constexpr uint32_t kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified = 1u << 6u;
// Spatial terrain clipmaps already choose their resolution by selecting a
// page cascade. Rasterizing an OR-reduced brick parent on top of that spatial
// LOD invents occupied cells that neither the authored leaves nor Tree64 have.
// Bit 16 is outside the hierarchy-local and clipmap-LOD bytes in `level`.
inline constexpr uint32_t kSparseVoxelBrickForceLeafRaster = 1u << 16u;
// Floor on measured brick error, as a fraction of the brick's own voxel. A
// brick can be geometrically exact - flat ground reduces with no deviation at
// all - while still carrying material detail the coarser level cannot show, so
// zero error would pin the cut to the coarsest node at any distance.
inline constexpr float kSparseVoxelBrickErrorFloor = 0.25f;

// Culling flags for SparseVoxelBrickCullUniformsGPU::cullingFlags.
//
// These are deliberately *not* the kSparseVoxelCulling* constants the SVRaster
// chain uses. Those number reverse-Z as bit 4; the brick shader has always read
// it as bit 2, so passing the SVRaster constant left the occlusion test running
// its forward-Z comparison against a reverse-Z pyramid - comparing the box's
// farthest corner against the pyramid's nearest occluder, which rejects
// genuinely visible bricks and changes its mind as the depth buffer moves.
// Separate constants beside the shader they belong to make the mismatch
// impossible to reintroduce.
inline constexpr uint32_t kSparseVoxelBrickCullFrustum = 1u << 0u;
inline constexpr uint32_t kSparseVoxelBrickCullOcclusion = 1u << 1u;
inline constexpr uint32_t kSparseVoxelBrickCullReverseZ = 1u << 2u;
// Diagnostic: accumulate the visible bricks' screen area. Costs a contended
// global atomic per surviving brick, so it stays off outside a profiling run.
inline constexpr uint32_t kSparseVoxelBrickCullMeasureArea = 1u << 3u;
// Diagnostic: clear the frame-wide fragment traversal counter bank before
// phase zero. The shipping draw shader never writes those counters, so its
// cull reset must not pay for clearing them either.
inline constexpr uint32_t kSparseVoxelBrickCullRasterDiagnostics = 1u << 4u;
// Diagnostic: hash the complete selected brick LOD cut before visibility
// rejection. Enabled only by terrain tracing because it adds global atomics.
inline constexpr uint32_t kSparseVoxelBrickCullLODDiagnostics = 1u << 5u;

// The optional exact-micro distance shader stores its world-metre cutoff in
// debugFlags.y as raw f32 bits. Zero selects the legacy shader instead, so the
// default path neither decodes the field nor executes an extra comparison.
inline bool sparseVoxelBrickExactMicroDistanceEnabled(const float metres)
{
  return std::isfinite(metres) && metres > 0.0f;
}

inline uint32_t sparseVoxelBrickExactMicroDistanceBits(const float metres)
{
  const float sanitized =
      sparseVoxelBrickExactMicroDistanceEnabled(metres) ? metres : 0.0f;
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(sanitized));
  std::memcpy(&bits, &sanitized, sizeof(bits));
  return bits;
}

// Largest page voxel size allowed to carve exact microgeometry, as raw f32
// bits. Non-finite or non-positive values disable the limit.
inline uint32_t sparseVoxelBrickExactMicroMaxVoxelBits(const float metres)
{
  const float sanitized =
      std::isfinite(metres) && metres > 0.0f ? metres : 0.0f;
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(sanitized));
  std::memcpy(&bits, &sanitized, sizeof(bits));
  return bits;
}

// Exact-microgeometry handoff scale, as raw f32 bits for the draw uniform.
// The descriptor's own lodHandoff is clamped to [0.25, 1.0] and can therefore
// only retire exact carving *later*; this divides the projected microvoxel
// size, so values above one retire it at proportionally greater distance. One
// is bit-exact with the historical behaviour.
inline uint32_t sparseVoxelMicroHandoffScaleBits(const float scale)
{
  const float sanitized = std::isfinite(scale) && scale > 1.0f
      ? std::min(scale, 16.0f)
      : 1.0f;
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(sanitized));
  std::memcpy(&bits, &sanitized, sizeof(bits));
  return bits;
}

// H5 routing threshold, as raw f32 bits. Projected occupied-box footprint in
// pixels at or below which a brick is owned by the tiled compute raster. Zero
// keeps every brick on the hardware path, which is the shipping default.
inline uint32_t sparseVoxelBrickRouteToComputeBits(const float pixels)
{
  const float sanitized =
      std::isfinite(pixels) && pixels > 0.0f ? pixels : 0.0f;
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(sanitized));
  std::memcpy(&bits, &sanitized, sizeof(bits));
  return bits;
}

// Temporal cull-state packing. Low bits retain the occlusion agreement count;
// high bits double-buffer LOD ownership so the parallel GPU cut never reads a
// partially published parent/child decision.
inline constexpr uint32_t kSparseVoxelBrickHistoryOcclusionMask = 3u;
inline constexpr uint32_t kSparseVoxelBrickHistoryLODInitialized = 1u << 29u;
inline constexpr uint32_t kSparseVoxelBrickHistoryPreviousLOD = 1u << 30u;
inline constexpr uint32_t kSparseVoxelBrickHistoryPendingLOD = 1u << 31u;

// One entry per 32 voxels: the occupancy bits, plus the popcount of every bit
// before this word.
//
// The prefix is what makes the material lookup a single load. Addressing a
// voxel's material by the popcount of the bits preceding it otherwise means
// summing up to fifteen words at the end of every DDA that hits - fifteen
// dependent storage loads and fifteen popcounts, in the fragment shader.
struct SparseVoxelBrickOccupancyCellGPU
{
  uint32_t bits = 0u;
  uint32_t prefix = 0u;
};

struct alignas(16) SparseVoxelBrickOccupancyGPU
{
  std::array<SparseVoxelBrickOccupancyCellGPU, kSparseVoxelBrickOccupancyWords> cells{};
};

// Appearance for one occupied voxel, in eight bytes.
//
// The octree material record is 32 bytes reached through a 4-byte index, so a
// shading fragment pays a 4-byte load, a dependent 32-byte load, and a cache
// miss for a scattered index. Voxels need very little of that, so the fields
// are packed inline instead: the indirection disappears and the record shrinks
// by 4x.
//
//   packed[0] : R (0..7) | G (8..15) | B (16..23) | roughness (24..31)
//   packed[1] : emissive (0..7) | metallic (8..15) |
//               transient face-hit (16..19) |
//               bottom palette index (20..27) |
//               top/bottom blend (28) | side blend (29) |
//               secondary enabled (30) | micro enabled (31)
//
// Stored palette entries leave bits 16..19 clear. The draw pass writes its
// four-bit surface tag there in the visibility attachment. Per-voxel terrain
// context lives in the parallel micro-context word instead. The representation
// uses ten of the remaining twelve bits for a second index into the same
// 256-entry palette plus two face-category modes. Bit 30 distinguishes an
// authored secondary layer from legacy records whose reserved words are zero.
struct SparseVoxelBrickMaterialGPU
{
  std::array<uint32_t, 2> packed{};
};

inline constexpr uint32_t kSparseVoxelBrickSurfaceShift = 16u;
inline constexpr uint32_t kSparseVoxelBrickTransientMask =
    0x0fu << kSparseVoxelBrickSurfaceShift;
inline constexpr uint32_t kSparseVoxelBrickSecondaryPaletteShift = 20u;
inline constexpr uint32_t kSparseVoxelBrickSecondaryPaletteMask = 0xffu;
inline constexpr uint32_t kSparseVoxelBrickBlendTopBottomFaces = 1u << 28u;
inline constexpr uint32_t kSparseVoxelBrickBlendSideFaces = 1u << 29u;
inline constexpr uint32_t kSparseVoxelBrickSecondaryEnabled = 1u << 30u;
inline constexpr uint32_t kSparseVoxelBrickMicroEnabled = 1u << 31u;
inline constexpr uint32_t kSparseVoxelBrickSecondaryMetadataMask =
    (kSparseVoxelBrickSecondaryPaletteMask <<
     kSparseVoxelBrickSecondaryPaletteShift) |
    kSparseVoxelBrickBlendTopBottomFaces |
    kSparseVoxelBrickBlendSideFaces |
    kSparseVoxelBrickSecondaryEnabled;

struct alignas(16) SparseVoxelBrickNodeGPU
{
  // xyz: world-space minimum corner of the brick. w: voxel size at this level,
  // so the brick spans w * 8 on each axis.
  std::array<float, 4> minimumVoxelSize{};
  // Inclusive occupied cell range packed as six 3-bit fields
  // (minX, minY, minZ, maxX, maxY, maxZ). A brick holding a wall slab occupies
  // a thin slice of its 8-cubed box, so drawing and marching the full box is
  // mostly empty space: this is the tight box to use instead.
  uint32_t packedOccupiedBounds = 0u;
  // 4^3 coarse occupancy, bit (x + 4y + 16z) set when any of the eight voxels
  // under it is occupied. Low word holds z < 2, high word z >= 2.
  //
  // It lives in the node rather than beside the occupancy words so the vertex
  // shader can forward it as two flat varyings: the fragment then skips empty
  // space with no memory traffic at all, and only touches the occupancy buffer
  // inside a coarse cell that has something in it.
  uint32_t coarseOccupancyLow = 0u;
  uint32_t coarseOccupancyHigh = 0u;
  // Index into the occupancy array, and the base of this brick's run in the
  // material array. These five fields are everything the vertex shader
  // forwards, so they are kept adjacent.
  uint32_t occupancyIndex = 0u;
  uint32_t materialBase = 0u;
  // Lets the culling pass decide "this node is the cut" from two independent
  // tests - my detail is fine enough, my parent's was not - instead of walking
  // the tree. The LOD metric is monotonic (a parent's voxel is exactly twice a
  // child's), so that selects a crack-free cut with no queue and no barriers.
  // The parent's *bounds* are derived arithmetically rather than fetched; see
  // brickParentBounds() in the culling shader.
  uint32_t parentIndex = kSparseVoxelBrickInvalidIndex;
  uint32_t childCount = 0u;
  // Low byte is the hierarchy-local LOD. Combined terrain clipmaps store their
  // ring LOD in the next byte so the draw pass can reserve exact microgeometry
  // for the canonical finest representation. Bit 16 is the compatibility
  // fallback that forces terrain to rasterize authored leaves instead of
  // measured-error hierarchy parents.
  uint32_t level = 0u;
  uint32_t occupiedCount = 0u;
  // Offset into SparseVoxelBrickHierarchy::childIndices. Nodes are emitted in
  // spatial-key order per level, so a node's children are not adjacent in the
  // node array and cannot be addressed by a base plus popcount.
  uint32_t firstChild = 0u;
  // Instance this brick's voxels came from, or kSparseVoxelBrickInvalidIndex
  // when the build was given no instance mapping. Translated planet pages do
  // not need an instance palette selector (paletteBase is already explicit),
  // so bit 31 instead tags four packed 7-bit XZ clip coordinates there.
  //
  // The transform is already baked into the world-space grid, so this is not
  // used to place anything - it is the selector for per-instance data, starting
  // with the colour palette a voxel's material byte indexes into. A coarse
  // brick can straddle two instances; it keeps the one that contributed most of
  // its occupied cells, because a palette lookup needs a single answer.
  uint32_t instanceIndex = kSparseVoxelBrickInvalidIndex;
  // First palette entry this brick's voxel indices are relative to.
  //
  // Usually `instanceIndex * paletteStride`, but stored explicitly because a
  // brick whose cells come from more than one instance gets a palette of its
  // own. Those exist: on Sponza split per material, 11.9% of bricks straddle
  // and 5.3% of all voxels sit in the minority instance of one. Resolving them
  // through the majority's palette is what produced the whole error tail
  // (max 34 -> 166 over the finest level alone).
  //
  // The shader needs only the base - it indexes `palette[base + index]` and the
  // index is a byte - so the entry count stays a build-time concern.
  uint32_t paletteBase = 0u;
  // Stable logical field-mapping identity. Zero selects the material-global
  // mapping. Planet packing supplies a deterministic per-region ID; this is
  // never a transient clipmap or residency slot.
  uint32_t regionMappingId = 0u;
  // Measured deviation of this brick from the finest representation of the same
  // volume, in world units, and the running maximum over its ancestors.
  //
  // Not derived from voxel size. A coarsened flat plain deviates by almost
  // nothing while a coarsened cliff loses most of a voxel, and a metric built
  // from voxel size alone cannot tell them apart - it spends the same detail on
  // ground that carries none. selfError is what the cut divides by distance;
  // parentError is the same quantity for the ancestor above, so selection stays
  // two independent tests with no traversal.
  //
  // Monotonic by construction: a parent's error is at least the maximum of its
  // children's. Without that the two-test cut is no longer crack-free, because
  // a monotonic metric is what admits exactly one cut per root-to-leaf path.
  // Monotonicity used to be free - a parent's voxel was exactly twice its
  // child's - and that guarantee disappears the moment error stops being a
  // function of voxel size.
  //
  // These three words occupy what was implicit tail padding before the 16-byte
  // alignment of canonicalGridOriginScale. Implicit padding is never written,
  // and whole nodes are hashed into PlanetVoxelChunkRecord::brickPayloadRevision
  // and serialized into the transient page store, so the heap garbage in the gap
  // made a page's identity depend on what had been allocated before it. Naming
  // them costs no space: the struct is still 96 bytes and the WGSL side still
  // pads implicitly to the same layout.
  float selfError = 0.0f;
  // Roots have no ancestor, so nothing can coarsen past them.
  float parentError = std::numeric_limits<float>::max();
  uint32_t reservedTailPadding = 0u;
  // Integer origin of this brick in the hierarchy's canonical finest-parent
  // grid. xyz never comes from a world-space float division; the brick builder
  // derives it from its signed spatial key and terrain packing applies a
  // double-precision page translation. w is the number of finest parents
  // represented by one cell at this node. Exact microgeometry uses this
  // address so adjacent bricks/pages share one procedural coordinate system.
  alignas(16) std::array<int32_t, 4> canonicalGridOriginScale{};
};
static_assert(sizeof(SparseVoxelBrickNodeGPU) == 96u);
static_assert(offsetof(SparseVoxelBrickNodeGPU, canonicalGridOriginScale) == 80u);

// Coarse virtual-geometry-style group metadata for brick cuts.
//
// A brick node remains the exact raster/tracing payload. Groups are a sidecar
// selection layer: several adjacent bricks at the same hierarchy level share a
// projected-error decision, so LOD transitions can happen as stable patches
// inside a chunk rather than as unrelated per-brick flips along chunk edges.
//
// The bounds are implicit regular-grid bounds in the hierarchy's canonical
// finest grid. `minimumExtent.xyz` is the world-space group minimum and `.w`
// is the full cubic side length. Children/material/occupancy still live on
// SparseVoxelBrickNodeGPU; `firstNode/nodeCount` indexes
// SparseVoxelBrickHierarchy::lodGroupNodeIndices.
//
// Low six bits of `flags` mark group faces where occupied member voxels touch
// the outer macro-cell boundary. Those faces are silhouette-sensitive: the
// group-driven cut must either match the neighbouring/parent edge resolution
// or use an edge-locked seam representation before accepting this group as a
// coarse patch. Bit 6 certifies exactly that for generated terrain: the
// representative is a conservative surface parent built from the same stitched
// page boundaries as its children, so the culler may coarsen non-full/boundary
// terrain parents without falling back to whole-chunk LOD.
//
// `representativeNode` is the simplified top node for parent/child groups.
// Generic grid buckets may leave it invalid; terrain group cuts should prefer
// parent-backed groups so a 2x2 XZ child patch can coarsen to one known parent
// whose border samples define the shared silhouette.
struct alignas(16) SparseVoxelBrickLODGroupGPU
{
  std::array<float, 4> minimumExtent{};
  std::array<int32_t, 4> canonicalGridOriginScale{};
  uint32_t firstNode = 0u;
  uint32_t nodeCount = 0u;
  uint32_t level = 0u;
  // Low six bits carry kSparseVoxelBrickLODGroupBoundaryMask; bit 6 carries
  // kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified.
  uint32_t flags = 0u;
  float sharedError = 0.0f;
  float minimumError = 0.0f;
  float maximumError = 0.0f;
  uint32_t representativeNode = kSparseVoxelBrickLODGroupInvalidIndex;
};
static_assert(sizeof(SparseVoxelBrickLODGroupGPU) == 64u);

inline bool sparseVoxelBrickHasFinerTerrainClip(
    const SparseVoxelBrickNodeGPU &node)
{
  return node.instanceIndex != kSparseVoxelBrickInvalidIndex &&
      (node.instanceIndex & kSparseVoxelBrickTerrainClipFlag) != 0u;
}

inline std::array<float, 4> sparseVoxelBrickFinerTerrainClipBounds(
    const SparseVoxelBrickNodeGPU &node)
{
  if (!sparseVoxelBrickHasFinerTerrainClip(node))
    return {};
  const float step = node.minimumVoxelSize[3] /
      static_cast<float>(kSparseVoxelBrickTerrainClipQuantization);
  return {
    node.minimumVoxelSize[0] +
        static_cast<float>(node.instanceIndex &
                           kSparseVoxelBrickTerrainClipMask) * step,
    node.minimumVoxelSize[2] +
        static_cast<float>((node.instanceIndex >> 7u) &
                           kSparseVoxelBrickTerrainClipMask) * step,
    node.minimumVoxelSize[0] +
        static_cast<float>((node.instanceIndex >> 14u) &
                           kSparseVoxelBrickTerrainClipMask) * step,
    node.minimumVoxelSize[2] +
        static_cast<float>((node.instanceIndex >> 21u) &
                           kSparseVoxelBrickTerrainClipMask) * step,
  };
}

struct alignas(16) SparseVoxelBrickCullUniformsGPU
{
  std::array<float, 16> worldToClip{};
  std::array<float, 4> cameraWorldPosition{};
  std::array<float, 4> focalPrincipal{};
  std::array<uint32_t, 4> viewport{};
  uint32_t rootCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t visibleCapacity = 0u;
  uint32_t lodGroupCount = 0u;
  // Projected voxel size, in pixels, below which a node is drawn rather than
  // refined. Matches the octree path's lodPixelThreshold in spirit.
  float lodPixelThreshold = 4.0f;
  uint32_t cullingFlags = 0u;
  uint32_t depth = 0u;
  uint32_t levelCount = 0u;
  uint32_t hzbMipCount = 1u;
  // The pyramid's mip 0 is the reduce-to-power-of-two staging level, not a
  // reduction, so the usable chain starts above it - same convention the LOD
  // pass follows.
  uint32_t hzbFirstValidMip = 0u;
  // 0 = draw what was visible last frame (frustum only), 1 = draw the rest
  // against the pyramid built from phase one. Two-phase is what makes
  // occlusion culling usable while the camera moves: the pyramid it tests
  // against is built from this frame's view, not the previous one.
  uint32_t phase = 0u;
  // Fraction of a brick's own voxel size that the occlusion test moves its
  // nearest point toward the eye before comparing against the pyramid. An
  // LOD-coarse brick rasterizes a surface dilated by up to one of its own
  // voxels, so the depth it contributes is nearer than the true surface and
  // culls whatever legitimately peeks past it. One voxel is the exact bound.
  float occlusionDilationVoxels = 1.0f;
  // Stable streamed-terrain page slots address two GPU feedback buffers. Zero
  // disables terrain requests for ordinary brick scenes.
  uint32_t terrainPageRequestCapacity = 0u;
  // Mirror of SparseVoxelBrickDrawUniformsGPU::microTuning[1], the H5 routing
  // threshold, so the binner and the draw agree on brick ownership.
  uint32_t routeToComputePixelsBits = 0u;
  // Fixed generated-world chunk feedback. The ordinary hierarchy threshold
  // selects the cut inside the committed payload; these thresholds add the
  // asynchronous page split/merge dead band around the same error metric.
  float terrainSplitErrorPixels = 4.0f;
  float terrainMergeErrorPixels = 1.5f;
  float terrainOutputPixelScale = 1.0f;
  uint32_t terrainMinimumResidentFrames = 4u;
  uint32_t terrainFrameIndex = 0u;
  uint32_t terrainFeedbackEnabled = 0u;
  // Generated-world hierarchy error is divided by
  // 1 + growth * distanceKilometres^exponent. Zero growth preserves the
  // ordinary projected-pixel LOD metric exactly.
  float terrainDistanceErrorGrowthPerKilometre = 0.10f;
  float terrainDistanceErrorExponent = 1.0f;
  // Non-zero only when the previous HZB was built for the same view and can be
  // used to trim the phase-zero occluder seed safely.
  uint32_t hzbHistoryValid = 0u;
  // WGSL uniform vec4 members are 16-byte aligned. Keep the canonical split at
  // the same offset in the CPU mirror instead of relying on implicit tail
  // padding after the scalar terrain controls above.
  uint32_t canonicalPadding = 0u;
  // Generated streamed terrain uses the same canonical camera-relative
  // lattice as the draw shader. Culling in absolute f32 world coordinates at
  // planetary distances makes LOD/frustum/HZB projection disagree with the
  // geometry that is actually rasterized.
  std::array<float, 4> canonicalWorldGrid{};
  std::array<int32_t, 4> canonicalCameraCell{};
  std::array<float, 4> canonicalCameraOffset{};
};

static_assert(offsetof(SparseVoxelBrickCullUniformsGPU, canonicalWorldGrid) %
                  16u ==
              0u);

// Per stable terrain slot. `publishedFrame` is compared against the current
// cull frame on the GPU; `flags` deliberately distinguishes fixed generated
// chunks from packaged planet clipmap rings.
struct alignas(16) SparseVoxelTerrainPageStateGPU
{
  uint32_t generation = 0u;
  uint32_t currentLOD = 0u;
  uint32_t publishedFrame = 0u;
  uint32_t flags = 0u;
};

inline constexpr uint32_t kSparseVoxelTerrainPageStateProgressiveGenerated =
    1u << 0u;
static_assert(sizeof(SparseVoxelTerrainPageStateGPU) == 16u);

inline constexpr uint32_t kSparseVoxelTerrainFeedbackRequested = 1u << 0u;

struct alignas(16) SparseVoxelTerrainFeedbackRecordGPU
{
  uint32_t slot = 0u;
  uint32_t generation = 0u;
  uint32_t priority = 0u;
  uint32_t desiredLOD = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};
static_assert(sizeof(SparseVoxelTerrainFeedbackRecordGPU) == 32u);

struct alignas(16) SparseVoxelBrickDrawUniformsGPU
{
  std::array<float, 16> worldToClip{};
  std::array<float, 16> cameraToWorld{};
  std::array<float, 4> cameraWorldPosition{};
  std::array<float, 4> focalPrincipal{};
  std::array<uint32_t, 4> viewport{};
  // x: count fragment invocations per pixel for the overdraw view.
  // y: optional exact-micro distance cutoff in metres, stored as raw f32 bits.
  //    Read only by the separately compiled distance-cutoff shader.
  // z: collect exact micro-geometry traversal counters. Debug only; every
  // fragment-side atomic is gated by this bit.
  // w: output-pixels-per-raster-pixel in unsigned Q16. This keeps the exact
  // microgeometry handoff tied to display pixels when an upscaler is active.
  std::array<uint32_t, 4> debugFlags{};
  // x: exact-micro handoff scale as raw f32 bits; see the WGSL mirror. Divides
  // the projected microvoxel size, so values above one retire exact
  // microgeometry at proportionally greater distance. Zero and one are both
  // the historical behaviour.
  // z: largest page voxel size, in metres as raw f32 bits, still allowed to
  // carve exact microgeometry. Zero leaves it unlimited.
  //
  // The projected-size handoff divides the *page's* voxel size by the
  // subdivision, so a coarse clipmap ring projects larger microvoxels than a
  // fine ring at a fraction of the distance and keeps carving out to the
  // horizon. Extending the render distance from 25 km to 42 km therefore cut
  // eligible entries 15.72 M -> 13.29 M while raising exact traces
  // 4.62 M -> 7.02 M. A distance cutoff cannot express this: the offending
  // pages are far away *and* coarse, and it is the coarseness that makes their
  // microgeometry fictional - the page's own source data is that voxel size.
  // w: gap-only tiled compute raster. Covered hardware pixels skip traversal.
  std::array<uint32_t, 4> microTuning{};
  // xyz: renderer-world origin of the generated terrain's canonical integer
  // lattice; w: finest lattice spacing. Zero disables canonical geometry.
  std::array<float, 4> canonicalWorldGrid{};
  // Camera split on that lattice. xyz is the integral finest-cell coordinate;
  // canonicalCameraOffset.xyz is the remaining sub-cell displacement in
  // metres. Generated terrain subtracts this split before converting to f32,
  // keeping 10 cm planes exact even hundreds of kilometres from world origin.
  std::array<int32_t, 4> canonicalCameraCell{};
  std::array<float, 4> canonicalCameraOffset{};
};

// Diagnostic counters shared by both brick draw phases. These intentionally
// live outside SparseVoxelBrickCounters: the cull counters are reset once per
// phase, while micro traversal statistics need one frame-wide accumulation.
struct alignas(16) SparseVoxelMicroCountersGPU
{
  uint32_t eligibleParentHits = 0u;
  uint32_t descriptorLoads = 0u;
  uint32_t acceptedMicroHits = 0u;
  uint32_t carvedParents = 0u;
  uint32_t parentContinuations = 0u;
  uint32_t finalDiscards = 0u;
  uint32_t heightfieldSteps = 0u;
  uint32_t volumeSteps = 0u;
  // The legacy carved/continuation counters combine exact traversal misses
  // with stochastic LOD fallback rejection. These split the two populations
  // so an optimization can be attributed to the work it actually removes.
  uint32_t exactTraceInvocations = 0u;
  uint32_t fallbackCandidates = 0u;
  uint32_t fallbackRejected = 0u;
  // Descriptor-valid staircase candidates, classified before the exact/fallback
  // handoff. These are repeated fragment demand, not unique parent voxels.
  uint32_t staircaseInteriorCandidates = 0u;
  uint32_t staircaseFlatCandidates = 0u;
  uint32_t staircaseSlopeCandidates = 0u;
  uint32_t staircaseWallCandidates = 0u;
  uint32_t staircaseOtherCandidates = 0u;
  // heightfieldSteps above is the one-face step total. Keep its established
  // name while recording invocation and outcome counts alongside it.
  uint32_t oneFaceInvocations = 0u;
  uint32_t oneFaceHits = 0u;
  uint32_t oneFaceMisses = 0u;
  uint32_t volumeInvocations = 0u;
  uint32_t volumeHits = 0u;
  uint32_t volumeMisses = 0u;
  // Exact traces routed through the solid Interior Staircase shortcut. Wall
  // candidates remain a separate sizing counter for a future one-face path.
  uint32_t solidInteriorShortcuts = 0u;
  uint32_t wallOneFaceEligible = 0u;
  // Brick traversal counters. A scale-4 group is the optional 4x4x4-voxel
  // binary skip above the register-held scale-2 grid. Fine probes are reads of
  // one exact parent-occupancy bit. firstHitExits identifies fragments whose
  // very first fine probe produced the accepted surface; traversalMisses are
  // proxy fragments that found no parent hit.
  uint32_t scale4GroupsTested = 0u;
  uint32_t scale4GroupsSkipped = 0u;
  uint32_t scale2CellsVisited = 0u;
  uint32_t fineOccupancyProbes = 0u;
  uint32_t firstHitExits = 0u;
  uint32_t traversalMisses = 0u;
  // Actual exact-trace invocations partitioned by normalized terrain context
  // and traversal shape. Non-staircase one-face work stays separate from the
  // general volume path; all six buckets sum to exactTraceInvocations.
  uint32_t traceContextInterior = 0u;
  uint32_t traceContextFlat = 0u;
  uint32_t traceContextSlope = 0u;
  uint32_t traceContextWall = 0u;
  uint32_t traceContextGeneralVolume = 0u;
  uint32_t traceContextOtherHeightfield = 0u;
};

struct alignas(16) SparseVoxelBrickDrawIndirectGPU
{
  uint32_t vertexCount = 0u;
  uint32_t instanceCount = 0u;
  uint32_t firstVertex = 0u;
  uint32_t firstInstance = 0u;
};

static_assert(sizeof(SparseVoxelBrickOccupancyGPU) == 128u);
static_assert(sizeof(SparseVoxelBrickNodeGPU) == 96u);
static_assert(sizeof(SparseVoxelBrickMaterialGPU) == 8u);
static_assert(sizeof(SparseVoxelMicroCountersGPU) == 144u);
static_assert(sizeof(SparseVoxelBrickDrawIndirectGPU) == 16u, "must match VkDrawIndirectCommand");

// Pack an octree material record into the eight-byte brick form. Emissive
// collapses to a single scalar because voxel emitters here are tinted by their
// own albedo; `kSparseVoxelBrickEmissiveScale` is the luminance that a full
// 255 encodes, and the material G-buffer applies the same constant on decode.
inline constexpr float kSparseVoxelBrickEmissiveScale = 16.0f;

SparseVoxelBrickMaterialGPU packSparseVoxelBrickMaterial(const SparseVoxelMaterialGPU &material);

// CPU-side build result. Node 0 .. rootCount-1 are the roots of the cut.
struct SparseVoxelBrickHierarchy
{
  std::vector<SparseVoxelBrickNodeGPU> nodes;
  std::vector<SparseVoxelBrickOccupancyGPU> occupancy;
  // One *palette index* per occupied voxel across all bricks, in brick order
  // then occupancy bit order, addressed by the popcount of the preceding bits.
  //
  // The record itself carries only six bytes of information - R, G, B,
  // roughness, emissive, metallic - and a single instance draws from a small
  // set of them, so storing the byte and looking the record up is an 8x
  // saving on much the largest array the hierarchy owns. Sponza at voxel 5:
  // 6.6M voxels, 53 MB of records against 6.6 MB of indices plus a palette
  // measured in kilobytes.
  std::vector<uint8_t> materialIndices;
  // One normalized micro-context word per occupied voxel, parallel to
  // materialIndices. Its upper byte is SparseVoxelMicroContext; its lower 56
  // bits are four fourteen-bit retained-height corners forming a bilinear
  // coverage patch. Keeping this out of the palette lets neighbouring voxels
  // share one stable material while requiring different terrain geometry.
  std::vector<uint64_t> microSurfaceData;
  // `paletteStride` entries per instance, indexed by
  // SparseVoxelBrickNodeGPU::instanceIndex. Sized per instance rather than per
  // brick because one instance is one authored material, so its voxels already
  // share a narrow colour range; a whole-scene palette would have to span every
  // material at once and quantise visibly.
  std::vector<SparseVoxelBrickMaterialGPU> palette;
  // Parallel to palette. Zero is the Disabled descriptor. The draw shader
  // checks bit 31 of palette.packed[1] before loading this ID, so legacy
  // materials retain one branch and no extra storage read.
  std::vector<uint16_t> paletteDescriptorIds;
  uint32_t paletteStride = 0u;
  // Palette fit, for judging whether the quantisation is visible at all.
  // `exactInstances` reproduced every one of their records; the errors are
  // measured in 0-255 units over the weighted six-byte record, counting only
  // voxels that had to be approximated.
  uint32_t paletteInstanceCount = 0u;
  uint32_t paletteExactInstances = 0u;
  double paletteMeanError = 0.0;
  double paletteMaximumError = 0.0;
  // Measured over every index actually emitted, at every level - which is the
  // number that decides whether coarse LOD bricks are degraded. The survey
  // above only covers level 0, and a coarse brick resolves through its majority
  // instance's palette, so a cell contributed by a different instance can miss.
  uint64_t paletteEmittedVoxels = 0u;
  uint64_t paletteEmittedApproximated = 0u;
  double paletteEmittedMeanError = 0.0;
  double paletteEmittedMaximumError = 0.0;
  // Bricks whose occupied cells come from more than one instance. These are the
  // only ones that can resolve a cell through a palette that was never built to
  // hold it, so they own the whole error tail in a split scene.
  uint32_t paletteStraddlingBricks = 0u;
  uint64_t paletteStraddlingVoxels = 0u;
  // Child node indices, referenced by SparseVoxelBrickNodeGPU::firstChild.
  std::vector<uint32_t> childIndices;
  // Virtual-geometry-style groups over brick nodes. These are sidecar metadata
  // for cut selection and future direct hierarchy tracing; they deliberately
  // do not alter the brick node ABI or the page residency contract.
  std::vector<SparseVoxelBrickLODGroupGPU> lodGroups;
  std::vector<uint32_t> lodGroupNodeIndices;
  std::vector<uint32_t> nodeLODGroupIndices;
  std::vector<uint32_t> representativeLODGroupIndices;
  uint32_t lodGroupSideBricks = kSparseVoxelBrickLODGroupDefaultSideBricks;
  // Node indices of the coarsest level, which the culling pass seeds from.
  std::vector<uint32_t> rootIndices;
  uint32_t rootCount = 0u;
  uint32_t levelCount = 0u;
  float finestVoxelSize = 0.0f;

  bool empty() const { return nodes.empty(); }
};

struct SparseVoxelMaterialSidecarMemory
{
  uint64_t microSurfaceLogicalBytes = 0u;
  uint64_t microSurfaceResidentBytes = 0u;
  uint64_t paletteDescriptorLogicalBytes = 0u;
  uint64_t paletteDescriptorResidentBytes = 0u;

  uint64_t logicalBytes() const
  {
    return microSurfaceLogicalBytes + paletteDescriptorLogicalBytes;
  }

  uint64_t residentBytes() const
  {
    return microSurfaceResidentBytes + paletteDescriptorResidentBytes;
  }
};

// Phase-zero accounting for the two material/context arrays carried by both a
// resident hierarchy and its uncompressed transient-page payload.
inline SparseVoxelMaterialSidecarMemory sparseVoxelMaterialSidecarMemory(
    const SparseVoxelBrickHierarchy &hierarchy)
{
  return {
    hierarchy.microSurfaceData.size() * sizeof(uint64_t),
    hierarchy.microSurfaceData.capacity() * sizeof(uint64_t),
    hierarchy.paletteDescriptorIds.size() * sizeof(uint16_t),
    hierarchy.paletteDescriptorIds.capacity() * sizeof(uint16_t),
  };
}

struct SparseVoxelBrickBuildSettings
{
  // Produce the per-voxel micro-surface words. They feed the brick draw,
  // the brick G-buffer and the Tree64 material extraction; a route that draws
  // through virtual geometry never reads them, and they are the largest part
  // of a chunk payload (one uint64 per material entry, ~16 MB per fine chunk).
  bool buildMicroSurface = true;
  // Bricks are built at this voxel size at level 0. Zero derives it from the
  // finest leaf in the source hierarchy.
  float voxelSize = 0.0f;
  // Guard against a pathological source producing an unbounded brick set.
  uint32_t maximumBricks = 1u << 20u;
  // Palette entries per instance. One byte per voxel caps this at 256; smaller
  // values trade colour error for palette memory, which is already negligible.
  uint32_t paletteSize = 256u;
  // Refinement passes when an instance has more distinct records than the
  // palette holds. Zero keeps the seeded centroids. Exact instances - the
  // common case, since most materials voxelise to well under 256 records -
  // skip clustering entirely and cost nothing here.
  uint32_t paletteRefinementIterations = 12u;
  // Optional source-material extension table keyed by
  // SparseVoxelMaterialGPU::materialId. Null preserves the legacy palette
  // byte-for-byte and emits descriptor zero for every entry.
  const SparseVoxelMicroMaterialLibrary *microMaterials = nullptr;
  // Stable material slots supersede microMaterials when supplied. Keeping the
  // legacy pointer during migration preserves existing scene and test callers.
  const voxel::material::VoxelMaterialManager *voxelMaterials = nullptr;
  // Optional high-precision terrain patches parallel to source materials.
  // Generated terrain uses this because SparseVoxelMaterialGPU::flags cannot
  // carry four UNorm14 corners alongside its context byte.
  const std::vector<uint64_t> *terrainSurfacePatches = nullptr;
  // Side length of a virtual-geometry-style LOD group, measured in brick nodes
  // at the node's own level. Two maps directly to one octree parent and its
  // 2x2x2 children; terrain heightfield pages usually occupy the 2x2 XZ subset
  // of that group. Zero disables group generation.
  uint32_t lodGroupSideBricks = kSparseVoxelBrickLODGroupDefaultSideBricks;
};

bool buildSparseVoxelBrickLODGroups(
    SparseVoxelBrickHierarchy &hierarchy,
    uint32_t groupSideBricks = kSparseVoxelBrickLODGroupDefaultSideBricks,
    std::string *error = nullptr);

// Build the brick octree from resident octree leaves, in world space.
//
// `centersLengths` and `materials` are parallel arrays indexed by voxel index;
// `leafVoxelIndices` selects the leaves to voxelize. Instance transforms are
// applied by the caller, so this works purely in world space and produces one
// hierarchy for the whole scene.
bool buildSparseVoxelBrickHierarchy(
    const std::vector<SparseVoxelCenterLengthGPU> &centersLengths,
    const std::vector<SparseVoxelMaterialGPU> &materials,
    const std::vector<uint32_t> &leafVoxelIndices,
    const SparseVoxelBrickBuildSettings &settings,
    SparseVoxelBrickHierarchy &hierarchy,
    std::string *error = nullptr,
    // Parallel to `centersLengths`: the instance each leaf belongs to. Empty
    // leaves every node's instanceIndex invalid, which is what a single-model
    // build wants.
    const std::vector<uint32_t> &leafInstances = {});

} // namespace voxel::svraster
