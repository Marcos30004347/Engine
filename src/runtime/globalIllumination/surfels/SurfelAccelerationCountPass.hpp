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

class SurfelAccelerationCountPass : public rendering::Pass
{
public:
  using Settings = SurfelRuntimeSettings;

  SurfelAccelerationCountPass(
      virtualgeometry::VirtualGeometryScene &scene,
      rendering::Buffer surfelStackBuffer,
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelAttachmentBuffer,
      rendering::Buffer surfelLifecycleBuffer,
      rendering::Buffer surfelLifecycleFeedbackBuffer,
      rendering::Buffer surfelOwnerCellBuffer,
      rendering::Buffer surfelGridBuffer,
      rendering::Buffer surfelPreviousGridBuffer,
      Settings settings)
      : scene_(scene),
        surfelStackBuffer_(surfelStackBuffer),
        surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelAttachmentBuffer_(surfelAttachmentBuffer),
        surfelLifecycleBuffer_(surfelLifecycleBuffer),
        surfelLifecycleFeedbackBuffer_(surfelLifecycleFeedbackBuffer),
        surfelOwnerCellBuffer_(surfelOwnerCellBuffer),
        surfelGridBuffer_(surfelGridBuffer),
        surfelPreviousGridBuffer_(surfelPreviousGridBuffer),
        settings_(settings)
  {
  }

  ~SurfelAccelerationCountPass() override
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

  void setOwnerResolveOnly(bool ownerResolveOnly)
  {
    ownerResolveOnly_ = ownerResolveOnly;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void updateUniforms(
      uint32_t frameIndex,
      const math::Vec3f &cameraPosition,
      const math::Vec3f &previousCameraPosition,
      const math::Vec3f &cameraRight,
      const math::Vec3f &cameraUp,
      const math::Vec3f &cameraForward,
      uint32_t viewportWidth,
      uint32_t viewportHeight,
      float worldUnitsPerPixelAtUnitDepth)
  {
    frameIndex_ = frameIndex;
    cameraPosition_ = cameraPosition;
    previousCameraPosition_ = previousCameraPosition;
    cameraRight_ = cameraRight;
    cameraUp_ = cameraUp;
    cameraForward_ = cameraForward;
    viewportWidth_ = viewportWidth;
    viewportHeight_ = viewportHeight;
    worldUnitsPerPixelAtUnitDepth_ = worldUnitsPerPixelAtUnitDepth;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.surfelCapacity) + 127u) / 128u, 1u, 1u);
  }

private:
  struct Uniforms
  {
    float cameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float previousCameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float cameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    uint32_t frameIndex = 0u;
    uint32_t surfelCapacity = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t lookupCellResolution = 16u;
    uint32_t targetSurfelsPerTile = 1u;
    uint32_t maxSurfelsPerTile = 3u;
    uint32_t viewport[2]{1u, 1u};
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
    uint32_t visibleRecycleMaxAgeFrames = 120u;
    uint32_t hiddenRecycleMaxAgeFrames = 360u;
    uint32_t recycleMaxAgeFrames = 240u;
    uint32_t visibleUntouchedAgeStep = 8u;
    uint32_t hiddenUntouchedAgeStep = 2u;
    uint32_t denseSurfelsAgeStep = 24u;
    float hiddenDistanceThresholdScale = 1.0f;
    float hiddenDensityThresholdScale = 1.0f;
    float hiddenDensityAgeScale = 1.0f;
    float radiusUpdateRate = 0.25f;
    float undersizedAgeBias = 0.5f;
    float oversizedAgeBias = 1.5f;
    uint32_t ownerResolveOnly = 0u;
    uint32_t ownerSelectionMode = 0u;
    uint32_t _padding1[2]{};
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
                    {.name = "surfelPosR", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelAttachment", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelLifecycle", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelLifecycleFeedback", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelOwnerCell", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelGrid", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousGrid", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instances", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "meshPartTransforms", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeStorageBufferView(surfelStackBuffer_, surfelStackBufferSize(settings_))},
                    {.binding = 2, .bufferView = makeStorageBufferView(surfelPositionRadiusBuffer_, surfelPositionRadiusBufferSize(settings_))},
                    {.binding = 3, .bufferView = makeStorageBufferView(surfelNormalLifecycleBuffer_, surfelNormalLifecycleBufferSize(settings_))},
                    {.binding = 4, .bufferView = makeStorageBufferView(surfelAttachmentBuffer_, surfelAttachmentBufferSize(settings_))},
                    {.binding = 5, .bufferView = makeStorageBufferView(surfelLifecycleBuffer_, surfelLifecycleBufferSize(settings_))},
                    {.binding = 6, .bufferView = makeStorageBufferView(surfelLifecycleFeedbackBuffer_, surfelRecycleClaimBufferSize(settings_))},
                    {.binding = 7, .bufferView = makeStorageBufferView(surfelOwnerCellBuffer_, surfelOwnerCellBufferSize(settings_))},
                    {.binding = 8, .bufferView = makeStorageBufferView(surfelGridBuffer_, surfelGridBufferSize(settings_))},
                    {.binding = 9, .bufferView = makeReadOnlyBufferView(surfelPreviousGridBuffer_, surfelGridBufferSize(settings_))},
                    {.binding = 10, .bufferView = makeReadOnlyBufferView(scene_.instanceBuffer, scene_.instanceBufferSize)},
                    {.binding = 11, .bufferView = makeReadOnlyBufferView(scene_.meshPartTransformsBuffer, scene_.meshPartTransformsBufferSize)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-acceleration-count-cs.spirv"),
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
    uniforms.frameIndex = frameIndex_;
    uniforms.surfelCapacity = std::max(1u, settings_.surfelCapacity);
    uniforms.gridCapacity = std::max(1u, settings_.gridCapacity);
    uniforms.lookupCellResolution = std::max(1u, settings_.lookupCellResolution);
    uniforms.targetSurfelsPerTile = std::max(1u, settings_.targetSurfelsPerTile);
    uniforms.maxSurfelsPerTile = std::max(uniforms.targetSurfelsPerTile, settings_.maxSurfelsPerTile);
    uniforms.viewport[0] = std::max(1u, viewportWidth_);
    uniforms.viewport[1] = std::max(1u, viewportHeight_);
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
    uniforms.visibleRecycleMaxAgeFrames = settings_.visibleRecycleMaxAgeFrames;
    uniforms.hiddenRecycleMaxAgeFrames = settings_.hiddenRecycleMaxAgeFrames;
    uniforms.recycleMaxAgeFrames = settings_.recycleMaxAgeFrames;
    uniforms.visibleUntouchedAgeStep = settings_.visibleUntouchedAgeStep;
    uniforms.hiddenUntouchedAgeStep = settings_.hiddenUntouchedAgeStep;
    uniforms.denseSurfelsAgeStep = settings_.denseSurfelsAgeStep;
    uniforms.hiddenDistanceThresholdScale = std::max(settings_.hiddenDistanceThresholdScale, 0.0f);
    uniforms.hiddenDensityThresholdScale = std::max(settings_.hiddenDensityThresholdScale, 0.1f);
    uniforms.hiddenDensityAgeScale = std::max(settings_.hiddenDensityAgeScale, 0.0f);
    uniforms.radiusUpdateRate = settings_.radiusUpdateRate;
    uniforms.undersizedAgeBias = settings_.undersizedAgeBias;
    uniforms.oversizedAgeBias = settings_.oversizedAgeBias;
    uniforms.ownerResolveOnly = ownerResolveOnly_ ? 1u : 0u;
    uniforms.ownerSelectionMode = settings_.ownerSelectionMode;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  virtualgeometry::VirtualGeometryScene &scene_;
  rendering::Buffer surfelStackBuffer_{};
  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelAttachmentBuffer_{};
  rendering::Buffer surfelLifecycleBuffer_{};
  rendering::Buffer surfelLifecycleFeedbackBuffer_{};
  rendering::Buffer surfelOwnerCellBuffer_{};
  rendering::Buffer surfelGridBuffer_{};
  rendering::Buffer surfelPreviousGridBuffer_{};
  Settings settings_{};
  math::Vec3f cameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f previousCameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f cameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f cameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f cameraForward_{0.0f, 0.0f, -1.0f};
  uint32_t frameIndex_ = 0u;
  uint32_t viewportWidth_ = 1u;
  uint32_t viewportHeight_ = 1u;
  float worldUnitsPerPixelAtUnitDepth_ = 0.001f;
  bool ownerResolveOnly_ = false;
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
