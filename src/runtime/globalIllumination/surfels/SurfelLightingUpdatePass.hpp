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

class SurfelLightingUpdatePass : public rendering::Pass
{
public:
  struct Settings
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    uint32_t viewPortWidth = 1u;
    uint32_t viewPortHeight = 1u;
    uint32_t frameIndex = 0u;
    uint32_t surfelCapacity = 1u;
    uint32_t traceHitOffset = 0u;
    uint32_t updatePeriodFrames = 8u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t lookupCellResolution = 1u;
    bool reverseZ = true;
    std::array<float, 3> cameraPosition{0.0f, 0.0f, 0.0f};
    float centralGridHalfExtent = 0.2f;
    float frustumFarDistance = 4096.0f;
    float hysteresis = 0.9f;
    float irradianceDecay = 0.995f;
    float normalBias = 0.02f;
    float viewBias = 0.05f;
    float energyPreservation = 0.95f;
    bool directionalShadowEnabled = true;
    bool directionalShadowReverseZ = true;
    uint32_t directionalShadowPageTableResolution = 0u;
    uint32_t directionalShadowActiveLayers = 0u;
    uint32_t directionalShadowActiveLights = 0u;
    uint32_t directionalShadowPhysicalPageSize = 0u;
    uint32_t directionalShadowCascadeCount = 0u;
    uint32_t directionalShadowFallbackCascadeOffset = 0u;
    float directionalShadowFirstCascadeWorldExtent = 0.0f;
    float directionalShadowBias = 0.0015f;
    float directionalShadowMaxBias = 0.006f;
    float directionalShadowPcfRadiusTexels = 1.5f;
    float directionalShadowNormalBiasTexels = 0.5f;
    uint32_t directionalShadowFilterTaps = 1u;
    std::array<float, 3> directionalLightDirection{0.0f, -1.0f, 0.0f};
    float directionalLightIntensity = 1.0f;
    std::array<float, 3> directionalLightColor{1.0f, 1.0f, 1.0f};
    float defaultDiffuseAlbedo = 0.35f;
    std::array<float, 3> pointLightPosition{0.0f, 0.0f, 0.0f};
    float pointLightRange = 1.0f;
    std::array<float, 3> pointLightColor{1.0f, 1.0f, 1.0f};
    float pointLightIntensity = 0.0f;
  };

  SurfelLightingUpdatePass(
      rendering::Buffer surfelGridBuffer,
      rendering::Buffer surfelListBuffer,
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelMaterialBuffer,
      rendering::Buffer surfelIrradianceBuffer,
      rendering::Buffer previousSurfelReservoirBuffer,
      rendering::Buffer currentSurfelReservoirBuffer,
      rendering::Buffer currentSurfelDirectReservoirBuffer,
      rendering::Buffer traceHitBuffer,
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Texture shadowLightingTexture,
      rendering::Buffer directionalLightsBuffer,
      uint64_t directionalLightsBufferSize,
      rendering::Buffer directionalShadowCascadeStatesBuffer,
      uint64_t directionalShadowCascadeStatesBufferSize,
      rendering::Buffer directionalShadowCascadeMatricesBuffer,
      uint64_t directionalShadowCascadeMatricesBufferSize,
      rendering::Buffer directionalShadowCameraStateBuffer,
      uint64_t directionalShadowCameraStateBufferSize,
      rendering::Buffer directionalShadowVirtualPageTableBuffer,
      uint64_t directionalShadowVirtualPageTableBufferSize,
      rendering::Texture directionalShadowAtlasTexture,
      Settings settings)
      : surfelGridBuffer_(surfelGridBuffer),
        surfelListBuffer_(surfelListBuffer),
        surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelMaterialBuffer_(surfelMaterialBuffer),
        surfelIrradianceBuffer_(surfelIrradianceBuffer),
        previousSurfelReservoirBuffer_(previousSurfelReservoirBuffer),
        currentSurfelReservoirBuffer_(currentSurfelReservoirBuffer),
        currentSurfelDirectReservoirBuffer_(currentSurfelDirectReservoirBuffer),
        traceHitBuffer_(traceHitBuffer),
        sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        shadowLightingTexture_(shadowLightingTexture),
        directionalLightsBuffer_(directionalLightsBuffer),
        directionalLightsBufferSize_(directionalLightsBufferSize),
        directionalShadowCascadeStatesBuffer_(directionalShadowCascadeStatesBuffer),
        directionalShadowCascadeStatesBufferSize_(directionalShadowCascadeStatesBufferSize),
        directionalShadowCascadeMatricesBuffer_(directionalShadowCascadeMatricesBuffer),
        directionalShadowCascadeMatricesBufferSize_(directionalShadowCascadeMatricesBufferSize),
        directionalShadowCameraStateBuffer_(directionalShadowCameraStateBuffer),
        directionalShadowCameraStateBufferSize_(directionalShadowCameraStateBufferSize),
        directionalShadowVirtualPageTableBuffer_(directionalShadowVirtualPageTableBuffer),
        directionalShadowVirtualPageTableBufferSize_(directionalShadowVirtualPageTableBufferSize),
        directionalShadowAtlasTexture_(directionalShadowAtlasTexture),
        settings_(settings)
  {
  }

  ~SurfelLightingUpdatePass() override
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
    if (directionalShadowUniformBuffer_.isValid())
      renderGraph->deleteBuffer(directionalShadowUniformBuffer_);
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
    uint32_t traceHitOffset = 0u;
    uint32_t updatePeriodFrames = 8u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t lookupCellResolution = 1u;
    uint32_t _padding0 = 0u;
    std::array<float, 4> cameraPositionAndHalfExtent{};
    float hysteresis = 0.9f;
    float irradianceDecay = 0.995f;
    float frustumFarDistance = 4096.0f;
    float normalBias = 0.02f;
    float viewBias = 0.05f;
    float energyPreservation = 0.95f;
    float _padding1[3]{};
    std::array<float, 4> directionalLightDirectionAndIntensity{};
    std::array<float, 4> directionalLightColorAndAlbedo{};
    std::array<float, 4> pointLightPositionAndRange{};
    std::array<float, 4> pointLightColorAndIntensity{};
  };

  struct DirectionalShadowUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t activeLayers = 0u;
    uint32_t cascadeCount = 0u;
    uint32_t activeLights = 0u;
    uint32_t enabled = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t reverseZ = 0u;
    uint32_t fallbackCascadeOffset = 0u;
    float firstCascadeWorldExtent = 0.0f;
    float directionalShadowBias = 0.0015f;
    float directionalMaxShadowBias = 0.006f;
    float pcfRadiusTexels = 1.5f;
    float directionalNormalBiasTexels = 0.5f;
    uint32_t shadowFilterTaps = 1u;
    uint32_t _padding0[2]{};
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
    if (!directionalShadowUniformBuffer_.isValid())
    {
      directionalShadowUniformBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_DirectionalShadowUniforms.buffer",
              .size = sizeof(DirectionalShadowUniforms),
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
                    {.name = "surfelGrid", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelList", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelPosR", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelMaterial", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelIrradiance", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "previousSurfelReservoir", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentSurfelReservoir", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentSurfelDirectReservoir", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "traceHits", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalLights", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowUniforms", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowCascadeStates", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowCascadeMatrices", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowCameraState", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowVirtualPageTable", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 17, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 18, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowLightingTexture", .binding = 19, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "directionalShadowAtlasTexture", .binding = 20, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeReadOnlyBufferView(surfelGridBuffer_, renderGraph->getBufferSize(surfelGridBuffer_))},
                    {.binding = 2, .bufferView = makeReadOnlyBufferView(surfelListBuffer_, renderGraph->getBufferSize(surfelListBuffer_))},
                    {.binding = 3, .bufferView = makeReadOnlyBufferView(surfelPositionRadiusBuffer_, renderGraph->getBufferSize(surfelPositionRadiusBuffer_))},
                    {.binding = 4, .bufferView = makeReadOnlyBufferView(surfelNormalLifecycleBuffer_, renderGraph->getBufferSize(surfelNormalLifecycleBuffer_))},
                    {.binding = 5, .bufferView = makeReadOnlyBufferView(surfelMaterialBuffer_, renderGraph->getBufferSize(surfelMaterialBuffer_))},
                    {.binding = 6, .bufferView = makeStorageBufferView(surfelIrradianceBuffer_, renderGraph->getBufferSize(surfelIrradianceBuffer_))},
                    {.binding = 7, .bufferView = makeReadOnlyBufferView(previousSurfelReservoirBuffer_, renderGraph->getBufferSize(previousSurfelReservoirBuffer_))},
                    {.binding = 8, .bufferView = makeStorageBufferView(currentSurfelReservoirBuffer_, renderGraph->getBufferSize(currentSurfelReservoirBuffer_))},
                    {.binding = 9, .bufferView = makeReadOnlyBufferView(currentSurfelDirectReservoirBuffer_, renderGraph->getBufferSize(currentSurfelDirectReservoirBuffer_))},
                    {.binding = 10, .bufferView = makeReadOnlyBufferView(traceHitBuffer_, renderGraph->getBufferSize(traceHitBuffer_))},
                    {.binding = 11, .bufferView = makeReadOnlyBufferView(directionalLightsBuffer_, std::max<uint64_t>(directionalLightsBufferSize_, sizeof(virtualgeometry::VirtualShadowMapManager::DirectionalLightGPU)))},
                    {.binding = 12, .bufferView = makeReadOnlyBufferView(directionalShadowUniformBuffer_, sizeof(DirectionalShadowUniforms))},
                    {.binding = 13, .bufferView = makeReadOnlyBufferView(directionalShadowCascadeStatesBuffer_, std::max<uint64_t>(directionalShadowCascadeStatesBufferSize_, sizeof(virtualgeometry::VirtualShadowMapManager::CascadeStateGPU)))},
                    {.binding = 14, .bufferView = makeReadOnlyBufferView(directionalShadowCascadeMatricesBuffer_, std::max<uint64_t>(directionalShadowCascadeMatricesBufferSize_, sizeof(virtualgeometry::VirtualShadowMapManager::CascadeMatrixGPU)))},
                    {.binding = 15, .bufferView = makeReadOnlyBufferView(directionalShadowCameraStateBuffer_, std::max<uint64_t>(directionalShadowCameraStateBufferSize_, sizeof(virtualgeometry::VirtualShadowMapManager::CameraStateGPU)))},
                    {.binding = 16, .bufferView = makeReadOnlyBufferView(directionalShadowVirtualPageTableBuffer_, directionalShadowVirtualPageTableBufferSize_)},
                },
                .textures = {
                    {.binding = 17, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 18, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 19, .textureView = makeSampledColorView(shadowLightingTexture_)},
                    {.binding = 20, .textureView = makeSampledDepthView(directionalShadowAtlasTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-lighting-update-cs.spirv"),
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
    uniforms.traceHitOffset = settings_.traceHitOffset;
    uniforms.updatePeriodFrames = std::max(1u, settings_.updatePeriodFrames);
    uniforms.gridCapacity = settings_.gridCapacity;
    uniforms.listCapacity = settings_.listCapacity;
    uniforms.lookupCellResolution = settings_.lookupCellResolution;
    uniforms.cameraPositionAndHalfExtent = {settings_.cameraPosition[0], settings_.cameraPosition[1], settings_.cameraPosition[2], settings_.centralGridHalfExtent};
    uniforms.hysteresis = settings_.hysteresis;
    uniforms.irradianceDecay = settings_.irradianceDecay;
    uniforms.frustumFarDistance = settings_.frustumFarDistance;
    uniforms.normalBias = settings_.normalBias;
    uniforms.viewBias = settings_.viewBias;
    uniforms.energyPreservation = settings_.energyPreservation;
    uniforms.directionalLightDirectionAndIntensity = {settings_.directionalLightDirection[0], settings_.directionalLightDirection[1], settings_.directionalLightDirection[2], settings_.directionalLightIntensity};
    uniforms.directionalLightColorAndAlbedo = {settings_.directionalLightColor[0], settings_.directionalLightColor[1], settings_.directionalLightColor[2], settings_.defaultDiffuseAlbedo};
    uniforms.pointLightPositionAndRange = {settings_.pointLightPosition[0], settings_.pointLightPosition[1], settings_.pointLightPosition[2], settings_.pointLightRange};
    uniforms.pointLightColorAndIntensity = {settings_.pointLightColor[0], settings_.pointLightColor[1], settings_.pointLightColor[2], settings_.pointLightIntensity};
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);

    DirectionalShadowUniforms shadowUniforms{};
    shadowUniforms.pageTableResolution = settings_.directionalShadowPageTableResolution;
    shadowUniforms.activeLayers = settings_.directionalShadowActiveLayers;
    shadowUniforms.cascadeCount = settings_.directionalShadowCascadeCount;
    shadowUniforms.activeLights = settings_.directionalShadowActiveLights;
    shadowUniforms.enabled = settings_.directionalShadowEnabled ? 1u : 0u;
    shadowUniforms.physicalPageSize = settings_.directionalShadowPhysicalPageSize;
    shadowUniforms.reverseZ = settings_.directionalShadowReverseZ ? 1u : 0u;
    shadowUniforms.fallbackCascadeOffset = settings_.directionalShadowFallbackCascadeOffset;
    shadowUniforms.firstCascadeWorldExtent = settings_.directionalShadowFirstCascadeWorldExtent;
    shadowUniforms.directionalShadowBias = settings_.directionalShadowBias;
    shadowUniforms.directionalMaxShadowBias = settings_.directionalShadowMaxBias;
    shadowUniforms.pcfRadiusTexels = settings_.directionalShadowPcfRadiusTexels;
    shadowUniforms.directionalNormalBiasTexels = settings_.directionalShadowNormalBiasTexels;
    shadowUniforms.shadowFilterTaps = settings_.directionalShadowFilterTaps;
    renderGraph->bufferWrite(directionalShadowUniformBuffer_, 0u, sizeof(DirectionalShadowUniforms), &shadowUniforms);
  }

  rendering::Buffer surfelGridBuffer_{};
  rendering::Buffer surfelListBuffer_{};
  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelMaterialBuffer_{};
  rendering::Buffer surfelIrradianceBuffer_{};
  rendering::Buffer previousSurfelReservoirBuffer_{};
  rendering::Buffer currentSurfelReservoirBuffer_{};
  rendering::Buffer currentSurfelDirectReservoirBuffer_{};
  rendering::Buffer traceHitBuffer_{};
  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture shadowLightingTexture_{};
  rendering::Buffer directionalLightsBuffer_{};
  uint64_t directionalLightsBufferSize_ = 0u;
  rendering::Buffer directionalShadowCascadeStatesBuffer_{};
  uint64_t directionalShadowCascadeStatesBufferSize_ = 0u;
  rendering::Buffer directionalShadowCascadeMatricesBuffer_{};
  uint64_t directionalShadowCascadeMatricesBufferSize_ = 0u;
  rendering::Buffer directionalShadowCameraStateBuffer_{};
  uint64_t directionalShadowCameraStateBufferSize_ = 0u;
  rendering::Buffer directionalShadowVirtualPageTableBuffer_{};
  uint64_t directionalShadowVirtualPageTableBufferSize_ = 0u;
  rendering::Texture directionalShadowAtlasTexture_{};
  Settings settings_{};
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  rendering::Buffer directionalShadowUniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
