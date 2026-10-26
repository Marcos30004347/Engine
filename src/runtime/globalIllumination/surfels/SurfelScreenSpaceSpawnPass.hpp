#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelScreenSpaceSpawnPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t frameIndex = 0u;
    uint32_t surfelCapacity = 1u;
    uint32_t surfelAllocationLimit = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t enabled = 1u;
    uint32_t spawnDownsample = 2u;
    uint32_t targetSurfelsPerTile = 1u;
    uint32_t maxSurfelsPerTile = 3u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    float targetScreenRadiusPixels = 6.0f;
    float lookupRadiusScale = 1.0f;
    float spawnMinRadius = 0.05f;
    float spawnDepthRadiusScale = 0.01f;
    float recycleCoverageThreshold = 0.25f;
    float recycleFreeListPressureBias = 0.5f;
    uint32_t recycleMaxAgeFrames = 240u;
  };

  SurfelScreenSpaceSpawnPass(
      virtualgeometry::VirtualGeometryScene &scene,
      rendering::Buffer surfelStackBuffer,
      rendering::Buffer surfelClaimGridBuffer,
      rendering::Buffer surfelRecycleClaimBuffer,
      rendering::Buffer surfelOwnerCellBuffer,
      rendering::Buffer surfelPreviousGridBuffer,
      rendering::Buffer surfelPreviousListBuffer,
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelAttachmentBuffer,
      rendering::Buffer surfelLifecycleBuffer,
      rendering::Buffer surfelMaterialBuffer,
      rendering::Buffer surfelIrradianceBuffer,
      rendering::Buffer previousSurfelReservoirBuffer,
      rendering::Buffer currentSurfelReservoirBuffer,
      rendering::Buffer previousDirectReservoirBuffer,
      rendering::Buffer currentDirectReservoirBuffer,
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Texture shadowLightingTexture,
      rendering::Texture baseColorTexture,
      rendering::Texture normalTexture,
      rendering::Texture packedGeometryIdsLoTexture,
      rendering::Texture packedGeometryIdsHiTexture,
      Settings settings)
      : scene_(scene),
        surfelStackBuffer_(surfelStackBuffer),
        surfelClaimGridBuffer_(surfelClaimGridBuffer),
        surfelRecycleClaimBuffer_(surfelRecycleClaimBuffer),
        surfelOwnerCellBuffer_(surfelOwnerCellBuffer),
        surfelPreviousGridBuffer_(surfelPreviousGridBuffer),
        surfelPreviousListBuffer_(surfelPreviousListBuffer),
        surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelAttachmentBuffer_(surfelAttachmentBuffer),
        surfelLifecycleBuffer_(surfelLifecycleBuffer),
        surfelMaterialBuffer_(surfelMaterialBuffer),
        surfelIrradianceBuffer_(surfelIrradianceBuffer),
        previousSurfelReservoirBuffer_(previousSurfelReservoirBuffer),
        currentSurfelReservoirBuffer_(currentSurfelReservoirBuffer),
        previousDirectReservoirBuffer_(previousDirectReservoirBuffer),
        currentDirectReservoirBuffer_(currentDirectReservoirBuffer),
        sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        shadowLightingTexture_(shadowLightingTexture),
        baseColorTexture_(baseColorTexture),
        normalTexture_(normalTexture),
        packedGeometryIdsLoTexture_(packedGeometryIdsLoTexture),
        packedGeometryIdsHiTexture_(packedGeometryIdsHiTexture),
        settings_(settings)
  {
  }

  ~SurfelScreenSpaceSpawnPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
  }

  void updateUniforms(
      const math::Mat4f &inverseViewProjection,
      const math::Vec3f &cameraPosition,
      const math::Vec3f &cameraRight,
      const math::Vec3f &cameraUp,
      const math::Vec3f &cameraForward,
      float worldUnitsPerPixelAtUnitDepth,
      bool reverseZ)
  {
    updateUniforms(
        inverseViewProjection,
        cameraPosition,
        cameraPosition,
        cameraRight,
        cameraUp,
        cameraForward,
        cameraRight,
        cameraUp,
        cameraForward,
        worldUnitsPerPixelAtUnitDepth,
        reverseZ);
  }

  void updateUniforms(
      const math::Mat4f &inverseViewProjection,
      const math::Vec3f &cameraPosition,
      const math::Vec3f &previousCameraPosition,
      const math::Vec3f &cameraRight,
      const math::Vec3f &cameraUp,
      const math::Vec3f &cameraForward,
      const math::Vec3f &previousCameraRight,
      const math::Vec3f &previousCameraUp,
      const math::Vec3f &previousCameraForward,
      float worldUnitsPerPixelAtUnitDepth,
      bool reverseZ)
  {
    inverseViewProjection_ = inverseViewProjection;
    cameraPosition_ = cameraPosition;
    previousCameraPosition_ = previousCameraPosition;
    cameraRight_ = cameraRight;
    cameraUp_ = cameraUp;
    cameraForward_ = cameraForward;
    previousCameraRight_ = previousCameraRight;
    previousCameraUp_ = previousCameraUp;
    previousCameraForward_ = previousCameraForward;
    worldUnitsPerPixelAtUnitDepth_ = worldUnitsPerPixelAtUnitDepth;
    reverseZ_ = reverseZ;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();

    constexpr uint32_t kSpawnTileSize = 16u;
    const uint32_t downsample = std::max(1u, settings_.spawnDownsample);
    const uint32_t dispatchWidth = (settings_.viewPortWidth + downsample - 1u) / downsample;
    const uint32_t dispatchHeight = (settings_.viewPortHeight + downsample - 1u) / downsample;

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((dispatchWidth + kSpawnTileSize - 1u) / kSpawnTileSize, (dispatchHeight + kSpawnTileSize - 1u) / kSpawnTileSize, 1u);
  }

private:
  struct Uniforms
  {
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    float cameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float previousCameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float cameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    float previousCameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float previousCameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float previousCameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    uint32_t viewport[2]{1u, 1u};
    uint32_t frameIndex = 0u;
    uint32_t reverseZ = 0u;
    uint32_t surfelCapacity = 1u;
    uint32_t surfelAllocationLimit = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t enabled = 1u;
    uint32_t spawnDownsample = 2u;
    uint32_t targetSurfelsPerTile = 1u;
    uint32_t maxSurfelsPerTile = 3u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    float targetScreenRadiusPixels = 6.0f;
    float lookupRadiusScale = 1.0f;
    float worldUnitsPerPixelAtUnitDepth = 0.001f;
    float spawnMinRadius = 0.05f;
    float spawnDepthRadiusScale = 0.01f;
    float recycleCoverageThreshold = 0.25f;
    float recycleFreeListPressureBias = 0.5f;
    uint32_t recycleMaxAgeFrames = 240u;
  };

  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
    };
  }

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  static uint64_t previousGridBufferSizeBytes(const Settings &settings)
  {
    return sizeof(uint32_t) * static_cast<uint64_t>(1u + std::max(1u, settings.gridCapacity));
  }

  static uint64_t previousListBufferSizeBytes(const Settings &settings)
  {
    return sizeof(uint32_t) * static_cast<uint64_t>(std::max(1u, settings.listCapacity));
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(Uniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
      });
    }

    if (layout_.isValid())
      return;

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelStack", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "spawnClaims", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "lifecycleFeedback", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelOwnerCell", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousGrid", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousList", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelPosR", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelAttachment", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelLifecycle", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instances", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageTable", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pagesBuffer", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "decodedClusterGeometryBuffer", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "decodedVertexBuffer", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "decodedIndexBuffer", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "decodedBoneWeightBuffer", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "meshPartTransforms", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelMaterial", .binding = 23, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelIrradiance", .binding = 24, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousSurfelReservoir", .binding = 28, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentSurfelReservoir", .binding = 29, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousDirectReservoir", .binding = 30, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentDirectReservoir", .binding = 31, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 19, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 20, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsLoTexture", .binding = 21, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "packedGeometryIdsHiTexture", .binding = 22, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "baseColorTexture", .binding = 25, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "normalTexture", .binding = 26, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowLightingTexture", .binding = 27, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = makeReadOnlyBufferView(uniformBuffer_, sizeof(Uniforms))},
                    {.binding = 1, .bufferView = makeStorageBufferView(surfelStackBuffer_, surfelStackBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 2, .bufferView = makeStorageBufferView(surfelClaimGridBuffer_, previousGridBufferSizeBytes(settings_))},
                    {.binding = 3, .bufferView = makeStorageBufferView(surfelRecycleClaimBuffer_, surfelRecycleClaimBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 4, .bufferView = makeStorageBufferView(surfelOwnerCellBuffer_, surfelOwnerCellBufferSize(SurfelRuntimeSettings{
                                                                                                .surfelCapacity = settings_.surfelCapacity,
                                                                                                .lookupCellResolution = settings_.lookupCellResolution,
                                                                                                .gridCapacity = settings_.gridCapacity,
                                                                                            }))},
                    {.binding = 5, .bufferView = makeReadOnlyBufferView(surfelPreviousGridBuffer_, previousGridBufferSizeBytes(settings_))},
                    {.binding = 6, .bufferView = makeReadOnlyBufferView(surfelPreviousListBuffer_, previousListBufferSizeBytes(settings_))},
                    {.binding = 7, .bufferView = makeStorageBufferView(surfelPositionRadiusBuffer_, surfelPositionRadiusBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 8, .bufferView = makeStorageBufferView(surfelNormalLifecycleBuffer_, surfelNormalLifecycleBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 9, .bufferView = makeStorageBufferView(surfelAttachmentBuffer_, surfelAttachmentBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 10, .bufferView = makeStorageBufferView(surfelLifecycleBuffer_, surfelLifecycleBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 11, .bufferView = makeReadOnlyBufferView(scene_.instanceBuffer, scene_.instanceBufferSize)},
                    {.binding = 12, .bufferView = makeReadOnlyBufferView(scene_.pageTableBuffer, scene_.pagesTableBufferSize)},
                    {.binding = 13, .bufferView = makeReadOnlyBufferView(scene_.pagesBuffer, scene_.pagesBufferSize)},
                    {.binding = 14, .bufferView = makeReadOnlyBufferView(scene_.decodedClusterGeometryBuffer, scene_.decodedClusterGeometryBufferSize)},
                    {.binding = 15, .bufferView = makeReadOnlyBufferView(scene_.decodedVertexBuffer, scene_.decodedVertexBufferSize)},
                    {.binding = 16, .bufferView = makeReadOnlyBufferView(scene_.decodedIndexBuffer, scene_.decodedIndexBufferSize)},
                    {.binding = 17, .bufferView = makeReadOnlyBufferView(scene_.decodedBoneWeightBuffer, scene_.decodedBoneWeightBufferSize)},
                    {.binding = 18, .bufferView = makeReadOnlyBufferView(scene_.meshPartTransformsBuffer, scene_.meshPartTransformsBufferSize)},
                    {.binding = 23, .bufferView = makeStorageBufferView(surfelMaterialBuffer_, surfelMaterialBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 24, .bufferView = makeStorageBufferView(surfelIrradianceBuffer_, surfelIrradianceBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 28, .bufferView = makeStorageBufferView(previousSurfelReservoirBuffer_, surfelReservoirBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 29, .bufferView = makeStorageBufferView(currentSurfelReservoirBuffer_, surfelReservoirBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 30, .bufferView = makeStorageBufferView(previousDirectReservoirBuffer_, surfelReservoirBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 31, .bufferView = makeStorageBufferView(currentDirectReservoirBuffer_, surfelReservoirBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                },
                .textures = {
                    {.binding = 19, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 20, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 21, .textureView = makeSampledColorView(packedGeometryIdsLoTexture_)},
                    {.binding = 22, .textureView = makeSampledColorView(packedGeometryIdsHiTexture_)},
                    {.binding = 25, .textureView = makeSampledColorView(baseColorTexture_)},
                    {.binding = 26, .textureView = makeSampledColorView(normalTexture_)},
                    {.binding = 27, .textureView = makeSampledColorView(shadowLightingTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-screen-space-spawn-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection_;
    uniforms.cameraPosition[0] = cameraPosition_.x();
    uniforms.cameraPosition[1] = cameraPosition_.y();
    uniforms.cameraPosition[2] = cameraPosition_.z();
    uniforms.previousCameraPosition[0] = previousCameraPosition_.x();
    uniforms.previousCameraPosition[1] = previousCameraPosition_.y();
    uniforms.previousCameraPosition[2] = previousCameraPosition_.z();
    uniforms.cameraRight[0] = cameraRight_.x();
    uniforms.cameraRight[1] = cameraRight_.y();
    uniforms.cameraRight[2] = cameraRight_.z();
    uniforms.cameraUp[0] = cameraUp_.x();
    uniforms.cameraUp[1] = cameraUp_.y();
    uniforms.cameraUp[2] = cameraUp_.z();
    uniforms.cameraForward[0] = cameraForward_.x();
    uniforms.cameraForward[1] = cameraForward_.y();
    uniforms.cameraForward[2] = cameraForward_.z();
    uniforms.previousCameraRight[0] = previousCameraRight_.x();
    uniforms.previousCameraRight[1] = previousCameraRight_.y();
    uniforms.previousCameraRight[2] = previousCameraRight_.z();
    uniforms.previousCameraUp[0] = previousCameraUp_.x();
    uniforms.previousCameraUp[1] = previousCameraUp_.y();
    uniforms.previousCameraUp[2] = previousCameraUp_.z();
    uniforms.previousCameraForward[0] = previousCameraForward_.x();
    uniforms.previousCameraForward[1] = previousCameraForward_.y();
    uniforms.previousCameraForward[2] = previousCameraForward_.z();
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.surfelCapacity = settings_.surfelCapacity;
    uniforms.surfelAllocationLimit = std::min(settings_.surfelAllocationLimit, settings_.surfelCapacity);
    uniforms.gridCapacity = settings_.gridCapacity;
    uniforms.listCapacity = settings_.listCapacity;
    uniforms.enabled = settings_.enabled;
    uniforms.spawnDownsample = settings_.spawnDownsample;
    uniforms.targetSurfelsPerTile = std::max(1u, settings_.targetSurfelsPerTile);
    uniforms.maxSurfelsPerTile = std::max(uniforms.targetSurfelsPerTile, settings_.maxSurfelsPerTile);
    uniforms.lookupCellResolution = std::max(1u, settings_.lookupCellResolution);
    uniforms.centralGridHalfExtent = settings_.centralGridHalfExtent;
    uniforms.gridScale = settings_.gridScale;
    uniforms.frustumFarDistance = settings_.frustumFarDistance;
    uniforms.targetScreenRadiusPixels = settings_.targetScreenRadiusPixels;
    uniforms.lookupRadiusScale = std::max(settings_.lookupRadiusScale, 0.1f);
    uniforms.worldUnitsPerPixelAtUnitDepth = worldUnitsPerPixelAtUnitDepth_;
    uniforms.spawnMinRadius = settings_.spawnMinRadius;
    uniforms.spawnDepthRadiusScale = settings_.spawnDepthRadiusScale;
    uniforms.recycleCoverageThreshold = settings_.recycleCoverageThreshold;
    uniforms.recycleFreeListPressureBias = settings_.recycleFreeListPressureBias;
    uniforms.recycleMaxAgeFrames = settings_.recycleMaxAgeFrames;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  virtualgeometry::VirtualGeometryScene &scene_;
  rendering::Buffer surfelStackBuffer_{};
  rendering::Buffer surfelClaimGridBuffer_{};
  rendering::Buffer surfelRecycleClaimBuffer_{};
  rendering::Buffer surfelOwnerCellBuffer_{};
  rendering::Buffer surfelPreviousGridBuffer_{};
  rendering::Buffer surfelPreviousListBuffer_{};
  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelAttachmentBuffer_{};
  rendering::Buffer surfelLifecycleBuffer_{};
  rendering::Buffer surfelMaterialBuffer_{};
  rendering::Buffer surfelIrradianceBuffer_{};
  rendering::Buffer previousSurfelReservoirBuffer_{};
  rendering::Buffer currentSurfelReservoirBuffer_{};
  rendering::Buffer previousDirectReservoirBuffer_{};
  rendering::Buffer currentDirectReservoirBuffer_{};
  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture shadowLightingTexture_{};
  rendering::Texture baseColorTexture_{};
  rendering::Texture normalTexture_{};
  rendering::Texture packedGeometryIdsLoTexture_{};
  rendering::Texture packedGeometryIdsHiTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f previousCameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f cameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f cameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f cameraForward_{0.0f, 0.0f, -1.0f};
  math::Vec3f previousCameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f previousCameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f previousCameraForward_{0.0f, 0.0f, -1.0f};
  float worldUnitsPerPixelAtUnitDepth_ = 0.001f;
  bool reverseZ_ = false;
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
