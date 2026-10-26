#include "voxel/svraster/SparseVoxelMicroGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace voxel::svraster
{
namespace
{

constexpr double kTraceEpsilon = 1.0e-12;
constexpr uint32_t kCarveDepthFractionBits = 4u;
constexpr uint32_t kCarveDepthFractionScale =
    1u << kCarveDepthFractionBits;

uint32_t maximumCarveDepthFixed(
    const SparseVoxelMicroDescriptor &descriptor)
{
  if (descriptor.geometryProfile ==
      SparseVoxelMicroGeometryProfile::Staircase)
    return sparseVoxelStaircaseCarveDepthFixed(descriptor.subdivision);
  return descriptor.maximumCarveDepth * kCarveDepthFractionScale +
      descriptor.carveDepthFraction;
}

uint32_t effectiveFaceMask(const SparseVoxelMicroDescriptor &descriptor)
{
  return descriptor.geometryProfile ==
          SparseVoxelMicroGeometryProfile::Staircase
      ? kSparseVoxelStaircaseFaceMask
      : descriptor.faceMask;
}

struct Interval
{
  double entry = 0.0;
  double exit = 0.0;
  SparseVoxelMicroFace entryFace = SparseVoxelMicroFace::NegativeX;
};

uint32_t faceIndex(const SparseVoxelMicroFace face)
{
  return static_cast<uint32_t>(face);
}

uint32_t faceAxis(const SparseVoxelMicroFace face)
{
  return faceIndex(face) >> 1u;
}

bool faceIsPositive(const SparseVoxelMicroFace face)
{
  return (faceIndex(face) & 1u) != 0u;
}

SparseVoxelMicroFace enteringFace(const uint32_t axis, const double direction)
{
  return static_cast<SparseVoxelMicroFace>(axis * 2u + (direction < 0.0 ? 1u : 0u));
}

bool validRay(const SparseVoxelMicroRay &ray)
{
  bool nonzero = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis]))
      return false;
    nonzero |= ray.direction[axis] != 0.0;
  }
  return nonzero && std::isfinite(ray.minimumDistance) && !std::isnan(ray.maximumDistance) && ray.minimumDistance >= 0.0 && ray.maximumDistance >= ray.minimumDistance;
}

bool intersectBox(const SparseVoxelMicroRay &ray, const std::array<double, 3> &minimum, const std::array<double, 3> &maximum, Interval &interval)
{
  interval.entry = ray.minimumDistance;
  interval.exit = ray.maximumDistance;
  bool selectedFace = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double origin = ray.origin[axis];
    const double direction = ray.direction[axis];
    if (direction == 0.0)
    {
      if (origin < minimum[axis] || origin > maximum[axis])
        return false;
      continue;
    }
    double nearDistance = (minimum[axis] - origin) / direction;
    double farDistance = (maximum[axis] - origin) / direction;
    if (nearDistance > farDistance)
      std::swap(nearDistance, farDistance);
    if (nearDistance > interval.entry || (!selectedFace && nearDistance >= interval.entry))
    {
      interval.entry = nearDistance;
      interval.entryFace = enteringFace(axis, direction);
      selectedFace = true;
    }
    interval.exit = std::min(interval.exit, farDistance);
    if (interval.exit + kTraceEpsilon < interval.entry)
      return false;
  }
  return interval.exit >= ray.minimumDistance;
}

std::array<double, 3> pointAt(const SparseVoxelMicroRay &ray, const double distance)
{
  return {
    ray.origin[0] + ray.direction[0] * distance,
    ray.origin[1] + ray.direction[1] * distance,
    ray.origin[2] + ray.direction[2] * distance,
  };
}

uint32_t cellCoordinate(double coordinate, const double direction, const uint32_t subdivision)
{
  // A reconstructed boundary can be several ULPs to the wrong side after the
  // slab arithmetic (for example 2.0 - 1.4). Move a fixed reference-scale
  // epsilon into the cell the ray enters before flooring. WGSL uses the same
  // directional ownership rule when this reference is ported to the shader.
  if (direction > 0.0)
    coordinate += kTraceEpsilon;
  else if (direction < 0.0)
    coordinate -= kTraceEpsilon;
  coordinate = std::clamp(coordinate, 0.0, 1.0);
  return std::min(static_cast<uint32_t>(std::floor(coordinate * subdivision)), subdivision - 1u);
}

std::array<uint32_t, 3> cellAt(const SparseVoxelMicroRay &ray, const double distance, const uint32_t subdivision)
{
  const std::array<double, 3> point = pointAt(ray, distance);
  return {
    cellCoordinate(point[0], ray.direction[0], subdivision),
    cellCoordinate(point[1], ray.direction[1], subdivision),
    cellCoordinate(point[2], ray.direction[2], subdivision),
  };
}

uint32_t mixHash(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

uint32_t hashColumn(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroFace face, const std::array<uint32_t, 2> &faceCell)
{
  uint32_t value = (descriptor.materialSeed & 0xffu) ^ 0x9e3779b9u;
  value = mixHash(value ^ static_cast<uint32_t>(parentKey[0]));
  value = mixHash(value ^ static_cast<uint32_t>(parentKey[1]));
  value = mixHash(value ^ static_cast<uint32_t>(parentKey[2]));
  value = mixHash(value ^ (faceIndex(face) * 0x85ebca6bu));
  value = mixHash(value ^ (faceCell[0] * 0xc2b2ae35u));
  value = mixHash(value ^ (faceCell[1] * 0x27d4eb2fu));
  return mixHash(value ^ (static_cast<uint32_t>(descriptor.geometryProfile) * 0x165667b1u));
}

uint32_t hashVolumeCell(
    const SparseVoxelMicroDescriptor &descriptor,
    const std::array<int32_t, 3> &parentKey,
    const std::array<uint32_t, 3> &cell)
{
  const uint32_t subdivision = descriptor.subdivision;
  const std::array<int64_t, 3> signedCanonicalCell =
      sparseVoxelMicroCanonicalCell(parentKey, subdivision, cell);
  const std::array<uint32_t, 3> canonicalCell{
    static_cast<uint32_t>(signedCanonicalCell[0]),
    static_cast<uint32_t>(signedCanonicalCell[1]),
    static_cast<uint32_t>(signedCanonicalCell[2]),
  };
  uint32_t value = (descriptor.materialSeed & 0xffu) ^ 0x517cc1b7u;
  value = mixHash(value ^ canonicalCell[0] * 0x8da6b343u);
  value = mixHash(value ^ canonicalCell[1] * 0xd8163841u);
  value = mixHash(value ^ canonicalCell[2] * 0xcb1ab31fu);
  return mixHash(value ^
      static_cast<uint32_t>(descriptor.geometryProfile) * 0x165667b1u);
}

uint32_t enabledFaceCount(const uint32_t faceMask)
{
  uint32_t bits = faceMask & 0x3fu;
  uint32_t count = 0u;
  while (bits != 0u)
  {
    count += bits & 1u;
    bits >>= 1u;
  }
  return count;
}

SparseVoxelMicroFace onlyEnabledFace(const uint32_t faceMask)
{
  uint32_t face = 0u;
  uint32_t bits = faceMask & 0x3fu;
  while ((bits & 1u) == 0u)
  {
    ++face;
    bits >>= 1u;
  }
  return static_cast<SparseVoxelMicroFace>(face);
}

struct FaceFrame
{
  uint32_t axis = 0u;
  uint32_t uAxis = 0u;
  uint32_t vAxis = 0u;
  bool invertU = false;
  bool invertV = false;
};

FaceFrame faceFrame(const SparseVoxelMicroFace face)
{
  switch (face)
  {
  case SparseVoxelMicroFace::NegativeX:
    return {0u, 2u, 1u, false, false};
  case SparseVoxelMicroFace::PositiveX:
    return {0u, 2u, 1u, true, false};
  case SparseVoxelMicroFace::NegativeY:
    return {1u, 0u, 2u, false, false};
  case SparseVoxelMicroFace::PositiveY:
    return {1u, 0u, 2u, false, true};
  case SparseVoxelMicroFace::NegativeZ:
    return {2u, 0u, 1u, true, false};
  case SparseVoxelMicroFace::PositiveZ:
    return {2u, 0u, 1u, false, false};
  }
  return {};
}

int64_t floorDivide64(const int64_t value, const int64_t divisor)
{
  const int64_t quotient = value / divisor;
  return quotient -
      ((value % divisor != 0 && (value < 0) != (divisor < 0)) ? 1 : 0);
}

uint32_t hashCanonicalColumn(
    const SparseVoxelMicroDescriptor &descriptor,
    const SparseVoxelMicroFace face,
    const std::array<int64_t, 3> &coordinate,
    const uint32_t salt)
{
  uint32_t value = (descriptor.materialSeed & 0xffu) ^ salt;
  value = mixHash(value ^
      static_cast<uint32_t>(coordinate[0]) * 0x8da6b343u);
  value = mixHash(value ^
      static_cast<uint32_t>(coordinate[1]) * 0xd8163841u);
  value = mixHash(value ^
      static_cast<uint32_t>(coordinate[2]) * 0xcb1ab31fu);
  value = mixHash(value ^ faceIndex(face) * 0x85ebca6bu);
  return mixHash(value ^
      static_cast<uint32_t>(descriptor.geometryProfile) * 0x165667b1u);
}

uint32_t clumpedColumnDepth(
    const SparseVoxelMicroDescriptor &descriptor,
    const std::array<int32_t, 3> &parentKey,
    const SparseVoxelMicroFace face,
    const std::array<uint32_t, 2> &faceCell)
{
  const std::array<int64_t, 3> canonical =
      sparseVoxelMicroCanonicalFaceColumn(
          parentKey, descriptor.subdivision, face, faceCell);
  std::array<int64_t, 3> cluster{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    cluster[axis] = floorDivide64(canonical[axis], 4);
  const uint32_t maximumDepth = maximumCarveDepthFixed(descriptor);
  const uint32_t range = maximumDepth + 1u;
  const int32_t base = static_cast<int32_t>(hashCanonicalColumn(
      descriptor, face, cluster, 0x68bc21ebu) % range);
  const int32_t jitter =
      (static_cast<int32_t>(hashCanonicalColumn(
           descriptor, face, canonical, 0x02e5be93u) % 3u) -
       1) *
      static_cast<int32_t>(kCarveDepthFractionScale);
  return static_cast<uint32_t>(std::clamp(
      base + jitter, 0,
      static_cast<int32_t>(maximumDepth)));
}

std::array<int32_t, 2> staircaseSurfaceDirection(
    const uint32_t context)
{
  static constexpr std::array<std::array<int32_t, 2>, 16u> directions{{
    {{8, 0}}, {{7, 3}}, {{6, 6}}, {{3, 7}},
    {{0, 8}}, {{-3, 7}}, {{-6, 6}}, {{-7, 3}},
    {{-8, 0}}, {{-7, -3}}, {{-6, -6}}, {{-3, -7}},
    {{0, -8}}, {{3, -7}}, {{6, -6}}, {{7, -3}},
  }};
  if (!sparseVoxelMicroContextIsSurface(context))
    return {0, 0};
  return directions[
      (context >> kSparseVoxelMicroContextAngleShift) &
      kSparseVoxelMicroContextAngleMask];
}

uint32_t staircaseColumnDepth(
    const SparseVoxelMicroDescriptor &descriptor,
    const std::array<int32_t, 3> &parentKey,
    const SparseVoxelMicroFace face,
    const std::array<uint32_t, 2> &faceCell,
    const uint32_t contextValue)
{
  const uint32_t context = contextValue & kSparseVoxelMicroContextMask;
  if (context == static_cast<uint32_t>(SparseVoxelMicroContext::Interior) ||
      context == static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface))
    return 0u;

  SparseVoxelMicroFace wallFace = SparseVoxelMicroFace::NegativeX;
  bool wall = true;
  switch (static_cast<SparseVoxelMicroContext>(context))
  {
  case SparseVoxelMicroContext::WallNegativeX:
    wallFace = SparseVoxelMicroFace::NegativeX;
    break;
  case SparseVoxelMicroContext::WallPositiveX:
    wallFace = SparseVoxelMicroFace::PositiveX;
    break;
  case SparseVoxelMicroContext::WallNegativeZ:
    wallFace = SparseVoxelMicroFace::NegativeZ;
    break;
  case SparseVoxelMicroContext::WallPositiveZ:
    wallFace = SparseVoxelMicroFace::PositiveZ;
    break;
  default:
    wall = false;
    break;
  }
  if (wall)
  {
    if (face != wallFace)
      return 0u;
    // Every lateral face frame uses +Y for V. Increasing the inset toward
    // the top breaks an otherwise parent-sized exposed face into horizontal
    // microvoxel ledges. Use an integer microcell depth: normalizing N depth
    // cells over N-1 intervals made every inset N/(N-1) cells wide.
    return faceCell[1] * kSparseVoxelStaircaseDepthPerMicrocellFixed;
  }

  // This is the legacy context-only staircase used by CPU reference fixtures.
  // Runtime terrain carries its context and continuous four-corner coverage
  // patch in the parallel normalized sidecar consumed by the shader.
  const std::array<int32_t, 2> direction =
      staircaseSurfaceDirection(context);
  if (face != SparseVoxelMicroFace::PositiveY)
  {
    // The top height plane already emits its vertical microcell risers. A
    // second face-relative cut would intersect that plane into an artificial
    // pyramid and expose the parent boundary again.
    return 0u;
  }

  if (direction[0] == 0 && direction[1] == 0)
    return 0u;
  const int32_t localX = static_cast<int32_t>(faceCell[0]);
  const int32_t localZ = static_cast<int32_t>(
      descriptor.subdivision - 1u - faceCell[1]);
  const int32_t subdivision = static_cast<int32_t>(descriptor.subdivision);
  const int32_t cellSpan = subdivision - 1;
  const int64_t projection =
      static_cast<int64_t>(direction[0]) * (2 * localX - cellSpan) +
      static_cast<int64_t>(direction[1]) * (2 * localZ - cellSpan);
  const int64_t extent =
      static_cast<int64_t>(
          std::abs(direction[0]) + std::abs(direction[1])) * cellSpan;
  const int32_t phase = static_cast<int32_t>(
      context & kSparseVoxelMicroContextPhaseMask);
  const int32_t centerDepth =
      ((static_cast<int32_t>(kSparseVoxelMicroContextPhaseMask) - phase) *
           subdivision +
       static_cast<int32_t>(kSparseVoxelMicroContextPhaseMask / 2u)) /
      static_cast<int32_t>(kSparseVoxelMicroContextPhaseMask);
  const int64_t denominator = 2 * extent;
  const int64_t numerator =
      static_cast<int64_t>(centerDepth) * denominator +
      projection * subdivision;
  const int64_t rounded = numerator >= 0
      ? (numerator + extent) / denominator
      : -((-numerator + extent) / denominator);
  const int32_t planeCoordinate = std::clamp(
      static_cast<int32_t>(rounded), 0, subdivision);
  return static_cast<uint32_t>(planeCoordinate) *
      kSparseVoxelStaircaseDepthPerMicrocellFixed;
}

double orientedCoordinate(const std::array<double, 3> &point, const uint32_t axis, const bool inverted)
{
  return inverted ? 1.0 - point[axis] : point[axis];
}

double orientedDirection(const SparseVoxelMicroRay &ray, const uint32_t axis, const bool inverted)
{
  return inverted ? -ray.direction[axis] : ray.direction[axis];
}

void setOrientedBounds(std::array<double, 3> &minimum, std::array<double, 3> &maximum, const uint32_t axis, const bool inverted, const double orientedMinimum, const double orientedMaximum)
{
  if (inverted)
  {
    minimum[axis] = 1.0 - orientedMaximum;
    maximum[axis] = 1.0 - orientedMinimum;
  }
  else
  {
    minimum[axis] = orientedMinimum;
    maximum[axis] = orientedMaximum;
  }
}

uint32_t parentIndex(const SparseVoxelMicroParentGrid &grid, const std::array<uint32_t, 3> &cell)
{
  return cell[0] + grid.dimensions[0] * (cell[1] + grid.dimensions[1] * cell[2]);
}

bool gridIsValid(const SparseVoxelMicroParentGrid &grid)
{
  const uint64_t count = static_cast<uint64_t>(grid.dimensions[0]) * grid.dimensions[1] * grid.dimensions[2];
  if (count == 0u || count != grid.occupied.size())
    return false;
  if (!grid.descriptors.empty() && grid.descriptors.size() != count)
    return false;
  if (!grid.contexts.empty() && grid.contexts.size() != count)
    return false;
  for (const uint8_t context : grid.contexts)
  {
    if ((context & ~kSparseVoxelMicroContextMask) != 0u)
      return false;
  }
  for (const SparseVoxelMicroDescriptor &descriptor : grid.descriptors)
  {
    if (!sparseVoxelMicroDescriptorIsValid(descriptor))
      return false;
  }
  return true;
}

} // namespace

uint64_t packSparseVoxelMicroSurfacePatch(
    const std::array<double, 4> &cornerHeights)
{
  uint64_t packed = 0u;
  for (uint32_t corner = 0u; corner < cornerHeights.size(); ++corner)
  {
    const double height = std::isfinite(cornerHeights[corner])
        ? std::clamp(cornerHeights[corner], 0.0, 1.0)
        : 0.0;
    const uint32_t quantized = static_cast<uint32_t>(std::lround(
        height * static_cast<double>(kSparseVoxelMicroSurfaceCornerMask)));
    packed |= static_cast<uint64_t>(
        quantized & kSparseVoxelMicroSurfaceCornerMask) <<
        (corner * kSparseVoxelMicroSurfaceCornerBits);
  }
  return packed;
}

uint32_t sparseVoxelMicroTerrainSlopeContext(const double normalizedSlope)
{
  const double slope = std::isfinite(normalizedSlope)
      ? std::clamp(normalizedSlope, 0.0, 1.0)
      : 0.0;
  const uint32_t quantized = static_cast<uint32_t>(std::lround(
      slope * static_cast<double>(
          kSparseVoxelMicroTerrainSlopeQuantization)));
  return kSparseVoxelMicroContextSurfaceFlag | (quantized + 1u);
}

std::array<double, 4> unpackSparseVoxelMicroSurfacePatch(
    const uint64_t packedPatch)
{
  std::array<double, 4> corners{};
  for (uint32_t corner = 0u; corner < corners.size(); ++corner)
  {
    const uint32_t quantized = static_cast<uint32_t>(
        (packedPatch >> (corner * kSparseVoxelMicroSurfaceCornerBits)) &
        kSparseVoxelMicroSurfaceCornerMask);
    corners[corner] = static_cast<double>(quantized) /
        static_cast<double>(kSparseVoxelMicroSurfaceCornerMask);
  }
  return corners;
}

uint32_t sparseVoxelMicroSurfaceColumnDepth(
    const uint64_t packedPatch, const uint32_t subdivision,
    const uint32_t x, const uint32_t z)
{
  if (subdivision == 0u || x >= subdivision || z >= subdivision)
    return 0u;
  const std::array<double, 4> corners =
      unpackSparseVoxelMicroSurfacePatch(packedPatch);
  const double u = (static_cast<double>(x) + 0.5) /
      static_cast<double>(subdivision);
  const double v = (static_cast<double>(z) + 0.5) /
      static_cast<double>(subdivision);
  const double negativeZ = corners[0] + (corners[1] - corners[0]) * u;
  const double positiveZ = corners[2] + (corners[3] - corners[2]) * u;
  const double retainedHeight = std::clamp(
      negativeZ + (positiveZ - negativeZ) * v, 0.0, 1.0);
  const double maximumDepth = static_cast<double>(subdivision);
  const double continuousDepth =
      (1.0 - retainedHeight) * maximumDepth;
  // The upper interval naturally floors to zero. Snap the symmetric final
  // half-cell to full carve so the lowest stair meets the lower parent plane
  // instead of leaving a one-microvoxel lip at that boundary.
  if (continuousDepth >= maximumDepth - 0.5)
    return subdivision;
  return static_cast<uint32_t>(std::clamp(
      // Keep the tolerance byte-for-byte equivalent to the WGSL evaluator.
      // Values immediately below an integer boundary otherwise disagreed at
      // some subdivisions after f32 interpolation on the GPU.
      std::floor(continuousDepth + 1.0e-5),
      0.0, maximumDepth));
}

double sparseVoxelMicroSurfaceExpectedCoverage(const uint64_t packedPatch)
{
  const std::array<double, 4> corners =
      unpackSparseVoxelMicroSurfacePatch(packedPatch);
  return (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25;
}

uint32_t packSparseVoxelMicroContextDataLegacy(
    const uint64_t packedContext)
{
  const auto corners = unpackSparseVoxelMicroSurfacePatch(
      unpackSparseVoxelMicroContextSurfacePatch(packedContext));
  uint32_t patch = 0u;
  for (uint32_t corner = 0u; corner < corners.size(); ++corner)
  {
    const uint32_t quantized = static_cast<uint32_t>(std::lround(
        std::clamp(corners[corner], 0.0, 1.0) * 63.0));
    patch |= (quantized & 0x3fu) << (corner * 6u);
  }
  return patch |
      (unpackSparseVoxelMicroContextValue(packedContext) << 24u);
}

bool sparseVoxelMicroDescriptorIsValid(const SparseVoxelMicroDescriptor &descriptor)
{
  if ((descriptor.geometryProfile !=
           SparseVoxelMicroGeometryProfile::Staircase &&
       (descriptor.faceMask & ~0x3fu) != 0u) ||
      static_cast<uint32_t>(descriptor.geometryProfile) >
          static_cast<uint32_t>(
              SparseVoxelMicroGeometryProfile::Staircase))
    return false;
  if (descriptor.subdivision == 0u)
  {
    return descriptor.maximumCarveDepth == 0u &&
        descriptor.carveDepthFraction == 0u &&
        descriptor.faceMask == 0u &&
        descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::Disabled;
  }
  return descriptor.subdivision >= 2u &&
      descriptor.subdivision <= kSparseVoxelMicroMaximumSubdivision &&
      descriptor.carveDepthFraction < kCarveDepthFractionScale &&
      maximumCarveDepthFixed(descriptor) <=
          descriptor.subdivision * kCarveDepthFractionScale;
}

bool sparseVoxelMicroDescriptorIsEnabled(const SparseVoxelMicroDescriptor &descriptor)
{
  return sparseVoxelMicroDescriptorIsValid(descriptor) && descriptor.subdivision != 0u && maximumCarveDepthFixed(descriptor) != 0u && effectiveFaceMask(descriptor) != 0u &&
         descriptor.geometryProfile != SparseVoxelMicroGeometryProfile::Disabled;
}

std::array<int32_t, 3> sparseVoxelMicroCanonicalParentKey(const std::array<int32_t, 4> &originScale, const std::array<uint32_t, 3> &parentCell)
{
  return {
    static_cast<int32_t>(static_cast<int64_t>(originScale[0]) +
        static_cast<int64_t>(parentCell[0]) * originScale[3]),
    static_cast<int32_t>(static_cast<int64_t>(originScale[1]) +
        static_cast<int64_t>(parentCell[1]) * originScale[3]),
    static_cast<int32_t>(static_cast<int64_t>(originScale[2]) +
        static_cast<int64_t>(parentCell[2]) * originScale[3]),
  };
}

std::array<int64_t, 3> sparseVoxelMicroCanonicalCell(
    const std::array<int32_t, 3> &parentKey, const uint32_t subdivision,
    const std::array<uint32_t, 3> &cell)
{
  return {
    static_cast<int64_t>(parentKey[0]) * subdivision + cell[0],
    static_cast<int64_t>(parentKey[1]) * subdivision + cell[1],
    static_cast<int64_t>(parentKey[2]) * subdivision + cell[2],
  };
}

std::array<int64_t, 3> sparseVoxelMicroCanonicalFaceColumn(
    const std::array<int32_t, 3> &parentKey, const uint32_t subdivision,
    const SparseVoxelMicroFace face,
    const std::array<uint32_t, 2> &faceCell)
{
  const FaceFrame frame = faceFrame(face);
  std::array<int64_t, 3> coordinate{
    static_cast<int64_t>(parentKey[0]) * subdivision,
    static_cast<int64_t>(parentKey[1]) * subdivision,
    static_cast<int64_t>(parentKey[2]) * subdivision,
  };
  const uint32_t localU = frame.invertU
      ? subdivision - 1u - faceCell[0]
      : faceCell[0];
  const uint32_t localV = frame.invertV
      ? subdivision - 1u - faceCell[1]
      : faceCell[1];
  coordinate[frame.uAxis] += localU;
  coordinate[frame.vAxis] += localV;
  if (faceIsPositive(face))
    coordinate[frame.axis] += subdivision;
  return coordinate;
}

std::array<uint32_t, 2> sparseVoxelMicroFaceCell(const std::array<uint32_t, 3> &cell, const uint32_t subdivision, const SparseVoxelMicroFace face)
{
  const FaceFrame frame = faceFrame(face);
  return {
    frame.invertU ? subdivision - 1u - cell[frame.uAxis] : cell[frame.uAxis],
    frame.invertV ? subdivision - 1u - cell[frame.vAxis] : cell[frame.vAxis],
  };
}

uint32_t packSparseVoxelMicroPayload(const uint16_t descriptorId, const std::array<uint32_t, 3> &cell, const uint32_t subdivision, const SparseVoxelMicroFace carveFace, const bool heightfield, const bool terrainSlope)
{
  if (descriptorId == 0u || subdivision < 2u ||
      subdivision > kSparseVoxelMicroMaximumSubdivision ||
      std::any_of(cell.begin(), cell.end(), [subdivision](const uint32_t coordinate)
      {
        return coordinate >= subdivision;
      }))
    return 0u;
  std::array<uint32_t, 3> payloadCell = cell;
  if (heightfield)
  {
    const FaceFrame frame = faceFrame(carveFace);
    const std::array<uint32_t, 2> faceCell =
        sparseVoxelMicroFaceCell(cell, subdivision, carveFace);
    payloadCell = {
      faceCell[0],
      faceCell[1],
      faceIsPositive(carveFace)
          ? subdivision - 1u - cell[frame.axis]
          : cell[frame.axis],
    };
  }
  return static_cast<uint32_t>(descriptorId) |
      ((payloadCell[0] & kSparseVoxelMicroPayloadCoordinateMask) << kSparseVoxelMicroPayloadUShift) |
      ((payloadCell[1] & kSparseVoxelMicroPayloadCoordinateMask) << kSparseVoxelMicroPayloadVShift) |
      ((payloadCell[2] & kSparseVoxelMicroPayloadCoordinateMask) << kSparseVoxelMicroPayloadDepthShift) |
      kSparseVoxelMicroPayloadValid |
      (heightfield ? kSparseVoxelMicroPayloadHeightfield : 0u) |
      (terrainSlope ? kSparseVoxelMicroPayloadTerrainSlope : 0u);
}

uint32_t packSparseVoxelMicroFallbackPayload(
    const uint16_t descriptorId, const bool terrainSlope)
{
  return descriptorId == 0u
      ? 0u
      : static_cast<uint32_t>(descriptorId) |
          kSparseVoxelMicroPayloadValid |
          kSparseVoxelMicroPayloadFallback |
          (terrainSlope ? kSparseVoxelMicroPayloadTerrainSlope : 0u);
}

double sparseVoxelMicroMaximumCarveDepth(
    const SparseVoxelMicroDescriptor &descriptor)
{
  return static_cast<double>(maximumCarveDepthFixed(descriptor)) /
      kCarveDepthFractionScale;
}

double sparseVoxelMicroCarveDepth(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroFace face, const std::array<uint32_t, 2> &faceCell, const uint32_t context)
{
  if (!sparseVoxelMicroDescriptorIsEnabled(descriptor) ||
      (effectiveFaceMask(descriptor) & (1u << faceIndex(face))) == 0u)
    return 0.0;
  uint32_t fixedDepth = 0u;
  switch (descriptor.geometryProfile)
  {
  case SparseVoxelMicroGeometryProfile::Constant:
    fixedDepth = maximumCarveDepthFixed(descriptor);
    break;
  case SparseVoxelMicroGeometryProfile::HashColumns:
    fixedDepth = hashColumn(descriptor, parentKey, face, faceCell) %
        (maximumCarveDepthFixed(descriptor) + 1u);
    break;
  case SparseVoxelMicroGeometryProfile::ClumpedColumns:
    fixedDepth = clumpedColumnDepth(
        descriptor, parentKey, face, faceCell);
    break;
  case SparseVoxelMicroGeometryProfile::Staircase:
    fixedDepth = staircaseColumnDepth(
        descriptor, parentKey, face, faceCell, context);
    break;
  case SparseVoxelMicroGeometryProfile::VolumeDensity:
    break;
  case SparseVoxelMicroGeometryProfile::Disabled:
    break;
  }
  return static_cast<double>(fixedDepth) / kCarveDepthFractionScale;
}

bool sparseVoxelMicroCellBounds(
    const SparseVoxelMicroDescriptor &descriptor,
    const std::array<int32_t, 3> &parentKey,
    const std::array<uint32_t, 3> &cell,
    std::array<double, 3> &minimum,
    std::array<double, 3> &maximum,
    const uint32_t context)
{
  minimum = {};
  maximum = {};
  if (!sparseVoxelMicroDescriptorIsValid(descriptor))
    return false;
  const uint32_t subdivision = std::max(descriptor.subdivision, 1u);
  if (cell[0] >= subdivision || cell[1] >= subdivision || cell[2] >= subdivision)
    return false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    minimum[axis] = static_cast<double>(cell[axis]) / subdivision;
    maximum[axis] = static_cast<double>(cell[axis] + 1u) / subdivision;
  }
  if (!sparseVoxelMicroDescriptorIsEnabled(descriptor))
    return true;
  if (descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::VolumeDensity)
  {
    // maximumCarveDepth is the empty-density threshold in N equal bins. The
    // global microcell address makes this field continuous across parents.
    const uint32_t fixedThreshold = maximumCarveDepthFixed(descriptor);
    const uint32_t fixedDensity = hashVolumeCell(
        descriptor, parentKey, cell) %
        (subdivision * kCarveDepthFractionScale);
    return fixedDensity >= fixedThreshold;
  }
  for (uint32_t value = 0u; value < 6u; ++value)
  {
    if ((effectiveFaceMask(descriptor) & (1u << value)) == 0u)
      continue;
    const SparseVoxelMicroFace face = static_cast<SparseVoxelMicroFace>(value);
    const uint32_t axis = faceAxis(face);
    const double carveBoundary = sparseVoxelMicroCarveDepth(
        descriptor, parentKey, face,
        sparseVoxelMicroFaceCell(cell, subdivision, face), context) /
        subdivision;
    if (faceIsPositive(face))
      maximum[axis] = std::min(maximum[axis], 1.0 - carveBoundary);
    else
      minimum[axis] = std::max(minimum[axis], carveBoundary);
    if (minimum[axis] >= maximum[axis] - kTraceEpsilon)
      return false;
  }
  return true;
}

bool sparseVoxelMicroCellOccupied(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const std::array<uint32_t, 3> &cell, const uint32_t context)
{
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
  return sparseVoxelMicroCellBounds(
      descriptor, parentKey, cell, minimum, maximum, context);
}

double sparseVoxelMicroHandoffWeight(
    const double projectedMicroPixels, const uint32_t encodedScale)
{
  const double scale = encodedScale == 0u
      ? 1.0
      : std::clamp(static_cast<double>(encodedScale) / 128.0, 0.25, 1.0);
  const double minimum = 0.50 * scale;
  const double maximum = 1.00 * scale;
  const double t = std::clamp(
      (projectedMicroPixels - minimum) / (maximum - minimum), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double sparseVoxelMicroExpectedCoverage(
    const SparseVoxelMicroDescriptor &descriptor, const uint32_t context)
{
  if (!sparseVoxelMicroDescriptorIsEnabled(descriptor))
    return 1.0;
  const double subdivision = static_cast<double>(descriptor.subdivision);
  const double maximumDepth = sparseVoxelMicroMaximumCarveDepth(descriptor);
  if (descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::Constant)
  {
    double coverage = 1.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double negativeDepth =
          (effectiveFaceMask(descriptor) & (1u << (axis * 2u))) != 0u
          ? maximumDepth
          : 0.0;
      const double positiveDepth =
          (effectiveFaceMask(descriptor) & (1u << (axis * 2u + 1u))) != 0u
          ? maximumDepth
          : 0.0;
      const double surviving = subdivision - std::min(
          subdivision, negativeDepth + positiveDepth);
      coverage *= surviving / subdivision;
    }
    return coverage;
  }
  if (descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::VolumeDensity)
    return 1.0 - maximumDepth / subdivision;
  if (descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::Staircase)
  {
    const auto typedContext = static_cast<SparseVoxelMicroContext>(context);
    if (typedContext == SparseVoxelMicroContext::Interior)
      return 1.0;
    if (typedContext == SparseVoxelMicroContext::FlatSurface)
      return 1.0;
    const double staircaseSpan = subdivision - 1.0;
    const double wallSurvival = std::clamp(
        1.0 - staircaseSpan * 0.5 / subdivision, 0.0, 1.0);
    if (typedContext >= SparseVoxelMicroContext::WallNegativeX &&
        typedContext <= SparseVoxelMicroContext::WallPositiveZ)
      return wallSurvival;
    if (sparseVoxelMicroContextIsSurface(context))
    {
      const double phase = static_cast<double>(
          context & kSparseVoxelMicroContextPhaseMask);
      const double averageDepth =
          (static_cast<double>(kSparseVoxelMicroContextPhaseMask) - phase) *
          subdivision /
          static_cast<double>(kSparseVoxelMicroContextPhaseMask);
      const double topSurvival = std::clamp(
          1.0 - averageDepth / subdivision, 0.0, 1.0);
      return topSurvival;
    }
  }
  const double faceSurvival = 1.0 -
      maximumDepth * 0.5 / subdivision;
  return std::pow(
      std::clamp(faceSurvival, 0.0, 1.0),
      static_cast<double>(enabledFaceCount(effectiveFaceMask(descriptor))));
}

SparseVoxelMicroTraceResult traceSparseVoxelMicroSolidStaircaseInterior(
    const SparseVoxelMicroDescriptor &descriptor,
    const SparseVoxelMicroRay &ray,
    const uint32_t context)
{
  SparseVoxelMicroTraceResult result{};
  const bool interior = context ==
      static_cast<uint32_t>(SparseVoxelMicroContext::Interior);
  if (!sparseVoxelMicroDescriptorIsValid(descriptor) ||
      !sparseVoxelMicroDescriptorIsEnabled(descriptor) ||
      descriptor.geometryProfile != SparseVoxelMicroGeometryProfile::Staircase ||
      !interior || !validRay(ray))
    return result;

  Interval parent{};
  if (!intersectBox(
          ray, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, parent))
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
    return result;
  }

  const uint32_t subdivision = descriptor.subdivision;
  const std::array<uint32_t, 3> entryCell =
      cellAt(ray, parent.entry, subdivision);
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    minimum[axis] = static_cast<double>(entryCell[axis]) / subdivision;
    maximum[axis] =
        static_cast<double>(entryCell[axis] + 1u) / subdivision;
  }
  SparseVoxelMicroRay segmentRay = ray;
  segmentRay.minimumDistance = parent.entry;
  segmentRay.maximumDistance = parent.exit;
  Interval solid{};
  if (!intersectBox(segmentRay, minimum, maximum, solid))
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
    result.distance = parent.exit;
    result.steps = 1u;
    return result;
  }
  result.status = SparseVoxelMicroTraceStatus::Hit;
  result.distance = solid.entry;
  result.cell = cellAt(ray, solid.entry, subdivision);
  result.face = solid.entryFace;
  result.steps = 1u;
  return result;
}

SparseVoxelMicroTraceResult traceSparseVoxelMicroOneFace(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroRay &ray, const uint32_t context)
{
  SparseVoxelMicroTraceResult result{};
  if (!sparseVoxelMicroDescriptorIsValid(descriptor) || !validRay(ray))
    return result;
  const bool staircaseSurface =
      descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::Staircase &&
      sparseVoxelMicroContextIsTerrainSurface(context);
  if (sparseVoxelMicroDescriptorIsEnabled(descriptor) &&
      descriptor.geometryProfile == SparseVoxelMicroGeometryProfile::VolumeDensity)
    return traceSparseVoxelMicroVolume(descriptor, parentKey, ray, context);
  if (sparseVoxelMicroDescriptorIsEnabled(descriptor) &&
      enabledFaceCount(effectiveFaceMask(descriptor)) != 1u &&
      !staircaseSurface)
    return result;
  if (!sparseVoxelMicroDescriptorIsEnabled(descriptor))
    return traceSparseVoxelMicroVolume(descriptor, parentKey, ray, context);

  Interval parent{};
  if (!intersectBox(ray, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, parent))
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
    return result;
  }

  const uint32_t subdivision = descriptor.subdivision;
  const SparseVoxelMicroFace carveFace = staircaseSurface
      ? SparseVoxelMicroFace::PositiveY
      : onlyEnabledFace(effectiveFaceMask(descriptor));
  const FaceFrame frame = faceFrame(carveFace);
  const std::array<double, 3> entryPoint = pointAt(ray, parent.entry);
  const double u = orientedCoordinate(entryPoint, frame.uAxis, frame.invertU);
  const double v = orientedCoordinate(entryPoint, frame.vAxis, frame.invertV);
  const double du = orientedDirection(ray, frame.uAxis, frame.invertU);
  const double dv = orientedDirection(ray, frame.vAxis, frame.invertV);
  uint32_t uCell = cellCoordinate(u, du, subdivision);
  uint32_t vCell = cellCoordinate(v, dv, subdivision);
  int32_t uStep = (du > 0.0) - (du < 0.0);
  int32_t vStep = (dv > 0.0) - (dv < 0.0);
  auto nextBoundary = [&](const double coordinate, const double direction, const uint32_t cell, const int32_t step)
  {
    if (step == 0)
      return std::numeric_limits<double>::infinity();
    const double boundary = static_cast<double>(step > 0 ? cell + 1u : cell) / subdivision;
    return parent.entry + (boundary - coordinate) / direction;
  };
  double nextU = nextBoundary(u, du, uCell, uStep);
  double nextV = nextBoundary(v, dv, vCell, vStep);
  const double deltaU = du == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / (std::abs(du) * subdivision);
  const double deltaV = dv == 0.0 ? std::numeric_limits<double>::infinity() : 1.0 / (std::abs(dv) * subdivision);
  double current = parent.entry;

  for (uint32_t stepIndex = 0u; stepIndex < 2u * subdivision + 2u; ++stepIndex)
  {
    result.steps = stepIndex + 1u;
    const double carveDepth = sparseVoxelMicroCarveDepth(
        descriptor, parentKey, carveFace, {uCell, vCell}, context);
    const double columnExit = std::min({nextU, nextV, parent.exit});
    if (carveDepth < subdivision)
    {
      std::array<double, 3> minimum{0.0, 0.0, 0.0};
      std::array<double, 3> maximum{1.0, 1.0, 1.0};
      setOrientedBounds(minimum, maximum, frame.uAxis, frame.invertU, static_cast<double>(uCell) / subdivision, static_cast<double>(uCell + 1u) / subdivision);
      setOrientedBounds(minimum, maximum, frame.vAxis, frame.invertV, static_cast<double>(vCell) / subdivision, static_cast<double>(vCell + 1u) / subdivision);
      if (faceIsPositive(carveFace))
        maximum[frame.axis] = static_cast<double>(subdivision - carveDepth) / subdivision;
      else
        minimum[frame.axis] = static_cast<double>(carveDepth) / subdivision;

      SparseVoxelMicroRay segmentRay = ray;
      segmentRay.minimumDistance = current;
      segmentRay.maximumDistance = columnExit;
      Interval solid{};
      if (intersectBox(segmentRay, minimum, maximum, solid))
      {
        result.status = SparseVoxelMicroTraceStatus::Hit;
        result.distance = solid.entry;
        result.cell = cellAt(ray, solid.entry, subdivision);
        result.face = solid.entryFace;
        return result;
      }
    }
    if (columnExit >= parent.exit - kTraceEpsilon)
      break;
    current = columnExit;
    if (nextU <= nextV)
    {
      const int64_t next = static_cast<int64_t>(uCell) + uStep;
      if (next < 0 || next >= subdivision)
        break;
      uCell = static_cast<uint32_t>(next);
      nextU += deltaU;
    }
    else
    {
      const int64_t next = static_cast<int64_t>(vCell) + vStep;
      if (next < 0 || next >= subdivision)
        break;
      vCell = static_cast<uint32_t>(next);
      nextV += deltaV;
    }
  }
  result.status = SparseVoxelMicroTraceStatus::Miss;
  result.distance = parent.exit;
  return result;
}

SparseVoxelMicroTraceResult traceSparseVoxelMicroVolume(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroRay &ray, const uint32_t context)
{
  SparseVoxelMicroTraceResult result{};
  if (!sparseVoxelMicroDescriptorIsValid(descriptor) || !validRay(ray))
    return result;
  Interval parent{};
  if (!intersectBox(ray, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, parent))
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
    return result;
  }

  const uint32_t subdivision = std::max(descriptor.subdivision, 1u);
  std::array<uint32_t, 3> cell = cellAt(ray, parent.entry, subdivision);
  std::array<int32_t, 3> step{};
  std::array<double, 3> next{};
  std::array<double, 3> delta{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double direction = ray.direction[axis];
    step[axis] = (direction > 0.0) - (direction < 0.0);
    if (step[axis] == 0)
    {
      next[axis] = std::numeric_limits<double>::infinity();
      delta[axis] = std::numeric_limits<double>::infinity();
      continue;
    }
    const double boundary = static_cast<double>(step[axis] > 0 ? cell[axis] + 1u : cell[axis]) / subdivision;
    next[axis] = (boundary - ray.origin[axis]) / direction;
    delta[axis] = 1.0 / (std::abs(direction) * subdivision);
  }

  double current = parent.entry;
  for (uint32_t stepIndex = 0u; stepIndex < 3u * subdivision + 3u; ++stepIndex)
  {
    result.steps = stepIndex + 1u;
    std::array<double, 3> solidMinimum{};
    std::array<double, 3> solidMaximum{};
    if (sparseVoxelMicroCellBounds(
            descriptor, parentKey, cell, solidMinimum, solidMaximum,
            context))
    {
      SparseVoxelMicroRay segmentRay = ray;
      segmentRay.minimumDistance = current;
      segmentRay.maximumDistance = std::min(
          {next[0], next[1], next[2], parent.exit});
      Interval solid{};
      if (intersectBox(
              segmentRay, solidMinimum, solidMaximum, solid))
      {
        result.status = SparseVoxelMicroTraceStatus::Hit;
        result.distance = solid.entry;
        result.cell = cell;
        result.face = solid.entryFace;
        return result;
      }
    }
    uint32_t axis = 0u;
    if (next[1] < next[axis])
      axis = 1u;
    if (next[2] < next[axis])
      axis = 2u;
    current = next[axis];
    if (current > parent.exit + kTraceEpsilon)
      break;
    const int64_t nextCell = static_cast<int64_t>(cell[axis]) + step[axis];
    if (nextCell < 0 || nextCell >= subdivision)
      break;
    cell[axis] = static_cast<uint32_t>(nextCell);
    next[axis] += delta[axis];
  }
  result.status = SparseVoxelMicroTraceStatus::Miss;
  result.distance = parent.exit;
  return result;
}

SparseVoxelMicroParentTraceResult traceSparseVoxelMicroParents(const SparseVoxelMicroParentGrid &grid, const SparseVoxelMicroRay &ray)
{
  SparseVoxelMicroParentTraceResult result{};
  if (!gridIsValid(grid) || !validRay(ray))
    return result;
  const std::array<double, 3> maximum{
    static_cast<double>(grid.dimensions[0]),
    static_cast<double>(grid.dimensions[1]),
    static_cast<double>(grid.dimensions[2]),
  };
  Interval volume{};
  if (!intersectBox(ray, {0.0, 0.0, 0.0}, maximum, volume))
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
    return result;
  }

  const std::array<double, 3> entryPoint = pointAt(ray, volume.entry);
  std::array<uint32_t, 3> cell{};
  std::array<int32_t, 3> step{};
  std::array<double, 3> next{};
  std::array<double, 3> delta{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    cell[axis] = cellCoordinate(entryPoint[axis] / maximum[axis], ray.direction[axis], grid.dimensions[axis]);
    step[axis] = (ray.direction[axis] > 0.0) - (ray.direction[axis] < 0.0);
    if (step[axis] == 0)
    {
      next[axis] = std::numeric_limits<double>::infinity();
      delta[axis] = std::numeric_limits<double>::infinity();
      continue;
    }
    const double boundary = static_cast<double>(step[axis] > 0 ? cell[axis] + 1u : cell[axis]);
    next[axis] = (boundary - ray.origin[axis]) / ray.direction[axis];
    delta[axis] = 1.0 / std::abs(ray.direction[axis]);
  }

  double current = volume.entry;
  SparseVoxelMicroFace currentFace = volume.entryFace;
  const uint32_t maximumSteps = grid.dimensions[0] + grid.dimensions[1] + grid.dimensions[2] + 3u;
  for (uint32_t stepIndex = 0u; stepIndex < maximumSteps; ++stepIndex)
  {
    result.parentSteps = stepIndex + 1u;
    const uint32_t index = parentIndex(grid, cell);
    if (grid.occupied[index] != 0u)
    {
      const SparseVoxelMicroDescriptor descriptor = grid.descriptors.empty() ? SparseVoxelMicroDescriptor{} : grid.descriptors[index];
      const uint32_t context = grid.contexts.empty() ? 0u : grid.contexts[index];
      if (!sparseVoxelMicroDescriptorIsEnabled(descriptor))
      {
        result.status = SparseVoxelMicroTraceStatus::Hit;
        result.distance = current;
        result.parentCell = cell;
        result.face = currentFace;
        return result;
      }

      const double parentExit = std::min({next[0], next[1], next[2], volume.exit});
      SparseVoxelMicroRay localRay = ray;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        localRay.origin[axis] -= cell[axis];
      localRay.minimumDistance = current;
      localRay.maximumDistance = parentExit;
      const std::array<int32_t, 3> parentKey{
        static_cast<int32_t>(cell[0]),
        static_cast<int32_t>(cell[1]),
        static_cast<int32_t>(cell[2]),
      };
      const SparseVoxelMicroTraceResult micro =
          enabledFaceCount(effectiveFaceMask(descriptor)) == 1u
          ? traceSparseVoxelMicroOneFace(
                descriptor, parentKey, localRay, context)
          : traceSparseVoxelMicroVolume(
                descriptor, parentKey, localRay, context);
      if (micro.hit())
      {
        result.status = SparseVoxelMicroTraceStatus::Hit;
        result.distance = micro.distance;
        result.parentCell = cell;
        result.microCell = micro.cell;
        result.face = micro.face;
        result.microHit = true;
        return result;
      }
      ++result.parentContinuations;
    }

    uint32_t axis = 0u;
    if (next[1] < next[axis])
      axis = 1u;
    if (next[2] < next[axis])
      axis = 2u;
    current = next[axis];
    if (current > volume.exit + kTraceEpsilon)
      break;
    const int64_t nextCell = static_cast<int64_t>(cell[axis]) + step[axis];
    if (nextCell < 0 || nextCell >= grid.dimensions[axis])
      break;
    cell[axis] = static_cast<uint32_t>(nextCell);
    next[axis] += delta[axis];
    currentFace = enteringFace(axis, ray.direction[axis]);
  }
  result.status = SparseVoxelMicroTraceStatus::Miss;
  result.distance = volume.exit;
  return result;
}

} // namespace voxel::svraster
