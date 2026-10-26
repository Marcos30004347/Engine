#pragma once
#include <cstring>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

template <typename K, typename V> class RadixSort2Pass : public Pass
{
  rendering::BindingGroups bindingGroupA;
  rendering::BindingGroups bindingGroupB;
  rendering::BindingsLayout sortLayout;

  rendering::Shader sortShaderHandle;

  rendering::ComputePipeline clearCountsPipeline;
  rendering::ComputePipeline sortCountPipeline;
  rendering::ComputePipeline sortReducePipeline;
  rendering::ComputePipeline sortScanPipeline;
  rendering::ComputePipeline sortScanAddPipeline;
  rendering::ComputePipeline sortScatterPipeline;
  rendering::Timer dispatchTimer;

  rendering::Buffer keysB;
  rendering::Buffer valuesB;
  rendering::Buffer uniformsBuffer;
  rendering::Buffer numKeysBuffer;
  rendering::Buffer countsBuffer;
  rendering::Buffer reducedBuffer;
  rendering::Buffer externalNumKeysBuffer;
  rendering::Buffer externalDispatchIndirectBuffer;

  uint32_t count;
  bool timingEnabled = true;

  rendering::Buffer keys;
  rendering::Buffer values;
  uint32_t sortingBits = 32;
  uint32_t sortingBitOffset = 0;

public:
  RadixSort2Pass(uint32_t count, const Buffer &keys, const Buffer &payload) : count(count), keys(keys), values(payload)
  {
  }

  RadixSort2Pass(uint32_t count, const Buffer &keys, const Buffer &payload, const Buffer &numKeys) : externalNumKeysBuffer(numKeys), count(count), keys(keys), values(payload)
  {
  }

  RadixSort2Pass(uint32_t count, const Buffer &keys, const Buffer &payload, const Buffer &numKeys, const Buffer &dispatchIndirect, bool enableTiming = true)
      : externalNumKeysBuffer(numKeys), externalDispatchIndirectBuffer(dispatchIndirect), count(count), timingEnabled(enableTiming), keys(keys), values(payload)
  {
  }

  // Restricts the sort to the digit range [bitOffset, bitOffset + bits).
  // Ordering is then only defined with respect to those bits, but keys sharing
  // them still land contiguously, which is all a grouping consumer needs. Each
  // 4-bit digit round costs a full set of dispatches, so halving the range
  // halves this pass group's cost. Must be called before the first record.
  void setSortingBitRange(uint32_t bitOffset, uint32_t bits)
  {
    sortingBitOffset = std::min(bitOffset, 32u);
    sortingBits = std::clamp(bits, BITS_PER_PASS, 32u - sortingBitOffset);
  }

  const rendering::Timer &getDispatchTimer() const
  {
    return dispatchTimer;
  }

  void ensureDispatchTimerAllocated()
  {
    timingEnabled = true;
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

  ~RadixSort2Pass()
  {
    renderGraph->deleteComputePipeline(clearCountsPipeline);
    renderGraph->deleteComputePipeline(sortCountPipeline);
    renderGraph->deleteComputePipeline(sortReducePipeline);
    renderGraph->deleteComputePipeline(sortScanPipeline);
    renderGraph->deleteComputePipeline(sortScanAddPipeline);
    renderGraph->deleteComputePipeline(sortScatterPipeline);

    renderGraph->deleteBindingGroups(bindingGroupA);
    renderGraph->deleteBindingGroups(bindingGroupB);
    renderGraph->deleteBindingsLayout(sortLayout);
    renderGraph->deleteShader(sortShaderHandle);

    renderGraph->deleteBuffer(keysB);
    renderGraph->deleteBuffer(valuesB);
    renderGraph->deleteBuffer(uniformsBuffer);
    if (!externalNumKeysBuffer.isValid())
    {
      renderGraph->deleteBuffer(numKeysBuffer);
    }
    renderGraph->deleteBuffer(countsBuffer);
    renderGraph->deleteBuffer(reducedBuffer);
    if (dispatchTimer.isValid())
    {
      renderGraph->deleteTimer(dispatchTimer);
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (timingEnabled && !dispatchTimer.isValid())
    {
      dispatchTimer = rendering::Timer{
        .name = passName + "_dispatch.timer",
        .sampleCount = 1u,
      };
    }

    uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;

    std::string sortShader = os::io::readRelativeFile("assets/shaders/spirv/radixsort2.spirv");

    uint32_t maxNumKeys = count;
    uint32_t maxNeededWgs = divCeil(maxNumKeys, BLOCK_SIZE);
    uint32_t countBufSize = maxNeededWgs * BIN_COUNT;
    uint32_t reducedBufSize = BIN_COUNT * divCeil(maxNeededWgs, BLOCK_SIZE) * BLOCK_SIZE;
    uint32_t numPasses = divCeil(sortingBits, BITS_PER_PASS);
    uint32_t alignedUniformSize = alignUp(sizeof(SortUniforms), uniformAlignment);
    uint32_t totalUniformBufferSize = alignedUniformSize * numPasses;

    keysB = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Keys_B.buffer",
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    valuesB = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Values_B.buffer",
          .size = count * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    // The sort configuration is immutable after graph construction. Keeping
    // it persistent avoids leaving later frame slots uninitialized while the
    // keys/count scratch buffers remain frame-local.
    uniformsBuffer = renderGraph->createBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = totalUniformBufferSize,
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });

    if (!externalNumKeysBuffer.isValid())
    {
      numKeysBuffer = createFrameLocalBuffer(
          BufferInfo{
            .name = passName + "_NumKeys.buffer",
            .size = sizeof(uint32_t),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          });
    }

    countsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Counts.buffer",
          .size = countBufSize * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    reducedBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Reduced.buffer",
          .size = reducedBufSize * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });
    if (!externalNumKeysBuffer.isValid())
    {
      renderGraph->bufferWrite(numKeysBuffer, 0, sizeof(uint32_t), (void **)&count);
    }

    sortLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_radixSort.layout",
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
                         .name = "num_keys_arr",
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
                         .name = "counts",
                         .binding = 3,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "values",
                         .binding = 4,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "out",
                         .binding = 5,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "out_values",
                         .binding = 6,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       },
                       {
                         .name = "reduced",
                         .binding = 7,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_StorageBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Compute,
                       }
                      }},
              },
        });

    sortShaderHandle = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_radixSort.shader",
          .layout = sortLayout,
          .src = sortShader,
          .type = ShaderType::SpirV,
        });

    clearCountsPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "clear_counts",
          .layout = sortLayout,
          .name = passName + "_clear_counts.pipeline",
          .shader = sortShaderHandle,
        });

    sortCountPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "sort_count",
          .layout = sortLayout,
          .name = passName + "_sort_count.pipeline",
          .shader = sortShaderHandle,
        });

    sortReducePipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "sort_reduce",
          .layout = sortLayout,
          .name = passName + "_sort_reduce.pipeline",
          .shader = sortShaderHandle,
        });

    sortScanPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "sort_scan",
          .layout = sortLayout,
          .name = passName + "_sort_scan.pipeline",
          .shader = sortShaderHandle,
        });

    sortScanAddPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "sort_scan_add",
          .layout = sortLayout,
          .name = passName + "_sort_scan_add.pipeline",
          .shader = sortShaderHandle,
        });

    sortScatterPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "sort_scatter",
          .layout = sortLayout,
          .name = passName + "_sort_scatter.pipeline",
          .shader = sortShaderHandle,
        });

    uint32_t numWgs = divCeil(count, BLOCK_SIZE);
    uint32_t numReduceWgs = BIN_COUNT * divCeil(numWgs, BLOCK_SIZE);

    bindingGroupA = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = sortLayout,
          .name = passName + "_sortBindingGroup_A",
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
                               .buffer = externalNumKeysBuffer.isValid() ? externalNumKeysBuffer : numKeysBuffer,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = sizeof(uint32_t),
                             }},
                        {.binding = 2,
                         .bufferView =
                             {
                               .buffer = keys,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 3,
                         .bufferView =
                             {
                               .buffer = countsBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = countBufSize * sizeof(uint32_t),
                             }},
                        {.binding = 4,
                         .bufferView =
                             {
                               .buffer = values,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 5,
                         .bufferView =
                             {
                               .buffer = keysB,
                               .access = AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 6,
                         .bufferView =
                             {
                               .buffer = valuesB,
                               .access = AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 7,
                         .bufferView =
                             {
                               .buffer = reducedBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = reducedBufSize * sizeof(uint32_t),
                             }},

                      },
                },
              },
        });

    bindingGroupB = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = sortLayout,
          .name = passName + "_sortBindingGroup_B",
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
                               .buffer = externalNumKeysBuffer.isValid() ? externalNumKeysBuffer : numKeysBuffer,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = sizeof(uint32_t),
                             }},
                        {.binding = 2,
                         .bufferView =
                             {
                               .buffer = keysB,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 3,
                         .bufferView =
                             {
                               .buffer = countsBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = countBufSize * sizeof(uint32_t),
                             }},
                        {.binding = 4,
                         .bufferView =
                             {
                               .buffer = valuesB,
                               .access = AccessPattern::SHADER_READ,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 5,
                         .bufferView =
                             {
                               .buffer = keys,
                               .access = AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 6,
                         .bufferView =
                             {
                               .buffer = values,
                               .access = AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = count * sizeof(uint32_t),
                             }},
                        {.binding = 7,
                         .bufferView =
                             {
                               .buffer = reducedBuffer,
                               .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                               .offset = 0,
                               .size = reducedBufSize * sizeof(uint32_t),
                             }},

                      },
                },
              },
        });

    bool finalResultInA = (numPasses % 2 == 0);

    uint8_t *uniformData = new uint8_t[totalUniformBufferSize];
    memset(uniformData, 0, totalUniformBufferSize);

    for (uint32_t pass = 0; pass < numPasses; pass++)
    {
      SortUniforms *uniformPtr = reinterpret_cast<SortUniforms *>(uniformData + pass * alignedUniformSize);
      uniformPtr->shift = sortingBitOffset + pass * BITS_PER_PASS;
    }

    renderGraph->bufferWrite(uniformsBuffer, 0, totalUniformBufferSize, (void *)uniformData);
    delete[] uniformData;

    if (timingEnabled)
      commandBuffer.cmdStartTimer(dispatchTimer, PipelineStage::COMPUTE_SHADER);
    for (uint32_t pass = 0; pass < numPasses; pass++)
    {
      auto currentBindingGroup = (pass % 2 == 0) ? bindingGroupA : bindingGroupB;

      uint32_t offset = alignedUniformSize * pass;
      uint32_t clearCount = countBufSize;
      uint32_t clearWgs = divCeil(clearCount, 256);

      commandBuffer.cmdBindComputePipeline(clearCountsPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::ClearCounts, clearWgs, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(sortCountPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::SortCount, numWgs, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(sortReducePipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::SortReduce, numReduceWgs, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(sortScanPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::SortScan, 1u, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(sortScanAddPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::SortScanAdd, numReduceWgs, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(sortScatterPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      dispatchStage(commandBuffer, DispatchIndirectSlot::SortScatter, numWgs, 1u, 1u);
    }
    if (timingEnabled)
      commandBuffer.cmdStopTimer(dispatchTimer, PipelineStage::COMPUTE_SHADER);

    if (!finalResultInA)
    {
      commandBuffer.cmdCopyBuffer(
          BufferView{.access = AccessPattern::TRANSFER_READ, .buffer = keysB, .offset = 0, .size = count * sizeof(uint32_t)},
          BufferView{.access = AccessPattern::TRANSFER_WRITE, .buffer = keys, .offset = 0, .size = count * sizeof(uint32_t)});

      commandBuffer.cmdCopyBuffer(
          BufferView{.access = AccessPattern::TRANSFER_READ, .buffer = valuesB, .offset = 0, .size = count * sizeof(uint32_t)},
          BufferView{.access = AccessPattern::TRANSFER_WRITE, .buffer = values, .offset = 0, .size = count * sizeof(uint32_t)});
    }
  }

private:
  const static uint32_t WG = 256;
  const static uint32_t ELEMENTS_PER_THREAD = 4;
  const static uint32_t BLOCK_SIZE = WG * ELEMENTS_PER_THREAD;
  static constexpr uint32_t BITS_PER_PASS = 4;
  static constexpr uint32_t BIN_COUNT = 1u << BITS_PER_PASS;

  enum class DispatchIndirectSlot : uint32_t
  {
    ClearCounts = 0u,
    SortCount = 1u,
    SortReduce = 2u,
    SortScan = 3u,
    SortScanAdd = 4u,
    SortScatter = 5u,
  };

  struct DispatchIndirectArgs
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 1u;
    uint32_t groupCountZ = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgs) == 12u, "RadixSort2Pass::DispatchIndirectArgs must stay tightly packed");

  void dispatchStage(
      rendering::CommandRecorder &commandBuffer,
      const DispatchIndirectSlot slot,
      const uint32_t groupCountX,
      const uint32_t groupCountY,
      const uint32_t groupCountZ)
  {
    if (externalDispatchIndirectBuffer.isValid())
    {
      commandBuffer.cmdDispatchIndirect(externalDispatchIndirectBuffer, static_cast<uint64_t>(slot) * sizeof(DispatchIndirectArgs));
      return;
    }

    commandBuffer.cmdDispatch(groupCountX, groupCountY, groupCountZ);
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
    uint32_t shift;
  };
};
} // namespace gpgpu
} // namespace rendering
