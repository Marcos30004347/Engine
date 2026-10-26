#include "voxel/svraster/SparseVoxelScene.hpp"

#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <cmath>
#include <set>
#include <utility>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool finiteNonNegativeHalf(const uint16_t value)
{
  return (value & 0x8000u) == 0u && (value & 0x7c00u) != 0x7c00u;
}

bool validAffineTransform(const std::array<float, 16> &matrix)
{
  for (const float value : matrix)
    if (!std::isfinite(value))
      return false;
  constexpr float epsilon = 1.0e-6f;
  if (std::abs(matrix[3]) > epsilon || std::abs(matrix[7]) > epsilon || std::abs(matrix[11]) > epsilon || std::abs(matrix[15] - 1.0f) > epsilon)
    return false;
  const float determinant =
      matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
      matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
      matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
  return std::isfinite(determinant) && std::abs(determinant) > epsilon;
}

uint64_t ancestorPath(const uint64_t path, const uint32_t level)
{
  const uint32_t unusedBits = 3u * (kMaxOctreeLevels - level);
  return (path >> unusedBits) << unusedBits;
}

} // namespace

bool validateSparseVoxelSceneSource(const SparseVoxelScene &scene, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (scene.tlasFanout < 2u || scene.tlasFanout > 8u || scene.tlasLeafSize == 0u || scene.tlasLeafSize > 64u)
    return fail(error, "Sparse voxel TLAS build settings are invalid");
  if (scene.sourceInfo.sourceRelativePath.find('\0') != std::string::npos || scene.sourceInfo.normalizedBuildOptions.find('\0') != std::string::npos)
    return fail(error, "Sparse voxel source strings contain an embedded NUL");
  std::set<uint32_t> referencedMaterialIds;
  for (const SparseVoxelMaterialAssetReference &reference : scene.voxelMaterialReferences)
  {
    if (reference.assetId.empty() || reference.assetId.find('\0') != std::string::npos)
      return fail(error, "Sparse voxel material asset reference has an invalid ID");
    if (!referencedMaterialIds.insert(reference.sourceMaterialId).second)
      return fail(error, "Sparse voxel material asset references duplicate a source material ID");
  }

  bool hasSourceIds = false;
  bool lacksSourceIds = false;
  for (uint32_t modelIndex = 0u; modelIndex < scene.models.size(); ++modelIndex)
  {
    const SparseVoxelBLAS &model = scene.models[modelIndex];
    if (!std::isfinite(model.rootCenter[0]) || !std::isfinite(model.rootCenter[1]) || !std::isfinite(model.rootCenter[2]) || !std::isfinite(model.rootExtent) || model.rootExtent <= 0.0f)
      return fail(error, "Sparse voxel BLAS root cube is invalid");
    if (model.maxLevel == 0u || model.maxLevel > kMaxOctreeLevels || model.flags != 0u)
      return fail(error, "Sparse voxel BLAS metadata is invalid");
    const size_t count = model.hierarchyNodes.size();
    if (count == 0u || model.octreePaths.size() != count || model.octreeLevels.size() != count || model.centersLengths.size() != count || model.materials.size() != count ||
        (!model.sourceIds.empty() && model.sourceIds.size() != count))
      return fail(error, "Sparse voxel BLAS parallel array sizes differ");
    if (model.firstRoot + model.rootCount > count || model.rootCount == 0u || model.rootCount > 8u)
      return fail(error, "Sparse voxel BLAS root range is invalid");
    hasSourceIds |= !model.sourceIds.empty();
    lacksSourceIds |= model.sourceIds.empty();

    uint32_t previousLevel = 0u;
    uint64_t previousPath = 0u;
    std::vector<uint32_t> parentCounts(count, 0u);
    for (size_t voxelIndex = 0u; voxelIndex < count; ++voxelIndex)
    {
      const uint64_t path = model.octreePaths[voxelIndex];
      const uint32_t level = model.octreeLevels[voxelIndex];
      if (level == 0u || level > model.maxLevel || !isCanonicalOctreePath(path, level))
        return fail(error, "Sparse voxel path or level is invalid");
      if (voxelIndex != 0u && (level < previousLevel || (level == previousLevel && path <= previousPath)))
        return fail(error, "Sparse voxel paths are not in canonical (level, path) order");
      previousLevel = level;
      previousPath = path;

      const SparseVoxelLODNodeGPU &node = model.hierarchyNodes[voxelIndex];
      if (node.voxelIndex != voxelIndex || node.level != level || node.childCount > 8u || node.firstChild + node.childCount > count)
        return fail(error, "Sparse voxel hierarchy node metadata is invalid");
      for (uint32_t childOffset = 0u; childOffset < node.childCount; ++childOffset)
      {
        const uint32_t childIndex = node.firstChild + childOffset;
        if (model.hierarchyNodes[childIndex].level != level + 1u || ancestorPath(model.octreePaths[childIndex], level) != path)
          return fail(error, "Sparse voxel hierarchy child relationship is invalid");
        ++parentCounts[childIndex];
      }
      const SparseVoxelCenterLengthGPU &centerLength = model.centersLengths[voxelIndex];
      if (!std::isfinite(centerLength.center[0]) || !std::isfinite(centerLength.center[1]) || !std::isfinite(centerLength.center[2]) || !std::isfinite(centerLength.length) || centerLength.length <= 0.0f)
        return fail(error, "Sparse voxel decoded center or length is invalid");
      const std::array<uint32_t, 3> coordinate = decodeOctreePath(path, level);
      const float expectedLength = model.rootExtent / static_cast<float>(uint32_t{1} << level);
      const std::array<float, 3> expectedCenter{
        model.rootCenter[0] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * expectedLength,
        model.rootCenter[1] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * expectedLength,
        model.rootCenter[2] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * expectedLength,
      };
      if (centerLength.length != expectedLength || centerLength.center[0] != expectedCenter[0] || centerLength.center[1] != expectedCenter[1] || centerLength.center[2] != expectedCenter[2])
        return fail(error, "Sparse voxel decoded center or length disagrees with its canonical path");

      const SparseVoxelMaterialGPU &material = model.materials[voxelIndex];
      if ((material.albedoRGBA8 >> 24u) != 0xffu || (material.roughnessMetallicUNorm8 & 0xffff0000u) != 0u || (material.emissiveB16F & 0xffff0000u) != 0u)
        return fail(error, "Sparse voxel material packing is not canonical");
      if (!finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveRG16F)) || !finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveRG16F >> 16u)) || !finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveB16F)))
        return fail(error, "Sparse voxel emissive material is invalid");
      if (material.flags != 0u ||
          !sparseVoxelMaterialSecondaryLayerIsCanonical(material))
        return fail(error, "Sparse voxel material flags or secondary layer are invalid");
    }
    for (uint32_t nodeIndex = 0u; nodeIndex < count; ++nodeIndex)
    {
      const bool root = nodeIndex >= model.firstRoot && nodeIndex < model.firstRoot + model.rootCount;
      if ((root && (model.hierarchyNodes[nodeIndex].level != 1u || parentCounts[nodeIndex] != 0u)) || (!root && parentCounts[nodeIndex] != 1u))
        return fail(error, "Sparse voxel hierarchy parent relationship is invalid");
    }
  }
  if (hasSourceIds && lacksSourceIds)
    return fail(error, "Sparse voxel SourceIds must be present for every model or omitted globally");

  for (const SparseVoxelInstance &instance : scene.instances)
  {
    if (instance.blasIndex >= scene.models.size() || instance.flags != 0u)
      return fail(error, "Sparse voxel instance reference or flags are invalid");
    if (!validAffineTransform(instance.modelToWorld))
      return fail(error, "Sparse voxel instance transform is not finite, affine, and invertible");
  }
  return true;
}

SparseVoxelScene makeExteriorShellScene(const uint64_t materialSeed)
{
  const BootstrapSparseVoxelLOD generated = generateExteriorShellLOD128(materialSeed);
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.0f, 0.0f, 0.0f};
  model.rootExtent = kBootstrapRootExtent;
  model.maxLevel = kExteriorShellLevel;
  model.firstRoot = generated.firstRoot;
  model.rootCount = generated.rootCount;
  model.hierarchyNodes = generated.nodes;
  model.octreePaths.reserve(generated.pathsLevels.size());
  model.octreeLevels.reserve(generated.pathsLevels.size());
  for (const SparseVoxelPathLevelGPU &pathLevel : generated.pathsLevels)
  {
    model.octreePaths.push_back(static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u));
    model.octreeLevels.push_back(static_cast<uint8_t>(pathLevel.level));
  }
  model.centersLengths = generated.centersLengths;
  model.materials = generated.materials;
  scene.models.push_back(std::move(model));
  scene.instances.push_back({0u, 0u, {1.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f}});
  scene.sourceInfo.importerVersion = 1u;
  scene.sourceInfo.sourceRelativePath = "generated/exterior-shell-128";
  scene.sourceInfo.normalizedBuildOptions = "level=7;solid=1";
  scene.sourceContentHash = splitmix64(materialSeed);
  scene.buildSettingsHash = splitmix64(scene.sourceContentHash);
  return scene;
}

} // namespace voxel::svraster
