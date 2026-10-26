#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelTracePreparePass : public rendering::Pass
{
public:
  struct Settings
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    uint32_t viewPortWidth = 1u;
    uint32_t viewPortHeight = 1u;
    uint32_t surfelCapacity = 1u;
    uint32_t rayOffset = 0u;
    uint32_t frameIndex = 0u;
    uint32_t updatePeriodFrames = 8u;
    uint32_t activeLightCount = 0u;
    bool reverseZ = true;
    float maxTraceDistance = 3000.0f;
    float normalBias = 0.02f;
    float viewBias = 0.05f;
    std::array<float, 3> directionalLightDirection{0.0f, -1.0f, 0.0f};
    float directionalLightIntensity = 1.0f;
    std::array<float, 3> directionalLightColor{1.0f, 1.0f, 1.0f};
    std::array<float, 3> pointLightPosition{0.0f, 0.0f, 0.0f};
    float pointLightRange = 1.0f;
    std::array<float, 3> pointLightColor{1.0f, 1.0f, 1.0f};
    float pointLightIntensity = 0.0f;
  };

  SurfelTracePreparePass(
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelMaterialBuffer,
      rendering::Buffer previousDirectReservoirBuffer,
      rendering::Buffer currentDirectReservoirBuffer,
      rendering::Buffer directionalLightsBuffer,
      uint64_t directionalLightsBufferSize,
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Buffer traceRayBuffer,
      Settings settings)
      : surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelMaterialBuffer_(surfelMaterialBuffer),
        previousDirectReservoirBuffer_(previousDirectReservoirBuffer),
        currentDirectReservoirBuffer_(currentDirectReservoirBuffer),
        directionalLightsBuffer_(directionalLightsBuffer),
        directionalLightsBufferSize_(directionalLightsBufferSize),
        sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        traceRayBuffer_(traceRayBuffer),
        settings_(settings)
  {
  }

  ~SurfelTracePreparePass() override
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

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.surfelCapacity) + 63u) / 64u, 1u, 1u);
  }

private:
  struct Uniforms
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    uint32_t viewport[2]{1u, 1u};
    uint32_t frameIndex = 0u;
    uint32_t reverseZ = 1u;
    uint32_t surfelCapacity = 1u;
    uint32_t rayOffset = 0u;
    uint32_t updatePeriodFrames = 8u;
    uint32_t activeLightCount = 0u;
    float maxTraceDistance = 3000.0f;
    float normalBias = 0.02f;
    float viewBias = 0.05f;
    float directionalLightDirection[3]{0.0f, -1.0f, 0.0f};
    float directionalLightIntensity = 1.0f;
    float directionalLightColor[3]{1.0f, 1.0f, 1.0f};
    float _padding0 = 0.0f;
    float pointLightPosition[3]{0.0f, 0.0f, 0.0f};
    float pointLightRange = 1.0f;
    float pointLightColor[3]{1.0f, 1.0f, 1.0f};
    float pointLightIntensity = 0.0f;
    float _padding1[3]{};
  };

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
    };
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
                    {.name = "surfelPosR", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelMaterial", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousDirectReservoir", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentDirectReservoir", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalLights", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "traceRays", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeReadOnlyBufferView(surfelPositionRadiusBuffer_, renderGraph->getBufferSize(surfelPositionRadiusBuffer_))},
                    {.binding = 2, .bufferView = makeReadOnlyBufferView(surfelNormalLifecycleBuffer_, renderGraph->getBufferSize(surfelNormalLifecycleBuffer_))},
                    {.binding = 3, .bufferView = makeReadOnlyBufferView(surfelMaterialBuffer_, renderGraph->getBufferSize(surfelMaterialBuffer_))},
                    {.binding = 4, .bufferView = makeReadOnlyBufferView(previousDirectReservoirBuffer_, renderGraph->getBufferSize(previousDirectReservoirBuffer_))},
                    {.binding = 5, .bufferView = makeStorageBufferView(currentDirectReservoirBuffer_, renderGraph->getBufferSize(currentDirectReservoirBuffer_))},
                    {.binding = 6, .bufferView = makeReadOnlyBufferView(directionalLightsBuffer_, std::max<uint64_t>(directionalLightsBufferSize_, sizeof(virtualgeometry::VirtualShadowMapManager::DirectionalLightGPU)))},
                    {.binding = 7, .bufferView = makeStorageBufferView(traceRayBuffer_, renderGraph->getBufferSize(traceRayBuffer_))},
                },
                .textures = {
                    {.binding = 8, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 9, .textureView = makeSampledColorView(geometryNormalTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-trace-prepare-cs.spirv"),
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
    uniforms.viewProjection = settings_.viewProjection;
    uniforms.inverseViewProjection = settings_.inverseViewProjection;
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.reverseZ = settings_.reverseZ ? 1u : 0u;
    uniforms.surfelCapacity = settings_.surfelCapacity;
    uniforms.rayOffset = settings_.rayOffset;
    uniforms.updatePeriodFrames = std::max(1u, settings_.updatePeriodFrames);
    uniforms.activeLightCount = settings_.activeLightCount;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.normalBias = settings_.normalBias;
    uniforms.viewBias = settings_.viewBias;
    std::copy(settings_.directionalLightDirection.begin(), settings_.directionalLightDirection.end(), uniforms.directionalLightDirection);
    uniforms.directionalLightIntensity = settings_.directionalLightIntensity;
    std::copy(settings_.directionalLightColor.begin(), settings_.directionalLightColor.end(), uniforms.directionalLightColor);
    std::copy(settings_.pointLightPosition.begin(), settings_.pointLightPosition.end(), uniforms.pointLightPosition);
    uniforms.pointLightRange = settings_.pointLightRange;
    std::copy(settings_.pointLightColor.begin(), settings_.pointLightColor.end(), uniforms.pointLightColor);
    uniforms.pointLightIntensity = settings_.pointLightIntensity;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelMaterialBuffer_{};
  rendering::Buffer previousDirectReservoirBuffer_{};
  rendering::Buffer currentDirectReservoirBuffer_{};
  rendering::Buffer directionalLightsBuffer_{};
  uint64_t directionalLightsBufferSize_ = 0u;
  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Buffer traceRayBuffer_{};
  Settings settings_{};
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
