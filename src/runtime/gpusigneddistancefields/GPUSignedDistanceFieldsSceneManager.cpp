#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneManager.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include "os/Logger.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "time/TimeSpan.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

namespace gpusigneddistancefields
{

namespace
{

using virtualgeometry::AABB;
using virtualgeometry::VirtualGeometryScene;

static GPUSignedDistanceFieldsManager::SurfaceMaterialGPU makeSurfaceMaterialFromDefinition(const virtualgeometry::Material::Definition &definition)
{
  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU material{};
  for (uint32_t channel = 0u; channel < 4u; ++channel)
  {
    material.baseColorOpacity[channel] = definition.baseColorFactor[channel];
  }
  material.baseColorOpacity[3] = std::clamp(definition.baseColorFactor[3] * definition.opacity, 0.0f, 1.0f);
  material.normalRoughness[3] = std::clamp(definition.roughnessFactor, 0.0f, 1.0f);
  material.emissiveMetallic[0] = std::max(0.0f, definition.emissiveFactor[0] * definition.emissiveStrength);
  material.emissiveMetallic[1] = std::max(0.0f, definition.emissiveFactor[1] * definition.emissiveStrength);
  material.emissiveMetallic[2] = std::max(0.0f, definition.emissiveFactor[2] * definition.emissiveStrength);
  material.emissiveMetallic[3] = std::clamp(definition.metallicFactor, 0.0f, 1.0f);
  return material;
}

static GPUSignedDistanceFieldsManager::SurfaceMaterialGPU loadFallbackSurfaceMaterial(
    const virtualgeometry::FallbackMeshData &mesh,
    const std::string &fallbackPath)
{
  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU material{};
  const uint32_t meshMaterialIndex = mesh.sections.empty() ? 0u : mesh.sections.front().materialIndex;
  material.flags[0] = meshMaterialIndex;
  if (meshMaterialIndex >= mesh.materialFiles.size() || mesh.materialFiles[meshMaterialIndex].empty())
  {
    return material;
  }

  std::filesystem::path materialPath(mesh.materialFiles[meshMaterialIndex]);
  if (materialPath.is_relative())
  {
    materialPath = std::filesystem::path(fallbackPath).parent_path() / materialPath;
  }

  virtualgeometry::Material::Definition definition{};
  if (!virtualgeometry::VirtualMaterialFile::loadDefinition(materialPath.string(), definition))
  {
    return material;
  }

  material = makeSurfaceMaterialFromDefinition(definition);
  material.flags[0] = meshMaterialIndex;
  return material;
}

static std::vector<uint32_t> buildFallbackVertexMaterialIds(const virtualgeometry::FallbackMeshData &mesh)
{
  std::vector<uint32_t> materialIds(mesh.vertices.size(), 0u);
  for (const virtualgeometry::FallbackMeshSection &section : mesh.sections)
  {
    const uint32_t triangleBegin = section.indexOffset / 3u;
    const uint32_t triangleEnd = std::min<uint32_t>(
        static_cast<uint32_t>(mesh.indices.size() / 3u),
        (section.indexOffset + section.indexCount + 2u) / 3u);
    for (uint32_t triangleIndex = triangleBegin; triangleIndex < triangleEnd; ++triangleIndex)
    {
      const uint32_t baseIndex = triangleIndex * 3u;
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        const uint32_t vertexIndex = mesh.indices[baseIndex + corner];
        if (vertexIndex < materialIds.size())
          materialIds[vertexIndex] = section.materialIndex;
      }
    }
  }
  return materialIds;
}

// ---------------------------------------------------------------------------
// AABB helpers (identical to SceneBridge)
// ---------------------------------------------------------------------------

static bool isValidAabb(const AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] &&
         bounds.minPoint[1] <= bounds.maxPoint[1] &&
         bounds.minPoint[2] <= bounds.maxPoint[2];
}

static bool intersectsAabb(const AABB &bounds, const float minPoint[3], const float maxPoint[3])
{
  return bounds.maxPoint[0] >= minPoint[0] && bounds.minPoint[0] <= maxPoint[0] &&
         bounds.maxPoint[1] >= minPoint[1] && bounds.minPoint[1] <= maxPoint[1] &&
         bounds.maxPoint[2] >= minPoint[2] && bounds.minPoint[2] <= maxPoint[2];
}

static bool intersectsAabbVec(const AABB &bounds, const math::Vec3f &minP, const math::Vec3f &maxP)
{
  const float mn[3] = {minP[0], minP[1], minP[2]};
  const float mx[3] = {maxP[0], maxP[1], maxP[2]};
  return intersectsAabb(bounds, mn, mx);
}

static bool clipAabb(const AABB &bounds, const float minPoint[3], const float maxPoint[3], AABB &clipped)
{
  if (!intersectsAabb(bounds, minPoint, maxPoint))
  {
    return false;
  }

  clipped = bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    clipped.minPoint[axis] = std::max(bounds.minPoint[axis], minPoint[axis]);
    clipped.maxPoint[axis] = std::min(bounds.maxPoint[axis], maxPoint[axis]);
  }
  return isValidAabb(clipped);
}

static AABB makeAabb(const math::Vec3f &minPoint, const math::Vec3f &maxPoint)
{
  AABB bounds;
  bounds.minPoint[0] = minPoint[0];
  bounds.minPoint[1] = minPoint[1];
  bounds.minPoint[2] = minPoint[2];
  bounds.maxPoint[0] = maxPoint[0];
  bounds.maxPoint[1] = maxPoint[1];
  bounds.maxPoint[2] = maxPoint[2];
  return bounds;
}

static uint32_t clampVoxelMin(float value, float gridMin, float invVoxelSize, uint32_t resolution);
static uint32_t clampVoxelMax(float value, float gridMin, float invVoxelSize, uint32_t resolution);

struct QuantizedJobBox
{
  uint32_t min[3] = {0u, 0u, 0u};
  uint32_t max[3] = {0u, 0u, 0u};
};

static bool isValidQuantizedJobBox(const QuantizedJobBox &box)
{
  return box.max[0] > box.min[0] &&
         box.max[1] > box.min[1] &&
         box.max[2] > box.min[2];
}

static QuantizedJobBox quantizeAabbToCascade(const AABB &bounds, const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade)
{
  QuantizedJobBox box{};
  box.min[0] = clampVoxelMin(bounds.minPoint[0], cascade.gridMin[0], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  box.min[1] = clampVoxelMin(bounds.minPoint[1], cascade.gridMin[1], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  box.min[2] = clampVoxelMin(bounds.minPoint[2], cascade.gridMin[2], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  box.max[0] = clampVoxelMax(bounds.maxPoint[0], cascade.gridMin[0], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  box.max[1] = clampVoxelMax(bounds.maxPoint[1], cascade.gridMin[1], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  box.max[2] = clampVoxelMax(bounds.maxPoint[2], cascade.gridMin[2], cascade.iVoxelSize, GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION);
  return box;
}

template <typename Fn>
static void forEachQuantizedDifferenceRegion(const QuantizedJobBox &outerBox, const QuantizedJobBox *excludedBox, Fn &&fn)
{
  if (!isValidQuantizedJobBox(outerBox))
  {
    return;
  }

  if (excludedBox == nullptr || !isValidQuantizedJobBox(*excludedBox))
  {
    fn(outerBox);
    return;
  }

  QuantizedJobBox clippedExcluded = *excludedBox;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    clippedExcluded.min[axis] = std::max(clippedExcluded.min[axis], outerBox.min[axis]);
    clippedExcluded.max[axis] = std::min(clippedExcluded.max[axis], outerBox.max[axis]);
  }
  if (!isValidQuantizedJobBox(clippedExcluded))
  {
    fn(outerBox);
    return;
  }

  const bool excludedCoversOuter =
      clippedExcluded.min[0] <= outerBox.min[0] &&
      clippedExcluded.min[1] <= outerBox.min[1] &&
      clippedExcluded.min[2] <= outerBox.min[2] &&
      clippedExcluded.max[0] >= outerBox.max[0] &&
      clippedExcluded.max[1] >= outerBox.max[1] &&
      clippedExcluded.max[2] >= outerBox.max[2];
  if (excludedCoversOuter)
  {
    return;
  }

  QuantizedJobBox box = outerBox;
  box.max[0] = std::min(box.max[0], clippedExcluded.min[0]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }

  box = outerBox;
  box.min[0] = std::max(box.min[0], clippedExcluded.max[0]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }

  const uint32_t innerMinX = std::max(outerBox.min[0], clippedExcluded.min[0]);
  const uint32_t innerMaxX = std::min(outerBox.max[0], clippedExcluded.max[0]);
  box = outerBox;
  box.min[0] = innerMinX;
  box.max[0] = innerMaxX;
  box.max[1] = std::min(box.max[1], clippedExcluded.min[1]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }

  box = outerBox;
  box.min[0] = innerMinX;
  box.max[0] = innerMaxX;
  box.min[1] = std::max(box.min[1], clippedExcluded.max[1]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }

  const uint32_t innerMinY = std::max(outerBox.min[1], clippedExcluded.min[1]);
  const uint32_t innerMaxY = std::min(outerBox.max[1], clippedExcluded.max[1]);
  box = outerBox;
  box.min[0] = innerMinX;
  box.max[0] = innerMaxX;
  box.min[1] = innerMinY;
  box.max[1] = innerMaxY;
  box.max[2] = std::min(box.max[2], clippedExcluded.min[2]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }

  box = outerBox;
  box.min[0] = innerMinX;
  box.max[0] = innerMaxX;
  box.min[1] = innerMinY;
  box.max[1] = innerMaxY;
  box.min[2] = std::max(box.min[2], clippedExcluded.max[2]);
  if (isValidQuantizedJobBox(box))
  {
    fn(box);
  }
}

static void appendQuantizedCascadeJobBox(
    std::vector<GPUSignedDistanceFieldsManager::JobGPU> &jobs,
    uint32_t instanceIndex,
    uint32_t flags,
    const QuantizedJobBox &box)
{
  if (!isValidQuantizedJobBox(box))
  {
    return;
  }

  GPUSignedDistanceFieldsManager::JobGPU job{};
  job.instanceIndex = instanceIndex;
  job.flags = flags;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    job.aabbMin[axis] = box.min[axis];
    job.aabbMax[axis] = box.max[axis];
  }
  jobs.push_back(job);
}

static void appendQuantizedCascadeJob(
    std::vector<GPUSignedDistanceFieldsManager::JobGPU> &jobs,
    uint32_t instanceIndex,
    uint32_t flags,
    const AABB &bounds,
    const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade)
{
  appendQuantizedCascadeJobBox(jobs, instanceIndex, flags, quantizeAabbToCascade(bounds, cascade));
}

static void appendCascadeInvalidateOverlapJob(
    std::vector<GPUSignedDistanceFieldsManager::JobGPU> &jobs,
    uint32_t instanceIndex,
    const AABB &bounds,
    const AABB *excludedBounds,
    const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade)
{
  if (excludedBounds == nullptr)
  {
    return;
  }

  AABB overlapBounds{};
  if (!clipAabb(bounds, excludedBounds->minPoint, excludedBounds->maxPoint, overlapBounds))
  {
    return;
  }

  appendQuantizedCascadeJob(
      jobs,
      instanceIndex,
      GPUSignedDistanceFieldsManager::JobFlag_Invalidate,
      overlapBounds,
      cascade);
}

template <typename Fn>
static void forEachAabbDifferenceRegion(const AABB &outerBounds, const AABB *excludedBounds, Fn &&fn)
{
  if (!isValidAabb(outerBounds))
  {
    return;
  }

  if (excludedBounds == nullptr || !isValidAabb(*excludedBounds))
  {
    fn(outerBounds);
    return;
  }

  AABB clippedExcluded{};
  if (!clipAabb(outerBounds, excludedBounds->minPoint, excludedBounds->maxPoint, clippedExcluded))
  {
    fn(outerBounds);
    return;
  }

  const bool excludedCoversOuter =
      clippedExcluded.minPoint[0] <= outerBounds.minPoint[0] &&
      clippedExcluded.minPoint[1] <= outerBounds.minPoint[1] &&
      clippedExcluded.minPoint[2] <= outerBounds.minPoint[2] &&
      clippedExcluded.maxPoint[0] >= outerBounds.maxPoint[0] &&
      clippedExcluded.maxPoint[1] >= outerBounds.maxPoint[1] &&
      clippedExcluded.maxPoint[2] >= outerBounds.maxPoint[2];
  if (excludedCoversOuter)
  {
    return;
  }

  AABB region = outerBounds;
  region.maxPoint[0] = std::min(region.maxPoint[0], clippedExcluded.minPoint[0]);
  if (isValidAabb(region))
  {
    fn(region);
  }

  region = outerBounds;
  region.minPoint[0] = std::max(region.minPoint[0], clippedExcluded.maxPoint[0]);
  if (isValidAabb(region))
  {
    fn(region);
  }

  const float innerMinX = std::max(outerBounds.minPoint[0], clippedExcluded.minPoint[0]);
  const float innerMaxX = std::min(outerBounds.maxPoint[0], clippedExcluded.maxPoint[0]);
  region = outerBounds;
  region.minPoint[0] = innerMinX;
  region.maxPoint[0] = innerMaxX;
  region.maxPoint[1] = std::min(region.maxPoint[1], clippedExcluded.minPoint[1]);
  if (isValidAabb(region))
  {
    fn(region);
  }

  region = outerBounds;
  region.minPoint[0] = innerMinX;
  region.maxPoint[0] = innerMaxX;
  region.minPoint[1] = std::max(region.minPoint[1], clippedExcluded.maxPoint[1]);
  if (isValidAabb(region))
  {
    fn(region);
  }

  const float innerMinY = std::max(outerBounds.minPoint[1], clippedExcluded.minPoint[1]);
  const float innerMaxY = std::min(outerBounds.maxPoint[1], clippedExcluded.maxPoint[1]);
  region = outerBounds;
  region.minPoint[0] = innerMinX;
  region.maxPoint[0] = innerMaxX;
  region.minPoint[1] = innerMinY;
  region.maxPoint[1] = innerMaxY;
  region.maxPoint[2] = std::min(region.maxPoint[2], clippedExcluded.minPoint[2]);
  if (isValidAabb(region))
  {
    fn(region);
  }

  region = outerBounds;
  region.minPoint[0] = innerMinX;
  region.maxPoint[0] = innerMaxX;
  region.minPoint[1] = innerMinY;
  region.maxPoint[1] = innerMaxY;
  region.minPoint[2] = std::max(region.minPoint[2], clippedExcluded.maxPoint[2]);
  if (isValidAabb(region))
  {
    fn(region);
  }
}

static AABB expandAabb(const AABB &bounds, float amount)
{
  AABB expanded = bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    expanded.minPoint[axis] -= amount;
    expanded.maxPoint[axis] += amount;
  }
  return expanded;
}

static AABB makeFallbackAabb(const virtualgeometry::FallbackMeshData &mesh)
{
  AABB bounds;
  bounds.minPoint[0] = mesh.boundsMin[0];
  bounds.minPoint[1] = mesh.boundsMin[1];
  bounds.minPoint[2] = mesh.boundsMin[2];
  bounds.maxPoint[0] = mesh.boundsMax[0];
  bounds.maxPoint[1] = mesh.boundsMax[1];
  bounds.maxPoint[2] = mesh.boundsMax[2];
  return bounds;
}

static AABB transformAabb(const AABB &localBounds, const math::Mat4f &transform)
{
  AABB transformed;
  for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
  {
    const math::Vec3f localCorner(
        (cornerIndex & 1u) ? localBounds.maxPoint[0] : localBounds.minPoint[0],
        (cornerIndex & 2u) ? localBounds.maxPoint[1] : localBounds.minPoint[1],
        (cornerIndex & 4u) ? localBounds.maxPoint[2] : localBounds.minPoint[2]);
    const math::Vec4f worldCorner = transform * math::Vec4f(localCorner[0], localCorner[1], localCorner[2], 1.0f);
    const float point[3] = {worldCorner[0], worldCorner[1], worldCorner[2]};
    transformed.expandBy(point);
  }
  return transformed;
}

static bool matricesEqual(const math::Mat4f &a, const math::Mat4f &b, float epsilon = 1.0e-5f)
{
  for (size_t i = 0u; i < 16u; ++i)
  {
    if (std::abs(a.data[i] - b.data[i]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

static bool palettesEqual(const std::vector<math::Mat4f> &a, const std::vector<math::Mat4f> &b, float epsilon = 1.0e-5f)
{
  if (a.size() != b.size())
  {
    return false;
  }
  for (size_t i = 0u; i < a.size(); ++i)
  {
    if (!matricesEqual(a[i], b[i], epsilon))
    {
      return false;
    }
  }
  return true;
}

// Compute a conservative world-space AABB from per-bone capsule data stored
// in the skeleton.  For each bone with a valid capsule (capsuleRadius > 0):
//   1. Transform both capsule endpoints by (modelMatrix * palette[i]).
//   2. Estimate the world-space radius by multiplying the local radius by the
//      maximum column-length of that transform (a conservative scale factor).
//   3. Expand the result AABB by spheres at the two world-space endpoints.
//
// Bones with capsuleRadius == 0 are skipped (v1 files or bones with no
// influenced vertices).  Returns an invalid AABB when no bone contributes.
//
// Complexity: O(bones * 2).
static AABB computeSkinnedWorldBoundsFromCapsules(
    const rendering::animation::Skeleton &skeleton,
    const math::Mat4f &modelMatrix,
    const std::vector<math::Mat4f> &palette)
{
  AABB result;
  const uint32_t boneCount = std::min(
      skeleton.getBoneCount(),
      static_cast<uint32_t>(palette.size()));

  for (uint32_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
  {
    const rendering::animation::Skeleton::Bone &bone = skeleton.getBone(boneIndex);
    if (bone.capsuleRadius <= 0.0f)
    {
      continue;
    }

    const math::Mat4f boneToWorld = modelMatrix * palette[boneIndex];

    // Estimate world-space scale: max length of the three column vectors.
    float maxColLen = 0.0f;
    for (uint32_t col = 0u; col < 3u; ++col)
    {
      const float cx = boneToWorld.data[col * 4u + 0u];
      const float cy = boneToWorld.data[col * 4u + 1u];
      const float cz = boneToWorld.data[col * 4u + 2u];
      maxColLen = std::max(maxColLen, std::sqrt(cx * cx + cy * cy + cz * cz));
    }
    const float worldRadius = bone.capsuleRadius * maxColLen;

    // Transform capsule endpoints into world space.
    const auto transformPoint = [&boneToWorld](const math::Vec3f &p) -> math::Vec3f
    {
      const math::Vec4f wp = boneToWorld * math::Vec4f(p[0], p[1], p[2], 1.0f);
      return math::Vec3f(wp[0], wp[1], wp[2]);
    };

    const math::Vec3f wsStart = transformPoint(bone.capsuleStart);
    const math::Vec3f wsEnd   = transformPoint(bone.capsuleEnd);

    // Expand by sphere at each endpoint.
    for (const math::Vec3f &centre : {wsStart, wsEnd})
    {
      const float pt[3] = {centre[0] - worldRadius, centre[1] - worldRadius, centre[2] - worldRadius};
      const float ptMax[3] = {centre[0] + worldRadius, centre[1] + worldRadius, centre[2] + worldRadius};
      result.expandBy(pt);
      result.expandBy(ptMax);
    }
  }
  return result;
}


static uint32_t clampVoxelMin(float value, float gridMin, float invVoxelSize, uint32_t resolution)
{
  const int32_t quantized = static_cast<int32_t>(std::floor((value - gridMin) * invVoxelSize));
  return static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(quantized, static_cast<int32_t>(resolution))));
}

static uint32_t clampVoxelMax(float value, float gridMin, float invVoxelSize, uint32_t resolution)
{
  const int32_t quantized = static_cast<int32_t>(std::ceil((value - gridMin) * invVoxelSize));
  return static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(quantized, static_cast<int32_t>(resolution))));
}

// Fallback LOD candidate search (same logic as SceneBridge).
static std::string buildShapeCacheKey(const std::string &path, std::optional<uint32_t> shapeIndex)
{
  if (!shapeIndex.has_value())
  {
    return path;
  }
  return path + "::shape" + std::to_string(*shapeIndex);
}

static std::filesystem::path buildFallbackMeshPath(const std::string &vgPath, uint32_t lodLevel, std::optional<uint32_t> shapeIndex = std::nullopt)
{
  const std::filesystem::path path = std::filesystem::path(vgPath).lexically_normal();
  const std::string shapeSuffix = shapeIndex.has_value() ? ".shape" + std::to_string(*shapeIndex) : "";
  return path.parent_path() / (path.stem().string() + shapeSuffix + ".lod" + std::to_string(lodLevel) + ".fallback.mesh");
}

static std::filesystem::path buildSoftwareRaytraceScenePath(const std::string &vgPath)
{
  const std::filesystem::path path = std::filesystem::path(vgPath).lexically_normal();
  return path.parent_path() / (path.stem().string() + ".softwareraytrace");
}

static bool isSoftwareRaytraceScenePath(const std::filesystem::path &path)
{
  return path.extension() == ".softwareraytrace";
}

static bool isShapeSpecificFallbackPath(const std::filesystem::path &path, std::optional<uint32_t> shapeIndex)
{
  return shapeIndex.has_value() && path.filename().string().find(".shape" + std::to_string(*shapeIndex) + ".lod") != std::string::npos;
}

static std::vector<std::filesystem::path> buildFallbackMeshCandidates(const std::string &vgPath, uint32_t requestedLod, std::optional<uint32_t> shapeIndex)
{
  static constexpr uint32_t kMaxFallbackLodSearch = 15u;
  std::vector<std::filesystem::path> candidates;
  candidates.reserve((kMaxFallbackLodSearch + 1u) * 2u + 1u);

  const uint32_t clampedLod = std::min(requestedLod, kMaxFallbackLodSearch);
  candidates.push_back(buildSoftwareRaytraceScenePath(vgPath));
  if (shapeIndex.has_value())
  {
    candidates.push_back(buildFallbackMeshPath(vgPath, clampedLod, shapeIndex));
  }
  candidates.push_back(buildFallbackMeshPath(vgPath, clampedLod));
  for (uint32_t lod = clampedLod + 1u; lod <= kMaxFallbackLodSearch; ++lod)
  {
    if (shapeIndex.has_value())
    {
      candidates.push_back(buildFallbackMeshPath(vgPath, lod, shapeIndex));
    }
    candidates.push_back(buildFallbackMeshPath(vgPath, lod));
  }
  for (uint32_t lod = clampedLod; lod > 0u; --lod)
  {
    if (shapeIndex.has_value())
    {
      candidates.push_back(buildFallbackMeshPath(vgPath, lod - 1u, shapeIndex));
    }
    candidates.push_back(buildFallbackMeshPath(vgPath, lod - 1u));
  }
  return candidates;
}

static virtualgeometry::FallbackMeshData mergeFallbackMeshes(const std::vector<virtualgeometry::FallbackMeshData> &meshes)
{
  virtualgeometry::FallbackMeshData merged{};
  if (meshes.empty())
  {
    return merged;
  }

  merged = meshes.front();
  if (meshes.size() == 1u)
  {
    return merged;
  }

  merged.vertices.clear();
  merged.indices.clear();
  merged.sections.clear();
  merged.triangleSurfaceHints.clear();
  merged.materialFiles.clear();

  merged.boundsMin[0] = std::numeric_limits<float>::max();
  merged.boundsMin[1] = std::numeric_limits<float>::max();
  merged.boundsMin[2] = std::numeric_limits<float>::max();
  merged.boundsMax[0] = std::numeric_limits<float>::lowest();
  merged.boundsMax[1] = std::numeric_limits<float>::lowest();
  merged.boundsMax[2] = std::numeric_limits<float>::lowest();

  for (const virtualgeometry::FallbackMeshData &mesh : meshes)
  {
    const uint32_t vertexOffset = static_cast<uint32_t>(merged.vertices.size());
    const uint32_t indexOffset = static_cast<uint32_t>(merged.indices.size());
    merged.vertices.insert(merged.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    merged.indices.reserve(merged.indices.size() + mesh.indices.size());
    for (const uint32_t index : mesh.indices)
    {
      merged.indices.push_back(index + vertexOffset);
    }
    merged.triangleSurfaceHints.insert(merged.triangleSurfaceHints.end(), mesh.triangleSurfaceHints.begin(), mesh.triangleSurfaceHints.end());

    if (!mesh.sections.empty())
    {
      for (const virtualgeometry::FallbackMeshSection &section : mesh.sections)
      {
        virtualgeometry::FallbackMeshSection mergedSection = section;
        mergedSection.indexOffset += indexOffset;
        merged.sections.push_back(mergedSection);
      }
    }
    else if (!mesh.indices.empty())
    {
      merged.sections.push_back(virtualgeometry::FallbackMeshSection{
          .indexOffset = indexOffset,
          .indexCount = static_cast<uint32_t>(mesh.indices.size()),
          .materialIndex = 0u,
      });
    }

    if (mesh.materialFiles.size() > merged.materialFiles.size())
    {
      merged.materialFiles.resize(mesh.materialFiles.size());
    }
    for (size_t materialIndex = 0u; materialIndex < mesh.materialFiles.size(); ++materialIndex)
    {
      if (merged.materialFiles[materialIndex].empty())
      {
        merged.materialFiles[materialIndex] = mesh.materialFiles[materialIndex];
      }
    }

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      merged.boundsMin[axis] = std::min(merged.boundsMin[axis], mesh.boundsMin[axis]);
      merged.boundsMax[axis] = std::max(merged.boundsMax[axis], mesh.boundsMax[axis]);
    }
  }

  return merged;
}

static bool loadFallbackMeshCandidate(const std::filesystem::path &path, std::optional<uint32_t> shapeIndex, virtualgeometry::FallbackMeshData &outMesh)
{
  if (!isSoftwareRaytraceScenePath(path))
  {
    if (shapeIndex.has_value() && !isShapeSpecificFallbackPath(path, shapeIndex))
    {
      return virtualgeometry::FallbackMeshFile::loadEntry(path.string(), *shapeIndex, outMesh);
    }
    return virtualgeometry::FallbackMeshFile::load(path.string(), outMesh);
  }

  if (shapeIndex.has_value())
  {
    softwarebvh::SoftwareRayTraceSceneInstanceData entry{};
    if (!softwarebvh::SoftwareRayTraceSceneFile::loadEntryFallbackMeshForShape(path.string(), *shapeIndex, entry))
    {
      return false;
    }
    outMesh = std::move(entry.fallbackMesh);
    return !outMesh.vertices.empty() && !outMesh.indices.empty();
  }

  softwarebvh::SoftwareRayTraceSceneFileHeader header{};
  if (!softwarebvh::SoftwareRayTraceSceneFile::loadHeader(path.string(), header) || header.entries.empty())
  {
    return false;
  }

  std::vector<virtualgeometry::FallbackMeshData> meshes;
  meshes.reserve(header.entries.size());
  for (uint32_t entryIndex = 0u; entryIndex < static_cast<uint32_t>(header.entries.size()); ++entryIndex)
  {
    softwarebvh::SoftwareRayTraceSceneInstanceData entry{};
    if (softwarebvh::SoftwareRayTraceSceneFile::loadEntryFallbackMesh(path.string(), entryIndex, entry) &&
        !entry.fallbackMesh.vertices.empty() &&
        !entry.fallbackMesh.indices.empty())
    {
      meshes.push_back(std::move(entry.fallbackMesh));
    }
  }
  if (meshes.empty())
  {
    return false;
  }

  outMesh = mergeFallbackMeshes(meshes);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / settings
// ---------------------------------------------------------------------------

GPUSignedDistanceFieldsSceneManager::GPUSignedDistanceFieldsSceneManager(GPUSignedDistanceFieldsManager &manager)
    : GPUSignedDistanceFieldsSceneManager(manager, Settings{})
{
}

GPUSignedDistanceFieldsSceneManager::GPUSignedDistanceFieldsSceneManager(GPUSignedDistanceFieldsManager &manager, Settings settings)
    : manager_(manager), settings_(settings)
{
}

void GPUSignedDistanceFieldsSceneManager::setSettings(Settings settings)
{
  if (settings.fallbackLod != settings_.fallbackLod)
  {
    // LOD change invalidates the entire mesh registry and all instance state.
    instances_.clear();
    meshCache_.clear();
    candidateToCacheKey_.clear();
    meshTable_.clear();
    surfaceMaterials_.clear();
    vertices_.clear();
    indices_.clear();
    boneWeights_.clear();
    meshTableDirty_ = false;

    staticFreeSlots_.clear();
    nextStaticSlot_ = 0u;
    staticActiveCount_ = 0u;

    skinnedFreeSlots_.clear();
    nextSkinnedSlot_ = 0u;
    skinnedActiveCount_ = 0u;

    nextBoneMatrixSlot_ = 0u;

    pendingUpdateQueue_.clear();
    queuedInstances_.clear();
  }
  settings_ = settings;
}

bool GPUSignedDistanceFieldsSceneManager::isQueued(virtualgeometry::InstanceId instanceId) const
{
  for (const SceneInstanceKey key : queuedInstances_)
  {
    if (sceneInstanceKeyObjectId(key) == instanceId)
    {
      return true;
    }
  }
  return false;
}

bool GPUSignedDistanceFieldsSceneManager::hasCompletedInitialBuild(virtualgeometry::InstanceId instanceId) const
{
  bool found = false;
  for (const auto &entry : instances_)
  {
    if (sceneInstanceKeyObjectId(entry.first) != instanceId)
    {
      continue;
    }
    found = true;
    if (!entry.second.hasCompletedInitialBuild)
    {
      return false;
    }
  }
  return found;
}

GPUSignedDistanceFieldsSceneManager::SceneInstanceKey
GPUSignedDistanceFieldsSceneManager::makeSceneInstanceKey(virtualgeometry::InstanceId instanceId, uint32_t shapeIndex)
{
  return (static_cast<SceneInstanceKey>(instanceId) << 32u) | static_cast<SceneInstanceKey>(shapeIndex);
}

virtualgeometry::InstanceId GPUSignedDistanceFieldsSceneManager::sceneInstanceKeyObjectId(SceneInstanceKey key)
{
  return static_cast<virtualgeometry::InstanceId>(key >> 32u);
}

uint32_t GPUSignedDistanceFieldsSceneManager::sceneInstanceKeyShapeIndex(SceneInstanceKey key)
{
  return static_cast<uint32_t>(key & 0xffffffffull);
}

// ---------------------------------------------------------------------------
// Slot allocation
// ---------------------------------------------------------------------------

uint32_t GPUSignedDistanceFieldsSceneManager::allocateGpuSlot(GPUSignedDistanceFieldsManager::Path path)
{
  if (path == GPUSignedDistanceFieldsManager::Path::Static)
  {
    if (!staticFreeSlots_.empty())
    {
      const uint32_t slot = staticFreeSlots_.back();
      staticFreeSlots_.pop_back();
      return slot;
    }
    return nextStaticSlot_++;
  }
  else
  {
    if (!skinnedFreeSlots_.empty())
    {
      const uint32_t slot = skinnedFreeSlots_.back();
      skinnedFreeSlots_.pop_back();
      return slot;
    }
    return nextSkinnedSlot_++;
  }
}

void GPUSignedDistanceFieldsSceneManager::freeGpuSlot(GPUSignedDistanceFieldsManager::Path path, uint32_t slot)
{
  // Zero the GPU slot so stale data is not used by any debug passes.
  const GPUSignedDistanceFieldsManager::InstanceGPU zero{};
  manager_.uploadInstanceSlice(path, slot, zero);

  if (path == GPUSignedDistanceFieldsManager::Path::Static)
  {
    staticFreeSlots_.push_back(slot);
  }
  else
  {
    skinnedFreeSlots_.push_back(slot);
  }
}

uint32_t GPUSignedDistanceFieldsSceneManager::allocateBoneMatrixSlot(uint32_t matrixCount)
{
  const uint32_t slot = nextBoneMatrixSlot_;
  nextBoneMatrixSlot_ += matrixCount;
  return slot;
}

// ---------------------------------------------------------------------------
// Mesh loading (registration-time, cached permanently)
// ---------------------------------------------------------------------------

GPUSignedDistanceFieldsSceneManager::CachedFallbackMesh *
GPUSignedDistanceFieldsSceneManager::getOrLoadMesh(const std::string &vgPath, std::optional<uint32_t> shapeIndex)
{
  const std::string requestKey = buildShapeCacheKey(vgPath, shapeIndex);
  // Check if we already resolved this vgPath to a cache key.
  {
    const auto keyIt = candidateToCacheKey_.find(requestKey);
    if (keyIt != candidateToCacheKey_.end())
    {
      const auto cacheIt = meshCache_.find(keyIt->second);
      if (cacheIt != meshCache_.end())
      {
        return &cacheIt->second;
      }
    }
  }

  // Search for a valid LOD file among candidates.
  const std::vector<std::filesystem::path> candidates = buildFallbackMeshCandidates(vgPath, settings_.fallbackLod, shapeIndex);

  // First, check if any candidate is already in the mesh cache.
  for (const std::filesystem::path &candidate : candidates)
  {
    const std::string candidateKey = buildShapeCacheKey(candidate.string(), shapeIndex);
    const auto cacheIt = meshCache_.find(candidateKey);
    if (cacheIt != meshCache_.end() && cacheIt->second.available)
    {
      candidateToCacheKey_[requestKey] = candidateKey;
      return &cacheIt->second;
    }
  }

  // Find the first existing file on disk.
  std::filesystem::path selectedPath;
  for (const std::filesystem::path &candidate : candidates)
  {
    if (std::filesystem::exists(candidate))
    {
      selectedPath = candidate;
      break;
    }
  }
  if (selectedPath.empty())
  {
    selectedPath = candidates.front();
  }

  const std::string cacheKey = buildShapeCacheKey(selectedPath.string(), shapeIndex);
  candidateToCacheKey_[requestKey] = cacheKey;

  // Already attempted (failed) load.
  {
    const auto cacheIt = meshCache_.find(cacheKey);
    if (cacheIt != meshCache_.end())
    {
      return &cacheIt->second;
    }
  }

  // Load from disk and register geometry into shared GPU buffers.
  CachedFallbackMesh cached{};
  cached.fallbackPath = selectedPath.string();

  if (!loadFallbackMeshCandidate(selectedPath, shapeIndex, cached.mesh))
  {
    diagnostics_.lastMissingFallbackPath = cached.fallbackPath;
    os::Logger::warningf(
        "[GPUSignedDistanceFields] Missing fallback mesh for '%s' shape=%u near requested lod=%u; searched first path '%s'",
        vgPath.c_str(),
        shapeIndex.value_or(0u),
        settings_.fallbackLod,
        candidates.front().string().c_str());
    cached.available = false;
    auto &entry = meshCache_.emplace(cacheKey, std::move(cached)).first->second;
    return &entry;
  }

  cached.available = true;
  diagnostics_.lastLoadedFallbackPath = cached.fallbackPath;

  if (selectedPath != candidates.front())
  {
    os::Logger::logf(
        "[GPUSignedDistanceFields] Requested fallback lod=%u for '%s' but using '%s'",
        settings_.fallbackLod,
        vgPath.c_str(),
        cached.fallbackPath.c_str());
  }

  if ((cached.mesh.indices.size() % 3u) != 0u)
  {
    throw std::runtime_error("Fallback mesh index buffer must contain complete triangles: " + cacheKey);
  }

  const bool isSkinnedMesh = cached.mesh.skeleton.getBoneCount() > 0u;

  const uint32_t vertexOffset = static_cast<uint32_t>(vertices_.size());
  const uint32_t indexOffset = static_cast<uint32_t>(indices_.size());
  const uint32_t meshIndex = static_cast<uint32_t>(meshTable_.size());
  const std::vector<uint32_t> vertexMaterialIds = buildFallbackVertexMaterialIds(cached.mesh);

  for (uint32_t vertexIndex = 0u; vertexIndex < cached.mesh.vertices.size(); ++vertexIndex)
  {
    const virtualgeometry::FallbackMeshVertex &vertex = cached.mesh.vertices[vertexIndex];
    GPUSignedDistanceFieldsManager::VertexGPU gpuVertex{};
    gpuVertex.position = math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]);
    gpuVertex.boneWeightOffset = static_cast<uint32_t>(boneWeights_.size());
    gpuVertex.uvMaterial[0] = vertex.uv[0];
    gpuVertex.uvMaterial[1] = vertex.uv[1];
    for (uint32_t channel = 0u; channel < 4u; ++channel)
    {
      gpuVertex.surfaceBaseColorOpacity[channel] = vertex.surfaceBaseColorOpacity[channel];
      gpuVertex.surfaceNormalRoughness[channel] = vertex.surfaceNormalRoughness[channel];
      gpuVertex.surfaceEmissiveMetallic[channel] = vertex.surfaceEmissiveMetallic[channel];
      gpuVertex.surfaceFlags[channel] = vertex.surfaceFlags[channel];
    }
    gpuVertex.surfaceFlags[0] = vertexIndex < vertexMaterialIds.size() ? vertexMaterialIds[vertexIndex] : 0u;
    vertices_.push_back(gpuVertex);

    if (isSkinnedMesh)
    {
      for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
      {
        GPUSignedDistanceFieldsManager::BoneWeightGPU gpuBoneWeight{};
        gpuBoneWeight.weight = vertex.boneWeights[influenceIndex];
        gpuBoneWeight.boneIndex = vertex.boneIndices[influenceIndex];
        boneWeights_.push_back(gpuBoneWeight);
      }
    }
  }

  indices_.insert(indices_.end(), cached.mesh.indices.begin(), cached.mesh.indices.end());

  GPUSignedDistanceFieldsManager::MeshTableEntryGPU meshEntry{};
  meshEntry.vertexOffset = vertexOffset;
  meshEntry.vertexCount = static_cast<uint32_t>(cached.mesh.vertices.size());
  meshEntry.indexOffset = indexOffset;
  meshEntry.triangleCount = static_cast<uint32_t>(cached.mesh.indices.size() / 3u);
  meshEntry.boneWeightsPerVertex = isSkinnedMesh ? 4u : 0u;
  meshEntry.flags = isSkinnedMesh ? GPUSignedDistanceFieldsManager::MeshFlag_Skinned : GPUSignedDistanceFieldsManager::MeshFlag_None;
  meshEntry.surfaceMaterialIndex = static_cast<uint32_t>(surfaceMaterials_.size());
  meshTable_.push_back(meshEntry);
  surfaceMaterials_.push_back(loadFallbackSurfaceMaterial(cached.mesh, cached.fallbackPath));

  cached.meshIndex = meshIndex;

  // Check whether the skeleton has usable capsule data (v2 files).
  if (isSkinnedMesh)
  {
    const uint32_t boneCount = cached.mesh.skeleton.getBoneCount();
    for (uint32_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
    {
      if (cached.mesh.skeleton.getBone(boneIndex).capsuleRadius > 0.0f)
      {
        cached.hasCapsuleData = true;
        break;
      }
    }
    if (!cached.hasCapsuleData)
    {
      os::Logger::warningf(
          "[GPUSignedDistanceFields] Skinned fallback mesh '%s' has no capsule data (v1 file?); instance will be excluded from SDF jobs.",
          cacheKey.c_str());
    }
  }

  meshTableDirty_ = true;

  os::Logger::logf(
      "[GPUSignedDistanceFields] Registered fallback mesh '%s' (lod=%u vertices=%zu triangles=%zu bones=%u meshIndex=%u)",
      requestKey.c_str(),
      cached.mesh.lodLevel,
      cached.mesh.vertices.size(),
      cached.mesh.indices.size() / 3u,
      cached.mesh.skeleton.getBoneCount(),
      meshIndex);

  auto &entry = meshCache_.emplace(cacheKey, std::move(cached)).first->second;
  return &entry;
}

// ---------------------------------------------------------------------------
// Exposed slab computation (cascade scroll optimization)
//
// When a cascade scrolls by (dX, dY, dZ) voxels, the newly visible region
// is up to three axis-aligned slabs at the leading edges.  Only instances
// that overlap at least one slab need to submit a scroll refresh job.
// ---------------------------------------------------------------------------

GPUSignedDistanceFieldsSceneManager::ExposedSlabs
GPUSignedDistanceFieldsSceneManager::computeExposedSlabs(const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade)
{
  ExposedSlabs result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int32_t delta = cascade.clipmapInvalidationOffset[axis];
    if (delta == 0)
    {
      continue;
    }
    result.active[axis] = true;
    const float voxelSize = cascade.voxelSize;
    const float gridMin = cascade.gridMin[axis];
    const float gridMax = cascade.gridMax[axis];
    const float slabThickness = static_cast<float>(std::abs(delta)) * voxelSize;

    // Fill full range for non-scrolled axes; scrolled axis gets the thin slab.
    for (uint32_t a2 = 0u; a2 < 3u; ++a2)
    {
      result.slabs[axis].min[a2] = cascade.gridMin[a2];
      result.slabs[axis].max[a2] = cascade.gridMax[a2];
    }

    if (delta > 0)
    {
      // Camera moved in +axis: new voxels appeared at the high end.
      result.slabs[axis].min[axis] = gridMax - slabThickness;
      result.slabs[axis].max[axis] = gridMax;
    }
    else
    {
      // Camera moved in -axis: new voxels appeared at the low end.
      result.slabs[axis].min[axis] = gridMin;
      result.slabs[axis].max[axis] = gridMin + slabThickness;
    }
  }
  return result;
}

bool GPUSignedDistanceFieldsSceneManager::instanceOverlapsExposedSlabs(
    const AABB &bounds,
    const ExposedSlabs &slabs)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!slabs.active[axis])
    {
      continue;
    }
    if (intersectsAabb(bounds, slabs.slabs[axis].min, slabs.slabs[axis].max))
    {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Job emission
// ---------------------------------------------------------------------------

void GPUSignedDistanceFieldsSceneManager::appendJobsForInstance(
    const InstanceState &state,
    const AABB &jobBounds,
    bool needsInvalidate,
    std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> &staticJobs,
    std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> &skinnedJobs) const
{
  auto &pathJobs = state.hasSkinning ? skinnedJobs : staticJobs;

  for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
  {
    const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade = manager_.getCascadeInfo(cascadeIndex);
    const AABB cascadeBounds = makeAabb(cascade.gridMin, cascade.gridMax);

    const AABB inflatedJobBounds = expandAabb(jobBounds, cascade.voxelSize);
    AABB clippedCascadeBounds{};
    if (!clipAabb(inflatedJobBounds, cascadeBounds.minPoint, cascadeBounds.maxPoint, clippedCascadeBounds))
    {
      continue;
    }

    appendQuantizedCascadeJob(
        pathJobs[cascadeIndex],
        state.gpuSlot,
        needsInvalidate ? GPUSignedDistanceFieldsManager::JobFlag_Invalidate : GPUSignedDistanceFieldsManager::JobFlag_None,
        clippedCascadeBounds,
        cascade);
  }
}

// ---------------------------------------------------------------------------
// Main sync
// ---------------------------------------------------------------------------

void GPUSignedDistanceFieldsSceneManager::sync(VirtualGeometryScene &scene, const math::Vec3f &clipmapCenter)
{
  const lib::time::TimeSpan syncStart = lib::time::TimeSpan::now();
  const auto measurePhase = [](double &accumulator, auto &&fn)
  {
    const lib::time::TimeSpan start = lib::time::TimeSpan::now();
    fn();
    accumulator += (lib::time::TimeSpan::now() - start).milliseconds();
  };

  diagnostics_.syncTotalMs = 0.0;
  diagnostics_.syncClipmapUpdateMs = 0.0;
  diagnostics_.syncSceneWalkMs = 0.0;
  diagnostics_.syncMeshLookupMs = 0.0;
  diagnostics_.syncBoundsMs = 0.0;
  diagnostics_.syncInstanceUploadMs = 0.0;
  diagnostics_.syncBoneUploadMs = 0.0;
  diagnostics_.syncEvictMs = 0.0;
  diagnostics_.syncSharedGeometryUploadMs = 0.0;
  diagnostics_.syncRebuildJobsMs = 0.0;
  diagnostics_.syncScrollJobsMs = 0.0;
  diagnostics_.syncJobUploadMs = 0.0;

  measurePhase(diagnostics_.syncClipmapUpdateMs, [&]()
  {
    manager_.updateClipmaps(clipmapCenter);
  });
  lastFrameUpdatedCount_ = 0u;

  // Reset per-frame diagnostics (keep mesh registry counts stable).
  diagnostics_.instanceSlicesUploadedThisFrame = 0u;
  diagnostics_.skinnedBoundsRecomputedThisFrame = 0u;
  diagnostics_.skinnedBoundsCachedThisFrame = 0u;
  diagnostics_.skippedEmptySceneInstanceCount = 0u;
  diagnostics_.skippedMissingRuntimeDataCount = 0u;
  diagnostics_.skippedMissingFallbackMeshCount = 0u;
  diagnostics_.skippedMissingSceneInstanceCount = 0u;
  diagnostics_.skippedInvalidBoundsCount = 0u;
  diagnostics_.staticJobCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.skinnedJobCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.staticJobVoxelCounts.assign(manager_.getCascadeCount(), 0u);
  diagnostics_.skinnedJobVoxelCounts.assign(manager_.getCascadeCount(), 0u);

  // ---------------------------------------------------------------------------
  // Phase 1: Walk the scene — register new instances, update dirty ones.
  //   NOTE: getOrLoadMesh() may load new fallback meshes here and set
  //   meshTableDirty_. The geometry upload (Phase 2) therefore runs AFTER this
  //   phase so that every mesh referenced by jobs emitted in Phase 4 is already
  //   present in the GPU buffers on the same frame it is first needed.
  // ---------------------------------------------------------------------------
  std::unordered_set<SceneInstanceKey> aliveIds;
  aliveIds.reserve(instances_.size());

  measurePhase(diagnostics_.syncSceneWalkMs, [&]()
  {
    for (uint32_t denseIndex = 0u; denseIndex < static_cast<uint32_t>(scene.objectInstances.size()); ++denseIndex)
    {
      if (denseIndex >= static_cast<uint32_t>(scene.objectDenseToSparse.size()))
      {
        continue;
      }

      const virtualgeometry::InstanceId instanceId = scene.objectDenseToSparse[denseIndex];
      if (instanceId == virtualgeometry::INVALID_INSTANCE_ID)
      {
        continue;
      }

      const VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];
      if (record.sceneInstanceIds.empty())
      {
        ++diagnostics_.skippedEmptySceneInstanceCount;
        continue;
      }

      auto runtimeDataIt = scene.files.find(record.objectName);
      if (runtimeDataIt == scene.files.end())
      {
        ++diagnostics_.skippedMissingRuntimeDataCount;
        continue;
      }
      VirtualGeometryScene::VirtualGeometryObjectRuntimeData *runtimeData = runtimeDataIt.value();

      const uint32_t shapeCount = std::max<uint32_t>(1u, static_cast<uint32_t>(runtimeData->shapes.size()));
      for (uint32_t shapeIndex = 0u; shapeIndex < shapeCount; ++shapeIndex)
      {
        const std::optional<uint32_t> optionalShapeIndex = runtimeData->shapes.empty() ? std::nullopt : std::optional<uint32_t>(shapeIndex);
        if (shapeIndex >= static_cast<uint32_t>(record.sceneInstanceIds.size()))
        {
          ++diagnostics_.skippedMissingSceneInstanceCount;
          continue;
        }

        CachedFallbackMesh *cachedMesh = nullptr;
        measurePhase(diagnostics_.syncMeshLookupMs, [&]()
        {
          // Mesh loading happens once per unique path/shape (registration-time).
          cachedMesh = getOrLoadMesh(runtimeData->file.getPath(), optionalShapeIndex);
        });
        if (cachedMesh == nullptr || !cachedMesh->available)
        {
          ++diagnostics_.skippedMissingFallbackMeshCount;
          continue;
        }

        const virtualgeometry::VirtualGeometryInstanceGPUData *sceneInstance =
            scene.instanceManager.getInstance(record.sceneInstanceIds[shapeIndex]);
        if (sceneInstance == nullptr)
        {
          ++diagnostics_.skippedMissingSceneInstanceCount;
          continue;
        }

        const bool hasSkinning =
            cachedMesh->mesh.skeleton.getBoneCount() > 0u &&
            !record.currentMeshPartTransforms.empty();
        const SceneInstanceKey instanceKey = makeSceneInstanceKey(instanceId, shapeIndex);
        aliveIds.insert(instanceKey);

        // ----- Get or create instance state -----
        auto [stateIt, inserted] = instances_.emplace(instanceKey, InstanceState{});
        InstanceState &state = stateIt->second;

        if (inserted)
        {
          state.objectInstanceId = instanceId;
          state.shapeIndex = shapeIndex;
          state.hasSkinning = hasSkinning;
          state.meshIndex = cachedMesh->meshIndex;
          state.boneCount = hasSkinning ? static_cast<uint32_t>(record.currentMeshPartTransforms.size()) : 0u;

          const auto gpuPath = hasSkinning
                                   ? GPUSignedDistanceFieldsManager::Path::Skinned
                                   : GPUSignedDistanceFieldsManager::Path::Static;
          state.gpuSlot = allocateGpuSlot(gpuPath);

          if (hasSkinning && state.boneCount > 0u)
          {
            // Allocate current + previous palette sequentially.
            state.boneMatrixSlot = allocateBoneMatrixSlot(state.boneCount * 2u);
          }

          if (hasSkinning)
          {
            ++skinnedActiveCount_;
          }
          else
          {
            ++staticActiveCount_;
          }
        }

        // ----- Dirty detection -----
        const bool modelChanged =
            !state.hasCachedBounds ||
            !matricesEqual(sceneInstance->modelMatrix, state.cachedModelMatrix);

        const bool bonesChanged =
            hasSkinning && !palettesEqual(record.currentMeshPartTransforms, state.cachedBonePalette);

        // ----- World bounds (recomputed only when needed) -----
        if (modelChanged || bonesChanged)
        {
          AABB newBounds;
          bool validBounds = true;
          measurePhase(diagnostics_.syncBoundsMs, [&]()
          {
            if (hasSkinning)
            {
              if (!cachedMesh->hasCapsuleData)
              {
                // No capsule data (v1 file) — skip; bounds cannot be computed.
                validBounds = false;
                return;
              }
              newBounds = computeSkinnedWorldBoundsFromCapsules(
                  cachedMesh->mesh.skeleton, sceneInstance->modelMatrix, record.currentMeshPartTransforms);
              ++diagnostics_.skinnedBoundsRecomputedThisFrame;
            }
            else
            {
              newBounds = transformAabb(makeFallbackAabb(cachedMesh->mesh), sceneInstance->modelMatrix);
            }
            validBounds = isValidAabb(newBounds);
          });

          if (!validBounds)
          {
            ++diagnostics_.skippedInvalidBoundsCount;
            continue;
          }

          state.cachedWorldBounds = newBounds;
          state.hasCachedBounds = true;

          // ----- Build GPU instance struct -----
          GPUSignedDistanceFieldsManager::InstanceGPU gpuInstance{};
          gpuInstance.modelMatrix = sceneInstance->modelMatrix;
          gpuInstance.previousModelMatrix = sceneInstance->previousModelMatrix;
          gpuInstance.aabbMin = math::Vec3f(newBounds.minPoint[0], newBounds.minPoint[1], newBounds.minPoint[2]);
          gpuInstance.aabbMax = math::Vec3f(newBounds.maxPoint[0], newBounds.maxPoint[1], newBounds.maxPoint[2]);
          gpuInstance.meshIndex = cachedMesh->meshIndex;

          if (hasSkinning)
          {
            gpuInstance.flags = GPUSignedDistanceFieldsManager::InstanceFlag_Skinned;
            gpuInstance.boneMatrixOffset = state.boneMatrixSlot;
            gpuInstance.previousBoneMatrixOffset = state.boneMatrixSlot + state.boneCount;
            gpuInstance.boneCount = state.boneCount;
          }

          const auto gpuPath = hasSkinning
                                   ? GPUSignedDistanceFieldsManager::Path::Skinned
                                   : GPUSignedDistanceFieldsManager::Path::Static;
          measurePhase(diagnostics_.syncInstanceUploadMs, [&]()
          {
            manager_.uploadInstanceSlice(gpuPath, state.gpuSlot, gpuInstance);
          });
          ++diagnostics_.instanceSlicesUploadedThisFrame;

          // Upload bone matrices only when they actually changed.
          if (hasSkinning && bonesChanged && state.boneCount > 0u)
          {
            measurePhase(diagnostics_.syncBoneUploadMs, [&]()
            {
              manager_.uploadBoneMatricesSlice(
                  state.boneMatrixSlot,
                  state.boneCount,
                  record.currentMeshPartTransforms.data());
              manager_.uploadBoneMatricesSlice(
                  state.boneMatrixSlot + state.boneCount,
                  state.boneCount,
                  record.previousMeshPartTransforms.empty()
                      ? record.currentMeshPartTransforms.data()
                      : record.previousMeshPartTransforms.data());
            });

            state.cachedBonePalette = record.currentMeshPartTransforms;
          }

          state.cachedModelMatrix = sceneInstance->modelMatrix;
        }
        else if (hasSkinning)
        {
          ++diagnostics_.skinnedBoundsCachedThisFrame;
        }

        if (!state.hasCachedBounds)
        {
          continue;
        }

        // ----- SDF rebuild queuing -----
        const bool needsInitialBuild = !state.hasCompletedInitialBuild;
        const bool needsDynamicUpdate =
            record.gpuSignedDistanceFields.dynamic &&
            record.gpuSignedDistanceFields.contentRevision > state.lastCompletedContentRevision;
        const bool needsManualRebuild =
            record.gpuSignedDistanceFields.rebuildRevision > state.lastCompletedRebuildRevision;

        if ((needsInitialBuild || needsDynamicUpdate || needsManualRebuild) &&
            !state.queued &&
            queuedInstances_.insert(instanceKey).second)
        {
          state.queued = true;
          pendingUpdateQueue_.push_back(instanceKey);
        }
      }
    }
  });

  // ---------------------------------------------------------------------------
  // Phase 3: Evict instances no longer in the scene.
  // ---------------------------------------------------------------------------
  measurePhase(diagnostics_.syncEvictMs, [&]()
  {
    for (auto it = instances_.begin(); it != instances_.end();)
    {
      if (aliveIds.find(it->first) == aliveIds.end())
      {
        InstanceState &state = it->second;
        const auto gpuPath = state.hasSkinning
                                 ? GPUSignedDistanceFieldsManager::Path::Skinned
                                 : GPUSignedDistanceFieldsManager::Path::Static;

        if (state.gpuSlot != UINT32_MAX)
        {
          freeGpuSlot(gpuPath, state.gpuSlot);
        }

        if (state.hasSkinning)
        {
          skinnedActiveCount_ = skinnedActiveCount_ > 0u ? skinnedActiveCount_ - 1u : 0u;
        }
        else
        {
          staticActiveCount_ = staticActiveCount_ > 0u ? staticActiveCount_ - 1u : 0u;
        }

        queuedInstances_.erase(it->first);
        it = instances_.erase(it);
      }
      else
      {
        ++it;
      }
    }
  });

  manager_.setInstanceCount(GPUSignedDistanceFieldsManager::Path::Static, nextStaticSlot_);
  manager_.setActiveInstanceCount(GPUSignedDistanceFieldsManager::Path::Static, staticActiveCount_);
  manager_.setInstanceCount(GPUSignedDistanceFieldsManager::Path::Skinned, nextSkinnedSlot_);
  manager_.setActiveInstanceCount(GPUSignedDistanceFieldsManager::Path::Skinned, skinnedActiveCount_);

  // ---------------------------------------------------------------------------
  // Phase 2: Upload geometry if new meshes were registered during Phase 1.
  //   This runs after the scene walk so that any mesh loaded by getOrLoadMesh()
  //   this frame is present in the GPU buffers before Phase 4 emits jobs.
  // ---------------------------------------------------------------------------
  measurePhase(diagnostics_.syncSharedGeometryUploadMs, [&]()
  {
    if (meshTableDirty_)
    {
      manager_.uploadMeshTable(meshTable_);
      manager_.uploadSurfaceMaterials(surfaceMaterials_);
      manager_.uploadVertices(vertices_);
      manager_.uploadIndices(indices_);
      manager_.uploadBoneWeights(boneWeights_);
      os::Logger::logf(
          "[GPUSignedDistanceFields] Uploaded shared geometry: meshes=%zu surfaceMaterials=%zu vertices=%zu indices=%zu boneWeights=%zu",
          meshTable_.size(), surfaceMaterials_.size(), vertices_.size(), indices_.size(), boneWeights_.size());
      meshTableDirty_ = false;
    }
  });

  // ---------------------------------------------------------------------------
  // Phase 4: Emit SDF rebuild jobs (bounded by maxMeshUpdatesPerFrame).
  // ---------------------------------------------------------------------------
  std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> staticJobs(manager_.getCascadeCount());
  std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> skinnedJobs(manager_.getCascadeCount());
  std::unordered_set<SceneInstanceKey> updatedThisSync;

  measurePhase(diagnostics_.syncRebuildJobsMs, [&]()
  {
    while (lastFrameUpdatedCount_ < settings_.maxMeshUpdatesPerFrame && !pendingUpdateQueue_.empty())
    {
      const SceneInstanceKey instanceKey = pendingUpdateQueue_.front();
      pendingUpdateQueue_.pop_front();
      queuedInstances_.erase(instanceKey);

      auto stateIt = instances_.find(instanceKey);
      if (stateIt == instances_.end())
      {
        continue;
      }
      InstanceState &state = stateIt->second;
      state.queued = false;

      if (!state.hasCachedBounds)
      {
        continue;
      }

      // Look up scene record to read revision numbers.
      const virtualgeometry::InstanceId instanceId = sceneInstanceKeyObjectId(instanceKey);
      if (instanceId >= static_cast<virtualgeometry::InstanceId>(scene.objectSparseToDense.size()))
      {
        continue;
      }
      const uint32_t denseIndex = scene.objectSparseToDense[instanceId];
      if (denseIndex == virtualgeometry::INVALID_INSTANCE_ID ||
          denseIndex >= static_cast<uint32_t>(scene.objectInstances.size()))
      {
        continue;
      }
      const VirtualGeometryScene::ObjectInstanceRecord &record = scene.objectInstances[denseIndex];

      const bool hasPendingContentUpdate =
          record.gpuSignedDistanceFields.contentRevision > state.lastCompletedContentRevision;
      const bool hasPendingManualRebuild =
          record.gpuSignedDistanceFields.rebuildRevision > state.lastCompletedRebuildRevision;
      const bool needsInitialBuild = !state.hasCompletedInitialBuild;
      const bool needsInvalidate =
          state.hasCompletedInitialBuild &&
          state.hasLastBuiltBounds &&
          isValidAabb(state.lastBuiltBounds) &&
          (hasPendingContentUpdate || hasPendingManualRebuild);

      size_t jobCountBefore = 0u;
      for (uint32_t c = 0u; c < manager_.getCascadeCount(); ++c)
      {
        jobCountBefore += staticJobs[c].size() + skinnedJobs[c].size();
      }

      if (needsInitialBuild)
      {
        appendJobsForInstance(state, state.cachedWorldBounds, true, staticJobs, skinnedJobs);
        appendJobsForInstance(state, state.cachedWorldBounds, false, staticJobs, skinnedJobs);
      }
      else if (needsInvalidate)
      {
        const AABB unionBounds = AABB::Union(state.lastBuiltBounds, state.cachedWorldBounds);
        appendJobsForInstance(state, unionBounds, true, staticJobs, skinnedJobs);
        appendJobsForInstance(state, state.cachedWorldBounds, false, staticJobs, skinnedJobs);
      }
      else
      {
        appendJobsForInstance(state, state.cachedWorldBounds, false, staticJobs, skinnedJobs);
      }

      size_t jobCountAfter = 0u;
      for (uint32_t c = 0u; c < manager_.getCascadeCount(); ++c)
      {
        jobCountAfter += staticJobs[c].size() + skinnedJobs[c].size();
      }

      if (jobCountAfter > jobCountBefore)
      {
        state.hasCompletedInitialBuild = true;
      }
      state.lastCompletedContentRevision = record.gpuSignedDistanceFields.contentRevision;
      state.lastCompletedRebuildRevision = record.gpuSignedDistanceFields.rebuildRevision;
      state.lastBuiltBounds = state.cachedWorldBounds;
      state.hasLastBuiltBounds = true;
      updatedThisSync.insert(instanceKey);
      ++lastFrameUpdatedCount_;
    }
  });

  // ---------------------------------------------------------------------------
  // Phase 5: Cascade scroll — emit refresh jobs only for instances that
  //          intersect the newly exposed slab.
  // ---------------------------------------------------------------------------
  measurePhase(diagnostics_.syncScrollJobsMs, [&]()
  {
    const auto cascadeWasScrolled = [](const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade) -> bool
    {
      return cascade.clipmapInvalidationOffset[0] != 0 ||
             cascade.clipmapInvalidationOffset[1] != 0 ||
             cascade.clipmapInvalidationOffset[2] != 0;
    };

    const auto computeExitedSlabs = [](const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade) -> ExposedSlabs
    {
      ExposedSlabs result{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int32_t delta = cascade.clipmapInvalidationOffset[axis];
        if (delta == 0)
        {
          continue;
        }

        result.active[axis] = true;
        const float voxelSize = cascade.voxelSize;
        const float gridMin = cascade.gridMin[axis];
        const float gridMax = cascade.gridMax[axis];
        const float slabThickness = static_cast<float>(std::abs(delta)) * voxelSize;

        for (uint32_t a2 = 0u; a2 < 3u; ++a2)
        {
          result.slabs[axis].min[a2] = cascade.gridMin[a2];
          result.slabs[axis].max[a2] = cascade.gridMax[a2];
        }

        if (delta > 0)
        {
          // Camera moved in +axis, so the finer cascade vacated the low side.
          result.slabs[axis].min[axis] = gridMin - slabThickness;
          result.slabs[axis].max[axis] = gridMin;
        }
        else
        {
          // Camera moved in -axis, so the finer cascade vacated the high side.
          result.slabs[axis].min[axis] = gridMax;
          result.slabs[axis].max[axis] = gridMax + slabThickness;
        }
      }
      return result;
    };

    const auto appendJobsForSlabs =
        [&](const InstanceState &state,
            uint32_t targetCascadeIndex,
            const AABB &inflatedBounds,
            const GPUSignedDistanceFieldsManager::CascadeInfoGPU &slabCascade,
            const ExposedSlabs &slabs,
            bool trailingSlabs)
    {
      const auto &targetCascade = manager_.getCascadeInfo(targetCascadeIndex);
      const AABB targetCascadeBounds = makeAabb(targetCascade.gridMin, targetCascade.gridMax);
      auto &targetJobs = state.hasSkinning ? skinnedJobs[targetCascadeIndex] : staticJobs[targetCascadeIndex];

      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        if (!slabs.active[axis])
        {
          continue;
        }

        AABB slabBounds{};
        if (!clipAabb(inflatedBounds, slabs.slabs[axis].min, slabs.slabs[axis].max, slabBounds))
        {
          continue;
        }

        // Keep each exposed slab intact. Allowing overlaps between multiple
        // scrolled axes is cheaper than risking under-coverage in the corner
        // regions where two or three slabs meet while the clipmap is moving.
        (void)slabCascade;
        (void)trailingSlabs;

        AABB clippedTargetBounds{};
        if (!clipAabb(slabBounds, targetCascadeBounds.minPoint, targetCascadeBounds.maxPoint, clippedTargetBounds))
        {
          continue;
        }

        appendQuantizedCascadeJob(
            targetJobs,
            state.gpuSlot,
            GPUSignedDistanceFieldsManager::JobFlag_None,
            clippedTargetBounds,
            targetCascade);
      }
    };

    for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
    {
      const GPUSignedDistanceFieldsManager::CascadeInfoGPU &cascade = manager_.getCascadeInfo(cascadeIndex);
      const bool cascadeScrolled = cascadeWasScrolled(cascade);
      const ExposedSlabs enteredSlabs = cascadeScrolled ? computeExposedSlabs(cascade) : ExposedSlabs{};
      const AABB targetCascadeBounds = makeAabb(cascade.gridMin, cascade.gridMax);

      const GPUSignedDistanceFieldsManager::CascadeInfoGPU *finerCascade = nullptr;
      bool finerCascadeScrolled = false;
      ExposedSlabs finerExitedSlabs{};
      if (cascadeIndex > 0u)
      {
        finerCascade = &manager_.getCascadeInfo(cascadeIndex - 1u);
        if (finerCascade->isEnabled != 0u)
        {
          finerCascadeScrolled = cascadeWasScrolled(*finerCascade);
          if (finerCascadeScrolled)
          {
            finerExitedSlabs = computeExitedSlabs(*finerCascade);
          }
        }
      }

      if (!cascadeScrolled && !finerCascadeScrolled)
      {
        continue;
      }

      for (auto &[instanceKey, state] : instances_)
      {
        if (updatedThisSync.count(instanceKey) > 0u)
        {
          // Already submitted a full rebuild job this frame; scroll job is redundant.
          continue;
        }
        if (!state.hasCachedBounds)
        {
          continue;
        }

        const AABB inflated = expandAabb(state.cachedWorldBounds, cascade.voxelSize);
        if (!intersectsAabb(inflated, targetCascadeBounds.minPoint, targetCascadeBounds.maxPoint))
        {
          continue;
        }

        if (cascadeScrolled && instanceOverlapsExposedSlabs(state.cachedWorldBounds, enteredSlabs))
        {
          const size_t jobCountBefore = staticJobs[cascadeIndex].size() + skinnedJobs[cascadeIndex].size();
          appendJobsForSlabs(state, cascadeIndex, inflated, cascade, enteredSlabs, false);
          const size_t jobCountAfter = staticJobs[cascadeIndex].size() + skinnedJobs[cascadeIndex].size();
          if (jobCountAfter > jobCountBefore)
          {
            state.hasCompletedInitialBuild = true;
            state.lastBuiltBounds = state.cachedWorldBounds;
            state.hasLastBuiltBounds = true;
          }
        }

        if (finerCascade != nullptr &&
            finerCascadeScrolled &&
            instanceOverlapsExposedSlabs(state.cachedWorldBounds, finerExitedSlabs))
        {
          // The finer cascade moved away from this region, so the current coarse
          // cascade must backfill it even though it is not entering from the
          // coarse cascade's outer boundary.
          const size_t jobCountBefore = staticJobs[cascadeIndex].size() + skinnedJobs[cascadeIndex].size();
          appendJobsForSlabs(state, cascadeIndex, inflated, *finerCascade, finerExitedSlabs, true);
          const size_t jobCountAfter = staticJobs[cascadeIndex].size() + skinnedJobs[cascadeIndex].size();
          if (jobCountAfter > jobCountBefore)
          {
            state.hasCompletedInitialBuild = true;
            state.lastBuiltBounds = state.cachedWorldBounds;
            state.hasLastBuiltBounds = true;
          }
        }
      }
    }
  });

  // ---------------------------------------------------------------------------
  // Phase 6: Upload job lists.
  // ---------------------------------------------------------------------------
  measurePhase(diagnostics_.syncJobUploadMs, [&]()
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
    {
      manager_.uploadJobs(GPUSignedDistanceFieldsManager::Path::Static, cascadeIndex, staticJobs[cascadeIndex]);
      manager_.uploadJobs(GPUSignedDistanceFieldsManager::Path::Skinned, cascadeIndex, skinnedJobs[cascadeIndex]);

      const auto &staticCascade = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Static).cascades[cascadeIndex];
      const auto &skinnedCascade = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Skinned).cascades[cascadeIndex];
      diagnostics_.staticJobCounts[cascadeIndex] = staticCascade.jobCount;
      diagnostics_.skinnedJobCounts[cascadeIndex] = skinnedCascade.jobCount;
      diagnostics_.staticJobVoxelCounts[cascadeIndex] = staticCascade.jobVoxelCount;
      diagnostics_.skinnedJobVoxelCounts[cascadeIndex] = skinnedCascade.jobVoxelCount;
    }
  });

  // ---------------------------------------------------------------------------
  // Update diagnostics.
  // ---------------------------------------------------------------------------
  diagnostics_.cachedFallbackMeshCount = static_cast<uint32_t>(meshCache_.size());
  diagnostics_.availableFallbackMeshCount = 0u;
  diagnostics_.missingFallbackMeshCount = 0u;
  for (const auto &entry : meshCache_)
  {
    if (entry.second.available)
    {
      ++diagnostics_.availableFallbackMeshCount;
    }
    else
    {
      ++diagnostics_.missingFallbackMeshCount;
    }
  }
  diagnostics_.preparedInstanceCount = static_cast<uint32_t>(aliveIds.size());
  diagnostics_.staticInstanceCount = staticActiveCount_;
  diagnostics_.skinnedInstanceCount = skinnedActiveCount_;
  diagnostics_.uploadedMeshCount = static_cast<uint32_t>(meshTable_.size());
  diagnostics_.uploadedVertexCount = static_cast<uint32_t>(vertices_.size());
  diagnostics_.uploadedIndexCount = static_cast<uint32_t>(indices_.size());
  diagnostics_.uploadedBoneWeightCount = static_cast<uint32_t>(boneWeights_.size());
  diagnostics_.syncTotalMs = (lib::time::TimeSpan::now() - syncStart).milliseconds();
}

} // namespace gpusigneddistancefields
