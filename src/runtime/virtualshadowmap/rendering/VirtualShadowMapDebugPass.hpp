#pragma once

#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "time/TimeSpan.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace virtualgeometry
{
namespace gpgpu
{

namespace detail
{

inline rendering::TextureView makeDebugSampledDepthView(rendering::Texture texture)
{
  return rendering::TextureView{
    .texture = texture,
    .access = rendering::AccessPattern::SHADER_READ,
    .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
    .index = 0,
    .flags = rendering::ImageAspectFlags::Depth,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };
}

inline rendering::TextureView makeDebugStorageColorView(rendering::Texture texture)
{
  return rendering::TextureView{
    .texture = texture,
    .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
    .layout = rendering::ResourceLayout::GENERAL,
    .index = 0,
    .flags = rendering::ImageAspectFlags::Color,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };
}

inline rendering::TextureView makeDebugSampledColorView(rendering::Texture texture)
{
  return rendering::TextureView{
    .texture = texture,
    .access = rendering::AccessPattern::SHADER_READ,
    .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
    .index = 0,
    .flags = rendering::ImageAspectFlags::Color,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };
}

inline rendering::Texture makeDebugFallbackSampledTexture(rendering::RenderGraph *renderGraph, const std::string &name)
{
  return renderGraph->createTexture(
      rendering::TextureInfo{
          .name = name,
          .width = 1u,
          .height = 1u,
          .depth = 1u,
          .mipLevels = 1u,
          .format = rendering::Format::Format_RGBA16Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Sampled,
      });
}

constexpr uint32_t kVsmShadowFilterModePcf = 1u;
constexpr uint32_t kVsmScreenSpaceShadowMaxDispatches = 8u;
constexpr uint32_t kVsmScreenSpaceShadowWaveSize = 64u;

inline const std::string &getCachedVsmShadowDepthSpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-shadow-depth.spirv");
  return spirv;
}

inline const std::string &getCachedVsmShadowVisibilitySpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-shadow-visibility.spirv");
  return spirv;
}

inline const std::string &getCachedVsmShadowCompareSpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-shadow-compare.spirv");
  return spirv;
}

inline const std::string &getCachedVsmShadowRsmssSpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-shadow-rsmss.spirv");
  return spirv;
}

inline const std::string &getCachedVsmShadowMaskPcfSpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-shadow-mask-pcf.spirv");
  return spirv;
}

inline const std::string &getCachedVsmScreenSpaceShadowSpirv()
{
  static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-screen-space-shadow.spirv");
  return spirv;
}

inline void logPassSetupStep(const std::string &passName, const char *stepName, const lib::time::TimeSpan &stepStart)
{
  const double elapsedMs = (lib::time::TimeSpan::now() - stepStart).milliseconds();
  os::print("[VSMSetup] %s %s took %.2f ms\n", passName.c_str(), stepName, elapsedMs);
}

inline uint32_t sanitizeShadowFilterTapCount(uint32_t tapCount)
{
  switch (tapCount)
  {
  case 1u:
  case 4u:
  case 8u:
  case 9u:
  case 16u:
    return tapCount;
  default:
    return 1u;
  }
}

inline uint32_t sanitizeShadowPcfTapCount(uint32_t tapCount)
{
  switch (tapCount)
  {
  case 1u:
  case 4u:
  case 9u:
  case 16u:
    return tapCount;
  default:
    return 4u;
  }
}

inline uint32_t sanitizeRbssmBlockerSearchTapCount(uint32_t tapCount)
{
  if (tapCount == 0u)
  {
    return 1u;
  }

  return (tapCount % 2u == 0u) ? (tapCount + 1u) : tapCount;
}

struct VirtualShadowMapDebugUniforms
{
  uint32_t pageTableResolution = 0u;
  uint32_t activeLayers = 0u;
  uint32_t cascadeCount = 0u;
  uint32_t enabled = 0u;
  int32_t debugLayer = -1;
  uint32_t _padding0[3] = {0u, 0u, 0u};
  float firstCascadeWorldExtent = 0.0f;
  float _padding1[3] = {0.0f, 0.0f, 0.0f};
};

struct VirtualShadowMapShadowMaskUniforms
{
  uint32_t pageTableResolution = 0u;
  uint32_t activeLayers = 0u;
  uint32_t cascadeCount = 0u;
  uint32_t enabled = 0u;
  uint32_t physicalAtlasResolution = 0u;
  uint32_t physicalPageSize = 0u;
  uint32_t reverseZ = 0u;
  uint32_t fallbackCascadeOffset = 0u;
  float firstCascadeWorldExtent = 0.0f;
  float shadowBias = 0.0015f;
  float slopeScaleBias = 1.0f;
  float maxShadowBias = 0.006f;
  uint32_t shadowFilterTaps = 1u;
  uint32_t rbssmBlockerSearchTaps = 5u;
  uint32_t shadowFilterMode = 0u;
  uint32_t _padding3 = 0u;
  float rbsmDepthThreshold = 0.001f;
  float rbssmLightRadiusTexels = 1.5f;
  float rbssmMaxPenumbraTexels = 12.0f;
  float rbssmPenumbraScale = 1.0f;
  float pcfMinRadiusTexels = 0.75f;
  float pcfMaxRadiusTexels = 8.0f;
  float normalBiasTexels = 0.5f;
  float _padding4[1] = {0.0f};
};

struct VirtualShadowMapShadowPcfUniforms
{
  uint32_t pageTableResolution = 0u;
  uint32_t activeLayers = 0u;
  uint32_t cascadeCount = 0u;
  uint32_t activeDirectionalLights = 0u;
  uint32_t enabled = 0u;
  uint32_t physicalPageSize = 0u;
  uint32_t reverseZ = 0u;
  uint32_t fallbackCascadeOffset = 0u;
  float firstCascadeWorldExtent = 0.0f;
  float shadowBias = 0.0015f;
  float slopeScaleBias = 1.0f;
  float maxShadowBias = 0.006f;
  float pcfRadiusTexels = 1.5f;
  float normalBiasTexels = 0.5f;
  float contactShadowDistance = 4.0f;
  float contactShadowThickness = 0.5f;
  float ambientShadowColor[3] = {0.18f, 0.20f, 0.24f};
  float contactShadowIntensity = 1.0f;
  uint32_t shadowFilterTaps = 1u;
  uint32_t contactShadowSamples = 0u;
  uint32_t screenSpaceShadowEnabled = 0u;
  float contactShadowStartBias = 0.05f;
  float _padding0[2] = {0.0f, 0.0f};
};

struct VirtualShadowMapScreenSpaceShadowUniforms
{
  alignas(16) float lightCoordinate[4] = {0.0f, 0.0f, 0.0f, -1.0f};
  int32_t waveOffset[2] = {0, 0};
  float depthBounds[2] = {0.0f, 1.0f};
  float surfaceThickness = 0.005f;
  float bilinearThreshold = 0.02f;
  float shadowContrast = 4.0f;
  float rayDistance = 60.0f;
  float farDepthValue = 0.0f;
  float nearDepthValue = 1.0f;
  uint32_t ignoreEdgePixels = 0u;
  uint32_t usePrecisionOffset = 0u;
  uint32_t bilinearSamplingOffsetMode = 0u;
  uint32_t debugOutputEdgeMask = 0u;
  uint32_t debugOutputThreadIndex = 0u;
  uint32_t debugOutputWaveIndex = 0u;
  uint32_t useEarlyOut = 1u;
  uint32_t treatSkippedEdgeSamplesAsLit = 1u;
  uint32_t _padding0 = 0u;
};
static_assert(sizeof(VirtualShadowMapScreenSpaceShadowUniforms) == 96u, "VirtualShadowMapScreenSpaceShadowUniforms size mismatch");

struct VirtualShadowMapDispatchIndirectArgs
{
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t z = 0u;
};
static_assert(sizeof(VirtualShadowMapDispatchIndirectArgs) == 12u, "VirtualShadowMapDispatchIndirectArgs size mismatch");

struct VirtualShadowMapScreenSpaceShadowDispatchData
{
  uint32_t waveCount[3] = {0u, 0u, 0u};
  int32_t waveOffset[2] = {0, 0};
};

struct VirtualShadowMapScreenSpaceShadowDispatchList
{
  float lightCoordinate[4] = {0.0f, 0.0f, 0.0f, -1.0f};
  std::array<VirtualShadowMapScreenSpaceShadowDispatchData, kVsmScreenSpaceShadowMaxDispatches> dispatches{};
  uint32_t dispatchCount = 0u;
};

inline int32_t sssMin(int32_t a, int32_t b)
{
  return a > b ? b : a;
}

inline int32_t sssMax(int32_t a, int32_t b)
{
  return a > b ? a : b;
}

inline VirtualShadowMapScreenSpaceShadowDispatchList buildScreenSpaceShadowDispatchList(
    const math::Vec4f &lightProjection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool expandedZRange = false,
    int32_t waveSize = static_cast<int32_t>(kVsmScreenSpaceShadowWaveSize))
{
  VirtualShadowMapScreenSpaceShadowDispatchList result{};
  const int32_t minRenderBounds[2] = {0, 0};
  const int32_t maxRenderBounds[2] = {static_cast<int32_t>(viewportWidth), static_cast<int32_t>(viewportHeight)};

  float xyLightW = lightProjection[3];
  const float fpLimit = 0.000002f * static_cast<float>(waveSize);

  if (xyLightW >= 0.0f && xyLightW < fpLimit)
  {
    xyLightW = fpLimit;
  }
  else if (xyLightW < 0.0f && xyLightW > -fpLimit)
  {
    xyLightW = -fpLimit;
  }

  result.lightCoordinate[0] = ((lightProjection[0] / xyLightW) * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
  result.lightCoordinate[1] = ((lightProjection[1] / xyLightW) * -0.5f + 0.5f) * static_cast<float>(viewportHeight);
  result.lightCoordinate[2] = lightProjection[3] == 0.0f ? 0.0f : (lightProjection[2] / lightProjection[3]);
  result.lightCoordinate[3] = lightProjection[3] > 0.0f ? 1.0f : -1.0f;

  if (expandedZRange)
  {
    result.lightCoordinate[2] = result.lightCoordinate[2] * 0.5f + 0.5f;
  }

  const int32_t lightXY[2] = {
    static_cast<int32_t>(result.lightCoordinate[0] + 0.5f),
    static_cast<int32_t>(result.lightCoordinate[1] + 0.5f),
  };

  const int32_t biasedBounds[4] = {
    minRenderBounds[0] - lightXY[0],
    -(maxRenderBounds[1] - lightXY[1]),
    maxRenderBounds[0] - lightXY[0],
    -(minRenderBounds[1] - lightXY[1]),
  };

  for (int32_t q = 0; q < 4; ++q)
  {
    const bool vertical = q == 0 || q == 3;
    const int32_t bounds[4] = {
      sssMax(0, ((q & 1) ? biasedBounds[0] : -biasedBounds[2])) / waveSize,
      sssMax(0, ((q & 2) ? biasedBounds[1] : -biasedBounds[3])) / waveSize,
      sssMax(0, (((q & 1) ? biasedBounds[2] : -biasedBounds[0]) + waveSize * (vertical ? 1 : 2) - 1)) / waveSize,
      sssMax(0, (((q & 2) ? biasedBounds[3] : -biasedBounds[1]) + waveSize * (vertical ? 2 : 1) - 1)) / waveSize,
    };

    if ((bounds[2] - bounds[0]) <= 0 || (bounds[3] - bounds[1]) <= 0)
    {
      continue;
    }

    if (result.dispatchCount >= result.dispatches.size())
    {
      break;
    }

    const int32_t biasX = (q == 2 || q == 3) ? 1 : 0;
    const int32_t biasY = (q == 1 || q == 3) ? 1 : 0;

    auto &dispatch = result.dispatches[result.dispatchCount++];
    dispatch.waveCount[0] = static_cast<uint32_t>(waveSize);
    dispatch.waveCount[1] = static_cast<uint32_t>(bounds[2] - bounds[0]);
    dispatch.waveCount[2] = static_cast<uint32_t>(bounds[3] - bounds[1]);
    dispatch.waveOffset[0] = ((q & 1) ? bounds[0] : -bounds[2]) + biasX;
    dispatch.waveOffset[1] = ((q & 2) ? -bounds[3] : bounds[1]) + biasY;

    int32_t axisDelta = biasedBounds[0] - biasedBounds[1];
    if (q == 1)
    {
      axisDelta = biasedBounds[2] + biasedBounds[1];
    }
    else if (q == 2)
    {
      axisDelta = -biasedBounds[0] - biasedBounds[3];
    }
    else if (q == 3)
    {
      axisDelta = -biasedBounds[2] + biasedBounds[3];
    }

    axisDelta = (axisDelta + waveSize - 1) / waveSize;
    if (axisDelta <= 0 || result.dispatchCount >= result.dispatches.size())
    {
      continue;
    }

    auto &dispatchSplit = result.dispatches[result.dispatchCount++];
    dispatchSplit = dispatch;

    if (q == 0)
    {
      dispatchSplit.waveCount[2] = static_cast<uint32_t>(sssMin(static_cast<int32_t>(dispatch.waveCount[2]), axisDelta));
      dispatch.waveCount[2] -= dispatchSplit.waveCount[2];
      dispatchSplit.waveOffset[1] = dispatch.waveOffset[1] + static_cast<int32_t>(dispatch.waveCount[2]);
      dispatchSplit.waveOffset[0]--;
      dispatchSplit.waveCount[1]++;
    }
    else if (q == 1)
    {
      dispatchSplit.waveCount[1] = static_cast<uint32_t>(sssMin(static_cast<int32_t>(dispatch.waveCount[1]), axisDelta));
      dispatch.waveCount[1] -= dispatchSplit.waveCount[1];
      dispatchSplit.waveOffset[0] = dispatch.waveOffset[0] + static_cast<int32_t>(dispatch.waveCount[1]);
      dispatchSplit.waveCount[2]++;
    }
    else if (q == 2)
    {
      dispatchSplit.waveCount[1] = static_cast<uint32_t>(sssMin(static_cast<int32_t>(dispatch.waveCount[1]), axisDelta));
      dispatch.waveCount[1] -= dispatchSplit.waveCount[1];
      dispatch.waveOffset[0] += static_cast<int32_t>(dispatchSplit.waveCount[1]);
      dispatchSplit.waveCount[2]++;
      dispatchSplit.waveOffset[1]--;
    }
    else
    {
      dispatchSplit.waveCount[2] = static_cast<uint32_t>(sssMin(static_cast<int32_t>(dispatch.waveCount[2]), axisDelta));
      dispatch.waveCount[2] -= dispatchSplit.waveCount[2];
      dispatch.waveOffset[1] += static_cast<int32_t>(dispatchSplit.waveCount[2]);
      dispatchSplit.waveCount[1]++;
    }

    if (dispatchSplit.waveCount[1] == 0u || dispatchSplit.waveCount[2] == 0u)
    {
      dispatchSplit = result.dispatches[--result.dispatchCount];
    }
    if (dispatch.waveCount[1] == 0u || dispatch.waveCount[2] == 0u)
    {
      dispatch = result.dispatches[--result.dispatchCount];
    }
  }

  for (uint32_t i = 0u; i < result.dispatchCount; ++i)
  {
    result.dispatches[i].waveOffset[0] *= waveSize;
    result.dispatches[i].waveOffset[1] *= waveSize;
  }

  return result;
}

} // namespace detail

class VirtualShadowMapPagesDebugPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    int32_t debugLayer = -1;
  };

  VirtualShadowMapPagesDebugPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapPagesDebugPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapDebugUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageState", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapDebugUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getVirtualPageStateBuffer(), .offset = 0, .size = manager_.getVirtualPageStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 7, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-debug.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "pages_debug_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void writeUniforms()
  {
    const detail::VirtualShadowMapDebugUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .debugLayer = settings_.debugLayer,
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapDebugUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapTableDebugPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    int32_t debugLayer = -1;
  };

  VirtualShadowMapTableDebugPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapTableDebugPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapDebugUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageState", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapDebugUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getVirtualPageStateBuffer(), .offset = 0, .size = manager_.getVirtualPageStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 7, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-debug.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "table_debug_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void writeUniforms()
  {
    const detail::VirtualShadowMapDebugUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .debugLayer = settings_.debugLayer,
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapDebugUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapShadowDepthPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float shadowBias = 0.0015f;
    float slopeScaleBias = 1.0f;
    float maxShadowBias = 0.006f;
    uint32_t shadowFilterTaps = 1u;
    uint32_t rbssmBlockerSearchTaps = 5u;
    uint32_t shadowFilterMode = 0u;
    float rbsmDepthThreshold = 0.001f;
    float rbssmLightRadiusTexels = 1.5f;
    float rbssmMaxPenumbraTexels = 12.0f;
    float rbssmPenumbraScale = 1.0f;
    float pcfMinRadiusTexels = 0.75f;
    float pcfMaxRadiusTexels = 8.0f;
    float normalBiasTexels = 0.5f;
  };

  VirtualShadowMapShadowDepthPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapShadowDepthPass() override
  {
    destroy();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureDummyTextures();
    createSharedResources();
    createBindings(dummyDiscontinuityTexture_, dummyOndsTexture_, dummyVisibilityTexture_, dummyRsmssTexture_);
    createPipeline();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void destroy()
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
    if (dummyDiscontinuityTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyDiscontinuityTexture_);
    }
    if (dummyOndsTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyOndsTexture_);
    }
    if (dummyVisibilityTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyVisibilityTexture_);
    }
    if (dummyRsmssTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyRsmssTexture_);
    }
  }

  void ensureDummyTextures()
  {
    if (!dummyDiscontinuityTexture_.isValid())
    {
      dummyDiscontinuityTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyDiscontinuity.texture");
    }
    if (!dummyOndsTexture_.isValid())
    {
      dummyOndsTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyOnds.texture");
    }
    if (!dummyVisibilityTexture_.isValid())
    {
      dummyVisibilityTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyVisibility.texture");
    }
    if (!dummyRsmssTexture_.isValid())
    {
      dummyRsmssTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyRsmss.texture");
    }
  }

  void createSharedResources()
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowAtlasTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "discontinuityTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "ondsTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "visibilityTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "rsmssTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });
  }

  void createBindings(
      rendering::Texture discontinuityTexture,
      rendering::Texture ondsTexture,
      rendering::Texture visibilityTexture,
      rendering::Texture rsmssTexture)
  {
    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                        {.binding = 6, .textureView = detail::makeDebugSampledColorView(manager_.getShadowAtlasTexture())},
                        {.binding = 7, .textureView = detail::makeDebugSampledColorView(discontinuityTexture)},
                        {.binding = 8, .textureView = detail::makeDebugSampledColorView(ondsTexture)},
                        {.binding = 9, .textureView = detail::makeDebugSampledColorView(visibilityTexture)},
                        {.binding = 10, .textureView = detail::makeDebugSampledColorView(rsmssTexture)},
                    },
                    .storageTextures = {
                        {.binding = 11, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });
  }

  void createPipeline()
  {
    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmShadowDepthSpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeUniforms()
  {
    const detail::VirtualShadowMapShadowMaskUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .shadowBias = settings_.shadowBias,
      .slopeScaleBias = settings_.slopeScaleBias,
      .maxShadowBias = settings_.maxShadowBias,
      .shadowFilterTaps = detail::sanitizeShadowFilterTapCount(settings_.shadowFilterTaps),
      .rbssmBlockerSearchTaps = detail::sanitizeRbssmBlockerSearchTapCount(settings_.rbssmBlockerSearchTaps),
      .shadowFilterMode = settings_.shadowFilterMode,
      .rbsmDepthThreshold = settings_.rbsmDepthThreshold,
      .rbssmLightRadiusTexels = settings_.rbssmLightRadiusTexels,
      .rbssmMaxPenumbraTexels = settings_.rbssmMaxPenumbraTexels,
      .rbssmPenumbraScale = settings_.rbssmPenumbraScale,
      .pcfMinRadiusTexels = settings_.pcfMinRadiusTexels,
      .pcfMaxRadiusTexels = settings_.pcfMaxRadiusTexels,
      .normalBiasTexels = settings_.normalBiasTexels,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapShadowMaskUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::Texture dummyDiscontinuityTexture_;
  rendering::Texture dummyOndsTexture_;
  rendering::Texture dummyVisibilityTexture_;
  rendering::Texture dummyRsmssTexture_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapShadowVisibilityPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float shadowBias = 0.0015f;
    float slopeScaleBias = 1.0f;
    float maxShadowBias = 0.006f;
    uint32_t shadowFilterTaps = 1u;
    uint32_t rbssmBlockerSearchTaps = 5u;
    uint32_t shadowFilterMode = 0u;
    float rbsmDepthThreshold = 0.001f;
    float rbssmLightRadiusTexels = 1.5f;
    float rbssmMaxPenumbraTexels = 12.0f;
    float rbssmPenumbraScale = 1.0f;
    float pcfMinRadiusTexels = 0.75f;
    float pcfMaxRadiusTexels = 8.0f;
    float normalBiasTexels = 0.5f;
  };

  VirtualShadowMapShadowVisibilityPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture discontinuityTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), discontinuityTexture_(discontinuityTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapShadowVisibilityPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
    if (dummyOndsTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyOndsTexture_);
    }
    if (dummyVisibilityTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyVisibilityTexture_);
    }
    if (dummyRsmssTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyRsmssTexture_);
    }
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!dummyOndsTexture_.isValid())
    {
      dummyOndsTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyOnds.texture");
    }
    if (!dummyVisibilityTexture_.isValid())
    {
      dummyVisibilityTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyVisibility.texture");
    }
    if (!dummyRsmssTexture_.isValid())
    {
      dummyRsmssTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyRsmss.texture");
    }

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowAtlasTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "discontinuityTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "ondsTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "visibilityTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "rsmssTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                        {.binding = 6, .textureView = detail::makeDebugSampledColorView(manager_.getShadowAtlasTexture())},
                        {.binding = 7, .textureView = detail::makeDebugSampledColorView(discontinuityTexture_)},
                        {.binding = 8, .textureView = detail::makeDebugSampledColorView(dummyOndsTexture_)},
                        {.binding = 9, .textureView = detail::makeDebugSampledColorView(dummyVisibilityTexture_)},
                        {.binding = 10, .textureView = detail::makeDebugSampledColorView(dummyRsmssTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 11, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmShadowVisibilitySpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void writeUniforms()
  {
    const detail::VirtualShadowMapShadowMaskUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .shadowBias = settings_.shadowBias,
      .slopeScaleBias = settings_.slopeScaleBias,
      .maxShadowBias = settings_.maxShadowBias,
      .shadowFilterTaps = detail::sanitizeShadowFilterTapCount(settings_.shadowFilterTaps),
      .rbssmBlockerSearchTaps = detail::sanitizeRbssmBlockerSearchTapCount(settings_.rbssmBlockerSearchTaps),
      .shadowFilterMode = settings_.shadowFilterMode,
      .rbsmDepthThreshold = settings_.rbsmDepthThreshold,
      .rbssmLightRadiusTexels = settings_.rbssmLightRadiusTexels,
      .rbssmMaxPenumbraTexels = settings_.rbssmMaxPenumbraTexels,
      .rbssmPenumbraScale = settings_.rbssmPenumbraScale,
      .pcfMinRadiusTexels = settings_.pcfMinRadiusTexels,
      .pcfMaxRadiusTexels = settings_.pcfMaxRadiusTexels,
      .normalBiasTexels = settings_.normalBiasTexels,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapShadowMaskUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture discontinuityTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::Texture dummyOndsTexture_;
  rendering::Texture dummyVisibilityTexture_;
  rendering::Texture dummyRsmssTexture_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapShadowComparePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float shadowBias = 0.0015f;
    float slopeScaleBias = 1.0f;
    float maxShadowBias = 0.006f;
    uint32_t shadowFilterTaps = 1u;
    uint32_t rbssmBlockerSearchTaps = 5u;
    uint32_t shadowFilterMode = 0u;
    float rbsmDepthThreshold = 0.001f;
    float rbssmLightRadiusTexels = 1.5f;
    float rbssmMaxPenumbraTexels = 12.0f;
    float rbssmPenumbraScale = 1.0f;
    float pcfMinRadiusTexels = 0.75f;
    float pcfMaxRadiusTexels = 8.0f;
    float normalBiasTexels = 0.5f;
  };

  VirtualShadowMapShadowComparePass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture discontinuityTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), discontinuityTexture_(discontinuityTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapShadowComparePass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
    if (dummyOndsTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyOndsTexture_);
    }
    if (dummyVisibilityTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyVisibilityTexture_);
    }
    if (dummyRsmssTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyRsmssTexture_);
    }
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!dummyOndsTexture_.isValid())
    {
      dummyOndsTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyOnds.texture");
    }
    if (!dummyVisibilityTexture_.isValid())
    {
      dummyVisibilityTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyVisibility.texture");
    }
    if (!dummyRsmssTexture_.isValid())
    {
      dummyRsmssTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyRsmss.texture");
    }

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowAtlasTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "discontinuityTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "ondsTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "visibilityTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "rsmssTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                        {.binding = 6, .textureView = detail::makeDebugSampledColorView(manager_.getShadowAtlasTexture())},
                        {.binding = 7, .textureView = detail::makeDebugSampledColorView(discontinuityTexture_)},
                        {.binding = 8, .textureView = detail::makeDebugSampledColorView(dummyOndsTexture_)},
                        {.binding = 9, .textureView = detail::makeDebugSampledColorView(dummyVisibilityTexture_)},
                        {.binding = 10, .textureView = detail::makeDebugSampledColorView(dummyRsmssTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 11, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmShadowCompareSpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void writeUniforms()
  {
    const detail::VirtualShadowMapShadowMaskUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .shadowBias = settings_.shadowBias,
      .slopeScaleBias = settings_.slopeScaleBias,
      .maxShadowBias = settings_.maxShadowBias,
      .shadowFilterTaps = detail::sanitizeShadowFilterTapCount(settings_.shadowFilterTaps),
      .rbssmBlockerSearchTaps = detail::sanitizeRbssmBlockerSearchTapCount(settings_.rbssmBlockerSearchTaps),
      .shadowFilterMode = settings_.shadowFilterMode,
      .rbsmDepthThreshold = settings_.rbsmDepthThreshold,
      .rbssmLightRadiusTexels = settings_.rbssmLightRadiusTexels,
      .rbssmMaxPenumbraTexels = settings_.rbssmMaxPenumbraTexels,
      .rbssmPenumbraScale = settings_.rbssmPenumbraScale,
      .pcfMinRadiusTexels = settings_.pcfMinRadiusTexels,
      .pcfMaxRadiusTexels = settings_.pcfMaxRadiusTexels,
      .normalBiasTexels = settings_.normalBiasTexels,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapShadowMaskUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture discontinuityTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::Texture dummyOndsTexture_;
  rendering::Texture dummyVisibilityTexture_;
  rendering::Texture dummyRsmssTexture_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapShadowRsmssPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float shadowBias = 0.0015f;
    float slopeScaleBias = 1.0f;
    float maxShadowBias = 0.006f;
    uint32_t shadowFilterTaps = 1u;
    uint32_t rbssmBlockerSearchTaps = 5u;
    uint32_t shadowFilterMode = 0u;
    float rbsmDepthThreshold = 0.001f;
    float rbssmLightRadiusTexels = 1.5f;
    float rbssmMaxPenumbraTexels = 12.0f;
    float rbssmPenumbraScale = 1.0f;
    float pcfMinRadiusTexels = 0.75f;
    float pcfMaxRadiusTexels = 8.0f;
    float normalBiasTexels = 0.5f;
  };

  VirtualShadowMapShadowRsmssPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture discontinuityTexture,
      rendering::Texture ondsTexture,
      rendering::Texture visibilityTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), discontinuityTexture_(discontinuityTexture), ondsTexture_(ondsTexture), visibilityTexture_(visibilityTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapShadowRsmssPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
    if (dummyRsmssTexture_.isValid())
    {
      renderGraph->deleteTexture(dummyRsmssTexture_);
    }
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!dummyRsmssTexture_.isValid())
    {
      dummyRsmssTexture_ = detail::makeDebugFallbackSampledTexture(renderGraph, passName + "_DummyRsmss.texture");
    }

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowAtlasTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "discontinuityTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "ondsTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "visibilityTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "rsmssTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapShadowMaskUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 4, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                        {.binding = 6, .textureView = detail::makeDebugSampledColorView(manager_.getShadowAtlasTexture())},
                        {.binding = 7, .textureView = detail::makeDebugSampledColorView(discontinuityTexture_)},
                        {.binding = 8, .textureView = detail::makeDebugSampledColorView(ondsTexture_)},
                        {.binding = 9, .textureView = detail::makeDebugSampledColorView(visibilityTexture_)},
                        {.binding = 10, .textureView = detail::makeDebugSampledColorView(dummyRsmssTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 11, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmShadowRsmssSpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void writeUniforms()
  {
    const detail::VirtualShadowMapShadowMaskUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .enabled = enabled_ ? 1u : 0u,
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .shadowBias = settings_.shadowBias,
      .slopeScaleBias = settings_.slopeScaleBias,
      .maxShadowBias = settings_.maxShadowBias,
      .shadowFilterTaps = detail::sanitizeShadowFilterTapCount(settings_.shadowFilterTaps),
      .rbssmBlockerSearchTaps = detail::sanitizeRbssmBlockerSearchTapCount(settings_.rbssmBlockerSearchTaps),
      .shadowFilterMode = settings_.shadowFilterMode,
      .rbsmDepthThreshold = settings_.rbsmDepthThreshold,
      .rbssmLightRadiusTexels = settings_.rbssmLightRadiusTexels,
      .rbssmMaxPenumbraTexels = settings_.rbssmMaxPenumbraTexels,
      .rbssmPenumbraScale = settings_.rbssmPenumbraScale,
      .pcfMinRadiusTexels = settings_.pcfMinRadiusTexels,
      .pcfMaxRadiusTexels = settings_.pcfMaxRadiusTexels,
      .normalBiasTexels = settings_.normalBiasTexels,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapShadowMaskUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture discontinuityTexture_;
  rendering::Texture ondsTexture_;
  rendering::Texture visibilityTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::Texture dummyRsmssTexture_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

class VirtualShadowMapScreenSpaceShadowPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float surfaceThickness = 0.005f;
    float bilinearThreshold = 0.02f;
    float shadowContrast = 4.0f;
    float rayDistance = 60.0f;
    std::array<float, 2> depthBounds = {0.0f, 1.0f};
    bool ignoreEdgePixels = false;
    bool usePrecisionOffset = false;
    bool bilinearSamplingOffsetMode = false;
    bool debugOutputEdgeMask = false;
    bool debugOutputThreadIndex = false;
    bool debugOutputWaveIndex = false;
    bool useEarlyOut = true;
    bool treatSkippedEdgeSamplesAsLit = true;
  };

  VirtualShadowMapScreenSpaceShadowPass(
      rendering::Texture depthTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : depthTexture_(depthTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapScreenSpaceShadowPass() override
  {
    destroy();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffers_[0].isValid())
    {
      writeRuntimeData();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffers_[0].isValid())
    {
      writeRuntimeData();
    }
  }

  void updateRuntimeState(const math::Mat4f &viewProjection, const math::Vec3f &lightDirection, bool reverseZ)
  {
    // The renderer stores directional lights as the direction light travels.
    // Bend's screen-space shadow projection expects the vector toward the light source.
    const math::Vec3f screenSpaceShadowDirection = lightDirection * -1.0f;
    const math::Vec4f lightProjectionInput(
        screenSpaceShadowDirection[0],
        screenSpaceShadowDirection[1],
        screenSpaceShadowDirection[2],
        0.0f);
    cachedLightProjection_ = viewProjection * lightProjectionInput;
    reverseZ_ = reverseZ;
    hasRuntimeState_ = true;
    if (uniformBuffers_[0].isValid())
    {
      writeRuntimeData();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createSharedResources();
    createBindings();
    createPipeline();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    for (uint32_t dispatchIndex = 0u; dispatchIndex < detail::kVsmScreenSpaceShadowMaxDispatches; ++dispatchIndex)
    {
      commandBuffer.cmdBindBindingGroups(bindingGroups_[dispatchIndex], nullptr, 0);
      commandBuffer.cmdDispatchIndirect(indirectArgsBuffer_, static_cast<uint64_t>(dispatchIndex) * sizeof(detail::VirtualShadowMapDispatchIndirectArgs));
    }
  }

private:
  void destroy()
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    for (auto &bindingGroup : bindingGroups_)
    {
      renderGraph->deleteBindingGroups(bindingGroup);
    }
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(indirectArgsBuffer_);
    for (auto &uniformBuffer : uniformBuffers_)
    {
      renderGraph->deleteBuffer(uniformBuffer);
    }
  }

  void createSharedResources()
  {
    for (uint32_t dispatchIndex = 0u; dispatchIndex < detail::kVsmScreenSpaceShadowMaxDispatches; ++dispatchIndex)
    {
      uniformBuffers_[dispatchIndex] = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms" + std::to_string(dispatchIndex) + ".buffer",
              .size = sizeof(detail::VirtualShadowMapScreenSpaceShadowUniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }

    indirectArgsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_DispatchIndirect.buffer",
            .size = detail::kVsmScreenSpaceShadowMaxDispatches * sizeof(detail::VirtualShadowMapDispatchIndirectArgs),
            .usage = rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });

    writeRuntimeData();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });
  }

  void createBindings()
  {
    for (uint32_t dispatchIndex = 0u; dispatchIndex < detail::kVsmScreenSpaceShadowMaxDispatches; ++dispatchIndex)
    {
      bindingGroups_[dispatchIndex] = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_BindingGroups" + std::to_string(dispatchIndex),
              .groups = {
                  rendering::GroupInfo{
                      .name = "group0",
                      .buffers = {
                          {.binding = 0, .bufferView = {.buffer = uniformBuffers_[dispatchIndex], .offset = 0, .size = sizeof(detail::VirtualShadowMapScreenSpaceShadowUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                      },
                      .textures = {
                          {.binding = 1, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                      },
                      .storageTextures = {
                          {.binding = 2, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                      },
                  },
              },
          });
    }
  }

  void createPipeline()
  {
    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmScreenSpaceShadowSpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeRuntimeData()
  {
    std::array<detail::VirtualShadowMapScreenSpaceShadowUniforms, detail::kVsmScreenSpaceShadowMaxDispatches> uniforms{};
    std::array<detail::VirtualShadowMapDispatchIndirectArgs, detail::kVsmScreenSpaceShadowMaxDispatches> indirectArgs{};

    if (enabled_ && hasRuntimeState_)
    {
      const auto dispatchList = detail::buildScreenSpaceShadowDispatchList(cachedLightProjection_, settings_.width, settings_.height);
      const float farDepthValue = reverseZ_ ? 0.0f : 1.0f;
      const float nearDepthValue = reverseZ_ ? 1.0f : 0.0f;

      for (uint32_t dispatchIndex = 0u;
           dispatchIndex < dispatchList.dispatchCount && dispatchIndex < detail::kVsmScreenSpaceShadowMaxDispatches;
           ++dispatchIndex)
      {
        const auto &dispatch = dispatchList.dispatches[dispatchIndex];
        auto &uniform = uniforms[dispatchIndex];
        std::memcpy(uniform.lightCoordinate, dispatchList.lightCoordinate, sizeof(uniform.lightCoordinate));
        uniform.waveOffset[0] = dispatch.waveOffset[0];
        uniform.waveOffset[1] = dispatch.waveOffset[1];
        uniform.depthBounds[0] = settings_.depthBounds[0];
        uniform.depthBounds[1] = settings_.depthBounds[1];
        uniform.surfaceThickness = settings_.surfaceThickness;
        uniform.bilinearThreshold = settings_.bilinearThreshold;
        uniform.shadowContrast = settings_.shadowContrast;
        uniform.rayDistance = settings_.rayDistance;
        uniform.farDepthValue = farDepthValue;
        uniform.nearDepthValue = nearDepthValue;
        uniform.ignoreEdgePixels = settings_.ignoreEdgePixels ? 1u : 0u;
        uniform.usePrecisionOffset = settings_.usePrecisionOffset ? 1u : 0u;
        uniform.bilinearSamplingOffsetMode = settings_.bilinearSamplingOffsetMode ? 1u : 0u;
        uniform.debugOutputEdgeMask = settings_.debugOutputEdgeMask ? 1u : 0u;
        uniform.debugOutputThreadIndex = settings_.debugOutputThreadIndex ? 1u : 0u;
        uniform.debugOutputWaveIndex = settings_.debugOutputWaveIndex ? 1u : 0u;
        uniform.useEarlyOut = settings_.useEarlyOut ? 1u : 0u;
        uniform.treatSkippedEdgeSamplesAsLit = settings_.treatSkippedEdgeSamplesAsLit ? 1u : 0u;

        indirectArgs[dispatchIndex] = detail::VirtualShadowMapDispatchIndirectArgs{
          .x = dispatch.waveCount[0],
          .y = dispatch.waveCount[1],
          .z = dispatch.waveCount[2],
        };
      }
    }

    for (uint32_t dispatchIndex = 0u; dispatchIndex < detail::kVsmScreenSpaceShadowMaxDispatches; ++dispatchIndex)
    {
      renderGraph->bufferWrite(uniformBuffers_[dispatchIndex], 0, sizeof(detail::VirtualShadowMapScreenSpaceShadowUniforms), &uniforms[dispatchIndex]);
    }
    renderGraph->bufferWrite(indirectArgsBuffer_, 0, sizeof(indirectArgs), indirectArgs.data());
  }

  rendering::Texture depthTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;
  bool reverseZ_ = true;
  bool hasRuntimeState_ = false;
  math::Vec4f cachedLightProjection_ = math::Vec4f(0.0f, 0.0f, 0.0f, 0.0f);

  std::array<rendering::Buffer, detail::kVsmScreenSpaceShadowMaxDispatches> uniformBuffers_{};
  rendering::Buffer indirectArgsBuffer_;
  rendering::BindingsLayout layout_;
  std::array<rendering::BindingGroups, detail::kVsmScreenSpaceShadowMaxDispatches> bindingGroups_{};
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
};

class VirtualShadowMapShadowPcfPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    float shadowBias = 0.0015f;
    float slopeScaleBias = 1.0f;
    float maxShadowBias = 0.006f;
    uint32_t shadowFilterTaps = 1u;
    float pcfRadiusTexels = 1.5f;
    float normalBiasTexels = 0.5f;
    std::array<float, 3> ambientShadowColor = {0.18f, 0.20f, 0.24f};
    uint32_t contactShadowSamples = 0u;
    float contactShadowDistance = 24.0f;
    float contactShadowThickness = 2.0f;
    float contactShadowIntensity = 0.0f;
    float contactShadowStartBias = 1.0f;
    bool screenSpaceShadowEnabled = true;
  };

  VirtualShadowMapShadowPcfPass(
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture screenSpaceShadowTexture,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), depthTexture_(depthTexture), screenSpaceShadowTexture_(screenSpaceShadowTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapShadowPcfPass() override
  {
    destroy();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createSharedResources();
    createBindings();
    createPipeline();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX,
        (settings_.height + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY,
        1u);
  }

private:
  void destroy()
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
  }

  void createSharedResources()
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(detail::VirtualShadowMapShadowPcfUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "directionalLights", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowAtlasTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "screenSpaceShadowTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });
  }

  void createBindings()
  {
    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(detail::VirtualShadowMapShadowPcfUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = manager_.getDirectionalLightsBuffer(), .offset = 0, .size = manager_.getDirectionalLightsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 5, .textureView = detail::makeDebugSampledDepthView(depthTexture_)},
                        {.binding = 7, .textureView = detail::makeDebugSampledColorView(manager_.getShadowAtlasTexture())},
                        {.binding = 8, .textureView = detail::makeDebugSampledColorView(screenSpaceShadowTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 9, .textureView = detail::makeDebugStorageColorView(outputTexture_)},
                    },
                },
            },
        });
  }

  void createPipeline()
  {
    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = detail::getCachedVsmShadowMaskPcfSpirv(),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeUniforms()
  {
    const detail::VirtualShadowMapShadowPcfUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .cascadeCount = manager_.getCascadeCount(),
      .activeDirectionalLights = manager_.getActiveDirectionalLightCount(),
      .enabled = enabled_ ? 1u : 0u,
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .shadowBias = settings_.shadowBias,
      .slopeScaleBias = settings_.slopeScaleBias,
      .maxShadowBias = settings_.maxShadowBias,
      .pcfRadiusTexels = settings_.pcfRadiusTexels,
      .normalBiasTexels = settings_.normalBiasTexels,
      .contactShadowDistance = settings_.contactShadowDistance,
      .contactShadowThickness = settings_.contactShadowThickness,
      .ambientShadowColor = {settings_.ambientShadowColor[0], settings_.ambientShadowColor[1], settings_.ambientShadowColor[2]},
      .contactShadowIntensity = settings_.contactShadowIntensity,
      .shadowFilterTaps = detail::sanitizeShadowPcfTapCount(settings_.shadowFilterTaps),
      .contactShadowSamples = settings_.contactShadowSamples,
      .screenSpaceShadowEnabled = settings_.screenSpaceShadowEnabled ? 1u : 0u,
      .contactShadowStartBias = settings_.contactShadowStartBias,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<detail::VirtualShadowMapShadowPcfUniforms *>(&uniforms));
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture screenSpaceShadowTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool enabled_ = false;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

} // namespace gpgpu
} // namespace virtualgeometry
