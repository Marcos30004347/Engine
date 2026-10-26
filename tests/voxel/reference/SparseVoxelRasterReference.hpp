#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace voxel::svraster::reference
{

struct ProjectionFixture
{
  SparseVoxelRasterUniformsGPU uniforms{};
  std::vector<SparseVoxelInstanceGPU> instances;
  std::vector<VisibleInstanceGPU> visibleInstances;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelPathLevelGPU> pathsLevels;
};

struct ScanResult
{
  std::vector<uint32_t> offsets;
  uint32_t requiredCount = 0u;
  uint32_t candidateCount = 0u;
  bool overflow = false;
};

struct Ray
{
  std::array<float, 3> origin{};
  std::array<float, 3> direction{};
};

struct VoxelSegment
{
  float tEnter = 0.0f;
  float tExit = 0.0f;
  uint32_t enterFace = 0u;
  uint32_t exitFace = 0u;
  bool hit = false;
  bool cameraInside = false;
};

struct ResolveFixture
{
  SparseVoxelRasterUniformsGPU uniforms{};
  std::vector<SparseVoxelInstanceGPU> instances;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<ProjectedVoxelGPU> projected;
  std::vector<TileCandidateGPU> candidates;
  std::vector<SparseVoxelTileRangeGPU> ranges;
};

struct ResolveResult
{
  std::vector<ResolvedVoxelSurfaceGPU> surfaces;
  std::vector<uint32_t> coverage;
  std::vector<float> linearDepth;
};

struct GBufferFixture
{
  SparseVoxelRasterUniformsGPU uniforms{};
  std::vector<SparseVoxelInstanceGPU> instances;
  std::vector<SparseVoxelMaterialGPU> materials;
  std::vector<ResolvedVoxelSurfaceGPU> surfaces;
};

inline std::array<float, 3> transformPoint(const std::array<float, 16> &matrix, const std::array<float, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

inline std::array<float, 3> transformVector(const std::array<float, 16> &matrix, const std::array<float, 3> &vector)
{
  return {
    matrix[0] * vector[0] + matrix[4] * vector[1] + matrix[8] * vector[2],
    matrix[1] * vector[0] + matrix[5] * vector[1] + matrix[9] * vector[2],
    matrix[2] * vector[0] + matrix[6] * vector[1] + matrix[10] * vector[2],
  };
}

inline std::array<float, 3> normalized(const std::array<float, 3> &value)
{
  const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
  return {value[0] / length, value[1] / length, value[2] / length};
}

inline uint32_t roundToNearestEvenNonNegative(const float value)
{
  const float baseFloat = std::floor(value);
  const uint32_t base = static_cast<uint32_t>(baseFloat);
  const float fraction = value - baseFloat;
  return base + static_cast<uint32_t>(fraction > 0.5f || (fraction == 0.5f && (base & 1u) != 0u));
}

inline uint32_t packPixel(const uint32_t x, const uint32_t y)
{
  return ((x & 0xffffu) << 16u) | (y & 0xffffu);
}
inline std::array<uint32_t, 2> unpackPixel(const uint32_t packed)
{
  return {packed >> 16u, packed & 0xffffu};
}

inline uint32_t cameraQuadrant(const std::array<float, 3> &corner, const std::array<float, 3> &camera)
{
  return (corner[0] < camera[0] ? 4u : 0u) | (corner[1] < camera[1] ? 2u : 0u) | (corner[2] < camera[2] ? 1u : 0u);
}

inline std::array<uint32_t, 4> tileRectangle(const ProjectedVoxelGPU &projected, const SparseVoxelRasterUniformsGPU &uniforms)
{
  const auto minimum = unpackPixel(projected.packedInclusivePixelBounds[0]);
  const auto maximum = unpackPixel(projected.packedInclusivePixelBounds[1]);
  const uint32_t lastX = std::max(1u, uniforms.viewportTiles[2]) - 1u;
  const uint32_t lastY = std::max(1u, uniforms.viewportTiles[3]) - 1u;
  return {
    std::min(minimum[0] / kSparseVoxelTileSize, lastX),
    std::min(minimum[1] / kSparseVoxelTileSize, lastY),
    std::min(maximum[0] / kSparseVoxelTileSize, lastX),
    std::min(maximum[1] / kSparseVoxelTileSize, lastY),
  };
}

inline std::vector<ProjectedVoxelGPU> preprocess(const ProjectionFixture &fixture)
{
  uint32_t visibleVoxelCount = 0u;
  for (const VisibleInstanceGPU &visible : fixture.visibleInstances)
    visibleVoxelCount += visible.visibleVoxelCount;
  std::vector<ProjectedVoxelGPU> result(visibleVoxelCount);
  uint32_t outputIndex = 0u;
  for (const VisibleInstanceGPU &visible : fixture.visibleInstances)
  {
    for (uint32_t localIndex = 0u; localIndex < visible.visibleVoxelCount; ++localIndex, ++outputIndex)
    {
      ProjectedVoxelGPU projected{};
      projected.instanceIndex = kInvalidSparseVoxelIndex;
      projected.voxelIndex = kInvalidSparseVoxelIndex;
      const uint32_t voxelIndex = visible.firstVisibleVoxel + localIndex;
      const SparseVoxelCenterLengthGPU &centerLength = fixture.centersLengths.at(voxelIndex);
      const SparseVoxelInstanceGPU &instance = fixture.instances.at(visible.instanceIndex);
      const std::array<float, 3> camera = {fixture.uniforms.cameraWorldPositionNear[0], fixture.uniforms.cameraWorldPositionNear[1], fixture.uniforms.cameraWorldPositionNear[2]};
      const std::array<float, 3> cameraLocal = transformPoint(instance.worldToModel, camera);
      const float nearDistance = fixture.uniforms.cameraWorldPositionNear[3];

      std::array<float, 2> coordinateMinimum = {1.0e30f, 1.0e30f};
      std::array<float, 2> coordinateMaximum = {-1.0e30f, -1.0e30f};
      uint32_t quadrantBitset = 0u;
      uint32_t projectedVertexCount = 0u;
      std::array<std::array<float, 3>, 8> cameraCorners{};
      std::array<float, 3> worldMinimum{1.0e30f, 1.0e30f, 1.0e30f};
      std::array<float, 3> worldMaximum{-1.0e30f, -1.0e30f, -1.0e30f};
      const float radius = 0.5f * centerLength.length;
      for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
      {
        const std::array<float, 3> localCorner = {
          centerLength.center[0] + radius * ((cornerIndex & 4u) != 0u ? 1.0f : -1.0f),
          centerLength.center[1] + radius * ((cornerIndex & 2u) != 0u ? 1.0f : -1.0f),
          centerLength.center[2] + radius * ((cornerIndex & 1u) != 0u ? 1.0f : -1.0f),
        };
        const std::array<float, 3> worldCorner = transformPoint(instance.modelToWorld, localCorner);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          worldMinimum[axis] = std::min(worldMinimum[axis], worldCorner[axis]);
          worldMaximum[axis] = std::max(worldMaximum[axis], worldCorner[axis]);
        }
        const std::array<float, 3> cameraCorner = transformPoint(fixture.uniforms.worldToCamera, worldCorner);
        cameraCorners[cornerIndex] = cameraCorner;
        quadrantBitset |= 1u << cameraQuadrant(localCorner, cameraLocal);
        if (cameraCorner[2] >= nearDistance)
        {
          ++projectedVertexCount;
          const float x = cameraCorner[0] / cameraCorner[2];
          const float y = cameraCorner[1] / cameraCorner[2];
          coordinateMinimum[0] = std::min(coordinateMinimum[0], x);
          coordinateMinimum[1] = std::min(coordinateMinimum[1], y);
          coordinateMaximum[0] = std::max(coordinateMaximum[0], x);
          coordinateMaximum[1] = std::max(coordinateMaximum[1], y);
        }
      }
      constexpr std::array<uint32_t, 3> edgeMasks{4u, 2u, 1u};
      for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
      {
        for (const uint32_t edgeMask : edgeMasks)
        {
          if ((cornerIndex & edgeMask) != 0u)
            continue;
          const auto &a = cameraCorners[cornerIndex];
          const auto &b = cameraCorners[cornerIndex | edgeMask];
          const bool aInside = a[2] >= nearDistance;
          const bool bInside = b[2] >= nearDistance;
          if (aInside == bInside)
            continue;
          const float interpolation = (nearDistance - a[2]) / (b[2] - a[2]);
          const float x = (a[0] + (b[0] - a[0]) * interpolation) / nearDistance;
          const float y = (a[1] + (b[1] - a[1]) * interpolation) / nearDistance;
          ++projectedVertexCount;
          coordinateMinimum[0] = std::min(coordinateMinimum[0], x);
          coordinateMinimum[1] = std::min(coordinateMinimum[1], y);
          coordinateMaximum[0] = std::max(coordinateMaximum[0], x);
          coordinateMaximum[1] = std::max(coordinateMaximum[1], y);
        }
      }
      if (projectedVertexCount == 0u)
      {
        result[outputIndex] = projected;
        continue;
      }

      const float minimumX = fixture.uniforms.focalPrincipal[0] * coordinateMinimum[0] + fixture.uniforms.focalPrincipal[2] - 0.5f;
      const float minimumY = fixture.uniforms.focalPrincipal[1] * coordinateMinimum[1] + fixture.uniforms.focalPrincipal[3] - 0.5f;
      const float maximumX = fixture.uniforms.focalPrincipal[0] * coordinateMaximum[0] + fixture.uniforms.focalPrincipal[2] - 0.5f;
      const float maximumY = fixture.uniforms.focalPrincipal[1] * coordinateMaximum[1] + fixture.uniforms.focalPrincipal[3] - 0.5f;
      const float viewportX = static_cast<float>(fixture.uniforms.viewportTiles[0]);
      const float viewportY = static_cast<float>(fixture.uniforms.viewportTiles[1]);
      if (minimumX > viewportX || minimumY > viewportY || maximumX < 0.0f || maximumY < 0.0f)
      {
        result[outputIndex] = projected;
        continue;
      }
      const uint32_t pixelMinimumX = static_cast<uint32_t>(std::floor(std::clamp(minimumX, 0.0f, viewportX)));
      const uint32_t pixelMinimumY = static_cast<uint32_t>(std::floor(std::clamp(minimumY, 0.0f, viewportY)));
      const uint32_t pixelMaximumX = static_cast<uint32_t>(std::ceil(std::clamp(maximumX, 0.0f, viewportX)));
      const uint32_t pixelMaximumY = static_cast<uint32_t>(std::ceil(std::clamp(maximumY, 0.0f, viewportY)));
      if (pixelMinimumX > pixelMaximumX || pixelMinimumY > pixelMaximumY)
      {
        result[outputIndex] = projected;
        continue;
      }
      projected.packedInclusivePixelBounds = {packPixel(pixelMinimumX, pixelMinimumY), packPixel(pixelMaximumX, pixelMaximumY)};
      projected.quadrantBitset = quadrantBitset;
      projected.instanceIndex = visible.instanceIndex;
      projected.voxelIndex = voxelIndex;
      const auto rectangle = tileRectangle(projected, fixture.uniforms);
      projected.duplicateCount = (rectangle[2] - rectangle[0] + 1u) * (rectangle[3] - rectangle[1] + 1u) * static_cast<uint32_t>(__builtin_popcount(quadrantBitset));
      std::array<float, 3> distanceVector{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        distanceVector[axis] = std::max(
            {worldMinimum[axis] - camera[axis], 0.0f,
             camera[axis] - worldMaximum[axis]});
      }
      const float minimumHitDistance = std::sqrt(
          distanceVector[0] * distanceVector[0] +
          distanceVector[1] * distanceVector[1] +
          distanceVector[2] * distanceVector[2]);
      std::memcpy(
          &projected.reserved0, &minimumHitDistance,
          sizeof(projected.reserved0));
      result[outputIndex] = projected;
    }
  }
  return result;
}

inline ScanResult exclusiveScan(const std::vector<ProjectedVoxelGPU> &projected, const uint32_t capacity)
{
  ScanResult result{};
  result.offsets.resize(projected.size());
  for (size_t index = 0u; index < projected.size(); ++index)
  {
    result.offsets[index] = result.requiredCount;
    result.requiredCount += projected[index].duplicateCount;
  }
  result.overflow = result.requiredCount > capacity;
  result.candidateCount = result.overflow ? 0u : result.requiredCount;
  return result;
}

inline std::array<uint32_t, 2> repeatedQuadrantBits(const uint32_t quadrant)
{
  uint64_t value = 0u;
  for (uint32_t level = 0u; level < kMaxOctreeLevels; ++level)
    value = (value << 3u) | (quadrant & 7u);
  return {static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u)};
}

inline uint64_t candidateKey(const TileCandidateGPU &candidate)
{
  return static_cast<uint64_t>(candidate.keyLo) | (static_cast<uint64_t>(candidate.keyHi) << 32u);
}

inline std::vector<TileCandidateGPU>
emit(const std::vector<ProjectedVoxelGPU> &projected, const std::vector<uint32_t> &offsets, const std::vector<SparseVoxelPathLevelGPU> &pathsLevels, const SparseVoxelRasterUniformsGPU &uniforms)
{
  uint32_t count = 0u;
  for (const ProjectedVoxelGPU &voxel : projected)
    count += voxel.duplicateCount;
  std::vector<TileCandidateGPU> result(count);
  for (uint32_t projectedIndex = 0u; projectedIndex < projected.size(); ++projectedIndex)
  {
    const ProjectedVoxelGPU &voxel = projected[projectedIndex];
    if (voxel.duplicateCount == 0u)
      continue;
    uint32_t output = offsets[projectedIndex];
    const auto rectangle = tileRectangle(voxel, uniforms);
    const SparseVoxelPathLevelGPU &path = pathsLevels.at(voxel.voxelIndex);
    for (uint32_t quadrant = 0u; quadrant < 8u; ++quadrant)
    {
      if ((voxel.quadrantBitset & (1u << quadrant)) == 0u)
        continue;
      const auto repeated = repeatedQuadrantBits(quadrant);
      const uint32_t rankLo = path.pathLo ^ repeated[0];
      const uint32_t rankHi = (path.pathHi ^ repeated[1]) & 0xffffu;
      for (uint32_t y = rectangle[1]; y <= rectangle[3]; ++y)
      {
        for (uint32_t x = rectangle[0]; x <= rectangle[2]; ++x)
        {
          const uint32_t tile = y * uniforms.viewportTiles[2] + x;
          result[output] = TileCandidateGPU{rankLo, (tile << 16u) | rankHi, voxel.instanceIndex, voxel.voxelIndex, quadrant, projectedIndex, output, 0u};
          ++output;
        }
      }
    }
  }
  return result;
}

inline void stableSort(std::vector<TileCandidateGPU> &candidates)
{
  std::stable_sort(
      candidates.begin(),
      candidates.end(),
      [](const TileCandidateGPU &left, const TileCandidateGPU &right)
      {
        return candidateKey(left) < candidateKey(right);
      });
}

inline std::vector<SparseVoxelTileRangeGPU> buildRanges(const std::vector<TileCandidateGPU> &candidates, const uint32_t tileCount)
{
  std::vector<SparseVoxelTileRangeGPU> ranges(tileCount);
  for (uint32_t index = 0u; index < candidates.size(); ++index)
  {
    const uint32_t tile = candidates[index].keyHi >> 16u;
    if (index == 0u || (candidates[index - 1u].keyHi >> 16u) != tile)
      ranges.at(tile).begin = index;
    if (index + 1u == candidates.size() || (candidates[index + 1u].keyHi >> 16u) != tile)
      ranges.at(tile).end = index + 1u;
  }
  return ranges;
}

inline uint32_t packLocalFace(const uint32_t axis, const bool positive)
{
  return axis | (positive ? 4u : 0u);
}

inline std::array<float, 3> unpackLocalFaceNormal(const uint32_t packedFace)
{
  std::array<float, 3> result{};
  const uint32_t axis = packedFace & 3u;
  if (axis < 3u)
    result[axis] = (packedFace & 4u) != 0u ? 1.0f : -1.0f;
  return result;
}

inline uint32_t rayQuadrant(const std::array<float, 3> &direction)
{
  return (direction[0] < 0.0f ? 4u : 0u) | (direction[1] < 0.0f ? 2u : 0u) | (direction[2] < 0.0f ? 1u : 0u);
}

inline VoxelSegment voxelSegmentForPixel(const Ray &ray, const SparseVoxelCenterLengthGPU &voxel)
{
  const float radius = 0.5f * voxel.length;
  const std::array<float, 3> minimum = {
    std::nextafter(voxel.center[0] - radius, -std::numeric_limits<float>::infinity()),
    std::nextafter(voxel.center[1] - radius, -std::numeric_limits<float>::infinity()),
    std::nextafter(voxel.center[2] - radius, -std::numeric_limits<float>::infinity()),
  };
  const std::array<float, 3> maximum = {
    std::nextafter(voxel.center[0] + radius, std::numeric_limits<float>::infinity()),
    std::nextafter(voxel.center[1] + radius, std::numeric_limits<float>::infinity()),
    std::nextafter(voxel.center[2] + radius, std::numeric_limits<float>::infinity()),
  };
  bool inside = true;
  float tEnter = -std::numeric_limits<float>::max();
  float tExit = std::numeric_limits<float>::max();
  uint32_t enterFace = 0u;
  uint32_t exitFace = 0u;
  float enterFaceScore = -1.0f;
  float exitFaceScore = -1.0f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    inside &= ray.origin[axis] >= minimum[axis] && ray.origin[axis] <= maximum[axis];
    if (std::abs(ray.direction[axis]) <= 1.0e-20f)
    {
      if (ray.origin[axis] < minimum[axis] || ray.origin[axis] > maximum[axis])
        return {};
      continue;
    }
    const float t0 = (minimum[axis] - ray.origin[axis]) / ray.direction[axis];
    const float t1 = (maximum[axis] - ray.origin[axis]) / ray.direction[axis];
    const float nearT = std::min(t0, t1);
    const float farT = std::max(t0, t1);
    const float faceScore = std::abs(ray.direction[axis]);
    if (enterFaceScore < 0.0f)
    {
      tEnter = nearT;
      enterFace = packLocalFace(axis, ray.direction[axis] < 0.0f);
      enterFaceScore = faceScore;
    }
    else
    {
      const float enterTolerance = kSparseVoxelSurfaceTieEpsilon * std::max(1.0f, std::max(std::abs(nearT), std::abs(tEnter)));
      const float scoreTolerance = kSparseVoxelSurfaceTieEpsilon * std::max(1.0f, std::max(faceScore, enterFaceScore));
      if (nearT > tEnter + enterTolerance || (std::abs(nearT - tEnter) <= enterTolerance && faceScore > enterFaceScore + scoreTolerance))
      {
        enterFace = packLocalFace(axis, ray.direction[axis] < 0.0f);
        enterFaceScore = faceScore;
      }
      tEnter = std::max(tEnter, nearT);
    }
    if (exitFaceScore < 0.0f)
    {
      tExit = farT;
      exitFace = packLocalFace(axis, ray.direction[axis] > 0.0f);
      exitFaceScore = faceScore;
    }
    else
    {
      const float exitTolerance = kSparseVoxelSurfaceTieEpsilon * std::max(1.0f, std::max(std::abs(farT), std::abs(tExit)));
      const float scoreTolerance = kSparseVoxelSurfaceTieEpsilon * std::max(1.0f, std::max(faceScore, exitFaceScore));
      if (farT < tExit - exitTolerance || (std::abs(farT - tExit) <= exitTolerance && faceScore > exitFaceScore + scoreTolerance))
      {
        exitFace = packLocalFace(axis, ray.direction[axis] > 0.0f);
        exitFaceScore = faceScore;
      }
      tExit = std::min(tExit, farT);
    }
  }
  if (tEnter > tExit || tExit < 0.0f)
    return {};
  const float surfaceT = inside ? tExit : tEnter;
  if (surfaceT < 0.0f)
    return {};
  return {tEnter, tExit, enterFace, exitFace, true, inside};
}

inline bool hitPrecedes(
    const float candidateT,
    const uint32_t candidateInstance,
    const uint32_t candidateVoxel,
    const float candidateFaceScore,
    const float bestT,
    const uint32_t bestInstance,
    const uint32_t bestVoxel,
    const float bestFaceScore)
{
  if (bestVoxel == kInvalidSparseVoxelIndex)
    return true;
  const auto quantizedPositiveFloat = [](const float value, const uint32_t rounding, const uint32_t shift)
  {
    const float positive = std::max(value, 0.0f);
    uint32_t bits = 0u;
    std::memcpy(&bits, &positive, sizeof(bits));
    return (bits + rounding) >> shift;
  };
  const uint32_t candidateTKey = quantizedPositiveFloat(candidateT, 128u, 8u);
  const uint32_t bestTKey = quantizedPositiveFloat(bestT, 128u, 8u);
  if (candidateTKey < bestTKey)
    return true;
  if (candidateTKey > bestTKey)
    return false;
  if (candidateInstance != bestInstance)
    return candidateInstance < bestInstance;
  const uint32_t candidateScoreKey =
      quantizedPositiveFloat(candidateFaceScore, 64u, 7u);
  const uint32_t bestScoreKey = quantizedPositiveFloat(bestFaceScore, 64u, 7u);
  if (candidateScoreKey != bestScoreKey)
    return candidateScoreKey > bestScoreKey;
  return candidateVoxel < bestVoxel;
}

inline Ray cameraRay(const SparseVoxelRasterUniformsGPU &uniforms, const uint32_t x, const uint32_t y)
{
  const std::array<float, 3> cameraDirection = {
    (static_cast<float>(x) + 0.5f - uniforms.focalPrincipal[2]) / uniforms.focalPrincipal[0],
    (static_cast<float>(y) + 0.5f - uniforms.focalPrincipal[3]) / uniforms.focalPrincipal[1],
    1.0f,
  };
  return {
    {uniforms.cameraWorldPositionNear[0], uniforms.cameraWorldPositionNear[1], uniforms.cameraWorldPositionNear[2]},
    normalized(transformVector(uniforms.cameraToWorld, cameraDirection)),
  };
}

inline ResolveResult resolveSolidVoxels(const ResolveFixture &fixture)
{
  const uint32_t width = fixture.uniforms.viewportTiles[0];
  const uint32_t height = fixture.uniforms.viewportTiles[1];
  ResolveResult result{};
  result.surfaces.resize(static_cast<size_t>(width) * height);
  result.coverage.resize(static_cast<size_t>(width) * height);
  result.linearDepth.resize(static_cast<size_t>(width) * height);
  for (ResolvedVoxelSurfaceGPU &surface : result.surfaces)
  {
    surface.instanceIndex = kInvalidSparseVoxelIndex;
    surface.voxelIndex = kInvalidSparseVoxelIndex;
  }

  for (uint32_t y = 0u; y < height; ++y)
  {
    for (uint32_t x = 0u; x < width; ++x)
    {
      const uint32_t pixelIndex = y * width + x;
      const uint32_t tile = (y / kSparseVoxelTileSize) * fixture.uniforms.viewportTiles[2] + x / kSparseVoxelTileSize;
      const SparseVoxelTileRangeGPU &range = fixture.ranges.at(tile);
      const Ray worldRay = cameraRay(fixture.uniforms, x, y);
      float bestT = std::numeric_limits<float>::max();
      uint32_t bestInstance = kInvalidSparseVoxelIndex;
      uint32_t bestVoxel = kInvalidSparseVoxelIndex;
      uint32_t bestFace = 0u;
      float bestFaceScore = -1.0f;
      bool bestInside = false;
      for (uint32_t candidateIndex = range.begin; candidateIndex < range.end; ++candidateIndex)
      {
        const TileCandidateGPU &candidate = fixture.candidates.at(candidateIndex);
        const ProjectedVoxelGPU &projected = fixture.projected.at(candidate.projectedVoxelIndex);
        const auto minimum = unpackPixel(projected.packedInclusivePixelBounds[0]);
        const auto maximum = unpackPixel(projected.packedInclusivePixelBounds[1]);
        if (x < minimum[0] || y < minimum[1] || x > maximum[0] || y > maximum[1])
          continue;
        const SparseVoxelInstanceGPU &instance = fixture.instances.at(candidate.instanceIndex);
        const Ray localRay{transformPoint(instance.worldToModel, worldRay.origin), transformVector(instance.worldToModel, worldRay.direction)};
        if (rayQuadrant(localRay.direction) != candidate.quadrant)
          continue;
        const VoxelSegment segment = voxelSegmentForPixel(localRay, fixture.centersLengths.at(candidate.voxelIndex));
        if (!segment.hit)
          continue;
        const float surfaceT = segment.cameraInside ? segment.tExit : segment.tEnter;
        const uint32_t face = segment.cameraInside ? segment.exitFace : segment.enterFace;
        const float faceScore = std::abs(localRay.direction[face & 3u]);
        if (hitPrecedes(surfaceT, candidate.instanceIndex, candidate.voxelIndex, faceScore, bestT, bestInstance, bestVoxel, bestFaceScore))
        {
          bestT = surfaceT;
          bestInstance = candidate.instanceIndex;
          bestVoxel = candidate.voxelIndex;
          bestFace = face;
          bestFaceScore = faceScore;
          bestInside = segment.cameraInside;
        }
      }
      if (bestVoxel != kInvalidSparseVoxelIndex)
      {
        result.surfaces[pixelIndex] = ResolvedVoxelSurfaceGPU{
          bestT,
          bestInstance,
          bestVoxel,
          bestFace,
          kSparseVoxelSurfaceHit | (bestInside ? kSparseVoxelSurfaceCameraInside : 0u),
          0u,
          0u,
          0u,
        };
        result.coverage[pixelIndex] = 1u;
        result.linearDepth[pixelIndex] = bestT;
      }
    }
  }
  return result;
}

inline std::array<float, 4> transformVec4(const std::array<float, 16> &matrix, const std::array<float, 4> &value)
{
  return {
    matrix[0] * value[0] + matrix[4] * value[1] + matrix[8] * value[2] + matrix[12] * value[3],
    matrix[1] * value[0] + matrix[5] * value[1] + matrix[9] * value[2] + matrix[13] * value[3],
    matrix[2] * value[0] + matrix[6] * value[1] + matrix[10] * value[2] + matrix[14] * value[3],
    matrix[3] * value[0] + matrix[7] * value[1] + matrix[11] * value[2] + matrix[15] * value[3],
  };
}

inline std::array<float, 3> transformNormal(const std::array<float, 16> &worldToModel, const std::array<float, 3> &localNormal)
{
  return normalized({
    worldToModel[0] * localNormal[0] + worldToModel[1] * localNormal[1] + worldToModel[2] * localNormal[2],
    worldToModel[4] * localNormal[0] + worldToModel[5] * localNormal[1] + worldToModel[6] * localNormal[2],
    worldToModel[8] * localNormal[0] + worldToModel[9] * localNormal[1] + worldToModel[10] * localNormal[2],
  });
}

inline float srgbChannelToLinear(const float value)
{
  return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline float halfToFloat(const uint16_t half)
{
  const uint32_t sign = static_cast<uint32_t>(half) >> 15u;
  const uint32_t exponent = (static_cast<uint32_t>(half) >> 10u) & 0x1fu;
  const uint32_t mantissa = static_cast<uint32_t>(half) & 0x3ffu;
  if (exponent == 0u)
  {
    if (mantissa == 0u)
      return sign == 0u ? 0.0f : -0.0f;
    const float value = std::ldexp(static_cast<float>(mantissa), -24);
    return sign == 0u ? value : -value;
  }
  uint32_t bits = (sign << 31u) | ((exponent == 0x1fu ? 0xffu : exponent + 112u) << 23u) | (mantissa << 13u);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline std::vector<SparseVoxelGBufferPixelGPU> writeVoxelGBuffer(const GBufferFixture &fixture)
{
  const uint32_t width = fixture.uniforms.viewportTiles[0];
  const uint32_t height = fixture.uniforms.viewportTiles[1];
  std::vector<SparseVoxelGBufferPixelGPU> result(static_cast<size_t>(width) * height);
  for (SparseVoxelGBufferPixelGPU &pixel : result)
    pixel.motionDepth[2] = fixture.uniforms.gbufferClearDepth;
  for (uint32_t y = 0u; y < height; ++y)
  {
    for (uint32_t x = 0u; x < width; ++x)
    {
      const uint32_t pixelIndex = y * width + x;
      const ResolvedVoxelSurfaceGPU &surface = fixture.surfaces.at(pixelIndex);
      if ((surface.hitFlags & kSparseVoxelSurfaceHit) == 0u)
        continue;
      const SparseVoxelInstanceGPU &instance = fixture.instances.at(surface.instanceIndex);
      const SparseVoxelMaterialGPU &material = fixture.materials.at(surface.voxelIndex);
      const Ray ray = cameraRay(fixture.uniforms, x, y);
      const std::array<float, 3> worldPosition = {
        ray.origin[0] + surface.surfaceT * ray.direction[0],
        ray.origin[1] + surface.surfaceT * ray.direction[1],
        ray.origin[2] + surface.surfaceT * ray.direction[2],
      };
      const std::array<float, 3> worldNormal = transformNormal(instance.worldToModel, unpackLocalFaceNormal(surface.packedLocalFace));
      const std::array<float, 4> currentClip = transformVec4(fixture.uniforms.worldToClip, {worldPosition[0], worldPosition[1], worldPosition[2], 1.0f});
      std::array<float, 2> motion{};
      if ((fixture.uniforms.gbufferFlags & kSparseVoxelGBufferHasPreviousFrame) != 0u && std::abs(currentClip[3]) > 1.0e-6f)
      {
        const std::array<float, 3> localPosition = transformPoint(instance.worldToModel, worldPosition);
        const std::array<float, 3> previousWorldPosition = transformPoint(instance.previousModelToWorld, localPosition);
        const std::array<float, 4> previousClip = transformVec4(
            fixture.uniforms.previousWorldToClip,
            {previousWorldPosition[0], previousWorldPosition[1], previousWorldPosition[2], 1.0f});
        if (std::abs(previousClip[3]) > 1.0e-6f)
        {
          motion = {
            0.5f * (currentClip[0] / currentClip[3] - previousClip[0] / previousClip[3]),
            -0.5f * (currentClip[1] / currentClip[3] - previousClip[1] / previousClip[3]),
          };
        }
      }

      SparseVoxelGBufferPixelGPU &output = result[pixelIndex];
      output.ids = {surface.instanceIndex, surface.voxelIndex, material.materialId, 0u};
      const float scale = 1.0f / 255.0f;
      output.baseColor = {
        srgbChannelToLinear(static_cast<float>(material.albedoRGBA8 & 0xffu) * scale),
        srgbChannelToLinear(static_cast<float>((material.albedoRGBA8 >> 8u) & 0xffu) * scale),
        srgbChannelToLinear(static_cast<float>((material.albedoRGBA8 >> 16u) & 0xffu) * scale),
        static_cast<float>((material.albedoRGBA8 >> 24u) & 0xffu) * scale,
      };
      const float roughness = static_cast<float>(material.roughnessMetallicUNorm8 & 0xffu) * scale;
      const float metallic = static_cast<float>((material.roughnessMetallicUNorm8 >> 8u) & 0xffu) * scale;
      output.materialProperties = {roughness, metallic, output.baseColor[3], static_cast<float>(material.flags)};
      // Authored emissive data remains in the material ABI, but the sparse
      // voxel material pass currently disables emissive output.
      output.emissive = {0.0f, 0.0f, 0.0f, 0.0f};
      output.geometryNormal = {worldNormal[0], worldNormal[1], worldNormal[2], 1.0f};
      output.normal = {worldNormal[0], worldNormal[1], worldNormal[2], roughness};
      output.motionDepth = {motion[0], motion[1], currentClip[2] / currentClip[3], 0.0f};
    }
  }
  return result;
}

} // namespace voxel::svraster::reference
