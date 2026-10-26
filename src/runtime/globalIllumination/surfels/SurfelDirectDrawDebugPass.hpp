#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelDirectDrawDebugPass : public rendering::Pass
{
public:
  enum class DebugMode : uint32_t
  {
    Coverage = 0u,
    Albedo = 1u,
    Normal = 2u,
    Irradiance = 3u,
  };

  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t surfelCapacity = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    float lookupRadiusScale = 1.0f;
    float spawnMinRadius = 0.05f;
    float spawnDepthRadiusScale = 0.01f;
    uint32_t debugMode = static_cast<uint32_t>(DebugMode::Coverage);
  };

  SurfelDirectDrawDebugPass(
      rendering::Buffer surfelGridBuffer,
      rendering::Buffer surfelListBuffer,
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelMaterialBuffer,
      rendering::Buffer surfelIrradianceBuffer,
      rendering::Texture sceneDepthTexture,
      rendering::Texture geometryNormalTexture,
      rendering::Texture sourceColorTexture,
      rendering::Texture outputDebugTexture,
      Settings settings)
      : surfelGridBuffer_(surfelGridBuffer),
        surfelListBuffer_(surfelListBuffer),
        surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelMaterialBuffer_(surfelMaterialBuffer),
        surfelIrradianceBuffer_(surfelIrradianceBuffer),
        sceneDepthTexture_(sceneDepthTexture),
        geometryNormalTexture_(geometryNormalTexture),
        sourceColorTexture_(sourceColorTexture),
        outputDebugTexture_(outputDebugTexture),
        settings_(settings)
  {
  }

  ~SurfelDirectDrawDebugPass() override
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

  void updateUniforms(const math::Mat4f &inverseViewProjection, const math::Vec3f &cameraPosition, const math::Vec3f &cameraRight, const math::Vec3f &cameraUp, const math::Vec3f &cameraForward, bool reverseZ)
  {
    inverseViewProjection_ = inverseViewProjection;
    cameraPosition_ = cameraPosition;
    cameraRight_ = cameraRight;
    cameraUp_ = cameraUp;
    cameraForward_ = cameraForward;
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

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.viewPortWidth + 7u) / 8u, (settings_.viewPortHeight + 7u) / 8u, 1u);
  }

private:
  struct Uniforms
  {
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    float cameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float cameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    uint32_t viewport[2]{1u, 1u};
    uint32_t reverseZ = 0u;
    uint32_t surfelCapacity = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    float lookupRadiusScale = 1.0f;
    float spawnMinRadius = 0.05f;
    float spawnDepthRadiusScale = 0.01f;
    uint32_t debugMode = static_cast<uint32_t>(DebugMode::Coverage);
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

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
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
                    {.name = "surfelGrid", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelList", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelPosR", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelMaterial", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelIrradiance", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "geometryNormalTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sourceColorTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputDebugTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeReadOnlyBufferView(surfelGridBuffer_, surfelGridBufferSize(SurfelRuntimeSettings{.gridCapacity = settings_.gridCapacity}))},
                    {.binding = 2, .bufferView = makeReadOnlyBufferView(surfelListBuffer_, sizeof(uint32_t) * static_cast<uint64_t>(std::max(1u, settings_.listCapacity)))},
                    {.binding = 3, .bufferView = makeReadOnlyBufferView(surfelPositionRadiusBuffer_, surfelPositionRadiusBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 4, .bufferView = makeReadOnlyBufferView(surfelNormalLifecycleBuffer_, surfelNormalLifecycleBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 5, .bufferView = makeReadOnlyBufferView(surfelMaterialBuffer_, surfelMaterialBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                    {.binding = 6, .bufferView = makeReadOnlyBufferView(surfelIrradianceBuffer_, surfelIrradianceBufferSize(SurfelRuntimeSettings{.surfelCapacity = settings_.surfelCapacity}))},
                },
                .textures = {
                    {.binding = 7, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 8, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 9, .textureView = makeSampledColorView(sourceColorTexture_)},
                },
                .storageTextures = {
                    {.binding = 10, .textureView = makeStorageColorView(outputDebugTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-direct-draw-debug-cs.spirv"),
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
    uniforms.cameraRight[0] = cameraRight_.x();
    uniforms.cameraRight[1] = cameraRight_.y();
    uniforms.cameraRight[2] = cameraRight_.z();
    uniforms.cameraUp[0] = cameraUp_.x();
    uniforms.cameraUp[1] = cameraUp_.y();
    uniforms.cameraUp[2] = cameraUp_.z();
    uniforms.cameraForward[0] = cameraForward_.x();
    uniforms.cameraForward[1] = cameraForward_.y();
    uniforms.cameraForward[2] = cameraForward_.z();
    uniforms.viewport[0] = std::max(1u, settings_.viewPortWidth);
    uniforms.viewport[1] = std::max(1u, settings_.viewPortHeight);
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.surfelCapacity = std::max(1u, settings_.surfelCapacity);
    uniforms.gridCapacity = std::max(1u, settings_.gridCapacity);
    uniforms.listCapacity = std::max(1u, settings_.listCapacity);
    uniforms.lookupCellResolution = std::max(1u, settings_.lookupCellResolution);
    uniforms.centralGridHalfExtent = settings_.centralGridHalfExtent;
    uniforms.gridScale = settings_.gridScale;
    uniforms.frustumFarDistance = settings_.frustumFarDistance;
    uniforms.lookupRadiusScale = settings_.lookupRadiusScale;
    uniforms.spawnMinRadius = settings_.spawnMinRadius;
    uniforms.spawnDepthRadiusScale = settings_.spawnDepthRadiusScale;
    uniforms.debugMode = settings_.debugMode;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  rendering::Buffer surfelGridBuffer_{};
  rendering::Buffer surfelListBuffer_{};
  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelMaterialBuffer_{};
  rendering::Buffer surfelIrradianceBuffer_{};
  rendering::Texture sceneDepthTexture_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture sourceColorTexture_{};
  rendering::Texture outputDebugTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f cameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f cameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f cameraForward_{0.0f, 0.0f, -1.0f};
  bool reverseZ_ = true;
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
