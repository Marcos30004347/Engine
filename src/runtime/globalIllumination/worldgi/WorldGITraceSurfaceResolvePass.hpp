#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/TraceShadingTypes.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGITraceSurfaceResolvePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t surfaceCount = 0u;
    uint32_t sourceOverride = GITraceSurfaceSource_Invalid;
    bool enabled = true;
  };

  WorldGITraceSurfaceResolvePass(
      rendering::Buffer traceShadingBuffer,
      rendering::Buffer traceSurfaceBuffer,
      Settings settings)
      : traceShadingBuffer_(traceShadingBuffer),
        traceSurfaceBuffer_(traceSurfaceBuffer),
        settings_(settings)
  {
  }

  ~WorldGITraceSurfaceResolvePass() override
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

  void setSettings(Settings settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (settings_.surfaceCount == 0u)
      return;

    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.surfaceCount) + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t enabled = 1u;
    uint32_t surfaceCount = 0u;
    uint32_t sourceOverride = GITraceSurfaceSource_Invalid;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(Uniforms) == 16u, "WorldGITraceSurfaceResolvePass uniforms must match WGSL layout");

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  void ensureResources()
  {
    const uint32_t surfaceCount = std::max(1u, settings_.surfaceCount);
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
                      {.name = "traceSurfaces", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              }},
          });
    }

    if (bindingGroups_.isValid() && bindingSurfaceCount_ != surfaceCount)
    {
      renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = {};
    }

    if (!bindingGroups_.isValid())
    {
      bindingSurfaceCount_ = surfaceCount;
      bindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_Groups",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 1u, .bufferView = makeBufferView(traceShadingBuffer_, traceShadingParametersBufferSize(surfaceCount), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 2u, .bufferView = makeBufferView(traceSurfaceBuffer_, giTraceSurfaceBufferSize(surfaceCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
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
              .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-trace-surface-resolve-cs.spirv"),
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
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.surfaceCount = settings_.surfaceCount;
    uniforms.sourceOverride = settings_.sourceOverride;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer traceShadingBuffer_{};
  rendering::Buffer traceSurfaceBuffer_{};
  Settings settings_{};
  uint32_t bindingSurfaceCount_ = 0u;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace globalillumination::worldgi
