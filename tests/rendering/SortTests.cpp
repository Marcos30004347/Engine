#include <iostream>

#include "os/File.hpp"
#include "os/Logger.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

using namespace rendering;
using namespace backend;

const static uint32_t histogram_wg_size = 256;
const static uint32_t rs_histogram_block_rows = 14;
const static uint32_t histo_block_kvs = histogram_wg_size * rs_histogram_block_rows;
const static uint32_t rs_scatter_block_rows = rs_histogram_block_rows;
const static uint32_t scatter_block_kvs = histogram_wg_size * rs_scatter_block_rows;
const static uint32_t rs_radix_log2 = 8u;
const static uint32_t rs_radix_size = 1u << rs_radix_log2;
const static uint32_t rs_keyval_size = 32u / rs_radix_log2;

inline static uint32_t scatterBlocksCount(uint32_t n)
{
  return (n + scatter_block_kvs - 1) / scatter_block_kvs;
}

inline static uint32_t histogramBlocksCount(uint32_t n)
{
  uint64_t size = scatterBlocksCount(n) * scatter_block_kvs;
  return (size + histo_block_kvs - 1) / histo_block_kvs;
}

inline static uint32_t keysBufferSize(uint32_t n)
{
  return histogramBlocksCount(n) * histo_block_kvs;
}

// inline static Params GetInfo(uint32_t len)
// {
//   return {
//     .numKeys = len,
//     .paddedSize = KeysBufferSize(len),
//     .evenPass = 0,
//     .oddPass = 0,
//   };
// }

struct Params
{
  uint32_t numKeys;
  uint32_t paddedSize;
  uint32_t evenPass;
  uint32_t oddPass;
};

inline static Params GetInfo(uint32_t len)
{
  return {
    .numKeys = len,
    .paddedSize = keysBufferSize(len),
    .evenPass = 0,
    .oddPass = 0,
  };
}

int main()
{
  os::Logger::start();

  DeviceRequiredLimits limits = (DeviceRequiredLimits){
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});
  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  std::string sortShader = os::io::readRelativeFile("assets/shaders/spirv/radixsort.spirv");

  RenderGraph *renderGraph = new RenderGraph(rhi);

  uint32_t count = 1024;

  uint32_t data[count];

  for (uint32_t i = 0; i < count; i++)
  {
    data[i] = count - i;
  }
  uint32_t scatter_blocks = scatterBlocksCount(count);
  uint32_t padded_size = keysBufferSize(count);
  uint32_t histo_size = rs_radix_size * sizeof(uint32_t);
  uint32_t internal_size = (rs_keyval_size + scatter_blocks) * histo_size;

  Buffer keys = renderGraph->createBuffer(
      BufferInfo{
        .name = "Keys.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });
  Buffer payload = renderGraph->createBuffer(
      BufferInfo{
        .name = "Payload.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });
  Buffer keysAux = renderGraph->createBuffer(
      BufferInfo{
        .name = "KeysAux.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage,
      });
  Buffer payloadAux = renderGraph->createBuffer(
      BufferInfo{
        .name = "PayloadAux.buffer",
        .size = padded_size * sizeof(uint32_t),
        .usage = BufferUsage::BufferUsage_Storage,
      });
  Buffer infos = renderGraph->createBuffer(
      BufferInfo{
        .name = "Infos.buffer",
        .size = sizeof(Params),
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });
  Buffer histograms = renderGraph->createBuffer(
      BufferInfo{
        .name = "Histogram.buffer",
        .size = internal_size,
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      });
  Buffer debug = renderGraph->createBuffer(
      BufferInfo{
        .name = "Debug.buffer",
        .size = std::max(internal_size, padded_size),
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });

  Params params = GetInfo(count);

  renderGraph->bufferWrite(keys, 0, count * sizeof(uint32_t), (void **)&data);
  renderGraph->bufferWrite(payload, 0, count * sizeof(uint32_t), (void **)&data);
  renderGraph->bufferWrite(infos, 0, sizeof(Params), (void **)&params);

  auto radixSortLayout = renderGraph->createBindingsLayout(
      BindingsLayoutInfo{
        .name = "radixSortLayout.layout",
        .groups =
            {
              BindingGroupLayout{
                .buffers =
                    {
                      BindingGroupLayoutBufferEntry{
                        .name = "infos",
                        .binding = 0,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                      BindingGroupLayoutBufferEntry{
                        .name = "histograms",
                        .binding = 1,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                      BindingGroupLayoutBufferEntry{
                        .name = "keys_a",
                        .binding = 2,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                      BindingGroupLayoutBufferEntry{
                        .name = "keys_b",
                        .binding = 3,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                      BindingGroupLayoutBufferEntry{
                        .name = "payload_a",
                        .binding = 4,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                      BindingGroupLayoutBufferEntry{
                        .name = "payload_b",
                        .binding = 5,
                        .isDynamic = false,
                        .type = BufferBindingType::BufferBindingType_StorageBuffer,
                        .visibility = BindingVisibility::BindingVisibility_Compute,
                      },
                    }},
            },
      });

  auto radixSortShader = renderGraph->createShader(
      ShaderInfo{
        .name = "radixSortShader.shader",
        .layout = radixSortLayout,
        .src = sortShader,
        .type = ShaderType::SpirV,
      });

  auto radixSortBindingGroup = renderGraph->createBindingGroups(
      BindingGroupsInfo{
        .layout = radixSortLayout,
        .name = "radixSortBindingGroups",
        .groups =
            {
              GroupInfo{
                .name = "Group0",
                .buffers =
                    {
                      BindingBuffer{
                        .binding = 0,
                        .bufferView =
                            {
                              .buffer = infos,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = sizeof(Params),
                            }},
                      BindingBuffer{
                        .binding = 1,
                        .bufferView =
                            {
                              .buffer = histograms,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = internal_size,
                            }},
                      BindingBuffer{
                        .binding = 2,
                        .bufferView =
                            {
                              .buffer = keys,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = padded_size * sizeof(uint32_t),
                            }},
                      BindingBuffer{
                        .binding = 3,
                        .bufferView =
                            {
                              .buffer = keysAux,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = padded_size * sizeof(uint32_t),
                            }},
                      BindingBuffer{
                        .binding = 4,
                        .bufferView =
                            {
                              .buffer = payload,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = padded_size * sizeof(uint32_t),
                            }},
                      BindingBuffer{
                        .binding = 5,
                        .bufferView =
                            {
                              .buffer = payloadAux,
                              .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                              .offset = 0,
                              .size = padded_size * sizeof(uint32_t),
                            }},
                    },
              },
            },
      });

  auto radixSortZeroHistogramPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "zero_histograms",
        .layout = radixSortLayout,
        .name = "zero_histograms",
        .shader = radixSortShader,
      });
  auto radixSortCalculateHistogramPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "calculate_histogram",
        .layout = radixSortLayout,
        .name = "calculate_histogram",
        .shader = radixSortShader,
      });
  auto radixSortPrefixHistogramPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "prefix_histogram",
        .layout = radixSortLayout,
        .name = "prefix_histogram",
        .shader = radixSortShader,
      });
  auto radixSortScatterEvenPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "scatter_even",
        .layout = radixSortLayout,
        .name = "scatter_even",
        .shader = radixSortShader,
      });
  auto radixSortScatterOddPipeline = renderGraph->createComputePipeline(
      ComputePipelineInfo{
        .entry = "scatter_odd",
        .layout = radixSortLayout,
        .name = "scatter_odd",
        .shader = radixSortShader,
      });

  RHICommandBuffer commandBuffer;

  commandBuffer.cmdBindComputePipeline(radixSortZeroHistogramPipeline);
  commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, nullptr, 0);
  commandBuffer.cmdDispatch(histogramBlocksCount(count), 1, 1);
  commandBuffer.cmdBindComputePipeline(radixSortCalculateHistogramPipeline);
  commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, nullptr, 0);
  commandBuffer.cmdDispatch(histogramBlocksCount(count), 1, 1);
  commandBuffer.cmdBindComputePipeline(radixSortPrefixHistogramPipeline);
  commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, nullptr, 0);
  commandBuffer.cmdDispatch(sizeof(uint32_t), 1, 1);

  for (uint32_t i = 0; i < sizeof(uint32_t) / 2; i++)
  {
    commandBuffer.cmdBindComputePipeline(radixSortScatterEvenPipeline);
    commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, nullptr, 0);
    commandBuffer.cmdDispatch(scatterBlocksCount(count), 1, 1);
    commandBuffer.cmdBindComputePipeline(radixSortScatterOddPipeline);
    commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, nullptr, 0);
    commandBuffer.cmdDispatch(scatterBlocksCount(count), 1, 1);
  }

  commandBuffer.cmdCopyBuffer(
      BufferView{
        .buffer = keys,
        .offset = 0,
        .size = count * sizeof(uint32_t),
        .access = AccessPattern::SHADER_READ,
      },
      BufferView{
        .buffer = debug,
        .offset = 0,
        .size = count * sizeof(uint32_t),
        .access = AccessPattern::SHADER_WRITE,
      });

  renderGraph->enqueuePass("RadixSort", commandBuffer);
  renderGraph->compile();

  RenderGraph::Frame frame;

  renderGraph->run(frame);
  renderGraph->waitFrame(frame);

  renderGraph->bufferRead(
      debug,
      0,
      count * sizeof(uint32_t),
      [count](const void *data)
      {
        uint32_t *values = (uint32_t *)data;
        for (uint32_t i = 0; i < count; i++)
        {
          os::print("%u ", values[i]);
        }
      });
  os::print("\n");

  //   renderGraph->deleteComputePipeline(radixSortPipeline);
  //   renderGraph->deleteBindingGroups(radixSortBindingGroup);
  //   renderGraph->deleteBindingsLayout(radixSortLayout);
  //   renderGraph->deleteBuffer(valuesBuffer);
  //   renderGraph->deleteBuffer(uniformBuffer);
  //   renderGraph->deleteBuffer(pullBuffer);

  renderGraph->deleteShader(radixSortShader);

  os::Logger::shutdown();

  return 0;
}