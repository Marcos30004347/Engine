#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/TraceShadingTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{

class TraceShadingAlbedoDebugPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t rayCount = 1u;
    bool enabled = true;
  };

  TraceShadingAlbedoDebugPass(rendering::Buffer traceShadingBuffer, rendering::Texture outputTexture, Settings settings)
      : traceShadingBuffer_(traceShadingBuffer), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~TraceShadingAlbedoDebugPass() override
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

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    if (!pipeline_.isValid() || !bindingGroups_.isValid() || !outputTexture_.isValid())
      return;

    writeUniforms();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.width) + 7u) / 8u, (std::max(1u, settings_.height) + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t controls[4]{0u, 1u, 1u, 1u};
  };
  static_assert(sizeof(Uniforms) == 16u, "TraceShadingAlbedoDebugPass uniforms must match WGSL layout");

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
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
    const uint64_t rayCount = std::max<uint64_t>(1u, settings_.rayCount);
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
                      {.name = "traceShading", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
                  .storageTextures = {
                      {.name = "outputTexture", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              }},
          });
    }

    if (bindingGroups_.isValid() && bindingRayCount_ != rayCount)
    {
      renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = {};
    }
    if (!bindingGroups_.isValid())
    {
      bindingRayCount_ = rayCount;
      bindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_Groups",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 1u, .bufferView = makeBufferView(traceShadingBuffer_, traceShadingParametersBufferSize(static_cast<uint32_t>(rayCount)), rendering::AccessPattern::SHADER_READ)},
                  },
                  .storageTextures = {
                      {.binding = 2u, .textureView = makeStorageColorView(outputTexture_)},
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
              .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/trace-shading-albedo-debug-cs.spirv"),
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
    uniforms.controls[1] = settings_.width;
    uniforms.controls[2] = settings_.height;
    uniforms.controls[3] = settings_.rayCount;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer traceShadingBuffer_{};
  rendering::Texture outputTexture_{};
  Settings settings_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  uint64_t bindingRayCount_ = 0u;
};

} // namespace globalillumination
