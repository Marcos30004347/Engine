#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneBridge.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneManager.hpp"
#include "math/math.hpp"
#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

using namespace virtualgeometry;

namespace
{

namespace fs = std::filesystem;
using Path = gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path;

bool floatEqual(float left, float right, float epsilon = 1.0e-5f)
{
  return std::fabs(left - right) <= epsilon;
}

bool isExpectedGpuBackendUnavailable(const std::exception &error)
{
  const std::string message = error.what();
#if defined(__APPLE__)
  if (message.find("failed to create instance") != std::string::npos)
    return true;
#endif
  return message.find("Vulkan is not supported on this device") != std::string::npos;
}

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path teapotAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/teapot.obj";
}

fs::path buildFallbackMeshPath(const fs::path &virtualGeometryPath, uint32_t lodLevel)
{
  return virtualGeometryPath.parent_path() /
         (virtualGeometryPath.stem().string() + ".lod" + std::to_string(lodLevel) + ".fallback.mesh");
}

fs::path buildSoftwareRaytraceScenePath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() /
         (virtualGeometryPath.stem().string() + ".softwareraytrace");
}

size_t sourceIndexCount(const VirtualGeometrySourceAsset &asset)
{
  size_t indexCount = 0u;
  for (const SourceShapeBuildInput &shape : asset.shapes)
    for (const MeshPart &meshPart : shape.meshParts)
      indexCount += meshPart.shape.indices.size();
  return indexCount;
}

VirtualGeometrySourceAsset makeMultiShapeSourceAsset()
{
  VirtualGeometrySourceAsset asset{};
  asset.materialFiles = {
      "materials/shape0.vmat",
      "materials/shape1.vmat",
      "materials/shape2.vmat",
      "materials/shape3.vmat",
  };

  const auto addVertex = [&asset](float x, float y, float z, float u, float v) -> uint32_t
  {
    Vertex vertex(x, y, z);
    vertex.norm[0] = 0.0f;
    vertex.norm[1] = 0.0f;
    vertex.norm[2] = 1.0f;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    asset.vertices.push_back(vertex);
    return static_cast<uint32_t>(asset.vertices.size() - 1u);
  };

  for (uint32_t shapeIndex = 0u; shapeIndex < 4u; ++shapeIndex)
  {
    SourceShapeBuildInput shape{};
    shape.materialIndex = shapeIndex;
    MeshPart meshPart{};

    const uint32_t triangleCount = shapeIndex + 1u;
    const float xBase = static_cast<float>(shapeIndex) * 8.0f;
    for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
    {
      const float yBase = static_cast<float>(triangleIndex) * 1.75f;
      const float z = 0.25f * static_cast<float>(shapeIndex);
      const uint32_t i0 = addVertex(xBase + 0.0f, yBase + 0.0f, z, 0.0f, 0.0f);
      const uint32_t i1 = addVertex(xBase + 1.0f, yBase + 0.0f, z, 1.0f, 0.0f);
      const uint32_t i2 = addVertex(xBase + 0.25f, yBase + 1.0f, z, 0.25f, 1.0f);
      meshPart.shape.indices.push_back(i0);
      meshPart.shape.indices.push_back(i1);
      meshPart.shape.indices.push_back(i2);
    }

    shape.meshParts.push_back(std::move(meshPart));
    asset.shapes.push_back(std::move(shape));
  }

  return asset;
}

std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU> convertExpectedVertices(const FallbackMeshData &mesh)
{
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU> vertices;
  vertices.reserve(mesh.vertices.size());
  for (const FallbackMeshVertex &vertex : mesh.vertices)
  {
    gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU gpuVertex{};
    gpuVertex.position = math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]);
    gpuVertex.boneWeightOffset = 0u;
    for (uint32_t channel = 0u; channel < 4u; ++channel)
    {
      gpuVertex.surfaceBaseColorOpacity[channel] = vertex.surfaceBaseColorOpacity[channel];
      gpuVertex.surfaceNormalRoughness[channel] = vertex.surfaceNormalRoughness[channel];
      gpuVertex.surfaceEmissiveMetallic[channel] = vertex.surfaceEmissiveMetallic[channel];
      gpuVertex.surfaceFlags[channel] = vertex.surfaceFlags[channel];
    }
    vertices.push_back(gpuVertex);
  }
  return vertices;
}

gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU convertExpectedMeshEntry(const FallbackMeshData &mesh)
{
  gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU meshEntry{};
  meshEntry.vertexOffset = 0u;
  meshEntry.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
  meshEntry.indexOffset = 0u;
  meshEntry.triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  meshEntry.boneWeightsPerVertex = mesh.skeleton.getBoneCount() > 0u ? 4u : 0u;
  meshEntry.flags = mesh.skeleton.getBoneCount() > 0u ? gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshFlag_Skinned
                                                       : gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshFlag_None;
  return meshEntry;
}

gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU convertExpectedInstance(const FallbackMeshData &mesh)
{
  gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU instance{};
  instance.modelMatrix = math::Mat4f::identity();
  instance.previousModelMatrix = math::Mat4f::identity();
  instance.aabbMin = math::Vec3f(mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2]);
  instance.meshIndex = 0u;
  instance.aabbMax = math::Vec3f(mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]);
  instance.flags = gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceFlag_None;
  instance.boneMatrixOffset = 0u;
  instance.previousBoneMatrixOffset = 0u;
  instance.boneCount = 0u;
  return instance;
}

bool equalVec3(const math::Vec3f &left, const math::Vec3f &right, float epsilon = 1.0e-5f)
{
  return floatEqual(left[0], right[0], epsilon) &&
         floatEqual(left[1], right[1], epsilon) &&
         floatEqual(left[2], right[2], epsilon);
}

bool equalMat4(const math::Mat4f &left, const math::Mat4f &right, float epsilon = 1.0e-5f)
{
  for (uint32_t index = 0u; index < 16u; ++index)
  {
    if (!floatEqual(left.data[index], right.data[index], epsilon))
      return false;
  }
  return true;
}

void assertMeshTableEntryEqual(
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU &actual,
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU &expected)
{
  assert(actual.vertexOffset == expected.vertexOffset);
  assert(actual.vertexCount == expected.vertexCount);
  assert(actual.indexOffset == expected.indexOffset);
  assert(actual.triangleCount == expected.triangleCount);
  assert(actual.boneWeightsPerVertex == expected.boneWeightsPerVertex);
  assert(actual.flags == expected.flags);
}

void assertVertexEqual(
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU &actual,
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU &expected)
{
  assert(equalVec3(actual.position, expected.position));
  assert(actual.boneWeightOffset == expected.boneWeightOffset);
  assert(actual.surfaceBaseColorOpacity == expected.surfaceBaseColorOpacity);
  assert(actual.surfaceNormalRoughness == expected.surfaceNormalRoughness);
  assert(actual.surfaceEmissiveMetallic == expected.surfaceEmissiveMetallic);
  assert(actual.surfaceFlags == expected.surfaceFlags);
}

void assertInstanceEqual(
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU &actual,
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU &expected)
{
  assert(equalMat4(actual.modelMatrix, expected.modelMatrix));
  assert(equalMat4(actual.previousModelMatrix, expected.previousModelMatrix));
  assert(equalVec3(actual.aabbMin, expected.aabbMin));
  assert(actual.meshIndex == expected.meshIndex);
  assert(equalVec3(actual.aabbMax, expected.aabbMax));
  assert(actual.flags == expected.flags);
  assert(actual.boneMatrixOffset == expected.boneMatrixOffset);
  assert(actual.previousBoneMatrixOffset == expected.previousBoneMatrixOffset);
  assert(actual.boneCount == expected.boneCount);
}

void fillReadbackVector(
    rendering::RenderGraph *renderGraph,
    const rendering::Buffer &buffer,
    void *destination,
    uint64_t byteSize)
{
  renderGraph->bufferRead(
      buffer,
      0u,
      byteSize,
      [&](const void *data)
      {
        if (byteSize > 0u)
          std::memcpy(destination, data, static_cast<size_t>(byteSize));
      });
}

} // namespace

int main()
{
  os::Logger::start(100);

  const fs::path teapotPath = teapotAssetPath();
  assert(fs::exists(teapotPath));

  const fs::path tempRoot = fs::temp_directory_path() / "GPUSignedDistanceFieldsSceneBridgeTests";
  const fs::path materialsDir = tempRoot / "materials";
  const fs::path virtualGeometryPath = tempRoot / "teapot.obj.virtualgeometry";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  assert(!error);

  os::Logger::logf("[Test] Loading source asset: %s", teapotPath.string().c_str());
  const VirtualGeometrySourceAsset sourceAsset =
      VirtualGeometryEncoder::loadAssetFromOBJFile(teapotPath.string(), materialsDir.string());
  assert(!sourceAsset.vertices.empty());
  assert(!sourceAsset.shapes.empty());

  FallbackMeshBuilder::Settings settings{};
  settings.lod0TargetTriangleRatio = 0.25f;
  settings.maxLODCount = 6u;
  const std::vector<FallbackMeshData> fallbackLods = FallbackMeshBuilder::buildLODs(sourceAsset, settings);
  assert(!fallbackLods.empty());
  const FallbackMeshData &expectedFallback = fallbackLods.front();
  assert((expectedFallback.indices.size() % 3u) == 0u);
  assert(expectedFallback.indices.size() <= sourceIndexCount(sourceAsset) / 4u);
  const fs::path fallbackPath = buildFallbackMeshPath(virtualGeometryPath, expectedFallback.lodLevel);
  assert(FallbackMeshFile::save(fallbackPath.string(), expectedFallback));

  FallbackMeshData loadedFallback;
  assert(FallbackMeshFile::load(fallbackPath.string(), loadedFallback));
  assert(loadedFallback.indices == expectedFallback.indices);
  assert(loadedFallback.vertices.size() == expectedFallback.vertices.size());
  os::Logger::logf(
      "[Test] Fallback prepared: lod=%u vertices=%zu triangles=%zu path=%s",
      loadedFallback.lodLevel,
      loadedFallback.vertices.size(),
      loadedFallback.indices.size() / 3u,
      fallbackPath.string().c_str());

  VirtualGeometryBuildData build = VirtualGeometryBuilder::build(sourceAsset);
  QuantizationConfig quantizationConfig{};
  quantizationConfig.quantization_factor = 4u;
  quantizationConfig.unit_scale = 100.0f;
  VirtualGeometryEncodedData encoded = VirtualGeometryCompressor::encode(build, quantizationConfig);

  {
    VirtualGeometryFile writer(virtualGeometryPath.string(), true);
    assert(writer.isOpen());
    assert(writer.write(encoded, build.pages, MESHLET_LZ4));
  }
  os::Logger::logf(
      "[Test] Virtual geometry encoded: hierarchy=%zu pages=%zu path=%s",
      encoded.hierarchy.size(),
      build.pages.size(),
      virtualGeometryPath.string().c_str());

  rendering::DeviceRequiredLimits limits{0u, 0u, 0u};
  rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute;
  std::unique_ptr<rendering::backend::vulkan::VulkanRHI> rhi;
  std::unique_ptr<rendering::RenderGraph> renderGraph;
  try
  {
    rhi = std::make_unique<rendering::backend::vulkan::VulkanRHI>(
        rendering::backend::vulkan::Vulkan_1_2,
        limits,
        features,
        std::vector<std::string>{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
    renderGraph = std::make_unique<rendering::RenderGraph>(rhi.get());
  }
  catch (const std::exception &gpuError)
  {
    if (!isExpectedGpuBackendUnavailable(gpuError))
      throw;

    os::Logger::warningf("[Test] GPU upload/readback skipped: %s", gpuError.what());
    fs::remove_all(tempRoot, error);
    os::Logger::shutdown();
    std::cout << "GPUSignedDistanceFieldsSceneBridgeTests passed (GPU upload/readback skipped)\n";
    return 0;
  }
  os::Logger::logf("[Test] GPU backend initialized; validating upload buffers");

  const uint32_t hierarchyBytes = std::max<uint32_t>(
      static_cast<uint32_t>(encoded.hierarchy.size() * sizeof(VirtualGeometryHierarchy) * 4u),
      4096u);
  const uint64_t pagesBytes = 64ull * 1024ull * 1024ull;
  VirtualGeometryScene scene(renderGraph.get(), hierarchyBytes, pagesBytes);
  scene.registerObjectForStreaming("teapot", virtualGeometryPath.string());
  const InstanceId instanceId = scene.instantiateObjectInstance(
      "teapot",
      math::Vec3f(0.0f, 0.0f, 0.0f),
      math::Quatf::identity(),
      1.0f);
  assert(instanceId != INVALID_INSTANCE_ID);
  assert(scene.objectInstances.size() == 1u);
  scene.updateInstanceBuffer();
  scene.prepareFrame(0u);
  os::Logger::logf(
      "[Test] Scene prepared: instances=%zu sparseMappings=%zu",
      scene.objectInstances.size(),
      scene.objectSparseToDense.size());

  gpusigneddistancefields::GPUSignedDistanceFieldsManager::Settings managerSettings{};
  managerSettings.cascadeCount = 1u;
  managerSettings.firstCascadeVoxelSize = 1.0f;
  managerSettings.cascadeVoxelSizeScale = 2.0f;
  managerSettings.atlasBrickCapacityPerAxis = 4u;
  managerSettings.maxBricks = 32u;
  managerSettings.maxMeshes = 4u;
  managerSettings.maxVertices = std::max<uint32_t>(64u, static_cast<uint32_t>(loadedFallback.vertices.size()));
  managerSettings.maxIndices = std::max<uint32_t>(64u, static_cast<uint32_t>(loadedFallback.indices.size()));
  managerSettings.maxBoneWeights = 64u;
  managerSettings.maxBoneMatrices = 64u;
  managerSettings.maxInstancesPerPath = 8u;
  managerSettings.maxJobsPerCascade = 8u;
  managerSettings.maxReferences = 128u;
  managerSettings.maxBricksPerBake = 32u;

  gpusigneddistancefields::GPUSignedDistanceFieldsManager manager(renderGraph.get(), managerSettings);
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneBridge bridge(manager);
  bridge.sync(scene, math::Vec3f(0.0f, 0.0f, 0.0f));

  const auto &diagnostics = bridge.getDiagnostics();
  os::Logger::logf(
      "[Test] Bridge diagnostics: loaded=%u missing=%u prepared=%u staticInstances=%u skinnedInstances=%u uploadedMeshes=%u uploadedVertices=%u uploadedIndices=%u skipped(emptyIds=%u runtime=%u fallback=%u scene=%u bounds=%u) pending=%u updated=%u",
      diagnostics.availableFallbackMeshCount,
      diagnostics.missingFallbackMeshCount,
      diagnostics.preparedInstanceCount,
      diagnostics.staticInstanceCount,
      diagnostics.skinnedInstanceCount,
      diagnostics.uploadedMeshCount,
      diagnostics.uploadedVertexCount,
      diagnostics.uploadedIndexCount,
      diagnostics.skippedEmptySceneInstanceCount,
      diagnostics.skippedMissingRuntimeDataCount,
      diagnostics.skippedMissingFallbackMeshCount,
      diagnostics.skippedMissingSceneInstanceCount,
      diagnostics.skippedInvalidBoundsCount,
      bridge.getPendingUpdateCount(),
      bridge.getLastFrameUpdatedCount());

  assert(diagnostics.availableFallbackMeshCount == 1u);
  assert(diagnostics.missingFallbackMeshCount == 0u);
  assert(diagnostics.preparedInstanceCount == 1u);
  assert(diagnostics.staticInstanceCount == 1u);
  assert(diagnostics.skinnedInstanceCount == 0u);
  assert(diagnostics.uploadedMeshCount == 1u);
  assert(diagnostics.uploadedVertexCount == loadedFallback.vertices.size());
  assert(diagnostics.uploadedIndexCount == loadedFallback.indices.size());
  assert(diagnostics.uploadedBoneWeightCount == 0u);
  assert(diagnostics.skippedEmptySceneInstanceCount == 0u);
  assert(diagnostics.skippedMissingRuntimeDataCount == 0u);
  assert(diagnostics.skippedMissingFallbackMeshCount == 0u);
  assert(diagnostics.skippedMissingSceneInstanceCount == 0u);
  assert(diagnostics.skippedInvalidBoundsCount == 0u);
  assert(diagnostics.lastLoadedFallbackPath == fallbackPath.string());
  assert(bridge.getPendingUpdateCount() == 0u);
  assert(bridge.getLastFrameUpdatedCount() == 1u);

  const auto &uploadedMeshTable = manager.getUploadedMeshTable();
  const auto &uploadedVertices = manager.getUploadedVertices();
  const auto &uploadedIndices = manager.getUploadedIndices();
  const auto &uploadedBoneWeights = manager.getUploadedBoneWeights();
  const auto &uploadedBoneMatrices = manager.getUploadedBoneMatrices();
  const auto &uploadedStaticInstances = manager.getUploadedInstances(Path::Static);
  const auto &uploadedSkinnedInstances = manager.getUploadedInstances(Path::Skinned);
  assert(uploadedMeshTable.size() == 1u);
  assert(uploadedVertices.size() == loadedFallback.vertices.size());
  assert(uploadedIndices.size() == loadedFallback.indices.size());
  assert(uploadedBoneWeights.empty());
  assert(uploadedBoneMatrices.empty());
  assert(uploadedStaticInstances.size() == 1u);
  assert(uploadedSkinnedInstances.empty());
  assert(manager.getPathResources(Path::Static).instanceCount == 1u);
  assert(manager.getPathResources(Path::Skinned).instanceCount == 0u);

  const auto expectedVertices = convertExpectedVertices(loadedFallback);
  const auto expectedMeshEntry = convertExpectedMeshEntry(loadedFallback);
  const auto expectedInstance = convertExpectedInstance(loadedFallback);

  assertMeshTableEntryEqual(uploadedMeshTable.front(), expectedMeshEntry);
  for (size_t vertexIndex = 0u; vertexIndex < expectedVertices.size(); ++vertexIndex)
  {
    assertVertexEqual(uploadedVertices[vertexIndex], expectedVertices[vertexIndex]);
  }
  assert(uploadedIndices == loadedFallback.indices);
  assertInstanceEqual(uploadedStaticInstances.front(), expectedInstance);
  assert(uploadedStaticInstances.front().meshIndex == 0u);
  assert(uploadedStaticInstances.front().flags == gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceFlag_None);
  os::Logger::logf(
      "[Test] Upload payload validated before GPU readback: meshes=%zu vertices=%zu indices=%zu staticInstances=%zu",
      uploadedMeshTable.size(),
      uploadedVertices.size(),
      uploadedIndices.size(),
      uploadedStaticInstances.size());

  const fs::path unifiedVirtualGeometryPath = tempRoot / "teapot_unified.obj.virtualgeometry";
  {
    VirtualGeometryFile writer(unifiedVirtualGeometryPath.string(), true);
    assert(writer.isOpen());
    assert(writer.write(encoded, build.pages, MESHLET_LZ4));
  }
  const fs::path softwareRaytracePath = buildSoftwareRaytraceScenePath(unifiedVirtualGeometryPath);
  const softwarebvh::SoftwareRayTraceSceneInstanceData softwareRaytraceEntry =
      softwarebvh::SoftwareRayTraceSceneFile::buildInstance(expectedFallback, 0u, 0u, 0u, unifiedVirtualGeometryPath.string());
  assert(softwarebvh::SoftwareRayTraceSceneFile::save(softwareRaytracePath.string(), softwareRaytraceEntry));

  auto unifiedRenderGraph = std::make_unique<rendering::RenderGraph>(rhi.get());
  VirtualGeometryScene unifiedScene(unifiedRenderGraph.get(), hierarchyBytes, pagesBytes);
  unifiedScene.registerObjectForStreaming("teapot_unified", unifiedVirtualGeometryPath.string());
  const InstanceId unifiedInstanceId = unifiedScene.instantiateObjectInstance(
      "teapot_unified",
      math::Vec3f(0.0f, 0.0f, 0.0f),
      math::Quatf::identity(),
      1.0f);
  assert(unifiedInstanceId != INVALID_INSTANCE_ID);
  unifiedScene.updateInstanceBuffer();
  unifiedScene.prepareFrame(0u);

  gpusigneddistancefields::GPUSignedDistanceFieldsManager unifiedManager(unifiedRenderGraph.get(), managerSettings);
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager sceneManager(unifiedManager);
  sceneManager.sync(unifiedScene, math::Vec3f(0.0f, 0.0f, 0.0f));

  const auto &managerDiagnostics = sceneManager.getDiagnostics();
  assert(managerDiagnostics.availableFallbackMeshCount == 1u);
  assert(managerDiagnostics.missingFallbackMeshCount == 0u);
  assert(managerDiagnostics.preparedInstanceCount == 1u);
  assert(managerDiagnostics.staticInstanceCount == 1u);
  assert(managerDiagnostics.skippedMissingFallbackMeshCount == 0u);
  assert(managerDiagnostics.uploadedMeshCount == 1u);
  assert(managerDiagnostics.uploadedVertexCount == loadedFallback.vertices.size());
  assert(managerDiagnostics.uploadedIndexCount == loadedFallback.indices.size());
  assert(managerDiagnostics.lastLoadedFallbackPath == softwareRaytracePath.string());
  assert(unifiedManager.getUploadedVertices().size() == loadedFallback.vertices.size());
  assert(unifiedManager.getUploadedIndices() == loadedFallback.indices);
  assert(unifiedManager.getPathResources(Path::Static).instanceCount == 1u);
  os::Logger::logf(
      "[Test] SceneManager loaded unified software raytrace fallback: path=%s vertices=%u indices=%u",
      managerDiagnostics.lastLoadedFallbackPath.c_str(),
      managerDiagnostics.uploadedVertexCount,
      managerDiagnostics.uploadedIndexCount);

  auto cascadeRenderGraph = std::make_unique<rendering::RenderGraph>(rhi.get());
  VirtualGeometryScene cascadeScene(cascadeRenderGraph.get(), hierarchyBytes, pagesBytes);
  cascadeScene.registerObjectForStreaming("teapot_unified", unifiedVirtualGeometryPath.string());
  const InstanceId cascadeInstanceId = cascadeScene.instantiateObjectInstance(
      "teapot_unified",
      math::Vec3f(0.0f, 0.0f, 0.0f),
      math::Quatf::identity(),
      1.0f);
  assert(cascadeInstanceId != INVALID_INSTANCE_ID);
  cascadeScene.updateInstanceBuffer();
  cascadeScene.prepareFrame(0u);

  gpusigneddistancefields::GPUSignedDistanceFieldsManager::Settings cascadeManagerSettings = managerSettings;
  cascadeManagerSettings.cascadeCount = 2u;
  cascadeManagerSettings.maxJobsPerCascade = 16u;
  gpusigneddistancefields::GPUSignedDistanceFieldsManager cascadeManager(cascadeRenderGraph.get(), cascadeManagerSettings);
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager cascadeSceneManager(cascadeManager);
  cascadeSceneManager.sync(cascadeScene, math::Vec3f(0.0f, 0.0f, 0.0f));

  const auto &uploadedCascadeJobs = cascadeManager.getUploadedJobs(Path::Static);
  assert(uploadedCascadeJobs.size() >= 2u);
  assert(!uploadedCascadeJobs[0].empty());
  assert(!uploadedCascadeJobs[1].empty());
  const auto hasInvalidateJob = [](const std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::JobGPU> &jobs)
  {
    return std::any_of(
        jobs.begin(),
        jobs.end(),
        [](const gpusigneddistancefields::GPUSignedDistanceFieldsManager::JobGPU &job)
        {
          return (job.flags & gpusigneddistancefields::GPUSignedDistanceFieldsManager::JobFlag_Invalidate) != 0u;
        });
  };
  const bool coarseCascadeHasBuildJob = std::any_of(
      uploadedCascadeJobs[1].begin(),
      uploadedCascadeJobs[1].end(),
      [](const gpusigneddistancefields::GPUSignedDistanceFieldsManager::JobGPU &job)
      {
        return (job.flags & gpusigneddistancefields::GPUSignedDistanceFieldsManager::JobFlag_Invalidate) == 0u;
      });
  assert(hasInvalidateJob(uploadedCascadeJobs[0]));
  assert(hasInvalidateJob(uploadedCascadeJobs[1]));
  assert(coarseCascadeHasBuildJob);
  os::Logger::logf(
      "[Test] SceneManager emits complete cascade jobs: cascade0=%zu cascade1=%zu",
      uploadedCascadeJobs[0].size(),
      uploadedCascadeJobs[1].size());

  const VirtualGeometrySourceAsset multiShapeAsset = makeMultiShapeSourceAsset();
  const std::vector<FallbackMeshData> multiShapeFallbackMeshes = FallbackMeshBuilder::buildOriginalPerShape(multiShapeAsset);
  assert(multiShapeFallbackMeshes.size() == multiShapeAsset.shapes.size());

  uint32_t multiShapeVertexCount = 0u;
  uint32_t multiShapeIndexCount = 0u;
  std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> multiShapeEntries;
  multiShapeEntries.reserve(multiShapeFallbackMeshes.size());
  const fs::path multiShapeVirtualGeometryPath = tempRoot / "multi_shape.obj.virtualgeometry";
  const fs::path multiShapeSoftwareRaytracePath = buildSoftwareRaytraceScenePath(multiShapeVirtualGeometryPath);

  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(multiShapeFallbackMeshes.size()); ++shapeIndex)
  {
    const FallbackMeshData &mesh = multiShapeFallbackMeshes[shapeIndex];
    multiShapeVertexCount += static_cast<uint32_t>(mesh.vertices.size());
    multiShapeIndexCount += static_cast<uint32_t>(mesh.indices.size());
    multiShapeEntries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(
        mesh,
        INVALID_INSTANCE_ID,
        INVALID_SCENE_INSTANCE_ID,
        shapeIndex,
        multiShapeVirtualGeometryPath.string()));
    multiShapeEntries.back().lodLevel = 0u;
    multiShapeEntries.back().fallbackMesh.lodLevel = 0u;
  }

  VirtualGeometryBuildData multiShapeBuild = VirtualGeometryBuilder::build(multiShapeAsset);
  VirtualGeometryEncodedData multiShapeEncoded = VirtualGeometryCompressor::encode(multiShapeBuild, quantizationConfig);
  {
    VirtualGeometryFile writer(multiShapeVirtualGeometryPath.string(), true);
    assert(writer.isOpen());
    assert(writer.write(multiShapeEncoded, multiShapeBuild.pages, MESHLET_LZ4));
  }
  assert(softwarebvh::SoftwareRayTraceSceneFile::save(multiShapeSoftwareRaytracePath.string(), multiShapeEntries));

  auto multiShapeRenderGraph = std::make_unique<rendering::RenderGraph>(rhi.get());
  VirtualGeometryScene multiShapeScene(multiShapeRenderGraph.get(), hierarchyBytes, pagesBytes);
  multiShapeScene.registerObjectForStreaming("multi_shape", multiShapeVirtualGeometryPath.string());
  const InstanceId multiShapeInstanceId = multiShapeScene.instantiateObjectInstance(
      "multi_shape",
      math::Vec3f(0.0f, 0.0f, 0.0f),
      math::Quatf::identity(),
      1.0f);
  assert(multiShapeInstanceId != INVALID_INSTANCE_ID);
  assert(multiShapeScene.objectInstances.size() == 1u);
  assert(multiShapeScene.objectInstances.front().sceneInstanceIds.size() == multiShapeFallbackMeshes.size());
  multiShapeScene.updateInstanceBuffer();
  multiShapeScene.prepareFrame(0u);

  gpusigneddistancefields::GPUSignedDistanceFieldsManager multiShapeManager(multiShapeRenderGraph.get(), managerSettings);
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager multiShapeSceneManager(multiShapeManager);
  multiShapeSceneManager.sync(multiShapeScene, math::Vec3f(0.0f, 0.0f, 0.0f));

  const auto &multiShapeDiagnostics = multiShapeSceneManager.getDiagnostics();
  const uint32_t expectedMultiShapeCount = static_cast<uint32_t>(multiShapeFallbackMeshes.size());
  assert(multiShapeDiagnostics.availableFallbackMeshCount == expectedMultiShapeCount);
  assert(multiShapeDiagnostics.missingFallbackMeshCount == 0u);
  assert(multiShapeDiagnostics.preparedInstanceCount == expectedMultiShapeCount);
  assert(multiShapeDiagnostics.staticInstanceCount == expectedMultiShapeCount);
  assert(multiShapeDiagnostics.skinnedInstanceCount == 0u);
  assert(multiShapeDiagnostics.skippedMissingFallbackMeshCount == 0u);
  assert(multiShapeDiagnostics.skippedMissingSceneInstanceCount == 0u);
  assert(multiShapeDiagnostics.uploadedMeshCount == expectedMultiShapeCount);
  assert(multiShapeDiagnostics.uploadedVertexCount == multiShapeVertexCount);
  assert(multiShapeDiagnostics.uploadedIndexCount == multiShapeIndexCount);
  assert(multiShapeManager.getUploadedMeshTable().size() == expectedMultiShapeCount);
  assert(multiShapeManager.getUploadedVertices().size() == multiShapeVertexCount);
  assert(multiShapeManager.getUploadedIndices().size() == multiShapeIndexCount);
  assert(multiShapeManager.getPathResources(Path::Static).instanceCount == expectedMultiShapeCount);
  os::Logger::logf(
      "[Test] SceneManager preserved multi-shape unified fallback scene: shapes=%u vertices=%u indices=%u path=%s",
      expectedMultiShapeCount,
      multiShapeDiagnostics.uploadedVertexCount,
      multiShapeDiagnostics.uploadedIndexCount,
      multiShapeSoftwareRaytracePath.string().c_str());

  const auto &shared = manager.getSharedResources();
  const auto &staticPathResources = manager.getPathResources(Path::Static);

  const uint64_t meshTableBytes = sizeof(gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU);
  const uint64_t verticesBytes = static_cast<uint64_t>(expectedVertices.size()) * sizeof(gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU);
  const uint64_t indicesBytes = static_cast<uint64_t>(loadedFallback.indices.size()) * sizeof(uint32_t);
  const uint64_t instancesBytes = sizeof(gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU);

  rendering::Buffer meshTableReadback = renderGraph->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFieldsSceneBridgeTests.MeshTable.readback",
          .size = meshTableBytes,
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  rendering::Buffer verticesReadback = renderGraph->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFieldsSceneBridgeTests.Vertices.readback",
          .size = std::max<uint64_t>(verticesBytes, 4u),
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  rendering::Buffer indicesReadback = renderGraph->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFieldsSceneBridgeTests.Indices.readback",
          .size = std::max<uint64_t>(indicesBytes, 4u),
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  rendering::Buffer instancesReadback = renderGraph->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFieldsSceneBridgeTests.StaticInstances.readback",
          .size = instancesBytes,
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });

  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "GPUSignedDistanceFieldsSceneBridgeTests.copyMeshTable",
      0u,
      shared.meshTable,
      0u,
      meshTableBytes,
      meshTableReadback,
      0u,
      meshTableBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "GPUSignedDistanceFieldsSceneBridgeTests.copyVertices",
      1u,
      shared.vertices,
      0u,
      verticesBytes,
      verticesReadback,
      0u,
      verticesBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "GPUSignedDistanceFieldsSceneBridgeTests.copyIndices",
      2u,
      shared.indices,
      0u,
      indicesBytes,
      indicesReadback,
      0u,
      indicesBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "GPUSignedDistanceFieldsSceneBridgeTests.copyStaticInstances",
      3u,
      staticPathResources.instances,
      0u,
      instancesBytes,
      instancesReadback,
      0u,
      instancesBytes);

  renderGraph->compile();
  rendering::RenderGraph::Frame frame;
  rendering::RenderGraph::Overrides overrides;
  renderGraph->run(frame, overrides);
  renderGraph->waitFrame(frame);
  os::Logger::logf("[Test] GPU copy passes completed, validating readback data");

  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU> meshTableGpu(1u);
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::VertexGPU> verticesGpu(expectedVertices.size());
  std::vector<uint32_t> indicesGpu(loadedFallback.indices.size());
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> staticInstancesGpu(1u);

  fillReadbackVector(renderGraph.get(), meshTableReadback, meshTableGpu.data(), meshTableBytes);
  fillReadbackVector(renderGraph.get(), verticesReadback, verticesGpu.data(), verticesBytes);
  fillReadbackVector(renderGraph.get(), indicesReadback, indicesGpu.data(), indicesBytes);
  fillReadbackVector(renderGraph.get(), instancesReadback, staticInstancesGpu.data(), instancesBytes);

  assertMeshTableEntryEqual(meshTableGpu.front(), expectedMeshEntry);
  for (size_t vertexIndex = 0u; vertexIndex < expectedVertices.size(); ++vertexIndex)
  {
    assertVertexEqual(verticesGpu[vertexIndex], expectedVertices[vertexIndex]);
  }
  assert(indicesGpu == loadedFallback.indices);
  assertInstanceEqual(staticInstancesGpu.front(), expectedInstance);
  assertMeshTableEntryEqual(meshTableGpu.front(), uploadedMeshTable.front());
  assertInstanceEqual(staticInstancesGpu.front(), uploadedStaticInstances.front());

  os::Logger::logf(
      "[Test] GPU readback validated: vertices=%zu indices=%zu staticInstances=%zu fallbackPath=%s",
      verticesGpu.size(),
      indicesGpu.size(),
      staticInstancesGpu.size(),
      fallbackPath.string().c_str());

  fs::remove_all(tempRoot, error);
  os::Logger::shutdown();
  std::cout << "GPUSignedDistanceFieldsSceneBridgeTests passed\n";
  return 0;
}
