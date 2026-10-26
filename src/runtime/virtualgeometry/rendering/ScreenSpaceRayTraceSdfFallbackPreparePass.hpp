#pragma once

#include <algorithm>
#include <cstdint>

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class ScreenSpaceRayTraceSdfFallbackPreparePass : public ::rendering::Pass
{
public:
  struct Settings
  {
    bool enabled = true;
    uint32_t budget = 65536u;
    uint32_t maxFailedRays = 1u << 20u;
    float tMin = 0.01f;
    float tMax = 3000.0f;
  };

  ScreenSpaceRayTraceSdfFallbackPreparePass(
      ::rendering::Buffer failedRaysBuffer,
      ::rendering::Buffer sdfRayInputBuffer,
      Settings settings)
      : failedRaysBuffer_(failedRaysBuffer), sdfRayInputBuffer_(sdfRayInputBuffer), settings_(sanitizeSettings(settings))
  {
  }

  ~ScreenSpaceRayTraceSdfFallbackPreparePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (prepareDispatchPipeline_.isValid())
      renderGraph->deleteComputePipeline(prepareDispatchPipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (dispatchIndirectArgsBuffer_.isValid())
      renderGraph->deleteBuffer(dispatchIndirectArgsBuffer_);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  const Settings &getSettings() const
  {
    return settings_;
  }

  const ::rendering::Buffer &getDispatchIndirectArgsBuffer()
  {
    ensureDispatchIndirectArgsBuffer();
    return dispatchIndirectArgsBuffer_;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {::rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "failedRays", .binding = 1u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sdfRayInput", .binding = 2u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dispatchArgs", .binding = 3u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    ensureDispatchIndirectArgsBuffer();
    bindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    makeBufferBinding(0u, uniformBuffer_, ::rendering::AccessPattern::SHADER_READ, sizeof(Uniforms)),
                    makeBufferBinding(1u, failedRaysBuffer_, ::rendering::AccessPattern::SHADER_READ, failedRaysBufferSize()),
                    makeBufferBinding(2u, sdfRayInputBuffer_, ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, sdfRayInputBufferSize()),
                    makeBufferBinding(3u, dispatchIndirectArgsBuffer_, ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, sizeof(DispatchIndirectArgsGPU)),
                },
            }},
        });

    shader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/screen-space-raytrace-sdf-fallback-prepare-cs.spirv"),
            .type = ::rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
    prepareDispatchPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "prepare_dispatch",
            .layout = layout_,
            .name = passName + "_PrepareDispatch.pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(prepareDispatchPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.budget + 63u) / 64u, 1u, 1u);
  }

private:
  struct DispatchIndirectArgsGPU
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 1u;
    uint32_t groupCountZ = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgsGPU) == 12u, "ScreenSpaceRayTraceSdfFallbackPreparePass::DispatchIndirectArgsGPU must stay tightly packed");

  struct Uniforms
  {
    uint32_t budget = 1u;
    uint32_t maxFailedRays = 1u;
    uint32_t enabled = 1u;
    uint32_t _padding = 0u;
    float tMin = 0.01f;
    float tMax = 3000.0f;
    uint32_t _padding1[2] = {};
  };
  static_assert(sizeof(Uniforms) == 32u, "ScreenSpaceRayTraceSdfFallbackPreparePass::Uniforms must stay tightly packed");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.budget = std::max(1u, settings.budget);
    settings.maxFailedRays = std::max(1u, settings.maxFailedRays);
    settings.tMin = std::max(0.0f, settings.tMin);
    settings.tMax = std::max(settings.tMax, settings.tMin + 1.0e-4f);
    return settings;
  }

  static ::rendering::BindingBuffer makeBufferBinding(uint32_t binding, ::rendering::Buffer buffer, ::rendering::AccessPattern access, uint64_t size)
  {
    return ::rendering::BindingBuffer{
        .bufferView = {.buffer = buffer, .offset = 0u, .size = size, .access = access},
        .binding = binding,
    };
  }

  uint64_t failedRaysBufferSize() const
  {
    constexpr uint64_t kFailedRayHeaderBytes = 32u;
    constexpr uint64_t kFailedRayBytes = 48u;
    return kFailedRayHeaderBytes + static_cast<uint64_t>(settings_.maxFailedRays) * kFailedRayBytes;
  }

  uint64_t sdfRayInputBufferSize() const
  {
    return static_cast<uint64_t>(settings_.budget) * sizeof(gpusigneddistancefields::gpgpu::TraceRayGPU);
  }

  ::rendering::Buffer createFrameLocalBuffer(::rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void ensureDispatchIndirectArgsBuffer()
  {
    if (dispatchIndirectArgsBuffer_.isValid() || renderGraph == nullptr)
      return;

    dispatchIndirectArgsBuffer_ = createFrameLocalBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_DispatchIndirectArgs.buffer",
            .size = sizeof(DispatchIndirectArgsGPU),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_CopySrc,
        });
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.budget = settings_.budget;
    uniforms.maxFailedRays = settings_.maxFailedRays;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.tMin = settings_.tMin;
    uniforms.tMax = settings_.tMax;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  Settings settings_{};
  ::rendering::Buffer failedRaysBuffer_{};
  ::rendering::Buffer sdfRayInputBuffer_{};
  ::rendering::Buffer dispatchIndirectArgsBuffer_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::ComputePipeline prepareDispatchPipeline_{};
};

} // namespace gpgpu
} // namespace virtualgeometry
