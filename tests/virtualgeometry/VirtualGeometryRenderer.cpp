#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "./utils/File.hpp"
#include "os/Logger.hpp"
#include "rendering/animation/AnimationFile.hpp"
#include "rendering/animation/AnimationPlayer.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/core/LightCamera.hpp"
#include "rendering/gpgpu/ColorToQuadPass.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/FrameStatisticsHeatmapPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "time/TimeSpan.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPrePassDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryRendererPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapPass.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
using namespace rendering;
using namespace backend;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

namespace fs = std::filesystem;

static ImU32 makeColorU32(float r, float g, float b, float a = 1.0f)
{
  const auto toByte = [](float value)
  {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<unsigned int>(clamped * 255.0f + 0.5f);
  };

  return IM_COL32(toByte(r), toByte(g), toByte(b), toByte(a));
}

static ImU32 makeHashedSeriesColor(size_t hash, float saturation, float value)
{
  const float hue = static_cast<float>(hash % 360u) / 360.0f;
  const float scaledHue = hue * 6.0f;
  const int sector = static_cast<int>(std::floor(scaledHue)) % 6;
  const float fraction = scaledHue - std::floor(scaledHue);
  const float p = value * (1.0f - saturation);
  const float q = value * (1.0f - fraction * saturation);
  const float t = value * (1.0f - (1.0f - fraction) * saturation);

  switch (sector)
  {
  case 0:
    return makeColorU32(value, t, p);
  case 1:
    return makeColorU32(q, value, p);
  case 2:
    return makeColorU32(p, value, t);
  case 3:
    return makeColorU32(p, q, value);
  case 4:
    return makeColorU32(t, p, value);
  default:
    return makeColorU32(value, p, q);
  }
}

// ============================================================================
// GPU-layout structures  (must stay in sync with the culling pass WGSL)
// ============================================================================

struct VisibleClusterInfo_CPU
{
  uint32_t pageIndex;
  uint32_t pageLocalClusterIndex;
  uint32_t instanceIndex;
  uint32_t _padding; // parentNodeIndex
};
static_assert(sizeof(VisibleClusterInfo_CPU) == 16);

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

static constexpr uint32_t SENTINEL_VALUE = 0xFFFFFFFFu;
static constexpr uint32_t HW_VISIBLE_CLUSTER_COUNT_INDEX = 2u;
static constexpr uint32_t SW_VISIBLE_CLUSTER_COUNT_INDEX = 4u;

static CullingCounters readCullingCounters(RenderGraph *renderGraph, const Buffer &buffer)
{
  CullingCounters counters{};
  renderGraph->bufferRead(
      buffer,
      0,
      sizeof(CullingCounters),
      [&counters](const void *gpuData)
      {
        std::memcpy(&counters, gpuData, sizeof(CullingCounters));
      });
  return counters;
}

static uint32_t readUint32Value(RenderGraph *renderGraph, const Buffer &buffer)
{
  uint32_t value = 0u;
  renderGraph->bufferRead(
      buffer,
      0,
      sizeof(value),
      [&value](const void *gpuData)
      {
        std::memcpy(&value, gpuData, sizeof(value));
      });
  return value;
}

template <typename T>
static std::vector<T> readBufferElements(RenderGraph *renderGraph, const Buffer &buffer, uint32_t elementCount)
{
  if (elementCount == 0u)
  {
    return {};
  }

  std::vector<T> elements(elementCount);
  renderGraph->bufferRead(
      buffer,
      0,
      static_cast<uint64_t>(elementCount) * sizeof(T),
      [&elements](const void *gpuData)
      {
        std::memcpy(elements.data(), gpuData, elements.size() * sizeof(T));
      });
  return elements;
}

template <typename T>
static std::vector<T> readBufferElements(rendering::RHI *rhi, rendering::BufferId bufferId, uint32_t elementCount)
{
  if (rhi == nullptr || bufferId == rendering::BufferId::Invalid || elementCount == 0u)
  {
    return {};
  }

  std::vector<T> elements(elementCount);
  rhi->bufferRead(
      bufferId,
      0u,
      static_cast<uint64_t>(elementCount) * sizeof(T),
      [&elements](const void *gpuData)
      {
        std::memcpy(elements.data(), gpuData, elements.size() * sizeof(T));
      });
  return elements;
}

static uint32_t popcount32(uint32_t value)
{
  uint32_t count = 0u;
  while (value != 0u)
  {
    count += value & 1u;
    value >>= 1u;
  }
  return count;
}

class SelectedDebugTargetPass : public rendering::Pass
{
public:
  enum TargetSlot : uint32_t
  {
    TargetSlot_BaseColor = 0u,
    TargetSlot_ClusterId = 1u,
    TargetSlot_PrepassDepth = 2u,
    TargetSlot_SceneDepth = 3u,
    TargetSlot_HardwareFrameStats = 4u,
    TargetSlot_VirtualShadowMapPages = 5u,
    TargetSlot_VirtualShadowMapTable = 6u,
    TargetSlot_VirtualShadowMapShadowLighting = 7u,
    TargetSlot_VirtualShadowMapScreenSpaceShadow = 8u,
  };

  struct Inputs
  {
    Texture baseColor;
    Texture clusterId;
    Texture prepassDepth;
    Texture sceneDepth;
    Texture hardwareFrameStats;
    Texture virtualShadowMapPages;
    Texture virtualShadowMapTable;
    Texture virtualShadowMapShadowLighting;
    Texture virtualShadowMapScreenSpaceShadow;
  };

  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
  };

  explicit SelectedDebugTargetPass(Texture outputTexture, Format outputFormat, Inputs inputs, Settings settings)
      : outputTexture_(outputTexture), outputFormat_(outputFormat), inputs_(inputs), settings_(settings)
  {
  }

  ~SelectedDebugTargetPass() override
  {
    renderGraph->deleteGraphicsPipeline(graphicsPipeline_);
    renderGraph->deleteShader(vertexShader_);
    renderGraph->deleteShader(fragmentShader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteSampler(sampler_);
  }

  void setSelectedSlot(uint32_t selectedSlot)
  {
    selectedSlot_ = selectedSlot;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = renderGraph->createBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(Uniforms),
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    sampler_ = renderGraph->createSampler(
        SamplerInfo{
          .name = passName + "_Sampler.sampler",
          .minFilter = Filter::Nearest,
          .magFilter = Filter::Nearest,
          .addressModeU = SamplerAddressMode::ClampToEdge,
          .addressModeV = SamplerAddressMode::ClampToEdge,
          .addressModeW = SamplerAddressMode::ClampToEdge,
          .anisotropyEnable = false,
          .maxAnisotropy = 1.0f,
          .maxLod = 1.0f,
        });

    layout_ = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_layout",
          .groups = {
              BindingGroupLayout{
                  .buffers = {
                      {.name = "uniforms", .binding = 0, .isDynamic = false, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex | BindingVisibility::BindingVisibility_Fragment},
                  },
                  .samplers = {
                      {.name = "texSampler", .binding = 1, .visibility = BindingVisibility::BindingVisibility_Fragment},
                  },
                  .textures = {
                      {.name = "baseColorTexture", .binding = 2, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "clusterIdTexture", .binding = 3, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "prepassDepthTexture", .binding = 4, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "sceneDepthTexture", .binding = 5, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "hardwareStatsTexture", .binding = 6, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "vsmPagesDebugTexture", .binding = 7, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "vsmTableDebugTexture", .binding = 8, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "vsmShadowLightingTexture", .binding = 9, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      {.name = "vsmScreenSpaceShadowTexture", .binding = 10, .visibility = BindingVisibility::BindingVisibility_Fragment},
                  },
              },
          },
        });

    const TextureView baseColorView = makeColorTextureView(inputs_.baseColor);
    const TextureView clusterIdView = makeColorTextureView(inputs_.clusterId);
    const TextureView prepassDepthView = makeColorTextureView(inputs_.prepassDepth);
    const TextureView sceneDepthView = makeDepthTextureView(inputs_.sceneDepth);
    const TextureView hardwareStatsView = makeColorTextureView(inputs_.hardwareFrameStats);
    const TextureView vsmPagesDebugView = makeColorTextureView(inputs_.virtualShadowMapPages);
    const TextureView vsmTableDebugView = makeColorTextureView(inputs_.virtualShadowMapTable);
    const TextureView vsmShadowLightingView = makeColorTextureView(inputs_.virtualShadowMapShadowLighting);
    const TextureView vsmScreenSpaceShadowView = makeColorTextureView(inputs_.virtualShadowMapScreenSpaceShadow);

    bindingGroups_ = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_bindingGroups",
          .groups = {
              GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                  },
                  .samplers = {
                      {.binding = 1, .sampler = sampler_, .view = baseColorView},
                  },
                  .textures = {
                      {.binding = 2, .textureView = baseColorView},
                      {.binding = 3, .textureView = clusterIdView},
                      {.binding = 4, .textureView = prepassDepthView},
                      {.binding = 5, .textureView = sceneDepthView},
                      {.binding = 6, .textureView = hardwareStatsView},
                      {.binding = 7, .textureView = vsmPagesDebugView},
                      {.binding = 8, .textureView = vsmTableDebugView},
                      {.binding = 9, .textureView = vsmShadowLightingView},
                      {.binding = 10, .textureView = vsmScreenSpaceShadowView},
                  },
              },
          },
        });

    vertexShader_ = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_vs.shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadPass-vs.spirv"),
          .type = ShaderType::SpirV,
        });
    fragmentShader_ = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_fs.shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadSelectedDebug-fs.spirv"),
          .type = ShaderType::SpirV,
        });

    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = vertexShader_;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = CullMode::None;
    pipelineInfo.vertexStage.winding = WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = fragmentShader_;
    pipelineInfo.fragmentStage.shaderEntry = "fs_main";
    pipelineInfo.fragmentStage.colorAttatchments = {
      {
        .format = outputFormat_,
        .loadOp = LoadOp::LoadOp_Load,
        .storeOp = StoreOp::StoreOp_Store,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      },
    };
    pipelineInfo.fragmentStage.depthAttatchment.enabled = false;
    graphicsPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);

    ColorAttachmentInfo colorInfo{};
    colorInfo.name = passName + "_ColorAttachment";
    colorInfo.view = TextureView{
      .texture = outputTexture_,
      .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ResourceLayout::COLOR_ATTACHMENT,
      .index = 0,
      .flags = ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
    colorInfo.clearValue = Color::rgba(0, 0, 0, 0);

    RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = Rect2D(0, 0, settings_.viewPortWidth, settings_.viewPortHeight);
    renderPass.viewport = Viewport(settings_.viewPortWidth, settings_.viewPortHeight);
    renderPass.colorAttachments = {colorInfo};
    renderPass.depthStencilAttachment.enabled = false;

    commandBuffer.cmdBindGraphicsPipeline(graphicsPipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDraw(6, 1, 0, 0);
    commandBuffer.cmdEndRenderPass();
  }

private:
  struct Uniforms
  {
    float ndcMinX = -1.0f;
    float ndcMinY = -1.0f;
    float ndcMaxX = 1.0f;
    float ndcMaxY = 1.0f;
    float uvMinX = 0.0f;
    float uvMinY = 0.0f;
    float uvMaxX = 1.0f;
    float uvMaxY = 1.0f;
    uint32_t selectedSlot = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
    uint32_t _padding2 = 0u;
  };

  static TextureView makeColorTextureView(Texture texture)
  {
    return TextureView{
      .texture = texture,
      .access = AccessPattern::SHADER_READ,
      .layout = ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  static TextureView makeDepthTextureView(Texture texture)
  {
    return TextureView{
      .texture = texture,
      .access = AccessPattern::SHADER_READ,
      .layout = ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = ImageAspectFlags::Depth,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.selectedSlot = selectedSlot_;
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(Uniforms), &uniforms);
  }

  Texture outputTexture_;
  Format outputFormat_;
  Inputs inputs_{};
  Settings settings_{};
  uint32_t selectedSlot_ = TargetSlot_BaseColor;

  BindingGroups bindingGroups_;
  BindingsLayout layout_;
  Shader vertexShader_;
  Shader fragmentShader_;
  GraphicsPipeline graphicsPipeline_;
  Buffer uniformBuffer_;
  Sampler sampler_;
};

struct CpuFrameReport
{
  struct Operation
  {
    double wallMs = 0.0;
    RenderGraph::CpuStats cpu{};
  };

  double totalMs = 0.0;
  double windowUpdateMs = 0.0;
  double simulationMs = 0.0;
  double cameraBuildMs = 0.0;
  double animationMs = 0.0;
  double uniformUploadMs = 0.0;
  double preCullingUniformMs = 0.0;
  double depthPreUniformMs = 0.0;
  double finalCullingUniformMs = 0.0;
  double hardwareDrawUniformMs = 0.0;
  double materialCameraMs = 0.0;
  double materialBeginFrameMs = 0.0;
  double vsmLightSetupMs = 0.0;
  double vsmUpdateMs = 0.0;
  double vsmInvalidateMs = 0.0;
  double acquireMs = 0.0;
  double renderRunMs = 0.0;
  double waitMs = 0.0;
  double waitBlockMs = 0.0;
  double timerReadbackMs = 0.0;
  double statsDumpMs = 0.0;
  double statsAllocatorReadMs = 0.0;
  double statsDrawCountersReadMs = 0.0;
  double statsPageTableReadMs = 0.0;
  double statsPageTableProcessMs = 0.0;
  double statsVisibleClustersReadMs = 0.0;
  double statsVisibleClustersProcessMs = 0.0;
  double statsDrawIndirectReadMs = 0.0;
  double statsDrawIndirectProcessMs = 0.0;
  double uiBuildMs = 0.0;
  double presentMs = 0.0;
  double streamingMs = 0.0;
  double vsmResetInvalidationsMs = 0.0;
  double instanceUploadMs = 0.0;
  double untrackedMs = 0.0;
  Operation uniformUpload{};
  Operation vtFeedback{};
  Operation sceneStreaming{};
  Operation instanceUpload{};
  Operation statsReadback{};
};

struct VSMVisibleClusterInfoCPU
{
  uint32_t pageIndex = 0u;
  uint32_t pageLocalClusterIndex = 0u;
  uint32_t instanceIndex = 0u;
  uint32_t layeredWrappedPageCoords = 0u;
  uint32_t layeredVirtualPageCoords = 0u;
  uint32_t meshPartIndex = 0u;
};
static_assert(sizeof(VSMVisibleClusterInfoCPU) == sizeof(uint32_t) * 6u);

struct VSMFrameStats
{
  uint32_t renderPages = 0u;
  uint32_t normalRenderPages = 0u;
  uint32_t normalRenderPageBudget = 0u;
  uint32_t fallbackRenderPages = 0u;
  uint32_t uniqueDrawnPages = 0u;
  double averageClustersPerDrawnPage = 0.0;
  uint32_t maxClustersPerDrawnPage = 0u;
  uint32_t totalDrawCommands = 0u;
  uint32_t executedDrawCommands = 0u;
  uint32_t drawCommandOverflow = 0u;
  uint64_t totalDrawTriangles = 0u;
  uint32_t hierarchyQueueSize = 0u;
  uint32_t clusterQueueSize = 0u;
  uint32_t readQueueSize = 0u;
  uint32_t allocatedPages = 0u;
  uint32_t validPages = 0u;
  uint32_t dirtyPages = 0u;
  uint32_t visiblePages = 0u;
  uint32_t fallbackPages = 0u;
  uint32_t freePhysicalPages = 0u;
  uint32_t usedPhysicalPages = 0u;
  uint32_t totalPhysicalPages = 0u;
};

using PassTimingSummary = RenderGraph::PassTimingSummary;
using TimerReadbackRecord = RenderGraph::TimerReadbackRecord;

struct CpuSectionTimingSummary
{
  std::string name;
  uint32_t orderIndex = 0u;
  double ms = 0.0;
};

struct PendingFrameDiagnostics
{
  RenderGraph::Frame frame{};
  uint32_t displayFrameIndex = 0u;
  float frameDeltaMs = 0.0f;
  bool timerResolveSubmitted = false;
  CpuFrameReport cpuFrame{};
  RenderGraph::CpuStats renderGraphCpuStart{};
  double renderGraphRunTotalMs = 0.0;
  std::vector<RenderGraph::RunPhaseRecord> renderGraphRunPhases{};
  std::vector<RenderGraph::RuntimeMetricRecord> renderGraphRuntimeMetrics{};
  RenderGraph::RunDebugStats renderGraphRunDebugStats{};
};

struct OverlayHistory
{
  static constexpr size_t kSampleCount = 180u;

  std::array<float, kSampleCount> frameMs{};
  std::array<float, kSampleCount> fps{};
  size_t nextIndex = 0u;
  size_t count = 0u;

  void push(float frameTimeMs)
  {
    frameMs[nextIndex] = frameTimeMs;
    fps[nextIndex] = frameTimeMs > 0.0f ? (1000.0f / frameTimeMs) : 0.0f;
    nextIndex = (nextIndex + 1u) % kSampleCount;
    count = std::min(count + 1u, kSampleCount);
  }

  void buildOrderedFrameTimes(std::array<float, kSampleCount> &ordered) const
  {
    ordered.fill(0.0f);
    for (size_t i = 0u; i < count; ++i)
    {
      const size_t srcIndex = (nextIndex + kSampleCount - count + i) % kSampleCount;
      ordered[i] = frameMs[srcIndex];
    }
  }

  void buildOrderedFps(std::array<float, kSampleCount> &ordered) const
  {
    ordered.fill(0.0f);
    for (size_t i = 0u; i < count; ++i)
    {
      const size_t srcIndex = (nextIndex + kSampleCount - count + i) % kSampleCount;
      ordered[i] = fps[srcIndex];
    }
  }
};

struct WindowTitleStats
{
  std::string baseTitle;
  double accumulatedFrameMs = 0.0;
  uint32_t accumulatedFrames = 0u;
  double latestFrameMs = 0.0;
  bool hasDisplayedStats = false;
  static constexpr double kRefreshIntervalMs = 250.0;

  explicit WindowTitleStats(std::string title) : baseTitle(std::move(title))
  {
  }

  void push(SDL_Window *window, double frameTimeMs)
  {
    if (window == nullptr || frameTimeMs <= 0.0)
    {
      return;
    }

    accumulatedFrameMs += frameTimeMs;
    accumulatedFrames += 1u;
    latestFrameMs = frameTimeMs;

    if (hasDisplayedStats && accumulatedFrameMs < kRefreshIntervalMs)
    {
      return;
    }

    const double averageFrameMs = accumulatedFrameMs / static_cast<double>(std::max(1u, accumulatedFrames));
    const double averageFps = averageFrameMs > 0.0 ? (1000.0 / averageFrameMs) : 0.0;

    std::ostringstream title;
    title << baseTitle << " | " << std::fixed << std::setprecision(1) << averageFps << " FPS | " << std::setprecision(2) << latestFrameMs << " ms";
    SDL_SetWindowTitle(window, title.str().c_str());

    accumulatedFrameMs = 0.0;
    accumulatedFrames = 0u;
    hasDisplayedStats = true;
  }

  void reset(SDL_Window *window) const
  {
    if (window != nullptr)
    {
      SDL_SetWindowTitle(window, baseTitle.c_str());
    }
  }
};

struct PassTimelineHistory
{
  static constexpr size_t kSampleCount = 180u;
  enum class Filter
  {
    Compute,
    Graphics,
  };

  struct Series
  {
    std::string name;
    uint32_t passIndex = 0u;
    ImU32 color = IM_COL32_WHITE;
    std::array<float, kSampleCount> values{};
    float latestMs = 0.0f;
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
  };

  std::vector<Series> series;
  std::unordered_map<std::string, size_t> seriesIndices;
  size_t nextSampleIndex = 0u;
  size_t count = 0u;
  uint32_t lastFrameIndex = 0u;
  bool hasFrames = false;
  float computeVisibleMaxMs = 0.0f;
  float graphicsVisibleMaxMs = 0.0f;
  std::vector<size_t> computeSeriesOrder;
  std::vector<size_t> graphicsSeriesOrder;
  std::vector<size_t> computeLegendOrder;
  std::vector<size_t> graphicsLegendOrder;

  static ImU32 makeSeriesColor(const std::string &name, uint32_t passIndex)
  {
    const size_t hash = std::hash<std::string>{}(name) ^ (static_cast<size_t>(passIndex) * 0x9e3779b97f4a7c15ull);
    return makeHashedSeriesColor(hash, 0.65f, 0.95f);
  }

  Series &ensureSeries(const PassTimingSummary &pass)
  {
    const auto [it, inserted] = seriesIndices.emplace(pass.name, series.size());
    if (inserted)
    {
      series.push_back(
          Series{
            .name = pass.name,
            .passIndex = pass.passIndex,
            .color = makeSeriesColor(pass.name, pass.passIndex),
          });
    }

    Series &entry = series[it->second];
    entry.passIndex = pass.passIndex;
    entry.hasComputeWork = pass.hasComputeWork;
    entry.hasGraphicsWork = pass.hasGraphicsWork;
    return entry;
  }

  void pushFrame(uint32_t frameIndex, const std::vector<PassTimingSummary> &passTimings)
  {
    if (hasFrames && lastFrameIndex == frameIndex)
    {
      return;
    }

    hasFrames = true;
    lastFrameIndex = frameIndex;
    const size_t sampleIndex = nextSampleIndex;

    for (Series &entry : series)
    {
      entry.values[sampleIndex] = 0.0f;
      entry.latestMs = 0.0f;
    }

    for (const PassTimingSummary &pass : passTimings)
    {
      Series &entry = ensureSeries(pass);
      entry.values[sampleIndex] = static_cast<float>(pass.gpuTimeMs);
      entry.latestMs = static_cast<float>(pass.gpuTimeMs);
    }

    nextSampleIndex = (nextSampleIndex + 1u) % kSampleCount;
    count = std::min(count + 1u, kSampleCount);
    rebuildCachedViews();
  }

  void rebuildCachedViews()
  {
    computeVisibleMaxMs = 0.0f;
    graphicsVisibleMaxMs = 0.0f;
    computeSeriesOrder.clear();
    graphicsSeriesOrder.clear();
    computeLegendOrder.clear();
    graphicsLegendOrder.clear();

    computeSeriesOrder.reserve(series.size());
    graphicsSeriesOrder.reserve(series.size());
    computeLegendOrder.reserve(series.size());
    graphicsLegendOrder.reserve(series.size());
    for (size_t seriesIndex = 0u; seriesIndex < series.size(); ++seriesIndex)
    {
      const Series &entry = series[seriesIndex];
      if (entry.hasComputeWork)
      {
        computeSeriesOrder.push_back(seriesIndex);
        computeLegendOrder.push_back(seriesIndex);
        for (size_t sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
        {
          computeVisibleMaxMs = std::max(computeVisibleMaxMs, entry.values[sampleIndex]);
        }
      }

      if (entry.hasGraphicsWork)
      {
        graphicsSeriesOrder.push_back(seriesIndex);
        graphicsLegendOrder.push_back(seriesIndex);
        for (size_t sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
        {
          graphicsVisibleMaxMs = std::max(graphicsVisibleMaxMs, entry.values[sampleIndex]);
        }
      }
    }

    const auto byPassIndex = [&](size_t a, size_t b)
    {
      const Series &lhs = series[a];
      const Series &rhs = series[b];
      if (lhs.passIndex != rhs.passIndex)
      {
        return lhs.passIndex < rhs.passIndex;
      }

      return lhs.name < rhs.name;
    };
    const auto byLatest = [&](size_t a, size_t b)
    {
      const Series &lhs = series[a];
      const Series &rhs = series[b];
      if (lhs.latestMs != rhs.latestMs)
      {
        return lhs.latestMs > rhs.latestMs;
      }
      if (lhs.passIndex != rhs.passIndex)
      {
        return lhs.passIndex < rhs.passIndex;
      }
      return lhs.name < rhs.name;
    };

    std::sort(computeSeriesOrder.begin(), computeSeriesOrder.end(), byPassIndex);
    std::sort(graphicsSeriesOrder.begin(), graphicsSeriesOrder.end(), byPassIndex);
    std::sort(computeLegendOrder.begin(), computeLegendOrder.end(), byLatest);
    std::sort(graphicsLegendOrder.begin(), graphicsLegendOrder.end(), byLatest);
  }

  void buildOrderedValues(const Series &entry, std::array<float, kSampleCount> &ordered) const
  {
    ordered.fill(0.0f);
    if (count == 0u)
    {
      return;
    }

    const size_t startIndex = (nextSampleIndex + kSampleCount - count) % kSampleCount;
    for (size_t i = 0u; i < count; ++i)
    {
      ordered[i] = entry.values[(startIndex + i) % kSampleCount];
    }
  }

  static bool matchesFilter(const Series &entry, Filter filter)
  {
    switch (filter)
    {
    case Filter::Compute:
      return entry.hasComputeWork;
    case Filter::Graphics:
      return entry.hasGraphicsWork;
    }
    return false;
  }

  float getVisibleMaxMs(Filter filter) const
  {
    return filter == Filter::Compute ? computeVisibleMaxMs : graphicsVisibleMaxMs;
  }

  std::vector<const Series *> getSortedSeries(Filter filter) const
  {
    const std::vector<size_t> &sortedIndices =
        filter == Filter::Compute ? computeSeriesOrder : graphicsSeriesOrder;
    std::vector<const Series *> ordered;
    ordered.reserve(sortedIndices.size());
    for (size_t seriesIndex : sortedIndices)
    {
      ordered.push_back(&series[seriesIndex]);
    }
    return ordered;
  }

  std::vector<const Series *> getLegendSeriesSortedByLatest(Filter filter) const
  {
    const std::vector<size_t> &sortedIndices =
        filter == Filter::Compute ? computeLegendOrder : graphicsLegendOrder;
    std::vector<const Series *> ordered;
    ordered.reserve(sortedIndices.size());
    for (size_t seriesIndex : sortedIndices)
    {
      ordered.push_back(&series[seriesIndex]);
    }
    return ordered;
  }
};

struct CpuTimelineHistory
{
  static constexpr size_t kSampleCount = 180u;

  struct Series
  {
    std::string name;
    uint32_t orderIndex = 0u;
    ImU32 color = IM_COL32_WHITE;
    std::array<float, kSampleCount> values{};
    float latestMs = 0.0f;
  };

  std::vector<Series> series;
  std::unordered_map<std::string, size_t> seriesIndices;
  size_t nextSampleIndex = 0u;
  size_t count = 0u;
  uint32_t lastFrameIndex = 0u;
  bool hasFrames = false;

  static ImU32 makeSeriesColor(const std::string &name, uint32_t orderIndex)
  {
    const size_t hash = std::hash<std::string>{}(name) ^ (static_cast<size_t>(orderIndex) * 0x517cc1b727220a95ull);
    return makeHashedSeriesColor(hash, 0.55f, 0.95f);
  }

  Series &ensureSeries(const CpuSectionTimingSummary &section)
  {
    const auto [it, inserted] = seriesIndices.emplace(section.name, series.size());
    if (inserted)
    {
      series.push_back(
          Series{
            .name = section.name,
            .orderIndex = section.orderIndex,
            .color = makeSeriesColor(section.name, section.orderIndex),
          });
    }

    Series &entry = series[it->second];
    entry.orderIndex = section.orderIndex;
    return entry;
  }

  void pushFrame(uint32_t frameIndex, const std::vector<CpuSectionTimingSummary> &sections)
  {
    if (hasFrames && lastFrameIndex == frameIndex)
    {
      return;
    }

    hasFrames = true;
    lastFrameIndex = frameIndex;
    const size_t sampleIndex = nextSampleIndex;

    for (Series &entry : series)
    {
      entry.values[sampleIndex] = 0.0f;
      entry.latestMs = 0.0f;
    }

    for (const CpuSectionTimingSummary &section : sections)
    {
      Series &entry = ensureSeries(section);
      entry.values[sampleIndex] = static_cast<float>(section.ms);
      entry.latestMs = static_cast<float>(section.ms);
    }

    nextSampleIndex = (nextSampleIndex + 1u) % kSampleCount;
    count = std::min(count + 1u, kSampleCount);
  }

  void buildOrderedValues(const Series &entry, std::array<float, kSampleCount> &ordered) const
  {
    ordered.fill(0.0f);
    if (count == 0u)
    {
      return;
    }

    const size_t startIndex = (nextSampleIndex + kSampleCount - count) % kSampleCount;
    for (size_t i = 0u; i < count; ++i)
    {
      ordered[i] = entry.values[(startIndex + i) % kSampleCount];
    }
  }

  float getVisibleMaxMs() const
  {
    float maxMs = 0.0f;
    for (const Series &entry : series)
    {
      for (size_t i = 0u; i < count; ++i)
      {
        maxMs = std::max(maxMs, entry.values[i]);
      }
    }
    return maxMs;
  }

  std::vector<const Series *> getSortedSeries() const
  {
    std::vector<const Series *> ordered;
    ordered.reserve(series.size());
    for (const Series &entry : series)
    {
      ordered.push_back(&entry);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const Series *a, const Series *b)
        {
          if (a->orderIndex != b->orderIndex)
          {
            return a->orderIndex < b->orderIndex;
          }

          return a->name < b->name;
        });
    return ordered;
  }

  std::vector<const Series *> getLegendSeriesSortedByLatest() const
  {
    std::vector<const Series *> ordered;
    ordered.reserve(series.size());
    for (const Series &entry : series)
    {
      ordered.push_back(&entry);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const Series *a, const Series *b)
        {
          if (a->latestMs != b->latestMs)
          {
            return a->latestMs > b->latestMs;
          }
          if (a->orderIndex != b->orderIndex)
          {
            return a->orderIndex < b->orderIndex;
          }
          return a->name < b->name;
        });
    return ordered;
  }
};

static void checkImGuiVkResult(VkResult result)
{
  if (result == VK_SUCCESS)
  {
    return;
  }

  throw std::runtime_error("Dear ImGui Vulkan backend failed with VkResult=" + std::to_string(static_cast<int>(result)));
}

static VkRenderPass createImGuiOverlayRenderPass(VkDevice device, VkFormat swapChainFormat)
{
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = swapChainFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorAttachmentRef{};
  colorAttachmentRef.attachment = 0u;
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1u;
  subpass.pColorAttachments = &colorAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0u;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 1u;
  renderPassInfo.pAttachments = &colorAttachment;
  renderPassInfo.subpassCount = 1u;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1u;
  renderPassInfo.pDependencies = &dependency;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  checkImGuiVkResult(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass));
  return renderPass;
}

template <typename Fn> static double measureWallTimeMs(Fn &&fn)
{
  const auto start = lib::time::TimeSpan::now();
  fn();
  return (lib::time::TimeSpan::now() - start).milliseconds();
}

static std::vector<CpuSectionTimingSummary> buildCpuSectionSummaries(const CpuFrameReport &cpuFrame)
{
  std::vector<CpuSectionTimingSummary> sections;
  sections.reserve(30u);

  auto appendSection = [&](const char *name, double ms)
  {
    sections.push_back(
        CpuSectionTimingSummary{
          .name = name,
          .orderIndex = static_cast<uint32_t>(sections.size()),
          .ms = ms,
        });
  };

  appendSection("Window/Input", cpuFrame.windowUpdateMs);
  appendSection("Simulation", cpuFrame.simulationMs);
  appendSection("Camera Build", cpuFrame.cameraBuildMs);
  appendSection("Animation", cpuFrame.animationMs);
  appendSection("PreCull Uniforms", cpuFrame.preCullingUniformMs);
  appendSection("DepthPre Uniforms", cpuFrame.depthPreUniformMs);
  appendSection("FinalCull Uniforms", cpuFrame.finalCullingUniformMs);
  appendSection("HWDraw Uniforms", cpuFrame.hardwareDrawUniformMs);
  appendSection("Material Camera", cpuFrame.materialCameraMs);
  appendSection("Material Begin", cpuFrame.materialBeginFrameMs);
  appendSection("VSM Light Setup", cpuFrame.vsmLightSetupMs);
  appendSection("VSM Update", cpuFrame.vsmUpdateMs);
  appendSection("VSM Invalidate", cpuFrame.vsmInvalidateMs);
  appendSection("Swap Acquire", cpuFrame.acquireMs);
  appendSection("RenderGraph Run", cpuFrame.renderRunMs);
  appendSection("RG Wait Block", cpuFrame.waitBlockMs);
  appendSection("Timer Readback", cpuFrame.timerReadbackMs);
  appendSection("VT Feedback", cpuFrame.vtFeedback.wallMs);
  appendSection("VT Streaming", cpuFrame.streamingMs);
  appendSection("VSM Reset", cpuFrame.vsmResetInvalidationsMs);
  appendSection("Instance Upload", cpuFrame.instanceUploadMs);
  appendSection("Stats Alloc", cpuFrame.statsAllocatorReadMs);
  appendSection("Stats Draw Ctrs", cpuFrame.statsDrawCountersReadMs);
  appendSection("Stats VPT Read", cpuFrame.statsPageTableReadMs);
  appendSection("Stats VPT Scan", cpuFrame.statsPageTableProcessMs);
  appendSection("Stats Page Clusters Read", cpuFrame.statsVisibleClustersReadMs);
  appendSection("Stats Page Clusters Scan", cpuFrame.statsVisibleClustersProcessMs);
  appendSection("Stats Indirect Read", cpuFrame.statsDrawIndirectReadMs);
  appendSection("Stats Indirect Scan", cpuFrame.statsDrawIndirectProcessMs);
  appendSection("UI Build", cpuFrame.uiBuildMs);
  appendSection("Present", cpuFrame.presentMs);
  appendSection("Untracked", cpuFrame.untrackedMs);

  return sections;
}

static std::vector<CpuSectionTimingSummary> buildSortedCpuSectionSummaries(const CpuFrameReport &cpuFrame)
{
  std::vector<CpuSectionTimingSummary> sections = buildCpuSectionSummaries(cpuFrame);
  sections.erase(
      std::remove_if(
          sections.begin(),
          sections.end(),
          [](const CpuSectionTimingSummary &section)
          {
            return section.ms <= 0.0;
          }),
      sections.end());
  std::sort(
      sections.begin(),
      sections.end(),
      [](const CpuSectionTimingSummary &a, const CpuSectionTimingSummary &b)
      {
        if (a.ms != b.ms)
        {
          return a.ms > b.ms;
        }
        return a.orderIndex < b.orderIndex;
      });
  return sections;
}

static std::vector<PassTimingSummary> buildPassTimingSummaries(const std::vector<TimerReadbackRecord> &timerRecords)
{
  std::vector<PassTimingSummary> passTimings;
  passTimings.reserve(timerRecords.size());

  std::unordered_map<std::string, size_t> passIndices;
  passIndices.reserve(timerRecords.size());
  for (const TimerReadbackRecord &record : timerRecords)
  {
    const auto [it, inserted] = passIndices.emplace(record.passName, passTimings.size());
    if (inserted)
    {
      passTimings.push_back(
          PassTimingSummary{
            .name = record.passName,
            .passIndex = record.passIndex,
          });
    }

    PassTimingSummary &summary = passTimings[it->second];
    summary.gpuTimeMs += record.gpuTimeMs;
    summary.timerCount += 1u;
    summary.hasComputeWork = summary.hasComputeWork || record.hasComputeWork;
    summary.hasGraphicsWork = summary.hasGraphicsWork || record.hasGraphicsWork;
  }

  std::sort(
      passTimings.begin(),
      passTimings.end(),
      [](const PassTimingSummary &a, const PassTimingSummary &b)
      {
        if (a.passIndex != b.passIndex)
        {
          return a.passIndex < b.passIndex;
        }
        return a.name < b.name;
      });

  return passTimings;
}

static double computeTrackedCpuFrameMs(const CpuFrameReport &cpuFrame);

struct OverlayPreparationInput
{
  uint32_t frameIndex = 0u;
  CpuFrameReport cpuFrame{};
  RenderGraph::CpuStats renderGraphCpu{};
  double renderGraphRunTotalMs = 0.0;
  std::vector<RenderGraph::RunPhaseRecord> renderGraphRunPhases{};
  std::vector<RenderGraph::RuntimeMetricRecord> renderGraphRuntimeMetrics{};
  RenderGraph::RunDebugStats renderGraphRunDebugStats{};
  RenderGraph::FrameWaitSummary frameWaitSummary{};
  uint32_t timerFrameSlot = 0u;
  bool collectTimers = false;
  rendering::BufferId vsmAllocatorCountersBufferId = rendering::BufferId::Invalid;
  rendering::BufferId vsmDrawCountersBufferId = rendering::BufferId::Invalid;
  rendering::BufferId vsmVirtualPageTableBufferId = rendering::BufferId::Invalid;
  rendering::BufferId vsmPageClusterCountsBufferId = rendering::BufferId::Invalid;
  rendering::BufferId vsmDrawIndirectBufferId = rendering::BufferId::Invalid;
  uint32_t allocationRequestCapacity = 0u;
  uint32_t futureAllocationRequestCapacity = 0u;
  uint32_t visibleClusterDrawCapacity = 0u;
  uint32_t physicalPageTableResolution = 0u;
  uint32_t activeVSMPageCount = 0u;
  bool collectStats = false;
};

struct PreparedOverlayState
{
  uint32_t frameIndex = 0u;
  float deltaTimeMs = 0.0f;
  CpuFrameReport cpuFrame{};
  RenderGraph::CpuStats renderGraphCpu{};
  VSMFrameStats vsmStats{};
  std::vector<PassTimingSummary> passTimings{};
  std::vector<CpuSectionTimingSummary> currentCpuSections{};
  double renderGraphRunTotalMs = 0.0;
  std::vector<RenderGraph::RunPhaseRecord> renderGraphRunPhases{};
  std::vector<RenderGraph::RuntimeMetricRecord> renderGraphRuntimeMetrics{};
  RenderGraph::RunDebugStats renderGraphRunDebugStats{};
  RenderGraph::FrameWaitSummary frameWaitSummary{};
  OverlayHistory overlayHistory{};
  PassTimelineHistory passTimelineHistory{};
  CpuTimelineHistory cpuTimelineHistory{};
};

class OverlayPreparationWorker
{
public:
  explicit OverlayPreparationWorker(RenderGraph *renderGraph)
      : renderGraph_(renderGraph),
        rhi_(renderGraph != nullptr ? renderGraph->getRHI() : nullptr),
        workerThread_([this]() { run(); })
  {
  }

  OverlayPreparationWorker(const OverlayPreparationWorker &) = delete;
  OverlayPreparationWorker &operator=(const OverlayPreparationWorker &) = delete;

  ~OverlayPreparationWorker()
  {
    stop();
  }

  void enqueue(OverlayPreparationInput input)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pendingInputs_.push_back(std::move(input));
    }
    condition_.notify_one();
  }

  std::shared_ptr<const PreparedOverlayState> getLatestSnapshot() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return latestSnapshot_;
  }

  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
    }
    condition_.notify_one();
    if (workerThread_.joinable())
    {
      workerThread_.join();
    }
  }

private:
  VSMFrameStats buildVsmStats(CpuFrameReport &cpuFrame, const OverlayPreparationInput &input) const
  {
    static constexpr uint32_t kAllocatorCounterCappedRequests = 0u;
    static constexpr uint32_t kAllocatorCounterFallbackRequests = 1u;
    static constexpr uint32_t kAllocatorCounterFreePageCount = 2u;
    static constexpr uint32_t kAllocatorCounterCount = 3u;
    static constexpr uint32_t kDrawCounterHierarchyQueueSize = 0u;
    static constexpr uint32_t kDrawCounterClusterQueueSize = 1u;
    static constexpr uint32_t kDrawCounterReadQueueSize = 2u;
    static constexpr uint32_t kDrawCounterVisibleClusterCount = 3u;
    static constexpr uint32_t kDrawCounterShadowDrawOverflow = 4u;
    static constexpr uint32_t kVptDirtyBit = 1u << 0u;
    static constexpr uint32_t kVptVisibleBit = 1u << 1u;
    static constexpr uint32_t kVptAllocatedBit = 1u << 2u;
    static constexpr uint32_t kVptFallbackBit = 1u << 3u;

    VSMFrameStats vsmStats{};
    if (!input.collectStats || rhi_ == nullptr)
    {
      return vsmStats;
    }

    const std::vector<uint32_t> allocatorCounters = [&]()
    {
      std::vector<uint32_t> result;
      cpuFrame.statsAllocatorReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(
                rhi_,
                input.vsmAllocatorCountersBufferId,
                kAllocatorCounterCount);
          });
      return result;
    }();

    const std::vector<uint32_t> drawCounters = [&]()
    {
      std::vector<uint32_t> result;
      cpuFrame.statsDrawCountersReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(
                rhi_,
                input.vsmDrawCountersBufferId,
                8u);
          });
      return result;
    }();

    const uint32_t regularRenderPages =
        allocatorCounters.size() > kAllocatorCounterCappedRequests
            ? std::min(allocatorCounters[kAllocatorCounterCappedRequests], input.allocationRequestCapacity)
            : 0u;
    const uint32_t fallbackRenderPages =
        allocatorCounters.size() > kAllocatorCounterFallbackRequests
            ? std::min(allocatorCounters[kAllocatorCounterFallbackRequests], input.futureAllocationRequestCapacity)
            : 0u;
    const uint32_t totalDrawCommands =
        drawCounters.size() > kDrawCounterVisibleClusterCount
            ? drawCounters[kDrawCounterVisibleClusterCount]
            : 0u;
    const uint32_t executedDrawCommands = std::min(totalDrawCommands, input.visibleClusterDrawCapacity);

    vsmStats.normalRenderPages = regularRenderPages;
    vsmStats.normalRenderPageBudget = input.allocationRequestCapacity;
    vsmStats.fallbackRenderPages = fallbackRenderPages;
    vsmStats.renderPages = regularRenderPages + fallbackRenderPages;
    vsmStats.totalDrawCommands = totalDrawCommands;
    vsmStats.executedDrawCommands = executedDrawCommands;
    vsmStats.drawCommandOverflow =
        drawCounters.size() > kDrawCounterShadowDrawOverflow
            ? drawCounters[kDrawCounterShadowDrawOverflow]
            : 0u;
    vsmStats.hierarchyQueueSize =
        drawCounters.size() > kDrawCounterHierarchyQueueSize
            ? drawCounters[kDrawCounterHierarchyQueueSize]
            : 0u;
    vsmStats.clusterQueueSize =
        drawCounters.size() > kDrawCounterClusterQueueSize
            ? drawCounters[kDrawCounterClusterQueueSize]
            : 0u;
    vsmStats.readQueueSize =
        drawCounters.size() > kDrawCounterReadQueueSize
            ? drawCounters[kDrawCounterReadQueueSize]
            : 0u;

    vsmStats.freePhysicalPages =
        allocatorCounters.size() > kAllocatorCounterFreePageCount
            ? allocatorCounters[kAllocatorCounterFreePageCount]
            : 0u;
    vsmStats.totalPhysicalPages = input.physicalPageTableResolution * input.physicalPageTableResolution;
    vsmStats.usedPhysicalPages = vsmStats.totalPhysicalPages - std::min(vsmStats.freePhysicalPages, vsmStats.totalPhysicalPages);

    const std::vector<uint32_t> vsmPageTable = [&]()
    {
      std::vector<uint32_t> result;
      cpuFrame.statsPageTableReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(
                rhi_,
                input.vsmVirtualPageTableBufferId,
                input.activeVSMPageCount);
          });
      return result;
    }();

    cpuFrame.statsPageTableProcessMs = measureWallTimeMs(
        [&]()
        {
          for (uint32_t entry : vsmPageTable)
          {
            const bool allocated = (entry & kVptAllocatedBit) != 0u;
            const bool dirty = (entry & kVptDirtyBit) != 0u;
            const bool visible = (entry & kVptVisibleBit) != 0u;
            const bool fallback = (entry & kVptFallbackBit) != 0u;
            if (allocated)
            {
              ++vsmStats.allocatedPages;
            }
            if (allocated && !dirty)
            {
              ++vsmStats.validPages;
            }
            if (dirty)
            {
              ++vsmStats.dirtyPages;
            }
            if (visible)
            {
              ++vsmStats.visiblePages;
            }
            if (fallback)
            {
              ++vsmStats.fallbackPages;
            }
          }
        });

    if (executedDrawCommands > 0u)
    {
      const std::vector<uint32_t> pageClusterCounts = [&]()
      {
        std::vector<uint32_t> result;
        cpuFrame.statsVisibleClustersReadMs = measureWallTimeMs(
            [&]()
            {
              result = readBufferElements<uint32_t>(
                  rhi_,
                  input.vsmPageClusterCountsBufferId,
                  input.activeVSMPageCount);
            });
        return result;
      }();

      cpuFrame.statsVisibleClustersProcessMs = measureWallTimeMs(
          [&]()
          {
            uint64_t totalClustersAcrossDrawnPages = 0u;
            for (uint32_t pageClusterCount : pageClusterCounts)
            {
              if (pageClusterCount == 0u)
              {
                continue;
              }

              ++vsmStats.uniqueDrawnPages;
              totalClustersAcrossDrawnPages += pageClusterCount;
              vsmStats.maxClustersPerDrawnPage = std::max(vsmStats.maxClustersPerDrawnPage, pageClusterCount);
            }

            if (vsmStats.uniqueDrawnPages > 0u)
            {
              vsmStats.averageClustersPerDrawnPage =
                  static_cast<double>(totalClustersAcrossDrawnPages) / static_cast<double>(vsmStats.uniqueDrawnPages);
            }
          });

      const std::vector<uint32_t> drawIndirectWords = [&]()
      {
        std::vector<uint32_t> result;
        cpuFrame.statsDrawIndirectReadMs = measureWallTimeMs(
            [&]()
            {
              result = readBufferElements<uint32_t>(
                  rhi_,
                  input.vsmDrawIndirectBufferId,
                  executedDrawCommands * 4u);
            });
        return result;
      }();

      cpuFrame.statsDrawIndirectProcessMs = measureWallTimeMs(
          [&]()
          {
            for (uint32_t drawIndex = 0u; drawIndex < executedDrawCommands; ++drawIndex)
            {
              vsmStats.totalDrawTriangles += static_cast<uint64_t>(drawIndirectWords[drawIndex * 4u]) / 3u;
            }
          });
    }

    cpuFrame.statsDumpMs =
        cpuFrame.statsAllocatorReadMs +
        cpuFrame.statsDrawCountersReadMs +
        cpuFrame.statsPageTableReadMs +
        cpuFrame.statsPageTableProcessMs +
        cpuFrame.statsVisibleClustersReadMs +
        cpuFrame.statsVisibleClustersProcessMs +
        cpuFrame.statsDrawIndirectReadMs +
        cpuFrame.statsDrawIndirectProcessMs;
    cpuFrame.statsReadback.wallMs = cpuFrame.statsDumpMs;
    return vsmStats;
  }

  void run()
  {
    for (;;)
    {
      OverlayPreparationInput input{};
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(
            lock,
            [&]()
            {
              return stopRequested_ || !pendingInputs_.empty();
            });

        if (stopRequested_ && pendingInputs_.empty())
        {
          break;
        }

        input = std::move(pendingInputs_.front());
        pendingInputs_.pop_front();
      }

      PreparedOverlayState prepared{};
      prepared.frameIndex = input.frameIndex;
      prepared.cpuFrame = std::move(input.cpuFrame);
      prepared.renderGraphCpu = input.renderGraphCpu;
      prepared.vsmStats = buildVsmStats(prepared.cpuFrame, input);
      const auto timerReadbackStart = lib::time::TimeSpan::now();
      const std::vector<TimerReadbackRecord> timerRecords =
          (input.collectTimers && renderGraph_ != nullptr)
              ? renderGraph_->readResolvedTimerRecords(input.timerFrameSlot)
              : std::vector<TimerReadbackRecord>{};
      prepared.cpuFrame.timerReadbackMs = input.collectTimers ? (lib::time::TimeSpan::now() - timerReadbackStart).milliseconds() : 0.0;
      prepared.passTimings = buildPassTimingSummaries(timerRecords);
      prepared.cpuFrame.totalMs = computeTrackedCpuFrameMs(prepared.cpuFrame);
      prepared.cpuFrame.untrackedMs = 0.0;
      prepared.deltaTimeMs = static_cast<float>(prepared.cpuFrame.totalMs);
      prepared.currentCpuSections = buildSortedCpuSectionSummaries(prepared.cpuFrame);
      prepared.renderGraphRunTotalMs = input.renderGraphRunTotalMs;
      prepared.renderGraphRunPhases = std::move(input.renderGraphRunPhases);
      prepared.renderGraphRuntimeMetrics = std::move(input.renderGraphRuntimeMetrics);
      prepared.renderGraphRunDebugStats = input.renderGraphRunDebugStats;
      prepared.frameWaitSummary = input.frameWaitSummary;

      if (prepared.deltaTimeMs > 0.0f)
      {
        overlayHistory_.push(prepared.deltaTimeMs);
      }
      passTimelineHistory_.pushFrame(prepared.frameIndex, prepared.passTimings);
      cpuTimelineHistory_.pushFrame(prepared.frameIndex, buildCpuSectionSummaries(prepared.cpuFrame));
      prepared.overlayHistory = overlayHistory_;
      prepared.passTimelineHistory = passTimelineHistory_;
      prepared.cpuTimelineHistory = cpuTimelineHistory_;

      std::shared_ptr<const PreparedOverlayState> snapshot = std::make_shared<PreparedOverlayState>(std::move(prepared));
      {
        std::lock_guard<std::mutex> lock(mutex_);
        latestSnapshot_ = std::move(snapshot);
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  RenderGraph *renderGraph_ = nullptr;
  rendering::RHI *rhi_ = nullptr;
  std::thread workerThread_;
  std::deque<OverlayPreparationInput> pendingInputs_;
  std::shared_ptr<const PreparedOverlayState> latestSnapshot_;
  bool stopRequested_ = false;
  OverlayHistory overlayHistory_{};
  PassTimelineHistory passTimelineHistory_{};
  CpuTimelineHistory cpuTimelineHistory_{};
};

static double computeTrackedCpuFrameMs(const CpuFrameReport &cpuFrame)
{
  return
      cpuFrame.windowUpdateMs +
      cpuFrame.simulationMs +
      cpuFrame.cameraBuildMs +
      cpuFrame.animationMs +
      cpuFrame.preCullingUniformMs +
      cpuFrame.depthPreUniformMs +
      cpuFrame.finalCullingUniformMs +
      cpuFrame.hardwareDrawUniformMs +
      cpuFrame.materialCameraMs +
      cpuFrame.materialBeginFrameMs +
      cpuFrame.vsmLightSetupMs +
      cpuFrame.vsmUpdateMs +
      cpuFrame.vsmInvalidateMs +
      cpuFrame.acquireMs +
      cpuFrame.renderRunMs +
      cpuFrame.waitBlockMs +
      cpuFrame.timerReadbackMs +
      cpuFrame.vtFeedback.wallMs +
      cpuFrame.streamingMs +
      cpuFrame.vsmResetInvalidationsMs +
      cpuFrame.instanceUploadMs +
      cpuFrame.statsAllocatorReadMs +
      cpuFrame.statsDrawCountersReadMs +
      cpuFrame.statsPageTableReadMs +
      cpuFrame.statsPageTableProcessMs +
      cpuFrame.statsVisibleClustersReadMs +
      cpuFrame.statsVisibleClustersProcessMs +
      cpuFrame.statsDrawIndirectReadMs +
      cpuFrame.statsDrawIndirectProcessMs +
      cpuFrame.uiBuildMs +
      cpuFrame.presentMs;
}

static void drawPassTimelineGraph(
    const char *canvasId,
    const char *emptyLabel,
    const PassTimelineHistory &timelineHistory,
    PassTimelineHistory::Filter filter,
    ImVec2 requestedSize)
{
  ImVec2 canvasSize = requestedSize;
  if (canvasSize.x <= 0.0f)
  {
    canvasSize.x = ImGui::GetContentRegionAvail().x;
  }
  if (canvasSize.y <= 0.0f)
  {
    canvasSize.y = 240.0f;
  }

  const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(canvasId, canvasSize);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 outerMin = canvasPos;
  const ImVec2 outerMax = ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
  drawList->AddRectFilled(outerMin, outerMax, IM_COL32(14, 18, 24, 210), 8.0f);
  drawList->AddRect(outerMin, outerMax, IM_COL32(90, 98, 110, 255), 8.0f);

  const float leftPad = 52.0f;
  const float rightPad = 12.0f;
  const float topPad = 12.0f;
  const float bottomPad = 22.0f;
  const ImVec2 plotMin = ImVec2(outerMin.x + leftPad, outerMin.y + topPad);
  const ImVec2 plotMax = ImVec2(outerMax.x - rightPad, outerMax.y - bottomPad);
  const float plotWidth = plotMax.x - plotMin.x;
  const float plotHeight = plotMax.y - plotMin.y;

  if (plotWidth <= 1.0f || plotHeight <= 1.0f)
  {
    return;
  }

  const float maxMs = std::max(1.0f, timelineHistory.getVisibleMaxMs(filter) * 1.1f);
  for (int gridIndex = 0; gridIndex < 5; ++gridIndex)
  {
    const float t = static_cast<float>(gridIndex) / 4.0f;
    const float y = plotMax.y + (plotMin.y - plotMax.y) * t;
    drawList->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), IM_COL32(60, 68, 80, 200), 1.0f);

    char label[32];
    std::snprintf(label, sizeof(label), "%.2f", maxMs * t);
    drawList->AddText(ImVec2(outerMin.x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 196, 205, 255), label);
  }

  drawList->AddText(ImVec2(plotMin.x, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "oldest");
  drawList->AddText(ImVec2(plotMax.x - 42.0f, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "newest");

  if (timelineHistory.count == 0u)
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), emptyLabel);
    return;
  }

  const std::vector<const PassTimelineHistory::Series *> orderedSeries = timelineHistory.getSortedSeries(filter);
  if (orderedSeries.empty())
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), emptyLabel);
    return;
  }

  drawList->PushClipRect(plotMin, plotMax, true);
  for (const PassTimelineHistory::Series *entry : orderedSeries)
  {
    std::array<float, PassTimelineHistory::kSampleCount> values{};
    timelineHistory.buildOrderedValues(*entry, values);

    bool hasNonZeroSample = false;
    for (size_t sampleIndex = 0u; sampleIndex < timelineHistory.count; ++sampleIndex)
    {
      if (values[sampleIndex] > 0.0f)
      {
        hasNonZeroSample = true;
        break;
      }
    }
    if (!hasNonZeroSample)
    {
      continue;
    }

    ImVec2 previousPoint{};
    bool hasPreviousPoint = false;
    const size_t pointCount = std::max<size_t>(timelineHistory.count, 1u);
    for (size_t sampleIndex = 0u; sampleIndex < timelineHistory.count; ++sampleIndex)
    {
      const float normalizedX = pointCount > 1u ? (static_cast<float>(sampleIndex) / static_cast<float>(pointCount - 1u)) : 0.0f;
      const float normalizedY = std::clamp(values[sampleIndex] / maxMs, 0.0f, 1.0f);
      const ImVec2 point(
          plotMin.x + (plotMax.x - plotMin.x) * normalizedX,
          plotMax.y + (plotMin.y - plotMax.y) * normalizedY);

      if (hasPreviousPoint)
      {
        drawList->AddLine(previousPoint, point, entry->color, 1.75f);
      }
      previousPoint = point;
      hasPreviousPoint = true;
    }

    if (hasPreviousPoint)
    {
      drawList->AddCircleFilled(previousPoint, 2.5f, entry->color, 10);
    }
  }
  drawList->PopClipRect();
}

static void drawPassLegendSection(
    const char *heading,
    const char *emptyLabel,
    const PassTimelineHistory &timelineHistory,
    PassTimelineHistory::Filter filter,
    float availableWidth)
{
  ImGui::TextUnformatted(heading);
  ImGui::Separator();

  const std::vector<const PassTimelineHistory::Series *> orderedSeries = timelineHistory.getLegendSeriesSortedByLatest(filter);
  if (orderedSeries.empty())
  {
    ImGui::TextDisabled("%s", emptyLabel);
    return;
  }

  for (const PassTimelineHistory::Series *entry : orderedSeries)
  {
    const ImVec2 swatchMin = ImGui::GetCursorScreenPos();
    const ImVec2 swatchMax = ImVec2(swatchMin.x + 10.0f, swatchMin.y + 10.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(swatchMin, swatchMax, entry->color, 2.0f);
    ImGui::Dummy(ImVec2(14.0f, 10.0f));
    ImGui::SameLine();

    const char *passType =
        (entry->hasComputeWork && entry->hasGraphicsWork) ? "mixed" :
        (entry->hasComputeWork ? "compute" : "graphics");
    ImGui::Text("%.3f ms [%s]", entry->latestMs, passType);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availableWidth - 36.0f);
    ImGui::TextUnformatted(entry->name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }
}

static void buildPassTimelineOverlay(const PassTimelineHistory &timelineHistory)
{
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags overlayWindowFlags =
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float columnGap = 16.0f;
  const float columnWidth = std::min(430.0f, (viewport->WorkSize.x - outerMargin * 2.0f - columnGap) * 0.5f);
  const float rightColumnX = viewport->WorkPos.x + viewport->WorkSize.x - outerMargin - columnWidth;
  const float topWindowHeight = 138.0f;
  const float graphY = viewport->WorkPos.y + outerMargin + topWindowHeight + 12.0f;
  const float graphHeight = std::max(220.0f, viewport->WorkSize.y - (graphY - viewport->WorkPos.y) - outerMargin);
  const float sectionGap = 12.0f;
  const float sectionHeight = std::max(104.0f, (graphHeight - sectionGap) * 0.5f);

  ImGui::SetNextWindowPos(ImVec2(rightColumnX, graphY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(columnWidth, graphHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.84f);
  ImGui::Begin("Pass Timeline", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::Text("Per-pass GPU history over the last %d frames", static_cast<int>(timelineHistory.count));
  ImGui::TextUnformatted("Compute Passes");
  drawPassTimelineGraph(
      "ComputePassTimelineCanvas",
      "No compute GPU timer samples yet",
      timelineHistory,
      PassTimelineHistory::Filter::Compute,
      ImVec2(-1.0f, sectionHeight - 28.0f));
  ImGui::Dummy(ImVec2(0.0f, sectionGap - 4.0f));
  ImGui::TextUnformatted("Graphics Passes");
  drawPassTimelineGraph(
      "GraphicsPassTimelineCanvas",
      "No graphics GPU timer samples yet",
      timelineHistory,
      PassTimelineHistory::Filter::Graphics,
      ImVec2(-1.0f, sectionHeight - 28.0f));
  ImGui::End();
}

static void drawCpuTimelineGraph(const CpuTimelineHistory &timelineHistory, ImVec2 requestedSize)
{
  ImVec2 canvasSize = requestedSize;
  if (canvasSize.x <= 0.0f)
  {
    canvasSize.x = ImGui::GetContentRegionAvail().x;
  }
  if (canvasSize.y <= 0.0f)
  {
    canvasSize.y = 140.0f;
  }

  const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("CpuTimelineCanvas", canvasSize);

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 outerMin = canvasPos;
  const ImVec2 outerMax = ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
  drawList->AddRectFilled(outerMin, outerMax, IM_COL32(14, 18, 24, 210), 8.0f);
  drawList->AddRect(outerMin, outerMax, IM_COL32(90, 98, 110, 255), 8.0f);

  const float leftPad = 52.0f;
  const float rightPad = 12.0f;
  const float topPad = 12.0f;
  const float bottomPad = 22.0f;
  const ImVec2 plotMin = ImVec2(outerMin.x + leftPad, outerMin.y + topPad);
  const ImVec2 plotMax = ImVec2(outerMax.x - rightPad, outerMax.y - bottomPad);
  const float plotWidth = plotMax.x - plotMin.x;
  const float plotHeight = plotMax.y - plotMin.y;

  if (plotWidth <= 1.0f || plotHeight <= 1.0f)
  {
    return;
  }

  const float maxMs = std::max(1.0f, timelineHistory.getVisibleMaxMs() * 1.1f);
  for (int gridIndex = 0; gridIndex < 5; ++gridIndex)
  {
    const float t = static_cast<float>(gridIndex) / 4.0f;
    const float y = plotMax.y + (plotMin.y - plotMax.y) * t;
    drawList->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), IM_COL32(60, 68, 80, 200), 1.0f);

    char label[32];
    std::snprintf(label, sizeof(label), "%.2f", maxMs * t);
    drawList->AddText(ImVec2(outerMin.x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 196, 205, 255), label);
  }

  drawList->AddText(ImVec2(plotMin.x, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "oldest");
  drawList->AddText(ImVec2(plotMax.x - 42.0f, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "newest");

  if (timelineHistory.count == 0u)
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), "No CPU section samples yet");
    return;
  }

  const std::vector<const CpuTimelineHistory::Series *> orderedSeries = timelineHistory.getSortedSeries();
  drawList->PushClipRect(plotMin, plotMax, true);
  for (const CpuTimelineHistory::Series *entry : orderedSeries)
  {
    std::array<float, CpuTimelineHistory::kSampleCount> values{};
    timelineHistory.buildOrderedValues(*entry, values);

    bool hasNonZeroSample = false;
    for (size_t sampleIndex = 0u; sampleIndex < timelineHistory.count; ++sampleIndex)
    {
      if (values[sampleIndex] > 0.0f)
      {
        hasNonZeroSample = true;
        break;
      }
    }
    if (!hasNonZeroSample)
    {
      continue;
    }

    ImVec2 previousPoint{};
    bool hasPreviousPoint = false;
    const size_t pointCount = std::max<size_t>(timelineHistory.count, 1u);
    for (size_t sampleIndex = 0u; sampleIndex < timelineHistory.count; ++sampleIndex)
    {
      const float normalizedX = pointCount > 1u ? (static_cast<float>(sampleIndex) / static_cast<float>(pointCount - 1u)) : 0.0f;
      const float normalizedY = std::clamp(values[sampleIndex] / maxMs, 0.0f, 1.0f);
      const ImVec2 point(
          plotMin.x + (plotMax.x - plotMin.x) * normalizedX,
          plotMax.y + (plotMin.y - plotMax.y) * normalizedY);

      if (hasPreviousPoint)
      {
        drawList->AddLine(previousPoint, point, entry->color, 1.5f);
      }
      previousPoint = point;
      hasPreviousPoint = true;
    }

    if (hasPreviousPoint)
    {
      drawList->AddCircleFilled(previousPoint, 2.0f, entry->color, 10);
    }
  }
  drawList->PopClipRect();
}

static double nsToMs(uint64_t ns);

static void buildCpuTimelineOverlay(
    const CpuTimelineHistory &timelineHistory,
    const CpuFrameReport &cpuFrame,
    const std::vector<CpuSectionTimingSummary> &currentSections,
    double renderGraphRunTotalMs,
    const std::vector<RenderGraph::RunPhaseRecord> &renderGraphRunPhases,
    const std::vector<RenderGraph::RuntimeMetricRecord> &renderGraphRuntimeMetrics,
    const RenderGraph::RunDebugStats &renderGraphRunDebugStats,
    const RenderGraph::FrameWaitSummary &frameWaitSummary)
{
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags overlayWindowFlags =
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float windowWidth = std::min(620.0f, viewport->WorkSize.x - outerMargin * 2.0f);
  const float windowHeight = std::min(460.0f, viewport->WorkSize.y * 0.62f);
  const float windowX = viewport->WorkPos.x + (viewport->WorkSize.x - windowWidth) * 0.5f;
  const float windowY = viewport->WorkPos.y + viewport->WorkSize.y - outerMargin - windowHeight;

  ImGui::SetNextWindowPos(ImVec2(windowX, windowY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.84f);
  ImGui::Begin("CPU Timeline", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.85f);
  ImGui::Text("CPU section history over the last %d frames", static_cast<int>(timelineHistory.count));
  ImGui::TextDisabled("Completed-frame diagnostics, delayed by 1 frame");
  drawCpuTimelineGraph(timelineHistory, ImVec2(-1.0f, 72.0f));
  ImGui::Separator();

  ImGui::Text("Completed CPU frame %.3f ms", cpuFrame.totalMs);
  if (ImGui::CollapsingHeader("Current CPU Sections", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (currentSections.empty())
    {
      ImGui::TextDisabled("No completed CPU sections yet");
    }
    else if (ImGui::BeginTable("CurrentCpuSectionsTable", 3, ImGuiTableFlags_SizingStretchProp))
    {
      const size_t itemsPerColumn = (currentSections.size() + 2u) / 3u;
      for (size_t row = 0u; row < itemsPerColumn; ++row)
      {
        ImGui::TableNextRow();
        for (int column = 0; column < 3; ++column)
        {
          const size_t index = row + static_cast<size_t>(column) * itemsPerColumn;
          ImGui::TableSetColumnIndex(column);
          if (index >= currentSections.size())
          {
            continue;
          }

          const CpuSectionTimingSummary &entry = currentSections[index];
          ImGui::Text("%s %.3f", entry.name.c_str(), entry.ms);
        }
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("RenderGraph Run", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Run %.3f ms  Submit %u  Cmd buffers %u", renderGraphRunTotalMs, renderGraphRunDebugStats.submissionCount, renderGraphRunDebugStats.submittedCommandBufferCount);
    ImGui::Text(
        "Wait %.3f ms  Block %.3f ms  Timer readback %.3f ms",
        cpuFrame.waitMs,
        cpuFrame.waitBlockMs,
        cpuFrame.timerReadbackMs);
    ImGui::Text(
        "Ext waits %u  Same-queue skipped %u  Waited nodes %u",
        renderGraphRunDebugStats.externalWaitDependencyCount,
        renderGraphRunDebugStats.sameQueueWaitDependencyCount,
        frameWaitSummary.waitedNodeCount);
    ImGui::Text(
        "Buffer barriers %u  Texture barriers %u",
        renderGraphRunDebugStats.emittedBufferBarrierCount,
        renderGraphRunDebugStats.emittedTextureBarrierCount);

    if (ImGui::BeginTable("RenderGraphRunTable", 3, ImGuiTableFlags_SizingStretchProp))
    {
      for (const RenderGraph::RunPhaseRecord &phase : renderGraphRunPhases)
      {
        const double phaseMs = static_cast<double>(phase.totalNs) / 1e6;
        const double phasePct = renderGraphRunTotalMs > 0.0 ? ((phaseMs / renderGraphRunTotalMs) * 100.0) : 0.0;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(phase.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f ms", phaseMs);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.1f%%", phasePct);
      }
      ImGui::EndTable();
    }
  }

  if (!renderGraphRuntimeMetrics.empty() && ImGui::CollapsingHeader("Runtime Resource Metrics"))
  {
    if (ImGui::BeginTable("RenderGraphRuntimeMetrics", 4, ImGuiTableFlags_SizingStretchProp))
    {
      for (const auto &metric : renderGraphRuntimeMetrics)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(metric.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f ms", static_cast<double>(metric.totalNs) / 1e6);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%llu", static_cast<unsigned long long>(metric.callCount));
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.3f ms", static_cast<double>(metric.maxNs) / 1e6);
      }
      ImGui::EndTable();
    }
  }

  ImGui::End();
}

static void buildStatsOverlay(
    uint32_t frameIndex,
    float deltaTimeMs,
    bool shadowPcfEnabled,
    const CpuFrameReport &cpuFrame,
    const RenderGraph::CpuStats &renderGraphCpu,
    const VSMFrameStats &vsmStats,
    const std::vector<PassTimingSummary> &passTimings,
    const OverlayHistory &history,
    const PassTimelineHistory &timelineHistory)
{
  std::array<float, OverlayHistory::kSampleCount> orderedFrameMs{};
  std::array<float, OverlayHistory::kSampleCount> orderedFps{};
  history.buildOrderedFrameTimes(orderedFrameMs);
  history.buildOrderedFps(orderedFps);

  double totalGpuMs = 0.0;
  double maxPassGpuMs = 0.0;
  for (const PassTimingSummary &pass : passTimings)
  {
    totalGpuMs += pass.gpuTimeMs;
    maxPassGpuMs = std::max(maxPassGpuMs, pass.gpuTimeMs);
  }

  const double vtCpuMs = cpuFrame.vtFeedback.wallMs + cpuFrame.streamingMs;
  const double vsmCpuMs =
      cpuFrame.vsmLightSetupMs +
      cpuFrame.vsmUpdateMs +
      cpuFrame.vsmInvalidateMs +
      cpuFrame.vsmResetInvalidationsMs +
      cpuFrame.statsAllocatorReadMs +
      cpuFrame.statsDrawCountersReadMs +
      cpuFrame.statsPageTableReadMs +
      cpuFrame.statsPageTableProcessMs +
      cpuFrame.statsVisibleClustersReadMs +
      cpuFrame.statsVisibleClustersProcessMs +
      cpuFrame.statsDrawIndirectReadMs +
      cpuFrame.statsDrawIndirectProcessMs;

  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags overlayWindowFlags =
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float columnGap = 16.0f;
  const float columnWidth = std::min(430.0f, (viewport->WorkSize.x - outerMargin * 2.0f - columnGap) * 0.5f);
  const float leftColumnX = viewport->WorkPos.x + outerMargin;
  const float rightColumnX = viewport->WorkPos.x + viewport->WorkSize.x - outerMargin - columnWidth;
  const float topY = viewport->WorkPos.y + outerMargin;
  const float topWindowHeight = 118.0f;
  const float bottomY = topY + topWindowHeight + 12.0f;
  const float bottomHeight = std::max(180.0f, viewport->WorkSize.y - (bottomY - viewport->WorkPos.y) - outerMargin);

  ImGui::SetNextWindowPos(ImVec2(leftColumnX, topY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(columnWidth, topWindowHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGui::Begin("Renderer Stats", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::Text("Frame %u", frameIndex);
  ImGui::Text("Delta %.3f ms  FPS %.1f", deltaTimeMs, deltaTimeMs > 0.0f ? (1000.0f / deltaTimeMs) : 0.0f);
  ImGui::Text("GPU %.3f ms  Max Pass %.3f ms  Passes %d", totalGpuMs, maxPassGpuMs, static_cast<int>(passTimings.size()));
  ImGui::Text("Acquire %.3f  Run %.3f  Wait %.3f", cpuFrame.acquireMs, cpuFrame.renderRunMs, cpuFrame.waitMs);
  ImGui::Text("Wait block %.3f  Timers %.3f  Stats %.3f", cpuFrame.waitBlockMs, cpuFrame.timerReadbackMs, cpuFrame.statsDumpMs);
  ImGui::Text("Window %.3f  Sim %.3f  Cam %.3f  Uniforms %.3f", cpuFrame.windowUpdateMs, cpuFrame.simulationMs, cpuFrame.cameraBuildMs, cpuFrame.uniformUploadMs);
  ImGui::Text("Feedback %.3f  Stream %.3f  Inst %.3f  UI %.3f", cpuFrame.vtFeedback.wallMs, cpuFrame.streamingMs, cpuFrame.instanceUploadMs, cpuFrame.uiBuildMs);
  ImGui::Text("VT CPU %.3f  VSM CPU %.3f  PCF %s  Untracked %.3f", vtCpuMs, vsmCpuMs, shadowPcfEnabled ? "on" : "off", cpuFrame.untrackedMs);
  ImGui::Text("Present %.3f", cpuFrame.presentMs);
  ImGui::Text(
      "RG writes %llu / %.3f ms  reads %llu / %.3f ms",
      static_cast<unsigned long long>(renderGraphCpu.bufferWrites.callCount),
      static_cast<double>(renderGraphCpu.bufferWrites.totalNs) / 1e6,
      static_cast<unsigned long long>(renderGraphCpu.bufferReads.callCount),
      static_cast<double>(renderGraphCpu.bufferReads.totalNs) / 1e6);
  if (history.count > 1u)
  {
    ImGui::Separator();
    ImGui::PlotLines("Frame ms", orderedFrameMs.data(), static_cast<int>(history.count), 0, nullptr, 0.0f, 40.0f, ImVec2(columnWidth - 24.0f, 24.0f));
    ImGui::PlotLines("FPS", orderedFps.data(), static_cast<int>(history.count), 0, nullptr, 0.0f, 240.0f, ImVec2(columnWidth - 24.0f, 24.0f));
  }
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(rightColumnX, topY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(columnWidth, topWindowHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGui::Begin("VSM Stats", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::Text("Draw jobs %u  executed %u  overflow %u", vsmStats.totalDrawCommands, vsmStats.executedDrawCommands, vsmStats.drawCommandOverflow);
  ImGui::Text("Triangles %llu  unique pages %u", static_cast<unsigned long long>(vsmStats.totalDrawTriangles), vsmStats.uniqueDrawnPages);
  ImGui::Text("Clusters/page avg %.2f  max %u", vsmStats.averageClustersPerDrawnPage, vsmStats.maxClustersPerDrawnPage);
  ImGui::Text("Render pages %u  normal %u / %u  fallback %u", vsmStats.renderPages, vsmStats.normalRenderPages, vsmStats.normalRenderPageBudget, vsmStats.fallbackRenderPages);
  ImGui::Text("Queues H %u  C %u  R %u", vsmStats.hierarchyQueueSize, vsmStats.clusterQueueSize, vsmStats.readQueueSize);
  ImGui::Text("VPT alloc %u  valid %u  dirty %u", vsmStats.allocatedPages, vsmStats.validPages, vsmStats.dirtyPages);
  ImGui::Text("Visible %u  fallback %u", vsmStats.visiblePages, vsmStats.fallbackPages);
  ImGui::Text("Physical pages %u / %u  free %u", vsmStats.usedPhysicalPages, vsmStats.totalPhysicalPages, vsmStats.freePhysicalPages);
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(leftColumnX, bottomY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(columnWidth, bottomHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.84f);
  ImGui::Begin("Pass Legend", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.9f);
  const float legendSectionGap = 10.0f;
  const float legendPanelWidth = ImGui::GetContentRegionAvail().x;
  const float legendPanelHeight = std::max(96.0f, (ImGui::GetContentRegionAvail().y - legendSectionGap) * 0.5f);

  ImGui::BeginChild("ComputePassLegendPanel", ImVec2(0.0f, legendPanelHeight), true, ImGuiWindowFlags_None);
  drawPassLegendSection(
      "Compute Passes",
      "No compute-timed passes",
      timelineHistory,
      PassTimelineHistory::Filter::Compute,
      legendPanelWidth);
  ImGui::EndChild();

  ImGui::Dummy(ImVec2(0.0f, legendSectionGap));

  ImGui::BeginChild("GraphicsPassLegendPanel", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_None);
  drawPassLegendSection(
      "Graphics Passes",
      "No graphics-timed passes",
      timelineHistory,
      PassTimelineHistory::Filter::Graphics,
      legendPanelWidth);
  ImGui::EndChild();
  ImGui::End();
}

static int shadowPcfTapCountToIndex(uint32_t tapCount)
{
  switch (tapCount)
  {
  case 1u:
    return 0;
  case 4u:
    return 1;
  case 9u:
    return 2;
  case 16u:
    return 3;
  default:
    return 1;
  }
}

static uint32_t shadowPcfTapIndexToCount(int tapIndex)
{
  static constexpr std::array<uint32_t, 4> kTapCounts = {1u, 4u, 9u, 16u};
  const int clampedIndex = std::clamp(tapIndex, 0, static_cast<int>(kTapCounts.size()) - 1);
  return kTapCounts[static_cast<size_t>(clampedIndex)];
}

static bool buildShadowControlsOverlay(
    float &prepassLodErrorThreshold,
    float &geometryLodErrorThreshold,
    float &vsmLodErrorThreshold,
    bool &vsmDirtyPageStencilEnabled,
    bool &shadowPcfEnabled,
    uint32_t &shadowFilterTaps,
    float &shadowBias,
    float &slopeScaleBias,
    float &maxShadowBias,
    float &pcfRadiusTexels,
    float &normalBiasTexels,
    std::array<float, 3> &ambientShadowColor,
    bool &screenSpaceShadowEnabled,
    float &screenSpaceSurfaceThickness,
    float &screenSpaceBilinearThreshold,
    float &screenSpaceShadowContrast,
    float &screenSpaceRayDistance,
    std::array<float, 2> &screenSpaceDepthBounds,
    bool &screenSpaceIgnoreEdgePixels,
    bool &screenSpaceUsePrecisionOffset,
    bool &screenSpaceBilinearSamplingOffsetMode,
    bool &screenSpaceUseEarlyOut,
    bool &screenSpaceTreatSkippedEdgeSamplesAsLit,
    bool &screenSpaceDebugOutputEdgeMask,
    bool &screenSpaceDebugOutputThreadIndex,
    bool &screenSpaceDebugOutputWaveIndex)
{
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags windowFlags =
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float windowWidth = std::min(420.0f, viewport->WorkSize.x - outerMargin * 2.0f);
  const float windowX = viewport->WorkPos.x + (viewport->WorkSize.x - windowWidth) * 0.5f;
  const float windowY = viewport->WorkPos.y + outerMargin;

  ImGui::SetNextWindowPos(ImVec2(windowX, windowY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(windowWidth, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);

  bool changed = false;
  ImGui::Begin("Shadow Controls", nullptr, windowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::TextUnformatted("Runtime renderer controls");
  ImGui::TextDisabled("Tab / Shift+Tab / arrows work while mouse-look stays active.");
  ImGui::SeparatorText("LOD");
  changed |= ImGui::SliderFloat("Prepass error LOD", &prepassLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  changed |= ImGui::SliderFloat("Geometry error LOD", &geometryLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  changed |= ImGui::SliderFloat("VSM error LOD", &vsmLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  ImGui::SeparatorText("VSM");
  changed |= ImGui::Checkbox("Dirty-page stencil", &vsmDirtyPageStencilEnabled);
  changed |= ImGui::Checkbox("Enable PCF", &shadowPcfEnabled);

  int tapIndex = shadowPcfTapCountToIndex(shadowFilterTaps);
  if (ImGui::SliderInt("PCF taps", &tapIndex, 0, 3))
  {
    shadowFilterTaps = shadowPcfTapIndexToCount(tapIndex);
    changed = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%u", shadowFilterTaps);

  changed |= ImGui::SliderFloat("PCF radius", &pcfRadiusTexels, 0.0f, 6.0f, "%.2f texels");
  changed |= ImGui::SliderFloat("Shadow bias", &shadowBias, 0.0f, 0.02f, "%.5f");
  changed |= ImGui::SliderFloat("Slope-scale bias", &slopeScaleBias, 0.0f, 8.0f, "%.3f");
  changed |= ImGui::SliderFloat("Max shadow bias", &maxShadowBias, 0.0f, 0.03f, "%.5f");
  changed |= ImGui::SliderFloat("Normal bias", &normalBiasTexels, 0.0f, 8.0f, "%.2f texels");
  changed |= ImGui::ColorEdit3("Shadow ambient", ambientShadowColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);

  ImGui::SeparatorText("Screen Space Shadow");
  changed |= ImGui::Checkbox("Enable SSS", &screenSpaceShadowEnabled);
  changed |= ImGui::SliderFloat("SSS surface thickness", &screenSpaceSurfaceThickness, 0.0005f, 0.05f, "%.4f");
  changed |= ImGui::SliderFloat("SSS bilinear threshold", &screenSpaceBilinearThreshold, 0.0f, 0.25f, "%.3f");
  changed |= ImGui::SliderFloat("SSS contrast", &screenSpaceShadowContrast, 0.25f, 8.0f, "%.2f");
  changed |= ImGui::SliderFloat("SSS ray distance", &screenSpaceRayDistance, 1.0f, 120.0f, "%.0f px");
  changed |= ImGui::SliderFloat2("SSS depth bounds", screenSpaceDepthBounds.data(), 0.0f, 1.0f, "%.3f");
  if (screenSpaceDepthBounds[0] > screenSpaceDepthBounds[1])
  {
    std::swap(screenSpaceDepthBounds[0], screenSpaceDepthBounds[1]);
    changed = true;
  }
  changed |= ImGui::Checkbox("SSS ignore edge pixels", &screenSpaceIgnoreEdgePixels);
  changed |= ImGui::Checkbox("SSS precision offset", &screenSpaceUsePrecisionOffset);
  changed |= ImGui::Checkbox("SSS bilinear offset mode", &screenSpaceBilinearSamplingOffsetMode);
  changed |= ImGui::Checkbox("SSS early out", &screenSpaceUseEarlyOut);
  changed |= ImGui::Checkbox("SSS skipped edges are fully lit", &screenSpaceTreatSkippedEdgeSamplesAsLit);
  ImGui::SeparatorText("SSS Debug");
  changed |= ImGui::Checkbox("SSS edge mask", &screenSpaceDebugOutputEdgeMask);
  changed |= ImGui::Checkbox("SSS thread index", &screenSpaceDebugOutputThreadIndex);
  changed |= ImGui::Checkbox("SSS wave index", &screenSpaceDebugOutputWaveIndex);
  ImGui::End();
  return changed;
}

static constexpr uint32_t VSM_ALLOCATOR_COUNTER_CAPPED_REQUESTS = 0u;
static constexpr uint32_t VSM_ALLOCATOR_COUNTER_FALLBACK_REQUESTS = 1u;
static constexpr uint32_t VSM_ALLOCATOR_COUNTER_FREE_PAGE_COUNT = 2u;
static constexpr uint32_t VSM_DRAW_COUNTER_HIERARCHY_QUEUE_SIZE = 0u;
static constexpr uint32_t VSM_DRAW_COUNTER_CLUSTER_QUEUE_SIZE = 1u;
static constexpr uint32_t VSM_DRAW_COUNTER_READ_QUEUE_SIZE = 2u;
static constexpr uint32_t VSM_DRAW_COUNTER_SHADOW_VISIBLE_CLUSTER_COUNT = 3u;
static constexpr uint32_t VSM_DRAW_COUNTER_SHADOW_DRAW_OVERFLOW = 4u;
static constexpr uint32_t VSM_VPT_DIRTY_BIT = 1u << 0u;
static constexpr uint32_t VSM_VPT_VISIBLE_BIT = 1u << 1u;
static constexpr uint32_t VSM_VPT_ALLOCATED_BIT = 1u << 2u;
static constexpr uint32_t VSM_VPT_FALLBACK_BIT = 1u << 3u;

static double nsToMs(uint64_t ns)
{
  return static_cast<double>(ns) / 1e6;
}

static std::string formatBytes(uint64_t bytes)
{
  std::ostringstream oss;
  if (bytes >= 1024u * 1024u)
  {
    oss << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
  }
  else if (bytes >= 1024u)
  {
    oss << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / 1024.0 << " KiB";
  }
  else
  {
    oss << bytes << " B";
  }
  return oss.str();
}

static RenderGraph::CpuTransferStats diffCpuTransferStats(const RenderGraph::CpuTransferStats &after, const RenderGraph::CpuTransferStats &before)
{
  RenderGraph::CpuTransferStats diff{};
  diff.callCount = after.callCount - before.callCount;
  diff.totalBytes = after.totalBytes - before.totalBytes;
  diff.totalNs = after.totalNs - before.totalNs;
  return diff;
}

static RenderGraph::CpuStats diffCpuStats(const RenderGraph::CpuStats &after, const RenderGraph::CpuStats &before)
{
  RenderGraph::CpuStats diff{};
  diff.bufferWrites = diffCpuTransferStats(after.bufferWrites, before.bufferWrites);
  diff.bufferReads = diffCpuTransferStats(after.bufferReads, before.bufferReads);
  return diff;
}

template <typename Fn> static void measureCpuOperation(RenderGraph *renderGraph, CpuFrameReport::Operation &operation, Fn &&fn)
{
  const RenderGraph::CpuStats cpuBefore = renderGraph->getCpuStats();
  const auto start = lib::time::TimeSpan::now();
  fn();
  const auto end = lib::time::TimeSpan::now();
  operation.wallMs = (end - start).milliseconds();
  operation.cpu = diffCpuStats(renderGraph->getCpuStats(), cpuBefore);
}

static void appendTransferDelta(std::ostringstream &ss, const char *label, const RenderGraph::CpuTransferStats &stats)
{
  ss << label << "=" << stats.callCount << "/" << nsToMs(stats.totalNs) << "ms/" << formatBytes(stats.totalBytes);
}

static void appendOperationSummary(std::ostringstream &ss, const char *label, const CpuFrameReport::Operation &operation)
{
  ss << label << "=" << std::fixed << std::setprecision(3) << operation.wallMs << "ms ";
  appendTransferDelta(ss, "w", operation.cpu.bufferWrites);
  ss << " ";
  appendTransferDelta(ss, "r", operation.cpu.bufferReads);
}

static void logCpuFrameStats(uint32_t frameIndex, const CpuFrameReport &cpuFrame, const RenderGraph::CpuStats &renderGraphCpu)
{
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(3) << "[CPU] frame=" << frameIndex << " total=" << cpuFrame.totalMs << "ms"
     << " window=" << cpuFrame.windowUpdateMs << "ms"
     << " sim=" << cpuFrame.simulationMs << "ms"
     << " cam=" << cpuFrame.cameraBuildMs << "ms"
     << " upload=" << cpuFrame.uniformUploadMs << "ms"
     << " run=" << cpuFrame.renderRunMs << "ms"
     << " wait=" << cpuFrame.waitMs << "ms"
     << " stats=" << cpuFrame.statsDumpMs << "ms"
     << " present=" << cpuFrame.presentMs << "ms"
     << " stream=" << cpuFrame.streamingMs << "ms"
     << " inst=" << cpuFrame.instanceUploadMs << "ms"
     << " writes=" << renderGraphCpu.bufferWrites.callCount << "/" << nsToMs(renderGraphCpu.bufferWrites.totalNs) << "ms/" << formatBytes(renderGraphCpu.bufferWrites.totalBytes)
     << " wMax=" << nsToMs(renderGraphCpu.bufferWrites.maxNs) << "ms"
     << " reads=" << renderGraphCpu.bufferReads.callCount << "/" << nsToMs(renderGraphCpu.bufferReads.totalNs) << "ms/" << formatBytes(renderGraphCpu.bufferReads.totalBytes)
     << " rMax=" << nsToMs(renderGraphCpu.bufferReads.maxNs) << "ms";
  os::Logger::log(ss.str());

  std::ostringstream opsA;
  opsA << "[CPU][Ops] frame=" << frameIndex << " ";
  appendOperationSummary(opsA, "uniforms", cpuFrame.uniformUpload);
  opsA << " ";
  appendOperationSummary(opsA, "vtFeedback", cpuFrame.vtFeedback);
  os::Logger::log(opsA.str());

  std::ostringstream opsB;
  opsB << "[CPU][Ops] frame=" << frameIndex << " ";
  appendOperationSummary(opsB, "stream", cpuFrame.sceneStreaming);
  opsB << " ";
  appendOperationSummary(opsB, "instance", cpuFrame.instanceUpload);
  opsB << " ";
  appendOperationSummary(opsB, "statsReadback", cpuFrame.statsReadback);
  os::Logger::log(opsB.str());
}

static void logVSMFrameStats(uint32_t frameIndex, const VSMFrameStats &stats)
{
  std::ostringstream pages;
  pages << "[VSM] frame=" << frameIndex
        << " renderPages=" << stats.renderPages
        << " normal=" << stats.normalRenderPages
        << " fallback=" << stats.fallbackRenderPages
        << " uniqueDrawnPages=" << stats.uniqueDrawnPages
        << " avgClustersPerPage=" << std::fixed << std::setprecision(2) << stats.averageClustersPerDrawnPage
        << " maxClustersPerPage=" << stats.maxClustersPerDrawnPage
        << " drawCommands=" << stats.totalDrawCommands
        << " executed=" << stats.executedDrawCommands
        << " tris=" << stats.totalDrawTriangles
        << " overflow=" << stats.drawCommandOverflow;
  os::Logger::log(pages.str());

  std::ostringstream residency;
  residency << "[VSM] frame=" << frameIndex
            << " vpt alloc=" << stats.allocatedPages
            << " valid=" << stats.validPages
            << " dirty=" << stats.dirtyPages
            << " visible=" << stats.visiblePages
            << " fallback=" << stats.fallbackPages
            << " phys used=" << stats.usedPhysicalPages << "/" << stats.totalPhysicalPages
            << " free=" << stats.freePhysicalPages
            << " queues h=" << stats.hierarchyQueueSize
            << " read=" << stats.readQueueSize
            << " cluster=" << stats.clusterQueueSize;
  os::Logger::log(residency.str());
}

static math::Quatf multiplyQuaternions(const math::Quatf &a, const math::Quatf &b)
{
  return math::Quatf(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
      .normalize();
}

#ifdef DEBUG_BINDINGS
struct ClusterVisibilityKey
{
  uint32_t pageIndex = UINT32_MAX;
  uint32_t localClusterIndex = UINT32_MAX;
  uint32_t instanceIndex = UINT32_MAX;

  bool operator==(const ClusterVisibilityKey &other) const
  {
    return pageIndex == other.pageIndex && localClusterIndex == other.localClusterIndex && instanceIndex == other.instanceIndex;
  }
};

struct ClusterVisibilityKeyHash
{
  std::size_t operator()(const ClusterVisibilityKey &key) const noexcept
  {
    std::size_t seed = 0u;
    seed ^= static_cast<std::size_t>(key.pageIndex) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
    seed ^= static_cast<std::size_t>(key.localClusterIndex) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
    seed ^= static_cast<std::size_t>(key.instanceIndex) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
    return seed;
  }
};

struct VisibleRenderedClusterState
{
  ClusterVisibilityKey key{};
  uint32_t nodeIndex = SENTINEL_VALUE;
  ClusterDebugReason clusterReason = ClusterDebugReason::NotVisited;
  HierarchyDebugReason hierarchyReason = HierarchyDebugReason::NotVisited;
  float selfErrorPx = 0.0f;
  float parentErrorPx = 0.0f;
};

struct CullingDebugSnapshot
{
  bool valid = false;
  uint32_t captureIndex = 0u;
  uint64_t frameCounter = 0u;
  float cameraPos[3] = {0.0f, 0.0f, 0.0f};
  float nearPlane = 0.0f;
  float farPlane = 0.0f;
  float lodErrorThreshold = 0.0f;
  float projY = 0.0f;
  uint32_t viewportW = 0u;
  uint32_t viewportH = 0u;
  uint32_t hiZLevels = 0u;
  std::vector<HierarchyDebugRecord> hierarchy;
  std::vector<ClusterDebugRecord> clusters;
  std::vector<PageTableEntry> pageTable;
  std::vector<VisibleRenderedClusterState> visibleRenderedClusters;
};

struct ClusterTopologyInfo
{
  bool valid = false;
  uint32_t localPageIndex = UINT32_MAX;
  uint32_t localClusterIndex = UINT32_MAX;
  uint32_t hierarchyNodeIndex = UINT32_MAX;
  LODBounds self{};
  LODBounds parent{};
};

struct ClusterCaptureInfo
{
  bool hasRecord = false;
  bool pageInstalled = false;
  uint32_t globalPageIndex = UINT32_MAX;
  uint32_t localClusterIndex = UINT32_MAX;
  uint32_t nodeIndex = SENTINEL_VALUE;
  float selfErrorPx = 0.0f;
  float parentErrorPx = 0.0f;
  ClusterDebugReason reason = ClusterDebugReason::NotVisited;
};

static float debugBitsToFloat(uint32_t bits)
{
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(float));
  return v;
}

static bool isRenderedClusterReason(ClusterDebugReason reason)
{
  return reason == ClusterDebugReason::RenderedHardware || reason == ClusterDebugReason::RenderedSoftware;
}

static uint64_t debugClusterLinearIndex(uint32_t globalPageIdx, uint32_t localIdx)
{
  return static_cast<uint64_t>(globalPageIdx) * static_cast<uint64_t>(DEBUG_MAX_CLUSTERS_PER_PAGE) + static_cast<uint64_t>(localIdx);
}

static std::vector<ClusterTopologyInfo> buildClusterTopology(const VirtualGeometryBuildData &buildData, const std::vector<VirtualGeometryHierarchy> &hierarchy)
{
  std::vector<ClusterTopologyInfo> topology(buildData.clusters.size());

  for (uint32_t pageIdx = 0u; pageIdx < buildData.pages.size(); ++pageIdx)
  {
    const VirtualGeometryBuildPage &page = buildData.pages[pageIdx];
    for (uint32_t localIdx = 0u; localIdx < page.clusterCount; ++localIdx)
    {
      const uint32_t clusterIdx = page.clusterOffset + localIdx;
      if (clusterIdx >= topology.size())
        continue;

      ClusterTopologyInfo &info = topology[clusterIdx];
      info.valid = true;
      info.localPageIndex = pageIdx;
      info.localClusterIndex = localIdx;
      info.self = buildData.clusters[clusterIdx].self;
      info.parent = buildData.clusters[clusterIdx].parent;
    }
  }

  for (uint32_t nodeIdx = 0u; nodeIdx < hierarchy.size(); ++nodeIdx)
  {
    const VirtualGeometryHierarchy &node = hierarchy[nodeIdx];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    if (node.pageIndex == UINT32_MAX)
      continue;

    const uint32_t pageIdx = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    if (pageIdx >= buildData.pages.size())
      continue;

    const VirtualGeometryBuildPage &page = buildData.pages[pageIdx];
    for (uint32_t i = 0u; i < node.child_count; ++i)
    {
      const uint32_t localIdx = node.child_start + i;
      if (localIdx >= page.clusterCount)
        continue;
      const uint32_t clusterIdx = page.clusterOffset + localIdx;
      if (clusterIdx >= topology.size())
        continue;
      topology[clusterIdx].hierarchyNodeIndex = nodeIdx;
    }
  }

  return topology;
}

static ClusterCaptureInfo gatherClusterCaptureInfo(const CullingDebugSnapshot &snapshot, const ClusterTopologyInfo &topology, uint32_t instancePageTableOffset)
{
  ClusterCaptureInfo info{};
  if (!topology.valid)
    return info;

  info.localClusterIndex = topology.localClusterIndex;
  info.globalPageIndex = instancePageTableOffset + topology.localPageIndex;
  if (info.globalPageIndex < snapshot.pageTable.size())
    info.pageInstalled = snapshot.pageTable[info.globalPageIndex].isInstalled != 0u;

  const uint64_t linearIdx = debugClusterLinearIndex(info.globalPageIndex, info.localClusterIndex);
  if (linearIdx >= snapshot.clusters.size())
    return info;

  const ClusterDebugRecord &record = snapshot.clusters[linearIdx];
  info.hasRecord = true;
  info.reason = static_cast<ClusterDebugReason>(record.reason);
  info.selfErrorPx = debugBitsToFloat(record.selfErrorPxBits);
  info.parentErrorPx = debugBitsToFloat(record.parentErrorPxBits);
  info.nodeIndex = record.nodeIndex;
  return info;
}

static HierarchyDebugReason gatherHierarchyReason(const CullingDebugSnapshot &snapshot, uint32_t nodeIndex)
{
  if (nodeIndex == SENTINEL_VALUE || nodeIndex >= snapshot.hierarchy.size())
    return HierarchyDebugReason::NotVisited;
  return static_cast<HierarchyDebugReason>(snapshot.hierarchy[nodeIndex].reason);
}

static void reportSelfErrorChildrenDiagnostics(
    const CullingDebugSnapshot &first,
    const CullingDebugSnapshot &second,
    const VirtualGeometryBuildData &buildData,
    const std::vector<ClusterTopologyInfo> &clusterTopology,
    uint32_t instancePageTableOffset)
{
  constexpr uint32_t kMaxParentsToReport = 64u;
  uint32_t parentsReported = 0u;

  os::Logger::log("\n--- SelfErrorTooHigh -> child diagnostics ---");

  for (uint32_t parentCluster = 0u; parentCluster < buildData.clusterDAG.size(); ++parentCluster)
  {
    if (parentCluster >= clusterTopology.size())
      break;

    const ClusterCaptureInfo parent0 = gatherClusterCaptureInfo(first, clusterTopology[parentCluster], instancePageTableOffset);
    const ClusterCaptureInfo parent1 = gatherClusterCaptureInfo(second, clusterTopology[parentCluster], instancePageTableOffset);
    if (parent0.reason != ClusterDebugReason::SelfErrorTooHigh && parent1.reason != ClusterDebugReason::SelfErrorTooHigh)
      continue;

    const ClusterTopologyInfo &topology = clusterTopology[parentCluster];
    const HierarchyDebugReason nodeReason0 = gatherHierarchyReason(first, topology.hierarchyNodeIndex);
    const HierarchyDebugReason nodeReason1 = gatherHierarchyReason(second, topology.hierarchyNodeIndex);

    ++parentsReported;
    os::Logger::logf(
        "parentCluster=%u page=%u local=%u node=%u | cap0=%s(selfErrPx=%.4f parentErrPx=%.4f node=%s installed=%s) | cap1=%s(selfErrPx=%.4f parentErrPx=%.4f node=%s installed=%s)",
        parentCluster,
        topology.localPageIndex,
        topology.localClusterIndex,
        topology.hierarchyNodeIndex,
        clusterDebugReasonToString(parent0.reason),
        parent0.selfErrorPx,
        parent0.parentErrorPx,
        hierarchyDebugReasonToString(nodeReason0),
        parent0.pageInstalled ? "yes" : "no",
        clusterDebugReasonToString(parent1.reason),
        parent1.selfErrorPx,
        parent1.parentErrorPx,
        hierarchyDebugReasonToString(nodeReason1),
        parent1.pageInstalled ? "yes" : "no");
    os::Logger::logf(
        "  bounds: self(c=(%.4f, %.4f, %.4f) r=%.4f e=%.6f) parent(c=(%.4f, %.4f, %.4f) r=%.4f e=%.6f)",
        topology.self.center[0],
        topology.self.center[1],
        topology.self.center[2],
        topology.self.radius,
        topology.self.error,
        topology.parent.center[0],
        topology.parent.center[1],
        topology.parent.center[2],
        topology.parent.radius,
        topology.parent.error);

    const std::vector<uint32_t> &children = buildData.clusterDAG[parentCluster];
    if (children.empty())
    {
      os::Logger::log("  no DAG children");
    }
    else
    {
      for (uint32_t childCluster : children)
      {
        if (childCluster >= clusterTopology.size())
          continue;
        const ClusterTopologyInfo &childTopology = clusterTopology[childCluster];
        const ClusterCaptureInfo child0 = gatherClusterCaptureInfo(first, childTopology, instancePageTableOffset);
        const ClusterCaptureInfo child1 = gatherClusterCaptureInfo(second, childTopology, instancePageTableOffset);
        const HierarchyDebugReason childNodeReason0 = gatherHierarchyReason(first, childTopology.hierarchyNodeIndex);
        const HierarchyDebugReason childNodeReason1 = gatherHierarchyReason(second, childTopology.hierarchyNodeIndex);

        os::Logger::logf(
            "  child=%u page=%u local=%u node=%u | cap0 cluster=%s(selfErrPx=%.4f parentErrPx=%.4f) node=%s installed=%s | cap1 cluster=%s(selfErrPx=%.4f parentErrPx=%.4f) node=%s installed=%s",
            childCluster,
            childTopology.localPageIndex,
            childTopology.localClusterIndex,
            childTopology.hierarchyNodeIndex,
            clusterDebugReasonToString(child0.reason),
            child0.selfErrorPx,
            child0.parentErrorPx,
            hierarchyDebugReasonToString(childNodeReason0),
            child0.pageInstalled ? "yes" : "no",
            clusterDebugReasonToString(child1.reason),
            child1.selfErrorPx,
            child1.parentErrorPx,
            hierarchyDebugReasonToString(childNodeReason1),
            child1.pageInstalled ? "yes" : "no");
      }
    }

    if (parentsReported >= kMaxParentsToReport)
    {
      os::Logger::logf("... truncated after %u parent clusters.", kMaxParentsToReport);
      break;
    }
  }

  if (parentsReported == 0u)
    os::Logger::log("No SelfErrorTooHigh clusters found in either capture.");
}

static void reportDebugSnapshotDiff(
    const CullingDebugSnapshot &first,
    const CullingDebugSnapshot &second,
    const VirtualGeometryBuildData &buildData,
    const std::vector<ClusterTopologyInfo> &clusterTopology,
    uint32_t instancePageTableOffset)
{
  if (!first.valid || !second.valid)
    return;

  os::Logger::log("\n=================== CULLING DEBUG SNAPSHOT REPORT ===================");
  os::Logger::logf(
      "Capture[0] frame=%llu cam=(%.3f, %.3f, %.3f) near=%.4f far=%.4f threshold=%.4f viewport=%ux%u hiZ=%u projY=%.4f",
      static_cast<unsigned long long>(first.frameCounter),
      first.cameraPos[0],
      first.cameraPos[1],
      first.cameraPos[2],
      first.nearPlane,
      first.farPlane,
      first.lodErrorThreshold,
      first.viewportW,
      first.viewportH,
      first.hiZLevels,
      first.projY);
  os::Logger::logf(
      "Capture[1] frame=%llu cam=(%.3f, %.3f, %.3f) near=%.4f far=%.4f threshold=%.4f viewport=%ux%u hiZ=%u projY=%.4f",
      static_cast<unsigned long long>(second.frameCounter),
      second.cameraPos[0],
      second.cameraPos[1],
      second.cameraPos[2],
      second.nearPlane,
      second.farPlane,
      second.lodErrorThreshold,
      second.viewportW,
      second.viewportH,
      second.hiZLevels,
      second.projY);

  const uint32_t nodeCount = static_cast<uint32_t>(std::max(first.hierarchy.size(), second.hierarchy.size()));
  uint32_t visitedNodeCount = 0u;
  os::Logger::log("\n--- Visited hierarchy nodes (capture0 vs capture1) ---");
  for (uint32_t nodeIdx = 0u; nodeIdx < nodeCount; ++nodeIdx)
  {
    const HierarchyDebugRecord rec0 = (nodeIdx < first.hierarchy.size()) ? first.hierarchy[nodeIdx] : HierarchyDebugRecord{};
    const HierarchyDebugRecord rec1 = (nodeIdx < second.hierarchy.size()) ? second.hierarchy[nodeIdx] : HierarchyDebugRecord{};
    const auto reason0 = static_cast<HierarchyDebugReason>(rec0.reason);
    const auto reason1 = static_cast<HierarchyDebugReason>(rec1.reason);
    if (reason0 == HierarchyDebugReason::NotVisited && reason1 == HierarchyDebugReason::NotVisited)
      continue;
    ++visitedNodeCount;
    os::Logger::logf(
        "node=%u | cap0=%s (parentErrPx=%.4f threshold=%.4f inst=%u) | cap1=%s (parentErrPx=%.4f threshold=%.4f inst=%u)",
        nodeIdx,
        hierarchyDebugReasonToString(reason0),
        debugBitsToFloat(rec0.parentErrorPxBits),
        debugBitsToFloat(rec0.thresholdPxBits),
        rec0.instanceIndex,
        hierarchyDebugReasonToString(reason1),
        debugBitsToFloat(rec1.parentErrorPxBits),
        debugBitsToFloat(rec1.thresholdPxBits),
        rec1.instanceIndex);
  }
  os::Logger::logf("Visited nodes in union: %u", visitedNodeCount);

  const uint32_t clusterCount = static_cast<uint32_t>(std::max(first.clusters.size(), second.clusters.size()));
  uint32_t visitedClusterCount = 0u;
  uint32_t renderedOnlyInFirst = 0u;
  uint32_t renderedOnlyInSecond = 0u;

  os::Logger::log("\n--- Visited clusters (capture0 vs capture1) ---");
  for (uint32_t clusterLinearIdx = 0u; clusterLinearIdx < clusterCount; ++clusterLinearIdx)
  {
    const ClusterDebugRecord rec0 = (clusterLinearIdx < first.clusters.size()) ? first.clusters[clusterLinearIdx] : ClusterDebugRecord{};
    const ClusterDebugRecord rec1 = (clusterLinearIdx < second.clusters.size()) ? second.clusters[clusterLinearIdx] : ClusterDebugRecord{};

    const auto reason0 = static_cast<ClusterDebugReason>(rec0.reason);
    const auto reason1 = static_cast<ClusterDebugReason>(rec1.reason);

    if (reason0 == ClusterDebugReason::NotVisited && reason1 == ClusterDebugReason::NotVisited)
      continue;

    ++visitedClusterCount;

    const uint32_t pageIndex = clusterLinearIdx / DEBUG_MAX_CLUSTERS_PER_PAGE;
    const uint32_t localClusterIndex = clusterLinearIdx % DEBUG_MAX_CLUSTERS_PER_PAGE;

    os::Logger::logf(
        "cluster(page=%u local=%u) | cap0=%s (selfErrPx=%.4f parentErrPx=%.4f node=%u) | cap1=%s (selfErrPx=%.4f parentErrPx=%.4f node=%u)",
        pageIndex,
        localClusterIndex,
        clusterDebugReasonToString(reason0),
        debugBitsToFloat(rec0.selfErrorPxBits),
        debugBitsToFloat(rec0.parentErrorPxBits),
        rec0.nodeIndex,
        clusterDebugReasonToString(reason1),
        debugBitsToFloat(rec1.selfErrorPxBits),
        debugBitsToFloat(rec1.parentErrorPxBits),
        rec1.nodeIndex);

    const bool rendered0 = isRenderedClusterReason(reason0);
    const bool rendered1 = isRenderedClusterReason(reason1);
    if (rendered0 && !rendered1)
    {
      ++renderedOnlyInFirst;
      os::Logger::logf("  diff: rendered in capture0, not rendered in capture1 (capture1 reason=%s)", clusterDebugReasonToString(reason1));
    }
    else if (!rendered0 && rendered1)
    {
      ++renderedOnlyInSecond;
      os::Logger::logf("  diff: rendered in capture1, not rendered in capture0 (capture0 reason=%s)", clusterDebugReasonToString(reason0));
    }
  }

  os::Logger::logf("Visited clusters in union: %u", visitedClusterCount);
  os::Logger::logf("Rendered only in capture0: %u", renderedOnlyInFirst);
  os::Logger::logf("Rendered only in capture1: %u", renderedOnlyInSecond);
  reportSelfErrorChildrenDiagnostics(first, second, buildData, clusterTopology, instancePageTableOffset);
  os::Logger::log("=====================================================================\n");
}

enum class CullingCaptureRequest : uint32_t
{
  None = 0u,
  CaptureBaseline,
  CompareAgainstBaseline,
};

struct FinalCullingReadbackBuffers
{
  Buffer counters;
  Buffer hierarchyDebug;
  Buffer clusterDebug;
  Buffer pageTable;
  Buffer visibleClusters;
};

struct ReadbackBufferSet
{
  Buffer handle;
  std::vector<rendering::BufferId> frameIds;
};

struct VSMReadbackBuffers
{
  ReadbackBufferSet allocatorCounters;
  ReadbackBufferSet drawCounters;
  ReadbackBufferSet virtualPageTable;
  ReadbackBufferSet pageClusterCounts;
  ReadbackBufferSet drawIndirect;
};

static CullingDebugSnapshot captureFinalCullingSnapshot(
    RenderGraph *renderGraph,
    const VirtualGeometryScene &scene,
    const VirtualGeometryCullingMultipleDispatchesPass &finalCullingPass,
    const FinalCullingReadbackBuffers &readbackBuffers,
    uint32_t captureIndex,
    uint64_t frameCounter,
    const math::Vec3f &cameraPosition,
    float nearPlane,
    float farPlane,
    float lodErrorThreshold,
    float projY,
    uint32_t viewportW,
    uint32_t viewportH,
    uint32_t hiZLevels)
{
  CullingDebugSnapshot snapshot{};
  snapshot.valid = true;
  snapshot.captureIndex = captureIndex;
  snapshot.frameCounter = frameCounter;
  snapshot.cameraPos[0] = cameraPosition[0];
  snapshot.cameraPos[1] = cameraPosition[1];
  snapshot.cameraPos[2] = cameraPosition[2];
  snapshot.nearPlane = nearPlane;
  snapshot.farPlane = farPlane;
  snapshot.lodErrorThreshold = lodErrorThreshold;
  snapshot.projY = projY;
  snapshot.viewportW = viewportW;
  snapshot.viewportH = viewportH;
  snapshot.hiZLevels = hiZLevels;

  snapshot.hierarchy.resize(finalCullingPass.getHierarchyDebugRecordCount());
  renderGraph->bufferRead(
      readbackBuffers.hierarchyDebug,
      0,
      snapshot.hierarchy.size() * sizeof(HierarchyDebugRecord),
      [&snapshot](const void *gpuData)
      {
        std::memcpy(snapshot.hierarchy.data(), gpuData, snapshot.hierarchy.size() * sizeof(HierarchyDebugRecord));
      });

  snapshot.clusters.resize(finalCullingPass.getClusterDebugRecordCount());
  renderGraph->bufferRead(
      readbackBuffers.clusterDebug,
      0,
      snapshot.clusters.size() * sizeof(ClusterDebugRecord),
      [&snapshot](const void *gpuData)
      {
        std::memcpy(snapshot.clusters.data(), gpuData, snapshot.clusters.size() * sizeof(ClusterDebugRecord));
      });

  snapshot.pageTable.resize(scene.pagesTableBufferSize / sizeof(PageTableEntry));
  renderGraph->bufferRead(
      readbackBuffers.pageTable,
      0,
      snapshot.pageTable.size() * sizeof(PageTableEntry),
      [&snapshot](const void *gpuData)
      {
        std::memcpy(snapshot.pageTable.data(), gpuData, snapshot.pageTable.size() * sizeof(PageTableEntry));
      });

  const CullingCounters counters = readCullingCounters(renderGraph, readbackBuffers.counters);
  const uint32_t visibleCount = counters.visibleClusterHardwareCount;
  std::vector<VisibleClusterInfo_CPU> visibleInfos(visibleCount);
  if (visibleCount > 0u)
  {
    renderGraph->bufferRead(
        readbackBuffers.visibleClusters,
        0,
        visibleCount * sizeof(VisibleClusterInfo_CPU),
        [&visibleInfos, visibleCount](const void *gpuData)
        {
          std::memcpy(visibleInfos.data(), gpuData, visibleCount * sizeof(VisibleClusterInfo_CPU));
        });
  }

  snapshot.visibleRenderedClusters.reserve(visibleCount);
  for (const VisibleClusterInfo_CPU &info : visibleInfos)
  {
    VisibleRenderedClusterState state{};
    state.key.pageIndex = info.pageIndex;
    state.key.localClusterIndex = info.pageLocalClusterIndex;
    state.key.instanceIndex = info.instanceIndex;
    state.nodeIndex = info._padding;

    const uint64_t linearIndex = debugClusterLinearIndex(info.pageIndex, info.pageLocalClusterIndex);
    if (linearIndex < snapshot.clusters.size())
    {
      const ClusterDebugRecord &record = snapshot.clusters[linearIndex];
      state.clusterReason = static_cast<ClusterDebugReason>(record.reason);
      state.selfErrorPx = debugBitsToFloat(record.selfErrorPxBits);
      state.parentErrorPx = debugBitsToFloat(record.parentErrorPxBits);
      if (record.nodeIndex != SENTINEL_VALUE)
      {
        state.nodeIndex = record.nodeIndex;
      }
    }
    state.hierarchyReason = gatherHierarchyReason(snapshot, state.nodeIndex);
    snapshot.visibleRenderedClusters.push_back(state);
  }

  return snapshot;
}

static void logVisibleRenderedClusters(const CullingDebugSnapshot &snapshot)
{
  os::Logger::log("\n=================== R CAPTURE BASELINE ===================");
  os::Logger::logf(
      "capture=%u frame=%llu renderedClusters=%zu cam=(%.3f, %.3f, %.3f)",
      snapshot.captureIndex,
      static_cast<unsigned long long>(snapshot.frameCounter),
      snapshot.visibleRenderedClusters.size(),
      snapshot.cameraPos[0],
      snapshot.cameraPos[1],
      snapshot.cameraPos[2]);

  for (size_t i = 0; i < snapshot.visibleRenderedClusters.size(); ++i)
  {
    const VisibleRenderedClusterState &state = snapshot.visibleRenderedClusters[i];
    os::Logger::logf(
        "visible[%zu] page=%u local=%u inst=%u node=%u cluster=%s nodeState=%s selfErrPx=%.4f parentErrPx=%.4f",
        i,
        state.key.pageIndex,
        state.key.localClusterIndex,
        state.key.instanceIndex,
        state.nodeIndex,
        clusterDebugReasonToString(state.clusterReason),
        hierarchyDebugReasonToString(state.hierarchyReason),
        state.selfErrorPx,
        state.parentErrorPx);
  }
  os::Logger::log("==========================================================\n");
}

static void compareBaselineVisibleClusters(const CullingDebugSnapshot &baseline, const CullingDebugSnapshot &current)
{
  std::unordered_map<ClusterVisibilityKey, VisibleRenderedClusterState, ClusterVisibilityKeyHash> currentVisible;
  currentVisible.reserve(current.visibleRenderedClusters.size());
  for (const VisibleRenderedClusterState &state : current.visibleRenderedClusters)
  {
    currentVisible[state.key] = state;
  }

  os::Logger::log("\n=================== R CAPTURE COMPARE ===================");
  os::Logger::logf(
      "baseline frame=%llu rendered=%zu | current frame=%llu rendered=%zu",
      static_cast<unsigned long long>(baseline.frameCounter),
      baseline.visibleRenderedClusters.size(),
      static_cast<unsigned long long>(current.frameCounter),
      current.visibleRenderedClusters.size());

  uint32_t stillVisibleCount = 0u;
  uint32_t lostCount = 0u;
  for (const VisibleRenderedClusterState &baselineState : baseline.visibleRenderedClusters)
  {
    if (currentVisible.find(baselineState.key) != currentVisible.end())
    {
      ++stillVisibleCount;
      continue;
    }

    ++lostCount;
    ClusterDebugReason currentClusterReason = ClusterDebugReason::NotVisited;
    float selfErrorPx = 0.0f;
    float parentErrorPx = 0.0f;
    const uint64_t linearIndex = debugClusterLinearIndex(baselineState.key.pageIndex, baselineState.key.localClusterIndex);
    if (linearIndex < current.clusters.size())
    {
      const ClusterDebugRecord &record = current.clusters[linearIndex];
      currentClusterReason = static_cast<ClusterDebugReason>(record.reason);
      selfErrorPx = debugBitsToFloat(record.selfErrorPxBits);
      parentErrorPx = debugBitsToFloat(record.parentErrorPxBits);
    }

    const HierarchyDebugReason currentHierarchyReason = gatherHierarchyReason(current, baselineState.nodeIndex);
    os::Logger::logf(
        "lost[%u] page=%u local=%u inst=%u node=%u | cluster=%s node=%s selfErrPx=%.4f parentErrPx=%.4f",
        lostCount - 1u,
        baselineState.key.pageIndex,
        baselineState.key.localClusterIndex,
        baselineState.key.instanceIndex,
        baselineState.nodeIndex,
        clusterDebugReasonToString(currentClusterReason),
        hierarchyDebugReasonToString(currentHierarchyReason),
        selfErrorPx,
        parentErrorPx);
  }

  os::Logger::logf("stillVisible=%u lost=%u", stillVisibleCount, lostCount);
  os::Logger::log("=========================================================\n");
}

#endif

#ifndef DEBUG_BINDINGS
struct ReadbackBufferSet
{
  Buffer handle;
  std::vector<rendering::BufferId> frameIds;
};

struct VSMReadbackBuffers
{
  ReadbackBufferSet allocatorCounters;
  ReadbackBufferSet drawCounters;
  ReadbackBufferSet virtualPageTable;
  ReadbackBufferSet pageClusterCounts;
  ReadbackBufferSet drawIndirect;
};
#endif

// ============================================================================
// Page word layout helpers  (must stay in sync with virtualgeometrydata.wgsl
// AND VirtualGeometryFile.cpp PageBuffer::encode)
//
// Page binary layout (from PageBuffer::encode):
//   Header: 7 words
//     [0] num_meshlets
//     [1] position_data_size   (words)
//     [2] normal_data_size     (words)
//     [3] uv_data_size         (words)
//     [4] index_data_size      (words, padded to uint32)
//     [5] bone_weight_data_size(words)
//     [6] dependency_count
//
//   Meshlet descriptor table: num_meshlets * MESHLET_DESC_WORDS words
//   Per-meshlet descriptor layout (33 words each):
//     [ 0] pos_word_off
//     [ 1] pos_words
//     [ 2] norm_off
//     [ 3] norm_count (vertex_count)
//     [ 4] uv_off
//     [ 5] uv_count   (vertex_count * 2)
//     [ 6] idx_word_off
//     [ 7] idx_words
//     [ 8] vertex_count
//     [ 9] triangle_count
//     [10] bits_per_vertex_position_channel_x
//     [11] bits_per_vertex_position_channel_y
//     [12] bits_per_vertex_position_channel_z
//     [13] vertex_position_quantization_factor
//     [14] min_vertex_position_channel_x  (as float bits)
//     [15] min_vertex_position_channel_y  (as float bits)
//     [16] min_vertex_position_channel_z  (as float bits)
//     [17] self.center[0]   (as float bits)
//     [18] self.center[1]   (as float bits)
//     [19] self.center[2]   (as float bits)
//     [20] self.radius      (as float bits)
//     [21] self.error       (as float bits)
//     [22] parent.center[0] (as float bits)
//     [23] parent.center[1] (as float bits)
//     [24] parent.center[2] (as float bits)
//     [25] parent.radius    (as float bits)
//     [26] parent.error     (as float bits)
//     [27] cone.axis[0]    (as float bits)
//     [28] cone.axis[1]    (as float bits)
//     [29] cone.axis[2]    (as float bits)
//     [30] cone.cutoff     (as float bits)
//     [31] boneWeightOffset
//     [32] boneWeightsPerVertex
// ============================================================================

static constexpr uint32_t MESHLET_DESC_WORDS = virtualgeometry::MESHLET_DESC_WORDS;
static constexpr uint32_t PAGE_HEADER_WORDS = virtualgeometry::PAGE_HEADER_WORDS;

// Descriptor field word offsets within each meshlet descriptor.
static constexpr uint32_t DESC_POS_WORD_OFF = 0u;
static constexpr uint32_t DESC_POS_WORDS = 1u;
static constexpr uint32_t DESC_NORM_OFF = 2u;
static constexpr uint32_t DESC_NORM_COUNT = 3u;
static constexpr uint32_t DESC_UV_OFF = 4u;
static constexpr uint32_t DESC_UV_COUNT = 5u;
static constexpr uint32_t DESC_IDX_WORD_OFF = 6u;
static constexpr uint32_t DESC_IDX_WORDS = 7u;
static constexpr uint32_t DESC_VERT_COUNT = 8u;
static constexpr uint32_t DESC_TRI_COUNT = 9u;
static constexpr uint32_t DESC_BITS_X = 10u;
static constexpr uint32_t DESC_BITS_Y = 11u;
static constexpr uint32_t DESC_BITS_Z = 12u;
static constexpr uint32_t DESC_QFACTOR = 13u;
static constexpr uint32_t DESC_MIN_X = 14u;
static constexpr uint32_t DESC_MIN_Y = 15u;
static constexpr uint32_t DESC_MIN_Z = 16u;
static constexpr uint32_t DESC_SELF_CX = 17u;
static constexpr uint32_t DESC_SELF_CY = 18u;
static constexpr uint32_t DESC_SELF_CZ = 19u;
static constexpr uint32_t DESC_SELF_R = 20u;
static constexpr uint32_t DESC_SELF_ERR = 21u;
static constexpr uint32_t DESC_PARENT_CX = 22u;
static constexpr uint32_t DESC_PARENT_CY = 23u;
static constexpr uint32_t DESC_PARENT_CZ = 24u;
static constexpr uint32_t DESC_PARENT_R = 25u;
static constexpr uint32_t DESC_PARENT_ERR = 26u;
static constexpr uint32_t DESC_CONE_AXIS_X = 27u;
static constexpr uint32_t DESC_CONE_AXIS_Y = 28u;
static constexpr uint32_t DESC_CONE_AXIS_Z = 29u;
static constexpr uint32_t DESC_CONE_CUTOFF = 30u;
static constexpr uint32_t DESC_BW_OFFSET = 31u;
static constexpr uint32_t DESC_BW_PER_VERT = 32u;

static uint32_t computeMipCount(uint32_t width, uint32_t height)
{
  uint32_t maxDim = std::max(width, height);
  uint32_t levels = 1u;
  while (maxDim > 1u)
  {
    maxDim >>= 1u;
    ++levels;
  }
  return levels;
}

static std::string formatClipMask(uint32_t mask, uint32_t clipCount)
{
  std::ostringstream oss;
  oss << "[";
  bool first = true;
  for (uint32_t clipIdx = 0u; clipIdx < clipCount; ++clipIdx)
  {
    if ((mask & (1u << clipIdx)) == 0u)
    {
      continue;
    }
    if (!first)
    {
      oss << ",";
    }
    first = false;
    oss << clipIdx;
  }
  oss << "]";
  return oss.str();
}

// ============================================================================
// CPU mirrors of GPU LOD helpers
// ============================================================================

static float cpuGetInstanceScale(const float m[16])
{
  return std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
}

static float cpuCalculateProjectedError(float cx, float cy, float cz, float radius, float errorLocal, const float modelMatrix[16], float vpX, float vpY, float vpZ, float nearPlane, float projY1, uint32_t viewportHeight)
{
  const float ws = cpuGetInstanceScale(modelMatrix);
  const float wcX = modelMatrix[0] * cx + modelMatrix[4] * cy + modelMatrix[8] * cz + modelMatrix[12];
  const float wcY = modelMatrix[1] * cx + modelMatrix[5] * cy + modelMatrix[9] * cz + modelMatrix[13];
  const float wcZ = modelMatrix[2] * cx + modelMatrix[6] * cy + modelMatrix[10] * cz + modelMatrix[14];
  const float dx = wcX - vpX, dy = wcY - vpY, dz = wcZ - vpZ;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float d = std::max(dist - radius * ws, nearPlane);
  return (errorLocal * ws / d) * projY1 * 0.5f * static_cast<float>(viewportHeight);
}

// Mirror of GPU shouldVisitChildNodes — tests only the coarser (parent) error.
// Returns true when the parent representation is not accurate enough and we
// must descend into children.
static bool cpuShouldVisitChildNodes(const VirtualGeometryHierarchy &node, const float modelMatrix[16], float vpX, float vpY, float vpZ, float nearPlane, float projY1, uint32_t viewportHeight, float errorThreshold)
{
  const float pe = cpuCalculateProjectedError(node.max_center_x, node.max_center_y, node.max_center_z, node.max_radius, node.max_parent_lod_error, modelMatrix, vpX, vpY, vpZ, nearPlane, projY1, viewportHeight);
  return pe >= errorThreshold;
}

// ============================================================================
// CPU page-data accessors (using corrected offsets)
// ============================================================================

static uint32_t cpuPageWordBase(const PageTableEntry &entry)
{
  return entry.bufferOffset / sizeof(uint32_t);
}

static uint32_t cpuReadDesc(const uint32_t *pagesWords, const PageTableEntry &entry, uint32_t localIdx, uint32_t fieldOff)
{
  return pagesWords[cpuPageWordBase(entry) + virtualgeometry::PAGE_HEADER_WORDS + localIdx * virtualgeometry::MESHLET_DESC_WORDS + fieldOff];
}

static uint32_t cpuReadTriangleCount(const uint32_t *pagesWords, const PageTableEntry &entry, uint32_t localIdx)
{
  return cpuReadDesc(pagesWords, entry, localIdx, DESC_TRI_COUNT);
}

// Mirror of GPU isClusterCoarseEnough — tests only the cluster's own (finer)
// error.  Returns true when the cluster is detailed enough to render.
static bool cpuIsClusterCoarseEnough(
    const uint32_t *pagesWords,
    const PageTableEntry &entry,
    uint32_t meshletLocalIdx,
    const float modelMatrix[16],
    float vpX,
    float vpY,
    float vpZ,
    float nearPlane,
    float projY1,
    uint32_t viewportHeight,
    float errorThreshold)
{
  auto asFloat = [](uint32_t v)
  {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };

  const float cx = asFloat(cpuReadDesc(pagesWords, entry, meshletLocalIdx, DESC_SELF_CX));
  const float cy = asFloat(cpuReadDesc(pagesWords, entry, meshletLocalIdx, DESC_SELF_CY));
  const float cz = asFloat(cpuReadDesc(pagesWords, entry, meshletLocalIdx, DESC_SELF_CZ));
  const float r = asFloat(cpuReadDesc(pagesWords, entry, meshletLocalIdx, DESC_SELF_R));
  const float err = asFloat(cpuReadDesc(pagesWords, entry, meshletLocalIdx, DESC_SELF_ERR));

  const float se = cpuCalculateProjectedError(cx, cy, cz, r, err, modelMatrix, vpX, vpY, vpZ, nearPlane, projY1, viewportHeight);
  return se < errorThreshold;
}

// ============================================================================
// CPU simulation — mirrors processHierarchyNodes + processClusters exactly
// ============================================================================

struct SimCluster
{
  uint32_t nodeIndex;
  uint32_t pageLocalClusterIndex;
  uint32_t globalPageIndex;
  uint32_t triangleCount;
  float selfError_px;
};

static uint32_t nodeClusterLocalBit(const VirtualGeometryHierarchy &node, uint32_t pageLocalClusterIndex)
{
  if (pageLocalClusterIndex < node.child_start)
    return UINT32_MAX;
  const uint32_t localBit = pageLocalClusterIndex - node.child_start;
  if (localBit >= node.child_count || localBit >= 8u)
    return UINT32_MAX;
  return localBit;
}

static bool hierarchyUsesClusterBitsets(const std::vector<VirtualGeometryHierarchy> &nodes)
{
  for (const auto &n : nodes)
  {
    if ((n.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    if ((n.flags & (HIERARCHY_STREAMING_MASK_BITS | HIERARCHY_ENABLED_MASK_BITS)) != 0u)
      return true;
  }
  return false;
}

static bool clusterEnabledForNode(const VirtualGeometryHierarchy &node, uint32_t pageLocalClusterIndex, bool useBitsetCulling)
{
  if (!useBitsetCulling)
    return true;

  const uint32_t localBit = nodeClusterLocalBit(node, pageLocalClusterIndex);
  if (localBit == UINT32_MAX)
    return false;

  const uint8_t enabledMask = static_cast<uint8_t>((node.flags & HIERARCHY_ENABLED_MASK_BITS) >> HIERARCHY_ENABLED_MASK_SHIFT);
  return ((enabledMask >> localBit) & 1u) != 0u;
}

static bool clusterStreamingForNode(const VirtualGeometryHierarchy &node, uint32_t pageLocalClusterIndex, bool useBitsetCulling)
{
  if (!useBitsetCulling)
    return (node.flags & STREAMING_LEAF_FLAG) != 0u;

  const uint32_t localBit = nodeClusterLocalBit(node, pageLocalClusterIndex);
  if (localBit == UINT32_MAX)
    return false;

  const uint8_t streamingMask = static_cast<uint8_t>((node.flags & HIERARCHY_STREAMING_MASK_BITS) >> HIERARCHY_STREAMING_MASK_SHIFT);
  return ((streamingMask >> localBit) & 1u) != 0u;
}

static bool nodeHasAnyStreamingCluster(const VirtualGeometryHierarchy &node, bool useBitsetCulling)
{
  if (!useBitsetCulling)
    return (node.flags & STREAMING_LEAF_FLAG) != 0u;

  const uint32_t bitCount = std::min(node.child_count, 8u);
  if (bitCount == 0u)
    return false;
  const uint32_t fullMask = (1u << bitCount) - 1u;
  const uint32_t streamingMask = (node.flags & HIERARCHY_STREAMING_MASK_BITS) >> HIERARCHY_STREAMING_MASK_SHIFT;
  return (streamingMask & fullMask) != 0u;
}

static std::vector<SimCluster> printCPUHierarchySimulation(
    const std::vector<VirtualGeometryHierarchy> &nodes,
    const std::vector<PageTableEntry> &pageTable,
    const std::vector<uint8_t> &pagesRaw,
    const rendering::Camera &cam,
    float errorThreshold,
    float nearPlane,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    uint32_t pageTableOffset,
    uint32_t hierarchyStartOffset,
    uint32_t frameCounter)
{
  // Identity column-major model matrix — single instance at origin.
  static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  const uint32_t *pagesWords = reinterpret_cast<const uint32_t *>(pagesRaw.data());

  const math::Vec3f &vp = cam.getPosition();
  const float vpX = vp[0], vpY = vp[1], vpZ = vp[2];
  const float p11 = cam.getProjectionMatrix().at(1, 1);
  const bool useBitsetCulling = hierarchyUsesClusterBitsets(nodes);

  os::Logger::logf(
      "\n======================================================"
      "\n CPU Hierarchy + Cluster Simulation  frame=%u"
      "\n  errorThreshold=%.4f  near=%.4f  viewport=%ux%u"
      "\n  camPos=(%.3f, %.3f, %.3f)  proj[1][1]=%.4f"
      "\n======================================================",
      frameCounter,
      errorThreshold,
      nearPlane,
      viewportWidth,
      viewportHeight,
      vpX,
      vpY,
      vpZ,
      p11);

  // --------------------------------------------------------------------------
  // PASS 1 — BFS over hierarchy nodes
  //   Mirrors processHierarchyNodes: call cpuShouldVisitChildNodes on every
  //   node.  Inner nodes whose coarser representation is not accurate enough
  //   have their children enqueued.  Leaf nodes that pass are collected into
  //   leafWork for cluster processing.
  // --------------------------------------------------------------------------

  struct LeafWork
  {
    uint32_t nodeIndex;
    uint32_t globalPageIndex;
    uint32_t clusterStart; // page-local first cluster index
    uint32_t clusterCount;
  };

  std::vector<LeafWork> leafWork;

  uint32_t totalNodesVisited = 0u;
  uint32_t totalNodesDescended = 0u;
  uint32_t totalLeavesSkipped = 0u;

  char line[640];

  os::Logger::log("\n--- Hierarchy traversal (mirrors processHierarchyNodes) ---");
  os::Logger::log("  idx   type   pe(px)   descend?  installed?  streamLeaf  page  child_start  child_cnt  lod[min..parent]");
  os::Logger::log("  ---   -----  -------  --------  ----------  ----------  ----  -----------  ---------  ----------------");

  std::vector<uint32_t> queue;
  queue.push_back(hierarchyStartOffset);

  while (!queue.empty())
  {
    std::vector<uint32_t> next;
    for (uint32_t ni : queue)
    {
      if (ni >= static_cast<uint32_t>(nodes.size()))
        continue;

      const VirtualGeometryHierarchy &n = nodes[ni];
      ++totalNodesVisited;

      const bool isLeaf = (n.flags & 1u) != 0u;
      const bool notInst = (n.pageIndex & (1u << 31)) != 0u;
      const bool isStreamingLeaf = nodeHasAnyStreamingCluster(n, useBitsetCulling);

      // GPU logic: shouldVisitChildNodes checks only the parent/coarser error.
      const bool shouldDescend = cpuShouldVisitChildNodes(n, kIdentity, vpX, vpY, vpZ, nearPlane, p11, viewportHeight, errorThreshold);

      const float pe = cpuCalculateProjectedError(n.max_center_x, n.max_center_y, n.max_center_z, n.max_radius, n.max_parent_lod_error, kIdentity, vpX, vpY, vpZ, nearPlane, p11, viewportHeight);

      std::snprintf(
          line,
          sizeof(line),
          "  [%3u] %-5s  %7.3f  %-8s  %-10s  %-10s  %4u  %11s  %9u  [%.4f..%.4f]",
          ni,
          isLeaf ? "LEAF" : "INNER",
          pe,
          shouldDescend ? "YES" : "no",
          notInst ? "not inst" : "OK",
          isStreamingLeaf ? "YES" : "no",
          n.pageIndex & ~(1u << 31),
          n.child_start == UINT32_MAX ? "NONE" : std::to_string(n.child_start).c_str(),
          n.child_count,
          std::abs(n.min_lod_error),
          (n.max_parent_lod_error == std::numeric_limits<float>::max()) ? -1.0f : n.max_parent_lod_error);
      os::Logger::log(line);

      if (!shouldDescend)
        continue;

      ++totalNodesDescended;

      if (isLeaf)
      {
        // Leaf: validate page is installed, then record cluster range.
        if (notInst || n.child_start == UINT32_MAX)
        {
          ++totalLeavesSkipped;
          os::Logger::logf("    ^ skipped (notInst=%s  child_start=%s)", notInst ? "SET" : "clear", n.child_start == UINT32_MAX ? "NONE" : std::to_string(n.child_start).c_str());
          continue;
        }

        const uint32_t rawPage = n.pageIndex & ~(1u << 31);
        const uint32_t globalPage = pageTableOffset + rawPage;

        if (globalPage >= static_cast<uint32_t>(pageTable.size()) || pageTable[globalPage].isInstalled == 0u)
        {
          ++totalLeavesSkipped;
          os::Logger::logf("    ^ skipped (page %u not installed in page table)", globalPage);
          continue;
        }

        leafWork.push_back({ni, globalPage, n.child_start, n.child_count});
        os::Logger::logf("    ^ queued %u clusters [%u..%u) on page %u", n.child_count, n.child_start, n.child_start + n.child_count, globalPage);
      }
      else
      {
        // Inner: enqueue children at absolute hierarchy indices.
        if (n.child_start != UINT32_MAX)
          for (uint32_t ci = 0; ci < n.child_count; ++ci)
            next.push_back(hierarchyStartOffset + n.child_start + ci);
      }
    }
    queue = std::move(next);
  }

  os::Logger::logf(
      "\n  Nodes visited        : %u / %zu"
      "\n  Nodes descended      : %u"
      "\n  Leaves skipped       : %u"
      "\n  Leaf batches queued  : %zu",
      totalNodesVisited,
      nodes.size(),
      totalNodesDescended,
      totalLeavesSkipped,
      leafWork.size());

  if (pagesRaw.empty())
  {
    os::Logger::log(
        "\n  (pages buffer not yet available — skipping cluster simulation)"
        "\n======================================================\n");
    return {};
  }

  // --------------------------------------------------------------------------
  // PASS 2 — cluster culling
  //   Mirrors processClusters: for each cluster in every selected leaf run
  //   cpuIsClusterCoarseEnough (self-error < threshold).
  // --------------------------------------------------------------------------

  os::Logger::log("\n--- Cluster culling (mirrors processClusters) ---");
  os::Logger::log("  node  globalPage  localIdx  tris  selfErr(px)  coarse?");
  os::Logger::log("  ----  ----------  --------  ----  -----------  -------");

  struct NodeSummary
  {
    uint32_t nodeIndex;
    std::vector<uint32_t> passingLocalIndices;
    uint32_t totalTris = 0u;
    uint32_t totalTested = 0u;
  };

  std::vector<SimCluster> visible;
  std::vector<NodeSummary> nodeSummaries;

  auto findSummary = [&](uint32_t ni) -> NodeSummary &
  {
    for (auto &s : nodeSummaries)
      if (s.nodeIndex == ni)
        return s;
    nodeSummaries.push_back({ni, {}, 0u, 0u});
    return nodeSummaries.back();
  };

  for (const LeafWork &lw : leafWork)
  {
    const PageTableEntry &entry = pageTable[lw.globalPageIndex];
    NodeSummary &ns = findSummary(lw.nodeIndex);

    for (uint32_t ci = 0; ci < lw.clusterCount; ++ci)
    {
      const uint32_t localIdx = lw.clusterStart + ci;
      const uint32_t triCount = cpuReadTriangleCount(pagesWords, entry, localIdx);
      if (triCount == 0u)
        continue;

      ++ns.totalTested;

      auto asFloat = [](uint32_t v)
      {
        float f;
        std::memcpy(&f, &v, 4);
        return f;
      };
      if (!clusterEnabledForNode(nodes[lw.nodeIndex], localIdx, useBitsetCulling))
        continue;
      const bool isStreamingLeaf = clusterStreamingForNode(nodes[lw.nodeIndex], localIdx, useBitsetCulling);
      const float rawSelfError = asFloat(cpuReadDesc(pagesWords, entry, localIdx, DESC_SELF_ERR));
      const float effectiveSelfError = isStreamingLeaf ? 0.0f : rawSelfError;

      const float selfErrPx = cpuCalculateProjectedError(
          asFloat(cpuReadDesc(pagesWords, entry, localIdx, DESC_SELF_CX)),
          asFloat(cpuReadDesc(pagesWords, entry, localIdx, DESC_SELF_CY)),
          asFloat(cpuReadDesc(pagesWords, entry, localIdx, DESC_SELF_CZ)),
          asFloat(cpuReadDesc(pagesWords, entry, localIdx, DESC_SELF_R)),
          effectiveSelfError,
          kIdentity,
          vpX,
          vpY,
          vpZ,
          nearPlane,
          p11,
          viewportHeight);

      const bool coarse = (selfErrPx < errorThreshold);

      std::snprintf(
          line,
          sizeof(line),
          "  [%3u]  %10u  %8u  %4u  %11.4f  %s  (streamLeaf=%s rawErr=%.6f effErr=%.6f)",
          lw.nodeIndex,
          lw.globalPageIndex,
          localIdx,
          triCount,
          selfErrPx,
          coarse ? "YES" : "no",
          isStreamingLeaf ? "YES" : "no",
          rawSelfError,
          effectiveSelfError);
      os::Logger::log(line);

      if (!coarse)
        continue;

      visible.push_back({lw.nodeIndex, localIdx, lw.globalPageIndex, triCount, selfErrPx});
      ns.passingLocalIndices.push_back(localIdx);
      ns.totalTris += triCount;
    }
  }

  // --------------------------------------------------------------------------
  // PASS 3 — per-node summary of picked clusters
  // --------------------------------------------------------------------------

  os::Logger::log("\n--- Per-node cluster pick summary ---");
  os::Logger::log(
      "  nodeIdx  tested  picked  totalTris  page  lod[min..parent]"
      "  aabb[(min)-(max)]");
  os::Logger::log(
      "  -------  ------  ------  ---------  ----  ----------------"
      "  -----------------");

  uint64_t grandTotalTris = 0u;
  for (const NodeSummary &ns : nodeSummaries)
  {
    const VirtualGeometryHierarchy &n = nodes[ns.nodeIndex];
    grandTotalTris += ns.totalTris;

    std::snprintf(
        line,
        sizeof(line),
        "  [%3u]    %4u    %4zu    %9u  %4u  [%.4f..%.4f]"
        "  [(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)]",
        ns.nodeIndex,
        ns.totalTested,
        ns.passingLocalIndices.size(),
        ns.totalTris,
        n.pageIndex & ~(1u << 31),
        std::abs(n.min_lod_error),
        (n.max_parent_lod_error == std::numeric_limits<float>::max()) ? -1.0f : n.max_parent_lod_error,
        n.min_x,
        n.min_y,
        n.min_z,
        n.max_x,
        n.max_y,
        n.max_z);
    os::Logger::log(line);

    if (!ns.passingLocalIndices.empty())
    {
      std::string clStr = "           clusters: [";
      for (size_t k = 0; k < ns.passingLocalIndices.size(); ++k)
      {
        clStr += std::to_string(ns.passingLocalIndices[k]);
        if (k + 1 < ns.passingLocalIndices.size())
          clStr += ", ";
      }
      clStr += "]";
      os::Logger::log(clStr.c_str());
    }
  }

  os::Logger::logf(
      "\n  Total visible clusters : %zu"
      "\n  Total triangles        : %llu"
      "\n  Unique nodes picked    : %zu"
      "\n======================================================\n",
      visible.size(),
      static_cast<unsigned long long>(grandTotalTris),
      nodeSummaries.size());

  return visible;
}

static void printGPUVisibleClusterSummary(const std::vector<VirtualGeometryHierarchy> &nodes, const std::vector<VisibleClusterInfo_CPU> &gpuVisible, uint32_t gpuVisibleCount)
{
  std::unordered_map<uint32_t, uint32_t> nodePickCount;
  std::unordered_map<uint32_t, uint32_t> nodeStreamingPickCount;
  nodePickCount.reserve(gpuVisibleCount);
  nodeStreamingPickCount.reserve(gpuVisibleCount);
  const bool useBitsetCulling = hierarchyUsesClusterBitsets(nodes);

  for (uint32_t i = 0; i < gpuVisibleCount; ++i)
  {
    ++nodePickCount[gpuVisible[i]._padding];
    const uint32_t nodeIdx = gpuVisible[i]._padding;
    if (nodeIdx < nodes.size() && clusterStreamingForNode(nodes[nodeIdx], gpuVisible[i].pageLocalClusterIndex, useBitsetCulling))
      ++nodeStreamingPickCount[nodeIdx];
  }

  os::Logger::log("\n--- GPU visible cluster summary (from HW visible buffer) ---");
  os::Logger::log("  nodeIdx  picks  streamPicks  baseLeaf  installed  rawPage");
  os::Logger::log("  -------  -----  -----------  --------  ---------  -------");

  for (const auto &it : nodePickCount)
  {
    const uint32_t nodeIndex = it.first;
    const uint32_t picks = it.second;
    if (nodeIndex >= nodes.size())
      continue;
    const auto &n = nodes[nodeIndex];
    const uint32_t streamPicks = nodeStreamingPickCount[nodeIndex];
    const bool baseLeaf = (n.flags & 1u) != 0u;
    const bool installed = (n.pageIndex & PAGE_NOT_INSTALLED_BIT) == 0u;
    const uint32_t rawPage = n.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    os::Logger::logf("  [%3u]    %4u      %4u/%-4u    %-3s      %-3s      %5u", nodeIndex, picks, streamPicks, picks, baseLeaf ? "YES" : "no", installed ? "YES" : "no", rawPage);
  }

  os::Logger::logf("  total GPU visible clusters: %u", gpuVisibleCount);
}

static void printCPUGPUDiff(const std::vector<SimCluster> &cpuVisible, const std::vector<VisibleClusterInfo_CPU> &gpuVisible, uint32_t gpuVisibleCount, const std::vector<VirtualGeometryHierarchy> &nodes)
{
  struct ClusterKey
  {
    uint32_t nodeIndex;
    uint32_t globalPageIndex;
    uint32_t pageLocalClusterIndex;

    bool operator==(const ClusterKey &o) const
    {
      return nodeIndex == o.nodeIndex && globalPageIndex == o.globalPageIndex && pageLocalClusterIndex == o.pageLocalClusterIndex;
    }
  };

  struct ClusterKeyHash
  {
    size_t operator()(const ClusterKey &k) const
    {
      size_t h = static_cast<size_t>(k.nodeIndex);
      h ^= static_cast<size_t>(k.globalPageIndex) + 0x9e3779b9u + (h << 6) + (h >> 2);
      h ^= static_cast<size_t>(k.pageLocalClusterIndex) + 0x9e3779b9u + (h << 6) + (h >> 2);
      return h;
    }
  };

  std::unordered_set<ClusterKey, ClusterKeyHash> cpuSet;
  cpuSet.reserve(cpuVisible.size());
  for (const auto &c : cpuVisible)
    cpuSet.insert(ClusterKey{c.nodeIndex, c.globalPageIndex, c.pageLocalClusterIndex});

  std::unordered_set<ClusterKey, ClusterKeyHash> gpuSet;
  gpuSet.reserve(gpuVisibleCount);
  for (uint32_t i = 0; i < gpuVisibleCount; ++i)
    gpuSet.insert(ClusterKey{gpuVisible[i]._padding, gpuVisible[i].pageIndex, gpuVisible[i].pageLocalClusterIndex});

  uint32_t cpuOnly = 0u;
  uint32_t gpuOnly = 0u;
  constexpr uint32_t kMaxPrintedDiff = 32u;
  const bool useBitsetCulling = hierarchyUsesClusterBitsets(nodes);

  os::Logger::log("\n--- CPU/GPU visible cluster diff ---");
  for (const auto &k : cpuSet)
  {
    if (gpuSet.find(k) != gpuSet.end())
      continue;
    ++cpuOnly;
    if (cpuOnly <= kMaxPrintedDiff)
    {
      const bool streamLeaf = (k.nodeIndex < nodes.size()) ? clusterStreamingForNode(nodes[k.nodeIndex], k.pageLocalClusterIndex, useBitsetCulling) : false;
      os::Logger::logf("  CPU_ONLY node=%u page=%u localCluster=%u streamLeaf=%s", k.nodeIndex, k.globalPageIndex, k.pageLocalClusterIndex, streamLeaf ? "YES" : "no");
    }
  }

  for (const auto &k : gpuSet)
  {
    if (cpuSet.find(k) != cpuSet.end())
      continue;
    ++gpuOnly;
    if (gpuOnly <= kMaxPrintedDiff)
    {
      const bool streamLeaf = (k.nodeIndex < nodes.size()) ? clusterStreamingForNode(nodes[k.nodeIndex], k.pageLocalClusterIndex, useBitsetCulling) : false;
      os::Logger::logf("  GPU_ONLY node=%u page=%u localCluster=%u streamLeaf=%s", k.nodeIndex, k.globalPageIndex, k.pageLocalClusterIndex, streamLeaf ? "YES" : "no");
    }
  }

  os::Logger::logf("  CPU visible=%zu  GPU visible=%u  CPU_ONLY=%u  GPU_ONLY=%u", cpuVisible.size(), gpuVisibleCount, cpuOnly, gpuOnly);
}

struct RendererAssetPaths
{
  fs::path assetPath;
  fs::path virtualGeometryPath;
  std::string objectName;
  bool isGLTF = false;
};

struct RendererLaunchOptions
{
  RendererAssetPaths assetPaths;
  std::string shadowFilter = "pcf";
  bool enableVulkanValidationLayers = false;
};

struct PreparedRendererAsset
{
  VirtualGeometryEncodedData encoded;
  std::vector<fs::path> animationPaths;
  std::string defaultAnimationName;
};

static std::string toLower(std::string value)
{
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
  return value;
}

static std::string trim(std::string value)
{
  auto isSpace = [](unsigned char c)
  {
    return std::isspace(c) != 0;
  };

  value.erase(
      value.begin(),
      std::find_if(
          value.begin(),
          value.end(),
          [&](unsigned char c)
          {
            return !isSpace(c);
          }));
  value.erase(
      std::find_if(
          value.rbegin(),
          value.rend(),
          [&](unsigned char c)
          {
            return !isSpace(c);
          })
          .base(),
      value.end());
  return value;
}

static std::string sanitizeFilename(std::string value)
{
  for (char &c : value)
  {
    const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!isAlphaNum && c != '_' && c != '-')
      c = '_';
  }

  while (!value.empty() && value.front() == '_')
    value.erase(value.begin());
  while (!value.empty() && value.back() == '_')
    value.pop_back();

  return value.empty() ? "asset" : value;
}

static bool isGLTFAssetPath(const fs::path &path)
{
  const std::string extension = toLower(path.extension().string());
  return extension == ".gltf" || extension == ".glb";
}

static bool isOBJAssetPath(const fs::path &path)
{
  return toLower(path.extension().string()) == ".obj";
}

static bool objAssetReferencesMaterialLibrary(const fs::path &path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return false;

  std::string line;
  while (std::getline(file, line))
  {
    const std::string trimmed = trim(line);
    if (trimmed.rfind("mtllib", 0u) == 0u)
      return true;
  }

  return false;
}

static void printUsage(const char *exeName)
{
  os::Logger::logf(
      "Usage: %s [--asset <path-relative-to-assets>] [--shadow-filter pcf] [--vulkan-validations-layers]\n"
      "  example: %s --asset meshes/gltf/CesiumMan.gltf --shadow-filter pcf --vulkan-validations-layers",
      exeName,
      exeName);
}

static RendererLaunchOptions resolveRendererLaunchOptions(int argc, char **argv)
{
  const fs::path executableDir = utils::getExecutableDirectory();
  const fs::path assetRoot = executableDir / "assets";

  RendererLaunchOptions options;
  std::string assetArgument = "meshes/gltf/CesiumMan.gltf";
  bool sawPositional = false;

  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h")
    {
      printUsage(argv[0]);
      std::exit(0);
    }

    if (argument == "--asset")
    {
      if (index + 1 >= argc)
        throw std::runtime_error("--asset requires a value");
      assetArgument = argv[++index];
      sawPositional = true;
      continue;
    }

    if (argument == "--shadow-filter")
    {
      if (index + 1 >= argc)
        throw std::runtime_error("--shadow-filter requires a value");
      options.shadowFilter = toLower(argv[++index]);
      if (options.shadowFilter != "pcf")
        throw std::runtime_error("--shadow-filter only supports 'pcf'");
      continue;
    }

    if (argument == "--vulkan-validations-layers")
    {
      options.enableVulkanValidationLayers = true;
      continue;
    }

    if (!argument.empty() && argument[0] == '-')
      throw std::runtime_error("Unknown option: " + argument);

    if (sawPositional)
      throw std::runtime_error("Only one asset path may be provided");
    assetArgument = argument;
    sawPositional = true;
  }

  fs::path assetPath = fs::path(assetArgument);
  if (!assetPath.is_absolute())
  {
    if (assetPath.begin() != assetPath.end() && *assetPath.begin() == "assets")
      assetPath = executableDir / assetPath;
    else
      assetPath = assetRoot / assetPath;
  }

  if (!fs::exists(assetPath))
    throw std::runtime_error("Asset file not found: " + assetPath.string());

  if (!isOBJAssetPath(assetPath) && !isGLTFAssetPath(assetPath))
    throw std::runtime_error("Unsupported asset extension: " + assetPath.extension().string());

  RendererAssetPaths paths;
  paths.assetPath = assetPath;
  paths.virtualGeometryPath = assetPath;
  paths.virtualGeometryPath += ".virtualgeometry";
  paths.objectName = sanitizeFilename(assetPath.stem().string());
  paths.isGLTF = isGLTFAssetPath(assetPath);
  options.assetPaths = paths;
  return options;
}

static std::vector<fs::path> ensureAnimationFiles(const fs::path &assetPath, const fs::path &outputDirectory, const rendering::animation::Skeleton &skeleton)
{
  std::vector<fs::path> animationPaths;
  if (!isGLTFAssetPath(assetPath) || skeleton.empty())
    return animationPaths;

  std::vector<rendering::animation::AnimationFile> animations;
  if (!rendering::animation::AnimationFile::createFromGLTF(assetPath.string(), skeleton, animations))
    throw std::runtime_error("Failed to extract animations from glTF: " + assetPath.string());

  animationPaths.reserve(animations.size());
  const std::string assetStem = sanitizeFilename(assetPath.stem().string());
  for (size_t animationIndex = 0u; animationIndex < animations.size(); ++animationIndex)
  {
    const std::string animationStem = sanitizeFilename(animations[animationIndex].name.empty() ? ("animation_" + std::to_string(animationIndex)) : animations[animationIndex].name);
    const fs::path animationPath = outputDirectory / (assetStem + "." + animationStem + ".animation");
    if (!fs::exists(animationPath) && !animations[animationIndex].save(animationPath.string()))
      throw std::runtime_error("Failed to write animation file: " + animationPath.string());
    animationPaths.push_back(animationPath);
  }

  return animationPaths;
}

static VirtualGeometryEncodedData loadOrBuildVirtualGeometryAsset(const fs::path &assetPath, const fs::path &virtualGeometryPath, const QuantizationConfig &quantizationConfig)
{
  VirtualGeometryEncodedData encoded;
  if (fs::exists(virtualGeometryPath))
  {
    os::Logger::logf("[1] Loading cached virtual geometry: %s", virtualGeometryPath.string().c_str());
    VirtualGeometryFile reader(virtualGeometryPath.string(), /*write=*/false);
    if (!reader.isOpen())
      throw std::runtime_error("Cannot open cached VG file for reading: " + virtualGeometryPath.string());
    if (reader.readAll(encoded))
    {
      const bool missingObjMaterials = isOBJAssetPath(assetPath) && objAssetReferencesMaterialLibrary(assetPath) && encoded.materialFiles.empty();
      if (!missingObjMaterials)
        return encoded;

      os::Logger::logf("[1] Cached OBJ virtual geometry has no generated materials, rebuilding: %s", virtualGeometryPath.string().c_str());
    }

    os::Logger::logf("[1] Cached virtual geometry is incompatible, rebuilding: %s", virtualGeometryPath.string().c_str());
  }

  os::Logger::logf("[1] Encoding asset: %s", assetPath.string().c_str());

  VirtualGeometryBuildData build;
  if (isOBJAssetPath(assetPath))
  {
    const fs::path materialOutputDirectory = virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + "_materials");
    build = VirtualGeometryEncoder::buildFromOBJFile(assetPath.string(), {}, materialOutputDirectory.string());
  }
  else if (isGLTFAssetPath(assetPath))
  {
    build = VirtualGeometryBuilder::buildFromGLTFFile(assetPath.string());
  }
  else
  {
    throw std::runtime_error("Unsupported asset type: " + assetPath.string());
  }

  encoded = VirtualGeometryCompressor::encode(build, quantizationConfig);

  VirtualGeometryFile writer(virtualGeometryPath.string(), /*write=*/true);
  if (!writer.isOpen())
    throw std::runtime_error("Cannot open VG file for writing: " + virtualGeometryPath.string());
  if (!writer.write(encoded, build.pages, MESHLET_LZ4))
    throw std::runtime_error("Failed to write VG file: " + virtualGeometryPath.string());

  return encoded;
}

static PreparedRendererAsset prepareRendererAsset(const RendererAssetPaths &paths, const QuantizationConfig &quantizationConfig)
{
  PreparedRendererAsset prepared;
  prepared.encoded = loadOrBuildVirtualGeometryAsset(paths.assetPath, paths.virtualGeometryPath, quantizationConfig);

  if (paths.isGLTF)
  {
    prepared.animationPaths = ensureAnimationFiles(paths.assetPath, paths.virtualGeometryPath.parent_path(), prepared.encoded.skeleton);
    if (!prepared.animationPaths.empty())
    {
      rendering::animation::AnimationFile defaultAnimation;
      if (!rendering::animation::AnimationFile::load(prepared.animationPaths.front().string(), defaultAnimation))
        throw std::runtime_error("Failed to load animation file: " + prepared.animationPaths.front().string());
      prepared.defaultAnimationName = defaultAnimation.name;
    }
  }

  return prepared;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char **argv)
{
  os::Logger::start(100);

  // --------------------------------------------------------------------------
  // [1] Resolve asset and build/load virtual geometry cache
  // --------------------------------------------------------------------------
  const RendererLaunchOptions launchOptions = resolveRendererLaunchOptions(argc, argv);
  const RendererAssetPaths &assetPaths = launchOptions.assetPaths;

  QuantizationConfig qcfg;
  qcfg.quantization_factor = 4;
  qcfg.unit_scale = 100.0f;

  const PreparedRendererAsset preparedAsset = prepareRendererAsset(assetPaths, qcfg);
  const VirtualGeometryEncodedData &encoded = preparedAsset.encoded;

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf(
      "[asset] source=%s vg=%s pages=%u hierarchy=%u animations=%zu",
      assetPaths.assetPath.string().c_str(),
      assetPaths.virtualGeometryPath.string().c_str(),
      pageCount,
      hierarchySize,
      preparedAsset.animationPaths.size());

  // --------------------------------------------------------------------------
  // [2] Window + RHI
  // --------------------------------------------------------------------------
  os::Logger::log("[2] Creating window and Vulkan RHI...");

  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Virtual Geometry", 1920, 1080);
  auto *nativeWindow = static_cast<window::sdl3::SDL3Window *>(window);
  WindowTitleStats windowTitleStats{"Virtual Geometry"};

  DeviceRequiredLimits limits = {0, 0, 0};
  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle | DeviceFeatures::DeviceFeatures_Timestamp;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {}, launchOptions.enableVulkanValidationLayers);
  os::Logger::logf("[Vulkan] validation layers: %s", launchOptions.enableVulkanValidationLayers ? "enabled" : "disabled");

  {
    auto surfaces = std::vector<VkSurfaceKHR>();
    surfaces.push_back(window->getVulkanSurface(rhi->getInstance()));
    rhi->init(surfaces);
  }

  SwapChain swapChain = rhi->createSwapChain(0, window->getWidth(), window->getHeight());
  const uint32_t maxFramesInFlight = std::max(1u, rhi->getSwapChainImagesCount(swapChain));
  RenderGraph *renderGraph = new RenderGraph(
      rhi,
      RenderGraph::Settings{
          .maxTimers = 1024u,
          .maxFramesInFlight = maxFramesInFlight,
      });
  renderGraph->addSwapChainImages(swapChain);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO &imguiIo = ImGui::GetIO();
  imguiIo.IniFilename = nullptr;
  imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  if (!ImGui_ImplSDL3_InitForVulkan(nativeWindow->sdlWindow))
  {
    throw std::runtime_error("Failed to initialize Dear ImGui SDL3 backend");
  }

  const VkRenderPass imguiRenderPass = createImGuiOverlayRenderPass(rhi->getDevice(), rhi->getSwapChainVkFormat(swapChain));
  ImGui_ImplVulkan_InitInfo imguiInitInfo{};
  imguiInitInfo.ApiVersion = VK_API_VERSION_1_2;
  imguiInitInfo.Instance = rhi->getInstance();
  imguiInitInfo.PhysicalDevice = rhi->getPhysicalDevice();
  imguiInitInfo.Device = rhi->getDevice();
  imguiInitInfo.QueueFamily = rhi->getGraphicsQueueFamilyIndex();
  imguiInitInfo.Queue = rhi->getGraphicsQueue();
  imguiInitInfo.DescriptorPoolSize = 64u;
  imguiInitInfo.MinImageCount = std::max(2u, rhi->getSwapChainImagesCount(swapChain));
  imguiInitInfo.ImageCount = std::max(2u, rhi->getSwapChainImagesCount(swapChain));
  imguiInitInfo.PipelineInfoMain.RenderPass = imguiRenderPass;
  imguiInitInfo.PipelineInfoMain.Subpass = 0u;
  imguiInitInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  imguiInitInfo.CheckVkResultFn = checkImGuiVkResult;
  if (!ImGui_ImplVulkan_Init(&imguiInitInfo))
  {
    throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend");
  }
  std::unordered_map<uintptr_t, VkFramebuffer> imguiFramebuffers;

  const uint32_t kVW = rhi->getSwapChainImagesWidth(swapChain);
  const uint32_t kVH = rhi->getSwapChainImagesHeight(swapChain);

  // --------------------------------------------------------------------------
  // [3] Scene
  // --------------------------------------------------------------------------
  os::Logger::log("[3] Building VirtualGeometryScene...\n");

  const uint32_t kHierarchyBufBytes = std::max(hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
  const uint64_t kPagesBufBytes = 256ull * 1024 * 1024;
  os::print("[3] Building VirtualGeometryScene...\n");
  VirtualGeometryScene scene(renderGraph, kHierarchyBufBytes, kPagesBufBytes);
  os::Logger::log("[3] Register object\n");
  scene.registerObjectForStreaming(assetPaths.objectName, assetPaths.virtualGeometryPath.string());

  // --------------------------------------------------------------------------
  // [4] Camera
  // --------------------------------------------------------------------------
  os::Logger::log("[4] Configuring camera...\n");
  os::print("[4] Configuring camera...");
  constexpr bool kReverseZ = true;
  constexpr float kClearDepth = 0.0f;
  constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
  const float kAspect = static_cast<float>(kVW) / static_cast<float>(kVH);
  constexpr float kNear = 0.1f;
  constexpr float kFar = 10000.0f;
  const uint32_t kHiZWidth = kVW;
  const uint32_t kHiZHeight = kVH;
  const uint32_t kHiZLvls = computeMipCount(kHiZWidth, kHiZHeight);

  constexpr float kMinError = 1.0f;
  float prepassLodErrorThreshold = kMinError;
  float geometryLodErrorThreshold = kMinError;
  float vsmLodErrorThreshold = kMinError;

  math::Vec3f objectCentre(0.0f, 0.0f, 0.0f);
  float kRadius = 1.0f;
  if (!encoded.hierarchy.empty())
  {
    math::Vec3f aabbMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    math::Vec3f aabbMax(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    for (const auto &node : encoded.hierarchy)
    {
      aabbMin[0] = std::min(aabbMin[0], node.min_x);
      aabbMin[1] = std::min(aabbMin[1], node.min_y);
      aabbMin[2] = std::min(aabbMin[2], node.min_z);
      aabbMax[0] = std::max(aabbMax[0], node.max_x);
      aabbMax[1] = std::max(aabbMax[1], node.max_y);
      aabbMax[2] = std::max(aabbMax[2], node.max_z);
    }
    objectCentre = (aabbMin + aabbMax) * 0.5f;
    kRadius = std::max((aabbMax - aabbMin).length() * 0.5f, 0.1f);
  }
  const float halfFovY = kFovY * 0.5f;
  const float halfFovX = std::atan(std::tan(halfFovY) * kAspect);
  const float limitingHalfFov = std::min(halfFovX, halfFovY);
  constexpr float kFitPadding = 1.08f;
  const float kCamDist = (kRadius / std::tan(limitingHalfFov)) * kFitPadding;
  const math::Vec3f camPos(objectCentre[0], objectCentre[1], objectCentre[2] + kCamDist);

  rendering::Camera cam(kFovY, camPos, math::Vec3f(0.0f, 0.0f, -1.0f), kReverseZ);
  cam.setAspectRatio(kAspect);
  cam.setNearFar(kNear, kFar);
  cam.updateMatrices();

  // --------------------------------------------------------------------------
  // [4c] Camera animation + input state
  // --------------------------------------------------------------------------
  const float kCamSpeed = kRadius / 1.0f;
  const float kObjectRotateSpeed = 1.5f; // rad/s
  const float kMouseLookSensitivity = 0.0035f;
  const float kMaxLookDotY = 0.995f;
  const math::Vec3f kWorldUp(0.0f, 1.0f, 0.0f);

  math::Vec3f cameraPosition = camPos;
  math::Quatf cameraRotation = math::Quatf::identity();
  float objectPitch = 0.0f;
  float objectYaw = 0.0f;
  math::Quatf objectRotation = math::Quatf::identity();
  const math::Vec3f objectPosition(0.0f, 0.0f, 0.0f);
  math::Vec3f lightDirection = math::Vec3f(-0.35f, -1.0f, -0.25f).normalize();
  uint32_t shadowFilterTaps = 4u;
  float shadowBias = 0.0015f;
  float slopeScaleBias = 1.0f;
  float maxShadowBias = 0.006f;
  float pcfRadiusTexels = 1.5f;
  bool vsmDirtyPageStencilEnabled = true;
  bool shadowPcfEnabled = false;
  float normalBiasTexels = 1.0f;
  std::array<float, 3> shadowAmbientColor = {0.18f, 0.20f, 0.24f};
  bool screenSpaceShadowEnabled = true;
  float screenSpaceSurfaceThickness = 0.005f;
  float screenSpaceBilinearThreshold = 0.04f;
  float screenSpaceShadowContrast = 2.5f;
  float screenSpaceRayDistance = 60.0f;
  std::array<float, 2> screenSpaceDepthBounds = {0.0f, 1.0f};
  bool screenSpaceIgnoreEdgePixels = true;
  bool screenSpaceUsePrecisionOffset = true;
  bool screenSpaceBilinearSamplingOffsetMode = false;
  bool screenSpaceUseEarlyOut = true;
  bool screenSpaceTreatSkippedEdgeSamplesAsLit = true;
  bool screenSpaceDebugOutputEdgeMask = false;
  bool screenSpaceDebugOutputThreadIndex = false;
  bool screenSpaceDebugOutputWaveIndex = false;
  constexpr uint32_t kVSMPageResolution = 128;
  constexpr float kVSMPageWorldScale = 2.0f;
  constexpr uint32_t kVSMMaxDirtyPagesPerFrame = 1024u;
  constexpr uint32_t kVSMMaxFallbackPagesPerFrame = 64u;
  constexpr bool kEnableOptionalVsmTableOutput = false;
  VirtualShadowMapManager vsmManager(
      renderGraph,
      VirtualShadowMapManager::Settings{
          .virtualPageTableResolution = 32u,
          .physicalAtlasResolution = 16384u,
          .physicalPageSize = kVSMPageResolution,
          .cascadeCount = 4u,
          .maxDirectionalLights = 1u,
          .maxAllocationRequestsPerFrame = kVSMMaxDirtyPagesPerFrame,
          .maxFutureAllocationRequests = kVSMMaxFallbackPagesPerFrame,
          .firstCascadeWorldExtent = std::max(kRadius, 64.0f),
          .cascadeWorldExtentScale = 2.0f,
          .pageWorldScale = kVSMPageWorldScale,
          .lightDistance = std::max(kRadius, 64.0f),
          .reverseZ = false,
      });
  const VirtualShadowMapManager::LightId vsmLightId = vsmManager.createDirectionalLight(lightDirection, math::Vec3f(1.0f, 1.0f, 1.0f));
  scene.setStreamingResidencyInvalidationCallback(
      [&](const AABB &bounds)
      {
        vsmManager.queueInvalidateRegion(bounds);
      });

  os::Logger::log(
      "[controls] Middle-drag / Secondary-drag = camera look   Up/Down = camera forward/backward   "
      "WASD = object rotate   L = align directional light to camera");

  SDL_SetWindowRelativeMouseMode(nativeWindow->sdlWindow, false);
  SDL_ShowCursor();
  os::print("[5]");

  InstanceId instId = scene.instantiateObjectInstance(assetPaths.objectName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
  scene.updateInstanceBuffer(true);

  rendering::animation::AnimationPlayer animationPlayer(encoded.skeleton);
  bool hasActiveAnimation = false;
  std::string activeAnimationName;
  float animationTimeSeconds = 0.0f;
  for (const fs::path &animationPath : preparedAsset.animationPaths)
  {
    if (!animationPlayer.loadAnimation(animationPath.string()))
      throw std::runtime_error("Failed to load animation file: " + animationPath.string());
  }
  if (!preparedAsset.defaultAnimationName.empty())
  {
    hasActiveAnimation = scene.applyAnimationFrame(instId, animationPlayer, preparedAsset.defaultAnimationName, 0.0f, true);
    activeAnimationName = preparedAsset.defaultAnimationName;
    if (hasActiveAnimation)
      os::Logger::logf("[animation] playing default clip: %s", activeAnimationName.c_str());
  }

  // --------------------------------------------------------------------------
  // [5] Render targets
  // --------------------------------------------------------------------------
  os::Logger::log("[5] Creating render targets...");
  os::print("[5] Creating render targets...\n");
  Texture depthTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "DepthTexture",
        .format = Format::Format_Depth32Float,
        .height = kVH,
        .width = kVW,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
      });

  auto makeDepthTex = [&](const char *name) -> Texture
  {
    return renderGraph->createTexture(
        TextureInfo{
          .name = name,
          .format = Format::Format_Depth32Float,
          .height = kVH,
          .width = kVW,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
        });
  };

  TextureView depthView{
    .texture = depthTexture,
    .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
    .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    .index = 0,
    .flags = ImageAspectFlags::Depth,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };

  auto makeColorTex = [&](const char *name, Format fmt) -> Texture
  {
    return renderGraph->createTexture(
        TextureInfo{
          .name = name,
          .format = fmt,
          .height = kVH,
          .width = kVW,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
        });
  };

  auto makeDebugStorageTex = [&](const char *name, Format fmt) -> Texture
  {
    return renderGraph->createTexture(
        TextureInfo{
          .name = name,
          .format = fmt,
          .height = kVH,
          .width = kVW,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
        });
  };

  auto makeSizedDebugStorageTex = [&](const char *name, Format fmt, uint32_t width, uint32_t height) -> Texture
  {
    return renderGraph->createTexture(
        TextureInfo{
          .name = name,
          .format = fmt,
          .height = height,
          .width = width,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        });
  };

  Texture colorTexture = makeColorTex("MaterialColorTexture", Format::Format_RGBA16Float);
  Texture packedGeometryIdsLoTexture = makeColorTex("PackedGeometryIdsLoTexture", Format::Format_R32Uint);
  Texture packedGeometryIdsHiTexture = makeColorTex("PackedGeometryIdsHiTexture", Format::Format_R32Uint);
  Texture materialIdTexture = makeColorTex("MaterialIdTexture", Format::Format_R32Uint);
  Texture materialUVTexture = makeColorTex("MaterialUVTexture", Format::Format_RG32Float);

  auto makeColorView = [](Texture tex) -> TextureView
  {
    return TextureView{
      .texture = tex,
      .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ResourceLayout::COLOR_ATTACHMENT,
      .index = 0,
      .flags = ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  };

  auto makeDepthView = [](Texture tex) -> TextureView
  {
    return TextureView{
      .texture = tex,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
      .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0,
      .flags = ImageAspectFlags::Depth,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  };

  Texture swapChainTexture = renderGraph->createTexture(
      TextureInfo{
        .isVirtual = true,
        .name = "ColorAttatchmentTexture",
        .height = kVH,
        .width = kVW,
        .format = rhi->getSwapChainFormat(swapChain),
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_None,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_ColorAttachment,
      });

  Texture previousDepthHZBTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "PreviousDepthHZBTexture",
        .height = kHiZHeight,
        .width = kHiZWidth,
        .format = Format::Format_R32Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_None,
        .mipLevels = kHiZLvls,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
      });

#ifdef COLLECT_FRAME_STATISTICS
  Texture hardwareFrameStatisticsHeatmapTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "HardwareFrameStatisticsHeatmapTexture",
        .height = kVH,
        .width = kVW,
        .format = Format::Format_RGBA16Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_ColorAttachment,
      });
#endif

  Texture virtualShadowMapPagesDebugTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "VirtualShadowMapPagesDebugTexture",
        .height = kVH,
        .width = kVW,
        .format = Format::Format_RGBA16Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        .frameLocal = maxFramesInFlight > 1u,
      });
  Texture virtualShadowMapTableDebugTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "VirtualShadowMapTableDebugTexture",
        .height = kVH,
        .width = kVW,
        .format = Format::Format_RGBA16Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        .frameLocal = maxFramesInFlight > 1u,
      });
  constexpr uint32_t kVSMClipmapStateDebugSize = 512u;
  Texture virtualShadowMapTableClip0DebugTexture =
      makeSizedDebugStorageTex("VirtualShadowMapTableClip0DebugTexture", Format::Format_RGBA16Float, kVSMClipmapStateDebugSize, kVSMClipmapStateDebugSize);
  Texture virtualShadowMapTableClip1DebugTexture =
      makeSizedDebugStorageTex("VirtualShadowMapTableClip1DebugTexture", Format::Format_RGBA16Float, kVSMClipmapStateDebugSize, kVSMClipmapStateDebugSize);
  Texture virtualShadowMapScreenSpaceShadowTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "VirtualShadowMapScreenSpaceShadowTexture",
        .height = kVH,
        .width = kVW,
        .format = Format::Format_R32Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        .frameLocal = maxFramesInFlight > 1u,
      });
  Texture virtualShadowMapShadowLightingTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "VirtualShadowMapShadowLightingTexture",
        .height = kVH,
        .width = kVW,
        .format = Format::Format_RGBA16Float,
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        .frameLocal = maxFramesInFlight > 1u,
      });
  auto makePerFrameOverrideTextureIds = [&](
                                          const std::string &baseName,
                                          Format format,
                                          BufferUsage memoryProperties,
                                          ImageUsage usage,
                                          uint32_t width,
                                          uint32_t height,
                                          uint32_t mipLevels,
                                          uint32_t arrayLayers = 1u)
  {
    std::vector<TextureId> ids(maxFramesInFlight, TextureId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
    {
      ids[frameSlot] = rhi->createTexture(
          TextureInfo{
              .name = baseName + ".override_frame" + std::to_string(frameSlot),
              .format = format,
              .memoryProperties = memoryProperties,
              .usage = usage,
              .width = width,
              .height = height,
              .depth = 1u,
              .arrayLayers = arrayLayers,
              .mipLevels = mipLevels,
          });
    }
    return ids;
  };
  const std::vector<TextureId> perFrameDepthTextureIds = makePerFrameOverrideTextureIds(
      depthTexture.name,
      Format::Format_Depth32Float,
      BufferUsage::BufferUsage_None,
      ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
      kVW,
      kVH,
      1u);
  const std::vector<TextureId> perFrameColorTextureIds = makePerFrameOverrideTextureIds(
      colorTexture.name,
      Format::Format_RGBA16Float,
      BufferUsage::BufferUsage_None,
      ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      kVW,
      kVH,
      1u);
  const std::vector<TextureId> perFrameMaterialIdTextureIds = makePerFrameOverrideTextureIds(
      materialIdTexture.name,
      Format::Format_R32Uint,
      BufferUsage::BufferUsage_None,
      ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      kVW,
      kVH,
      1u);
  const std::vector<TextureId> perFrameMaterialUVTextureIds = makePerFrameOverrideTextureIds(
      materialUVTexture.name,
      Format::Format_RG32Float,
      BufferUsage::BufferUsage_None,
      ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      kVW,
      kVH,
      1u);
  const std::vector<TextureId> perFramePreviousDepthHZBTextureIds = makePerFrameOverrideTextureIds(
      previousDepthHZBTexture.name,
      Format::Format_R32Float,
      rendering::BufferUsage::BufferUsage_None,
      rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
      kHiZWidth,
      kHiZHeight,
      kHiZLvls);
  const std::vector<TextureId> perFrameVsmPagesDebugTextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapPagesDebugTexture.name,
          Format::Format_RGBA16Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVW,
          kVH,
          1u);
  const std::vector<TextureId> perFrameVsmTableDebugTextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapTableDebugTexture.name,
          Format::Format_RGBA16Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVW,
          kVH,
          1u);
  const std::vector<TextureId> perFrameVsmShadowLightingTextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapShadowLightingTexture.name,
          Format::Format_RGBA16Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVW,
          kVH,
          1u);
  const std::vector<TextureId> perFrameVsmScreenSpaceShadowTextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapScreenSpaceShadowTexture.name,
          Format::Format_R32Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVW,
          kVH,
          1u);
  const std::vector<TextureId> perFrameVsmTableClip0TextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapTableClip0DebugTexture.name,
          Format::Format_RGBA16Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVSMClipmapStateDebugSize,
          kVSMClipmapStateDebugSize,
          1u);
  const std::vector<TextureId> perFrameVsmTableClip1TextureIds =
      makePerFrameOverrideTextureIds(
          virtualShadowMapTableClip1DebugTexture.name,
          Format::Format_RGBA16Float,
          rendering::BufferUsage::BufferUsage_Storage,
          rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          kVSMClipmapStateDebugSize,
          kVSMClipmapStateDebugSize,
          1u);
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
    .depthView = depthView,
    .depthTexture = depthTexture,
    .packedGeometryIdsLoView = makeColorView(packedGeometryIdsLoTexture),
    .packedGeometryIdsHiView = makeColorView(packedGeometryIdsHiTexture),
    .materialIdView = makeColorView(materialIdTexture),
    .materialUVView = makeColorView(materialUVTexture),
    .colorView = makeColorView(colorTexture),
    .colorTexture = colorTexture,
  };
  VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget{
    .depthView = depthView,
    .depthTexture = depthTexture,
  };
  // --------------------------------------------------------------------------
  // [6] Passes
  // --------------------------------------------------------------------------
  os::Logger::log("[6] Registering passes...");

  VirtualGeometryCullingMultipleDispatchesPass::Settings prepassCullingSettings;
  prepassCullingSettings.maxHierarchyLevels = 8;
  prepassCullingSettings.clustersQueueBufferSize = 1024 * 1024 * 16;
  prepassCullingSettings.errorTreshhold = prepassLodErrorThreshold;
  prepassCullingSettings.viewPortWidth = kVW;
  prepassCullingSettings.viewPortHeight = kVH;
  prepassCullingSettings.hiZMipLevels = kHiZLvls;
  prepassCullingSettings.useExternalHiZTexture = true;
  prepassCullingSettings.enableFrustumCulling = true;
  prepassCullingSettings.enableOcclusionCulling = true;

  VirtualGeometryCullingMultipleDispatchesPass::Settings cullingSettings;
  cullingSettings.maxHierarchyLevels = 8;
  cullingSettings.clustersQueueBufferSize = 1024 * 1024 * 16;
  cullingSettings.errorTreshhold = geometryLodErrorThreshold;
  cullingSettings.viewPortWidth = kVW;
  cullingSettings.viewPortHeight = kVH;
  cullingSettings.hiZMipLevels = kHiZLvls;
  cullingSettings.useExternalHiZTexture = true;
  cullingSettings.enableFrustumCulling = true;
  cullingSettings.enableOcclusionCulling = true;

  VirtualGeometryHardwareDrawPass::Settings hwSettings;
  hwSettings.viewPortWidth = kVW;
  hwSettings.viewPortHeight = kVH;
  hwSettings.colorFormat = rhi->getSwapChainFormat(swapChain);
  hwSettings.depthFormat = Format::Format_Depth32Float;
  hwSettings.maxDrawnClusters = 256;

  VirtualGeometryDepthPrePassDrawPass::Settings depthPrePassSettings;
  depthPrePassSettings.viewPortWidth = kVW;
  depthPrePassSettings.viewPortHeight = kVH;
  depthPrePassSettings.maxDrawnClusters = 256;
  depthPrePassSettings.depthFormat = Format::Format_Depth32Float;

  VirtualGeometryDepthPyramidPass::Settings depthPyramidSettings;
  depthPyramidSettings.width = kVW;
  depthPyramidSettings.height = kVH;
  depthPyramidSettings.outputWidth = kHiZWidth;
  depthPyramidSettings.outputHeight = kHiZHeight;
  depthPyramidSettings.mipCount = kHiZLvls;
  depthPyramidSettings.outputFormat = Format::Format_R32Float;

  VirtualGeometryRendererPass::Settings rendererSettings;
  rendererSettings.prepassCullingSettings = prepassCullingSettings;
  rendererSettings.depthPrePassSettings = depthPrePassSettings;
  rendererSettings.depthPyramidSettings = depthPyramidSettings;
  rendererSettings.finalCullingSettings = cullingSettings;
  rendererSettings.hardwareDrawSettings = hwSettings;
  rendererSettings.materialPassSettings.viewPortWidth = kVW;
  rendererSettings.materialPassSettings.viewPortHeight = kVH;
  rendererSettings.materialPassSettings.colorFormat = Format::Format_RGBA16Float;
  rendererSettings.materialPassSettings.depthFormat = Format::Format_Depth32Float;
  rendererSettings.registerMaterialPass = false;

  auto *rendererPass =
      renderGraph->registerPass<VirtualGeometryRendererPass>("virtualGeometryRendererPass", 0, scene, frameTarget, prePassTarget, virtualShadowMapShadowLightingTexture, previousDepthHZBTexture, kClearDepth, rendererSettings);
  VirtualShadowMapPass::Settings vsmPassSettings;
  vsmPassSettings.bookkeepingSettings.depthTextureWidth = kVW;
  vsmPassSettings.bookkeepingSettings.depthTextureHeight = kVH;
  vsmPassSettings.enableTableDebugPass = kEnableOptionalVsmTableOutput;
  vsmPassSettings.tableDebugSettings.width = kVW;
  vsmPassSettings.tableDebugSettings.height = kVH;
  auto configureShadowSettings = [&](auto &settings)
  {
    settings.shadowBias = shadowBias;
    settings.slopeScaleBias = slopeScaleBias;
    settings.maxShadowBias = maxShadowBias;
    settings.shadowFilterTaps = shadowPcfEnabled ? shadowFilterTaps : 1u;
    settings.pcfRadiusTexels = pcfRadiusTexels;
    settings.normalBiasTexels = normalBiasTexels;
    settings.ambientShadowColor = shadowAmbientColor;
    settings.contactShadowSamples = 0u;
    settings.contactShadowDistance = 0.0f;
    settings.contactShadowThickness = 0.0f;
    settings.contactShadowIntensity = 0.0f;
    settings.contactShadowStartBias = 0.0f;
    settings.screenSpaceShadowEnabled = screenSpaceShadowEnabled;
  };
  auto configureScreenSpaceShadowSettings = [&](auto &settings)
  {
    settings.width = kVW;
    settings.height = kVH;
    settings.surfaceThickness = screenSpaceSurfaceThickness;
    settings.bilinearThreshold = screenSpaceBilinearThreshold;
    settings.shadowContrast = screenSpaceShadowContrast;
    settings.rayDistance = screenSpaceRayDistance;
    settings.depthBounds = screenSpaceDepthBounds;
    settings.ignoreEdgePixels = screenSpaceIgnoreEdgePixels;
    settings.usePrecisionOffset = screenSpaceUsePrecisionOffset;
    settings.bilinearSamplingOffsetMode = screenSpaceBilinearSamplingOffsetMode;
    settings.useEarlyOut = screenSpaceUseEarlyOut;
    settings.treatSkippedEdgeSamplesAsLit = screenSpaceTreatSkippedEdgeSamplesAsLit;
    settings.debugOutputEdgeMask = screenSpaceDebugOutputEdgeMask;
    settings.debugOutputThreadIndex = screenSpaceDebugOutputThreadIndex;
    settings.debugOutputWaveIndex = screenSpaceDebugOutputWaveIndex;
  };
  configureScreenSpaceShadowSettings(vsmPassSettings.screenSpaceShadowSettings);
  vsmPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
  vsmPassSettings.drawSettings.enableDirtyPageStencil = vsmDirtyPageStencilEnabled;
  vsmPassSettings.shadowPcfSettings.width = kVW;
  vsmPassSettings.shadowPcfSettings.height = kVH;
  configureShadowSettings(vsmPassSettings.shadowPcfSettings);
  auto *vsmPass = renderGraph->registerPass<VirtualShadowMapPass>(
      "virtualShadowMapPass",
      31,
      scene,
      vsmManager,
      rendererPass->getSceneDepthTexture(),
      virtualShadowMapPagesDebugTexture,
      virtualShadowMapTableDebugTexture,
      virtualShadowMapScreenSpaceShadowTexture,
      virtualShadowMapShadowLightingTexture,
      vsmPassSettings);
  // The VSM cache (atlas, page tables, allocator state) is still shared across frames.
  // Serialize the next frame's first cache mutation against every previous-frame pass
  // that can still read or write that shared cache.
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "virtualShadowMapPass.draw");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "virtualShadowMapPass.tableDebug");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "virtualShadowMapPass.shadowLighting");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "virtualShadowMapTableClip0Pass");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "virtualShadowMapTableClip1Pass");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "copyVSMAllocatorCountersReadback");
  renderGraph->addInterFrameDependency("virtualShadowMapPass.bookkeeping", "copyVSMVirtualPageTableReadback");
  // The main virtual-geometry renderer still relies on pass-local scratch resources
  // that are conceptually frame-local. Serialize the next frame's renderer kickoff
  // behind the previous frame's material pass so the depth/culling chain feeding VSM
  // cannot overlap across frames while those scratch allocations remain shared.
  renderGraph->addInterFrameDependency("virtualGeometryRendererPass.preCulling", "virtualGeometryMaterialPass");
  vsmPass->getBookkeepingPass()->setDebugOutputEnabled(false);
  if (auto *pass = vsmPass->getTableDebugPass())
    pass->setEnabled(false);
  if (auto *pass = vsmPass->getScreenSpaceShadowPass())
    pass->setEnabled(screenSpaceShadowEnabled);
  uint32_t selectedDebugTargetSlot = SelectedDebugTargetPass::TargetSlot_BaseColor;
  auto applyShadowFilterSettings = [&]()
  {
    configureShadowSettings(vsmPassSettings.shadowPcfSettings);
    configureScreenSpaceShadowSettings(vsmPassSettings.screenSpaceShadowSettings);
    vsmPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
    vsmPassSettings.drawSettings.enableDirtyPageStencil = vsmDirtyPageStencilEnabled;
    if (auto *pass = vsmPass->getShadowPcfPass())
    {
      pass->setSettings(vsmPassSettings.shadowPcfSettings);
      pass->setEnabled(true);
    }
    if (auto *pass = vsmPass->getDrawPass())
    {
      pass->setLodErrorThreshold(vsmLodErrorThreshold);
      pass->setDirtyPageStencilEnabled(vsmDirtyPageStencilEnabled);
    }
    if (auto *pass = vsmPass->getScreenSpaceShadowPass())
    {
      pass->setSettings(vsmPassSettings.screenSpaceShadowSettings);
      pass->setEnabled(screenSpaceShadowEnabled);
    }
  };
  auto logShadowFilterSettings = [&]()
  {
    os::Logger::logf(
        "[Shadow] lod prepass=%.2f geometry=%.2f vsm=%.2f | stencil=%s | filter=%s enabled=%s taps=%u radius=%.2f | sss=%s thickness=%.4f bilinear=%.3f contrast=%.2f ray=%.0f edgeSkip=%s skippedEdgesLit=%s offsetMode=%s",
        prepassLodErrorThreshold,
        geometryLodErrorThreshold,
        vsmLodErrorThreshold,
        vsmDirtyPageStencilEnabled ? "on" : "off",
        shadowPcfEnabled ? "PCF" : "Hard",
        shadowPcfEnabled ? "yes" : "no",
        shadowFilterTaps,
        pcfRadiusTexels,
        screenSpaceShadowEnabled ? "yes" : "no",
        screenSpaceSurfaceThickness,
        screenSpaceBilinearThreshold,
        screenSpaceShadowContrast,
        screenSpaceRayDistance,
        screenSpaceIgnoreEdgePixels ? "yes" : "no",
        screenSpaceTreatSkippedEdgeSamplesAsLit ? "yes" : "no",
        screenSpaceBilinearSamplingOffsetMode ? "alt" : "default");
  };
  applyShadowFilterSettings();
  logShadowFilterSettings();
  os::Logger::log("[Shadow] controls: overlay sliders tune VSM bias/PCF plus screen-space shadow thickness/ray distance/edge handling/debug, F2 toggles PCF, -=/+=tap count, [/] = PCF radius, 1-0=debug views");
  if (kEnableOptionalVsmTableOutput)
  {
    auto *vsmTableClip0Pass = renderGraph->registerPass<virtualgeometry::gpgpu::VirtualShadowMapTableDebugPass>(
        "virtualShadowMapTableClip0Pass",
        32,
        vsmManager,
        rendererPass->getSceneDepthTexture(),
        virtualShadowMapTableClip0DebugTexture,
        virtualgeometry::gpgpu::VirtualShadowMapTableDebugPass::Settings{
            .width = kVSMClipmapStateDebugSize,
            .height = kVSMClipmapStateDebugSize,
            .debugLayer = 0,
        });
    auto *vsmTableClip1Pass = renderGraph->registerPass<virtualgeometry::gpgpu::VirtualShadowMapTableDebugPass>(
        "virtualShadowMapTableClip1Pass",
        33,
        vsmManager,
        rendererPass->getSceneDepthTexture(),
        virtualShadowMapTableClip1DebugTexture,
        virtualgeometry::gpgpu::VirtualShadowMapTableDebugPass::Settings{
            .width = kVSMClipmapStateDebugSize,
            .height = kVSMClipmapStateDebugSize,
            .debugLayer = 1,
        });
    vsmTableClip0Pass->setEnabled(true);
    vsmTableClip1Pass->setEnabled(true);
  }

  auto *preCullingPass = rendererPass->getPreCullingPass();
  auto *depthPrePass = rendererPass->getDepthPrePass();
  auto *finalCullingPass = rendererPass->getFinalCullingPass();
  auto *hwDrawPass = rendererPass->getHardwareDrawPass();
  auto *materialPass = renderGraph->registerPass<VirtualGeometryMaterialPass>(
      "virtualGeometryMaterialPass",
      39,
      scene,
      frameTarget,
      virtualShadowMapShadowLightingTexture,
      rendererSettings.materialPassSettings);
  auto *vsmDrawPass = vsmPass->getDrawPass();

  auto makeReadbackBuffer = [&](const std::string &name, uint64_t size) -> ReadbackBufferSet
  {
    ReadbackBufferSet bufferSet{};
    bufferSet.handle = renderGraph->createBuffer(
        BufferInfo{
          .name = name,
          .size = size,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        });
    bufferSet.frameIds.resize(maxFramesInFlight, BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
    {
      bufferSet.frameIds[frameSlot] = rhi->createBuffer(
          BufferInfo{
            .name = name + ".frame" + std::to_string(frameSlot),
            .size = size,
            .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
          });
    }
    return bufferSet;
  };

  const uint64_t vsmAllocatorCountersBytes = vsmManager.getAllocatorCountersBufferSize();
  const uint64_t vsmDrawCountersBytes = 8u * sizeof(uint32_t);
  const uint64_t vsmVirtualPageTableBytes = vsmManager.getVirtualPageTableBufferSize();
  const uint32_t activeVSMPageCount =
      vsmManager.getVirtualPageTableResolution() * vsmManager.getVirtualPageTableResolution() * std::max(1u, vsmManager.getActiveLayerCount());
  const uint64_t vsmPageClusterCountsBytes = static_cast<uint64_t>(activeVSMPageCount) * sizeof(uint32_t);
  const uint64_t vsmDrawIndirectBytes = static_cast<uint64_t>(vsmDrawPass->getVisibleClusterDrawCapacity()) * 4u * sizeof(uint32_t);
  VSMReadbackBuffers vsmReadbackBuffers{
    .allocatorCounters = makeReadbackBuffer("VSMAllocatorCounters.readback", vsmAllocatorCountersBytes),
    .drawCounters = makeReadbackBuffer("VSMDrawCounters.readback", vsmDrawCountersBytes),
    .virtualPageTable = makeReadbackBuffer("VSMVirtualPageTable.readback", vsmVirtualPageTableBytes),
    .pageClusterCounts = makeReadbackBuffer("VSMPageClusterCounts.readback", vsmPageClusterCountsBytes),
    .drawIndirect = makeReadbackBuffer("VSMDrawIndirect.readback", vsmDrawIndirectBytes),
  };

  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyVSMAllocatorCountersReadback",
      34,
      vsmManager.getAllocatorCountersBuffer(),
      0,
      vsmAllocatorCountersBytes,
      vsmReadbackBuffers.allocatorCounters.handle,
      0,
      vsmAllocatorCountersBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyVSMDrawCountersReadback",
      35,
      vsmDrawPass->getCullingStatisticsBuffer(),
      0,
      vsmDrawCountersBytes,
      vsmReadbackBuffers.drawCounters.handle,
      0,
      vsmDrawCountersBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyVSMVirtualPageTableReadback",
      36,
      vsmManager.getVirtualPageTableBuffer(),
      0,
      vsmVirtualPageTableBytes,
      vsmReadbackBuffers.virtualPageTable.handle,
      0,
      vsmVirtualPageTableBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyVSMPageClusterCountsReadback",
      37,
      vsmDrawPass->getPageClusterCountsBuffer(),
      0,
      vsmPageClusterCountsBytes,
      vsmReadbackBuffers.pageClusterCounts.handle,
      0,
      vsmPageClusterCountsBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyVSMDrawIndirectReadback",
      38,
      vsmDrawPass->getShadowDrawIndirectBuffer(),
      0,
      vsmDrawIndirectBytes,
      vsmReadbackBuffers.drawIndirect.handle,
      0,
      vsmDrawIndirectBytes);

#ifdef DEBUG_BINDINGS
  FinalCullingReadbackBuffers finalCullingReadbackBuffers{
    .counters = makeReadbackBuffer("FinalCullingCounters.readback", sizeof(CullingCounters)),
    .hierarchyDebug = makeReadbackBuffer("FinalCullingHierarchyDebug.readback", static_cast<uint64_t>(finalCullingPass->getHierarchyDebugRecordCount()) * sizeof(HierarchyDebugRecord)),
    .clusterDebug = makeReadbackBuffer("FinalCullingClusterDebug.readback", static_cast<uint64_t>(finalCullingPass->getClusterDebugRecordCount()) * sizeof(ClusterDebugRecord)),
    .pageTable = makeReadbackBuffer("FinalCullingPageTable.readback", scene.pagesTableBufferSize),
    .visibleClusters = makeReadbackBuffer("FinalCullingVisibleClusters.readback", static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo_CPU)),
  };

  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyFinalCullingCountersReadback", 18, finalCullingPass->getCullingStatisticsBuffer(), 0, sizeof(CullingCounters), finalCullingReadbackBuffers.counters, 0, sizeof(CullingCounters));
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyFinalCullingHierarchyDebugReadback",
      19,
      finalCullingPass->getHierarchyDebugBuffer(),
      0,
      static_cast<uint64_t>(finalCullingPass->getHierarchyDebugRecordCount()) * sizeof(HierarchyDebugRecord),
      finalCullingReadbackBuffers.hierarchyDebug,
      0,
      static_cast<uint64_t>(finalCullingPass->getHierarchyDebugRecordCount()) * sizeof(HierarchyDebugRecord));
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyFinalCullingClusterDebugReadback",
      20,
      finalCullingPass->getClusterDebugBuffer(),
      0,
      static_cast<uint64_t>(finalCullingPass->getClusterDebugRecordCount()) * sizeof(ClusterDebugRecord),
      finalCullingReadbackBuffers.clusterDebug,
      0,
      static_cast<uint64_t>(finalCullingPass->getClusterDebugRecordCount()) * sizeof(ClusterDebugRecord));
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyFinalCullingPageTableReadback", 21, scene.pageTableBuffer, 0, scene.pagesTableBufferSize, finalCullingReadbackBuffers.pageTable, 0, scene.pagesTableBufferSize);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
      "copyFinalCullingVisibleClustersReadback",
      22,
      finalCullingPass->getHWVisibleClusterInfosBuffer(),
      0,
      static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo_CPU),
      finalCullingReadbackBuffers.visibleClusters,
      0,
      static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo_CPU));
#endif

#ifdef COLLECT_FRAME_STATISTICS
  renderGraph->registerPass<rendering::passes::FrameStatisticsHeatmapPass>(
      "hardwareFrameStatisticsHeatmapPass",
      30,
      hwDrawPass->getFrameStatisticsBuffer(),
      hardwareFrameStatisticsHeatmapTexture,
      rendering::passes::FrameStatisticsHeatmapPass::Settings{
        .width = kVW,
        .height = kVH,
      });
#endif

  // --------------------------------------------------------------------------
  // [7] Swapchain clear pass + selected debug target view
  // --------------------------------------------------------------------------
  rendering::passes::ColorQuadPass::Settings clearSettings;
  clearSettings.viewPortWidth = kVW;
  clearSettings.viewPortHeight = kVH;
  clearSettings.region = Rect2D(0, 0, kVW, kVH);
  clearSettings.color = Color::rgba(0, 0, 0, 1);
  renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearSwapchain", 40, swapChainTexture, rhi->getSwapChainFormat(swapChain), clearSettings);

  auto makeFallbackColorTexture = [&](const std::string &name) -> Texture
  {
    return renderGraph->createTexture(
        TextureInfo{
          .name = name,
          .width = 1u,
          .height = 1u,
          .depth = 1u,
          .mipLevels = 1u,
          .format = Format::Format_RGBA16Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Sampled,
        });
  };

  const Texture fallbackColorTexture = makeFallbackColorTexture("SelectedDebugTargetFallbackColor");

  struct DisplayTargetDef
  {
    const char *name;
    uint32_t slot;
  };

  std::array<std::optional<DisplayTargetDef>, 5> displayTargets{};
  displayTargets[0] = DisplayTargetDef{"BaseColor", SelectedDebugTargetPass::TargetSlot_BaseColor};
  displayTargets[1] = DisplayTargetDef{"Shadow Lighting", SelectedDebugTargetPass::TargetSlot_VirtualShadowMapShadowLighting};
  displayTargets[2] = DisplayTargetDef{"SSS Shadow", SelectedDebugTargetPass::TargetSlot_VirtualShadowMapScreenSpaceShadow};
  displayTargets[3] = DisplayTargetDef{"VSM Pages", SelectedDebugTargetPass::TargetSlot_VirtualShadowMapPages};
  if (kEnableOptionalVsmTableOutput)
  {
    displayTargets[4] = DisplayTargetDef{"VSM Table", SelectedDebugTargetPass::TargetSlot_VirtualShadowMapTable};
  }

  SelectedDebugTargetPass::Inputs displayInputs{
    .baseColor = rendererPass->getColorTexture(),
    .clusterId = rendererPass->getColorTexture(),
    .prepassDepth = rendererPass->getPrepassDepthHZBTexture(),
    .sceneDepth = rendererPass->getSceneDepthTexture(),
#ifdef COLLECT_FRAME_STATISTICS
    .hardwareFrameStats = hardwareFrameStatisticsHeatmapTexture,
#else
    .hardwareFrameStats = fallbackColorTexture,
#endif
    .virtualShadowMapPages = virtualShadowMapPagesDebugTexture,
    .virtualShadowMapTable = virtualShadowMapTableDebugTexture,
    .virtualShadowMapShadowLighting = virtualShadowMapShadowLightingTexture,
    .virtualShadowMapScreenSpaceShadow = virtualShadowMapScreenSpaceShadowTexture,
  };

  auto *selectedDebugTargetPass = renderGraph->registerPass<SelectedDebugTargetPass>(
      "selectedDebugTargetPass",
      41,
      swapChainTexture,
      rhi->getSwapChainFormat(swapChain),
      displayInputs,
      SelectedDebugTargetPass::Settings{
          .viewPortWidth = kVW,
          .viewPortHeight = kVH,
      });
  auto updateSelectedDebugTarget = [&](uint32_t selectedSlot)
  {
    selectedDebugTargetSlot = selectedSlot;
    selectedDebugTargetPass->setSelectedSlot(selectedSlot);

    const bool showVsmPages = selectedSlot == SelectedDebugTargetPass::TargetSlot_VirtualShadowMapPages;
    const bool showVsmTable = selectedSlot == SelectedDebugTargetPass::TargetSlot_VirtualShadowMapTable;

    vsmPass->getBookkeepingPass()->setDebugOutputEnabled(showVsmPages);
    if (auto *pass = vsmPass->getTableDebugPass())
      pass->setEnabled(showVsmTable);
  };
  for (const auto &target : displayTargets)
  {
    if (target.has_value())
    {
      updateSelectedDebugTarget(target->slot);
      break;
    }
  }

  os::Logger::log("[8] Compiling render graph...");
  renderGraph->compile();
  std::vector<std::vector<RenderGraph::CommandBufferHandle>> swapChainCommandBufferHandles;
  swapChainCommandBufferHandles.resize(maxFramesInFlight);
  for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
  {
    auto &frameHandles = swapChainCommandBufferHandles[frameSlot];
    frameHandles.reserve(rhi->getSwapChainImagesCount(swapChain));
    for (uint32_t imageIndex = 0u; imageIndex < rhi->getSwapChainImagesCount(swapChain); ++imageIndex)
    {
      RenderGraph::Overrides overrides;
      const auto swapChainView = rhi->getSwapChainTextureView(swapChain, imageIndex);
      scene.appendFrameOverrides(overrides, frameSlot);
      preCullingPass->appendFrameOverrides(overrides, frameSlot);
      depthPrePass->appendFrameOverrides(overrides, frameSlot);
      finalCullingPass->appendFrameOverrides(overrides, frameSlot);
      hwDrawPass->appendFrameOverrides(overrides, frameSlot);
      materialPass->appendFrameOverrides(overrides, frameSlot);
      vsmManager.appendFrameOverrides(overrides, frameSlot);
      overrides.bufferOverrides.emplace(
          vsmReadbackBuffers.allocatorCounters.handle.name,
          RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.allocatorCounters.frameIds[frameSlot]});
      overrides.bufferOverrides.emplace(
          vsmReadbackBuffers.drawCounters.handle.name,
          RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.drawCounters.frameIds[frameSlot]});
      overrides.bufferOverrides.emplace(
          vsmReadbackBuffers.virtualPageTable.handle.name,
          RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.virtualPageTable.frameIds[frameSlot]});
      overrides.bufferOverrides.emplace(
          vsmReadbackBuffers.pageClusterCounts.handle.name,
          RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.pageClusterCounts.frameIds[frameSlot]});
      overrides.bufferOverrides.emplace(
          vsmReadbackBuffers.drawIndirect.handle.name,
          RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.drawIndirect.frameIds[frameSlot]});
      overrides.textureOverrides.emplace(
          "ColorAttatchmentTexture",
          RenderGraph::TextureOverride{
            .textureId = swapChainView.resourceId,
            .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          depthTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameDepthTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          colorTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameColorTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          materialIdTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameMaterialIdTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          materialUVTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameMaterialUVTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          previousDepthHZBTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFramePreviousDepthHZBTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapPagesDebugTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmPagesDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapTableDebugTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmTableDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapShadowLightingTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmShadowLightingTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapScreenSpaceShadowTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmScreenSpaceShadowTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapTableClip0DebugTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmTableClip0TextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      overrides.textureOverrides.emplace(
          virtualShadowMapTableClip1DebugTexture.name,
          RenderGraph::TextureOverride{
              .textureId = perFrameVsmTableClip1TextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
          });
      frameHandles.push_back(renderGraph->createCommandBuffer(overrides, frameSlot));
    }
  }
  {
    std::ostringstream targetsLog;
    targetsLog << "[Display] targets:";
    for (size_t targetIndex = 0; targetIndex < displayTargets.size(); ++targetIndex)
    {
      const auto &target = displayTargets[targetIndex];
      targetsLog << " " << ((targetIndex + 1u) % 10u) << "=" << (target.has_value() ? target->name : "off");
    }
    os::Logger::log(targetsLog.str());
  }

  // --------------------------------------------------------------------------
  // Frame loop
  // --------------------------------------------------------------------------
  float deltaTime = 0.0f;
  uint32_t frameCounter = 0u;
  bool isPaused = false;
  bool pauseLatch = false;
  bool lightLatch = false;
  uint32_t cameraLookDragButtons = 0u;
  bool shadowTapDecreaseLatch = false;
  bool shadowTapIncreaseLatch = false;
  bool pcfRadiusDecreaseLatch = false;
  bool pcfRadiusIncreaseLatch = false;
  bool pcfToggleLatch = false;
  std::array<bool, 5> displayTargetKeyLatch{};
  bool statsOverlayLatch = false;
  bool showStatsOverlay = true;
  OverlayPreparationWorker overlayPreparationWorker{renderGraph};
  std::shared_ptr<const PreparedOverlayState> latestOverlaySnapshot{};
  std::deque<PendingFrameDiagnostics> pendingFrameDiagnostics;
  renderGraph->setTimerReadbackEnabled(showStatsOverlay);

  scene.updatePageStreaming(assetPaths.objectName, 1u);
  scene.updateInstanceBuffer();

  auto finalizePendingFrame = [&](PendingFrameDiagnostics &pending)
  {
    const auto waitStart = lib::time::TimeSpan::now();
    renderGraph->waitFrame(pending.frame);
    pending.cpuFrame.waitMs = (lib::time::TimeSpan::now() - waitStart).milliseconds();
    pending.cpuFrame.waitBlockMs = renderGraph->getLastWaitFrameBlockSpanMs();
    pending.cpuFrame.timerReadbackMs = 0.0;

    measureCpuOperation(
        renderGraph,
        pending.cpuFrame.vtFeedback,
        [&]()
        {
          renderGraph->setCurrentFrameIndex(pending.frame.frameIndex);
          materialPass->processFeedback();
        });
    measureCpuOperation(
        renderGraph,
        pending.cpuFrame.sceneStreaming,
        [&]()
        {
          scene.updatePageStreaming(assetPaths.objectName, 1u);
        });
    pending.cpuFrame.streamingMs = pending.cpuFrame.sceneStreaming.wallMs;
    pending.cpuFrame.vsmResetInvalidationsMs = measureWallTimeMs([&]() { vsmManager.resetInvalidations(); });
    measureCpuOperation(
        renderGraph,
        pending.cpuFrame.instanceUpload,
        [&]()
        {
          scene.updateInstanceBuffer();
        });
    pending.cpuFrame.instanceUploadMs = pending.cpuFrame.instanceUpload.wallMs;

    if (showStatsOverlay)
    {
      OverlayPreparationInput input{};
      input.frameIndex = pending.displayFrameIndex;
      input.cpuFrame = pending.cpuFrame;
      input.renderGraphCpu = diffCpuStats(renderGraph->getCpuStats(), pending.renderGraphCpuStart);
      input.renderGraphRunTotalMs = pending.renderGraphRunTotalMs;
      input.renderGraphRunPhases = pending.renderGraphRunPhases;
      input.renderGraphRuntimeMetrics = pending.renderGraphRuntimeMetrics;
      input.renderGraphRunDebugStats = pending.renderGraphRunDebugStats;
      input.frameWaitSummary = renderGraph->getFrameWaitSummary();
      input.timerFrameSlot = pending.frame.frameIndex;
      input.collectTimers = pending.timerResolveSubmitted;
      input.vsmAllocatorCountersBufferId = vsmReadbackBuffers.allocatorCounters.frameIds[pending.frame.frameIndex];
      input.vsmDrawCountersBufferId = vsmReadbackBuffers.drawCounters.frameIds[pending.frame.frameIndex];
      input.vsmVirtualPageTableBufferId = vsmReadbackBuffers.virtualPageTable.frameIds[pending.frame.frameIndex];
      input.vsmPageClusterCountsBufferId = vsmReadbackBuffers.pageClusterCounts.frameIds[pending.frame.frameIndex];
      input.vsmDrawIndirectBufferId = vsmReadbackBuffers.drawIndirect.frameIds[pending.frame.frameIndex];
      input.allocationRequestCapacity = vsmManager.getAllocationRequestCapacity();
      input.futureAllocationRequestCapacity = vsmManager.getFutureAllocationRequestCapacity();
      input.visibleClusterDrawCapacity = vsmDrawPass->getVisibleClusterDrawCapacity();
      input.physicalPageTableResolution = vsmManager.getPhysicalPageTableResolution();
      input.activeVSMPageCount = activeVSMPageCount;
      input.collectStats = true;
      overlayPreparationWorker.enqueue(std::move(input));
    }
  };

  while (!window->shouldClose())
  {
    CpuFrameReport cpuFrame{};
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    {
      const auto windowUpdateStart = lib::time::TimeSpan::now();
      SDL_Event event;
      while (SDL_PollEvent(&event))
      {
        ImGui_ImplSDL3_ProcessEvent(&event);
        const bool imguiWantsMouse = showStatsOverlay && imguiIo.WantCaptureMouse;

        if (event.type == SDL_EVENT_QUIT)
        {
          nativeWindow->isRunning = false;
        }

        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
          int iw = 0;
          int ih = 0;
          SDL_GetWindowSize(nativeWindow->sdlWindow, &iw, &ih);
          nativeWindow->width = static_cast<uint32_t>(iw);
          nativeWindow->height = static_cast<uint32_t>(ih);
        }

        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        {
          cameraLookDragButtons = 0u;
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
          if (!imguiWantsMouse && (event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT))
          {
            cameraLookDragButtons |= SDL_BUTTON_MASK(event.button.button);
          }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        {
          if (event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT)
          {
            cameraLookDragButtons &= ~SDL_BUTTON_MASK(event.button.button);
          }
        }
        else if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
          const uint32_t lookDragMask = (SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE) | SDL_BUTTON_MASK(SDL_BUTTON_RIGHT));
          if (!imguiWantsMouse && (cameraLookDragButtons & lookDragMask) != 0u)
          {
            mouseDeltaX += event.motion.xrel;
            mouseDeltaY += event.motion.yrel;
          }
        }
      }
      cpuFrame.windowUpdateMs = (lib::time::TimeSpan::now() - windowUpdateStart).milliseconds();
    }
    if (window->shouldClose())
    {
      break;
    }

    // --- SDL3 input ----------------------------------------------------------
    const bool *keys = SDL_GetKeyboardState(nullptr);
    const bool imguiWantsKeyboard = showStatsOverlay && imguiIo.WantCaptureKeyboard;
    const bool imguiWantsMouse = showStatsOverlay && imguiIo.WantCaptureMouse;
    const bool moveForward = !imguiWantsKeyboard && keys[SDL_SCANCODE_UP];
    const bool moveBackward = !imguiWantsKeyboard && keys[SDL_SCANCODE_DOWN];
    const bool manualCamInput = moveForward || moveBackward;
    mouseDeltaX *= -1.0f;
    const bool manualLookInput = !imguiWantsMouse && (mouseDeltaX != 0.0f || mouseDeltaY != 0.0f);
    const bool rotatePitchUp = !imguiWantsKeyboard && keys[SDL_SCANCODE_W];
    const bool rotatePitchDown = !imguiWantsKeyboard && keys[SDL_SCANCODE_S];
    const bool rotateYawLeft = !imguiWantsKeyboard && keys[SDL_SCANCODE_A];
    const bool rotateYawRight = !imguiWantsKeyboard && keys[SDL_SCANCODE_D];
    const bool manualObjectInput = rotatePitchUp || rotatePitchDown || rotateYawLeft || rotateYawRight;
    const bool pauseKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_P];
    const bool lightKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_L];
    const bool shadowTapDecreaseKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_MINUS];
    const bool shadowTapIncreaseKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_EQUALS];
    const bool pcfRadiusDecreaseKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_LEFTBRACKET];
    const bool pcfRadiusIncreaseKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_RIGHTBRACKET];
    const bool pcfToggleKey = !imguiWantsKeyboard && keys[SDL_SCANCODE_F2];
    const bool statsOverlayKey = keys[SDL_SCANCODE_F1];
    const std::array<SDL_Scancode, 5> displayTargetKeys = {
      SDL_SCANCODE_1,
      SDL_SCANCODE_2,
      SDL_SCANCODE_3,
      SDL_SCANCODE_4,
      SDL_SCANCODE_5,
    };

    preCullingPass->settings.errorTreshhold = prepassLodErrorThreshold;
    finalCullingPass->settings.errorTreshhold = geometryLodErrorThreshold;

    if (statsOverlayKey)
    {
      if (!statsOverlayLatch)
      {
        statsOverlayLatch = true;
        showStatsOverlay = !showStatsOverlay;
        renderGraph->setTimerReadbackEnabled(showStatsOverlay);
      }
    }
    else
    {
      statsOverlayLatch = false;
    }

    if (pauseKey)
    {
      if (!pauseLatch)
      {
        pauseLatch = true;
        isPaused = !isPaused;
      }
    }
    else
    {
      pauseLatch = false;
    }

    if (lightKey)
    {
      if (!lightLatch)
      {
        lightLatch = true;
        lightDirection = (cameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f)).normalize();
        os::Logger::logf("[Light] directional light aligned to camera forward: (%.3f, %.3f, %.3f)", lightDirection[0], lightDirection[1], lightDirection[2]);
      }
    }
    else
    {
      lightLatch = false;
    }

    for (size_t keyIndex = 0; keyIndex < displayTargetKeys.size(); ++keyIndex)
    {
      const bool keyPressed = !imguiWantsKeyboard && keys[displayTargetKeys[keyIndex]];
      if (keyPressed && !displayTargetKeyLatch[keyIndex])
      {
        displayTargetKeyLatch[keyIndex] = true;
        if (displayTargets[keyIndex].has_value())
        {
          const DisplayTargetDef &displayTarget = *displayTargets[keyIndex];
          const uint32_t selectedSlot = displayTarget.slot;
          updateSelectedDebugTarget(selectedSlot);
          os::Logger::logf("[Display] selected %u=%s", static_cast<uint32_t>((keyIndex + 1u) % 10u), displayTarget.name);
        }
        else
        {
          os::Logger::logf("[Display] output %u is inactive", static_cast<uint32_t>((keyIndex + 1u) % 10u));
        }
      }
      else if (!keyPressed)
      {
        displayTargetKeyLatch[keyIndex] = false;
      }
    }

    bool shadowSettingsDirty = false;

    if (shadowTapDecreaseKey)
    {
      if (!shadowTapDecreaseLatch)
      {
        shadowTapDecreaseLatch = true;
        if (shadowFilterTaps > 9u)
        {
          shadowFilterTaps = 9u;
        }
        else if (shadowFilterTaps > 4u)
        {
          shadowFilterTaps = 4u;
        }
        else
        {
          shadowFilterTaps = 1u;
        }
        shadowSettingsDirty = true;
      }
    }
    else
    {
      shadowTapDecreaseLatch = false;
    }

    if (shadowTapIncreaseKey)
    {
      if (!shadowTapIncreaseLatch)
      {
        shadowTapIncreaseLatch = true;
        if (shadowFilterTaps < 4u)
        {
          shadowFilterTaps = 4u;
        }
        else if (shadowFilterTaps < 9u)
        {
          shadowFilterTaps = 9u;
        }
        else
        {
          shadowFilterTaps = 16u;
        }
        shadowSettingsDirty = true;
      }
    }
    else
    {
      shadowTapIncreaseLatch = false;
    }

    if (pcfToggleKey)
    {
      if (!pcfToggleLatch)
      {
        pcfToggleLatch = true;
        shadowPcfEnabled = !shadowPcfEnabled;
        shadowSettingsDirty = true;
      }
    }
    else
    {
      pcfToggleLatch = false;
    }

    if (pcfRadiusDecreaseKey)
    {
      if (!pcfRadiusDecreaseLatch)
      {
        pcfRadiusDecreaseLatch = true;
        pcfRadiusTexels = std::max(0.0f, pcfRadiusTexels - 0.25f);
        shadowSettingsDirty = true;
      }
    }
    else
    {
      pcfRadiusDecreaseLatch = false;
    }

    if (pcfRadiusIncreaseKey)
    {
      if (!pcfRadiusIncreaseLatch)
      {
        pcfRadiusIncreaseLatch = true;
        pcfRadiusTexels += 0.25f;
        shadowSettingsDirty = true;
      }
    }
    else
    {
      pcfRadiusIncreaseLatch = false;
    }

    if (shadowSettingsDirty)
    {
      applyShadowFilterSettings();
      logShadowFilterSettings();
      shadowSettingsDirty = false;
    }

    if (isPaused)
    {
      while (!pendingFrameDiagnostics.empty())
      {
        finalizePendingFrame(pendingFrameDiagnostics.front());
        pendingFrameDiagnostics.pop_front();
      }
      continue;
    }

    if (pendingFrameDiagnostics.size() >= maxFramesInFlight)
    {
      finalizePendingFrame(pendingFrameDiagnostics.front());
      pendingFrameDiagnostics.pop_front();
    }

    renderGraph->clearFrameStats();
    const RenderGraph::CpuStats renderGraphCpuStart = renderGraph->getCpuStats();
    const auto frameStart = lib::time::TimeSpan::now();
    const float dtSec = deltaTime / 1000.0f;
    const uint32_t frameSlot = frameCounter % maxFramesInFlight;
    renderGraph->setCurrentFrameIndex(frameSlot);

    const auto simulationStart = lib::time::TimeSpan::now();
    if (manualLookInput)
    {
      const math::Quatf yawRotation = math::Quatf::fromAxisAngle(kWorldUp, mouseDeltaX * kMouseLookSensitivity);
      cameraRotation = multiplyQuaternions(yawRotation, cameraRotation);

      const math::Vec3f currentRight = cameraRotation * math::Vec3f(1.0f, 0.0f, 0.0f);
      if (std::abs(mouseDeltaY) > 0.0f && currentRight.length() > 0.0f)
      {
        const math::Quatf pitchRotation = math::Quatf::fromAxisAngle(currentRight.normalize(), -mouseDeltaY * kMouseLookSensitivity);
        const math::Quatf proposedRotation = multiplyQuaternions(pitchRotation, cameraRotation);
        const math::Vec3f proposedForward = proposedRotation * math::Vec3f(0.0f, 0.0f, -1.0f);
        if (std::abs(proposedForward.y()) < kMaxLookDotY)
        {
          cameraRotation = proposedRotation;
        }
      }
    }

    if (manualCamInput)
    {
      const math::Vec3f cameraForward = (cameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f)).normalize();
      if (moveForward)
        cameraPosition = cameraPosition + cameraForward * (kCamSpeed * dtSec);
      if (moveBackward)
        cameraPosition = cameraPosition - cameraForward * (kCamSpeed * dtSec);
    }

    if (manualObjectInput)
    {
      if (rotatePitchUp)
        objectPitch += kObjectRotateSpeed * dtSec;
      if (rotatePitchDown)
        objectPitch -= kObjectRotateSpeed * dtSec;
      if (rotateYawLeft)
        objectYaw -= kObjectRotateSpeed * dtSec;
      if (rotateYawRight)
        objectYaw += kObjectRotateSpeed * dtSec;

      objectRotation = math::Quatf::fromEuler(objectPitch, objectYaw, 0.0f).normalize();
      scene.updateObjectInstanceTransform(instId, objectPosition, objectRotation, 1.0f);
    }
    cpuFrame.simulationMs = (lib::time::TimeSpan::now() - simulationStart).milliseconds();
    scene.prepareFrame(frameSlot);

    // --- Rebuild camera ------------------------------------------------------
    const auto cameraBuildStart = lib::time::TimeSpan::now();
    cam.setPosition(cameraPosition);
    cam.setRotation(cameraRotation);
    cam.updateMatrices();
    cpuFrame.cameraBuildMs = (lib::time::TimeSpan::now() - cameraBuildStart).milliseconds();

    // --- Upload uniforms to GPU passes ---------------------------------------
    const math::Mat4f &view = cam.getViewMatrix();
    const math::Mat4f &proj = cam.getProjectionMatrix();
    const math::Vec3f &cp = cam.getPosition();

    math::Vec4f vp4;
    vp4[0] = cp[0];
    vp4[1] = cp[1];
    vp4[2] = cp[2];
    vp4[3] = 1.0f;

    measureCpuOperation(
        renderGraph,
        cpuFrame.uniformUpload,
        [&]()
        {
          cpuFrame.preCullingUniformMs = measureWallTimeMs([&]() { preCullingPass->updateUniforms(view.data, proj.data, vp4.data, kVW, kVH, kNear, kFar, kHiZLvls); });
          cpuFrame.depthPreUniformMs = measureWallTimeMs([&]() { depthPrePass->updateUniforms(view.data, proj.data, vp4.data, kVW, kVH, kNear, kFar, kHiZLvls, geometryLodErrorThreshold); });
          cpuFrame.finalCullingUniformMs = measureWallTimeMs([&]() { finalCullingPass->updateUniforms(view.data, proj.data, vp4.data, kVW, kVH, kNear, kFar, kHiZLvls); });
          cpuFrame.hardwareDrawUniformMs = measureWallTimeMs([&]() { hwDrawPass->updateUniforms(view.data, proj.data, vp4.data, kVW, kVH, kNear, kFar, kHiZLvls, geometryLodErrorThreshold); });
          cpuFrame.materialCameraMs = measureWallTimeMs([&]() { materialPass->updateCamera(view, proj, kVW, kVH); });
          cpuFrame.materialBeginFrameMs = measureWallTimeMs([&]() { materialPass->beginFrame(); });
          cpuFrame.vsmLightSetupMs = measureWallTimeMs([&]() { vsmManager.setDirectionalLightDirection(vsmLightId, lightDirection); });
          cpuFrame.vsmUpdateMs = measureWallTimeMs([&]() { vsmManager.update(cam); });
          if (auto *pass = vsmPass->getScreenSpaceShadowPass())
          {
            const math::Mat4f viewProjection = proj * view;
            measureWallTimeMs([&]() { pass->updateRuntimeState(viewProjection, lightDirection, cam.isReverseZ()); });
          }

          const bool shadowContentChanged = manualObjectInput || hasActiveAnimation;
          if (shadowContentChanged)
          {
            cpuFrame.vsmInvalidateMs = measureWallTimeMs(
                [&]()
                {
                  AABB shadowInvalidationBounds;
                  shadowInvalidationBounds.minPoint[0] = objectPosition[0] + objectCentre[0] - kRadius;
                  shadowInvalidationBounds.minPoint[1] = objectPosition[1] + objectCentre[1] - kRadius;
                  shadowInvalidationBounds.minPoint[2] = objectPosition[2] + objectCentre[2] - kRadius;
                  shadowInvalidationBounds.maxPoint[0] = objectPosition[0] + objectCentre[0] + kRadius;
                  shadowInvalidationBounds.maxPoint[1] = objectPosition[1] + objectCentre[1] + kRadius;
                  shadowInvalidationBounds.maxPoint[2] = objectPosition[2] + objectCentre[2] + kRadius;
                  vsmManager.invalidateRegion(shadowInvalidationBounds);
                });
          }
        });
    cpuFrame.uniformUploadMs = cpuFrame.uniformUpload.wallMs;

    if (hasActiveAnimation)
    {
      cpuFrame.animationMs = measureWallTimeMs(
          [&]()
          {
            animationTimeSeconds += dtSec;
            if (!scene.applyAnimationFrame(instId, animationPlayer, activeAnimationName, animationTimeSeconds, true))
              throw std::runtime_error("Failed to apply animation frame for clip: " + activeAnimationName);
          });
    }

    // --- Submit frame --------------------------------------------------------
    RenderGraph::Frame frame;
    frame.frameIndex = frameSlot;
    const auto acquireStart = lib::time::TimeSpan::now();
    auto currentSwapView = rhi->getCurrentSwapChainTextureView(swapChain);
    cpuFrame.acquireMs = (lib::time::TimeSpan::now() - acquireStart).milliseconds();

    {
      const auto runStart = lib::time::TimeSpan::now();
      renderGraph->run(frame, swapChainCommandBufferHandles.at(frameSlot).at(currentSwapView.index));
      cpuFrame.renderRunMs = (lib::time::TimeSpan::now() - runStart).milliseconds();
    }

    const uint32_t displayFrameIndex = frameCounter + 1u;
    std::shared_ptr<const PreparedOverlayState> preparedOverlaySnapshot = overlayPreparationWorker.getLatestSnapshot();
    if (preparedOverlaySnapshot)
    {
      latestOverlaySnapshot = std::move(preparedOverlaySnapshot);
    }

    const bool drawStatsOverlay = showStatsOverlay;
    const auto uiBuildStart = lib::time::TimeSpan::now();
    if (drawStatsOverlay)
    {
      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();
      if (buildShadowControlsOverlay(
              prepassLodErrorThreshold,
              geometryLodErrorThreshold,
              vsmLodErrorThreshold,
              vsmDirtyPageStencilEnabled,
              shadowPcfEnabled,
              shadowFilterTaps,
              shadowBias,
              slopeScaleBias,
              maxShadowBias,
              pcfRadiusTexels,
              normalBiasTexels,
              shadowAmbientColor,
              screenSpaceShadowEnabled,
              screenSpaceSurfaceThickness,
              screenSpaceBilinearThreshold,
              screenSpaceShadowContrast,
              screenSpaceRayDistance,
              screenSpaceDepthBounds,
              screenSpaceIgnoreEdgePixels,
              screenSpaceUsePrecisionOffset,
              screenSpaceBilinearSamplingOffsetMode,
              screenSpaceUseEarlyOut,
              screenSpaceTreatSkippedEdgeSamplesAsLit,
              screenSpaceDebugOutputEdgeMask,
              screenSpaceDebugOutputThreadIndex,
              screenSpaceDebugOutputWaveIndex))
      {
        shadowSettingsDirty = true;
      }
      if (latestOverlaySnapshot)
      {
        buildStatsOverlay(
            latestOverlaySnapshot->frameIndex,
            latestOverlaySnapshot->deltaTimeMs,
            shadowPcfEnabled,
            latestOverlaySnapshot->cpuFrame,
            latestOverlaySnapshot->renderGraphCpu,
            latestOverlaySnapshot->vsmStats,
            latestOverlaySnapshot->passTimings,
            latestOverlaySnapshot->overlayHistory,
            latestOverlaySnapshot->passTimelineHistory);
        buildPassTimelineOverlay(latestOverlaySnapshot->passTimelineHistory);
        buildCpuTimelineOverlay(
            latestOverlaySnapshot->cpuTimelineHistory,
            latestOverlaySnapshot->cpuFrame,
            latestOverlaySnapshot->currentCpuSections,
            latestOverlaySnapshot->renderGraphRunTotalMs,
            latestOverlaySnapshot->renderGraphRunPhases,
            latestOverlaySnapshot->renderGraphRuntimeMetrics,
            latestOverlaySnapshot->renderGraphRunDebugStats,
            latestOverlaySnapshot->frameWaitSummary);
      }
      ImGui::Render();
    }
    cpuFrame.uiBuildMs = drawStatsOverlay ? (lib::time::TimeSpan::now() - uiBuildStart).milliseconds() : 0.0;

    if (shadowSettingsDirty)
    {
      applyShadowFilterSettings();
      logShadowFilterSettings();
    }

    const auto presentStart = lib::time::TimeSpan::now();
    if (drawStatsOverlay)
    {
      const uintptr_t framebufferKey = static_cast<uintptr_t>(currentSwapView.resourceId);
      rhi->presentWithOverlay(
          swapChain,
          currentSwapView.resourceId,
          rendering::ResourceLayout::COLOR_ATTACHMENT,
          [&, drawData = ImGui::GetDrawData()](VkCommandBuffer commandBuffer, VkImageView imageView, VkExtent2D extent)
          {
            if (drawData == nullptr || drawData->CmdListsCount == 0)
            {
              return;
            }

            VkFramebuffer &imguiFramebuffer = imguiFramebuffers[framebufferKey];
            if (imguiFramebuffer == VK_NULL_HANDLE)
            {
              VkFramebufferCreateInfo framebufferInfo{};
              framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
              framebufferInfo.renderPass = imguiRenderPass;
              framebufferInfo.attachmentCount = 1u;
              framebufferInfo.pAttachments = &imageView;
              framebufferInfo.width = extent.width;
              framebufferInfo.height = extent.height;
              framebufferInfo.layers = 1u;

              checkImGuiVkResult(vkCreateFramebuffer(rhi->getDevice(), &framebufferInfo, nullptr, &imguiFramebuffer));
            }

            VkRenderPassBeginInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = imguiRenderPass;
            renderPassInfo.framebuffer = imguiFramebuffer;
            renderPassInfo.renderArea.offset = {0, 0};
            renderPassInfo.renderArea.extent = extent;

            vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
            vkCmdEndRenderPass(commandBuffer);
          });
    }
    else
    {
      rhi->present(swapChain, currentSwapView.resourceId, rendering::ResourceLayout::COLOR_ATTACHMENT);
    }
    cpuFrame.presentMs = (lib::time::TimeSpan::now() - presentStart).milliseconds();

    const auto frameEnd = lib::time::TimeSpan::now();
    deltaTime = (frameEnd - frameStart).milliseconds();
    cpuFrame.totalMs = deltaTime;
    cpuFrame.untrackedMs = std::max(0.0, cpuFrame.totalMs - computeTrackedCpuFrameMs(cpuFrame));
    windowTitleStats.push(nativeWindow->sdlWindow, deltaTime);

    PendingFrameDiagnostics currentPending{};
    currentPending.frame = std::move(frame);
    currentPending.displayFrameIndex = displayFrameIndex;
    currentPending.frameDeltaMs = deltaTime;
    currentPending.timerResolveSubmitted = renderGraph->isTimerReadbackEnabled();
    currentPending.cpuFrame = cpuFrame;
    currentPending.renderGraphCpuStart = renderGraphCpuStart;
    currentPending.renderGraphRunTotalMs = renderGraph->getLastRunTotalMs();
    currentPending.renderGraphRunPhases = renderGraph->getLastRunPhaseRecords();
    currentPending.renderGraphRuntimeMetrics = renderGraph->getLastRuntimeMetricRecords();
    currentPending.renderGraphRunDebugStats = renderGraph->getLastRunDebugStats();
    pendingFrameDiagnostics.push_back(std::move(currentPending));

    ++frameCounter;
  }

  // --------------------------------------------------------------------------
  // Shutdown
  // --------------------------------------------------------------------------
  while (!pendingFrameDiagnostics.empty())
  {
    finalizePendingFrame(pendingFrameDiagnostics.front());
    pendingFrameDiagnostics.pop_front();
  }
  overlayPreparationWorker.stop();
  auto destroyReadbackBufferSet = [&](ReadbackBufferSet &bufferSet)
  {
    for (const BufferId bufferId : bufferSet.frameIds)
    {
      if (bufferId != BufferId::Invalid)
      {
        rhi->deleteBuffer(bufferId);
      }
    }
    bufferSet.frameIds.clear();
  };
  destroyReadbackBufferSet(vsmReadbackBuffers.allocatorCounters);
  destroyReadbackBufferSet(vsmReadbackBuffers.drawCounters);
  destroyReadbackBufferSet(vsmReadbackBuffers.virtualPageTable);
  destroyReadbackBufferSet(vsmReadbackBuffers.pageClusterCounts);
  destroyReadbackBufferSet(vsmReadbackBuffers.drawIndirect);
  for (const auto &frameHandles : swapChainCommandBufferHandles)
  {
    for (const RenderGraph::CommandBufferHandle handle : frameHandles)
    {
      renderGraph->destroyCommandBuffer(handle);
    }
  }
  rhi->waitIdle();
  for (const auto &[_, framebuffer] : imguiFramebuffers)
  {
    if (framebuffer != VK_NULL_HANDLE)
    {
      vkDestroyFramebuffer(rhi->getDevice(), framebuffer, nullptr);
    }
  }
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  vkDestroyRenderPass(rhi->getDevice(), imguiRenderPass, nullptr);
  renderGraph->removeSwapChainImages(swapChain);
  rhi->destroySwapChain(swapChain);
  delete renderGraph;
  os::Logger::shutdown();
  windowTitleStats.reset(nativeWindow->sdlWindow);
  delete window;
  delete rhi;

  return 0;
}
