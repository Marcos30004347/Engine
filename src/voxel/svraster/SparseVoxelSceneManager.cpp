#include "voxel/svraster/SparseVoxelSceneManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <numeric>
#include <utility>

namespace voxel::svraster
{
namespace
{

using Matrix = std::array<float, 16>;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

SparseVoxelAABB emptyBounds()
{
  const float maximum = std::numeric_limits<float>::max();
  return {{maximum, maximum, maximum}, {-maximum, -maximum, -maximum}};
}

bool validBounds(const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) || bounds.minimum[axis] > bounds.maximum[axis])
      return false;
  return true;
}

void expand(SparseVoxelAABB &bounds, const SparseVoxelAABB &other)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minimum[axis] = std::min(bounds.minimum[axis], other.minimum[axis]);
    bounds.maximum[axis] = std::max(bounds.maximum[axis], other.maximum[axis]);
  }
}

bool contains(const SparseVoxelAABB &outer, const SparseVoxelAABB &inner)
{
  constexpr float epsilon = 1.0e-5f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (inner.minimum[axis] < outer.minimum[axis] - epsilon || inner.maximum[axis] > outer.maximum[axis] + epsilon)
      return false;
  return true;
}

SparseVoxelAABB fromGPU(const SparseVoxelTLASNodeGPU &node)
{
  return {{node.worldBoundsMin[0], node.worldBoundsMin[1], node.worldBoundsMin[2]},
          {node.worldBoundsMax[0], node.worldBoundsMax[1], node.worldBoundsMax[2]}};
}

SparseVoxelAABB fromGPU(const SparseVoxelInstanceGPU &instance)
{
  return {{instance.worldBoundsMin[0], instance.worldBoundsMin[1], instance.worldBoundsMin[2]},
          {instance.worldBoundsMax[0], instance.worldBoundsMax[1], instance.worldBoundsMax[2]}};
}

std::array<float, 3> transformPoint(const Matrix &matrix, const std::array<float, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

bool inverseAffine(const Matrix &matrix, Matrix &inverse)
{
  const double a00 = matrix[0], a01 = matrix[4], a02 = matrix[8];
  const double a10 = matrix[1], a11 = matrix[5], a12 = matrix[9];
  const double a20 = matrix[2], a21 = matrix[6], a22 = matrix[10];
  const double c00 = a11 * a22 - a12 * a21;
  const double c01 = a02 * a21 - a01 * a22;
  const double c02 = a01 * a12 - a02 * a11;
  const double c10 = a12 * a20 - a10 * a22;
  const double c11 = a00 * a22 - a02 * a20;
  const double c12 = a02 * a10 - a00 * a12;
  const double c20 = a10 * a21 - a11 * a20;
  const double c21 = a01 * a20 - a00 * a21;
  const double c22 = a00 * a11 - a01 * a10;
  const double determinant = a00 * c00 + a01 * c10 + a02 * c20;
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12)
    return false;
  const double reciprocal = 1.0 / determinant;
  inverse = {};
  inverse[0] = static_cast<float>(c00 * reciprocal);
  inverse[4] = static_cast<float>(c01 * reciprocal);
  inverse[8] = static_cast<float>(c02 * reciprocal);
  inverse[1] = static_cast<float>(c10 * reciprocal);
  inverse[5] = static_cast<float>(c11 * reciprocal);
  inverse[9] = static_cast<float>(c12 * reciprocal);
  inverse[2] = static_cast<float>(c20 * reciprocal);
  inverse[6] = static_cast<float>(c21 * reciprocal);
  inverse[10] = static_cast<float>(c22 * reciprocal);
  inverse[15] = 1.0f;
  const std::array<float, 3> translation{matrix[12], matrix[13], matrix[14]};
  const std::array<float, 3> inverseTranslation = transformPoint(inverse, {-translation[0], -translation[1], -translation[2]});
  inverse[12] = inverseTranslation[0];
  inverse[13] = inverseTranslation[1];
  inverse[14] = inverseTranslation[2];
  for (const float value : inverse)
    if (!std::isfinite(value))
      return false;
  return true;
}

bool validAffineTransform(const Matrix &matrix)
{
  for (const float value : matrix)
    if (!std::isfinite(value))
      return false;
  constexpr float epsilon = 1.0e-6f;
  if (std::abs(matrix[3]) > epsilon ||
      std::abs(matrix[7]) > epsilon ||
      std::abs(matrix[11]) > epsilon ||
      std::abs(matrix[15] - 1.0f) > epsilon)
    return false;
  const float determinant =
      matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
      matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
      matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
  return std::isfinite(determinant) && std::abs(determinant) > epsilon;
}

SparseVoxelAABB transformedRootBounds(const SparseVoxelBLAS &blas, const Matrix &matrix)
{
  SparseVoxelAABB result = emptyBounds();
  const float radius = blas.rootExtent * 0.5f;
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const std::array<float, 3> local{
      blas.rootCenter[0] + ((corner & 1u) != 0u ? radius : -radius),
      blas.rootCenter[1] + ((corner & 2u) != 0u ? radius : -radius),
      blas.rootCenter[2] + ((corner & 4u) != 0u ? radius : -radius),
    };
    const auto world = transformPoint(matrix, local);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      result.minimum[axis] = std::min(result.minimum[axis], world[axis]);
      result.maximum[axis] = std::max(result.maximum[axis], world[axis]);
    }
  }
  return result;
}

struct BuildNode
{
  SparseVoxelAABB bounds{};
  std::vector<uint32_t> instances;
  std::vector<uint32_t> children;
};

uint32_t buildRecursive(
    std::vector<BuildNode> &nodes,
    std::vector<uint32_t> instances,
    const std::vector<SparseVoxelRuntimeInstance> &runtimeInstances,
    const uint32_t fanout,
    const uint32_t leafSize)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
  nodes.push_back({});
  SparseVoxelAABB bounds = emptyBounds();
  for (const uint32_t instanceIndex : instances)
    expand(bounds, fromGPU(runtimeInstances[instanceIndex].gpu));
  nodes[nodeIndex].bounds = bounds;
  if (instances.size() <= leafSize)
  {
    std::sort(instances.begin(), instances.end());
    nodes[nodeIndex].instances = std::move(instances);
    return nodeIndex;
  }

  SparseVoxelAABB centroidBounds = emptyBounds();
  for (const uint32_t instanceIndex : instances)
  {
    const SparseVoxelAABB instanceBounds = fromGPU(runtimeInstances[instanceIndex].gpu);
    SparseVoxelAABB point{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      point.minimum[axis] = point.maximum[axis] = (instanceBounds.minimum[axis] + instanceBounds.maximum[axis]) * 0.5f;
    expand(centroidBounds, point);
  }
  uint32_t axis = 0u;
  for (uint32_t candidate = 1u; candidate < 3u; ++candidate)
    if (centroidBounds.maximum[candidate] - centroidBounds.minimum[candidate] > centroidBounds.maximum[axis] - centroidBounds.minimum[axis])
      axis = candidate;
  std::stable_sort(
      instances.begin(), instances.end(),
      [&](const uint32_t left, const uint32_t right)
      {
        const SparseVoxelAABB leftBounds = fromGPU(runtimeInstances[left].gpu);
        const SparseVoxelAABB rightBounds = fromGPU(runtimeInstances[right].gpu);
        const float leftCenter = leftBounds.minimum[axis] + leftBounds.maximum[axis];
        const float rightCenter = rightBounds.minimum[axis] + rightBounds.maximum[axis];
        return leftCenter != rightCenter ? leftCenter < rightCenter : left < right;
      });

  const uint32_t childCount = std::min<uint32_t>(fanout, static_cast<uint32_t>((instances.size() + leafSize - 1u) / leafSize));
  const uint32_t baseSize = static_cast<uint32_t>(instances.size()) / childCount;
  const uint32_t remainder = static_cast<uint32_t>(instances.size()) % childCount;
  uint32_t begin = 0u;
  nodes[nodeIndex].children.reserve(childCount);
  for (uint32_t child = 0u; child < childCount; ++child)
  {
    const uint32_t size = baseSize + (child < remainder ? 1u : 0u);
    std::vector<uint32_t> childInstances(instances.begin() + begin, instances.begin() + begin + size);
    const uint32_t childNode = buildRecursive(nodes, std::move(childInstances), runtimeInstances, fanout, leafSize);
    nodes[nodeIndex].children.push_back(childNode);
    begin += size;
  }
  return nodeIndex;
}

SparseVoxelTLAS flattenBreadthFirst(const std::vector<BuildNode> &buildNodes, const uint32_t buildRoot)
{
  SparseVoxelTLAS result{};
  if (buildNodes.empty())
    return result;
  std::deque<std::pair<uint32_t, uint32_t>> pending;
  pending.push_back({buildRoot, 0u});
  std::vector<uint32_t> buildToFlat(buildNodes.size(), kInvalidSparseVoxelIndex);
  while (!pending.empty())
  {
    const auto [buildIndex, depth] = pending.front();
    pending.pop_front();
    buildToFlat[buildIndex] = static_cast<uint32_t>(result.nodes.size());
    result.nodes.push_back({});
    result.maxDepth = std::max(result.maxDepth, depth + 1u);
    for (const uint32_t child : buildNodes[buildIndex].children)
      pending.push_back({child, depth + 1u});
  }
  result.rootNode = buildToFlat[buildRoot];
  for (uint32_t buildIndex = 0u; buildIndex < buildNodes.size(); ++buildIndex)
  {
    const BuildNode &source = buildNodes[buildIndex];
    SparseVoxelTLASNodeGPU &target = result.nodes[buildToFlat[buildIndex]];
    target.worldBoundsMin = {source.bounds.minimum[0], source.bounds.minimum[1], source.bounds.minimum[2], 0.0f};
    target.worldBoundsMax = {source.bounds.maximum[0], source.bounds.maximum[1], source.bounds.maximum[2], 0.0f};
    if (!source.children.empty())
    {
      target.firstChild = buildToFlat[source.children.front()];
      target.childCount = static_cast<uint32_t>(source.children.size());
    }
    else
    {
      target.firstInstance = static_cast<uint32_t>(result.instanceIndices.size());
      target.instanceCount = static_cast<uint32_t>(source.instances.size());
      result.instanceIndices.insert(result.instanceIndices.end(), source.instances.begin(), source.instances.end());
    }
  }
  return result;
}

} // namespace

bool SparseVoxelSceneManager::setScene(const SparseVoxelScene &scene, std::string *error)
{
  if (!validateSparseVoxelSceneSource(scene, error))
    return false;
  scene_ = scene;
  // A topology replacement starts a new temporal identity domain even when
  // some stable indices happen to overlap the previous scene.
  runtimeInstances_.clear();
  if (!rebuildDerivedState(error))
    return false;
  ++sceneRevision_;
  ++topologyRevision_;
  ++transformRevision_;
  return true;
}

bool SparseVoxelSceneManager::setInstanceTransform(const uint32_t stableInstanceId, const Matrix &modelToWorld, std::string *error)
{
  if (stableInstanceId >= scene_.instances.size())
    return fail(error, "Sparse voxel stable instance ID is out of range");
  if (scene_.instances[stableInstanceId].modelToWorld == modelToWorld)
  {
    if (error != nullptr)
      error->clear();
    return true;
  }
  if (!validAffineTransform(modelToWorld))
    return fail(
        error,
        "Sparse voxel instance transform is not finite, affine, and "
        "invertible");

  // BLAS hierarchy arrays are immutable when an instance moves. Mutate only
  // the small instance record and rebuild the derived instance/TLAS state;
  // copying and revalidating the complete authored scene here made animation
  // cost proportional to voxel count.
  const Matrix previousTransform =
      scene_.instances[stableInstanceId].modelToWorld;
  scene_.instances[stableInstanceId].modelToWorld = modelToWorld;
  if (!rebuildDerivedState(error))
  {
    scene_.instances[stableInstanceId].modelToWorld = previousTransform;
    std::string ignored;
    rebuildDerivedState(&ignored);
    return false;
  }
  ++sceneRevision_;
  ++transformRevision_;
  return true;
}

void SparseVoxelSceneManager::commitPreviousTransforms()
{
  for (SparseVoxelRuntimeInstance &instance : runtimeInstances_)
    instance.gpu.previousModelToWorld = instance.gpu.modelToWorld;
}

bool SparseVoxelSceneManager::rebuildDerivedState(std::string *error)
{
  std::vector<Matrix> previous(scene_.instances.size());
  for (const SparseVoxelRuntimeInstance &instance : runtimeInstances_)
    if (instance.stableId < previous.size())
      previous[instance.stableId] = instance.gpu.previousModelToWorld;

  runtimeInstances_.clear();
  runtimeInstances_.reserve(scene_.instances.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < scene_.instances.size(); ++instanceIndex)
  {
    const SparseVoxelInstance &source = scene_.instances[instanceIndex];
    SparseVoxelRuntimeInstance runtime{};
    runtime.stableId = instanceIndex;
    runtime.gpu.modelToWorld = source.modelToWorld;
    if (!inverseAffine(source.modelToWorld, runtime.gpu.worldToModel))
      return fail(error, "Sparse voxel instance transform inverse could not be derived");
    bool previousIsZero = true;
    for (const float value : previous[instanceIndex])
      previousIsZero &= value == 0.0f;
    runtime.gpu.previousModelToWorld = previousIsZero ? source.modelToWorld : previous[instanceIndex];
    const SparseVoxelAABB bounds = transformedRootBounds(scene_.models[source.blasIndex], source.modelToWorld);
    runtime.gpu.worldBoundsMin = {bounds.minimum[0], bounds.minimum[1], bounds.minimum[2], 0.0f};
    runtime.gpu.worldBoundsMax = {bounds.maximum[0], bounds.maximum[1], bounds.maximum[2], 0.0f};
    runtime.gpu.blasIndex = source.blasIndex;
    runtime.gpu.flags = source.flags;
    runtimeInstances_.push_back(runtime);
  }

  blasRootRanges_.clear();
  blasRootRanges_.reserve(scene_.models.size());
  uint64_t nodeBase = 0u;
  for (const SparseVoxelBLAS &blas : scene_.models)
  {
    const uint64_t firstRoot = nodeBase + blas.firstRoot;
    if (firstRoot > std::numeric_limits<uint32_t>::max() || nodeBase + blas.hierarchyNodes.size() > std::numeric_limits<uint32_t>::max())
      return fail(error, "Sparse voxel flattened BLAS node range exceeds 32-bit GPU addressing");
    blasRootRanges_.push_back({static_cast<uint32_t>(firstRoot), blas.rootCount, blas.maxLevel, 0u});
    nodeBase += blas.hierarchyNodes.size();
  }

  tlas_ = {};
  if (!runtimeInstances_.empty())
  {
    std::vector<uint32_t> instanceIndices(runtimeInstances_.size());
    std::iota(instanceIndices.begin(), instanceIndices.end(), 0u);
    std::vector<BuildNode> buildNodes;
    const uint32_t root = buildRecursive(buildNodes, std::move(instanceIndices), runtimeInstances_, scene_.tlasFanout, scene_.tlasLeafSize);
    tlas_ = flattenBreadthFirst(buildNodes, root);
  }
  return validateSparseVoxelTLAS(*this, error);
}

bool validateSparseVoxelTLAS(const SparseVoxelSceneManager &manager, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto &instances = manager.runtimeInstances();
  const SparseVoxelTLAS &tlas = manager.tlas();
  if (instances.empty())
    return tlas.nodes.empty() && tlas.instanceIndices.empty() && tlas.rootNode == kInvalidSparseVoxelIndex && tlas.maxDepth == 0u
               ? true
               : fail(error, "Empty sparse voxel scene has non-empty TLAS state");
  if (tlas.rootNode >= tlas.nodes.size() || tlas.maxDepth == 0u)
    return fail(error, "Sparse voxel TLAS root or depth is invalid");
  std::vector<uint32_t> membership(instances.size(), 0u);
  std::vector<uint8_t> visited(tlas.nodes.size(), 0u);
  std::deque<uint32_t> pending{tlas.rootNode};
  while (!pending.empty())
  {
    const uint32_t nodeIndex = pending.front();
    pending.pop_front();
    if (nodeIndex >= tlas.nodes.size() || visited[nodeIndex] != 0u)
      return fail(error, "Sparse voxel TLAS is cyclic or references an invalid node");
    visited[nodeIndex] = 1u;
    const SparseVoxelTLASNodeGPU &node = tlas.nodes[nodeIndex];
    const SparseVoxelAABB bounds = fromGPU(node);
    if (!validBounds(bounds) || (node.childCount != 0u) == (node.instanceCount != 0u))
      return fail(error, "Sparse voxel TLAS node kind or bounds are invalid");
    if (node.childCount != 0u)
    {
      if (node.childCount > manager.scene().tlasFanout || node.firstChild + node.childCount > tlas.nodes.size())
        return fail(error, "Sparse voxel TLAS child range is invalid");
      for (uint32_t offset = 0u; offset < node.childCount; ++offset)
      {
        const uint32_t child = node.firstChild + offset;
        if (!contains(bounds, fromGPU(tlas.nodes[child])))
          return fail(error, "Sparse voxel TLAS parent does not contain its child");
        pending.push_back(child);
      }
    }
    else
    {
      if (node.instanceCount > manager.scene().tlasLeafSize || node.firstInstance + node.instanceCount > tlas.instanceIndices.size())
        return fail(error, "Sparse voxel TLAS leaf instance range is invalid");
      for (uint32_t offset = 0u; offset < node.instanceCount; ++offset)
      {
        const uint32_t instanceIndex = tlas.instanceIndices[node.firstInstance + offset];
        if (instanceIndex >= instances.size() || !contains(bounds, fromGPU(instances[instanceIndex].gpu)))
          return fail(error, "Sparse voxel TLAS leaf contains an invalid instance");
        ++membership[instanceIndex];
      }
    }
  }
  if (std::find(visited.begin(), visited.end(), uint8_t{0}) != visited.end())
    return fail(error, "Sparse voxel TLAS contains unreachable nodes");
  if (std::find_if(membership.begin(), membership.end(), [](const uint32_t count) { return count != 1u; }) != membership.end())
    return fail(error, "Sparse voxel TLAS does not contain every instance exactly once");
  return true;
}

} // namespace voxel::svraster
