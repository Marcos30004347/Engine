#pragma once
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

template <typename K, typename V> class RadixSortPass : public Pass
{
  static_assert(std::is_same<K, uint32_t>::value, "K must be uint32_t");
  static_assert(std::is_same<V, uint32_t>::value, "V must be uint32_t");
  rendering::Shader radixSortShader;

  rendering::Buffer infos;
  rendering::Buffer keysAux;
  rendering::Buffer payloadAux;
  rendering::Buffer histograms;

  rendering::ComputePipeline radixSortZeroHistogramPipeline;
  rendering::ComputePipeline radixSortCalculateHistogramPipeline;
  rendering::ComputePipeline radixSortPrefixHistogramPipeline;
  rendering::ComputePipeline radixSortScatterEvenPipeline;
  rendering::ComputePipeline radixSortScatterOddPipeline;

  rendering::BindingsLayout radixSortLayout;
  rendering::BindingGroups radixSortBindingGroup;

  uint32_t count;
  rendering::Buffer keys;
  rendering::Buffer payload;

public:
  RadixSortPass(uint32_t count, const Buffer &keys, const Buffer &payload) : count(count), keys(keys), payload(payload)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;

    auto sortShader = os::io::readRelativeFile("assets/shaders/spirv/radixsort.spirv");

    radixSortLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_radixSortLayout.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        BindingGroupLayoutBufferEntry{
                          .name = "infos",
                          .binding = 0,
                          .isDynamic = true,
                          .type = BufferBindingType::BufferBindingType_UniformBuffer,
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

    radixSortShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_radixSortShader.shader",
          .layout = radixSortLayout,
          .src = sortShader,
          .type = ShaderType::SpirV,
        });

    uint32_t alignedUniformSize = alignUp(sizeof(Params), uniformAlignment);
    uint32_t numPasses = ceilDiv2(bytesNeeded(UINT32_MAX));
    uint32_t totalUniformBufferSize = alignedUniformSize * numPasses * 2;

    infos = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "Infos.buffer",
          .size = totalUniformBufferSize,
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    uint32_t scatter_blocks = scatterBlocksCount(count);
    uint32_t padded_size = keysBufferSize(count);
    uint32_t histo_size = rs_radix_size * sizeof(uint32_t);
    uint32_t internal_size = (rs_keyval_size + scatter_blocks) * histo_size;

    keysAux = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "KeysAux.buffer",
          .size = padded_size * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage,
        });
    payloadAux = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "PayloadAux.buffer",
          .size = padded_size * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage,
        });

    histograms = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "Histogram.buffer",
          .size = internal_size,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    Params params = GetInfo(count);

    uint8_t *uniformData = new uint8_t[totalUniformBufferSize];
    memset(uniformData, 0, totalUniformBufferSize);

    for (uint32_t pass = 0; pass < numPasses * 2; pass++)
    {
      Params *uniformPtr = reinterpret_cast<Params *>(uniformData + pass * alignedUniformSize);
      *uniformPtr = GetInfo(count);
      uniformPtr->iter = pass;
    }

    Params *prev = reinterpret_cast<Params *>(uniformData);

    prev->oddPass = 0;
    prev->evenPass = 0;

    for (uint32_t pass = 1; pass < numPasses * 2; pass++)
    {
      Params *uniformPtr = reinterpret_cast<Params *>(uniformData + pass * alignedUniformSize);

      uniformPtr->oddPass = (pass % 2 == 0) ? (prev->oddPass + 1) % 2 : prev->oddPass;
      uniformPtr->evenPass = (pass % 2 == 1) ? (prev->evenPass + 1) % 2 : prev->evenPass;
      prev = uniformPtr;
    }

    renderGraph->bufferWrite(infos, 0, totalUniformBufferSize, (void **)uniformData);

    radixSortBindingGroup = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = radixSortLayout,
          .name = passName + "_radixSortBindingGroups",
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
                                .size = alignedUniformSize,
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

    radixSortZeroHistogramPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "zero_histograms",
          .layout = radixSortLayout,
          .name = passName + "_zero_histograms",
          .shader = radixSortShader,
        });
    radixSortCalculateHistogramPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "calculate_histogram",
          .layout = radixSortLayout,
          .name = passName + "_calculate_histogram",
          .shader = radixSortShader,
        });
    radixSortPrefixHistogramPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "prefix_histogram",
          .layout = radixSortLayout,
          .name = passName + "_prefix_histogram",
          .shader = radixSortShader,
        });
    radixSortScatterEvenPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "scatter_even",
          .layout = radixSortLayout,
          .name = passName + "_catter_even",
          .shader = radixSortShader,
        });
    radixSortScatterOddPipeline = renderGraph->createComputePipeline(
        ComputePipelineInfo{
          .entry = "scatter_odd",
          .layout = radixSortLayout,
          .name = passName + "_scatter_odd",
          .shader = radixSortShader,
        });

    uint32_t offset = 0;
    commandBuffer.cmdBindComputePipeline(radixSortZeroHistogramPipeline);
    commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, &offset, 1);
    commandBuffer.cmdDispatch(histogramBlocksCount(count), 1, 1);
    commandBuffer.cmdBindComputePipeline(radixSortCalculateHistogramPipeline);
    commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, &offset, 1);
    commandBuffer.cmdDispatch(histogramBlocksCount(count), 1, 1);
    commandBuffer.cmdBindComputePipeline(radixSortPrefixHistogramPipeline);
    commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, &offset, 1);
    commandBuffer.cmdDispatch(sizeof(uint32_t), 1, 1);

    for (uint32_t i = 0; i < ceilDiv2(bytesNeeded(UINT32_MAX)); i++)
    {
      commandBuffer.cmdBindComputePipeline(radixSortScatterEvenPipeline);
      commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(scatterBlocksCount(count), 1, 1);
      offset += alignedUniformSize;
      commandBuffer.cmdBindComputePipeline(radixSortScatterOddPipeline);
      commandBuffer.cmdBindBindingGroups(radixSortBindingGroup, &offset, 1);
      commandBuffer.cmdDispatch(scatterBlocksCount(count), 1, 1);
      offset += alignedUniformSize;
    }
  }

  ~RadixSortPass()
  {
    renderGraph->deleteComputePipeline(radixSortZeroHistogramPipeline);
    renderGraph->deleteComputePipeline(radixSortCalculateHistogramPipeline);
    renderGraph->deleteComputePipeline(radixSortPrefixHistogramPipeline);
    renderGraph->deleteComputePipeline(radixSortScatterEvenPipeline);
    renderGraph->deleteComputePipeline(radixSortScatterOddPipeline);
    renderGraph->deleteBindingsLayout(radixSortLayout);
    renderGraph->deleteBindingGroups(radixSortBindingGroup);
    renderGraph->deleteBuffer(infos);
    renderGraph->deleteBuffer(keysAux);
    renderGraph->deleteBuffer(payloadAux);
    renderGraph->deleteBuffer(histograms);
    renderGraph->deleteShader(radixSortShader);
  }

  inline static uint32_t keysBufferSize(uint32_t n)
  {
    return histogramBlocksCount(n) * histo_block_kvs;
  }

private:
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

  struct Params
  {
    uint32_t numKeys;
    uint32_t paddedSize;
    uint32_t evenPass;
    uint32_t oddPass;
    uint32_t iter;
    uint32_t padd[3];
  };

  inline static Params GetInfo(uint32_t len)
  {
    return {
      .numKeys = len,
      .paddedSize = keysBufferSize(len),
      .evenPass = 0,
      .oddPass = 0,
      .iter = 0,
    };
  }
  inline constexpr uint32_t bytesNeeded(uint32_t v)
  {
    return (v <= 0xFFu) ? 1 : (v <= 0xFFFFu) ? 2 : (v <= 0xFFFFFFu) ? 3 : 4;
  }

  inline uint32_t ceilDiv2(uint32_t x)
  {
    return (x + 1) / 2;
  }

  inline static uint32_t alignUp(uint32_t size, uint32_t alignment)
  {
    return ((size + alignment - 1u) / alignment) * alignment;
  }
};

} // namespace gpgpu
} // namespace rendering
