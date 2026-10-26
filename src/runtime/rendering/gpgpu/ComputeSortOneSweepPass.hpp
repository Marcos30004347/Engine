#pragma once

#include <array>
#include <vector>

#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

template <typename K, typename V> class ComputeSortOneSweepPass : public Pass
{
  static_assert(std::is_same<K, uint32_t>::value, "K must be uint32_t");
  static_assert(std::is_same<V, uint32_t>::value, "V must be uint32_t");

  rendering::BindingGroups bindingGroupA;
  rendering::BindingGroups bindingGroupB;
  rendering::BindingsLayout sortLayout;

  rendering::Shader sortShaderHandle;

  rendering::ComputePipeline globalHistogramPipeline;
  rendering::ComputePipeline scanPipeline;
  rendering::ComputePipeline passPipeline;
  rendering::Timer dispatchTimer;

  rendering::Buffer keysB;
  rendering::Buffer valuesB;
  rendering::Buffer uniformsBuffer;
  rendering::Buffer bumpBuffer;
  rendering::Buffer histBuffer;
  rendering::Buffer passHistBuffer;
  rendering::Buffer statusBuffer;

  uint32_t count;
  uint32_t threadBlocks = 0u;

  rendering::Buffer keys;
  rendering::Buffer values;

public:
  ComputeSortOneSweepPass(uint32_t count, const Buffer &keys, const Buffer &payload) : count(count), keys(keys), values(payload)
  {
  }

  const rendering::Timer &getDispatchTimer() const
  {
    return dispatchTimer;
  }

  void ensureDispatchTimerAllocated()
  {
    if (!dispatchTimer.isValid())
    {
      dispatchTimer = rendering::Timer{
        .name = passName + "_dispatch.timer",
        .sampleCount = 1u,
      };
    }

    renderGraph->deleteTimer(dispatchTimer);
    dispatchTimer = renderGraph->createTimer(
        TimerInfo{
          .name = dispatchTimer.name,
          .unit = TimerUnit::Miliseconds,
          .sampleCount = dispatchTimer.sampleCount,
        });
  }

  void prepareForRun()
  {
    renderGraph->bufferWrite(bumpBuffer, 0, bumpResetData.size() * sizeof(uint32_t), (void *)bumpResetData.data());
    renderGraph->bufferWrite(histBuffer, 0, histResetData.size() * sizeof(uint32_t), (void *)histResetData.data());
    renderGraph->bufferWrite(statusBuffer, 0, statusResetData.size() * sizeof(uint32_t), (void *)statusResetData.data());
    renderGraph->bufferWrite(passHistBuffer, 0, passHistResetData.size() * sizeof(uint32_t), (void *)passHistResetData.data());
  }

  ~ComputeSortOneSweepPass()
  {
    renderGraph->deleteComputePipeline(globalHistogramPipeline);
    renderGraph->deleteComputePipeline(scanPipeline);
    renderGraph->deleteComputePipeline(passPipeline);

    renderGraph->deleteBindingGroups(bindingGroupA);
    renderGraph->deleteBindingGroups(bindingGroupB);
    renderGraph->deleteBindingsLayout(sortLayout);
    renderGraph->deleteShader(sortShaderHandle);

    renderGraph->deleteBuffer(keysB);
    renderGraph->deleteBuffer(valuesB);
    renderGraph->deleteBuffer(uniformsBuffer);
    renderGraph->deleteBuffer(bumpBuffer);
    renderGraph->deleteBuffer(histBuffer);
    renderGraph->deleteBuffer(passHistBuffer);
    renderGraph->deleteBuffer(statusBuffer);
    if (dispatchTimer.isValid())
    {
      renderGraph->deleteTimer(dispatchTimer);
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!dispatchTimer.isValid())
    {
      dispatchTimer = rendering::Timer{
        .name = passName + "_dispatch.timer",
        .sampleCount = 1u,
      };
    }

    uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;
    std::string sortShader = os::io::readRelativeFile("assets/shaders/spirv/onesweep.spirv");

    threadBlocks = divCeil(count, PARTITION_SIZE);

    uint32_t alignedUniformSize = alignUp(sizeof(SortUniforms), uniformAlignment);
    uint32_t totalUniformBufferSize = alignedUniformSize * RADIX_PASS_COUNT;
    uint32_t passHistSize = threadBlocks * ALL_RADIX;

    keysB = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Keys_B.buffer",
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage,
        });

    valuesB = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Values_B.buffer",
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage,
        });

    uniformsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = totalUniformBufferSize,
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });

    bumpBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Bump.buffer",
          .size = RADIX_PASS_COUNT * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        });

    histBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Histogram.buffer",
          .size = ALL_RADIX * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        });

    passHistBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_PassHistogram.buffer",
          .size = passHistSize * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        });

    statusBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Status.buffer",
          .size = STATUS_ENTRY_COUNT * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        });

    sortLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_oneSweep.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {{
                         .name = "config",
                         .binding = 0,
                         .isDynamic = true,
                         .type = BufferBindingType::BufferBindingType_UniformBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "bump",
                         .binding = 1,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "src",
                         .binding = 2,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "dst",
                         .binding = 3,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "src_payload",
                         .binding = 4,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "dst_payload",
                         .binding = 5,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "hist",
                         .binding = 6,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "pass_hist",
                         .binding = 7,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "status",
                         .binding = 8,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       }
                      }},
              },
        });

    sortShaderHandle = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_oneSweep.shader",
          .layout = sortLayout,
          .src = sortShader,
          .type = ShaderType::SpirV,
        });

    globalHistogramPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "global_hist",
          .layout = sortLayout,
          .name = passName + "_onesweep_global_hist.pipeline",
          .shader = sortShaderHandle,
        });

    scanPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "onesweep_scan",
          .layout = sortLayout,
          .name = passName + "_onesweep_scan.pipeline",
          .shader = sortShaderHandle,
        });

    passPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "onesweep_pass",
          .layout = sortLayout,
          .name = passName + "_onesweep_pass.pipeline",
          .shader = sortShaderHandle,
        });

    bindingGroupA = createBindingGroup("A", alignedUniformSize, passHistSize, false);
    bindingGroupB = createBindingGroup("B", alignedUniformSize, passHistSize, true);

    uint8_t *uniformData = new uint8_t[totalUniformBufferSize];
    memset(uniformData, 0, totalUniformBufferSize);
    for (uint32_t pass = 0; pass < RADIX_PASS_COUNT; ++pass)
    {
      SortUniforms *uniformPtr = reinterpret_cast<SortUniforms *>(uniformData + pass * alignedUniformSize);
      uniformPtr->size = count;
      uniformPtr->shift = pass * RADIX_LOG2;
      uniformPtr->threadBlocks = threadBlocks;
      uniformPtr->padding0 = 0u;
    }
    renderGraph->bufferWrite(uniformsBuffer, 0, totalUniformBufferSize, (void *)uniformData);
    delete[] uniformData;

    passHistResetData.assign(passHistSize, 0u);
    for (uint32_t pass = 0; pass < RADIX_PASS_COUNT; ++pass)
    {
      uint32_t passOffset = pass * threadBlocks * RADIX_BIN_COUNT;
      for (uint32_t digit = 0; digit < RADIX_BIN_COUNT; ++digit)
      {
        passHistResetData[passOffset + digit] = FLAG_INCLUSIVE;
      }
    }

    uint32_t globalOffset = 0u;
    commandBuffer.cmdStartTimer(dispatchTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdBindComputePipeline(globalHistogramPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroupA, &globalOffset, 1);
    commandBuffer.cmdDispatch(divCeil(count, REDUCE_PARTITION_SIZE), 1, 1);

    for (uint32_t pass = 0; pass < RADIX_PASS_COUNT; ++pass)
    {
      auto currentBindingGroup = (pass % 2u == 0u) ? bindingGroupA : bindingGroupB;
      uint32_t offset = alignedUniformSize * pass;

      commandBuffer.cmdBindComputePipeline(scanPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(1, 1, 1);

      commandBuffer.cmdBindComputePipeline(passPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(threadBlocks, 1, 1);
    }
    commandBuffer.cmdStopTimer(dispatchTimer, PipelineStage::COMPUTE_SHADER);
  }

private:
  static constexpr uint32_t RADIX_PASS_COUNT = 4u;
  static constexpr uint32_t STATUS_ENTRY_COUNT = 3u;
  static constexpr uint32_t BLOCK_DIM = 256u;
  static constexpr uint32_t REDUCE_BLOCK_DIM = 128u;
  static constexpr uint32_t KEYS_PER_THREAD = 15u;
  static constexpr uint32_t REDUCE_KEYS_PER_THREAD = 30u;
  static constexpr uint32_t PARTITION_SIZE = BLOCK_DIM * KEYS_PER_THREAD;
  static constexpr uint32_t REDUCE_PARTITION_SIZE = REDUCE_BLOCK_DIM * REDUCE_KEYS_PER_THREAD;
  static constexpr uint32_t RADIX_BIN_COUNT = 256u;
  static constexpr uint32_t RADIX_LOG2 = 8u;
  static constexpr uint32_t ALL_RADIX = RADIX_BIN_COUNT * RADIX_PASS_COUNT;
  static constexpr uint32_t FLAG_INCLUSIVE = 2u;

  std::array<uint32_t, RADIX_PASS_COUNT> bumpResetData{};
  std::array<uint32_t, ALL_RADIX> histResetData{};
  std::array<uint32_t, STATUS_ENTRY_COUNT> statusResetData{};
  std::vector<uint32_t> passHistResetData;

  BindingGroups createBindingGroup(const std::string &suffix, uint32_t alignedUniformSize, uint32_t passHistSize, bool swapBuffers)
  {
    const Buffer &srcKeys = swapBuffers ? keysB : keys;
    const Buffer &dstKeys = swapBuffers ? keys : keysB;
    const Buffer &srcValues = swapBuffers ? valuesB : values;
    const Buffer &dstValues = swapBuffers ? values : valuesB;

    return renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = sortLayout,
          .name = passName + "_onesweepBindingGroup_" + suffix,
          .groups =
              {
                GroupInfo{
                  .name = "Group0",
                  .buffers =
                      {
                        {.binding = 0,
                         .bufferView =
                             {
                               .buffer = uniformsBuffer,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = alignedUniformSize,
                             }},
                        {.binding = 1,
                         .bufferView =
                             {
                               .buffer = bumpBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = RADIX_PASS_COUNT * sizeof(uint32_t),
                             }},
                        {.binding = 2,
                         .bufferView =
                             {
                               .buffer = srcKeys,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 3,
                         .bufferView =
                             {
                               .buffer = dstKeys,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 4,
                         .bufferView =
                             {
                               .buffer = srcValues,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 5,
                         .bufferView =
                             {
                               .buffer = dstValues,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 6,
                         .bufferView =
                             {
                               .buffer = histBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = ALL_RADIX * sizeof(uint32_t),
                             }},
                        {.binding = 7,
                         .bufferView =
                             {
                               .buffer = passHistBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = passHistSize * sizeof(uint32_t),
                             }},
                        {.binding = 8,
                         .bufferView =
                             {
                               .buffer = statusBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = STATUS_ENTRY_COUNT * sizeof(uint32_t),
                             }},
                      },
                },
              },
        });
  }

  inline static uint32_t divCeil(uint32_t a, uint32_t b)
  {
    return (a + b - 1u) / b;
  }

  inline static uint32_t alignUp(uint32_t size, uint32_t alignment)
  {
    return ((size + alignment - 1u) / alignment) * alignment;
  }

  struct SortUniforms
  {
    uint32_t size;
    uint32_t shift;
    uint32_t threadBlocks;
    uint32_t padding0;
  };
};

} // namespace gpgpu
} // namespace rendering
