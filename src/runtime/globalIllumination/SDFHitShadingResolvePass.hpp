#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/TraceShadingTypes.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{

class SDFHitShadingResolvePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t rayCount = 0u;
    bool enabled = true;
  };

  SDFHitShadingResolvePass(
      rendering::Buffer rayInputBuffer,
      rendering::Buffer hitOutputBuffer,
      rendering::Buffer failedRaysBuffer,
      rendering::Texture geometryNormalTexture,
      rendering::Texture baseColorTexture,
      rendering::Buffer traceShadingBuffer,
      Settings settings)
      : rayInputBuffer_(rayInputBuffer),
        hitOutputBuffer_(hitOutputBuffer),
        failedRaysBuffer_(failedRaysBuffer),
        geometryNormalTexture_(geometryNormalTexture),
        baseColorTexture_(baseColorTexture),
        traceShadingBuffer_(traceShadingBuffer),
        settings_(settings)
  {
  }

  ~SDFHitShadingResolvePass() override
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

  void setDispatchIndirectBuffer(rendering::Buffer dispatchIndirectBuffer)
  {
    dispatchIndirectBuffer_ = dispatchIndirectBuffer;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    if (dispatchIndirectBuffer_.isValid())
      commandBuffer.cmdDispatchIndirect(dispatchIndirectBuffer_, 0u);
    else
      commandBuffer.cmdDispatch((std::max(1u, settings_.rayCount) + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t controls[4]{0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 16u, "SDFHitShadingResolvePass uniforms must match WGSL layout");

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
                    {.name = "sdfRayInput", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sdfHitOutput", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "failedRays", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "traceShading", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "geometryNormalTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "baseColorTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    const uint64_t rayCount = static_cast<uint64_t>(std::max(1u, settings_.rayCount));
    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 1, .bufferView = makeBufferView(rayInputBuffer_, rayCount * sizeof(gpusigneddistancefields::gpgpu::TraceRayGPU), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 2, .bufferView = makeBufferView(hitOutputBuffer_, rayCount * sizeof(gpusigneddistancefields::gpgpu::TraceHitGPU), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 3, .bufferView = makeBufferView(failedRaysBuffer_, 32ull + rayCount * 48ull, rendering::AccessPattern::SHADER_READ)},
                    {.binding = 4, .bufferView = makeBufferView(traceShadingBuffer_, traceShadingParametersBufferSize(settings_.rayCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                },
                .textures = {
                    {.binding = 5, .textureView = makeSampledColorView(geometryNormalTexture_)},
                    {.binding = 6, .textureView = makeSampledColorView(baseColorTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/sdf-hit-shading-resolve-cs.spirv"),
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
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.rayCount;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  rendering::Buffer rayInputBuffer_{};
  rendering::Buffer hitOutputBuffer_{};
  rendering::Buffer failedRaysBuffer_{};
  rendering::Buffer dispatchIndirectBuffer_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture baseColorTexture_{};
  rendering::Buffer traceShadingBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace globalillumination
