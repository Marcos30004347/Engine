#include <cassert>
#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

using namespace rendering;
using namespace backend;

// Constants matching the WGSL shader
const static uint32_t WG = 256;
const static uint32_t ELEMENTS_PER_THREAD = 4;
const static uint32_t BLOCK_SIZE = WG * ELEMENTS_PER_THREAD; // 1024
const static uint32_t BITS_PER_PASS = 4;
const static uint32_t BIN_COUNT = 1u << BITS_PER_PASS; // 16

// Helper function for ceiling division
inline static uint32_t divCeil(uint32_t a, uint32_t b)
{
  return (a + b - 1u) / b;
}

// Helper function to align size to specified alignment
inline static uint32_t alignUp(uint32_t size, uint32_t alignment)
{
  return ((size + alignment - 1u) / alignment) * alignment;
}

// Uniforms structure matching the shader
struct SortUniforms
{
  uint32_t shift;
};

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = {
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
                            DeviceFeatures::DeviceFeatures_Timestamp;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});
  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  uint32_t uniformAlignment = rhi->GetProperties().uniformBufferAlignment;
  os::Logger::warningf("uniform buffer alignment: %u\n", uniformAlignment);

  std::string sortShader = os::io::readRelativeFile("assets/shaders/spirv/radixsort2.spirv");

  RenderGraph *renderGraph = new RenderGraph(rhi);

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

  Buffer keysA = renderGraph->createBuffer(
      BufferInfo{
        .name = "Keys_A.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer keysB = renderGraph->createBuffer(
      BufferInfo{
        .name = "Keys_B.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer valuesA = renderGraph->createBuffer(
      BufferInfo{
        .name = "Values_A.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer valuesB = renderGraph->createBuffer(
      BufferInfo{
        .name = "Values_B.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer uniformsBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Uniforms.buffer",
        .size = totalUniformBufferSize,
        .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
      });

  Buffer numKeysBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "NumKeys.buffer",
        .size = sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
      });

  Buffer countsBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Counts.buffer",
        .size = countBufSize * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer reducedBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Reduced.buffer",
        .size = reducedBufSize * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer debug2Buffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Debug2.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });

  Buffer debugBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "Debug.buffer",
        .size = count * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });

  renderGraph->bufferWrite(keysA, 0, count * sizeof(uint32_t), (void **)inputKeys);
  renderGraph->bufferWrite(valuesA, 0, count * sizeof(uint32_t), (void **)inputValues);
  renderGraph->bufferWrite(numKeysBuffer, 0, sizeof(uint32_t), (void **)&count);

  auto sortLayout = renderGraph->createBindingsLayout(
      BindingsLayoutInfo{
        .name = "radixSort.layout",
        .groups =
            {
              BindingGroupLayout{
                .buffers =
                    {
                      {
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
                      },
                      {
                        .name = "debug",
                        .binding = 8,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                    }},
            },
      });

  auto sortShaderHandle = renderGraph->createShader(
      ShaderInfo{
        .name = "radixSort.shader",
        .layout = sortLayout,
        .src = sortShader,
        .type = ShaderType::SpirV,
      });

  auto clearCountsPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "clear_counts",
        .layout = sortLayout,
        .name = "clear_counts.pipeline",
        .shader = sortShaderHandle,
      });

  auto sortCountPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "sort_count",
        .layout = sortLayout,
        .name = "sort_count.pipeline",
        .shader = sortShaderHandle,
      });

  auto sortReducePipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "sort_reduce",
        .layout = sortLayout,
        .name = "sort_reduce.pipeline",
        .shader = sortShaderHandle,
      });

  auto sortScanPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "sort_scan",
        .layout = sortLayout,
        .name = "sort_scan.pipeline",
        .shader = sortShaderHandle,
      });

  auto sortScanAddPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "sort_scan_add",
        .layout = sortLayout,
        .name = "sort_scan_add.pipeline",
        .shader = sortShaderHandle,
      });

  auto sortScatterPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "sort_scatter",
        .layout = sortLayout,
        .name = "sort_scatter.pipeline",
        .shader = sortShaderHandle,
      });

  uint32_t numWgs = divCeil(count, BLOCK_SIZE);
  uint32_t numReduceWgs = BIN_COUNT * divCeil(numWgs, BLOCK_SIZE);

  auto bindingGroupA = renderGraph
                           ->createBindingGroups(
                               BindingGroupsInfo{
                                 .layout = sortLayout,
                                 .name = "sortBindingGroup_A",
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
                                                      .buffer = keysA,
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
                                                      .buffer = valuesA,
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
                                               {.binding = 8,
                                                .bufferView =
                                                    {
                                                      .buffer = debug2Buffer,
                                                      .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                                      .offset = 0,
                                                      .size = count * sizeof(uint32_t),
                                                    }},
                                             },
                                       },
                                     },
                               });

  auto bindingGroupB = renderGraph
                           ->createBindingGroups(
                               BindingGroupsInfo{
                                 .layout = sortLayout,
                                 .name = "sortBindingGroup_B",
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
                                                      .buffer = keysA,
                                                      .access = AccessPattern::SHADER_WRITE,
                                                      .offset = 0,
                                                      .size = count * sizeof(uint32_t),
                                                    }},
                                               {.binding = 6,
                                                .bufferView =
                                                    {
                                                      .buffer = valuesA,
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
                                               {.binding = 8,
                                                .bufferView =
                                                    {
                                                      .buffer = debug2Buffer,
                                                      .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                                      .offset = 0,
                                                      .size = count * sizeof(uint32_t),
                                                    }},
                                             },
                                       },
                                     },
                               });

  auto clearCountsTimer = renderGraph->createTimer(TimerInfo{.name = "clearCountsTimer", .unit = TimerUnit::Miliseconds});
  auto sortCountTimer = renderGraph->createTimer(TimerInfo{.name = "sortCountTimer", .unit = TimerUnit::Miliseconds});
  auto sortReduceTimer = renderGraph->createTimer(TimerInfo{.name = "sortReduceTimer", .unit = TimerUnit::Miliseconds});
  auto sortScanTimer = renderGraph->createTimer(TimerInfo{.name = "sortScanTimer", .unit = TimerUnit::Miliseconds});
  auto sortScanAddTimer = renderGraph->createTimer(TimerInfo{.name = "sortScanAddTimer", .unit = TimerUnit::Miliseconds});
  auto sortScatterTimer = renderGraph->createTimer(TimerInfo{.name = "sortScatterTimer", .unit = TimerUnit::Miliseconds});
  auto totalTimer = renderGraph->createTimer(TimerInfo{.name = "totalTimer", .unit = TimerUnit::Miliseconds});

  RHICommandBuffer commandBuffer;

  bool finalResultInA = (numPasses % 2 == 0);

  uint8_t *uniformData = new uint8_t[totalUniformBufferSize];
  memset(uniformData, 0, totalUniformBufferSize);

  for (uint32_t pass = 0; pass < numPasses; pass++)
  {
    SortUniforms *uniformPtr = reinterpret_cast<SortUniforms *>(uniformData + pass * alignedUniformSize);
    uniformPtr->shift = pass * BITS_PER_PASS;
  }

  renderGraph->bufferWrite(uniformsBuffer, 0, totalUniformBufferSize, (void *)uniformData);

  commandBuffer.cmdStartTimer(totalTimer, PipelineStage::COMPUTE_SHADER);

  // Main radix sort loop
  for (uint32_t pass = 0; pass < numPasses; pass++)
  {
    // Alternate between binding groups
    auto currentBindingGroup = (pass % 2 == 0) ? bindingGroupA : bindingGroupB;

    uint32_t offset = alignedUniformSize * pass;
    uint32_t clearCount = countBufSize;
    uint32_t clearWgs = divCeil(clearCount, 256);

    // Clear counts
    commandBuffer.cmdBindComputePipeline(clearCountsPipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(clearCountsTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(clearWgs, 1, 1);
    commandBuffer.cmdStopTimer(clearCountsTimer, PipelineStage::COMPUTE_SHADER);

    // Count histogram
    commandBuffer.cmdBindComputePipeline(sortCountPipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(sortCountTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(numWgs, 1, 1);
    commandBuffer.cmdStopTimer(sortCountTimer, PipelineStage::COMPUTE_SHADER);

    // Reduce histogram
    commandBuffer.cmdBindComputePipeline(sortReducePipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(sortReduceTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(numReduceWgs, 1, 1);
    commandBuffer.cmdStopTimer(sortReduceTimer, PipelineStage::COMPUTE_SHADER);

    // Scan (prefix sum)
    commandBuffer.cmdBindComputePipeline(sortScanPipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(sortScanTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(1, 1, 1);
    commandBuffer.cmdStopTimer(sortScanTimer, PipelineStage::COMPUTE_SHADER);

    // Scan add
    commandBuffer.cmdBindComputePipeline(sortScanAddPipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(sortScanAddTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(numReduceWgs, 1, 1);
    commandBuffer.cmdStopTimer(sortScanAddTimer, PipelineStage::COMPUTE_SHADER);

    // Scatter
    commandBuffer.cmdBindComputePipeline(sortScatterPipeline);
    commandBuffer.cmdBindBindingGroups(currentBindingGroup, &offset, 1);
    commandBuffer.cmdStartTimer(sortScatterTimer, PipelineStage::COMPUTE_SHADER);
    commandBuffer.cmdDispatch(numWgs, 1, 1);
    commandBuffer.cmdStopTimer(sortScatterTimer, PipelineStage::COMPUTE_SHADER);
  }

  commandBuffer.cmdStopTimer(totalTimer, PipelineStage::COMPUTE_SHADER);

  // Copy final results to debug buffer
  // After even number of passes, result is in keysB; after odd, in keysA
  Buffer &finalKeysBuffer = finalResultInA ? keysA : keysB;

  commandBuffer.cmdCopyBuffer(
      BufferView{
        .buffer = finalKeysBuffer,
        .offset = 0,
        .size = count * sizeof(uint32_t),
        .access = AccessPattern::TRANSFER_READ,
      },
      BufferView{
        .buffer = debugBuffer,
        .offset = 0,
        .size = count * sizeof(uint32_t),
        .access = AccessPattern::TRANSFER_WRITE,
      });

  renderGraph->enqueuePass("RadixSort", commandBuffer);
  renderGraph->compile();

  RenderGraph::Frame frame;
  renderGraph->run(frame);
  renderGraph->waitFrame(frame);

  // Verify results
  renderGraph->bufferRead(
      debugBuffer,
      0,
      count * sizeof(uint32_t),
      [count](const void *data)
      {
        uint32_t *sortedKeys = (uint32_t *)data;
        for (uint32_t i = 0; i < count - 1; i++)
        {
          assert(sortedKeys[i + 1] > sortedKeys[i]);
        }
        os::Logger::logf("Sort verification passed for %u elements\n", count);
      });

  // Read and display timing results
  auto clearCountsBenchmark = renderGraph->readTimer(clearCountsTimer);
  auto sortCountBenchmark = renderGraph->readTimer(sortCountTimer);
  auto sortReduceBenchmark = renderGraph->readTimer(sortReduceTimer);
  auto sortScanBenchmark = renderGraph->readTimer(sortScanTimer);
  auto sortScanAddBenchmark = renderGraph->readTimer(sortScanAddTimer);
  auto sortScatterBenchmark = renderGraph->readTimer(sortScatterTimer);
  auto totalBenchmark = renderGraph->readTimer(totalTimer);

  os::Logger::warningf("\n=== RadixSort2 Benchmark Results ===");
  os::Logger::warningf("GPU took %fms to sort %u elements (%u passes)", totalBenchmark, count, numPasses);
  os::Logger::warningf("numWgs = %u, numReduceWgs = %u", numWgs, numReduceWgs);
  os::Logger::warningf("\nTotal times across all passes:");
  os::Logger::warningf("  clearCounts: %fms", clearCountsBenchmark);
  os::Logger::warningf("  sortCount: %fms", sortCountBenchmark);
  os::Logger::warningf("  sortReduce: %fms", sortReduceBenchmark);
  os::Logger::warningf("  sortScan: %fms", sortScanBenchmark);
  os::Logger::warningf("  sortScanAdd: %fms", sortScanAddBenchmark);
  os::Logger::warningf("  sortScatter: %fms", sortScatterBenchmark);

  // Cleanup
  delete[] inputKeys;
  delete[] inputValues;
  delete[] uniformData;
  renderGraph->deleteShader(sortShaderHandle);

  os::Logger::shutdown();
  return 0;
}