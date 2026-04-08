#pragma once
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

  rendering::Buffer keysB;
  rendering::Buffer valuesB;
  rendering::Buffer uniformsBuffer;
  rendering::Buffer numKeysBuffer;
  rendering::Buffer countsBuffer;
  rendering::Buffer reducedBuffer;

  uint32_t count;

  rendering::Buffer keys;
  rendering::Buffer values;

public:
  RadixSort2Pass(uint32_t count, const Buffer &keys, const Buffer &payload) : count(count), keys(keys), values(payload)
  {
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
    renderGraph->deleteBuffer(numKeysBuffer);
    renderGraph->deleteBuffer(countsBuffer);
    renderGraph->deleteBuffer(reducedBuffer);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;

    std::string sortShader = os::io::readRelativeFile("assets/shaders/spirv/radixsort2.spirv");

    uint32_t count = 1024 * 1024; // 1M elements
    uint32_t *inputKeys = new uint32_t[count];
    uint32_t *inputValues = new uint32_t[count];

    for (uint32_t i = 0; i < count; i++)
    {
      inputKeys[i] = count - i;
      inputValues[i] = i;
    }

    uint32_t maxNumKeys = count;
    uint32_t maxNeededWgs = divCeil(maxNumKeys, BLOCK_SIZE);
    uint32_t countBufSize = maxNeededWgs * BIN_COUNT;
    uint32_t reducedBufSize = BIN_COUNT * divCeil(maxNeededWgs, BLOCK_SIZE) * BLOCK_SIZE;
    uint32_t sortingBits = 32;
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

    uniformsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = totalUniformBufferSize,
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });

    numKeysBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_NumKeys.buffer",
          .size = sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        });

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

    renderGraph->bufferWrite(keys, 0, count * sizeof(uint32_t), (void **)inputKeys);
    renderGraph->bufferWrite(values, 0, count * sizeof(uint32_t), (void **)inputValues);
    renderGraph->bufferWrite(numKeysBuffer, 0, sizeof(uint32_t), (void **)&count);

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
                               .buffer = numKeysBuffer,
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
                               .buffer = numKeysBuffer,
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
      uniformPtr->shift = pass * BITS_PER_PASS;
    }

    renderGraph->bufferWrite(uniformsBuffer, 0, totalUniformBufferSize, (void *)uniformData);

    for (uint32_t pass = 0; pass < numPasses; pass++)
    {
      auto currentBindingGroup = (pass % 2 == 0) ? bindingGroupA : bindingGroupB;

      uint32_t offset = alignedUniformSize * pass;
      uint32_t clearCount = countBufSize;
      uint32_t clearWgs = divCeil(clearCount, 256);

      commandBuffer.cmdBindComputePipeline(clearCountsPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(clearWgs, 1, 1);
      commandBuffer.cmdBindComputePipeline(sortCountPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(numWgs, 1, 1);
      commandBuffer.cmdBindComputePipeline(sortReducePipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(numReduceWgs, 1, 1);
      commandBuffer.cmdBindComputePipeline(sortScanPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(1, 1, 1);
      commandBuffer.cmdBindComputePipeline(sortScanAddPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(numReduceWgs, 1, 1);
      commandBuffer.cmdBindComputePipeline(sortScatterPipeline);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(numWgs, 1, 1);
    }

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
  const static uint32_t BITS_PER_PASS = 4;
  const static uint32_t BIN_COUNT = 1u << BITS_PER_PASS;

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
