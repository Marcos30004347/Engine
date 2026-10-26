#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/TraceShadingTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTracePass.hpp"

namespace globalillumination
{

class ScreenSpaceHitShadingResolvePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t hitCapacity = 1u;
    bool enabled = true;
  };

  ScreenSpaceHitShadingResolvePass(
      rendering::Buffer screenHitsBuffer,
      rendering::Texture geometryNormalTexture,
      rendering::Texture baseColorTexture,
      rendering::Texture normalTexture,
      rendering::Texture materialIdTexture,
      rendering::Texture materialUVTexture,
      rendering::Buffer traceShadingBuffer,
      Settings settings)
      : screenHitsBuffer_(screenHitsBuffer),
        geometryNormalTexture_(geometryNormalTexture),
        baseColorTexture_(baseColorTexture),
        normalTexture_(normalTexture),
        materialIdTexture_(materialIdTexture),
        materialUVTexture_(materialUVTexture),
        traceShadingBuffer_(traceShadingBuffer),
        maxHitCapacity_(std::max(1u, settings.hitCapacity)),
        settings_(sanitizeSettings(settings, maxHitCapacity_))
  {
  }

  ~ScreenSpaceHitShadingResolvePass() override
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
    settings_ = sanitizeSettings(settings, maxHitCapacity_);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!settings_.enabled || settings_.hitCapacity == 0u)
      return;

    ensureResources();
    if (!pipeline_.isValid() || !bindingGroups_.isValid())
      return;

    writeUniforms();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.hitCapacity) + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t controls[4]{0u, 1u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 16u, "ScreenSpaceHitShadingResolvePass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings, uint32_t maxHitCapacity)
  {
    settings.hitCapacity = std::clamp(settings.hitCapacity, 1u, std::max(1u, maxHitCapacity));
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    const uint32_t hitCapacity = std::max(1u, settings_.hitCapacity);
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }

    if (!layout_.isValid())
    {
      layout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_Layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers = {
                      {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "screenHits", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "traceShading", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
                  .textures = {
                      {.name = "geometryNormalTexture", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "baseColorTexture", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "normalTexture", .binding = 5u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "materialIdTexture", .binding = 6u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "materialUVTexture", .binding = 7u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              }},
          });
    }

    if (bindingGroups_.isValid() && bindingHitCapacity_ != hitCapacity)
    {
      renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = {};
    }
    if (!bindingGroups_.isValid())
    {
      bindingHitCapacity_ = hitCapacity;
      bindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_Groups",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 1u, .bufferView = makeBufferView(screenHitsBuffer_, virtualgeometry::gpgpu::ScreenSpaceRayTracePass::screenHitsBufferSizeForCapacity(hitCapacity), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 2u, .bufferView = makeBufferView(traceShadingBuffer_, traceShadingParametersBufferSize(hitCapacity), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                  },
                  .textures = {
                      {.binding = 3u, .textureView = makeSampledColorView(geometryNormalTexture_)},
                      {.binding = 4u, .textureView = makeSampledColorView(baseColorTexture_)},
                      {.binding = 5u, .textureView = makeSampledColorView(normalTexture_)},
                      {.binding = 6u, .textureView = makeSampledColorView(materialIdTexture_)},
                      {.binding = 7u, .textureView = makeSampledColorView(materialUVTexture_)},
                  },
              }},
          });
    }

    if (!shader_.isValid())
    {
      shader_ = renderGraph->createShader(
          rendering::ShaderInfo{
              .name = passName + "_Shader",
              .layout = layout_,
              .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/screen-space-hit-shading-resolve-cs.spirv"),
              .type = rendering::ShaderType::SpirV,
          });
    }

    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          rendering::ComputePipelineInfo{
              .entry = "cs_main",
              .layout = layout_,
              .name = passName + "_Pipeline",
              .shader = shader_,
          });
    }
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.hitCapacity;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer screenHitsBuffer_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture baseColorTexture_{};
  rendering::Texture normalTexture_{};
  rendering::Texture materialIdTexture_{};
  rendering::Texture materialUVTexture_{};
  rendering::Buffer traceShadingBuffer_{};
  uint32_t maxHitCapacity_ = 1u;
  Settings settings_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  uint32_t bindingHitCapacity_ = 0u;
};

} // namespace globalillumination
