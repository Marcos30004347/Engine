#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace voxel::svraster
{

// Canonical descriptor-face order. This is intentionally different from the
// brick surface tag (axis in bits 0..1, sign in bit 2); descriptor masks need
// six contiguous bits for -X, +X, -Y, +Y, -Z, +Z.
enum class SparseVoxelMicroFace : uint32_t
{
  NegativeX = 0u,
  PositiveX = 1u,
  NegativeY = 2u,
  PositiveY = 3u,
  NegativeZ = 4u,
  PositiveZ = 5u,
};

// Staircase geometry is an opinionated terrain representation. Subdivision is
// structural and the terrain coverage patch supplies its large-scale slope,
// but procedural material arguments do not alter that shape. The transition
// uses the top and four lateral faces and ignores the random seed.
inline constexpr uint32_t kSparseVoxelStaircaseDepthPerMicrocellFixed = 16u;
inline constexpr uint32_t kSparseVoxelMicroMaximumSubdivision = 8u;
inline constexpr uint32_t sparseVoxelStaircaseCarveDepthFixed(
    const uint32_t subdivision)
{
  return subdivision * kSparseVoxelStaircaseDepthPerMicrocellFixed;
}
inline constexpr uint32_t kSparseVoxelStaircaseFaceMask =
    (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX)) |
    (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveX)) |
    (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY)) |
    (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeZ)) |
    (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveZ));

enum class SparseVoxelMicroGeometryProfile : uint32_t
{
  Disabled = 0u,
  Constant = 1u,
  HashColumns = 2u,
  VolumeDensity = 3u,
  ClumpedColumns = 4u,
  Staircase = 5u,
};

// Eight-bit terrain context. Runtime bricks store it in the upper byte of the
// per-voxel micro-context word; the lower 24 bits carry the four-corner surface
// patch. Runtime surface records set bit seven and store quantitative terrain
// slope in the remaining bits. Context-only legacy directional fixtures and
// the four explicit wall classes below retain their original encoding.
enum class SparseVoxelMicroContext : uint32_t
{
  Interior = 0u,
  FlatSurface = 1u,
  WallNegativeX = 10u,
  WallPositiveX = 11u,
  WallNegativeZ = 12u,
  WallPositiveZ = 13u,
};

inline constexpr uint32_t kSparseVoxelMicroContextSurfaceFlag = 0x80u;
inline constexpr uint32_t kSparseVoxelMicroContextPhaseMask = 0x07u;
inline constexpr uint32_t kSparseVoxelMicroContextAngleShift = 3u;
inline constexpr uint32_t kSparseVoxelMicroContextAngleMask = 0x0fu;
inline constexpr uint32_t kSparseVoxelMicroContextMask = 0xffu;
inline constexpr uint32_t kSparseVoxelMicroTerrainSlopeMask = 0x7fu;
inline constexpr uint32_t kSparseVoxelMicroTerrainSlopeQuantization = 126u;

inline constexpr uint32_t sparseVoxelMicroSurfaceContext(
    const uint32_t angleBin, const uint32_t phase = 4u)
{
  return kSparseVoxelMicroContextSurfaceFlag |
      ((angleBin & kSparseVoxelMicroContextAngleMask) <<
       kSparseVoxelMicroContextAngleShift) |
      (phase & kSparseVoxelMicroContextPhaseMask);
}

inline constexpr bool sparseVoxelMicroContextIsSurface(
    const uint32_t context)
{
  return (context & kSparseVoxelMicroContextSurfaceFlag) != 0u;
}

// Flat and quantitatively sloped surface contexts use the same +Y terrain
// heightfield frame. FlatSurface predates the encoded surface flag, so testing
// only that flag misclassifies flat terrain as generic multi-face geometry.
inline constexpr bool sparseVoxelMicroContextIsTerrainSurface(
    const uint32_t context)
{
  return context ==
          static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface) ||
      sparseVoxelMicroContextIsSurface(context);
}

// Runtime terrain surface records use the seven low context bits for a
// normalized slope angle. Code zero remains reserved for context-only legacy
// slopes, while codes 1..127 encode UNorm126. This lets old cached/runtime
// payloads retain their former binary meaning without adding another resolved
// attachment.
uint32_t sparseVoxelMicroTerrainSlopeContext(double normalizedSlope);
inline constexpr double sparseVoxelMicroContextTerrainSlope(
    const uint32_t context)
{
  if (!sparseVoxelMicroContextIsSurface(context))
    return 0.0;
  const uint32_t encoded = context & kSparseVoxelMicroTerrainSlopeMask;
  return encoded == 0u
      ? 1.0
      : static_cast<double>(encoded - 1u) /
          static_cast<double>(kSparseVoxelMicroTerrainSlopeQuantization);
}

// A terrain transition stores retained height at the four XZ corners as
// UNorm14 values in x-major order: -X-Z, +X-Z, -X+Z, +X+Z. Six bits made a
// 10 cm cell advance in 1.59 mm terraces, far larger than adjacent samples of
// kilometre-scale height maps. Fourteen bits reduce that step to 0.006 mm and
// still leave one byte for the terrain context in a 64-bit sidecar.
inline constexpr uint32_t kSparseVoxelMicroSurfaceCornerBits = 14u;
inline constexpr uint32_t kSparseVoxelMicroSurfaceCornerMask =
    (1u << kSparseVoxelMicroSurfaceCornerBits) - 1u;
inline constexpr uint64_t kSparseVoxelMicroSurfacePatchMask =
    0x00ffffffffffffffull;
inline constexpr uint32_t kSparseVoxelMicroContextDataShift = 56u;

// Normalized per-parent runtime context. This keeps material identity stable:
// terrain classification varies per voxel and therefore must not be encoded in
// either a palette entry or VoxelMaterialHandle.
inline constexpr uint64_t packSparseVoxelMicroContextData(
    const uint32_t context, const uint64_t surfacePatch)
{
  return (surfacePatch & kSparseVoxelMicroSurfacePatchMask) |
      (static_cast<uint64_t>(context & kSparseVoxelMicroContextMask) <<
       kSparseVoxelMicroContextDataShift);
}

inline constexpr uint32_t unpackSparseVoxelMicroContextValue(
    const uint64_t packedContext)
{
  return static_cast<uint32_t>(
      (packedContext >> kSparseVoxelMicroContextDataShift) &
      kSparseVoxelMicroContextMask);
}

inline constexpr uint64_t unpackSparseVoxelMicroContextSurfacePatch(
    const uint64_t packedContext)
{
  return packedContext & kSparseVoxelMicroSurfacePatchMask;
}

uint64_t packSparseVoxelMicroSurfacePatch(
    const std::array<double, 4> &cornerHeights);
std::array<double, 4> unpackSparseVoxelMicroSurfacePatch(uint64_t packedPatch);
uint32_t sparseVoxelMicroSurfaceColumnDepth(
    uint64_t packedPatch, uint32_t subdivision, uint32_t x, uint32_t z);
double sparseVoxelMicroSurfaceExpectedCoverage(uint64_t packedPatch);
uint32_t packSparseVoxelMicroContextDataLegacy(uint64_t packedContext);

struct SparseVoxelMicroDescriptor
{
  uint32_t subdivision = 0u;
  uint32_t maximumCarveDepth = 0u;
  uint32_t faceMask = 0u;
  SparseVoxelMicroGeometryProfile geometryProfile = SparseVoxelMicroGeometryProfile::Disabled;
  uint32_t materialSeed = 0u;
  // Sixteenths of one microcell added to maximumCarveDepth. Keeping the
  // subdivision integer preserves the canonical grid while allowing a cut to
  // stop inside a microcell instead of snapping to its boundary.
  uint32_t carveDepthFraction = 0u;
};

struct SparseVoxelMicroRay
{
  std::array<double, 3> origin{};
  std::array<double, 3> direction{};
  double minimumDistance = 0.0;
  double maximumDistance = std::numeric_limits<double>::infinity();
};

enum class SparseVoxelMicroTraceStatus : uint32_t
{
  Invalid = 0u,
  Miss = 1u,
  Hit = 2u,
};

struct SparseVoxelMicroTraceResult
{
  SparseVoxelMicroTraceStatus status = SparseVoxelMicroTraceStatus::Invalid;
  double distance = 0.0;
  std::array<uint32_t, 3> cell{};
  SparseVoxelMicroFace face = SparseVoxelMicroFace::NegativeX;
  uint32_t steps = 0u;

  bool hit() const
  {
    return status == SparseVoxelMicroTraceStatus::Hit;
  }
};

bool sparseVoxelMicroDescriptorIsValid(const SparseVoxelMicroDescriptor &descriptor);
bool sparseVoxelMicroDescriptorIsEnabled(const SparseVoxelMicroDescriptor &descriptor);

// Direct integer address for a parent inside a brick. `originScale` is copied
// from SparseVoxelBrickNodeGPU: xyz is the brick origin in canonical finest
// parents and w is this node's parent scale. No world float participates.
std::array<int32_t, 3> sparseVoxelMicroCanonicalParentKey(const std::array<int32_t, 4> &originScale, const std::array<uint32_t, 3> &parentCell);

// Canonical signed microcell address used by shared 3D profiles. Adjacent
// parent boundary layers differ by exactly one on the crossed axis.
std::array<int64_t, 3> sparseVoxelMicroCanonicalCell(const std::array<int32_t, 3> &parentKey, uint32_t subdivision, const std::array<uint32_t, 3> &cell);

// Integer coordinate of a face-relative column in the shared micro lattice.
// Opposing faces at one parent boundary resolve to the same coordinate after
// their canonical U/V inversions are undone.
std::array<int64_t, 3> sparseVoxelMicroCanonicalFaceColumn(const std::array<int32_t, 3> &parentKey, uint32_t subdivision, SparseVoxelMicroFace face, const std::array<uint32_t, 2> &faceCell);

// Face-relative orientation is right-handed (U x V = outward normal):
// -X: +Z,+Y; +X: -Z,+Y; -Y: +X,+Z; +Y: +X,-Z;
// -Z: -X,+Y; +Z: +X,+Y.
std::array<uint32_t, 2> sparseVoxelMicroFaceCell(const std::array<uint32_t, 3> &cell, uint32_t subdivision, SparseVoxelMicroFace face);

inline constexpr uint32_t kSparseVoxelMicroPayloadDescriptorMask = 0xffffu;
inline constexpr uint32_t kSparseVoxelMicroPayloadUShift = 16u;
inline constexpr uint32_t kSparseVoxelMicroPayloadVShift = 20u;
inline constexpr uint32_t kSparseVoxelMicroPayloadDepthShift = 24u;
inline constexpr uint32_t kSparseVoxelMicroPayloadCoordinateMask = 0x0fu;
inline constexpr uint32_t kSparseVoxelMicroPayloadValid = 1u << 28u;
inline constexpr uint32_t kSparseVoxelMicroPayloadHeightfield = 1u << 29u;
inline constexpr uint32_t kSparseVoxelMicroPayloadFallback = 1u << 30u;
inline constexpr uint32_t kSparseVoxelMicroPayloadTerrainSlope = 1u << 31u;

// Packs the visibility attachment shared with the WGSL brick traversal. Zero
// is reserved for legacy/parent hits. Heightfields store face-relative U, V,
// and inward depth; general volumes store parent-local X, Y, and Z. The exact
// visible face remains in the surface record in both cases.
uint32_t packSparseVoxelMicroPayload(uint16_t descriptorId, const std::array<uint32_t, 3> &cell, uint32_t subdivision, SparseVoxelMicroFace carveFace, bool heightfield = true, bool terrainSlope = false);

// Coarse brick/clipmap levels retain conservative parent visibility and carry
// only the descriptor into the material pass. This stable representation has
// no literal microcell coordinate and never claims exact geometry.
uint32_t packSparseVoxelMicroFallbackPayload(
    uint16_t descriptorId, bool terrainSlope = false);

double sparseVoxelMicroMaximumCarveDepth(
    const SparseVoxelMicroDescriptor &descriptor);
double sparseVoxelMicroCarveDepth(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, SparseVoxelMicroFace face, const std::array<uint32_t, 2> &faceCell, uint32_t context = 0u);

bool sparseVoxelMicroCellOccupied(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const std::array<uint32_t, 3> &cell, uint32_t context = 0u);
bool sparseVoxelMicroCellBounds(
    const SparseVoxelMicroDescriptor &descriptor,
    const std::array<int32_t, 3> &parentKey,
    const std::array<uint32_t, 3> &cell,
    std::array<double, 3> &minimum,
    std::array<double, 3> &maximum,
    uint32_t context = 0u);

// CPU mirrors of the shader's exact-to-fallback transition. A zero encoded
// scale selects the default; 128 encodes 1x explicitly.
double sparseVoxelMicroHandoffWeight(double projectedMicroPixels, uint32_t encodedScale = 0u);
double sparseVoxelMicroExpectedCoverage(const SparseVoxelMicroDescriptor &descriptor, uint32_t context = 0u);

// Rays and distances are in parent-local coordinates, where the parent AABB is
// [0,1]^3. The one-face path walks at most 2*N+2 face columns; the volume path
// walks at most 3*N+3 microcells.
//
// Built-in Staircase Interior contains no carved cells. This reference mirrors
// the shader's constant-time first-volume-cell shortcut.
SparseVoxelMicroTraceResult traceSparseVoxelMicroSolidStaircaseInterior(const SparseVoxelMicroDescriptor &descriptor, const SparseVoxelMicroRay &ray, uint32_t context);

SparseVoxelMicroTraceResult traceSparseVoxelMicroOneFace(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroRay &ray, uint32_t context = 0u);

SparseVoxelMicroTraceResult traceSparseVoxelMicroVolume(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroRay &ray, uint32_t context = 0u);

struct SparseVoxelMicroParentGrid
{
  std::array<uint32_t, 3> dimensions{};
  std::vector<uint8_t> occupied;
  // Empty means every occupied parent is legacy solid. Otherwise this is
  // parallel to occupied; disabled descriptors retain the legacy behavior.
  std::vector<SparseVoxelMicroDescriptor> descriptors;
  // Optional eight-bit context parallel to occupied. Empty selects Interior.
  std::vector<uint8_t> contexts;
};

struct SparseVoxelMicroParentTraceResult
{
  SparseVoxelMicroTraceStatus status = SparseVoxelMicroTraceStatus::Invalid;
  double distance = 0.0;
  std::array<uint32_t, 3> parentCell{};
  std::array<uint32_t, 3> microCell{};
  SparseVoxelMicroFace face = SparseVoxelMicroFace::NegativeX;
  uint32_t parentSteps = 0u;
  uint32_t parentContinuations = 0u;
  bool microHit = false;

  bool hit() const
  {
    return status == SparseVoxelMicroTraceStatus::Hit;
  }
};

// Reference for the brick traversal's three candidate outcomes: a disabled
// occupied parent accepts immediately, a surviving microcell accepts exactly,
// and a completely carved parent advances to the next parent cell.
SparseVoxelMicroParentTraceResult traceSparseVoxelMicroParents(const SparseVoxelMicroParentGrid &grid, const SparseVoxelMicroRay &ray);

} // namespace voxel::svraster
