#pragma once

// Copies a finished pattern table into that pattern's result buffer and writes
// its status record.

#include "graphrewrite/runtime/PatternStatus.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <vector>

namespace graphrewrite::rendering {

class GraphPublishPass : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t patternCount = 1u;
    uint64_t resultCapacityWords = 1u << 16u;
    uint64_t sourceCapacityWords = 1u << 16u;
  };

  explicit GraphPublishPass(Settings settings) : settings_(settings)
  {
  }

  ~GraphPublishPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(paramsBuffer_);
    renderGraph->deleteBuffer(sourceBuffer_);
    renderGraph->deleteBuffer(resultBuffer_);
    renderGraph->deleteBuffer(statusBuffer_);
  }

  // Enables one pattern's publish for the NEXT graph run. The dispatch for it
  // was recorded at registration; this only fills in what it should do.
  void publish(const runtime::PublishParams &params)
  {
    if (params.patternId >= settings_.patternCount)
    {
      return;
    }

    runtime::PublishParams enabled = params;

    enabled.enabled = 1u;

    renderGraph->bufferWrite(paramsBuffer_, params.patternId * alignedParams_, sizeof(runtime::PublishParams), &enabled);
  }

  // Disables every pattern's dispatch. Call between steps, or a pattern
  // publishes again with stale contents.
  void clearQueue()
  {
    for (uint32_t pattern = 0u; pattern < settings_.patternCount; ++pattern)
    {
      runtime::PublishParams disabled;

      disabled.patternId = pattern;

      renderGraph->bufferWrite(paramsBuffer_, pattern * alignedParams_, sizeof(runtime::PublishParams), &disabled);
    }
  }

  // Creates the persistent buffers, layout, shader and pipeline. Call this right
  // after registerPass: the buffers have to exist before anything else can name
  // them, and before the owner writes the table into them.
  void createResources()
  {
    using namespace ::rendering;

    if (sourceBuffer_.isValid())
    {
      return;
    }

    const uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;

    alignedParams_ = alignUp(static_cast<uint32_t>(sizeof(runtime::PublishParams)), uniformAlignment);

    paramsBuffer_ = renderGraph->createBuffer(BufferInfo{
        .name = passName + "_Params.buffer",
        .size = static_cast<uint64_t>(alignedParams_) * settings_.patternCount,
        .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });

    sourceBuffer_ = renderGraph->createBuffer(BufferInfo{
        .name = passName + "_Source.buffer",
        .size = settings_.sourceCapacityWords * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    });

    resultBuffer_ = renderGraph->createBuffer(BufferInfo{
        .name = passName + "_Result.buffer",
        .size = settings_.resultCapacityWords * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });

    statusBuffer_ = renderGraph->createBuffer(BufferInfo{
        .name = passName + "_Status.buffer",
        .size = std::max<uint64_t>(1u, settings_.patternCount) * sizeof(runtime::PatternStatus),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
    });

    layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
        .name = passName + "_Layout",
        .groups = {BindingGroupLayout{
            .buffers =
                {
                    {.name = "publish_params",
                     .binding = 0,
                     .isDynamic = true,
                     .type = BufferBindingType::BufferBindingType_UniformBuffer,
                     .visibility = BindingVisibility::BindingVisibility_Compute},
                    {.name = "source_table",
                     .binding = 1,
                     .isDynamic = false,
                     .type = BufferBindingType::BufferBindingType_StorageBuffer,
                     .visibility = BindingVisibility::BindingVisibility_Compute},
                    {.name = "pattern_result",
                     .binding = 2,
                     .isDynamic = false,
                     .type = BufferBindingType::BufferBindingType_StorageBuffer,
                     .visibility = BindingVisibility::BindingVisibility_Compute},
                    {.name = "pattern_status",
                     .binding = 3,
                     .isDynamic = false,
                     .type = BufferBindingType::BufferBindingType_StorageBuffer,
                     .visibility = BindingVisibility::BindingVisibility_Compute},
                },
        }},
    });

    shader_ = renderGraph->createShader(ShaderInfo{
        .name = passName + "_Shader",
        .layout = layout_,
        .src = os::io::readRelativeFile("assets/shaders/spirv/graphrewrite-publish.spirv"),
        .type = ShaderType::SpirV,
    });

    pipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{
        .entry = "publish",
        .layout = layout_,
        .name = passName + "_Pipeline",
        .shader = shader_,
    });
  }

  const ::rendering::Buffer &sourceBuffer() const
  {
    return sourceBuffer_;
  }

  const ::rendering::Buffer &resultBuffer() const
  {
    return resultBuffer_;
  }

  const ::rendering::Buffer &statusBuffer() const
  {
    return statusBuffer_;
  }

  static uint32_t alignUp(const uint32_t value, const uint32_t alignment)
  {
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
  }

  static uint32_t divCeil(const uint32_t value, const uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    using namespace ::rendering;

    createResources();

    bindingGroups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
        .name = passName + "_Bindings",
        .layout = layout_,
        .groups = {GroupInfo{
            .name = "group0",
            .buffers =
                {
                    {.binding = 0,
                     .bufferView = {.buffer = paramsBuffer_,
                                    .offset = 0u,
                                    .size = sizeof(runtime::PublishParams),
                                    .access = AccessPattern::SHADER_READ}},
                    {.binding = 1,
                     .bufferView = {.buffer = sourceBuffer_,
                                    .offset = 0u,
                                    .size = settings_.sourceCapacityWords * sizeof(uint32_t),
                                    .access = AccessPattern::SHADER_READ}},
                    {.binding = 2,
                     .bufferView = {.buffer = resultBuffer_,
                                    .offset = 0u,
                                    .size = settings_.resultCapacityWords * sizeof(uint32_t),
                                    .access = AccessPattern::SHADER_WRITE}},
                    {.binding = 3,
                     .bufferView = {.buffer = statusBuffer_,
                                    .offset = 0u,
                                    .size = std::max<uint64_t>(1u, settings_.patternCount) * sizeof(runtime::PatternStatus),
                                    .access = AccessPattern::SHADER_WRITE}},
                },
        }},
    });

    commandBuffer.cmdBindComputePipeline(pipeline_);

    // One fixed dispatch per pattern, sized to the result capacity. Which of
    // them do anything is decided per frame by the `enabled` flag in the params
    // slot each one reads.
    const uint32_t groups = divCeil(static_cast<uint32_t>(settings_.resultCapacityWords), 256u);

    for (uint32_t pattern = 0u; pattern < settings_.patternCount; ++pattern)
    {
      uint32_t dynamicOffset = pattern * alignedParams_;

      commandBuffer.cmdBindBindingGroups(bindingGroups_, &dynamicOffset, 1u);
      commandBuffer.cmdDispatch(groups, 1u, 1u);
    }
  }

private:
  Settings settings_;

  uint32_t alignedParams_ = 0u;

  ::rendering::Buffer paramsBuffer_;
  ::rendering::Buffer sourceBuffer_;
  ::rendering::Buffer resultBuffer_;
  ::rendering::Buffer statusBuffer_;

  ::rendering::BindingsLayout layout_;
  ::rendering::BindingGroups bindingGroups_;
  ::rendering::Shader shader_;
  ::rendering::ComputePipeline pipeline_;
};

} // namespace graphrewrite::rendering
