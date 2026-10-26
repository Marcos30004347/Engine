#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./utils/File.hpp"
#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/SparseSDFBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "globalIllumination/SDFRayTraceEvaluateMaterialPass.hpp"
#include "globalIllumination/ScreenSpaceHitShadingResolvePass.hpp"
#include "globalIllumination/TraceShadingAlbedoDebugPass.hpp"
#include "globalIllumination/WorldHitShadingResolvePass.hpp"
#include "globalIllumination/WorldHitShadingTypes.hpp"
#include "globalIllumination/worldgi/WorldGIComposePass.hpp"
#include "globalIllumination/worldgi/WorldGIHitShadingPass.hpp"
#include "globalIllumination/worldgi/WorldGILightGridDebugPass.hpp"
#include "globalIllumination/worldgi/WorldGILightGridUpdatePass.hpp"
#include "globalIllumination/worldgi/WorldGIRayGenPass.hpp"
#include "globalIllumination/worldgi/WorldGITraceSurfaceResolvePass.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "globalIllumination/surfels/SurfelAccelerationClearPass.hpp"
#include "globalIllumination/surfels/SurfelAccelerationCountPass.hpp"
#include "globalIllumination/surfels/SurfelAccelerationInsertPass.hpp"
#include "globalIllumination/surfels/SurfelAccelerationPrefixMergePass.hpp"
#include "globalIllumination/surfels/SurfelAccelerationPrefixSegmentsPass.hpp"
#include "globalIllumination/surfels/SurfelAccelerationPrefixSumPass.hpp"
#include "globalIllumination/surfels/SurfelDirectDrawDebugPass.hpp"
#include "globalIllumination/surfels/SurfelHeatmapDebugPass.hpp"
#include "globalIllumination/surfels/SurfelHierarchyDebugPass.hpp"
#include "globalIllumination/surfels/SurfelHierarchyTypes.hpp"
#include "globalIllumination/surfels/SurfelLightingUpdatePass.hpp"
#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"
#include "globalIllumination/surfels/SurfelScreenSpaceSpawnPass.hpp"
#include "globalIllumination/surfels/SurfelTracePreparePass.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsAtlasDebugPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFallbackDebugDrawPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFallbackDebugPreparePass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceDebugPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTracePass.hpp"
#include "os/Logger.hpp"
#include "rendering/animation/AnimationFile.hpp"
#include "rendering/animation/AnimationPlayer.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/core/LightCamera.hpp"
#include "rendering/gpgpu/ColorToQuadPass.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/FrameStatisticsHeatmapPass.hpp"
#include "rendering/gpgpu/RadixSort2Pass.hpp"
#include "rendering/gpgpu/SvgfAtrousFilterPass.hpp"
#include "rendering/gpgpu/SvgfDenoisePass.hpp"
#include "rendering/gpgpu/TextureCopyPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "scene/SceneManager.hpp"
#include "sdf/SDFSceneManager.hpp"
#include "sdf/rendering/SDFSceneGridBuildPass.hpp"
#include "sdf/rendering/SDFTraceDebugPass.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "softwarebvh/SWRBVHFallbackMeshFile.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareBLASSceneBridge.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"
#include "softwarebvh/SoftwareTLASSceneBridge.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"
#include "softwarebvh/rendering/SoftwareTLASPass.hpp"
#include "softwarebvh/rendering/SoftwareWorldBLASUpdatePass.hpp"
#include "time/TimeSpan.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPrePassDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTraceSdfFallbackPreparePass.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTraceSoftwareFallbackPreparePass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryRendererPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryTAAHistoryPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryTAAResolvePass.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapPass.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
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

template <typename T> static std::vector<T> readBufferElements(RenderGraph *renderGraph, const Buffer &buffer, uint32_t elementCount)
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

template <typename T> static std::vector<T> readBufferElements(rendering::RHI *rhi, rendering::BufferId bufferId, uint32_t elementCount)
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

class TraceStatsClearPass : public rendering::Pass
{
public:
  explicit TraceStatsClearPass(rendering::Buffer statsBuffer) : statsBuffer_(statsBuffer)
  {
  }

  ~TraceStatsClearPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
      .buffer = buffer,
      .offset = 0u,
      .size = size,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {rendering::BindingGroupLayout{
            .buffers =
                {
                  {.name = "traceStats", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
          }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_Groups",
          .groups = {rendering::GroupInfo{
            .name = "group0",
            .buffers =
                {
                  {.binding = 0, .bufferView = makeStorageBufferView(statsBuffer_, gpusigneddistancefields::gpgpu::getTraceStatsBufferSize())},
                },
          }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-trace-stats-clear-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "cs_main",
          .layout = layout_,
          .name = passName + "_Pipeline",
          .shader = shader_,
        });
  }

  rendering::Buffer statsBuffer_{};
  bool enabled_ = true;
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
};

static float haltonSequence(uint32_t index, uint32_t base)
{
  float result = 0.0f;
  float invBase = 1.0f / static_cast<float>(base);
  float fraction = invBase;
  uint32_t current = index;
  while (current > 0u)
  {
    result += static_cast<float>(current % base) * fraction;
    current /= base;
    fraction *= invBase;
  }
  return result;
}

static uint32_t computeScaledViewportExtent(uint32_t displayExtent, float renderScale)
{
  return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(displayExtent) * static_cast<double>(renderScale))));
}

static math::Vec2f computeTaaJitter(uint64_t frameIndex, uint32_t sampleCount, float jitterScale, float renderScale)
{
  const float clampedRenderScale = std::clamp(renderScale, 0.25f, 1.0f);
  const float upscaleFactor = 1.0f / clampedRenderScale;
  const uint32_t effectiveSampleCount = std::min(256u, std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(sampleCount) * static_cast<double>(std::clamp(upscaleFactor, 1.0f, 2.0f))))));
  const float effectiveJitterScale = jitterScale * std::clamp(upscaleFactor, 1.0f, 2.0f);
  const uint32_t sequenceIndex = static_cast<uint32_t>(frameIndex % std::max<uint64_t>(1u, effectiveSampleCount)) + 1u;
  math::Vec2f jitter;
  jitter[0] = (haltonSequence(sequenceIndex, 2u) - 0.5f) * effectiveJitterScale;
  jitter[1] = (haltonSequence(sequenceIndex, 3u) - 0.5f) * effectiveJitterScale;
  return jitter;
}

static math::Mat4f applyProjectionJitter(const math::Mat4f &projection, const math::Vec2f &jitterPixels, uint32_t width, uint32_t height)
{
  math::Mat4f jittered = projection;
  const float jitterNdcX = 2.0f * jitterPixels[0] / static_cast<float>(std::max(1u, width));
  const float jitterNdcY = -2.0f * jitterPixels[1] / static_cast<float>(std::max(1u, height));
  jittered.at(0, 2) -= jitterNdcX;
  jittered.at(1, 2) -= jitterNdcY;
  return jittered;
}

class SelectedDebugTargetPass : public rendering::Pass
{
public:
  enum TargetSlot : uint32_t
  {
    TargetSlot_ShadedColor = 0u,
    TargetSlot_DebugColor = 1u,
    TargetSlot_PrepassDepth = 2u,
    TargetSlot_SceneDepth = 3u,
    TargetSlot_Overdraw = 4u,
    TargetSlot_MaterialId = 5u,
    TargetSlot_MotionVectors = 6u,
    TargetSlot_VirtualShadowMapPages = 7u,
    TargetSlot_VirtualShadowMapTable = 8u,
    TargetSlot_VirtualShadowMapShadowLighting = 9u,
    TargetSlot_VirtualShadowMapScreenSpaceShadow = 10u,
    TargetSlot_TriangleId = 11u,
    TargetSlot_ClusterId = 12u,
    TargetSlot_SignedDistanceFieldsTrace = 13u,
    TargetSlot_SignedDistanceFieldsAtlas = 14u,
    TargetSlot_SignedDistanceFieldsFallbackScene = 15u,
    TargetSlot_SoftwareRayTrace = 16u,
    TargetSlot_SoftwareRayTraceHeatmap = 17u,
    TargetSlot_BaseColor = 18u,
    TargetSlot_Normal = 19u,
    TargetSlot_GeometryNormal = 20u,
    TargetSlot_DisocclusionMask = 21u,
    TargetSlot_ScreenProbes = 22u,
    TargetSlot_SpatialRadianceCache = 23u,
    TargetSlot_ScreenProbeIndirectDiffuse = 24u,
    TargetSlot_ScreenProbeBentNormal = 25u,
    TargetSlot_ScreenProbeAmbientOcclusion = 26u,
    TargetSlot_ScreenProbeAODebugDistance = 27u,
    TargetSlot_ScreenProbeAODebugCascade = 28u,
    TargetSlot_ScreenProbeAODebugFlags = 29u,
    TargetSlot_ScreenProbeAODebugSteps = 30u,
    TargetSlot_SpatialRadianceReservoirCache = 39u,
    TargetSlot_SurfelHierarchy = 40u,
    TargetSlot_SurfelDirectDraw = 41u,
    TargetSlot_SurfelHeatmap = 42u,
    TargetSlot_ScreenSpaceRayTraceSteps = 43u,
    TargetSlot_VirtualShadowMapAtlas = 44u,
    TargetSlot_ImperfectShadowMapVplTable = 45u,
    TargetSlot_ImperfectShadowMapVplAtlas = 46u,
    TargetSlot_ImperfectShadowMapVplPoints = 47u,
    TargetSlot_ImperfectShadowMapVplPages = 48u,
    TargetSlot_RsmIndirectDiffuse = 51u,
    TargetSlot_RsmFlux = 52u,
    TargetSlot_RsmNormal = 53u,
    TargetSlot_RsmWorldPosition = 54u,
    TargetSlot_RsmIndirectDiffuseDenoised = 55u,
    TargetSlot_VirtualShadowMapScreenSpaceSoftShadow = 56u,
    TargetSlot_WorldGIDiffuseLighting = 57u,
    TargetSlot_WorldGIGlossyRadiance = 58u,
    TargetSlot_WorldGIIndirectLighting = 59u,
  };

  struct Inputs
  {
    Texture shadedColor;
    Texture debugColor;
    Texture baseColor;
    Texture geometryNormal;
    Texture normal;
    Texture prepassDepth;
    Texture sceneDepth;
    Texture overdraw;
    Texture materialId;
    Texture motionVectors;
    Texture virtualShadowMapPages;
    Texture virtualShadowMapTable;
    Texture virtualShadowMapAtlas;
    Texture imperfectShadowMapVplPages;
    Texture imperfectShadowMapVplTable;
    Texture imperfectShadowMapVplAtlas;
    Texture imperfectShadowMapVplPoints;
    Texture virtualShadowMapShadowLighting;
    Texture virtualShadowMapScreenSpaceShadow;
    Texture virtualShadowMapScreenSpaceSoftShadow;
    Texture packedGeometryIdsLo;
    Texture packedGeometryIdsHi;
    Texture signedDistanceFieldsTrace;
    Texture signedDistanceFieldsAtlas;
    Texture signedDistanceFieldsSurfaceCacheAlbedoAtlas;
    Texture signedDistanceFieldsSurfaceCacheNormalAtlas;
    Texture signedDistanceFieldsFallbackScene;
    Texture softwareRayTrace;
    Texture disocclusionMask;
    Texture screenProbes;
    Texture spatialRadianceCache;
    Texture screenProbeIndirectDiffuse;
    Texture screenProbeBentNormal;
    Texture screenProbeAmbientOcclusion;
    Texture screenProbeAODebug;
    Texture rsmIndirectDiffuse;
    Texture rsmIndirectDiffuseDenoised;
    Texture worldGIDiffuseLighting;
    Texture worldGIGlossyRadiance;
    Texture worldGIIndirectLighting;
    Texture spatialRadianceReservoirCache;
    Texture surfelHierarchy;
    Texture surfelDirectDraw;
    Texture surfelHeatmap;
    Texture screenSpaceRayTraceSteps;
  };

  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
  };

  explicit SelectedDebugTargetPass(Texture outputTexture, Format outputFormat, Inputs inputs, Settings settings) : outputTexture_(outputTexture), outputFormat_(outputFormat), inputs_(inputs), settings_(settings)
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
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {.name = "uniforms",
                         .binding = 0,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_UniformBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Vertex | BindingVisibility::BindingVisibility_Fragment},
                      },
                  .samplers =
                      {
                        {.name = "texSampler", .binding = 1, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                  .textures =
                      {
                        {.name = "shadedColorTexture", .binding = 2, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "debugColorTexture", .binding = 3, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "baseColorTexture", .binding = 4, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "geometryNormalTexture", .binding = 5, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "normalTexture", .binding = 6, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "prepassDepthTexture", .binding = 7, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "sceneDepthTexture", .binding = 8, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "overdrawTexture", .binding = 9, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "materialIdTexture", .binding = 10, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "motionVectorTexture", .binding = 11, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmPagesDebugTexture", .binding = 12, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmTableDebugTexture", .binding = 13, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmShadowLightingTexture", .binding = 14, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmScreenSpaceShadowTexture", .binding = 15, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "packedGeometryIdsLoTexture", .binding = 16, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "packedGeometryIdsHiTexture", .binding = 17, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "signedDistanceFieldsTraceTexture", .binding = 18, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "signedDistanceFieldsAtlasTexture", .binding = 19, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "signedDistanceFieldsFallbackSceneTexture", .binding = 20, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "softwareRayTraceTexture", .binding = 21, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "disocclusionMaskTexture", .binding = 22, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenProbeDebugTexture", .binding = 23, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "spatialRadianceCacheTexture", .binding = 24, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenProbeIndirectDiffuseTexture", .binding = 25, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenProbeBentNormalTexture", .binding = 26, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenProbeAmbientOcclusionTexture", .binding = 27, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenProbeAODebugTexture", .binding = 28, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "spatialRadianceReservoirCacheTexture", .binding = 37, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "surfelHierarchyTexture", .binding = 38, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "surfelDirectDrawTexture", .binding = 39, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "surfelHeatmapTexture", .binding = 40, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "screenSpaceRayTraceStepsTexture", .binding = 41, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmAtlasDebugTexture", .binding = 42, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "ismVplTableDebugTexture", .binding = 43, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "ismVplAtlasDebugTexture", .binding = 44, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "ismVplPointsDebugTexture", .binding = 45, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "signedDistanceFieldsSurfaceCacheAlbedoAtlasTexture", .binding = 46, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "signedDistanceFieldsSurfaceCacheNormalAtlasTexture", .binding = 47, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "ismVplPagesDebugTexture", .binding = 48, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "rsmIndirectDiffuseTexture", .binding = 49, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "rsmIndirectDiffuseDenoisedTexture", .binding = 50, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "vsmScreenSpaceSoftShadowTexture", .binding = 51, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "worldGIDiffuseLightingTexture", .binding = 52, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "worldGIGlossyRadianceTexture", .binding = 53, .visibility = BindingVisibility::BindingVisibility_Fragment},
                        {.name = "worldGIIndirectLightingTexture", .binding = 54, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                },
              },
        });

    const TextureView shadedColorView = makeColorTextureView(inputs_.shadedColor);
    const TextureView debugColorView = makeColorTextureView(inputs_.debugColor);
    const TextureView baseColorView = makeColorTextureView(inputs_.baseColor);
    const TextureView geometryNormalView = makeColorTextureView(inputs_.geometryNormal);
    const TextureView normalView = makeColorTextureView(inputs_.normal);
    const TextureView prepassDepthView = makeColorTextureView(inputs_.prepassDepth);
    const TextureView sceneDepthView = makeDepthTextureView(inputs_.sceneDepth);
    const TextureView overdrawView = makeColorTextureView(inputs_.overdraw);
    const TextureView materialIdView = makeColorTextureView(inputs_.materialId);
    const TextureView motionVectorView = makeColorTextureView(inputs_.motionVectors);
    const TextureView vsmPagesDebugView = makeColorTextureView(inputs_.virtualShadowMapPages);
    const TextureView vsmTableDebugView = makeColorTextureView(inputs_.virtualShadowMapTable);
    const TextureView vsmAtlasDebugView = makeColorTextureView(inputs_.virtualShadowMapAtlas);
    const TextureView ismVplPagesDebugView = makeColorTextureView(inputs_.imperfectShadowMapVplPages);
    const TextureView ismVplTableDebugView = makeColorTextureView(inputs_.imperfectShadowMapVplTable);
    const TextureView ismVplAtlasDebugView = makeColorTextureView(inputs_.imperfectShadowMapVplAtlas);
    const TextureView ismVplPointsDebugView = makeColorTextureView(inputs_.imperfectShadowMapVplPoints);
    const TextureView vsmShadowLightingView = makeColorTextureView(inputs_.virtualShadowMapShadowLighting);
    const TextureView vsmScreenSpaceShadowView = makeColorTextureView(inputs_.virtualShadowMapScreenSpaceShadow);
    const TextureView vsmScreenSpaceSoftShadowView = makeColorTextureView(inputs_.virtualShadowMapScreenSpaceSoftShadow);
    const TextureView packedGeometryIdsLoView = makeColorTextureView(inputs_.packedGeometryIdsLo);
    const TextureView packedGeometryIdsHiView = makeColorTextureView(inputs_.packedGeometryIdsHi);
    const TextureView signedDistanceFieldsTraceView = makeColorTextureView(inputs_.signedDistanceFieldsTrace);
    const TextureView signedDistanceFieldsAtlasView = makeColorTextureView(inputs_.signedDistanceFieldsAtlas);
    const TextureView signedDistanceFieldsSurfaceCacheAlbedoAtlasView = makeColorTextureView(inputs_.signedDistanceFieldsSurfaceCacheAlbedoAtlas);
    const TextureView signedDistanceFieldsSurfaceCacheNormalAtlasView = makeColorTextureView(inputs_.signedDistanceFieldsSurfaceCacheNormalAtlas);
    const TextureView signedDistanceFieldsFallbackSceneView = makeColorTextureView(inputs_.signedDistanceFieldsFallbackScene);
    const TextureView softwareRayTraceView = makeColorTextureView(inputs_.softwareRayTrace);
    const TextureView disocclusionMaskView = makeColorTextureView(inputs_.disocclusionMask);
    const TextureView screenProbeDebugView = makeColorTextureView(inputs_.screenProbes);
    const TextureView spatialRadianceCacheView = makeColorTextureView(inputs_.spatialRadianceCache);
    const TextureView screenProbeIndirectDiffuseView = makeColorTextureView(inputs_.screenProbeIndirectDiffuse);
    const TextureView screenProbeBentNormalView = makeColorTextureView(inputs_.screenProbeBentNormal);
    const TextureView screenProbeAmbientOcclusionView = makeColorTextureView(inputs_.screenProbeAmbientOcclusion);
    const TextureView screenProbeAODebugView = makeColorTextureView(inputs_.screenProbeAODebug);
    const TextureView rsmIndirectDiffuseView = makeColorTextureView(inputs_.rsmIndirectDiffuse);
    const TextureView rsmIndirectDiffuseDenoisedView = makeColorTextureView(inputs_.rsmIndirectDiffuseDenoised);
    const TextureView worldGIDiffuseLightingView = makeColorTextureView(inputs_.worldGIDiffuseLighting);
    const TextureView worldGIGlossyRadianceView = makeColorTextureView(inputs_.worldGIGlossyRadiance);
    const TextureView worldGIIndirectLightingView = makeColorTextureView(inputs_.worldGIIndirectLighting);
    const TextureView spatialRadianceReservoirCacheView = makeColorTextureView(inputs_.spatialRadianceReservoirCache);
    const TextureView surfelHierarchyView = makeColorTextureView(inputs_.surfelHierarchy);
    const TextureView surfelDirectDrawView = makeColorTextureView(inputs_.surfelDirectDraw);
    const TextureView surfelHeatmapView = makeColorTextureView(inputs_.surfelHeatmap);
    const TextureView screenSpaceRayTraceStepsView = makeColorTextureView(inputs_.screenSpaceRayTraceSteps);

    bindingGroups_ = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_bindingGroups",
          .groups =
              {
                GroupInfo{
                  .name = "group0",
                  .buffers =
                      {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                      },
                  .samplers =
                      {
                        {.binding = 1, .sampler = sampler_, .view = shadedColorView},
                      },
                  .textures =
                      {
                        {.binding = 2, .textureView = shadedColorView},
                        {.binding = 3, .textureView = debugColorView},
                        {.binding = 4, .textureView = baseColorView},
                        {.binding = 5, .textureView = geometryNormalView},
                        {.binding = 6, .textureView = normalView},
                        {.binding = 7, .textureView = prepassDepthView},
                        {.binding = 8, .textureView = sceneDepthView},
                        {.binding = 9, .textureView = overdrawView},
                        {.binding = 10, .textureView = materialIdView},
                        {.binding = 11, .textureView = motionVectorView},
                        {.binding = 12, .textureView = vsmPagesDebugView},
                        {.binding = 13, .textureView = vsmTableDebugView},
                        {.binding = 14, .textureView = vsmShadowLightingView},
                        {.binding = 15, .textureView = vsmScreenSpaceShadowView},
                        {.binding = 16, .textureView = packedGeometryIdsLoView},
                        {.binding = 17, .textureView = packedGeometryIdsHiView},
                        {.binding = 18, .textureView = signedDistanceFieldsTraceView},
                        {.binding = 19, .textureView = signedDistanceFieldsAtlasView},
                        {.binding = 20, .textureView = signedDistanceFieldsFallbackSceneView},
                        {.binding = 21, .textureView = softwareRayTraceView},
                        {.binding = 22, .textureView = disocclusionMaskView},
                        {.binding = 23, .textureView = screenProbeDebugView},
                        {.binding = 24, .textureView = spatialRadianceCacheView},
                        {.binding = 25, .textureView = screenProbeIndirectDiffuseView},
                        {.binding = 26, .textureView = screenProbeBentNormalView},
                        {.binding = 27, .textureView = screenProbeAmbientOcclusionView},
                        {.binding = 28, .textureView = screenProbeAODebugView},
                        {.binding = 37, .textureView = spatialRadianceReservoirCacheView},
                        {.binding = 38, .textureView = surfelHierarchyView},
                        {.binding = 39, .textureView = surfelDirectDrawView},
                        {.binding = 40, .textureView = surfelHeatmapView},
                        {.binding = 41, .textureView = screenSpaceRayTraceStepsView},
                        {.binding = 42, .textureView = vsmAtlasDebugView},
                        {.binding = 43, .textureView = ismVplTableDebugView},
                        {.binding = 44, .textureView = ismVplAtlasDebugView},
                        {.binding = 45, .textureView = ismVplPointsDebugView},
                        {.binding = 46, .textureView = signedDistanceFieldsSurfaceCacheAlbedoAtlasView},
                        {.binding = 47, .textureView = signedDistanceFieldsSurfaceCacheNormalAtlasView},
                        {.binding = 48, .textureView = ismVplPagesDebugView},
                        {.binding = 49, .textureView = rsmIndirectDiffuseView},
                        {.binding = 50, .textureView = rsmIndirectDiffuseDenoisedView},
                        {.binding = 51, .textureView = vsmScreenSpaceSoftShadowView},
                        {.binding = 52, .textureView = worldGIDiffuseLightingView},
                        {.binding = 53, .textureView = worldGIGlossyRadianceView},
                        {.binding = 54, .textureView = worldGIIndirectLightingView},
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
  uint32_t selectedSlot_ = TargetSlot_ShadedColor;

  BindingGroups bindingGroups_;
  BindingsLayout layout_;
  Shader vertexShader_;
  Shader fragmentShader_;
  GraphicsPipeline graphicsPipeline_;
  Buffer uniformBuffer_;
  Sampler sampler_;
};

class VsmPagesOverlayPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
  };

  VsmPagesOverlayPass(Texture outputTexture, Format outputFormat, Texture overlayTexture, Settings settings)
      : outputTexture_(outputTexture), outputFormat_(outputFormat), overlayTexture_(overlayTexture), settings_(settings)
  {
  }

  ~VsmPagesOverlayPass() override
  {
    renderGraph->deleteGraphicsPipeline(graphicsPipeline_);
    renderGraph->deleteShader(vertexShader_);
    renderGraph->deleteShader(fragmentShader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteSampler(sampler_);
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setOpacity(float opacity)
  {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
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
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {.name = "uniforms",
                         .binding = 0,
                         .isDynamic = false,
                         .type = BufferBindingType::BufferBindingType_UniformBuffer,
                         .visibility = BindingVisibility::BindingVisibility_Vertex | BindingVisibility::BindingVisibility_Fragment},
                      },
                  .samplers =
                      {
                        {.name = "texSampler", .binding = 1, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                  .textures =
                      {
                        {.name = "vsmPagesTexture", .binding = 2, .visibility = BindingVisibility::BindingVisibility_Fragment},
                      },
                },
              },
        });

    const TextureView overlayTextureView = makeColorTextureView(overlayTexture_);
    bindingGroups_ = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_bindingGroups",
          .groups =
              {
                GroupInfo{
                  .name = "group0",
                  .buffers =
                      {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                      },
                  .samplers =
                      {
                        {.binding = 1, .sampler = sampler_, .view = overlayTextureView},
                      },
                  .textures =
                      {
                        {.binding = 2, .textureView = overlayTextureView},
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
          .src = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadVsmPagesOverlay-fs.spirv"),
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
        .blendMode = ColorBlendMode::AlphaBlend,
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
    uint32_t enabled = 0u;
    float opacity = 0.45f;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
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

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.enabled = enabled_ ? 1u : 0u;
    uniforms.opacity = opacity_;
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(Uniforms), &uniforms);
  }

  Texture outputTexture_;
  Format outputFormat_;
  Texture overlayTexture_;
  Settings settings_{};
  bool enabled_ = false;
  float opacity_ = 0.45f;

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
  double sdfSyncMs = 0.0;
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
  Operation sdfSync{};
  Operation softwareBlasSync{};
  Operation softwareTlasSync{};
  Operation vtFeedback{};
  Operation sceneStreaming{};
  Operation instanceUpload{};
  Operation statsReadback{};
  softwarebvh::SoftwareTLASManager::BuildSummary softwareTlasBuild{};
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

struct SDFFrameStats
{
  uint32_t brickCount = 0u;
  uint32_t dirtyBricks = 0u;
  uint32_t freeBricks = 0u;
  uint32_t clearBricks = 0u;
  uint32_t mergeBricks = 0u;
  uint32_t maxBricks = 0u;
};

struct ScreenProbeTraceStats
{
  uint32_t totalRays = 0u;
  uint32_t activeRays = 0u;
  uint32_t inactiveRays = 0u;
  uint32_t screenRawHits = 0u;
  uint32_t screenInvalidDirections = 0u;
  uint32_t sdfRawHits = 0u;
  uint32_t sdfInvalidDirections = 0u;
  uint32_t sdfHasCascade = 0u;
  uint32_t sdfHasValidBrick = 0u;
  uint32_t sdfHasRootAabb = 0u;
  uint32_t resolvedScreenHits = 0u;
  uint32_t resolvedSdfHits = 0u;
  uint32_t resolvedSky = 0u;
  uint32_t resolvedMisses = 0u;
  uint32_t screenInactiveRays = 0u;
  uint32_t sdfInactiveRays = 0u;
  uint32_t sdfHasUninitialized = 0u;
  uint32_t sdfHasTreeRoot = 0u;
  uint32_t sdfHasMidAabb = 0u;
  uint32_t sdfHasBottomAabb = 0u;
  uint32_t originFinite = 0u;
  uint32_t originInsideAnyCascade = 0u;
  uint32_t sdfInputActive = 0u;
  uint32_t sdfInputInsideAnyCascade = 0u;
  uint32_t sdfInputFiniteDirection = 0u;
  uint32_t sdfInputValidRange = 0u;
  uint32_t sdfOutputMetadataWritten = 0u;
  uint32_t sdfOutputMetadataMatch = 0u;
};

struct ScreenSpaceRayTraceStats
{
  uint32_t failedRays = 0u;
  uint32_t overflow = 0u;
  uint32_t capacity = 0u;
  uint32_t totalRays = 0u;
  uint32_t offscreenRays = 0u;
  float offscreenPercent = 0.0f;
};

struct ScreenSpaceRayTraceStatsReadback
{
  uint32_t failedRays = 0u;
  uint32_t overflow = 0u;
  uint32_t capacity = 0u;
  uint32_t totalRays = 0u;
  uint32_t offscreenRays = 0u;
  uint32_t padding[3] = {};
};
static_assert(sizeof(ScreenSpaceRayTraceStatsReadback) == 32u);

static constexpr uint32_t kScreenProbeTraceStatsCounterCount = 28u;

struct SurfelTraceStats
{
  uint32_t totalRays = 0u;
  uint32_t activeRays = 0u;
  uint32_t inactiveRays = 0u;
  uint32_t invalidDirectionRays = 0u;
  uint32_t hitRays = 0u;
  uint32_t missRays = 0u;
  uint32_t hasCascadeRays = 0u;
  uint32_t hasValidBrickRays = 0u;
  uint32_t hasUninitializedRays = 0u;
  uint32_t hasRootAabbRays = 0u;
  uint32_t hasTreeRootRays = 0u;
  uint32_t hasMidAabbRays = 0u;
  uint32_t hasBottomAabbRays = 0u;
  uint32_t failedCascadeRays = 0u;
  uint32_t failedRootRays = 0u;
  uint32_t failedMidRays = 0u;
  uint32_t failedBottomRays = 0u;
  uint32_t failedBrickRays = 0u;
  uint32_t failedUninitializedRays = 0u;
};

struct SoftwareRayTraceFrameStats
{
  uint32_t totalRays = 0u;
  uint32_t activeRays = 0u;
  uint32_t hitRays = 0u;
  uint32_t missRays = 0u;
  uint32_t invalidDirectionRays = 0u;
  uint32_t tlasNodeVisits = 0u;
  uint32_t blasNodeVisits = 0u;
  uint32_t worldBlasNodeVisits = 0u;
  uint32_t triangleTests = 0u;
  uint32_t workgroupQueuePushes = 0u;
  uint32_t workgroupQueuePops = 0u;
  uint32_t localStackSpills = 0u;
  uint32_t overlayNodeTests = 0u;
  uint32_t overlayEdgeHits = 0u;
  uint32_t lastHitPrimitive = UINT32_MAX;
  uint32_t lastHitInstance = UINT32_MAX;
  uint32_t lastHitValid = 0u;
  uint32_t traversalEarlyOutSkips = 0u;
  uint32_t debugPixelsReachedValidBLAS = 0u;
  uint32_t debugPixelsReachedInvalidBLAS = 0u;
  uint32_t debugPixelsVisitedBLASNode = 0u;
  uint32_t debugPixelsReadTriangle = 0u;
  uint32_t debugPixelsRejectedTriangle = 0u;
  uint32_t debugPixelsAcceptedTriangle = 0u;
  uint32_t debugPixelsRejectedWorldT = 0u;
  uint32_t debugPixelsAcceptedCandidate = 0u;
  uint32_t debugPixelsBLASLeafGroup = 0u;
  uint32_t debugPixelsInvalidLeafRemap = 0u;
  uint32_t debugPixelsInvalidLocalRay = 0u;
  uint32_t dynamicRayFetchCursor = 0u;
};

static constexpr uint32_t kSoftwareRayTraceStatsCounterCount = 30u;

struct SurfelIrradianceDebugStats
{
  uint32_t sampledSurfels = 0u;
  uint32_t nonZeroSurfels = 0u;
  uint32_t greenDominantSurfels = 0u;
  std::array<float, 3> averageRgb{0.0f, 0.0f, 0.0f};
  std::array<float, 3> maxRgb{0.0f, 0.0f, 0.0f};
  float maxWeight = 0.0f;
};

static ScreenProbeTraceStats decodeScreenProbeTraceStats(const std::vector<uint32_t> &counters)
{
  ScreenProbeTraceStats stats{};
  if (counters.size() < kScreenProbeTraceStatsCounterCount)
  {
    return stats;
  }

  stats.totalRays = counters[0];
  stats.activeRays = counters[1];
  stats.inactiveRays = counters[2];
  stats.screenRawHits = counters[3];
  stats.screenInvalidDirections = counters[4];
  stats.sdfRawHits = counters[5];
  stats.sdfInvalidDirections = counters[6];
  stats.sdfHasCascade = counters[7];
  stats.sdfHasValidBrick = counters[8];
  stats.sdfHasRootAabb = counters[9];
  stats.resolvedScreenHits = counters[10];
  stats.resolvedSdfHits = counters[11];
  stats.resolvedSky = counters[12];
  stats.resolvedMisses = counters[13];
  stats.screenInactiveRays = counters[14];
  stats.sdfInactiveRays = counters[15];
  stats.sdfHasUninitialized = counters[16];
  stats.sdfHasTreeRoot = counters[17];
  stats.sdfHasMidAabb = counters[18];
  stats.sdfHasBottomAabb = counters[19];
  stats.originFinite = counters[20];
  stats.originInsideAnyCascade = counters[21];
  stats.sdfInputActive = counters[22];
  stats.sdfInputInsideAnyCascade = counters[23];
  stats.sdfInputFiniteDirection = counters[24];
  stats.sdfInputValidRange = counters[25];
  stats.sdfOutputMetadataWritten = counters[26];
  stats.sdfOutputMetadataMatch = counters[27];
  return stats;
}

static ScreenSpaceRayTraceStats decodeScreenSpaceRayTraceStats(const ScreenSpaceRayTraceStatsReadback &readback)
{
  ScreenSpaceRayTraceStats stats{};
  stats.failedRays = readback.failedRays;
  stats.overflow = readback.overflow;
  stats.capacity = readback.capacity;
  stats.totalRays = readback.totalRays;
  stats.offscreenRays = readback.offscreenRays;
  if (stats.totalRays > 0u)
  {
    stats.offscreenPercent = 100.0f * static_cast<float>(stats.offscreenRays) / static_cast<float>(stats.totalRays);
  }
  return stats;
}

static uint32_t computeScreenSpaceRayTraceTotalRayBudget(uint32_t width, uint32_t height, uint32_t tileSize, uint32_t raysPerTile)
{
  const uint64_t safeTileSize = std::max<uint32_t>(tileSize, 1u);
  const uint64_t tilesX = (static_cast<uint64_t>(std::max(width, 1u)) + safeTileSize - 1u) / safeTileSize;
  const uint64_t tilesY = (static_cast<uint64_t>(std::max(height, 1u)) + safeTileSize - 1u) / safeTileSize;
  const uint64_t totalRays = tilesX * tilesY * static_cast<uint64_t>(std::max(raysPerTile, 1u));
  return static_cast<uint32_t>(std::min<uint64_t>(totalRays, std::numeric_limits<uint32_t>::max()));
}

static uint32_t effectiveScreenSpaceRayTraceFallbackBudget(
    bool traceAllForcedFallbackRays,
    uint32_t requestedBudget,
    uint32_t width,
    uint32_t height,
    uint32_t tileSize,
    uint32_t raysPerTile)
{
  if (!traceAllForcedFallbackRays)
    return std::max(1u, requestedBudget);
  return std::max(1u, computeScreenSpaceRayTraceTotalRayBudget(width, height, tileSize, raysPerTile));
}

static SurfelTraceStats decodeSurfelTraceStats(const std::vector<uint32_t> &counters)
{
  SurfelTraceStats stats{};
  if (counters.size() < gpusigneddistancefields::gpgpu::TraceStatsCounter_Count)
  {
    return stats;
  }

  using namespace gpusigneddistancefields::gpgpu;
  stats.totalRays = counters[TraceStatsCounter_TotalRays];
  stats.activeRays = counters[TraceStatsCounter_ActiveRays];
  stats.inactiveRays = counters[TraceStatsCounter_InactiveRays];
  stats.invalidDirectionRays = counters[TraceStatsCounter_InvalidDirectionRays];
  stats.hitRays = counters[TraceStatsCounter_HitRays];
  stats.missRays = counters[TraceStatsCounter_MissRays];
  stats.hasCascadeRays = counters[TraceStatsCounter_HasCascadeRays];
  stats.hasValidBrickRays = counters[TraceStatsCounter_HasValidBrickRays];
  stats.hasUninitializedRays = counters[TraceStatsCounter_HasUninitializedRays];
  stats.hasRootAabbRays = counters[TraceStatsCounter_HasRootAabbRays];
  stats.hasTreeRootRays = counters[TraceStatsCounter_HasTreeRootRays];
  stats.hasMidAabbRays = counters[TraceStatsCounter_HasMidAabbRays];
  stats.hasBottomAabbRays = counters[TraceStatsCounter_HasBottomAabbRays];
  stats.failedCascadeRays = counters[TraceStatsCounter_FailedCascadeRays];
  stats.failedRootRays = counters[TraceStatsCounter_FailedRootRays];
  stats.failedMidRays = counters[TraceStatsCounter_FailedMidRays];
  stats.failedBottomRays = counters[TraceStatsCounter_FailedBottomRays];
  stats.failedBrickRays = counters[TraceStatsCounter_FailedBrickRays];
  stats.failedUninitializedRays = counters[TraceStatsCounter_FailedUninitializedRays];
  return stats;
}

static SoftwareRayTraceFrameStats decodeSoftwareRayTraceStats(const std::vector<uint32_t> &counters)
{
  SoftwareRayTraceFrameStats stats{};
  if (counters.size() < kSoftwareRayTraceStatsCounterCount)
  {
    return stats;
  }

  stats.totalRays = counters[0];
  stats.activeRays = counters[1];
  stats.hitRays = counters[2];
  stats.missRays = counters[3];
  stats.invalidDirectionRays = counters[4];
  stats.tlasNodeVisits = counters[5];
  stats.blasNodeVisits = counters[6];
  stats.worldBlasNodeVisits = counters[7];
  stats.triangleTests = counters[8];
  stats.workgroupQueuePushes = counters[9];
  stats.workgroupQueuePops = counters[10];
  stats.localStackSpills = counters[11];
  stats.overlayNodeTests = counters[12];
  stats.overlayEdgeHits = counters[13];
  stats.lastHitPrimitive = counters[14];
  stats.lastHitInstance = counters[15];
  stats.lastHitValid = counters[16];
  stats.traversalEarlyOutSkips = counters[17];
  stats.debugPixelsReachedValidBLAS = counters[18];
  stats.debugPixelsReachedInvalidBLAS = counters[19];
  stats.debugPixelsVisitedBLASNode = counters[20];
  stats.debugPixelsReadTriangle = counters[21];
  stats.debugPixelsRejectedTriangle = counters[22];
  stats.debugPixelsAcceptedTriangle = counters[23];
  stats.debugPixelsRejectedWorldT = counters[24];
  stats.debugPixelsAcceptedCandidate = counters[25];
  stats.debugPixelsBLASLeafGroup = counters[26];
  stats.debugPixelsInvalidLeafRemap = counters[27];
  stats.debugPixelsInvalidLocalRay = counters[28];
  stats.dynamicRayFetchCursor = counters[29];
  return stats;
}

static SurfelIrradianceDebugStats summarizeSurfelIrradiance(const std::vector<globalillumination::surfels::SurfelIrradianceGPU> &samples)
{
  SurfelIrradianceDebugStats stats{};
  stats.sampledSurfels = static_cast<uint32_t>(samples.size());
  std::array<double, 3> sumRgb{0.0, 0.0, 0.0};
  for (const auto &sample : samples)
  {
    const float r = std::max(sample.irradianceWeight[0], 0.0f);
    const float g = std::max(sample.irradianceWeight[1], 0.0f);
    const float b = std::max(sample.irradianceWeight[2], 0.0f);
    const float w = std::max(sample.irradianceWeight[3], 0.0f);
    if (r > 1.0e-5f || g > 1.0e-5f || b > 1.0e-5f)
    {
      ++stats.nonZeroSurfels;
      sumRgb[0] += r;
      sumRgb[1] += g;
      sumRgb[2] += b;
      if (g > r && g > b)
        ++stats.greenDominantSurfels;
    }
    stats.maxRgb[0] = std::max(stats.maxRgb[0], r);
    stats.maxRgb[1] = std::max(stats.maxRgb[1], g);
    stats.maxRgb[2] = std::max(stats.maxRgb[2], b);
    stats.maxWeight = std::max(stats.maxWeight, w);
  }
  if (stats.nonZeroSurfels > 0u)
  {
    const double invCount = 1.0 / static_cast<double>(stats.nonZeroSurfels);
    stats.averageRgb[0] = static_cast<float>(sumRgb[0] * invCount);
    stats.averageRgb[1] = static_cast<float>(sumRgb[1] * invCount);
    stats.averageRgb[2] = static_cast<float>(sumRgb[2] * invCount);
  }
  return stats;
}

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

struct FallbackSceneFrameDiagnostics
{
  bool capture = false;
  bool preparePassEnabled = false;
  bool frustumCullingEnabled = true;
  uint32_t staticDrawCount = 0u;
  uint32_t skinnedDrawCount = 0u;
  uint32_t uploadedVertexCount = 0u;
  uint32_t uploadedIndexCount = 0u;
  uint32_t uploadedBoneWeightCount = 0u;
  std::array<math::Vec4f, 6> frustumPlanes{};
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager::Diagnostics bridgeDiagnostics{};
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::MeshTableEntryGPU> meshTable{};
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> staticInstances{};
  std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> skinnedInstances{};
  std::vector<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass::DrawIndirectCommandGPU> preparedStaticCommands{};
  std::vector<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass::DrawIndirectCommandGPU> preparedSkinnedCommands{};
};

struct PendingFrameDiagnostics
{
  RenderGraph::Frame frame{};
  uint32_t displayFrameIndex = 0u;
  float frameDeltaMs = 0.0f;
  bool timerResolveSubmitted = false;
  bool collectFrameMetrics = true;
  CpuFrameReport cpuFrame{};
  RenderGraph::CpuStats renderGraphCpuStart{};
  double renderGraphRunTotalMs = 0.0;
  std::vector<RenderGraph::RunPhaseRecord> renderGraphRunPhases{};
  std::vector<RenderGraph::RuntimeMetricRecord> renderGraphRuntimeMetrics{};
  RenderGraph::RunDebugStats renderGraphRunDebugStats{};
  FallbackSceneFrameDiagnostics fallbackScene{};
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

struct VirtualTextureFrameStats
{
  VirtualTextureSystem::FeedbackStats feedback{};
  uint32_t pageSize = 0u;
  uint32_t physicalPagesPerAxis = 0u;
  uint32_t totalPhysicalPages = 0u;
  uint32_t residentPages = 0u;
  uint32_t textureCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t pageTableEntryCount = 0u;
  bool hasDirtyResidency = false;
};

static VirtualTextureFrameStats buildVirtualTextureFrameStats(const VirtualTextureSystem &virtualTextures, const MaterialPass *materialPass)
{
  VirtualTextureFrameStats stats{};
  if (materialPass != nullptr)
    stats.feedback = materialPass->getLastFeedbackStats();

  const VirtualTextureSystem::AtlasInfoGPU &atlasInfo = virtualTextures.getAtlasInfo();
  stats.pageSize = atlasInfo.pageSize;
  stats.physicalPagesPerAxis = atlasInfo.physicalPagesPerAxis;
  stats.totalPhysicalPages = atlasInfo.totalPhysicalPages;
  stats.textureCount = atlasInfo.textureCount;
  stats.materialCount = atlasInfo.materialCount;
  stats.pageTableEntryCount = atlasInfo.pageTableEntryCount;
  stats.hasDirtyResidency = virtualTextures.hasDirtyResidency();

  const auto &pageTableEntries = virtualTextures.getPageTableEntries();
  for (const VirtualTextureSystem::PageTableEntryGPU &entry : pageTableEntries)
  {
    if ((entry.flags & VirtualTextureSystem::PageTableFlag_Resident) != 0u && entry.physicalPage != VirtualTextureSystem::InvalidPhysicalPage)
      ++stats.residentPages;
  }
  return stats;
}

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
    std::array<uint32_t, kSampleCount> timerCounts{};
    float latestMs = 0.0f;
    uint32_t latestTimerCount = 0u;
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
  };

  std::vector<Series> series;
  std::unordered_map<std::string, size_t> seriesIndices;
  std::array<uint32_t, kSampleCount> frameIndices{};
  size_t nextSampleIndex = 0u;
  size_t count = 0u;
  uint32_t lastFrameIndex = 0u;
  bool hasFrames = false;
  std::vector<size_t> computeSeriesOrder;
  std::vector<size_t> graphicsSeriesOrder;

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
    frameIndices[sampleIndex] = frameIndex;

    for (Series &entry : series)
    {
      entry.values[sampleIndex] = 0.0f;
      entry.timerCounts[sampleIndex] = 0u;
      entry.latestMs = 0.0f;
      entry.latestTimerCount = 0u;
    }

    for (const PassTimingSummary &pass : passTimings)
    {
      Series &entry = ensureSeries(pass);
      entry.values[sampleIndex] = static_cast<float>(pass.gpuTimeMs);
      entry.timerCounts[sampleIndex] = pass.timerCount;
      entry.latestMs = static_cast<float>(pass.gpuTimeMs);
      entry.latestTimerCount = pass.timerCount;
    }

    nextSampleIndex = (nextSampleIndex + 1u) % kSampleCount;
    count = std::min(count + 1u, kSampleCount);
    rebuildCachedViews();
  }

  void rebuildCachedViews()
  {
    computeSeriesOrder.clear();
    graphicsSeriesOrder.clear();

    computeSeriesOrder.reserve(series.size());
    graphicsSeriesOrder.reserve(series.size());
    for (size_t seriesIndex = 0u; seriesIndex < series.size(); ++seriesIndex)
    {
      const Series &entry = series[seriesIndex];
      if (entry.hasComputeWork)
      {
        computeSeriesOrder.push_back(seriesIndex);
      }

      if (entry.hasGraphicsWork)
      {
        graphicsSeriesOrder.push_back(seriesIndex);
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

    std::sort(computeSeriesOrder.begin(), computeSeriesOrder.end(), byPassIndex);
    std::sort(graphicsSeriesOrder.begin(), graphicsSeriesOrder.end(), byPassIndex);
  }

  size_t getOrderedStorageIndex(size_t orderedSampleIndex) const
  {
    return (nextSampleIndex + kSampleCount - count + orderedSampleIndex) % kSampleCount;
  }

  void buildOrderedValues(const Series &entry, std::array<float, kSampleCount> &ordered) const
  {
    ordered.fill(0.0f);
    if (count == 0u)
    {
      return;
    }

    for (size_t i = 0u; i < count; ++i)
    {
      ordered[i] = entry.values[getOrderedStorageIndex(i)];
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

  float getSampleValue(const Series &entry, size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0.0f;
    }
    return entry.values[getOrderedStorageIndex(orderedSampleIndex)];
  }

  uint32_t getSampleTimerCount(const Series &entry, size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0u;
    }
    return entry.timerCounts[getOrderedStorageIndex(orderedSampleIndex)];
  }

  float getSampleTotalMs(Filter filter, size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0.0f;
    }

    float totalMs = 0.0f;
    for (const Series &entry : series)
    {
      if (matchesFilter(entry, filter))
      {
        totalMs += getSampleValue(entry, orderedSampleIndex);
      }
    }
    return totalMs;
  }

  uint32_t getFrameIndexAtOrderedSample(size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0u;
    }
    return frameIndices[getOrderedStorageIndex(orderedSampleIndex)];
  }

  std::vector<const Series *> getSortedSeries(Filter filter) const
  {
    const std::vector<size_t> &sortedIndices = filter == Filter::Compute ? computeSeriesOrder : graphicsSeriesOrder;
    std::vector<const Series *> ordered;
    ordered.reserve(sortedIndices.size());
    for (size_t seriesIndex : sortedIndices)
    {
      ordered.push_back(&series[seriesIndex]);
    }
    return ordered;
  }

  std::vector<const Series *> getSeriesSortedBySample(Filter filter, size_t orderedSampleIndex) const
  {
    std::vector<const Series *> ordered = getSortedSeries(filter);
    std::sort(
        ordered.begin(),
        ordered.end(),
        [&](const Series *a, const Series *b)
        {
          const float aValue = getSampleValue(*a, orderedSampleIndex);
          const float bValue = getSampleValue(*b, orderedSampleIndex);
          if (aValue != bValue)
          {
            return aValue < bValue;
          }
          if (a->passIndex != b->passIndex)
          {
            return a->passIndex < b->passIndex;
          }
          return a->name < b->name;
        });
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

  size_t getOrderedStorageIndex(size_t orderedSampleIndex) const
  {
    return (nextSampleIndex + kSampleCount - count + orderedSampleIndex) % kSampleCount;
  }

  float getSampleValue(const Series &entry, size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0.0f;
    }
    return entry.values[getOrderedStorageIndex(orderedSampleIndex)];
  }

  float getSampleTotalMs(size_t orderedSampleIndex) const
  {
    if (orderedSampleIndex >= count)
    {
      return 0.0f;
    }

    float totalMs = 0.0f;
    for (const Series &entry : series)
    {
      totalMs += getSampleValue(entry, orderedSampleIndex);
    }
    return totalMs;
  }

  float getVisibleMaxTotalMs() const
  {
    float maxMs = 0.0f;
    for (size_t orderedSampleIndex = 0u; orderedSampleIndex < count; ++orderedSampleIndex)
    {
      maxMs = std::max(maxMs, getSampleTotalMs(orderedSampleIndex));
    }
    return maxMs;
  }

  std::vector<const Series *> getSeriesSortedBySample(size_t orderedSampleIndex) const
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
            return a->latestMs < b->latestMs;
          }
          if (a->orderIndex != b->orderIndex)
          {
            return a->orderIndex < b->orderIndex;
          }

          return a->name < b->name;
        });
    if (orderedSampleIndex + 1u != count)
    {
      std::sort(
          ordered.begin(),
          ordered.end(),
          [&](const Series *a, const Series *b)
          {
            const float aValue = getSampleValue(*a, orderedSampleIndex);
            const float bValue = getSampleValue(*b, orderedSampleIndex);
            if (aValue != bValue)
            {
              return aValue < bValue;
            }
            if (a->orderIndex != b->orderIndex)
            {
              return a->orderIndex < b->orderIndex;
            }
            return a->name < b->name;
          });
    }
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
  sections.reserve(48u);

  auto appendSection = [&](std::string name, double ms)
  {
    sections.push_back(
        CpuSectionTimingSummary{
          .name = std::move(name),
          .orderIndex = static_cast<uint32_t>(sections.size()),
          .ms = ms,
        });
  };

  auto transferStatsMs = [](const RenderGraph::CpuTransferStats &stats)
  {
    return static_cast<double>(stats.totalNs) / 1e6;
  };

  auto appendOperationSections = [&](const char *name, const CpuFrameReport::Operation &operation)
  {
    const double writeMs = transferStatsMs(operation.cpu.bufferWrites);
    const double readMs = transferStatsMs(operation.cpu.bufferReads);
    appendSection(std::string(name) + " CPU", std::max(0.0, operation.wallMs - writeMs - readMs));
    appendSection(std::string(name) + " Writes", writeMs);
    appendSection(std::string(name) + " Reads", readMs);
  };

  appendSection("Window/Input", cpuFrame.windowUpdateMs);
  appendSection("Simulation", cpuFrame.simulationMs);
  appendSection("Camera Build", cpuFrame.cameraBuildMs);
  appendSection("Animation", cpuFrame.animationMs);
  appendOperationSections("SDF Sync", cpuFrame.sdfSync);
  appendOperationSections("SWRT BLAS Sync", cpuFrame.softwareBlasSync);
  appendOperationSections("SWRT TLAS Sync", cpuFrame.softwareTlasSync);
  appendOperationSections("Uniform Upload", cpuFrame.uniformUpload);
  appendSection("Swap Acquire", cpuFrame.acquireMs);
  appendSection("RenderGraph Run", cpuFrame.renderRunMs);
  appendSection("RG Wait Block", cpuFrame.waitBlockMs);
  appendSection("Timer Readback", cpuFrame.timerReadbackMs);
  appendOperationSections("VT Feedback", cpuFrame.vtFeedback);
  appendOperationSections("VT Streaming", cpuFrame.sceneStreaming);
  appendSection("VSM Reset", cpuFrame.vsmResetInvalidationsMs);
  appendOperationSections("Instance Upload", cpuFrame.instanceUpload);
  appendOperationSections("Stats Readback", cpuFrame.statsReadback);
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

static const char *passTimingStageLabel(bool hasComputeWork, bool hasGraphicsWork)
{
  if (hasComputeWork && hasGraphicsWork)
  {
    return "mixed";
  }
  if (hasComputeWork)
  {
    return "compute";
  }
  if (hasGraphicsWork)
  {
    return "graphics";
  }
  return "none";
}

static const char *passTimingStatusLabel(float timeMs, uint32_t timerCount)
{
  if (timerCount == 0u)
  {
    return "missing";
  }
  if (timeMs <= 0.0f)
  {
    return "zero";
  }
  return "active";
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
  rendering::Buffer sdfContextCountersBuffer{};
  uint32_t sdfMaxBricks = 0u;
  bool collectSurfelTraceStats = false;
  rendering::BufferId surfelTraceStatsBufferId = rendering::BufferId::Invalid;
  bool collectSoftwareRayTraceStats = false;
  rendering::BufferId softwareRayTraceStatsBufferId = rendering::BufferId::Invalid;
  bool collectScreenFallbackTraceStats = false;
  rendering::BufferId screenFallbackTraceStatsBufferId = rendering::BufferId::Invalid;
};

struct PreparedOverlayState
{
  uint32_t frameIndex = 0u;
  float deltaTimeMs = 0.0f;
  CpuFrameReport cpuFrame{};
  RenderGraph::CpuStats renderGraphCpu{};
  VSMFrameStats vsmStats{};
  SDFFrameStats sdfStats{};
  SurfelTraceStats surfelTraceStats{};
  SoftwareRayTraceFrameStats softwareRayTraceStats{};
  SoftwareRayTraceFrameStats screenFallbackTraceStats{};
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
  explicit OverlayPreparationWorker(RenderGraph *renderGraph) : renderGraph_(renderGraph), rhi_(renderGraph != nullptr ? renderGraph->getRHI() : nullptr)
  {
    // Start the worker only after every member has finished construction.
    workerThread_ = std::thread(
        [this]()
        {
          run();
        });
  }

  OverlayPreparationWorker(const OverlayPreparationWorker &) = delete;
  OverlayPreparationWorker &operator=(const OverlayPreparationWorker &) = delete;

  ~OverlayPreparationWorker()
  {
    stop();
  }

  void enqueue(OverlayPreparationInput input)
  {
    try
    {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingInputs_.push_back(std::move(input));
      }
      condition_.notify_one();
    }
    catch (const std::system_error &error)
    {
      std::fprintf(stderr, "[OverlayWorker][Mutex] op=enqueue stage=%s mutex=%p error=%s\n", currentStage_, static_cast<void *>(&mutex_), error.what());
      std::fflush(stderr);
      throw;
    }
  }

  std::shared_ptr<const PreparedOverlayState> getLatestSnapshot() const
  {
    try
    {
      std::lock_guard<std::mutex> lock(mutex_);
      return latestSnapshot_;
    }
    catch (const std::system_error &error)
    {
      std::fprintf(stderr, "[OverlayWorker][Mutex] op=getLatestSnapshot stage=%s mutex=%p error=%s\n", currentStage_, static_cast<void *>(&mutex_), error.what());
      std::fflush(stderr);
      throw;
    }
  }

  void stop()
  {
    try
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
    catch (const std::system_error &error)
    {
      std::fprintf(stderr, "[OverlayWorker][Mutex] op=stop stage=%s mutex=%p error=%s\n", currentStage_, static_cast<void *>(&mutex_), error.what());
      std::fflush(stderr);
      throw;
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
            result = readBufferElements<uint32_t>(rhi_, input.vsmAllocatorCountersBufferId, kAllocatorCounterCount);
          });
      return result;
    }();

    const std::vector<uint32_t> drawCounters = [&]()
    {
      std::vector<uint32_t> result;
      cpuFrame.statsDrawCountersReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(rhi_, input.vsmDrawCountersBufferId, 8u);
          });
      return result;
    }();

    const uint32_t regularRenderPages = allocatorCounters.size() > kAllocatorCounterCappedRequests ? std::min(allocatorCounters[kAllocatorCounterCappedRequests], input.allocationRequestCapacity) : 0u;
    const uint32_t fallbackRenderPages = allocatorCounters.size() > kAllocatorCounterFallbackRequests ? std::min(allocatorCounters[kAllocatorCounterFallbackRequests], input.futureAllocationRequestCapacity) : 0u;
    const uint32_t totalDrawCommands = drawCounters.size() > kDrawCounterVisibleClusterCount ? drawCounters[kDrawCounterVisibleClusterCount] : 0u;
    const uint32_t executedDrawCommands = std::min(totalDrawCommands, input.visibleClusterDrawCapacity);

    vsmStats.normalRenderPages = regularRenderPages;
    vsmStats.normalRenderPageBudget = input.allocationRequestCapacity;
    vsmStats.fallbackRenderPages = fallbackRenderPages;
    vsmStats.renderPages = regularRenderPages + fallbackRenderPages;
    vsmStats.totalDrawCommands = totalDrawCommands;
    vsmStats.executedDrawCommands = executedDrawCommands;
    vsmStats.drawCommandOverflow = drawCounters.size() > kDrawCounterShadowDrawOverflow ? drawCounters[kDrawCounterShadowDrawOverflow] : 0u;
    vsmStats.hierarchyQueueSize = drawCounters.size() > kDrawCounterHierarchyQueueSize ? drawCounters[kDrawCounterHierarchyQueueSize] : 0u;
    vsmStats.clusterQueueSize = drawCounters.size() > kDrawCounterClusterQueueSize ? drawCounters[kDrawCounterClusterQueueSize] : 0u;
    vsmStats.readQueueSize = drawCounters.size() > kDrawCounterReadQueueSize ? drawCounters[kDrawCounterReadQueueSize] : 0u;

    vsmStats.freePhysicalPages = allocatorCounters.size() > kAllocatorCounterFreePageCount ? allocatorCounters[kAllocatorCounterFreePageCount] : 0u;
    vsmStats.totalPhysicalPages = input.physicalPageTableResolution * input.physicalPageTableResolution;
    vsmStats.usedPhysicalPages = vsmStats.totalPhysicalPages - std::min(vsmStats.freePhysicalPages, vsmStats.totalPhysicalPages);

    const std::vector<uint32_t> vsmPageTable = [&]()
    {
      std::vector<uint32_t> result;
      cpuFrame.statsPageTableReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(rhi_, input.vsmVirtualPageTableBufferId, input.activeVSMPageCount);
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
              result = readBufferElements<uint32_t>(rhi_, input.vsmPageClusterCountsBufferId, input.activeVSMPageCount);
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
              vsmStats.averageClustersPerDrawnPage = static_cast<double>(totalClustersAcrossDrawnPages) / static_cast<double>(vsmStats.uniqueDrawnPages);
            }
          });

      const std::vector<uint32_t> drawIndirectWords = [&]()
      {
        std::vector<uint32_t> result;
        cpuFrame.statsDrawIndirectReadMs = measureWallTimeMs(
            [&]()
            {
              result = readBufferElements<uint32_t>(rhi_, input.vsmDrawIndirectBufferId, executedDrawCommands * 4u);
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

    cpuFrame.statsDumpMs = cpuFrame.statsAllocatorReadMs + cpuFrame.statsDrawCountersReadMs + cpuFrame.statsPageTableReadMs + cpuFrame.statsPageTableProcessMs + cpuFrame.statsVisibleClustersReadMs +
                           cpuFrame.statsVisibleClustersProcessMs + cpuFrame.statsDrawIndirectReadMs + cpuFrame.statsDrawIndirectProcessMs;
    cpuFrame.statsReadback.wallMs = cpuFrame.statsDumpMs;
    auto recordStatsRead = [&](double wallMs, uint64_t byteCount)
    {
      if (wallMs <= 0.0 && byteCount == 0u)
      {
        return;
      }

      RenderGraph::CpuTransferStats &reads = cpuFrame.statsReadback.cpu.bufferReads;
      const uint64_t totalNs = static_cast<uint64_t>(std::max(0.0, wallMs) * 1e6);
      reads.callCount += 1u;
      reads.totalBytes += byteCount;
      reads.totalNs += totalNs;
      reads.maxNs = std::max(reads.maxNs, totalNs);
    };
    recordStatsRead(cpuFrame.statsAllocatorReadMs, static_cast<uint64_t>(kAllocatorCounterCount) * sizeof(uint32_t));
    recordStatsRead(cpuFrame.statsDrawCountersReadMs, 8u * sizeof(uint32_t));
    recordStatsRead(cpuFrame.statsPageTableReadMs, static_cast<uint64_t>(input.activeVSMPageCount) * sizeof(uint32_t));
    if (executedDrawCommands > 0u)
    {
      recordStatsRead(cpuFrame.statsVisibleClustersReadMs, static_cast<uint64_t>(input.activeVSMPageCount) * sizeof(uint32_t));
      recordStatsRead(cpuFrame.statsDrawIndirectReadMs, static_cast<uint64_t>(executedDrawCommands) * 4u * sizeof(uint32_t));
    }
    return vsmStats;
  }

  SDFFrameStats buildSdfStats(const OverlayPreparationInput &input) const
  {
    SDFFrameStats stats{};
    stats.maxBricks = input.sdfMaxBricks;
    if (!input.sdfContextCountersBuffer.isValid() || renderGraph_ == nullptr)
      return stats;

    static constexpr uint32_t kContextCounterCount = 6u;
    const std::vector<uint32_t> counters = readBufferElements<uint32_t>(renderGraph_, input.sdfContextCountersBuffer, kContextCounterCount);
    if (counters.size() >= kContextCounterCount)
    {
      stats.brickCount = counters[0];  // ContextCounter_BrickCount
      stats.dirtyBricks = counters[1]; // ContextCounter_DirtyBricks
      stats.freeBricks = counters[2];  // ContextCounter_FreeBricks
      stats.clearBricks = counters[3]; // ContextCounter_ClearBricks
      stats.mergeBricks = counters[4]; // ContextCounter_MergeBricks
    }
    return stats;
  }

  SurfelTraceStats buildSurfelTraceStats(CpuFrameReport &cpuFrame, const OverlayPreparationInput &input) const
  {
    SurfelTraceStats stats{};
    if (!input.collectSurfelTraceStats || rhi_ == nullptr || input.surfelTraceStatsBufferId == rendering::BufferId::Invalid)
    {
      return stats;
    }

    double surfelReadMs = 0.0;
    const std::vector<uint32_t> counters = [&]()
    {
      std::vector<uint32_t> result;
      surfelReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(rhi_, input.surfelTraceStatsBufferId, gpusigneddistancefields::gpgpu::TraceStatsCounter_Count);
          });
      cpuFrame.statsReadback.wallMs += surfelReadMs;
      return result;
    }();

    RenderGraph::CpuTransferStats &reads = cpuFrame.statsReadback.cpu.bufferReads;
    const uint64_t totalNs = static_cast<uint64_t>(std::max(0.0, surfelReadMs) * 1e6);
    reads.callCount += 1u;
    reads.totalBytes += gpusigneddistancefields::gpgpu::getTraceStatsBufferSize();
    reads.totalNs += totalNs;
    reads.maxNs = std::max(reads.maxNs, totalNs);

    stats = decodeSurfelTraceStats(counters);
    return stats;
  }

  SoftwareRayTraceFrameStats buildSoftwareRayTraceStats(CpuFrameReport &cpuFrame, const OverlayPreparationInput &input) const
  {
    SoftwareRayTraceFrameStats stats{};
    if (!input.collectSoftwareRayTraceStats || rhi_ == nullptr || input.softwareRayTraceStatsBufferId == rendering::BufferId::Invalid)
    {
      return stats;
    }

    double swrtReadMs = 0.0;
    const std::vector<uint32_t> counters = [&]()
    {
      std::vector<uint32_t> result;
      swrtReadMs = measureWallTimeMs(
          [&]()
          {
            result = readBufferElements<uint32_t>(rhi_, input.softwareRayTraceStatsBufferId, kSoftwareRayTraceStatsCounterCount);
          });
      return result;
    }();

    cpuFrame.statsReadback.wallMs += swrtReadMs;
    RenderGraph::CpuTransferStats &reads = cpuFrame.statsReadback.cpu.bufferReads;
    const uint64_t totalNs = static_cast<uint64_t>(std::max(0.0, swrtReadMs) * 1e6);
    reads.callCount += 1u;
    reads.totalBytes += sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU);
    reads.totalNs += totalNs;
    reads.maxNs = std::max(reads.maxNs, totalNs);

    stats = decodeSoftwareRayTraceStats(counters);
    return stats;
  }

  void run()
  {
    currentStage_ = "thread-start";
    try
    {
      for (;;)
      {
        OverlayPreparationInput input{};
        {
          currentStage_ = "wait-for-input";
          std::unique_lock<std::mutex> lock(mutex_);
          condition_.wait(
              lock,
              [&]()
              {
                return stopRequested_ || !pendingInputs_.empty();
              });

          if (stopRequested_ && pendingInputs_.empty())
          {
            currentStage_ = "stop-complete";
            break;
          }

          currentStage_ = "pop-input";
          input = std::move(pendingInputs_.front());
          pendingInputs_.pop_front();
        }

        currentStage_ = "prepare-overlay";
        PreparedOverlayState prepared{};
        prepared.frameIndex = input.frameIndex;
        prepared.cpuFrame = std::move(input.cpuFrame);
        prepared.renderGraphCpu = input.renderGraphCpu;
        prepared.vsmStats = buildVsmStats(prepared.cpuFrame, input);
        prepared.sdfStats = buildSdfStats(input);
        const auto timerReadbackStart = lib::time::TimeSpan::now();
        currentStage_ = "read-timers";
        const std::vector<TimerReadbackRecord> timerRecords = (input.collectTimers && renderGraph_ != nullptr) ? renderGraph_->readResolvedTimerRecords(input.timerFrameSlot) : std::vector<TimerReadbackRecord>{};
        prepared.cpuFrame.timerReadbackMs = input.collectTimers ? (lib::time::TimeSpan::now() - timerReadbackStart).milliseconds() : 0.0;
        prepared.passTimings = buildPassTimingSummaries(timerRecords);
        prepared.surfelTraceStats = buildSurfelTraceStats(prepared.cpuFrame, input);
        prepared.softwareRayTraceStats = buildSoftwareRayTraceStats(prepared.cpuFrame, input);
        OverlayPreparationInput screenFallbackInput = input;
        screenFallbackInput.collectSoftwareRayTraceStats = input.collectScreenFallbackTraceStats;
        screenFallbackInput.softwareRayTraceStatsBufferId = input.screenFallbackTraceStatsBufferId;
        prepared.screenFallbackTraceStats = buildSoftwareRayTraceStats(prepared.cpuFrame, screenFallbackInput);
        prepared.cpuFrame.totalMs = computeTrackedCpuFrameMs(prepared.cpuFrame);
        prepared.cpuFrame.untrackedMs = 0.0;
        prepared.deltaTimeMs = static_cast<float>(prepared.cpuFrame.totalMs);
        prepared.currentCpuSections = buildSortedCpuSectionSummaries(prepared.cpuFrame);
        prepared.renderGraphRunTotalMs = input.renderGraphRunTotalMs;
        prepared.renderGraphRunPhases = std::move(input.renderGraphRunPhases);
        prepared.renderGraphRuntimeMetrics = std::move(input.renderGraphRuntimeMetrics);
        prepared.renderGraphRunDebugStats = input.renderGraphRunDebugStats;
        prepared.frameWaitSummary = input.frameWaitSummary;

        currentStage_ = "build-history";
        if (prepared.deltaTimeMs > 0.0f)
        {
          overlayHistory_.push(prepared.deltaTimeMs);
        }
        passTimelineHistory_.pushFrame(prepared.frameIndex, prepared.passTimings);
        cpuTimelineHistory_.pushFrame(prepared.frameIndex, buildCpuSectionSummaries(prepared.cpuFrame));
        prepared.overlayHistory = overlayHistory_;
        prepared.passTimelineHistory = passTimelineHistory_;
        prepared.cpuTimelineHistory = cpuTimelineHistory_;

        currentStage_ = "publish-snapshot";
        std::shared_ptr<const PreparedOverlayState> snapshot = std::make_shared<PreparedOverlayState>(std::move(prepared));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          latestSnapshot_ = std::move(snapshot);
        }
      }
    }
    catch (const std::system_error &error)
    {
      std::fprintf(stderr, "[OverlayWorker][Mutex] op=thread stage=%s mutex=%p error=%s\n", currentStage_, static_cast<void *>(&mutex_), error.what());
      std::fflush(stderr);
      throw;
    }
    catch (const std::exception &error)
    {
      os::Logger::errorf("[OverlayWorker] thread failed stage=%s error=%s", currentStage_, error.what());
      throw;
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
  const char *currentStage_ = "constructed";
};

static double computeTrackedCpuFrameMs(const CpuFrameReport &cpuFrame)
{
  return cpuFrame.windowUpdateMs + cpuFrame.simulationMs + cpuFrame.cameraBuildMs + cpuFrame.animationMs + cpuFrame.sdfSync.wallMs + cpuFrame.softwareBlasSync.wallMs + cpuFrame.softwareTlasSync.wallMs + cpuFrame.uniformUpload.wallMs + cpuFrame.acquireMs + cpuFrame.renderRunMs +
         cpuFrame.waitBlockMs + cpuFrame.timerReadbackMs + cpuFrame.vtFeedback.wallMs + cpuFrame.sceneStreaming.wallMs + cpuFrame.vsmResetInvalidationsMs + cpuFrame.instanceUpload.wallMs + cpuFrame.statsReadback.wallMs +
         cpuFrame.uiBuildMs + cpuFrame.presentMs;
}

static void drawPassTimelineGraph(
    const char *canvasId,
    const char *emptyLabel,
    const PassTimelineHistory &timelineHistory,
    PassTimelineHistory::Filter filter,
    size_t visibleFrameCount,
    size_t selectedFrameOffsetFromLatest,
    float labelGutterWidth,
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
  const float rightPad = std::max(520.0f, labelGutterWidth);
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

  if (timelineHistory.count == 0u)
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), emptyLabel);
    return;
  }

  const size_t visibleCount = std::min(timelineHistory.count, std::max<size_t>(1u, visibleFrameCount));
  const size_t firstOrderedSample = timelineHistory.count > visibleCount ? (timelineHistory.count - visibleCount) : 0u;
  const size_t selectedOffset = std::min(selectedFrameOffsetFromLatest, visibleCount - 1u);
  const size_t selectedLocalSample = visibleCount - 1u - selectedOffset;
  const size_t selectedOrderedSample = firstOrderedSample + selectedLocalSample;
  float visibleMaxMs = 0.0f;
  for (size_t localSampleIndex = 0u; localSampleIndex < visibleCount; ++localSampleIndex)
  {
    visibleMaxMs = std::max(visibleMaxMs, timelineHistory.getSampleTotalMs(filter, firstOrderedSample + localSampleIndex));
  }
  const float maxMs = std::max(1.0f, visibleMaxMs * 1.1f);
  for (int gridIndex = 0; gridIndex < 5; ++gridIndex)
  {
    const float t = static_cast<float>(gridIndex) / 4.0f;
    const float y = plotMax.y + (plotMin.y - plotMax.y) * t;
    drawList->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), IM_COL32(60, 68, 80, 200), 1.0f);

    char label[32];
    std::snprintf(label, sizeof(label), "%.2f ms", maxMs * t);
    drawList->AddText(ImVec2(outerMin.x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 196, 205, 255), label);
  }

  drawList->AddText(ImVec2(plotMin.x, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "oldest visible");
  drawList->AddText(ImVec2(plotMax.x - 42.0f, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "newest");

  const std::vector<const PassTimelineHistory::Series *> orderedSeries = timelineHistory.getSeriesSortedBySample(filter, selectedOrderedSample);
  if (orderedSeries.empty())
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), emptyLabel);
    return;
  }

  const float selectedX = visibleCount > 1u ? plotMin.x + (plotMax.x - plotMin.x) * (static_cast<float>(selectedLocalSample) / static_cast<float>(visibleCount - 1u)) : plotMin.x;
  drawList->AddLine(ImVec2(selectedX, plotMin.y), ImVec2(selectedX, plotMax.y), IM_COL32(230, 230, 235, 130), 1.0f);
  char selectedLabel[48];
  std::snprintf(selectedLabel, sizeof(selectedLabel), "N-%zu", selectedOffset);
  drawList->AddText(ImVec2(std::min(selectedX + 4.0f, plotMax.x - 32.0f), plotMin.y + 4.0f), IM_COL32(230, 230, 235, 210), selectedLabel);

  struct PassLabel
  {
    float y = 0.0f;
    ImU32 color = IM_COL32_WHITE;
    std::string text;
  };
  std::vector<PassLabel> labels;
  labels.reserve(orderedSeries.size());

  std::array<float, PassTimelineHistory::kSampleCount> cumulativeValues{};
  drawList->PushClipRect(plotMin, plotMax, true);
  for (const PassTimelineHistory::Series *entry : orderedSeries)
  {
    bool hasNonZeroSample = false;
    const float selectedValue = timelineHistory.getSampleValue(*entry, selectedOrderedSample);
    for (size_t localSampleIndex = 0u; localSampleIndex < visibleCount; ++localSampleIndex)
    {
      const float value = timelineHistory.getSampleValue(*entry, firstOrderedSample + localSampleIndex);
      cumulativeValues[localSampleIndex] += value;
      if (value > 0.0f)
      {
        hasNonZeroSample = true;
      }
    }
    if (!hasNonZeroSample)
    {
      continue;
    }

    ImVec2 previousPoint{};
    bool hasPreviousPoint = false;
    const size_t pointCount = std::max<size_t>(visibleCount, 1u);
    for (size_t localSampleIndex = 0u; localSampleIndex < visibleCount; ++localSampleIndex)
    {
      const float normalizedX = pointCount > 1u ? (static_cast<float>(localSampleIndex) / static_cast<float>(pointCount - 1u)) : 0.0f;
      const float normalizedY = std::clamp(cumulativeValues[localSampleIndex] / maxMs, 0.0f, 1.0f);
      const ImVec2 point(plotMin.x + (plotMax.x - plotMin.x) * normalizedX, plotMax.y + (plotMin.y - plotMax.y) * normalizedY);

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

    if (selectedValue > 0.0f)
    {
      const float selectedCumulativeMs = cumulativeValues[selectedLocalSample];
      const float normalizedY = std::clamp(selectedCumulativeMs / maxMs, 0.0f, 1.0f);
      char labelText[256];
      std::snprintf(labelText, sizeof(labelText), "%.2f ms %s", selectedValue, entry->name.c_str());
      labels.push_back(
          PassLabel{
            .y = plotMax.y + (plotMin.y - plotMax.y) * normalizedY,
            .color = entry->color,
            .text = labelText,
          });
    }
  }
  drawList->PopClipRect();

  std::sort(
      labels.begin(),
      labels.end(),
      [](const PassLabel &a, const PassLabel &b)
      {
        return a.y < b.y;
      });
  const float textHeight = ImGui::GetTextLineHeight();
  float nextLabelY = plotMin.y;
  const float labelX = plotMax.x + 6.0f;
  for (const PassLabel &label : labels)
  {
    const float y = std::min(std::max(label.y - textHeight * 0.5f, nextLabelY), plotMax.y - textHeight);
    drawList->AddLine(ImVec2(plotMax.x, label.y), ImVec2(labelX - 2.0f, y + textHeight * 0.5f), IM_COL32(170, 175, 185, 130), 1.0f);
    drawList->AddText(ImVec2(labelX, y), label.color, label.text.c_str());
    nextLabelY = y + textHeight + 1.0f;
  }
}

static void buildPassTimelineOverlay(const PassTimelineHistory &timelineHistory)
{
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags overlayWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float columnGap = 16.0f;
  const float columnWidth = std::min(560.0f, (viewport->WorkSize.x - outerMargin * 2.0f - columnGap) * 0.5f);
  const float rightColumnX = viewport->WorkPos.x + viewport->WorkSize.x - outerMargin - columnWidth;
  const float topWindowHeight = 138.0f;
  const float graphY = viewport->WorkPos.y + outerMargin + topWindowHeight + 12.0f;
  const float graphHeight = std::max(220.0f, viewport->WorkSize.y - (graphY - viewport->WorkPos.y) - outerMargin);
  const float sectionGap = 12.0f;
  const float controlsHeight = 58.0f;
  const float sectionHeight = std::max(180.0f, (graphHeight - controlsHeight - sectionGap) * 0.38f);
  const float computeSectionHeight = sectionHeight;
  const float graphicsSectionHeight = sectionHeight;

  ImGui::SetNextWindowPos(ImVec2(rightColumnX, graphY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(columnWidth, graphHeight), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.84f);
  ImGui::Begin("Pass Timeline", nullptr, overlayWindowFlags);
  ImGui::SetWindowFontScale(0.9f);
  static int visibleFrameCount = 60;
  static int selectedFrameOffset = 0;
  const int availableFrameCount = static_cast<int>(std::max<size_t>(1u, timelineHistory.count));
  visibleFrameCount = std::clamp(visibleFrameCount, 1, availableFrameCount);
  selectedFrameOffset = std::clamp(selectedFrameOffset, 0, std::max(0, visibleFrameCount - 1));
  ImGui::Text("Per-pass GPU history over the last %d frames", static_cast<int>(timelineHistory.count));
  if (timelineHistory.count == 0u)
  {
    ImGui::BeginDisabled();
  }
  ImGui::SliderInt("Visible frames", &visibleFrameCount, 1, availableFrameCount);
  selectedFrameOffset = std::clamp(selectedFrameOffset, 0, std::max(0, visibleFrameCount - 1));
  ImGui::SliderInt("Inspect N-", &selectedFrameOffset, 0, std::max(0, visibleFrameCount - 1));
  if (timelineHistory.count == 0u)
  {
    ImGui::EndDisabled();
  }
  if (timelineHistory.count > 0u)
  {
    const size_t selectedOrderedSample = timelineHistory.count - 1u - static_cast<size_t>(selectedFrameOffset);
    ImGui::Text(
        "Frame %u  Compute %.3f ms  Graphics %.3f ms",
        timelineHistory.getFrameIndexAtOrderedSample(selectedOrderedSample),
        timelineHistory.getSampleTotalMs(PassTimelineHistory::Filter::Compute, selectedOrderedSample),
        timelineHistory.getSampleTotalMs(PassTimelineHistory::Filter::Graphics, selectedOrderedSample));
  }

  struct SelectedFramePassCost
  {
    const PassTimelineHistory::Series *entry = nullptr;
    float timeMs = 0.0f;
    size_t groupIndex = 0u;
    const char *type = "unknown";
  };

  struct SelectedFramePassGroup
  {
    const char *name = "";
    double totalMs = 0.0;
    size_t passCount = 0u;
  };
  constexpr std::array<const char *, 4> kSelectedFramePassGroupNames{
    "Virtual Shadow Maps",
    "GPU Signed Distance Fields",
    "Virtual Geometry Rendering",
    "Others",
  };

  auto classifySelectedFramePassGroup = [](const std::string &name) -> size_t
  {
    std::string lowerName = name;
    std::transform(
        lowerName.begin(),
        lowerName.end(),
        lowerName.begin(),
        [](unsigned char c)
        {
          return static_cast<char>(std::tolower(c));
        });

    if (lowerName.find("virtualshadowmap") != std::string::npos || lowerName.find("vsm") != std::string::npos)
    {
      return 0u;
    }
    if (lowerName.find("gpusigneddistancefields") != std::string::npos || lowerName.find("gpusdf") != std::string::npos || lowerName.find("signeddistancefields") != std::string::npos)
    {
      return 1u;
    }
    if (lowerName.find("virtualgeometry") != std::string::npos)
    {
      return 2u;
    }
    return 3u;
  };

  struct PassGraphContentSize
  {
    ImVec2 canvasSize{};
    float labelGutterWidth = 520.0f;
  };

  auto measurePassGraphContent = [&](PassTimelineHistory::Filter filter, float baseHeight)
  {
    float maxLabelWidth = 0.0f;

    if (timelineHistory.count > 0u)
    {
      const size_t selectedOrderedSample = timelineHistory.count - 1u - static_cast<size_t>(selectedFrameOffset);
      const std::vector<const PassTimelineHistory::Series *> orderedSeries = timelineHistory.getSeriesSortedBySample(filter, selectedOrderedSample);
      for (const PassTimelineHistory::Series *entry : orderedSeries)
      {
        const float selectedValue = timelineHistory.getSampleValue(*entry, selectedOrderedSample);
        if (selectedValue <= 0.0f)
        {
          continue;
        }

        char labelText[256];
        std::snprintf(labelText, sizeof(labelText), "%.2f ms %s", selectedValue, entry->name.c_str());
        maxLabelWidth = std::max(maxLabelWidth, ImGui::CalcTextSize(labelText).x);
      }
    }

    const float labelGutterWidth = std::max(520.0f, maxLabelWidth + 28.0f);
    const float canvasHeight = baseHeight;
    const float canvasWidth = std::max(ImGui::GetContentRegionAvail().x, 52.0f + 980.0f + labelGutterWidth);
    return PassGraphContentSize{
      .canvasSize = ImVec2(canvasWidth, canvasHeight),
      .labelGutterWidth = labelGutterWidth,
    };
  };

  auto drawScrollablePassGraph = [&](const char *childId, const char *canvasId, const char *emptyLabel, PassTimelineHistory::Filter filter, float visibleHeight)
  {
    const PassGraphContentSize graphContentSize = measurePassGraphContent(filter, visibleHeight);
    const float childHeight = visibleHeight + ImGui::GetStyle().ScrollbarSize + 4.0f;
    ImGui::BeginChild(childId, ImVec2(0.0f, childHeight), false, ImGuiWindowFlags_HorizontalScrollbar);
    drawPassTimelineGraph(canvasId, emptyLabel, timelineHistory, filter, static_cast<size_t>(visibleFrameCount), static_cast<size_t>(selectedFrameOffset), graphContentSize.labelGutterWidth, graphContentSize.canvasSize);
    ImGui::EndChild();
  };

  ImGui::TextUnformatted("Compute Passes");
  drawScrollablePassGraph("ComputePassTimelineScroll", "ComputePassTimelineCanvas", "No compute GPU timer samples yet", PassTimelineHistory::Filter::Compute, computeSectionHeight);
  ImGui::Dummy(ImVec2(0.0f, sectionGap - 4.0f));
  ImGui::TextUnformatted("Graphics Passes");
  drawScrollablePassGraph("GraphicsPassTimelineScroll", "GraphicsPassTimelineCanvas", "No graphics GPU timer samples yet", PassTimelineHistory::Filter::Graphics, graphicsSectionHeight);

  ImGui::Dummy(ImVec2(0.0f, sectionGap - 4.0f));
  ImGui::TextUnformatted("Selected Frame Pass Costs");
  if (timelineHistory.count == 0u)
  {
    ImGui::TextDisabled("No GPU timer samples yet");
  }
  else
  {
    const size_t selectedOrderedSample = timelineHistory.count - 1u - static_cast<size_t>(selectedFrameOffset);
    std::vector<SelectedFramePassCost> selectedPassCosts;
    selectedPassCosts.reserve(timelineHistory.series.size());
    std::array<SelectedFramePassGroup, kSelectedFramePassGroupNames.size()> selectedPassGroups{};
    for (size_t groupIndex = 0u; groupIndex < kSelectedFramePassGroupNames.size(); ++groupIndex)
    {
      selectedPassGroups[groupIndex].name = kSelectedFramePassGroupNames[groupIndex];
    }
    double selectedFrameTotalMs = 0.0;
    float maxNameWidth = 0.0f;
    for (const PassTimelineHistory::Series &entry : timelineHistory.series)
    {
      const float timeMs = timelineHistory.getSampleValue(entry, selectedOrderedSample);
      if (timeMs <= 0.0f)
      {
        continue;
      }

      const char *type = passTimingStageLabel(entry.hasComputeWork, entry.hasGraphicsWork);
      const size_t groupIndex = classifySelectedFramePassGroup(entry.name);
      selectedPassGroups[groupIndex].totalMs += timeMs;
      selectedPassGroups[groupIndex].passCount += 1u;
      selectedFrameTotalMs += timeMs;
      maxNameWidth = std::max(maxNameWidth, ImGui::CalcTextSize(entry.name.c_str()).x);
      selectedPassCosts.push_back(
          SelectedFramePassCost{
            .entry = &entry,
            .timeMs = timeMs,
            .groupIndex = groupIndex,
            .type = type,
          });
    }

    std::sort(
        selectedPassCosts.begin(),
        selectedPassCosts.end(),
        [](const SelectedFramePassCost &a, const SelectedFramePassCost &b)
        {
          if (a.timeMs != b.timeMs)
          {
            return a.timeMs > b.timeMs;
          }
          if (a.entry->passIndex != b.entry->passIndex)
          {
            return a.entry->passIndex < b.entry->passIndex;
          }
          return a.entry->name < b.entry->name;
        });

    if (selectedPassCosts.empty())
    {
      ImGui::TextDisabled("No pass timings for the selected frame");
    }
    else
    {
      const float tableHeight = std::max(180.0f, sectionHeight * 0.58f);
      const float nameColumnWidth = std::max(520.0f, maxNameWidth + 32.0f);
      const float tableInnerWidth = std::max(ImGui::GetContentRegionAvail().x, 94.0f + 96.0f + 64.0f + nameColumnWidth);
      const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
      if (ImGui::BeginTable("SelectedFramePassCostTable", 4, tableFlags, ImVec2(0.0f, tableHeight), tableInnerWidth))
      {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 94.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, nameColumnWidth);
        ImGui::TableHeadersRow();
        for (const SelectedFramePassCost &cost : selectedPassCosts)
        {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%.3f ms", cost.timeMs);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(cost.type);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%u", cost.entry->passIndex);
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(cost.entry->name.c_str());
        }
        ImGui::EndTable();
      }

      ImGui::Dummy(ImVec2(0.0f, sectionGap - 4.0f));
      ImGui::Text("Selected Frame Total %.3f ms", selectedFrameTotalMs);
      if (ImGui::BeginTable("SelectedFramePassGroupTotals", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
      {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Total");
        ImGui::TableSetupColumn("Share");
        ImGui::TableSetupColumn("Passes");
        ImGui::TableHeadersRow();
        for (const SelectedFramePassGroup &group : selectedPassGroups)
        {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(group.name);
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%.3f ms", group.totalMs);
          ImGui::TableSetColumnIndex(2);
          const double sharePercent = selectedFrameTotalMs > 0.0 ? (group.totalMs / selectedFrameTotalMs) * 100.0 : 0.0;
          ImGui::Text("%.1f%%", sharePercent);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%zu", group.passCount);
        }
        ImGui::EndTable();
      }

      ImGui::TextUnformatted("Grouped Pass Breakdown");
      const float groupedTableHeight = std::max(180.0f, sectionHeight * 0.72f);
      const float groupedTableInnerWidth = std::max(ImGui::GetContentRegionAvail().x, 180.0f + 94.0f + 96.0f + 64.0f + nameColumnWidth);
      if (ImGui::BeginTable("SelectedFrameGroupedPassCostTable", 5, tableFlags, ImVec2(0.0f, groupedTableHeight), groupedTableInnerWidth))
      {
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 94.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, nameColumnWidth);
        ImGui::TableHeadersRow();
        for (size_t groupIndex = 0u; groupIndex < selectedPassGroups.size(); ++groupIndex)
        {
          if (selectedPassGroups[groupIndex].passCount == 0u)
          {
            continue;
          }

          bool wroteGroupLabel = false;
          for (const SelectedFramePassCost &cost : selectedPassCosts)
          {
            if (cost.groupIndex != groupIndex)
            {
              continue;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (!wroteGroupLabel)
            {
              ImGui::Text("%s %.3f ms", selectedPassGroups[groupIndex].name, selectedPassGroups[groupIndex].totalMs);
              wroteGroupLabel = true;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ms", cost.timeMs);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(cost.type);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", cost.entry->passIndex);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(cost.entry->name.c_str());
          }
        }
        ImGui::EndTable();
      }

      ImGui::Dummy(ImVec2(0.0f, sectionGap - 4.0f));
      ImGui::TextUnformatted("Selected Frame Full Pass Table");
      std::vector<const PassTimelineHistory::Series *> allSeries = timelineHistory.series.empty() ? std::vector<const PassTimelineHistory::Series *>{} : [&]()
      {
        std::vector<const PassTimelineHistory::Series *> ordered;
        ordered.reserve(timelineHistory.series.size());
        for (const PassTimelineHistory::Series &entry : timelineHistory.series)
        {
          ordered.push_back(&entry);
        }
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const PassTimelineHistory::Series *a, const PassTimelineHistory::Series *b)
            {
              if (a->passIndex != b->passIndex)
              {
                return a->passIndex < b->passIndex;
              }
              return a->name < b->name;
            });
        return ordered;
      }();
      const float fullTableHeight = std::max(220.0f, sectionHeight * 0.88f);
      const float fullTableInnerWidth = std::max(ImGui::GetContentRegionAvail().x, 120.0f + 94.0f + 80.0f + 80.0f + 96.0f + 64.0f + nameColumnWidth);
      if (ImGui::BeginTable("SelectedFrameFullPassTable", 7, tableFlags, ImVec2(0.0f, fullTableHeight), fullTableInnerWidth))
      {
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 94.0f);
        ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Timers", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, nameColumnWidth);
        ImGui::TableHeadersRow();
        for (const PassTimelineHistory::Series *entry : allSeries)
        {
          const float timeMs = timelineHistory.getSampleValue(*entry, selectedOrderedSample);
          const uint32_t timerCount = timelineHistory.getSampleTimerCount(*entry, selectedOrderedSample);
          const char *status = passTimingStatusLabel(timeMs, timerCount);
          const char *stage = passTimingStageLabel(entry->hasComputeWork, entry->hasGraphicsWork);
          const float sharePercent = selectedFrameTotalMs > 0.0 ? (timeMs / static_cast<float>(selectedFrameTotalMs)) * 100.0f : 0.0f;
          const bool passMissing = timerCount == 0u;

          ImGui::TableNextRow();
          if (passMissing)
          {
            ImGui::BeginDisabled();
          }
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(status);
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%.3f ms", timeMs);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f%%", sharePercent);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%u", timerCount);
          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(stage);
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%u", entry->passIndex);
          ImGui::TableSetColumnIndex(6);
          ImGui::TextUnformatted(entry->name.c_str());
          if (passMissing)
          {
            ImGui::EndDisabled();
          }
        }
        ImGui::EndTable();
      }
    }
  }
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

  const float maxMs = std::max(1.0f, timelineHistory.getVisibleMaxTotalMs() * 1.1f);
  for (int gridIndex = 0; gridIndex < 5; ++gridIndex)
  {
    const float t = static_cast<float>(gridIndex) / 4.0f;
    const float y = plotMax.y + (plotMin.y - plotMax.y) * t;
    drawList->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), IM_COL32(60, 68, 80, 200), 1.0f);

    char label[32];
    std::snprintf(label, sizeof(label), "%.2f ms", maxMs * t);
    drawList->AddText(ImVec2(outerMin.x + 8.0f, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(190, 196, 205, 255), label);
  }

  drawList->AddText(ImVec2(plotMin.x, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "oldest");
  drawList->AddText(ImVec2(plotMax.x - 42.0f, outerMax.y - ImGui::GetTextLineHeight()), IM_COL32(160, 166, 175, 255), "newest");

  if (timelineHistory.count == 0u)
  {
    drawList->AddText(ImVec2(plotMin.x + 12.0f, plotMin.y + 12.0f), IM_COL32(220, 225, 230, 255), "No CPU section samples yet");
    return;
  }

  const size_t selectedOrderedSample = timelineHistory.count - 1u;
  const std::vector<const CpuTimelineHistory::Series *> orderedSeries = timelineHistory.getSeriesSortedBySample(selectedOrderedSample);
  std::array<float, CpuTimelineHistory::kSampleCount> cumulativeValues{};
  drawList->PushClipRect(plotMin, plotMax, true);
  for (const CpuTimelineHistory::Series *entry : orderedSeries)
  {
    bool hasNonZeroSample = false;
    for (size_t sampleIndex = 0u; sampleIndex < timelineHistory.count; ++sampleIndex)
    {
      const float value = timelineHistory.getSampleValue(*entry, sampleIndex);
      if (value > 0.0f)
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
      cumulativeValues[sampleIndex] += timelineHistory.getSampleValue(*entry, sampleIndex);
      const float normalizedX = pointCount > 1u ? (static_cast<float>(sampleIndex) / static_cast<float>(pointCount - 1u)) : 0.0f;
      const float normalizedY = std::clamp(cumulativeValues[sampleIndex] / maxMs, 0.0f, 1.0f);
      const ImVec2 point(plotMin.x + (plotMax.x - plotMin.x) * normalizedX, plotMax.y + (plotMin.y - plotMax.y) * normalizedY);

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
static std::string formatBytes(uint64_t bytes);

static void drawCpuOperationRow(const char *name, const char *description, const CpuFrameReport::Operation &operation)
{
  const RenderGraph::CpuTransferStats &writes = operation.cpu.bufferWrites;
  const RenderGraph::CpuTransferStats &reads = operation.cpu.bufferReads;
  const std::string writeBytes = formatBytes(writes.totalBytes);
  const std::string readBytes = formatBytes(reads.totalBytes);

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(name);
  if (description != nullptr && ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("%s", description);
  }
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%.3f ms", operation.wallMs);
  ImGui::TableSetColumnIndex(2);
  ImGui::Text("%llu / %.3f ms / %s", static_cast<unsigned long long>(writes.callCount), nsToMs(writes.totalNs), writeBytes.c_str());
  ImGui::TableSetColumnIndex(3);
  ImGui::Text("%llu / %.3f ms / %s", static_cast<unsigned long long>(reads.callCount), nsToMs(reads.totalNs), readBytes.c_str());
  ImGui::TableSetColumnIndex(4);
  ImGui::Text("W %.3f / R %.3f ms", nsToMs(writes.maxNs), nsToMs(reads.maxNs));
}

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
  const ImGuiWindowFlags overlayWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

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
  ImGui::Text("Tracked %.3f ms  Untracked %.3f ms  UI %.3f ms  Present %.3f ms", computeTrackedCpuFrameMs(cpuFrame), cpuFrame.untrackedMs, cpuFrame.uiBuildMs, cpuFrame.presentMs);
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

  if (ImGui::CollapsingHeader("CPU Operation Transfers", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::BeginTable("CpuOperationTransferTable", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
    {
      ImGui::TableSetupColumn("Operation");
      ImGui::TableSetupColumn("Wall");
      ImGui::TableSetupColumn("Writes");
      ImGui::TableSetupColumn("Reads");
      ImGui::TableSetupColumn("Max");
      ImGui::TableHeadersRow();
      drawCpuOperationRow("SDF Sync", "Updates SDF clipmaps, scans scene/fallback mesh state, uploads SDF mesh/instance/bone/job buffers, and queues SDF rebuild jobs.", cpuFrame.sdfSync);
      drawCpuOperationRow("SWRT BLAS Sync", "Scans visible virtual-geometry sources, resolves SWRT/SWRBVH fallback files, rebuilds BLAS CPU packing when dirty, and uploads BLAS buffers/atlases.", cpuFrame.softwareBlasSync);
      drawCpuOperationRow("SWRT TLAS Sync", "Scans software-trace scene instances and rebuilds/refits the software TLAS when BLAS handles, instance order, bounds, or transforms change.", cpuFrame.softwareTlasSync);
      drawCpuOperationRow("Uniform Upload", "Updates per-pass CPU-side runtime state and writes camera/material/VSM/SDF uniforms for the frame.", cpuFrame.uniformUpload);
      drawCpuOperationRow("VT Feedback", "Reads material feedback and prepares virtual-texture streaming requests.", cpuFrame.vtFeedback);
      drawCpuOperationRow("Streaming", "Updates virtual-geometry page streaming state.", cpuFrame.sceneStreaming);
      drawCpuOperationRow("Instance Upload", "Uploads the current virtual-geometry instance buffer.", cpuFrame.instanceUpload);
      drawCpuOperationRow("Stats Readback", "Reads and scans debug/statistics buffers for the overlay.", cpuFrame.statsReadback);
      ImGui::EndTable();
    }
    const auto &tlasBuild = cpuFrame.softwareTlasBuild;
    ImGui::Text(
        "SWRT TLAS rebuilt %s  refit %s  partial %s  dirty %u  BVH4 %u nodes/%u leaves  SAH %.3f -> %.3f %.2fx%s",
        tlasBuild.rebuilt ? "yes" : "no",
        tlasBuild.refitted ? "yes" : "no",
        tlasBuild.partialRebuilt ? "yes" : "no",
        tlasBuild.dirtyInstanceCount,
        tlasBuild.bvh4NodeCount,
        tlasBuild.bvh4LeafCount,
        tlasBuild.bvh4BuildSahScore,
        tlasBuild.bvh4CurrentSahScore,
        tlasBuild.bvh4SahGrowth,
        tlasBuild.bvh4RebuiltForSah ? " rebuilt" : "");
  }

  if (ImGui::CollapsingHeader("RenderGraph Run", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Run %.3f ms  Submit %u  Cmd buffers %u", renderGraphRunTotalMs, renderGraphRunDebugStats.submissionCount, renderGraphRunDebugStats.submittedCommandBufferCount);
    ImGui::Text("Wait %.3f ms  Block %.3f ms  Timer readback %.3f ms", cpuFrame.waitMs, cpuFrame.waitBlockMs, cpuFrame.timerReadbackMs);
    ImGui::Text("Ext waits %u  Same-queue skipped %u  Waited nodes %u", renderGraphRunDebugStats.externalWaitDependencyCount, renderGraphRunDebugStats.sameQueueWaitDependencyCount, frameWaitSummary.waitedNodeCount);
    ImGui::Text("Buffer barriers %u  Texture barriers %u", renderGraphRunDebugStats.emittedBufferBarrierCount, renderGraphRunDebugStats.emittedTextureBarrierCount);

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
    const VirtualTextureFrameStats &virtualTextureStats,
    const std::vector<PassTimingSummary> &passTimings,
    const OverlayHistory &history)
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
  const double sdfCpuMs = cpuFrame.sdfSyncMs;
  const double vsmCpuMs = cpuFrame.vsmLightSetupMs + cpuFrame.vsmUpdateMs + cpuFrame.vsmInvalidateMs + cpuFrame.vsmResetInvalidationsMs + cpuFrame.statsAllocatorReadMs + cpuFrame.statsDrawCountersReadMs +
                          cpuFrame.statsPageTableReadMs + cpuFrame.statsPageTableProcessMs + cpuFrame.statsVisibleClustersReadMs + cpuFrame.statsVisibleClustersProcessMs + cpuFrame.statsDrawIndirectReadMs +
                          cpuFrame.statsDrawIndirectProcessMs;

  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags overlayWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float columnGap = 16.0f;
  const float columnWidth = std::min(430.0f, (viewport->WorkSize.x - outerMargin * 2.0f - columnGap) * 0.5f);
  const float leftColumnX = viewport->WorkPos.x + outerMargin;
  const float rightColumnX = viewport->WorkPos.x + viewport->WorkSize.x - outerMargin - columnWidth;
  const float topY = viewport->WorkPos.y + outerMargin;
  const float topWindowHeight = 154.0f;

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
  ImGui::Text("Window %.3f  Sim %.3f  Cam %.3f  SDF %.3f", cpuFrame.windowUpdateMs, cpuFrame.simulationMs, cpuFrame.cameraBuildMs, cpuFrame.sdfSyncMs);
  ImGui::Text("Uniforms %.3f  Feedback %.3f  Stream %.3f  Inst %.3f", cpuFrame.uniformUploadMs, cpuFrame.vtFeedback.wallMs, cpuFrame.streamingMs, cpuFrame.instanceUploadMs);
  ImGui::Text("UI %.3f", cpuFrame.uiBuildMs);
  ImGui::Text("VT CPU %.3f  SDF CPU %.3f  VSM CPU %.3f  PCF %s", vtCpuMs, sdfCpuMs, vsmCpuMs, shadowPcfEnabled ? "on" : "off");
  ImGui::Text(
      "VT req %u/%u  upload %u  resident %u/%u",
      virtualTextureStats.feedback.requested,
      virtualTextureStats.feedback.deduplicated,
      virtualTextureStats.feedback.uploaded,
      virtualTextureStats.residentPages,
      virtualTextureStats.pageTableEntryCount);
  ImGui::Text(
      "VT overflow %u  evict %u  atlas %ux%u pages %u%s",
      virtualTextureStats.feedback.overflowed,
      virtualTextureStats.feedback.evicted,
      virtualTextureStats.physicalPagesPerAxis,
      virtualTextureStats.physicalPagesPerAxis,
      virtualTextureStats.pageSize,
      virtualTextureStats.hasDirtyResidency ? " dirty" : "");
  ImGui::Text("Untracked %.3f", cpuFrame.untrackedMs);
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
  case 25u:
    return 4;
  case 36u:
    return 5;
  case 49u:
    return 6;
  case 64u:
    return 7;
  default:
    return 1;
  }
}

static uint32_t shadowPcfTapIndexToCount(int tapIndex)
{
  static constexpr std::array<uint32_t, 8> kTapCounts = {1u, 4u, 9u, 16u, 25u, 36u, 49u, 64u};
  const int clampedIndex = std::clamp(tapIndex, 0, static_cast<int>(kTapCounts.size()) - 1);
  return kTapCounts[static_cast<size_t>(clampedIndex)];
}

enum class IndirectGIOption : uint32_t
{
  ImperfectShadowMaps = 0u,
};

enum class DiffuseGIOption : uint32_t
{
  ReflexiveShadowMaps = 0u,
  ImperfectShadowMaps = 1u,
};

enum class IsmVplSpawnMode
{
  DirectionalRsmRaycast,
  SurfaceImportanceFallback,
  SphereFallback,
};

struct IsmVplBuildStats
{
  IsmVplSpawnMode spawnMode = IsmVplSpawnMode::SphereFallback;
  uint32_t surfaceTriangleCount = 0u;
  uint32_t rsmRayCount = 0u;
  uint32_t rsmHitCount = 0u;
};

static constexpr bool kRendererEnableSoftwareRayTracingPath = true;
static constexpr bool kRendererEnableWorldHitShadingResolve = false;

static const char *indirectGIOptionLabel(IndirectGIOption option)
{
  switch (option)
  {
  case IndirectGIOption::ImperfectShadowMaps:
    return "Imperfect Shadow Map";
  }

  return "Unknown";
}

static const char *diffuseGIOptionLabel(DiffuseGIOption option)
{
  switch (option)
  {
  case DiffuseGIOption::ReflexiveShadowMaps:
    return "Reflexive Shadow Maps";
  case DiffuseGIOption::ImperfectShadowMaps:
    return "Imperfect Shadow Maps";
  }

  return "Unknown";
}

static const char *ismVplSpawnModeLabel(IsmVplSpawnMode mode)
{
  switch (mode)
  {
  case IsmVplSpawnMode::DirectionalRsmRaycast:
    return "Directional light RSM raycast";
  case IsmVplSpawnMode::SurfaceImportanceFallback:
    return "Surface importance fallback";
  case IsmVplSpawnMode::SphereFallback:
    return "Sphere fallback";
  }
  return "Unknown";
}

static const char *shadowMapTypeLabel(VirtualShadowMapManager::Settings::ShadowMapType type)
{
  using ShadowMapType = VirtualShadowMapManager::Settings::ShadowMapType;
  switch (type)
  {
  case ShadowMapType::VirtualShadowMap:
    return "Virtual Shadow Map";
  case ShadowMapType::ReflexiveShadowMap:
    return "Reflexive Shadow Map";
  }
  return "Unknown";
}

enum class RendererDisplayOutput : uint32_t
{
  ShadedColor = 0u,
  ShadowLighting,
  ScreenSpaceShadow,
  SsssHardShadow,
  SsssPenumbra,
  SsssTranslucency,
  SsssHorizontalBlur,
  SsssBlurredVisibility,
  SsssLayerOccupancy,
  MaterialId,
  SceneDepth,
  MotionVectors,
  VsmPages,
  VsmTable,
  VsmAtlas,
  IsmVplPages,
  IsmVplPoints,
  IsmVplTable,
  IsmVplAtlas,
	  SignedDistanceFieldsTrace,
	  SignedDistanceFieldsTraceMaterialId,
	  SignedDistanceFieldsTraceMaterialUV,
	  SignedDistanceFieldsTraceMaterialUVWindow,
	  SignedDistanceFieldsTraceMaterialAlbedo,
  SignedDistanceFieldsLocalInstanceRaymarch,
  SignedDistanceFieldsLocalInstanceNormal,
  SignedDistanceFieldsLocalInstanceAlbedo,
  SignedDistanceFieldsLocalInstanceBlinn,
  SignedDistanceFieldsLocalInstanceStepCount,
  SignedDistanceFieldsLocalInstanceCascades,
  SignedDistanceFieldsLocalInstanceCascadeInstances,
	  SignedDistanceFieldsAtlas,
  SignedDistanceFieldsFallbackScene,
  SignedDistanceFieldsFallbackSceneNormal,
  SignedDistanceFieldsFallbackSceneAlbedo,
  SignedDistanceFieldsFallbackSceneRoughness,
  SignedDistanceFieldsFallbackSceneMetallic,
  SignedDistanceFieldsFallbackSceneEmissive,
  SignedDistanceFieldsFallbackSceneOpacity,
  SignedDistanceFieldsFallbackSceneSurfaceFlags,
  SoftwareRayTrace,
  SoftwareRayTraceHeatmap,
  SoftwareRayTraceLocalAlbedo,
  SoftwareRayTraceWorldCwbvhAlbedo,
  GBufferBaseColor,
  GBufferGeometryNormal,
  GBufferNormal,
  GBufferTriangleId,
  GBufferClusterId,
  DisocclusionMask,
  ScreenProbes,
  SpatialRadianceCache,
  WorldShadingCacheDirectRadiance,
  WorldShadingCacheNormal,
  WorldShadingCacheAlbedo,
  WorldShadingCacheRoughnessMetallic,
  WorldShadingCacheEmissive,
  WorldShadingCacheDirection,
  WorldShadingCacheConfidence,
  SpatialRadianceReservoirCache,
  ScreenProbeIndirectDiffuse,
  ScreenProbeBentNormal,
  ScreenProbeAmbientOcclusion,
  ScreenProbeAODebugDistance,
  ScreenProbeAODebugCascade,
  ScreenProbeAODebugFlags,
  ScreenProbeAODebugSteps,
  RsmIndirectDiffuse,
  RsmIndirectDiffuseDenoised,
  RsmFlux,
  RsmNormal,
  RsmWorldPosition,
  RsmResolveMask,
  RsmValidSampleRatio,
  SurfelHierarchyRegion,
  SurfelHierarchyCellId,
  SurfelHierarchyFrustum,
  SurfelHierarchySlice,
  SurfelDirectDraw,
  SurfelAlbedo,
  SurfelNormal,
  SurfelIrradiance,
  SurfelHeatmap,
  ScreenSpaceRayTraceSteps,
  WorldGIDiffuseLighting,
  WorldGIGlossyRadiance,
  WorldGIIndirectLighting,
  Overdraw,
};

static constexpr std::array<RendererDisplayOutput, 71> kRendererDisplayOutputs = {
  RendererDisplayOutput::ShadedColor,
  RendererDisplayOutput::ShadowLighting,
  RendererDisplayOutput::ScreenSpaceShadow,
  RendererDisplayOutput::SsssHardShadow,
  RendererDisplayOutput::SsssPenumbra,
  RendererDisplayOutput::SsssTranslucency,
  RendererDisplayOutput::SsssHorizontalBlur,
  RendererDisplayOutput::SsssBlurredVisibility,
  RendererDisplayOutput::SsssLayerOccupancy,
  RendererDisplayOutput::MaterialId,
  RendererDisplayOutput::SceneDepth,
  RendererDisplayOutput::MotionVectors,
  RendererDisplayOutput::VsmPages,
	  RendererDisplayOutput::VsmTable,
  RendererDisplayOutput::VsmAtlas,
  RendererDisplayOutput::IsmVplPages,
  RendererDisplayOutput::IsmVplPoints,
  RendererDisplayOutput::IsmVplTable,
  RendererDisplayOutput::IsmVplAtlas,
	  RendererDisplayOutput::SignedDistanceFieldsTrace,
	  RendererDisplayOutput::SignedDistanceFieldsTraceMaterialId,
	  RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUV,
	  RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUVWindow,
	  RendererDisplayOutput::SignedDistanceFieldsTraceMaterialAlbedo,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades,
  RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances,
	  RendererDisplayOutput::SignedDistanceFieldsAtlas,
  RendererDisplayOutput::SignedDistanceFieldsFallbackScene,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneNormal,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneAlbedo,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneRoughness,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneMetallic,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneEmissive,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneOpacity,
  RendererDisplayOutput::SignedDistanceFieldsFallbackSceneSurfaceFlags,
  RendererDisplayOutput::SoftwareRayTrace,
  RendererDisplayOutput::SoftwareRayTraceHeatmap,
  RendererDisplayOutput::SoftwareRayTraceLocalAlbedo,
  RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo,
  RendererDisplayOutput::GBufferBaseColor,
  RendererDisplayOutput::GBufferGeometryNormal,
  RendererDisplayOutput::GBufferNormal,
  RendererDisplayOutput::GBufferTriangleId,
  RendererDisplayOutput::GBufferClusterId,
  RendererDisplayOutput::DisocclusionMask,
  RendererDisplayOutput::RsmIndirectDiffuse,
  RendererDisplayOutput::RsmIndirectDiffuseDenoised,
  RendererDisplayOutput::RsmFlux,
  RendererDisplayOutput::RsmNormal,
  RendererDisplayOutput::RsmWorldPosition,
  RendererDisplayOutput::RsmResolveMask,
  RendererDisplayOutput::RsmValidSampleRatio,
  RendererDisplayOutput::SurfelHierarchyRegion,
  RendererDisplayOutput::SurfelHierarchyCellId,
  RendererDisplayOutput::SurfelHierarchyFrustum,
  RendererDisplayOutput::SurfelHierarchySlice,
  RendererDisplayOutput::SurfelDirectDraw,
  RendererDisplayOutput::SurfelAlbedo,
  RendererDisplayOutput::SurfelNormal,
  RendererDisplayOutput::SurfelIrradiance,
  RendererDisplayOutput::SurfelHeatmap,
  RendererDisplayOutput::ScreenSpaceRayTraceSteps,
  RendererDisplayOutput::WorldGIDiffuseLighting,
  RendererDisplayOutput::WorldGIGlossyRadiance,
  RendererDisplayOutput::WorldGIIndirectLighting,
  RendererDisplayOutput::Overdraw,
};

static const char *rendererDisplayOutputLabel(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::ShadedColor:
    return "Shaded Color";
  case RendererDisplayOutput::ShadowLighting:
    return "Shadow Lighting";
  case RendererDisplayOutput::ScreenSpaceShadow:
    return "SSS Shadow";
  case RendererDisplayOutput::SsssHardShadow:
    return "SSSS Hard Shadow";
  case RendererDisplayOutput::SsssPenumbra:
    return "SSSS Penumbra";
  case RendererDisplayOutput::SsssTranslucency:
    return "SSSS Translucency";
  case RendererDisplayOutput::SsssHorizontalBlur:
    return "SSSS Horizontal Blur";
  case RendererDisplayOutput::SsssBlurredVisibility:
    return "SSSS Blurred Visibility";
  case RendererDisplayOutput::SsssLayerOccupancy:
    return "SSSS Layer";
  case RendererDisplayOutput::MaterialId:
    return "Material Id";
  case RendererDisplayOutput::SceneDepth:
    return "Scene Depth";
  case RendererDisplayOutput::MotionVectors:
    return "Motion Vectors";
  case RendererDisplayOutput::VsmPages:
    return "VSM Pages";
  case RendererDisplayOutput::VsmTable:
    return "VSM Table";
  case RendererDisplayOutput::VsmAtlas:
    return "VSM Atlas";
  case RendererDisplayOutput::IsmVplPages:
    return "VPL ISM Pages";
  case RendererDisplayOutput::IsmVplPoints:
    return "VPL Points";
  case RendererDisplayOutput::IsmVplTable:
    return "VPL ISM Table";
  case RendererDisplayOutput::IsmVplAtlas:
    return "VPL ISM Atlas";
	  case RendererDisplayOutput::SignedDistanceFieldsTrace:
	    return "SDF Trace";
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialId:
	    return "SDF Trace Material Id";
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUV:
	    return "SDF Trace Material UV";
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUVWindow:
	    return "SDF Trace Material UV Window";
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialAlbedo:
	    return "SDF Trace Material Albedo";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch:
    return "Per-instance SDF Raymarch";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal:
    return "Per-instance SDF Normal";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo:
    return "Per-instance SDF Albedo";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn:
    return "Per-instance SDF Blinn";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount:
    return "Per-instance SDF Trace Cost Heatmap";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades:
    return "Sparse SDF TLAS Node Visits";
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances:
    return "Sparse SDF TLAS Instance Tests";
	  case RendererDisplayOutput::SignedDistanceFieldsAtlas:
    return "SDF Atlas";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackScene:
    return "Fallback Scene Triangle Id";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneNormal:
    return "Fallback Scene Vertex Normal";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneAlbedo:
    return "Fallback Scene Vertex Albedo";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneRoughness:
    return "Fallback Scene Vertex Roughness";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneMetallic:
    return "Fallback Scene Vertex Metallic";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneEmissive:
    return "Fallback Scene Vertex Emissive";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneOpacity:
    return "Fallback Scene Vertex Opacity";
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneSurfaceFlags:
    return "Fallback Scene Surface Flags";
  case RendererDisplayOutput::SoftwareRayTrace:
    return "Software RT Triangle Id";
  case RendererDisplayOutput::SoftwareRayTraceHeatmap:
    return "Software RT Heatmap";
  case RendererDisplayOutput::SoftwareRayTraceLocalAlbedo:
    return "Software RT Local Albedo";
  case RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo:
    return "Software RT World CWBVH Albedo";
  case RendererDisplayOutput::GBufferBaseColor:
    return "GBuffer Base Color";
  case RendererDisplayOutput::GBufferGeometryNormal:
    return "GBuffer Geometry Normal";
  case RendererDisplayOutput::GBufferNormal:
    return "GBuffer Normal";
  case RendererDisplayOutput::GBufferTriangleId:
    return "GBuffer Triangle Id";
  case RendererDisplayOutput::GBufferClusterId:
    return "GBuffer Cluster Id";
  case RendererDisplayOutput::DisocclusionMask:
    return "Disocclusion Mask";
  case RendererDisplayOutput::ScreenProbes:
    return "Screen Probes";
  case RendererDisplayOutput::SpatialRadianceCache:
    return "World Shading Cache Irradiance";
  case RendererDisplayOutput::WorldShadingCacheDirectRadiance:
    return "World Shading Cache Direct Radiance";
  case RendererDisplayOutput::WorldShadingCacheNormal:
    return "World Shading Cache Normal";
  case RendererDisplayOutput::WorldShadingCacheAlbedo:
    return "World Shading Cache Albedo";
  case RendererDisplayOutput::WorldShadingCacheRoughnessMetallic:
    return "World Shading Cache Roughness / Metallic";
  case RendererDisplayOutput::WorldShadingCacheEmissive:
    return "World Shading Cache Emissive";
  case RendererDisplayOutput::WorldShadingCacheDirection:
    return "World Shading Cache Direction";
  case RendererDisplayOutput::WorldShadingCacheConfidence:
    return "World Shading Cache Confidence";
  case RendererDisplayOutput::SpatialRadianceReservoirCache:
    return "Spatial Radiance Reservoir Cache";
  case RendererDisplayOutput::ScreenProbeIndirectDiffuse:
    return "Screen Probe Indirect Diffuse";
  case RendererDisplayOutput::ScreenProbeBentNormal:
    return "Screen Probe Bent Normal";
  case RendererDisplayOutput::ScreenProbeAmbientOcclusion:
    return "Screen Probe Ambient Occlusion";
  case RendererDisplayOutput::ScreenProbeAODebugDistance:
    return "Screen Probe AO Debug Distance";
  case RendererDisplayOutput::ScreenProbeAODebugCascade:
    return "Screen Probe AO Debug Cascade";
  case RendererDisplayOutput::ScreenProbeAODebugFlags:
    return "Screen Probe AO Debug Flags";
  case RendererDisplayOutput::ScreenProbeAODebugSteps:
    return "Screen Probe AO Debug Steps";
  case RendererDisplayOutput::RsmIndirectDiffuse:
    return "RSM Indirect Diffuse";
  case RendererDisplayOutput::RsmIndirectDiffuseDenoised:
    return "RSM Indirect Diffuse Denoised";
  case RendererDisplayOutput::RsmFlux:
    return "RSM Flux";
  case RendererDisplayOutput::RsmNormal:
    return "RSM Normal";
  case RendererDisplayOutput::RsmWorldPosition:
    return "RSM World Position";
  case RendererDisplayOutput::RsmResolveMask:
    return "RSM Resolve Mask";
  case RendererDisplayOutput::RsmValidSampleRatio:
    return "RSM Valid Sample Ratio";
  case RendererDisplayOutput::SurfelHierarchyRegion:
    return "Surfel Grid Cell";
  case RendererDisplayOutput::SurfelHierarchyCellId:
    return "Surfel Hash Bucket";
  case RendererDisplayOutput::SurfelHierarchyFrustum:
    return "Surfel Grid Level";
  case RendererDisplayOutput::SurfelHierarchySlice:
    return "Surfel Cell Size";
  case RendererDisplayOutput::SurfelDirectDraw:
    return "Surfel Direct Draw";
  case RendererDisplayOutput::SurfelAlbedo:
    return "Surfel Albedo";
  case RendererDisplayOutput::SurfelNormal:
    return "Surfel Normal";
  case RendererDisplayOutput::SurfelIrradiance:
    return "Surfel Irradiance";
  case RendererDisplayOutput::SurfelHeatmap:
    return "Surfel Heatmap";
  case RendererDisplayOutput::ScreenSpaceRayTraceSteps:
    return "Screen Space RT Steps";
  case RendererDisplayOutput::WorldGIDiffuseLighting:
    return "World GI Diffuse Lighting";
  case RendererDisplayOutput::WorldGIGlossyRadiance:
    return "World GI Glossy Radiance";
  case RendererDisplayOutput::WorldGIIndirectLighting:
    return "World GI Indirect Lighting";
  case RendererDisplayOutput::Overdraw:
#ifdef COLLECT_FRAME_STATISTICS
    return "Overdraw";
#else
    return "Overdraw (requires frame stats)";
#endif
  }

  return "Unknown";
}

static std::string rendererDisplayOutputMaterialShaderOverride(RendererDisplayOutput output)
{
  switch (output)
  {
  default:
    return {};
  }
}

static uint32_t rendererDisplayOutputTargetSlot(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::ShadedColor:
    return SelectedDebugTargetPass::TargetSlot_ShadedColor;
  case RendererDisplayOutput::ShadowLighting:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapShadowLighting;
  case RendererDisplayOutput::ScreenSpaceShadow:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapScreenSpaceShadow;
  case RendererDisplayOutput::SsssHardShadow:
  case RendererDisplayOutput::SsssPenumbra:
  case RendererDisplayOutput::SsssTranslucency:
  case RendererDisplayOutput::SsssHorizontalBlur:
  case RendererDisplayOutput::SsssBlurredVisibility:
  case RendererDisplayOutput::SsssLayerOccupancy:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapScreenSpaceSoftShadow;
  case RendererDisplayOutput::MaterialId:
    return SelectedDebugTargetPass::TargetSlot_MaterialId;
  case RendererDisplayOutput::SceneDepth:
    return SelectedDebugTargetPass::TargetSlot_SceneDepth;
  case RendererDisplayOutput::MotionVectors:
    return SelectedDebugTargetPass::TargetSlot_MotionVectors;
  case RendererDisplayOutput::VsmPages:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapPages;
  case RendererDisplayOutput::VsmTable:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapTable;
  case RendererDisplayOutput::VsmAtlas:
    return SelectedDebugTargetPass::TargetSlot_VirtualShadowMapAtlas;
  case RendererDisplayOutput::IsmVplPages:
    return SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplPages;
  case RendererDisplayOutput::IsmVplPoints:
    return SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplPoints;
  case RendererDisplayOutput::IsmVplTable:
    return SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplTable;
  case RendererDisplayOutput::IsmVplAtlas:
    return SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplAtlas;
	  case RendererDisplayOutput::SignedDistanceFieldsTrace:
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialId:
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUV:
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUVWindow:
	  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialAlbedo:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances:
	    return SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace;
  case RendererDisplayOutput::SignedDistanceFieldsAtlas:
    return SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackScene:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneNormal:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneAlbedo:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneRoughness:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneMetallic:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneEmissive:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneOpacity:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneSurfaceFlags:
    return SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsFallbackScene;
  case RendererDisplayOutput::SoftwareRayTrace:
  case RendererDisplayOutput::SoftwareRayTraceLocalAlbedo:
  case RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo:
    return SelectedDebugTargetPass::TargetSlot_SoftwareRayTrace;
  case RendererDisplayOutput::SoftwareRayTraceHeatmap:
    return SelectedDebugTargetPass::TargetSlot_SoftwareRayTraceHeatmap;
  case RendererDisplayOutput::GBufferBaseColor:
    return SelectedDebugTargetPass::TargetSlot_BaseColor;
  case RendererDisplayOutput::GBufferGeometryNormal:
    return SelectedDebugTargetPass::TargetSlot_GeometryNormal;
  case RendererDisplayOutput::GBufferNormal:
    return SelectedDebugTargetPass::TargetSlot_Normal;
  case RendererDisplayOutput::GBufferTriangleId:
    return SelectedDebugTargetPass::TargetSlot_TriangleId;
  case RendererDisplayOutput::GBufferClusterId:
    return SelectedDebugTargetPass::TargetSlot_ClusterId;
  case RendererDisplayOutput::DisocclusionMask:
    return SelectedDebugTargetPass::TargetSlot_DisocclusionMask;
  case RendererDisplayOutput::ScreenProbes:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbes;
  case RendererDisplayOutput::SpatialRadianceCache:
  case RendererDisplayOutput::WorldShadingCacheDirectRadiance:
  case RendererDisplayOutput::WorldShadingCacheNormal:
  case RendererDisplayOutput::WorldShadingCacheAlbedo:
  case RendererDisplayOutput::WorldShadingCacheRoughnessMetallic:
  case RendererDisplayOutput::WorldShadingCacheEmissive:
  case RendererDisplayOutput::WorldShadingCacheDirection:
  case RendererDisplayOutput::WorldShadingCacheConfidence:
    return SelectedDebugTargetPass::TargetSlot_SpatialRadianceCache;
  case RendererDisplayOutput::SpatialRadianceReservoirCache:
    return SelectedDebugTargetPass::TargetSlot_SpatialRadianceReservoirCache;
  case RendererDisplayOutput::ScreenProbeIndirectDiffuse:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeIndirectDiffuse;
  case RendererDisplayOutput::ScreenProbeBentNormal:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeBentNormal;
  case RendererDisplayOutput::ScreenProbeAmbientOcclusion:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeAmbientOcclusion;
  case RendererDisplayOutput::ScreenProbeAODebugDistance:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeAODebugDistance;
  case RendererDisplayOutput::ScreenProbeAODebugCascade:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeAODebugCascade;
  case RendererDisplayOutput::ScreenProbeAODebugFlags:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeAODebugFlags;
  case RendererDisplayOutput::ScreenProbeAODebugSteps:
    return SelectedDebugTargetPass::TargetSlot_ScreenProbeAODebugSteps;
  case RendererDisplayOutput::RsmIndirectDiffuse:
    return SelectedDebugTargetPass::TargetSlot_RsmIndirectDiffuse;
  case RendererDisplayOutput::RsmIndirectDiffuseDenoised:
    return SelectedDebugTargetPass::TargetSlot_RsmIndirectDiffuseDenoised;
  case RendererDisplayOutput::RsmFlux:
    return SelectedDebugTargetPass::TargetSlot_RsmFlux;
  case RendererDisplayOutput::RsmNormal:
    return SelectedDebugTargetPass::TargetSlot_RsmNormal;
  case RendererDisplayOutput::RsmWorldPosition:
    return SelectedDebugTargetPass::TargetSlot_RsmWorldPosition;
  case RendererDisplayOutput::RsmResolveMask:
  case RendererDisplayOutput::RsmValidSampleRatio:
    return SelectedDebugTargetPass::TargetSlot_RsmIndirectDiffuse;
  case RendererDisplayOutput::SurfelHierarchyRegion:
  case RendererDisplayOutput::SurfelHierarchyCellId:
  case RendererDisplayOutput::SurfelHierarchyFrustum:
  case RendererDisplayOutput::SurfelHierarchySlice:
    return SelectedDebugTargetPass::TargetSlot_SurfelHierarchy;
  case RendererDisplayOutput::SurfelDirectDraw:
  case RendererDisplayOutput::SurfelAlbedo:
  case RendererDisplayOutput::SurfelNormal:
  case RendererDisplayOutput::SurfelIrradiance:
    return SelectedDebugTargetPass::TargetSlot_SurfelDirectDraw;
  case RendererDisplayOutput::SurfelHeatmap:
    return SelectedDebugTargetPass::TargetSlot_SurfelHeatmap;
  case RendererDisplayOutput::ScreenSpaceRayTraceSteps:
    return SelectedDebugTargetPass::TargetSlot_ScreenSpaceRayTraceSteps;
  case RendererDisplayOutput::WorldGIDiffuseLighting:
    return SelectedDebugTargetPass::TargetSlot_WorldGIDiffuseLighting;
  case RendererDisplayOutput::WorldGIGlossyRadiance:
    return SelectedDebugTargetPass::TargetSlot_WorldGIGlossyRadiance;
  case RendererDisplayOutput::WorldGIIndirectLighting:
    return SelectedDebugTargetPass::TargetSlot_WorldGIIndirectLighting;
  case RendererDisplayOutput::Overdraw:
    return SelectedDebugTargetPass::TargetSlot_Overdraw;
  }

  return SelectedDebugTargetPass::TargetSlot_ShadedColor;
}

static ScreenSpaceSoftShadowSettings::DebugMode rendererDisplayOutputSsssDebugMode(
    RendererDisplayOutput output,
    ScreenSpaceSoftShadowSettings::DebugMode fallbackMode)
{
  using DebugMode = ScreenSpaceSoftShadowSettings::DebugMode;
  switch (output)
  {
  case RendererDisplayOutput::SsssHardShadow:
    return DebugMode::HardShadow;
  case RendererDisplayOutput::SsssPenumbra:
    return DebugMode::Penumbra;
  case RendererDisplayOutput::SsssTranslucency:
    return DebugMode::Translucency;
  case RendererDisplayOutput::SsssHorizontalBlur:
    return DebugMode::HorizontalBlur;
  case RendererDisplayOutput::SsssBlurredVisibility:
    return DebugMode::BlurredVisibility;
  case RendererDisplayOutput::SsssLayerOccupancy:
    return DebugMode::LayerOccupancy;
  default:
    return fallbackMode;
  }
}

static gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode rendererDisplayOutputSdfTraceMode(
    RendererDisplayOutput output,
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode fallbackMode)
{
  using Mode = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode;
  switch (output)
  {
  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialId:
    return Mode::SurfaceMaterialId;
  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUV:
    return Mode::SurfaceMaterialUV;
  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialUVWindow:
    return Mode::SurfaceMaterialUVWindow;
  case RendererDisplayOutput::SignedDistanceFieldsTraceMaterialAlbedo:
    return Mode::SurfaceMaterialAlbedo;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch:
    return Mode::LocalInstanceRaymarch;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal:
    return Mode::LocalInstanceNormal;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo:
    return Mode::LocalInstanceAlbedo;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn:
    return Mode::LocalInstanceBlinn;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount:
    return Mode::LocalInstanceStepCount;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades:
    return Mode::LocalInstanceCascades;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances:
    return Mode::LocalInstanceCascadeInstances;
  default:
    return fallbackMode;
  }
}

static sdf::rendering::SDFTraceDebugPass::Mode rendererDisplayOutputSparseSdfTraceMode(
    RendererDisplayOutput output,
    sdf::rendering::SDFTraceDebugPass::Mode fallbackMode)
{
  using Mode = sdf::rendering::SDFTraceDebugPass::Mode;
  switch (output)
  {
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch:
    return Mode::LocalInstanceRaymarch;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal:
    return Mode::LocalInstanceNormal;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo:
    return Mode::LocalInstanceAlbedo;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn:
    return Mode::LocalInstanceBlinn;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount:
    return Mode::LocalInstanceStepCount;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades:
    return Mode::LocalInstanceCascades;
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances:
    return Mode::LocalInstanceCascadeInstances;
  default:
    return fallbackMode;
  }
}

static bool sdfTraceModeUsesLocalInstanceSdf(gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode mode)
{
  using Mode = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode;
  switch (mode)
  {
  case Mode::LocalInstanceRaymarch:
  case Mode::LocalInstanceNormal:
  case Mode::LocalInstanceAlbedo:
  case Mode::LocalInstanceBlinn:
  case Mode::LocalInstanceStepCount:
  case Mode::LocalInstanceCascades:
  case Mode::LocalInstanceCascadeInstances:
    return true;
  default:
    return false;
  }
}

static sdf::rendering::SDFTraceDebugPass::Mode sparseSdfTraceModeFromLegacyTraceMode(
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode mode,
    sdf::rendering::SDFTraceDebugPass::Mode fallbackMode)
{
  using LegacyMode = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode;
  using SparseMode = sdf::rendering::SDFTraceDebugPass::Mode;
  switch (mode)
  {
  case LegacyMode::LocalInstanceRaymarch:
    return SparseMode::LocalInstanceRaymarch;
  case LegacyMode::LocalInstanceNormal:
    return SparseMode::LocalInstanceNormal;
  case LegacyMode::LocalInstanceAlbedo:
    return SparseMode::LocalInstanceAlbedo;
  case LegacyMode::LocalInstanceBlinn:
    return SparseMode::LocalInstanceBlinn;
  case LegacyMode::LocalInstanceStepCount:
    return SparseMode::LocalInstanceStepCount;
  case LegacyMode::LocalInstanceCascades:
    return SparseMode::LocalInstanceCascades;
  case LegacyMode::LocalInstanceCascadeInstances:
    return SparseMode::LocalInstanceCascadeInstances;
  default:
    return fallbackMode;
  }
}

static bool rendererDisplayOutputUsesLocalInstanceSdf(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceRaymarch:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceNormal:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceAlbedo:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceBlinn:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceStepCount:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascades:
  case RendererDisplayOutput::SignedDistanceFieldsLocalInstanceCascadeInstances:
    return true;
  default:
    return false;
  }
}

static globalillumination::surfels::SurfelHierarchyDebugMode rendererDisplayOutputSurfelHierarchyDebugMode(RendererDisplayOutput output)
{
  using DebugMode = globalillumination::surfels::SurfelHierarchyDebugMode;
  switch (output)
  {
  case RendererDisplayOutput::SurfelHierarchyCellId:
    return DebugMode::CellId;
  case RendererDisplayOutput::SurfelHierarchyFrustum:
    return DebugMode::Frustum;
  case RendererDisplayOutput::SurfelHierarchySlice:
    return DebugMode::Slice;
  case RendererDisplayOutput::SurfelHierarchyRegion:
  default:
    return DebugMode::Region;
  }
}

static bool rendererDisplayOutputUsesSurfelHierarchy(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::SurfelHierarchyRegion:
  case RendererDisplayOutput::SurfelHierarchyCellId:
  case RendererDisplayOutput::SurfelHierarchyFrustum:
  case RendererDisplayOutput::SurfelHierarchySlice:
    return true;
  default:
    return false;
  }
}

static bool rendererDisplayOutputUsesSurfelDirectDraw(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::SurfelDirectDraw:
  case RendererDisplayOutput::SurfelAlbedo:
  case RendererDisplayOutput::SurfelNormal:
  case RendererDisplayOutput::SurfelIrradiance:
    return true;
  default:
    return false;
  }
}

static globalillumination::surfels::SurfelDirectDrawDebugPass::DebugMode rendererDisplayOutputSurfelDirectDrawDebugMode(RendererDisplayOutput output)
{
  using DebugMode = globalillumination::surfels::SurfelDirectDrawDebugPass::DebugMode;
  switch (output)
  {
  case RendererDisplayOutput::SurfelAlbedo:
    return DebugMode::Albedo;
  case RendererDisplayOutput::SurfelNormal:
    return DebugMode::Normal;
  case RendererDisplayOutput::SurfelIrradiance:
    return DebugMode::Irradiance;
  case RendererDisplayOutput::SurfelDirectDraw:
  default:
    return DebugMode::Coverage;
  }
}

static bool rendererDisplayOutputUsesSurfelHeatmap(RendererDisplayOutput output)
{
  return output == RendererDisplayOutput::SurfelHeatmap;
}

static bool rendererDisplayOutputIsSoftwareRayTracing(RendererDisplayOutput output)
{
  return output == RendererDisplayOutput::SoftwareRayTrace ||
         output == RendererDisplayOutput::SoftwareRayTraceHeatmap ||
         output == RendererDisplayOutput::SoftwareRayTraceLocalAlbedo ||
         output == RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo;
}

static bool rendererDisplayOutputIsSoftwareRayTracingAlbedo(RendererDisplayOutput output)
{
  return output == RendererDisplayOutput::SoftwareRayTraceLocalAlbedo ||
         output == RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo;
}

static bool rendererDisplayOutputUsesSoftwareRayTracing(RendererDisplayOutput output)
{
  return kRendererEnableSoftwareRayTracingPath && rendererDisplayOutputIsSoftwareRayTracing(output);
}

static bool rendererDisplayOutputUsesFallbackScene(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::SignedDistanceFieldsFallbackScene:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneNormal:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneAlbedo:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneRoughness:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneMetallic:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneEmissive:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneOpacity:
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneSurfaceFlags:
    return true;
  default:
    return false;
  }
}

static gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass::Mode rendererDisplayOutputFallbackSceneMode(RendererDisplayOutput output)
{
  using Mode = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass::Mode;
  switch (output)
  {
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneNormal:
    return Mode::VertexNormal;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneAlbedo:
    return Mode::VertexAlbedo;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneRoughness:
    return Mode::VertexRoughness;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneMetallic:
    return Mode::VertexMetallic;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneEmissive:
    return Mode::VertexEmissive;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneOpacity:
    return Mode::VertexOpacity;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackSceneSurfaceFlags:
    return Mode::VertexSurfaceFlags;
  case RendererDisplayOutput::SignedDistanceFieldsFallbackScene:
  default:
    return Mode::TriangleId;
  }
}

static softwarebvh::rendering::SoftwareRayTracePass::DebugMode rendererDisplayOutputSoftwareRayTraceDebugMode(RendererDisplayOutput output)
{
  using DebugMode = softwarebvh::rendering::SoftwareRayTracePass::DebugMode;
  switch (output)
  {
  case RendererDisplayOutput::SoftwareRayTraceHeatmap:
    return DebugMode::Heatmap;
  case RendererDisplayOutput::SoftwareRayTrace:
  default:
    return DebugMode::TriangleId;
  }
}

static bool rendererDisplayOutputUsesSurfels(RendererDisplayOutput output)
{
  return rendererDisplayOutputUsesSurfelHierarchy(output) || rendererDisplayOutputUsesSurfelDirectDraw(output) || rendererDisplayOutputUsesSurfelHeatmap(output);
}

static bool rendererDisplayOutputUsesWorldGI(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::WorldGIDiffuseLighting:
  case RendererDisplayOutput::WorldGIGlossyRadiance:
  case RendererDisplayOutput::WorldGIIndirectLighting:
    return true;
  default:
    return false;
  }
}

static bool rendererDisplayOutputUsesRsmScreenOutput(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::RsmIndirectDiffuse:
  case RendererDisplayOutput::RsmIndirectDiffuseDenoised:
  case RendererDisplayOutput::RsmFlux:
  case RendererDisplayOutput::RsmNormal:
  case RendererDisplayOutput::RsmWorldPosition:
  case RendererDisplayOutput::RsmResolveMask:
  case RendererDisplayOutput::RsmValidSampleRatio:
    return true;
  default:
    return false;
  }
}

static bool rendererDisplayOutputUsesIsmVplOutput(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::IsmVplPages:
  case RendererDisplayOutput::IsmVplPoints:
  case RendererDisplayOutput::IsmVplTable:
  case RendererDisplayOutput::IsmVplAtlas:
    return true;
  default:
    return false;
  }
}

static VirtualShadowMapRsmIndirectDiffusePass::DebugMode rendererDisplayOutputRsmDebugMode(RendererDisplayOutput output)
{
  switch (output)
  {
  case RendererDisplayOutput::RsmFlux:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::Flux;
  case RendererDisplayOutput::RsmNormal:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::Normal;
  case RendererDisplayOutput::RsmWorldPosition:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::WorldPosition;
  case RendererDisplayOutput::RsmResolveMask:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::ResolveMask;
  case RendererDisplayOutput::RsmValidSampleRatio:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::ValidSampleRatio;
  default:
    return VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse;
  }
}

struct RendererRebuildSettings
{
  RendererDisplayOutput displayOutput = RendererDisplayOutput::ShadedColor;
  DiffuseGIOption diffuseGIOption = DiffuseGIOption::ReflexiveShadowMaps;
  bool enableDiffuseGI = false;
  uint32_t rsmGiSampleCount = 8u;
  float rsmGiSampleRadiusMeters = 1024.0f;
  float rsmGiIntensity = 0.5f;
  float rsmGiNormalBias = 0.05f;
  float rsmGiMinDistance = 0.25f;
  float rsmGiMaxDistance = 512.0f;
  bool rsmGiTemporalAccumulation = true;
  uint32_t rsmGiTemporalFrames = 16u;
  float rsmGiTemporalDisocclusionWeight = 1.25f;
  float rsmGiTemporalDepthSigma = 0.0025f;
  float rsmGiTemporalNormalSigma = 24.0f;
  float rsmGiValidRatioMin = 0.35f;
  float rsmGiFireflyClamp = 16.0f;
  bool rsmGiDenoiserEnabled = true;
  uint32_t rsmGiDenoiserFrames = 32u;
  float rsmGiDenoiserDepthSigma = 0.0025f;
  float rsmGiDenoiserNormalSigma = 32.0f;
  float rsmGiDenoiserLuminanceSigma = 2.0f;
  float rsmGiDenoiserValidRatioMin = 0.25f;
  float rsmGiDenoiserFireflyClamp = 32.0f;
  float rsmGiDenoiserSpatialSigma = 1.0f;
  float rsmGiDenoiserSpatialRadius = 1.0f;
  float rsmGiDenoiserSpatialStrength = 0.65f;
  uint32_t rsmGiDenoiserAtrousIterations = 16u;
  float rsmGiDenoiserDisocclusionWeight = 2.0f;
  float rsmGiDenoiserMinCurrentWeight = 0.25f;
  IndirectGIOption indirectGIOption = IndirectGIOption::ImperfectShadowMaps;
  bool ismGiEnableVpls = true;
  uint32_t ismGiActiveVpls = 64u;
  uint32_t ismGiPagesPerVplFace = 1u;
  uint32_t ismGiPhysicalAtlasResolution = 2048u;
  uint32_t ismGiPhysicalPageSize = 64u;
  bool enableTsr = true;
  float temporalRenderScale = 0.75f;
  uint32_t temporalJitterSamples = 32u;
  float temporalJitterScale = 0.1f;

  bool enableTaa = true;
  uint32_t taaAccumulationFrames = 16u;
  float taaMotionScale = 0.12f;
  float taaDepthThreshold = 0.0025f;
  float taaNormalThreshold = 0.90f;
  float taaReactiveScale = 2.0f;
  float taaEdgeScale = 350.0f;
  float taaSharpenStrength = 0.45f;
  float taaDetailBoost = 0.35f;

  uint32_t prepassMaxHierarchyLevels = 8u;
  uint32_t prepassClusterQueueMiB = 16u;
  bool prepassEnableFrustumCulling = true;
  bool prepassEnableOcclusionCulling = true;

  uint32_t finalMaxHierarchyLevels = 8u;
  uint32_t finalClusterQueueMiB = 16u;
  bool finalEnableFrustumCulling = true;
  bool finalEnableOcclusionCulling = true;
  bool finalEnableStreamingPriorityUpdates = true;
  float finalSoftwarePixelsPerTriangleThreshold = 10.0f;

  uint32_t depthPrePassMaxDrawnClusters = 256u;
  uint32_t hardwareDrawMaxDrawnClusters = 256u;

  uint32_t materialFeedbackRequestCapacity = 16384u;
  uint32_t materialFeedbackStride = 2u;
  uint32_t materialMaxUploadsPerFrame = 256u;
  uint32_t materialTileSize = 128u;
  uint32_t materialTileWorkgroupSize = 16u;

  uint32_t vsmPhysicalAtlasResolution = 16384u;
  uint32_t vsmRsmAtlasResolution = 4096u;
  uint32_t vsmPhysicalPageSize = 128u;
  uint32_t vsmCascadeCount = 4u;
  bool vsmDirectionalFirstCascadeWorldExtentAuto = true;
  float vsmDirectionalFirstCascadeWorldExtent = 256.0f;
  float vsmDirectionalCascadeWorldExtentScale = 2.0f;
  float vsmPageWorldScale = 1.0f;
  uint32_t vsmMaxDirtyPagesPerFrame = 64u;
  uint32_t vsmMaxFallbackPagesPerFrame = 64u;
  uint32_t vsmHierarchyQueueCapacity = 1024u * 1024u;
  uint32_t vsmClusterQueueCapacity = 1024u * 1024u;
  uint32_t vsmMaxVisibleClusterDraws = 131072u;
  uint32_t vsmScratchClipmapCount = 2u;
  VirtualShadowMapDrawPass::RasterMode vsmRasterMode = VirtualShadowMapDrawPass::RasterMode::ClipSpaceAtlasDepth;
  VirtualShadowMapManager::Settings::ShadowMapType vsmShadowMapType = VirtualShadowMapManager::Settings::ShadowMapType::VirtualShadowMap;
  SoftShadowMethod softShadowMethod = SoftShadowMethod::Hard;
  bool vsmEnableTableDebugPass = false;
  bool vsmEnableAtlasDebugPass = false;

  uint32_t screenSpaceShadowScale = 1u;
  bool enableSignedDistanceFields = true;
  uint32_t sdfSurfaceCachePageResolution = 4u;
  bool enableSoftwareRayTracing = false;
  uint32_t softwareRayTraceResolutionDivisor = 1u;
  bool enableWorldGI = true;
  bool worldGIDiffuseEnabled = true;
  bool worldGIGlossyEnabled = true;
  bool worldGIIndirectEnabled = true;
  float worldGIDiffuseWeight = 1.0f;
  float worldGIGlossyWeight = 1.0f;
  float worldGIIndirectIntensity = 1.0f;
  float worldGIGlossyMaxRoughness = 0.65f;
  uint32_t worldGIRayResolutionDivisor = 4u;
  uint32_t worldGIRaysPerPixel = 2u;
  uint32_t worldGIRayMaxSteps = 96u;
  float worldGIRayMinDistance = 0.02f;
  float worldGIRayMaxDistance = 64.0f;
  float worldGIRayNormalBias = 0.03f;
  bool worldGILightGridEnabled = true;
  uint32_t worldGILightGridResolution = globalillumination::worldgi::kWorldGILightGridDefaultResolution;
  uint32_t worldGILightGridCascadeCount = 8u;
  uint32_t worldGILightGridMaxItemsPerCell = globalillumination::worldgi::kWorldGILightGridDefaultMaxItemsPerCell;
  float worldGILightGridBaseExtent = 2000.0f;
  float worldGILightGridCascadeScale = 2.0f;
  bool enableSurfels = false;
  uint32_t screenProbeSpacing = 8u;
  uint32_t screenProbeCandidatesPerProbe = 32u;
  float worldShadingCacheWorldCellSize = 0.5f;
  float spatialRadianceCacheWorldCellSize = 0.5f;

  bool operator==(const RendererRebuildSettings &other) const
  {
    return displayOutput == other.displayOutput && diffuseGIOption == other.diffuseGIOption && enableDiffuseGI == other.enableDiffuseGI &&
           rsmGiSampleCount == other.rsmGiSampleCount && rsmGiSampleRadiusMeters == other.rsmGiSampleRadiusMeters && rsmGiIntensity == other.rsmGiIntensity &&
           rsmGiNormalBias == other.rsmGiNormalBias && rsmGiMinDistance == other.rsmGiMinDistance && rsmGiMaxDistance == other.rsmGiMaxDistance &&
           rsmGiTemporalAccumulation == other.rsmGiTemporalAccumulation && rsmGiTemporalFrames == other.rsmGiTemporalFrames &&
           rsmGiTemporalDisocclusionWeight == other.rsmGiTemporalDisocclusionWeight && rsmGiTemporalDepthSigma == other.rsmGiTemporalDepthSigma &&
           rsmGiTemporalNormalSigma == other.rsmGiTemporalNormalSigma && rsmGiValidRatioMin == other.rsmGiValidRatioMin && rsmGiFireflyClamp == other.rsmGiFireflyClamp &&
           rsmGiDenoiserEnabled == other.rsmGiDenoiserEnabled && rsmGiDenoiserFrames == other.rsmGiDenoiserFrames &&
           rsmGiDenoiserDepthSigma == other.rsmGiDenoiserDepthSigma && rsmGiDenoiserNormalSigma == other.rsmGiDenoiserNormalSigma &&
           rsmGiDenoiserLuminanceSigma == other.rsmGiDenoiserLuminanceSigma && rsmGiDenoiserValidRatioMin == other.rsmGiDenoiserValidRatioMin &&
           rsmGiDenoiserFireflyClamp == other.rsmGiDenoiserFireflyClamp && rsmGiDenoiserSpatialSigma == other.rsmGiDenoiserSpatialSigma &&
           rsmGiDenoiserSpatialRadius == other.rsmGiDenoiserSpatialRadius && rsmGiDenoiserSpatialStrength == other.rsmGiDenoiserSpatialStrength &&
           rsmGiDenoiserAtrousIterations == other.rsmGiDenoiserAtrousIterations && rsmGiDenoiserDisocclusionWeight == other.rsmGiDenoiserDisocclusionWeight &&
           rsmGiDenoiserMinCurrentWeight == other.rsmGiDenoiserMinCurrentWeight &&
           indirectGIOption == other.indirectGIOption && ismGiEnableVpls == other.ismGiEnableVpls && ismGiActiveVpls == other.ismGiActiveVpls &&
           ismGiPagesPerVplFace == other.ismGiPagesPerVplFace && ismGiPhysicalAtlasResolution == other.ismGiPhysicalAtlasResolution && ismGiPhysicalPageSize == other.ismGiPhysicalPageSize &&
           enableTsr == other.enableTsr && temporalRenderScale == other.temporalRenderScale && temporalJitterSamples == other.temporalJitterSamples &&
           temporalJitterScale == other.temporalJitterScale && enableTaa == other.enableTaa && taaAccumulationFrames == other.taaAccumulationFrames && taaMotionScale == other.taaMotionScale &&
           taaDepthThreshold == other.taaDepthThreshold && taaNormalThreshold == other.taaNormalThreshold && taaReactiveScale == other.taaReactiveScale && taaEdgeScale == other.taaEdgeScale &&
           taaSharpenStrength == other.taaSharpenStrength && taaDetailBoost == other.taaDetailBoost && prepassMaxHierarchyLevels == other.prepassMaxHierarchyLevels &&
           prepassClusterQueueMiB == other.prepassClusterQueueMiB && prepassEnableFrustumCulling == other.prepassEnableFrustumCulling && prepassEnableOcclusionCulling == other.prepassEnableOcclusionCulling &&
           finalMaxHierarchyLevels == other.finalMaxHierarchyLevels && finalClusterQueueMiB == other.finalClusterQueueMiB && finalEnableFrustumCulling == other.finalEnableFrustumCulling &&
           finalEnableOcclusionCulling == other.finalEnableOcclusionCulling && finalEnableStreamingPriorityUpdates == other.finalEnableStreamingPriorityUpdates &&
           finalSoftwarePixelsPerTriangleThreshold == other.finalSoftwarePixelsPerTriangleThreshold && depthPrePassMaxDrawnClusters == other.depthPrePassMaxDrawnClusters &&
           hardwareDrawMaxDrawnClusters == other.hardwareDrawMaxDrawnClusters && materialFeedbackRequestCapacity == other.materialFeedbackRequestCapacity && materialFeedbackStride == other.materialFeedbackStride &&
           materialMaxUploadsPerFrame == other.materialMaxUploadsPerFrame && materialTileSize == other.materialTileSize && materialTileWorkgroupSize == other.materialTileWorkgroupSize &&
           vsmPhysicalAtlasResolution == other.vsmPhysicalAtlasResolution && vsmRsmAtlasResolution == other.vsmRsmAtlasResolution && vsmPhysicalPageSize == other.vsmPhysicalPageSize && vsmCascadeCount == other.vsmCascadeCount &&
           vsmDirectionalFirstCascadeWorldExtentAuto == other.vsmDirectionalFirstCascadeWorldExtentAuto && vsmDirectionalFirstCascadeWorldExtent == other.vsmDirectionalFirstCascadeWorldExtent &&
           vsmDirectionalCascadeWorldExtentScale == other.vsmDirectionalCascadeWorldExtentScale && vsmPageWorldScale == other.vsmPageWorldScale && vsmMaxDirtyPagesPerFrame == other.vsmMaxDirtyPagesPerFrame &&
           vsmMaxFallbackPagesPerFrame == other.vsmMaxFallbackPagesPerFrame && vsmHierarchyQueueCapacity == other.vsmHierarchyQueueCapacity && vsmClusterQueueCapacity == other.vsmClusterQueueCapacity &&
           vsmMaxVisibleClusterDraws == other.vsmMaxVisibleClusterDraws && vsmScratchClipmapCount == other.vsmScratchClipmapCount && vsmRasterMode == other.vsmRasterMode &&
           vsmShadowMapType == other.vsmShadowMapType && softShadowMethod == other.softShadowMethod &&
           vsmEnableTableDebugPass == other.vsmEnableTableDebugPass && vsmEnableAtlasDebugPass == other.vsmEnableAtlasDebugPass && screenSpaceShadowScale == other.screenSpaceShadowScale && enableSignedDistanceFields == other.enableSignedDistanceFields &&
           sdfSurfaceCachePageResolution == other.sdfSurfaceCachePageResolution && enableSoftwareRayTracing == other.enableSoftwareRayTracing &&
           softwareRayTraceResolutionDivisor == other.softwareRayTraceResolutionDivisor &&
           enableWorldGI == other.enableWorldGI && worldGIRayResolutionDivisor == other.worldGIRayResolutionDivisor && worldGIRaysPerPixel == other.worldGIRaysPerPixel &&
           worldGIRayMaxSteps == other.worldGIRayMaxSteps && worldGIRayMinDistance == other.worldGIRayMinDistance && worldGIRayMaxDistance == other.worldGIRayMaxDistance &&
           worldGIRayNormalBias == other.worldGIRayNormalBias && worldGILightGridEnabled == other.worldGILightGridEnabled && worldGILightGridResolution == other.worldGILightGridResolution &&
           worldGILightGridCascadeCount == other.worldGILightGridCascadeCount && worldGILightGridMaxItemsPerCell == other.worldGILightGridMaxItemsPerCell &&
           worldGILightGridBaseExtent == other.worldGILightGridBaseExtent && worldGILightGridCascadeScale == other.worldGILightGridCascadeScale &&
           enableSurfels == other.enableSurfels && screenProbeSpacing == other.screenProbeSpacing && screenProbeCandidatesPerProbe == other.screenProbeCandidatesPerProbe && worldShadingCacheWorldCellSize == other.worldShadingCacheWorldCellSize &&
           spatialRadianceCacheWorldCellSize == other.spatialRadianceCacheWorldCellSize;
  }

  bool operator!=(const RendererRebuildSettings &other) const
  {
    return !(*this == other);
  }
};

struct RendererControlsOverlayResult
{
  bool runtimeSettingsChanged = false;
  bool rebuildRequested = false;
};

struct RendererVSDFBuildSettings
{
  float targetMajorAxisCells = 512.0f;
  float minTargetCellSize = 5.0e-4f;
  float adaptiveTargetError = 0.5f;
  uint32_t adaptiveMaxDepth = 4u;
  uint32_t closedMaxAxisResolution = 256u;
  uint32_t openMaxAxisResolution = 384u;
  uint32_t detailOpenMaxAxisResolution = 512u;
  uint32_t closedVoxelBudgetKi = 768u;
  uint32_t openVoxelBudgetKi = 512u;
  uint32_t detailOpenVoxelBudgetKi = 1536u;
  uint32_t thinSurfaceVoxelBudgetKi = 2048u;
  float featureAxisResolutionCapScale = 1.5f;
  float openBoundsPadCells = 1.5f;
  float closedBoundsPadCells = 2.0f;
  float openNarrowBandCells = 18.0f;
  float closedThinNarrowBandCells = 12.0f;
  float unsignedConservativeBiasCells = 0.5f;
  float unsignedGuardBandCells = 1.0f;
};

static RendererVSDFBuildSettings gRendererVSDFBuildSettings;

static bool isRendererRebuildSettingsValid(const RendererRebuildSettings &settings)
{
  if (!std::isfinite(settings.temporalRenderScale) || settings.temporalRenderScale <= 0.0f || settings.temporalRenderScale > 1.0f)
  {
    return false;
  }
  if (settings.temporalJitterSamples == 0u || !std::isfinite(settings.temporalJitterScale) || settings.temporalJitterScale < 0.0f)
  {
    return false;
  }
  if (settings.taaAccumulationFrames == 0u || !std::isfinite(settings.taaMotionScale) || settings.taaMotionScale < 0.0f || !std::isfinite(settings.taaDepthThreshold) || settings.taaDepthThreshold < 0.0f ||
      !std::isfinite(settings.taaNormalThreshold) || settings.taaNormalThreshold < 0.0f || settings.taaNormalThreshold > 1.0f || !std::isfinite(settings.taaReactiveScale) || settings.taaReactiveScale < 0.0f ||
      !std::isfinite(settings.taaEdgeScale) || settings.taaEdgeScale < 0.0f || !std::isfinite(settings.taaSharpenStrength) || settings.taaSharpenStrength < 0.0f || !std::isfinite(settings.taaDetailBoost) ||
      settings.taaDetailBoost < 0.0f)
  {
    return false;
  }
  if (settings.worldGIRayResolutionDivisor == 0u || settings.worldGIRaysPerPixel == 0u || settings.worldGIRaysPerPixel > 8u || settings.worldGIRayMaxSteps == 0u ||
      !std::isfinite(settings.worldGIRayMinDistance) || settings.worldGIRayMinDistance < 0.0f || !std::isfinite(settings.worldGIRayMaxDistance) ||
      settings.worldGIRayMaxDistance <= settings.worldGIRayMinDistance || !std::isfinite(settings.worldGIRayNormalBias) || settings.worldGIRayNormalBias < 0.0f)
  {
    return false;
  }
  if (settings.worldGILightGridResolution == 0u || settings.worldGILightGridResolution > 64u || settings.worldGILightGridCascadeCount == 0u ||
      settings.worldGILightGridCascadeCount > globalillumination::worldgi::kWorldGILightGridMaxCascades || settings.worldGILightGridMaxItemsPerCell == 0u ||
      settings.worldGILightGridMaxItemsPerCell > 256u || !std::isfinite(settings.worldGILightGridBaseExtent) || settings.worldGILightGridBaseExtent <= 0.0f ||
      !std::isfinite(settings.worldGILightGridCascadeScale) || settings.worldGILightGridCascadeScale < 1.0f)
  {
    return false;
  }
  if (settings.prepassMaxHierarchyLevels == 0u || settings.finalMaxHierarchyLevels == 0u)
  {
    return false;
  }
  if (settings.prepassClusterQueueMiB == 0u || settings.finalClusterQueueMiB == 0u)
  {
    return false;
  }
  if (settings.depthPrePassMaxDrawnClusters == 0u || settings.hardwareDrawMaxDrawnClusters == 0u)
  {
    return false;
  }
  if (settings.materialFeedbackRequestCapacity == 0u || settings.materialFeedbackStride == 0u || settings.materialMaxUploadsPerFrame == 0u)
  {
    return false;
  }
  if (settings.materialTileSize == 0u || settings.materialTileWorkgroupSize == 0u)
  {
    return false;
  }
  if (settings.vsmPhysicalAtlasResolution == 0u || settings.vsmPhysicalPageSize == 0u || settings.vsmCascadeCount == 0u)
  {
    return false;
  }
  const bool rsmPayloadRequested =
      settings.vsmShadowMapType == VirtualShadowMapManager::Settings::ShadowMapType::ReflexiveShadowMap ||
      (settings.enableDiffuseGI && settings.diffuseGIOption == DiffuseGIOption::ReflexiveShadowMaps) ||
      rendererDisplayOutputUsesRsmScreenOutput(settings.displayOutput);
  if (rsmPayloadRequested &&
      settings.vsmRsmAtlasResolution != 1024u && settings.vsmRsmAtlasResolution != 2048u && settings.vsmRsmAtlasResolution != 4096u &&
      settings.vsmRsmAtlasResolution != 8192u && settings.vsmRsmAtlasResolution != 16384u)
  {
    return false;
  }
  if (!std::isfinite(settings.vsmDirectionalFirstCascadeWorldExtent) || settings.vsmDirectionalFirstCascadeWorldExtent <= 0.0f)
  {
    return false;
  }
  if (!std::isfinite(settings.vsmDirectionalCascadeWorldExtentScale) || settings.vsmDirectionalCascadeWorldExtentScale <= 0.0f)
  {
    return false;
  }
  if (!std::isfinite(settings.vsmPageWorldScale) || settings.vsmPageWorldScale <= 0.0f)
  {
    return false;
  }
  if ((settings.vsmPhysicalAtlasResolution % settings.vsmPhysicalPageSize) != 0u)
  {
    return false;
  }
  if (settings.vsmMaxDirtyPagesPerFrame == 0u || settings.vsmMaxFallbackPagesPerFrame == 0u)
  {
    return false;
  }
  if (settings.vsmHierarchyQueueCapacity == 0u || settings.vsmClusterQueueCapacity == 0u || settings.vsmMaxVisibleClusterDraws == 0u || settings.vsmScratchClipmapCount == 0u)
  {
    return false;
  }
  if (settings.vsmRasterMode != VirtualShadowMapDrawPass::RasterMode::ClipSpaceAtlasDepth)
  {
    return false;
  }

  if (!(settings.screenProbeSpacing == 8u || settings.screenProbeSpacing == 16u || settings.screenProbeSpacing == 32u || settings.screenProbeSpacing == 64u || settings.screenProbeSpacing == 128u))
  {
    return false;
  }

  if (settings.screenProbeCandidatesPerProbe == 0u || settings.screenProbeCandidatesPerProbe > 128u)
  {
    return false;
  }

  if (!gpusigneddistancefields::GPUSignedDistanceFieldsManager::isSurfaceCachePageResolutionValid(settings.sdfSurfaceCachePageResolution))
  {
    return false;
  }

  if (!std::isfinite(settings.worldShadingCacheWorldCellSize) || settings.worldShadingCacheWorldCellSize <= 0.0f)
  {
    return false;
  }

  if (!std::isfinite(settings.spatialRadianceCacheWorldCellSize) || settings.spatialRadianceCacheWorldCellSize <= 0.0f)
  {
    return false;
  }

  if (settings.indirectGIOption != IndirectGIOption::ImperfectShadowMaps || settings.ismGiActiveVpls == 0u || settings.ismGiActiveVpls > 64u ||
      settings.ismGiPagesPerVplFace == 0u || settings.ismGiPagesPerVplFace > 64u ||
      settings.ismGiPhysicalAtlasResolution < 512u || settings.ismGiPhysicalAtlasResolution > settings.vsmPhysicalAtlasResolution ||
      settings.ismGiPhysicalPageSize < 32u || settings.ismGiPhysicalPageSize > 128u ||
      settings.ismGiPhysicalAtlasResolution % settings.ismGiPhysicalPageSize != 0u)
  {
    return false;
  }
  uint32_t ismFaceSide = 1u;
  const uint32_t requestedIsmFaceSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max(settings.ismGiPagesPerVplFace, 1u)))));
  while (ismFaceSide < requestedIsmFaceSide && ismFaceSide < VirtualShadowMapManager::DEFAULT_PAGE_TABLE_RESOLUTION)
  {
    ismFaceSide <<= 1u;
  }
  const uint32_t ismAtlasPagesPerAxis = settings.ismGiPhysicalAtlasResolution / settings.ismGiPhysicalPageSize;
  const uint64_t ismPhysicalPageCapacity = static_cast<uint64_t>(ismAtlasPagesPerAxis) * ismAtlasPagesPerAxis;
  const uint64_t ismRequestedPageCapacity =
      settings.ismGiEnableVpls ? static_cast<uint64_t>(settings.ismGiActiveVpls) * VirtualShadowMapManager::POINT_LIGHT_FACE_COUNT * ismFaceSide * ismFaceSide : 0u;
  if (ismRequestedPageCapacity > ismPhysicalPageCapacity)
  {
    return false;
  }

  return (settings.screenSpaceShadowScale == 1u || settings.screenSpaceShadowScale == 2u || settings.screenSpaceShadowScale == 4u) &&
         (settings.softwareRayTraceResolutionDivisor == 1u || settings.softwareRayTraceResolutionDivisor == 2u || settings.softwareRayTraceResolutionDivisor == 4u || settings.softwareRayTraceResolutionDivisor == 8u);
}

static RendererControlsOverlayResult buildShadowControlsOverlay(
    float &prepassLodErrorThreshold,
    float &geometryLodErrorThreshold,
    float &vsmLodErrorThreshold,
    float &finalSoftwarePixelsPerTriangleThreshold,
    bool &directionalLightEnabled,
    math::Vec3f &directionalLightColor,
    float &directionalLightIntensity,
    bool &pointLightEnabled,
    math::Vec3f &pointLightColor,
    float &pointLightIntensity,
    float &pointLightRange,
    bool &shadowPcfEnabled,
    ScreenSpaceSoftShadowSettings &screenSpaceSoftShadowSettings,
    uint32_t &shadowFilterTaps,
    uint32_t &fallbackShadowFilterTaps,
    uint32_t &pcssBlockerSearchTaps,
    float &directionalShadowBias,
    float &spotShadowBias,
    float &pointShadowBias,
    float &directionalSlopeScaleBias,
    float &spotSlopeScaleBias,
    float &pointSlopeScaleBias,
    float &directionalMaxShadowBias,
    float &spotMaxShadowBias,
    float &pointMaxShadowBias,
    float &pcfRadiusTexels,
    float &pcssLightRadiusTexels,
    float &pcssMaxPenumbraTexels,
    float &pcssPenumbraScale,
    float &vsmLocalLightMipTransitionScale,
    float &vsmLocalLightMipBias,
    float &directionalNormalBiasTexels,
    float &spotNormalBiasTexels,
    float &pointNormalBiasTexels,
    bool &normalLightingEnabled,
    float &normalLightingThreshold,
    float &normalLightingExponent,
    std::array<float, 3> &ambientShadowColor,
    bool &screenSpaceShadowEnabled,
    float &screenSpaceSurfaceThickness,
    float &screenSpaceBilinearThreshold,
    float &screenSpaceShadowContrast,
    std::array<float, 2> &screenSpaceDepthBounds,
    bool &screenSpaceIgnoreEdgePixels,
    bool &screenSpaceUsePrecisionOffset,
    bool &screenSpaceBilinearSamplingOffsetMode,
    bool &screenSpaceUseEarlyOut,
    bool &screenSpaceTreatSkippedEdgeSamplesAsLit,
    bool &screenSpaceDebugOutputEdgeMask,
    bool &screenSpaceDebugOutputThreadIndex,
    bool &screenSpaceDebugOutputWaveIndex,
    bool &vsmPagesOverlayEnabled,
    float &vsmPagesOverlayOpacity,
    bool &spatialRadianceCacheOverlayEnabled,
    float &spatialRadianceCacheOverlayOpacity,
    bool &ismVplPointsOverlayEnabled,
    float &ismVplPointsOverlayOpacity,
    bool &worldGILightGridOverlayEnabled,
    float &worldGILightGridOverlayOpacity,
    int &worldGILightGridOverlayCascade,
    float &worldGILightGridOverlayLineWidth,
    bool &screenProbeScreenSpaceTracingEnabled,
    float &screenProbeRayMaxDistance,
    float &screenProbeRayOriginBias,
    uint32_t &screenProbeScreenSpaceMaxSteps,
    float &screenProbeScreenSpaceStepPixels,
    float &screenProbeScreenSpaceDepthThickness,
    float &screenProbeScreenSpaceRelativeThickness,
    bool &screenProbeSdfFallbackEnabled,
    uint32_t &screenProbeSdfMaxSteps,
    float &screenProbeSdfMaxDistance,
    float &screenProbeSdfHitThresholdScale,
    float &screenProbeSdfEmptySpaceStepScale,
    float &screenProbeSdfScreenNormalMatchBaseThreshold,
    float &screenProbeSdfScreenNormalMatchCascadeScale,
    bool &sdfTraceSurfaceCacheMaterialsEnabled,
    bool &screenProbeSkyEnabled,
    uint32_t &screenProbeGuidedCandidateCount,
    float &screenProbeTemporalBlend,
    float &screenProbeHistoryWeightScale,
    float &screenProbeRecursiveBounceBlend,
    float &screenProbeDisocclusionThreshold,
    float &screenProbePlaneDistanceThreshold,
    float &screenProbeNormalThreshold,
    float &screenProbeResolveFirstCacheBias,
    float &screenProbeResolveSecondCacheBias,
    float &screenProbeResolvePlaneDistanceScale,
    float &screenProbeResolveNormalWeightExponent,
    globalillumination::surfels::SurfelHierarchySettings &surfelHierarchySettings,
    globalillumination::surfels::SurfelRuntimeSettings &surfelRuntimeSettings,
    bool &surfelHierarchyFreeze,
    uint32_t surfelAllocatedCount,
    bool &screenSpaceRayTraceEnabled,
    bool &screenSpaceRayTraceScreenTracesEnabled,
    bool &screenSpaceRayTraceForceWorldFallback,
    bool &screenSpaceRayTraceTraceAllForcedFallbackRays,
    uint32_t screenSpaceRayTraceViewportWidth,
    uint32_t screenSpaceRayTraceViewportHeight,
    uint32_t &screenSpaceRayTraceTileSize,
    uint32_t &screenSpaceRayTraceRaysPerTile,
    uint32_t &screenSpaceRayTraceMaxSteps,
    uint32_t &screenSpaceRayTraceMaxFailedRays,
    bool &screenSpaceRayTraceSdfFallbackEnabled,
    bool &screenSpaceRayTraceSoftwareFallbackEnabled,
    bool &screenSpaceRayTraceSoftwareFallbackTraceStats,
    uint32_t &screenSpaceRayTraceSoftwareFallbackBudget,
    float &screenSpaceRayTraceMinDistance,
    float &screenSpaceRayTraceMaxDistance,
    float &screenSpaceRayTraceSurfaceThickness,
    float &screenSpaceRayTraceOriginBias,
    const ScreenSpaceRayTraceStats &screenSpaceRayTraceStats,
    float &softwareRayTraceMinDistance,
    float &softwareRayTraceMaxDistance,
    uint32_t &softwareRayTraceHeatmapMaxIterations,
    uint32_t &softwareRayTracePrimitiveMode,
    uint32_t &softwareRayTraceTraversalMode,
    uint32_t &softwareRayTraceTlasMode,
    uint32_t &softwareRayTraceFallbackLod,
    uint32_t &softwareRayTraceRbvhLod,
    uint32_t &softwareRayTraceShaderVariant,
    bool &softwareRayTraceOverlayTLASBounds,
    bool &softwareRayTraceOverlayTLASCwbvhBounds,
    uint32_t &softwareRayTraceOverlayTLASLevel,
    float &softwareRayTraceOverlayLineThickness,
    bool &softwareRayTraceTraversalEarlyOut,
    float &softwareRayTraceTraversalEarlyOutBias,
    bool &softwareRayTraceIsolateTLASInstance,
    uint32_t &softwareRayTraceSelectedTLASInstance,
    uint32_t softwareRayTraceTLASCandidateInstanceCount,
    uint32_t softwareRayTraceTLASFilteredInstanceCount,
    virtualgeometry::SceneInstanceId softwareRayTraceSelectedTLASSceneInstanceId,
    uint32_t softwareRayTraceTLASValidInstanceCount,
    uint32_t softwareRayTraceTLASSkippedMissingBlasCount,
    uint32_t softwareRayTraceTLASSkippedInvalidBoundsCount,
    uint32_t softwareRayTraceTLASCanonicalNodeCount,
    uint32_t softwareRayTraceTLASCwbvhNodeCount,
    uint32_t softwareRayTraceTLASBvh4NodeCount,
    uint32_t softwareRayTraceTLASInstanceCount,
    float softwareRayTraceTLASBvh4BuildSahScore,
    float softwareRayTraceTLASBvh4CurrentSahScore,
    float softwareRayTraceTLASBvh4SahGrowth,
    bool softwareRayTraceTLASBvh4RebuiltForSah,
    uint32_t softwareRayTraceBLASCount,
    uint32_t softwareRayTraceBLASCanonicalNodeCount,
    uint32_t softwareRayTraceBLASCwbvhNodeCount,
    uint32_t softwareRayTraceBLASPrimitiveCount,
    uint32_t softwareRayTraceBLASRbvhCardCount,
    uint32_t softwareRayTraceBLASPreparedCount,
    uint32_t softwareRayTraceBLASAvailableFallbackCount,
    uint32_t softwareRayTraceBLASMissingFallbackCount,
    uint32_t softwareRayTraceBLASSkippedMissingFallbackCount,
    const ScreenProbeTraceStats &screenProbeTraceStats,
    const SoftwareRayTraceFrameStats &softwareRayTraceStats,
    const SoftwareRayTraceFrameStats &screenFallbackTraceStats,
    const SurfelTraceStats &surfelTraceStats,
    const SurfelIrradianceDebugStats &surfelIrradianceDebugStats,
    int &vsmDebugLayer,
    int &ismVplDebugLayer,
    int &ismVplDebugSelectedVpl,
    int &ismVplProjectedLightIndex,
    float &ismVplDebugPointRadius,
    float &ismVplDebugExposure,
    uint32_t ismVplRuntimeProxyCount,
    uint32_t ismVplRuntimeAllocatedCount,
    uint32_t ismVplRuntimeManagerLightCount,
    uint32_t ismVplRuntimeLayerCount,
    uint32_t ismVplRuntimePageTableResolution,
    uint32_t ismVplRuntimePhysicalPageTableResolution,
    uint32_t ismVplRuntimePhysicalPageSize,
    const IsmVplBuildStats &ismVplBuildStats,
    int &vsmProjectedLightIndex,
    RendererRebuildSettings &editableRebuildSettings,
    const RendererRebuildSettings &appliedRebuildSettings)
{
  RendererControlsOverlayResult result{};
  const globalillumination::surfels::SurfelHierarchySettings initialSurfelHierarchySettings = surfelHierarchySettings;
  const globalillumination::surfels::SurfelRuntimeSettings initialSurfelRuntimeSettings = surfelRuntimeSettings;
  static constexpr uint32_t kZero = 0u;
  static constexpr uint32_t kOne = 1u;
  static constexpr uint32_t kFour = 4u;
  static constexpr uint32_t kSix = 6u;
  static constexpr uint32_t kEight = 8u;
  static constexpr uint32_t kTwentyFour = 24u;
  static constexpr uint32_t kSixteen = 16u;
  static constexpr uint32_t kThirtyTwo = 32u;
  static constexpr uint32_t kThirtyOne = 31u;
  static constexpr uint32_t kSixtyFour = 64u;
  static constexpr uint32_t kOneTwentyEight = 128u;
  static constexpr uint32_t kTwoFiftySix = 256u;
  static constexpr uint32_t kFiveTwelve = 512u;
  static constexpr uint32_t kOneKi = 1024u;
  static constexpr uint32_t kTwoKi = 2048u;
  static constexpr uint32_t kFourKi = 4096u;
  static constexpr uint32_t kTenThousand = 10000u;
  static constexpr uint32_t kSixteenKi = 16384u;
  static constexpr uint32_t kEightKi = 8192u;
  static constexpr uint32_t kSixtyFiveKi = 65536u;
  static constexpr uint32_t kTwoSixtyTwoKi = 262144u;
  static constexpr uint32_t kFourMi = 4u * 1024u * 1024u;
  auto ensureScreenSpaceRayTraceFallback =
      [&]()
  {
    if (screenSpaceRayTraceSdfFallbackEnabled || screenSpaceRayTraceSoftwareFallbackEnabled)
      return;
    if (kRendererEnableSoftwareRayTracingPath)
    {
      screenSpaceRayTraceSoftwareFallbackEnabled = true;
    }
    else if (editableRebuildSettings.enableSignedDistanceFields)
    {
      screenSpaceRayTraceSdfFallbackEnabled = true;
    }
  };
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float windowWidth = std::min(420.0f, viewport->WorkSize.x - outerMargin * 2.0f);
  const float windowX = viewport->WorkPos.x + (viewport->WorkSize.x - windowWidth) * 0.5f;
  const float windowY = viewport->WorkPos.y + outerMargin;

  ImGui::SetNextWindowPos(ImVec2(windowX, windowY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(windowWidth, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);

  ImGui::Begin("Shadow Controls", nullptr, windowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::TextUnformatted("Runtime renderer controls");
  ImGui::TextDisabled("Tab / Shift+Tab / arrows work while mouse-look stays active.");
  ImGui::SeparatorText("Culling");
  ImGui::TextDisabled("Lower thresholds refine earlier. Higher thresholds keep coarser hierarchy nodes and clusters longer.");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Prepass hierarchy error", &prepassLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Final cluster error", &geometryLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Final software px/triangle", &finalSoftwarePixelsPerTriangleThreshold, 1.0f, 64.0f, "%.1f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("VSM shadow-camera error", &vsmLodErrorThreshold, 0.1f, 16.0f, "%.2f px");
  ImGui::SeparatorText("Lights");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Directional light", &directionalLightEnabled);
  result.runtimeSettingsChanged |= ImGui::ColorEdit3("Directional color", &directionalLightColor[0], ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Directional intensity", &directionalLightIntensity, 0.0f, 16.0f, "%.2f");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Point light", &pointLightEnabled);
  result.runtimeSettingsChanged |= ImGui::ColorEdit3("Point color", &pointLightColor[0], ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point intensity", &pointLightIntensity, 0.0f, 16.0f, "%.2f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point radius", &pointLightRange, 1.0f, 128.0f, "%.1f");
  ImGui::SeparatorText("VSM");
  int softShadowMethodIndex = static_cast<int>(editableRebuildSettings.softShadowMethod);
  if (ImGui::Combo("Soft shadow method", &softShadowMethodIndex, "Hard\0PCF\0PCSS\0Layered SSSS\0"))
  {
    editableRebuildSettings.softShadowMethod = static_cast<SoftShadowMethod>(std::clamp(softShadowMethodIndex, 0, 3));
    shadowPcfEnabled = editableRebuildSettings.softShadowMethod != SoftShadowMethod::Hard;
    result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
  }
  result.runtimeSettingsChanged |= ImGui::Checkbox("Enable PCF", &shadowPcfEnabled);
  if (editableRebuildSettings.softShadowMethod == SoftShadowMethod::LayeredScreenSpaceSoftShadows)
  {
    int ssssScaleIndex = screenSpaceSoftShadowSettings.resolutionScale == 4u ? 2 : (screenSpaceSoftShadowSettings.resolutionScale == 2u ? 1 : 0);
    if (ImGui::Combo("SSSS resolution", &ssssScaleIndex, "1x\0" "1/2\0" "1/4\0"))
    {
      screenSpaceSoftShadowSettings.resolutionScale = ssssScaleIndex == 2 ? 4u : (ssssScaleIndex == 1 ? 2u : 1u);
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    int ssssKernelIndex = screenSpaceSoftShadowSettings.kernelSize == 23u ? 2 : (screenSpaceSoftShadowSettings.kernelSize == 5u ? 0 : 1);
    if (ImGui::Combo("SSSS kernel", &ssssKernelIndex, "5\0" "11\0" "23\0"))
    {
      screenSpaceSoftShadowSettings.kernelSize = ssssKernelIndex == 2 ? 23u : (ssssKernelIndex == 0 ? 5u : 11u);
      result.runtimeSettingsChanged = true;
    }
    if (ImGui::SliderScalar("SSSS max layers", ImGuiDataType_U32, &screenSpaceSoftShadowSettings.maxSoftShadowLayers, &kOne, &kThirtyTwo))
    {
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    result.runtimeSettingsChanged |= ImGui::SliderScalar("SSSS blocker taps", ImGuiDataType_U32, &screenSpaceSoftShadowSettings.blockerSearchTaps, &kOne, &kThirtyOne);
    result.runtimeSettingsChanged |= ImGui::Checkbox("SSSS min filter", &screenSpaceSoftShadowSettings.useMinFilter);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSSS light radius", &screenSpaceSoftShadowSettings.defaultLightRadiusWorld, 0.01f, 8.0f, "%.2f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSSS penumbra scale", &screenSpaceSoftShadowSettings.penumbraScale, 0.1f, 8.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSSS grazing threshold", &screenSpaceSoftShadowSettings.grazingThreshold, 0.01f, 1.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSSS depth reject", &screenSpaceSoftShadowSettings.depthRejectThreshold, 0.001f, 0.25f, "%.3f");
    result.runtimeSettingsChanged |= ImGui::Checkbox("SSSS translucent shadows", &screenSpaceSoftShadowSettings.translucentShadowsEnabled);
    int ssssDebugModeIndex = static_cast<int>(screenSpaceSoftShadowSettings.debugMode);
    if (ImGui::Combo("SSSS debug", &ssssDebugModeIndex, "Hard shadow\0Penumbra\0Translucency\0Horizontal blur\0Blurred visibility\0Layer\0"))
    {
      screenSpaceSoftShadowSettings.debugMode = static_cast<ScreenSpaceSoftShadowSettings::DebugMode>(std::clamp(ssssDebugModeIndex, 0, 5));
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderScalar("SSSS debug layer", ImGuiDataType_U32, &screenSpaceSoftShadowSettings.debugLayer, &kZero, &kThirtyOne);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSSS debug scale", &screenSpaceSoftShadowSettings.debugValueScale, 0.01f, 16.0f, "%.2f");
  }

  int tapIndex = shadowPcfTapCountToIndex(shadowFilterTaps);
  if (ImGui::SliderInt("PCF taps", &tapIndex, 0, 7))
  {
    shadowFilterTaps = shadowPcfTapIndexToCount(tapIndex);
    result.runtimeSettingsChanged = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%u", shadowFilterTaps);

  result.runtimeSettingsChanged |= ImGui::SliderFloat("PCF radius", &pcfRadiusTexels, 0.0f, 6.0f, "%.2f texels");
  if (editableRebuildSettings.softShadowMethod == SoftShadowMethod::PCSS)
  {
    result.runtimeSettingsChanged |= ImGui::SliderScalar("PCSS blocker taps", ImGuiDataType_U32, &pcssBlockerSearchTaps, &kOne, &kThirtyOne);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("PCSS light radius", &pcssLightRadiusTexels, 0.0f, 16.0f, "%.2f texels");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("PCSS max penumbra", &pcssMaxPenumbraTexels, 0.0f, 48.0f, "%.2f texels");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("PCSS penumbra scale", &pcssPenumbraScale, 0.0f, 8.0f, "%.2f");
  }
  int fallbackTapIndex = shadowPcfTapCountToIndex(fallbackShadowFilterTaps);
  if (ImGui::SliderInt("Fallback PCF taps", &fallbackTapIndex, 0, 7))
  {
    fallbackShadowFilterTaps = shadowPcfTapIndexToCount(fallbackTapIndex);
    result.runtimeSettingsChanged = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%u", fallbackShadowFilterTaps);
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Local-light mip scale", &vsmLocalLightMipTransitionScale, 0.5f, 8.0f, "%.2fx");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Local-light mip bias", &vsmLocalLightMipBias, -2.0f, 2.0f, "%.2f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Dir shadow bias", &directionalShadowBias, 0.0f, 0.02f, "%.5f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Spot shadow bias", &spotShadowBias, 0.0f, 0.02f, "%.5f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point shadow bias", &pointShadowBias, 0.0f, 0.02f, "%.7f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Dir slope bias", &directionalSlopeScaleBias, 0.0f, 8.0f, "%.3f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Spot slope bias", &spotSlopeScaleBias, 0.0f, 8.0f, "%.3f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point slope bias", &pointSlopeScaleBias, 0.0f, 8.0f, "%.3f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Dir max bias", &directionalMaxShadowBias, 0.0f, 0.03f, "%.5f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Spot max bias", &spotMaxShadowBias, 0.0f, 0.03f, "%.5f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point max bias", &pointMaxShadowBias, 0.0f, 0.03f, "%.5f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Dir normal bias", &directionalNormalBiasTexels, 0.0f, 8.0f, "%.2f texels");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Spot normal bias", &spotNormalBiasTexels, 0.0f, 8.0f, "%.2f texels");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Point normal bias", &pointNormalBiasTexels, 0.0f, 8.0f, "%.2f texels");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Normal-angle lighting", &normalLightingEnabled);
  if (normalLightingEnabled)
  {
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Normal-angle threshold", &normalLightingThreshold, 0.0f, 0.95f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Normal-angle exponent", &normalLightingExponent, 0.25f, 8.0f, "%.2f");
  }
  result.runtimeSettingsChanged |= ImGui::ColorEdit3("Shadow ambient", ambientShadowColor.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay VSM pages", &vsmPagesOverlayEnabled);
  if (vsmPagesOverlayEnabled)
  {
    result.runtimeSettingsChanged |= ImGui::SliderFloat("VSM overlay opacity", &vsmPagesOverlayOpacity, 0.0f, 1.0f, "%.2f");
  }
  result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay spatial cache", &spatialRadianceCacheOverlayEnabled);
  if (spatialRadianceCacheOverlayEnabled)
  {
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Spatial cache opacity", &spatialRadianceCacheOverlayOpacity, 0.0f, 1.0f, "%.2f");
  }
  result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay VPL points", &ismVplPointsOverlayEnabled);
  if (ismVplPointsOverlayEnabled)
  {
    result.runtimeSettingsChanged |= ImGui::SliderFloat("VPL point overlay opacity", &ismVplPointsOverlayOpacity, 0.0f, 1.0f, "%.2f");
  }
  result.runtimeSettingsChanged |= ImGui::Combo("Project pages for", &vsmProjectedLightIndex, "Directional Light\0Point Light\0");
  result.runtimeSettingsChanged |= ImGui::SliderInt("VSM debug layer", &vsmDebugLayer, -1, 63);

  ImGui::SeparatorText("Screen Space Contact Shadow");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact shadow", &screenSpaceShadowEnabled);
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Contact surface thickness", &screenSpaceSurfaceThickness, 0.0005f, 0.05f, "%.4f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Contact bilinear threshold", &screenSpaceBilinearThreshold, 0.0f, 0.25f, "%.3f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Contact contrast", &screenSpaceShadowContrast, 0.25f, 8.0f, "%.2f");
  result.runtimeSettingsChanged |= ImGui::SliderFloat2("Contact depth bounds", screenSpaceDepthBounds.data(), 0.0f, 1.0f, "%.3f");
  if (screenSpaceDepthBounds[0] > screenSpaceDepthBounds[1])
  {
    std::swap(screenSpaceDepthBounds[0], screenSpaceDepthBounds[1]);
    result.runtimeSettingsChanged = true;
  }
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact ignore edge pixels", &screenSpaceIgnoreEdgePixels);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact precision offset", &screenSpaceUsePrecisionOffset);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact bilinear offset mode", &screenSpaceBilinearSamplingOffsetMode);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact early out", &screenSpaceUseEarlyOut);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact skipped edges are fully lit", &screenSpaceTreatSkippedEdgeSamplesAsLit);
  ImGui::SeparatorText("Contact Shadow Debug");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact edge mask", &screenSpaceDebugOutputEdgeMask);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact thread index", &screenSpaceDebugOutputThreadIndex);
  result.runtimeSettingsChanged |= ImGui::Checkbox("Contact wave index", &screenSpaceDebugOutputWaveIndex);

  ImGui::SeparatorText("Screen Probe Tracing");
  result.runtimeSettingsChanged |= ImGui::Checkbox("Screen-space probe tracing", &screenProbeScreenSpaceTracingEnabled);
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe ray max distance", &screenProbeRayMaxDistance, 1.0f, 100000.0f, "%.1f m");
  result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe ray origin bias", &screenProbeRayOriginBias, 0.0f, 1.0f, "%.3f m");
  if (screenProbeScreenSpaceTracingEnabled)
  {
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Probe SS max steps", ImGuiDataType_U32, &screenProbeScreenSpaceMaxSteps, &kEight, &kOneTwentyEight);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SS step stride", &screenProbeScreenSpaceStepPixels, 0.5f, 8.0f, "%.2f px");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SS thickness", &screenProbeScreenSpaceDepthThickness, 0.001f, 0.50f, "%.3f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SS rel thickness", &screenProbeScreenSpaceRelativeThickness, 0.0f, 0.25f, "%.3f");
  }

  const bool sdfFallbackAvailable = editableRebuildSettings.enableSignedDistanceFields;
  if (!sdfFallbackAvailable)
    ImGui::BeginDisabled();
  result.runtimeSettingsChanged |= ImGui::Checkbox("Probe SDF fallback", &screenProbeSdfFallbackEnabled);
  if (screenProbeSdfFallbackEnabled && sdfFallbackAvailable)
  {
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Probe SDF max steps", ImGuiDataType_U32, &screenProbeSdfMaxSteps, &kEight, &kOneTwentyEight);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SDF max distance", &screenProbeSdfMaxDistance, 1.0f, 100000.0f, "%.1f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SDF hit threshold", &screenProbeSdfHitThresholdScale, 0.10f, 2.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SDF empty-space scale", &screenProbeSdfEmptySpaceStepScale, 0.25f, 2.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SDF screen-normal base", &screenProbeSdfScreenNormalMatchBaseThreshold, 0.0f, 0.5f, "%.3f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Probe SDF screen-normal cascade", &screenProbeSdfScreenNormalMatchCascadeScale, 0.0f, 4.0f, "%.2f x voxel");
  }
  if (!sdfFallbackAvailable)
    ImGui::EndDisabled();
  if (!sdfFallbackAvailable)
  {
    screenProbeSdfFallbackEnabled = false;
    ImGui::TextDisabled("Enable Signed Distance Fields in the rebuild settings to use fallback tracing.");
  }
  result.runtimeSettingsChanged |= ImGui::Checkbox("Probe sky fallback", &screenProbeSkyEnabled);
  ImGui::TextDisabled("Debug tip: disable screen-space + enable SDF + disable sky to isolate pure SDF hits.");
  ImGui::TextDisabled(
      "AO debug outputs: Distance=nearest SDF hit distance in cascade-voxel units, Cascade=nearest SDF cascade, Flags=green accepted, yellow coarse-overlap rejected, red uninitialized, blue near-surface rejected, "
      "Steps=nearest SDF step ratio.");
  ImGui::TextDisabled("Display Output > Screen Probes: first-cache reservoir estimate. Phase 7 outputs now live under the Screen Probe Indirect/Bent Normal/AO views.");
  ImGui::Text(
      "Trace stats: active %u  screen %u  sdf %u  sky %u  miss %u",
      screenProbeTraceStats.activeRays,
      screenProbeTraceStats.resolvedScreenHits,
      screenProbeTraceStats.resolvedSdfHits,
      screenProbeTraceStats.resolvedSky,
      screenProbeTraceStats.resolvedMisses);
  ImGui::TextDisabled(
      "Raw hits: SS %u invalid %u  SDF %u invalid %u", screenProbeTraceStats.screenRawHits, screenProbeTraceStats.screenInvalidDirections, screenProbeTraceStats.sdfRawHits, screenProbeTraceStats.sdfInvalidDirections);
  ImGui::TextDisabled("Probe origins: finite %u inside-cascade %u", screenProbeTraceStats.originFinite, screenProbeTraceStats.originInsideAnyCascade);
  ImGui::TextDisabled(
      "SDF input: active %u inside-cascade %u dir-finite %u valid-range %u",
      screenProbeTraceStats.sdfInputActive,
      screenProbeTraceStats.sdfInputInsideAnyCascade,
      screenProbeTraceStats.sdfInputFiniteDirection,
      screenProbeTraceStats.sdfInputValidRange);
  ImGui::TextDisabled("SDF output: metadata-written %u metadata-match %u", screenProbeTraceStats.sdfOutputMetadataWritten, screenProbeTraceStats.sdfOutputMetadataMatch);
  ImGui::TextDisabled(
      "SDF traversal: cascade %u root %u tree %u mid %u bottom %u brick %u uninit %u",
      screenProbeTraceStats.sdfHasCascade,
      screenProbeTraceStats.sdfHasRootAabb,
      screenProbeTraceStats.sdfHasTreeRoot,
      screenProbeTraceStats.sdfHasMidAabb,
      screenProbeTraceStats.sdfHasBottomAabb,
      screenProbeTraceStats.sdfHasValidBrick,
      screenProbeTraceStats.sdfHasUninitialized);

  ImGui::SeparatorText("Render Graph Rebuild");
  ImGui::TextDisabled("Display output previews immediately; Software RT and fallback scene outputs recompile the renderer.");
  if (ImGui::BeginCombo("Display Output", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput)))
  {
    for (const RendererDisplayOutput output : kRendererDisplayOutputs)
    {
      if (!kRendererEnableSoftwareRayTracingPath && rendererDisplayOutputIsSoftwareRayTracing(output))
      {
        continue;
      }
      if (rendererDisplayOutputUsesSoftwareRayTracing(output) && !editableRebuildSettings.enableSoftwareRayTracing)
      {
        continue;
      }
      const bool isSelected = editableRebuildSettings.displayOutput == output;
      if (ImGui::Selectable(rendererDisplayOutputLabel(output), isSelected))
      {
        const RendererDisplayOutput previousOutput = editableRebuildSettings.displayOutput;
        if (previousOutput != output)
        {
          editableRebuildSettings.displayOutput = output;
          if (output == RendererDisplayOutput::SoftwareRayTraceLocalAlbedo)
          {
            editableRebuildSettings.enableSoftwareRayTracing = true;
            softwareRayTraceTraversalMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance);
          }
          else if (output == RendererDisplayOutput::SoftwareRayTraceWorldCwbvhAlbedo)
          {
            editableRebuildSettings.enableSoftwareRayTracing = true;
            softwareRayTraceTraversalMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
            softwareRayTracePrimitiveMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle);
            softwareRayTraceTlasMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH);
          }
          else if (rendererDisplayOutputUsesLocalInstanceSdf(output))
          {
            editableRebuildSettings.enableSignedDistanceFields = true;
          }
          result.runtimeSettingsChanged = true;
          if (rendererDisplayOutputIsSoftwareRayTracing(previousOutput) || rendererDisplayOutputIsSoftwareRayTracing(output) ||
              rendererDisplayOutputUsesFallbackScene(previousOutput) || rendererDisplayOutputUsesFallbackScene(output) ||
              rendererDisplayOutputUsesRsmScreenOutput(previousOutput) || rendererDisplayOutputUsesRsmScreenOutput(output) ||
              rendererDisplayOutputUsesLocalInstanceSdf(previousOutput) || rendererDisplayOutputUsesLocalInstanceSdf(output) ||
              rendererDisplayOutputTargetSlot(previousOutput) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace ||
              rendererDisplayOutputTargetSlot(output) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace ||
              rendererDisplayOutputTargetSlot(previousOutput) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas ||
              rendererDisplayOutputTargetSlot(output) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas)
          {
            result.rebuildRequested = true;
          }
        }
      }
      if (isSelected)
      {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  if (ImGui::CollapsingHeader("Diffuse GI", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::Checkbox("Enable diffuse GI", &editableRebuildSettings.enableDiffuseGI))
    {
      result.rebuildRequested = true;
    }

    if (!editableRebuildSettings.enableDiffuseGI)
    {
      ImGui::TextDisabled("Enable this to build a screen-space indirect diffuse output from the selected method.");
    }
    else if (ImGui::BeginCombo("Diffuse GI method", diffuseGIOptionLabel(editableRebuildSettings.diffuseGIOption)))
    {
      constexpr std::array<DiffuseGIOption, 2> diffuseGIOptions = {
        DiffuseGIOption::ReflexiveShadowMaps,
        DiffuseGIOption::ImperfectShadowMaps,
      };
      for (const DiffuseGIOption option : diffuseGIOptions)
      {
        const bool isSelected = editableRebuildSettings.diffuseGIOption == option;
        if (ImGui::Selectable(diffuseGIOptionLabel(option), isSelected))
        {
          editableRebuildSettings.diffuseGIOption = option;
          result.rebuildRequested = true;
        }
        if (isSelected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    if (editableRebuildSettings.enableDiffuseGI && editableRebuildSettings.diffuseGIOption == DiffuseGIOption::ReflexiveShadowMaps)
    {
      ImGui::TextDisabled("Screen-space indirect diffuse from the active RSM light payloads.");
      ImGui::SliderScalar("RSM GI samples", ImGuiDataType_U32, &editableRebuildSettings.rsmGiSampleCount, &kOne, &kSixtyFour);
      ImGui::SliderFloat("RSM GI radius", &editableRebuildSettings.rsmGiSampleRadiusMeters, 0.25f, 1024.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("RSM GI intensity", &editableRebuildSettings.rsmGiIntensity, 0.0f, 128.0f, "%.2f");
      ImGui::SliderFloat("RSM GI normal bias", &editableRebuildSettings.rsmGiNormalBias, 0.0f, 1.0f, "%.3f m");
      ImGui::SliderFloat("RSM GI min distance", &editableRebuildSettings.rsmGiMinDistance, 0.01f, 4.0f, "%.2f m");
      ImGui::SliderFloat("RSM GI max distance", &editableRebuildSettings.rsmGiMaxDistance, 1.0f, 512.0f, "%.0f m");
      ImGui::Checkbox("RSM GI temporal accumulation", &editableRebuildSettings.rsmGiTemporalAccumulation);
      ImGui::SliderScalar("RSM GI temporal frames", ImGuiDataType_U32, &editableRebuildSettings.rsmGiTemporalFrames, &kOne, &kSixtyFour);
      ImGui::SliderFloat("RSM GI temporal disocclusion", &editableRebuildSettings.rsmGiTemporalDisocclusionWeight, 0.0f, 4.0f, "%.2f");
      ImGui::SliderFloat("RSM GI temporal depth sigma", &editableRebuildSettings.rsmGiTemporalDepthSigma, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("RSM GI temporal normal sigma", &editableRebuildSettings.rsmGiTemporalNormalSigma, 1.0f, 64.0f, "%.1f");
      ImGui::SliderFloat("RSM GI valid ratio min", &editableRebuildSettings.rsmGiValidRatioMin, 0.01f, 1.0f, "%.2f");
      ImGui::SliderFloat("RSM GI firefly clamp", &editableRebuildSettings.rsmGiFireflyClamp, 1.0f, 512.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
      ImGui::Checkbox("RSM GI SVGF denoiser", &editableRebuildSettings.rsmGiDenoiserEnabled);
      ImGui::SliderScalar("RSM GI denoiser frames", ImGuiDataType_U32, &editableRebuildSettings.rsmGiDenoiserFrames, &kOne, &kSixtyFour);
      ImGui::SliderFloat("RSM GI denoiser depth sigma", &editableRebuildSettings.rsmGiDenoiserDepthSigma, 0.0001f, 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("RSM GI denoiser normal sigma", &editableRebuildSettings.rsmGiDenoiserNormalSigma, 1.0f, 128.0f, "%.1f");
      ImGui::SliderFloat("RSM GI denoiser luminance sigma", &editableRebuildSettings.rsmGiDenoiserLuminanceSigma, 0.01f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("RSM GI denoiser valid ratio min", &editableRebuildSettings.rsmGiDenoiserValidRatioMin, 0.01f, 1.0f, "%.2f");
      ImGui::SliderFloat("RSM GI denoiser firefly clamp", &editableRebuildSettings.rsmGiDenoiserFireflyClamp, 1.0f, 512.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat("RSM GI denoiser spatial sigma", &editableRebuildSettings.rsmGiDenoiserSpatialSigma, 0.1f, 8.0f, "%.2f");
      ImGui::SliderFloat("RSM GI denoiser radius", &editableRebuildSettings.rsmGiDenoiserSpatialRadius, 1.0f, 8.0f, "%.1f");
      ImGui::SliderFloat("RSM GI denoiser spatial strength", &editableRebuildSettings.rsmGiDenoiserSpatialStrength, 0.0f, 1.0f, "%.2f");
      ImGui::SliderScalar("RSM GI denoiser atrous passes", ImGuiDataType_U32, &editableRebuildSettings.rsmGiDenoiserAtrousIterations, &kOne, &kSixteen);
      ImGui::SliderFloat("RSM GI denoiser disocclusion", &editableRebuildSettings.rsmGiDenoiserDisocclusionWeight, 0.0f, 8.0f, "%.2f");
      ImGui::SliderFloat("RSM GI denoiser min current weight", &editableRebuildSettings.rsmGiDenoiserMinCurrentWeight, 0.01f, 1.0f, "%.2f");
      if (ImGui::BeginCombo("RSM GI atlas", std::to_string(editableRebuildSettings.vsmRsmAtlasResolution).c_str()))
      {
        static constexpr uint32_t kRsmAtlasResolutions[] = {1024u, 2048u, 4096u, 8192u, 16384u};
        for (const uint32_t resolution : kRsmAtlasResolutions)
        {
          const bool isSelected = editableRebuildSettings.vsmRsmAtlasResolution == resolution;
          if (ImGui::Selectable(std::to_string(resolution).c_str(), isSelected))
          {
            editableRebuildSettings.vsmRsmAtlasResolution = resolution;
          }
          if (isSelected)
          {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      constexpr std::array<RendererDisplayOutput, 9> kRsmGiDebugOutputs = {
        RendererDisplayOutput::ShadedColor,
        RendererDisplayOutput::RsmIndirectDiffuse,
        RendererDisplayOutput::RsmIndirectDiffuseDenoised,
        RendererDisplayOutput::RsmFlux,
        RendererDisplayOutput::RsmNormal,
        RendererDisplayOutput::RsmWorldPosition,
        RendererDisplayOutput::RsmResolveMask,
        RendererDisplayOutput::RsmValidSampleRatio,
        RendererDisplayOutput::VsmAtlas,
      };
      if (ImGui::BeginCombo("Diffuse GI debug output", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput)))
      {
        for (const RendererDisplayOutput output : kRsmGiDebugOutputs)
        {
          const bool isSelected = editableRebuildSettings.displayOutput == output;
          if (ImGui::Selectable(rendererDisplayOutputLabel(output), isSelected))
          {
            editableRebuildSettings.displayOutput = output;
            result.rebuildRequested = true;
          }
          if (isSelected)
          {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
    }
    else if (editableRebuildSettings.diffuseGIOption == DiffuseGIOption::ImperfectShadowMaps)
    {
      ImGui::TextDisabled("Uses the VPL imperfect-shadow-map controls below.");
    }
  }

  if (ImGui::CollapsingHeader("Imperfect Shadow Maps", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::BeginCombo("Indirect GI option", indirectGIOptionLabel(editableRebuildSettings.indirectGIOption)))
    {
      constexpr std::array<IndirectGIOption, 1> indirectGIOptions = {
        IndirectGIOption::ImperfectShadowMaps,
      };
      for (const IndirectGIOption option : indirectGIOptions)
      {
        const bool isSelected = editableRebuildSettings.indirectGIOption == option;
        if (ImGui::Selectable(indirectGIOptionLabel(option), isSelected))
        {
          editableRebuildSettings.indirectGIOption = option;
          result.runtimeSettingsChanged = true;
        }
        if (isSelected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    ImGui::TextDisabled("GI-only VPLs use low-resolution point-light VSM pages with imperfect point splats.");
    ImGui::Checkbox("Allocate VPL ISMs", &editableRebuildSettings.ismGiEnableVpls);
    ImGui::SliderScalar("Active VPLs", ImGuiDataType_U32, &editableRebuildSettings.ismGiActiveVpls, &kOne, &kSixtyFour);
    ImGui::SliderScalar("Pages / VPL face", ImGuiDataType_U32, &editableRebuildSettings.ismGiPagesPerVplFace, &kOne, &kSixtyFour);
    ImGui::SliderScalar("Atlas resolution", ImGuiDataType_U32, &editableRebuildSettings.ismGiPhysicalAtlasResolution, &kFiveTwelve, &kFourKi);
    ImGui::SliderScalar("Physical page size", ImGuiDataType_U32, &editableRebuildSettings.ismGiPhysicalPageSize, &kThirtyTwo, &kOneTwentyEight);

    uint32_t ismFaceSide = 1u;
    const uint32_t requestedIsmFaceSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max(editableRebuildSettings.ismGiPagesPerVplFace, 1u)))));
    while (ismFaceSide < requestedIsmFaceSide && ismFaceSide < VirtualShadowMapManager::DEFAULT_PAGE_TABLE_RESOLUTION)
    {
      ismFaceSide <<= 1u;
    }
    const uint32_t ismActualPagesPerFace = ismFaceSide * ismFaceSide;
    const uint32_t ismAllocatedVpls = editableRebuildSettings.ismGiEnableVpls ? editableRebuildSettings.ismGiActiveVpls : 0u;
    const uint32_t ismAtlasPagesPerAxis = editableRebuildSettings.ismGiPhysicalAtlasResolution / std::max(editableRebuildSettings.ismGiPhysicalPageSize, 1u);
    ImGui::TextDisabled(
        "Allocated VPLs %u  page table %ux%u  pages / VPL %u  physical capacity %u",
        ismAllocatedVpls,
        ismFaceSide,
        ismFaceSide,
        ismActualPagesPerFace * VirtualShadowMapManager::POINT_LIGHT_FACE_COUNT,
        ismAtlasPagesPerAxis * ismAtlasPagesPerAxis);
    ImGui::Text(
        "Runtime VPLs: proxy %u  allocated %u  manager lights %u  layers %u",
        ismVplRuntimeProxyCount,
        ismVplRuntimeAllocatedCount,
        ismVplRuntimeManagerLightCount,
        ismVplRuntimeLayerCount);
    ImGui::TextDisabled(
        "Runtime table %ux%u  physical pages %ux%u  page size %u",
        ismVplRuntimePageTableResolution,
        ismVplRuntimePageTableResolution,
        ismVplRuntimePhysicalPageTableResolution,
        ismVplRuntimePhysicalPageTableResolution,
        ismVplRuntimePhysicalPageSize);
    ImGui::Text(
        "VPL spawn: %s  RSM rays %u  visible hits %u  triangles %u",
        ismVplSpawnModeLabel(ismVplBuildStats.spawnMode),
        ismVplBuildStats.rsmRayCount,
        ismVplBuildStats.rsmHitCount,
        ismVplBuildStats.surfaceTriangleCount);

    constexpr std::array<RendererDisplayOutput, 5> kIsmDebugOutputs = {
      RendererDisplayOutput::ShadedColor,
      RendererDisplayOutput::IsmVplPages,
      RendererDisplayOutput::IsmVplPoints,
      RendererDisplayOutput::IsmVplTable,
      RendererDisplayOutput::IsmVplAtlas,
    };
    if (ImGui::BeginCombo("ISM debug output", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput)))
    {
      for (const RendererDisplayOutput output : kIsmDebugOutputs)
      {
        const bool isSelected = editableRebuildSettings.displayOutput == output;
        if (ImGui::Selectable(rendererDisplayOutputLabel(output), isSelected))
        {
          editableRebuildSettings.displayOutput = output;
          result.runtimeSettingsChanged = true;
        }
        if (isSelected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    result.runtimeSettingsChanged |= ImGui::SliderInt("VPL index", &ismVplDebugSelectedVpl, -1, 63);
    const int maxProjectedVpl = std::max(0, static_cast<int>(std::min<uint32_t>(std::max(ismVplRuntimeAllocatedCount, ismVplRuntimeProxyCount), 64u)) - 1);
    if (ismVplProjectedLightIndex > maxProjectedVpl)
    {
      ismVplProjectedLightIndex = maxProjectedVpl;
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderInt("Project VPL pages for", &ismVplProjectedLightIndex, 0, maxProjectedVpl);
    result.runtimeSettingsChanged |= ImGui::SliderInt("ISM layer", &ismVplDebugLayer, -1, 2047);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Point radius", &ismVplDebugPointRadius, 1.0f, 24.0f, "%.1f px");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Point exposure", &ismVplDebugExposure, 0.0f, 64.0f, "%.1f");
    result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay points on shaded", &ismVplPointsOverlayEnabled);
    if (ismVplPointsOverlayEnabled)
    {
      result.runtimeSettingsChanged |= ImGui::SliderFloat("Overlay opacity", &ismVplPointsOverlayOpacity, 0.0f, 1.0f, "%.2f");
    }
    ImGui::TextDisabled("Layer -1 shows all VPL faces tiled; VPL index -1 draws every seed point.");
  }

  if (ImGui::CollapsingHeader("Global Illumination", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const bool worldGIEnabledChanged = ImGui::Checkbox("Enable World GI pass", &editableRebuildSettings.enableWorldGI);
    result.runtimeSettingsChanged |= worldGIEnabledChanged;
    if (worldGIEnabledChanged && !editableRebuildSettings.enableWorldGI && rendererDisplayOutputUsesWorldGI(editableRebuildSettings.displayOutput))
    {
      editableRebuildSettings.displayOutput = RendererDisplayOutput::ShadedColor;
    }
    ImGui::TextDisabled("Phase 1/2 path: separates diffuse, glossy, and final indirect outputs. Disabling this pass clears all three outputs.");
    result.runtimeSettingsChanged |= ImGui::Checkbox("World GI diffuse", &editableRebuildSettings.worldGIDiffuseEnabled);
    result.runtimeSettingsChanged |= ImGui::Checkbox("World GI glossy", &editableRebuildSettings.worldGIGlossyEnabled);
    result.runtimeSettingsChanged |= ImGui::Checkbox("World GI indirect", &editableRebuildSettings.worldGIIndirectEnabled);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI diffuse weight", &editableRebuildSettings.worldGIDiffuseWeight, 0.0f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI glossy weight", &editableRebuildSettings.worldGIGlossyWeight, 0.0f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI intensity", &editableRebuildSettings.worldGIIndirectIntensity, 0.0f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI glossy max roughness", &editableRebuildSettings.worldGIGlossyMaxRoughness, 0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("World GI raygen");
    int worldGIRayResolutionDivisorIndex = editableRebuildSettings.worldGIRayResolutionDivisor == 8u ? 3 : (editableRebuildSettings.worldGIRayResolutionDivisor == 4u ? 2 : (editableRebuildSettings.worldGIRayResolutionDivisor == 2u ? 1 : 0));
    if (ImGui::Combo("World GI ray resolution", &worldGIRayResolutionDivisorIndex, "1x\0" "1/2\0" "1/4\0" "1/8\0"))
    {
      editableRebuildSettings.worldGIRayResolutionDivisor = worldGIRayResolutionDivisorIndex == 3 ? 8u : (worldGIRayResolutionDivisorIndex == 2 ? 4u : (worldGIRayResolutionDivisorIndex == 1 ? 2u : 1u));
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    int worldGIRaysPerPixel = static_cast<int>(editableRebuildSettings.worldGIRaysPerPixel);
    if (ImGui::SliderInt("World GI rays/pixel", &worldGIRaysPerPixel, 1, 8))
    {
      editableRebuildSettings.worldGIRaysPerPixel = static_cast<uint32_t>(std::clamp(worldGIRaysPerPixel, 1, 8));
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    int worldGIRayMaxSteps = static_cast<int>(editableRebuildSettings.worldGIRayMaxSteps);
    if (ImGui::SliderInt("World GI SDF steps", &worldGIRayMaxSteps, 8, 256))
    {
      editableRebuildSettings.worldGIRayMaxSteps = static_cast<uint32_t>(std::clamp(worldGIRayMaxSteps, 8, 256));
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI ray min distance", &editableRebuildSettings.worldGIRayMinDistance, 0.0f, 0.25f, "%.3f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI ray max distance", &editableRebuildSettings.worldGIRayMaxDistance, 1.0f, 256.0f, "%.1f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI ray normal bias", &editableRebuildSettings.worldGIRayNormalBias, 0.0f, 0.25f, "%.3f");
    ImGui::SeparatorText("World GI light grid");
    result.runtimeSettingsChanged |= ImGui::Checkbox("World GI light grid", &editableRebuildSettings.worldGILightGridEnabled);
    int worldGILightGridResolution = static_cast<int>(editableRebuildSettings.worldGILightGridResolution);
    if (ImGui::SliderInt("World GI grid resolution", &worldGILightGridResolution, 4, 32))
    {
      editableRebuildSettings.worldGILightGridResolution = static_cast<uint32_t>(std::clamp(worldGILightGridResolution, 4, 32));
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    int worldGILightGridCascadeCount = static_cast<int>(editableRebuildSettings.worldGILightGridCascadeCount);
    if (ImGui::SliderInt("World GI grid cascades", &worldGILightGridCascadeCount, 1, static_cast<int>(globalillumination::worldgi::kWorldGILightGridMaxCascades)))
    {
      editableRebuildSettings.worldGILightGridCascadeCount = static_cast<uint32_t>(std::clamp(worldGILightGridCascadeCount, 1, static_cast<int>(globalillumination::worldgi::kWorldGILightGridMaxCascades)));
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    int worldGILightGridMaxItemsPerCell = static_cast<int>(editableRebuildSettings.worldGILightGridMaxItemsPerCell);
    if (ImGui::SliderInt("World GI grid max ids/cell", &worldGILightGridMaxItemsPerCell, 1, 128))
    {
      editableRebuildSettings.worldGILightGridMaxItemsPerCell = static_cast<uint32_t>(std::clamp(worldGILightGridMaxItemsPerCell, 1, 128));
      result.rebuildRequested = isRendererRebuildSettingsValid(editableRebuildSettings);
    }
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI grid base extent", &editableRebuildSettings.worldGILightGridBaseExtent, 4.0f, 2000.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI grid extent scale", &editableRebuildSettings.worldGILightGridCascadeScale, 1.0f, 4.0f, "%.2fx");
    result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay World GI grid", &worldGILightGridOverlayEnabled);
    if (worldGILightGridOverlayEnabled)
    {
      result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI grid overlay opacity", &worldGILightGridOverlayOpacity, 0.0f, 1.0f, "%.2f");
      result.runtimeSettingsChanged |= ImGui::SliderInt("World GI grid overlay cascade", &worldGILightGridOverlayCascade, -1, static_cast<int>(globalillumination::worldgi::kWorldGILightGridMaxCascades) - 1);
      result.runtimeSettingsChanged |= ImGui::SliderFloat("World GI grid line width", &worldGILightGridOverlayLineWidth, 0.005f, 0.10f, "%.3f");
    }
    constexpr std::array<RendererDisplayOutput, 4> kWorldGIOutputs = {
      RendererDisplayOutput::ShadedColor,
      RendererDisplayOutput::WorldGIDiffuseLighting,
      RendererDisplayOutput::WorldGIGlossyRadiance,
      RendererDisplayOutput::WorldGIIndirectLighting,
    };
    if (ImGui::BeginCombo("World GI debug output", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput)))
    {
      for (const RendererDisplayOutput output : kWorldGIOutputs)
      {
        const bool isSelected = editableRebuildSettings.displayOutput == output;
        if (ImGui::Selectable(rendererDisplayOutputLabel(output), isSelected))
        {
          editableRebuildSettings.displayOutput = output;
          result.runtimeSettingsChanged = true;
        }
        if (isSelected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  if (ImGui::CollapsingHeader("Radiance Cache Grids", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SeparatorText("Surfel Hierarchy");
    const uint32_t kLookupResolutionMin = 2u;
    const uint32_t kLookupResolutionMax = 64u;
    result.runtimeSettingsChanged |= ImGui::Checkbox("Freeze grid origin", &surfelHierarchyFreeze);

    result.runtimeSettingsChanged |= ImGui::SliderFloat("Central half extent", &surfelHierarchySettings.centralGridHalfExtent, 0.01f, 256.0f, "%.3f m", ImGuiSliderFlags_Logarithmic);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Lookup resolution", ImGuiDataType_U32, &surfelHierarchySettings.lookupCellResolution, &kLookupResolutionMin, &kLookupResolutionMax);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Frustum far distance", &surfelHierarchySettings.frustumFarDistance, 16.0f, 65536.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    surfelHierarchySettings.lookupCellResolution = std::max(1u, surfelHierarchySettings.lookupCellResolution);
    surfelHierarchySettings.centralGridHalfExtent = std::max(surfelHierarchySettings.centralGridHalfExtent, 0.01f);
    surfelHierarchySettings.gridScale = globalillumination::surfels::surfelHierarchyGridScaleFromHalfExtent(surfelHierarchySettings);
    surfelHierarchySettings.gridCapacity = globalillumination::surfels::surfelHierarchyLookupCellCount(surfelHierarchySettings);
    surfelRuntimeSettings.gridScale = surfelHierarchySettings.gridScale;
    surfelRuntimeSettings.lookupCellResolution = surfelHierarchySettings.lookupCellResolution;
    surfelRuntimeSettings.centralGridHalfExtent = surfelHierarchySettings.centralGridHalfExtent;
    surfelRuntimeSettings.frustumFarDistance = surfelHierarchySettings.frustumFarDistance;
    surfelRuntimeSettings.gridCapacity = surfelHierarchySettings.gridCapacity;
    const float baseCellSize = globalillumination::surfels::surfelHierarchyBaseCellSize(surfelHierarchySettings);
    const float centralHalfExtent = globalillumination::surfels::surfelHierarchyCentralHalfExtent(surfelHierarchySettings);
    ImGui::TextDisabled("Lookup = 1 central linear grid + 6 frustum grids, each with %u^3 cells.", std::max(1u, surfelHierarchySettings.lookupCellResolution));
    ImGui::TextDisabled("Central cell size %.4f m, central half extent %.4f m.", baseCellSize, centralHalfExtent);
    ImGui::TextDisabled("Outer grids are world-aligned face frusta grown from the camera-centered box with nonlinear depth slices out to %.2f m.", surfelHierarchySettings.frustumFarDistance);
    ImGui::TextDisabled("Direct-addressed cell count = %u.", std::max(1u, surfelHierarchySettings.gridCapacity));
    ImGui::TextDisabled("Changing lookup resolution or direct-address capacity rebuilds the surfel buffers.");
    if (rendererDisplayOutputUsesSurfelHierarchy(editableRebuildSettings.displayOutput))
    {
      ImGui::TextDisabled("Selected surfel view: %s", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput));
    }
  }

  if (ImGui::CollapsingHeader("Surfels", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::Checkbox("Enable surfel passes", &editableRebuildSettings.enableSurfels) && !editableRebuildSettings.enableSurfels && rendererDisplayOutputUsesSurfels(editableRebuildSettings.displayOutput))
    {
      editableRebuildSettings.displayOutput = RendererDisplayOutput::ShadedColor;
    }
    ImGui::TextDisabled("Rebuild toggle. Disables surfel spawn, hierarchy, trace, lighting update, stats, and surfel debug draws.");
    if (ImGui::BeginCombo("Surfel debug view", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput)))
    {
      constexpr std::array<RendererDisplayOutput, 4> kSurfelDirectDrawOutputs = {
        RendererDisplayOutput::SurfelDirectDraw,
        RendererDisplayOutput::SurfelAlbedo,
        RendererDisplayOutput::SurfelNormal,
        RendererDisplayOutput::SurfelIrradiance,
      };
      for (const RendererDisplayOutput output : kSurfelDirectDrawOutputs)
      {
        const bool isSelected = editableRebuildSettings.displayOutput == output;
        if (ImGui::Selectable(rendererDisplayOutputLabel(output), isSelected))
        {
          editableRebuildSettings.displayOutput = output;
        }
        if (isSelected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    int surfelOwnerSelectionMode = static_cast<int>(surfelRuntimeSettings.ownerSelectionMode);
    if (ImGui::Combo("Owner selection", &surfelOwnerSelectionMode, "Oldest\0Newest\0"))
    {
      surfelRuntimeSettings.ownerSelectionMode = static_cast<uint32_t>(surfelOwnerSelectionMode);
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Surfel capacity", ImGuiDataType_U32, &surfelRuntimeSettings.surfelCapacity, &kOneKi, &kFourMi);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Spawn budget / frame", ImGuiDataType_U32, &surfelRuntimeSettings.allocationBudgetPerFrame, &kZero, &kSixtyFiveKi);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Spawn downsample", ImGuiDataType_U32, &surfelRuntimeSettings.spawnDownsample, &kOne, &kEight);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Target density / tile", ImGuiDataType_U32, &surfelRuntimeSettings.targetSurfelsPerTile, &kOne, &kEight);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Max density / tile", ImGuiDataType_U32, &surfelRuntimeSettings.maxSurfelsPerTile, &kOne, &kEight);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Target screen radius", &surfelRuntimeSettings.targetScreenRadiusPixels, 0.5f, 32.0f, "%.2f px");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Lookup radius scale", &surfelRuntimeSettings.lookupRadiusScale, 0.5f, 2.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Spawn min radius", &surfelRuntimeSettings.spawnMinRadius, 0.001f, 4.0f, "%.3f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Depth radius scale", &surfelRuntimeSettings.spawnDepthRadiusScale, 0.001f, 0.100f, "%.4f");
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Visible recycle age", ImGuiDataType_U32, &surfelRuntimeSettings.visibleRecycleMaxAgeFrames, &kOne, &kEightKi);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Hidden recycle age", ImGuiDataType_U32, &surfelRuntimeSettings.hiddenRecycleMaxAgeFrames, &kOne, &kEightKi);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Visible age step", ImGuiDataType_U32, &surfelRuntimeSettings.visibleUntouchedAgeStep, &kOne, &kSixtyFour);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Hidden age step", ImGuiDataType_U32, &surfelRuntimeSettings.hiddenUntouchedAgeStep, &kOne, &kSixtyFour);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Dense age step", ImGuiDataType_U32, &surfelRuntimeSettings.denseSurfelsAgeStep, &kOne, &kSixtyFour);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Hidden retreat scale", &surfelRuntimeSettings.hiddenDistanceThresholdScale, 0.0f, 8.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Hidden density scale", &surfelRuntimeSettings.hiddenDensityThresholdScale, 0.25f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Hidden density age", &surfelRuntimeSettings.hiddenDensityAgeScale, 0.0f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderScalar("List entries / surfel", ImGuiDataType_U32, &surfelRuntimeSettings.listEntriesPerSurfel, &kOne, &kSixtyFour);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Recycle max age", ImGuiDataType_U32, &surfelRuntimeSettings.recycleMaxAgeFrames, &kOne, &kEightKi);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Recycle coverage", &surfelRuntimeSettings.recycleCoverageThreshold, 0.0f, 2.0f, "%.3f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Free-list pressure", &surfelRuntimeSettings.recycleFreeListPressureBias, 0.0f, 4.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Radius update rate", &surfelRuntimeSettings.radiusUpdateRate, 0.0f, 1.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Undersized age bias", &surfelRuntimeSettings.undersizedAgeBias, 0.1f, 1.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Oversized age bias", &surfelRuntimeSettings.oversizedAgeBias, 1.0f, 3.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Local follow max dist", &surfelRuntimeSettings.localSpaceFollowMaxDistance, 0.0f, 2.0f, "%.3f m");
    ImGui::SeparatorText("Lighting Trace");
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Lighting update period", ImGuiDataType_U32, &surfelRuntimeSettings.lightingUpdatePeriodFrames, &kOne, &kOneTwentyEight);
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Trace max steps", ImGuiDataType_U32, &surfelRuntimeSettings.lightingTraceMaxSteps, &kOne, &kOneTwentyEight);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Trace max distance", &surfelRuntimeSettings.lightingMaxTraceDistance, 1.0f, 100000.0f, "%.1f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Trace hit threshold", &surfelRuntimeSettings.lightingTraceHitThresholdScale, 0.10f, 2.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Trace empty-space scale", &surfelRuntimeSettings.lightingTraceEmptySpaceStepScale, 0.25f, 2.0f, "%.2f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Trace normal bias", &surfelRuntimeSettings.lightingTraceNormalBias, 0.0f, 0.25f, "%.4f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Trace view bias", &surfelRuntimeSettings.lightingTraceViewBias, 0.0f, 0.25f, "%.4f m");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Lighting hysteresis", &surfelRuntimeSettings.lightingHysteresis, 0.0f, 0.999f, "%.3f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Irradiance decay", &surfelRuntimeSettings.lightingIrradianceDecay, 0.80f, 0.9999f, "%.4f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Energy preservation", &surfelRuntimeSettings.lightingEnergyPreservation, 0.0f, 1.0f, "%.3f");
    result.runtimeSettingsChanged |= ImGui::SliderFloat("Default diffuse albedo", &surfelRuntimeSettings.lightingDefaultDiffuseAlbedo, 0.0f, 1.0f, "%.3f");
    int surfelTraceCascadeOverride = surfelRuntimeSettings.lightingTraceCascadeOverride;
    if (ImGui::SliderInt("Trace cascade override", &surfelTraceCascadeOverride, -1, 7))
    {
      surfelRuntimeSettings.lightingTraceCascadeOverride = surfelTraceCascadeOverride;
      result.runtimeSettingsChanged = true;
    }
    surfelRuntimeSettings.targetScreenRadiusPixels = std::max(surfelRuntimeSettings.targetScreenRadiusPixels, 0.1f);
    surfelRuntimeSettings.spawnDepthRadiusScale = std::max(surfelRuntimeSettings.spawnDepthRadiusScale, 0.0001f);
    surfelRuntimeSettings.targetSurfelsPerTile = std::max(surfelRuntimeSettings.targetSurfelsPerTile, 1u);
    surfelRuntimeSettings.maxSurfelsPerTile = std::max(surfelRuntimeSettings.maxSurfelsPerTile, surfelRuntimeSettings.targetSurfelsPerTile);
    surfelRuntimeSettings.visibleRecycleMaxAgeFrames = std::max(surfelRuntimeSettings.visibleRecycleMaxAgeFrames, 1u);
    surfelRuntimeSettings.hiddenRecycleMaxAgeFrames = std::max(surfelRuntimeSettings.hiddenRecycleMaxAgeFrames, 1u);
    surfelRuntimeSettings.hiddenDistanceThresholdScale = std::max(surfelRuntimeSettings.hiddenDistanceThresholdScale, 0.0f);
    surfelRuntimeSettings.hiddenDensityThresholdScale = std::max(surfelRuntimeSettings.hiddenDensityThresholdScale, 0.1f);
    surfelRuntimeSettings.hiddenDensityAgeScale = std::max(surfelRuntimeSettings.hiddenDensityAgeScale, 0.0f);
    surfelRuntimeSettings.lightingUpdatePeriodFrames = std::max(surfelRuntimeSettings.lightingUpdatePeriodFrames, 1u);
    surfelRuntimeSettings.lightingTraceMaxSteps = std::max(surfelRuntimeSettings.lightingTraceMaxSteps, 1u);
    surfelRuntimeSettings.lightingMaxTraceDistance = std::max(surfelRuntimeSettings.lightingMaxTraceDistance, 0.01f);
    surfelRuntimeSettings.lightingTraceHitThresholdScale = std::max(surfelRuntimeSettings.lightingTraceHitThresholdScale, 0.01f);
    surfelRuntimeSettings.lightingTraceEmptySpaceStepScale = std::max(surfelRuntimeSettings.lightingTraceEmptySpaceStepScale, 0.01f);
    surfelRuntimeSettings.lightingTraceNormalBias = std::max(surfelRuntimeSettings.lightingTraceNormalBias, 0.0f);
    surfelRuntimeSettings.lightingTraceViewBias = std::max(surfelRuntimeSettings.lightingTraceViewBias, 0.0f);
    surfelRuntimeSettings.lightingHysteresis = std::clamp(surfelRuntimeSettings.lightingHysteresis, 0.0f, 0.999f);
    surfelRuntimeSettings.lightingIrradianceDecay = std::clamp(surfelRuntimeSettings.lightingIrradianceDecay, 0.0f, 0.99999f);
    surfelRuntimeSettings.lightingEnergyPreservation = std::clamp(surfelRuntimeSettings.lightingEnergyPreservation, 0.0f, 1.0f);
    surfelRuntimeSettings.lightingDefaultDiffuseAlbedo = std::clamp(surfelRuntimeSettings.lightingDefaultDiffuseAlbedo, 0.0f, 1.0f);
    surfelRuntimeSettings.lightingTraceCascadeOverride = std::clamp(surfelRuntimeSettings.lightingTraceCascadeOverride, -1, 7);

    const uint64_t surfelStackBytes = globalillumination::surfels::surfelStackBufferSize(surfelRuntimeSettings);
    const uint64_t surfelPosBytes = globalillumination::surfels::surfelPositionRadiusBufferSize(surfelRuntimeSettings);
    const uint64_t surfelNorBytes = globalillumination::surfels::surfelNormalLifecycleBufferSize(surfelRuntimeSettings);
    const uint64_t surfelAttachmentBytes = globalillumination::surfels::surfelAttachmentBufferSize(surfelRuntimeSettings);
    const uint64_t surfelLifecycleBytes = globalillumination::surfels::surfelLifecycleBufferSize(surfelRuntimeSettings);
    const uint64_t surfelGridBytes = globalillumination::surfels::surfelGridBufferSize(surfelRuntimeSettings);
    const uint64_t surfelGridCursorBytes = globalillumination::surfels::surfelGridWriteCursorBufferSize(surfelRuntimeSettings);
    const uint64_t surfelListBytes = globalillumination::surfels::surfelListBufferSize(surfelRuntimeSettings);
    const uint64_t surfelPrefixSegmentsBytes = globalillumination::surfels::surfelPrefixSegmentsBufferSize(surfelRuntimeSettings);
    const uint64_t surfelTotalBytes =
        surfelStackBytes + surfelPosBytes + surfelNorBytes + surfelAttachmentBytes + surfelLifecycleBytes + 2u * (surfelGridBytes + surfelGridCursorBytes + surfelListBytes + surfelPrefixSegmentsBytes);
    ImGui::TextDisabled("src-dgi frame model: count/prefix/insert rebuild current grid first, then spawn queries that current lookup.");
    ImGui::TextDisabled("Density is mainly driven by target/max density per tile. Size is driven by target screen radius, spawn min radius, and depth radius scale.");
    ImGui::TextDisabled("Allocated surfels: %u / %u", surfelAllocatedCount, std::max(1u, surfelRuntimeSettings.surfelCapacity));
    ImGui::TextDisabled("Spawn budget / frame: %u", surfelRuntimeSettings.allocationBudgetPerFrame);
    ImGui::Text(
        "Irradiance stats: nonzero %u / %u  green-dominant %u  maxW %.2f",
        surfelIrradianceDebugStats.nonZeroSurfels,
        surfelIrradianceDebugStats.sampledSurfels,
        surfelIrradianceDebugStats.greenDominantSurfels,
        surfelIrradianceDebugStats.maxWeight);
    ImGui::TextDisabled(
        "Irradiance avg RGB %.3f %.3f %.3f  max RGB %.3f %.3f %.3f",
        surfelIrradianceDebugStats.averageRgb[0],
        surfelIrradianceDebugStats.averageRgb[1],
        surfelIrradianceDebugStats.averageRgb[2],
        surfelIrradianceDebugStats.maxRgb[0],
        surfelIrradianceDebugStats.maxRgb[1],
        surfelIrradianceDebugStats.maxRgb[2]);
    ImGui::Text("Trace stats: rays %u  hits %u  miss %u  invalid %u", surfelTraceStats.totalRays, surfelTraceStats.hitRays, surfelTraceStats.missRays, surfelTraceStats.invalidDirectionRays);
    ImGui::TextDisabled(
        "Trace stages: cascade %u  root %u  tree %u  mid %u  bottom %u  brick %u  uninit %u",
        surfelTraceStats.hasCascadeRays,
        surfelTraceStats.hasRootAabbRays,
        surfelTraceStats.hasTreeRootRays,
        surfelTraceStats.hasMidAabbRays,
        surfelTraceStats.hasBottomAabbRays,
        surfelTraceStats.hasValidBrickRays,
        surfelTraceStats.hasUninitializedRays);
    ImGui::TextDisabled(
        "Trace failures: cascade %u  root %u  mid %u  bottom %u  brick %u  uninit %u",
        surfelTraceStats.failedCascadeRays,
        surfelTraceStats.failedRootRays,
        surfelTraceStats.failedMidRays,
        surfelTraceStats.failedBottomRays,
        surfelTraceStats.failedBrickRays,
        surfelTraceStats.failedUninitializedRays);
    ImGui::TextDisabled(
        "Buffers: stack %.2f MB  pos %.2f MB  nor %.2f MB  attach %.2f MB  life %.2f MB",
        double(surfelStackBytes) / (1024.0 * 1024.0),
        double(surfelPosBytes) / (1024.0 * 1024.0),
        double(surfelNorBytes) / (1024.0 * 1024.0),
        double(surfelAttachmentBytes) / (1024.0 * 1024.0),
        double(surfelLifecycleBytes) / (1024.0 * 1024.0));
    ImGui::TextDisabled(
        "Accel ping-pong: grid %.2f MB  cursor %.2f MB  list %.2f MB  seg %.2f MB  (%u buckets, %u entries)",
        double(2u * surfelGridBytes) / (1024.0 * 1024.0),
        double(2u * surfelGridCursorBytes) / (1024.0 * 1024.0),
        double(2u * surfelListBytes) / (1024.0 * 1024.0),
        double(2u * surfelPrefixSegmentsBytes) / (1024.0 * 1024.0),
        std::max(1u, surfelRuntimeSettings.gridCapacity),
        globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings));
    ImGui::TextDisabled("Total persistent surfel memory: %.2f MB", double(surfelTotalBytes) / (1024.0 * 1024.0));
    ImGui::TextDisabled("Spawn policy: screen tiles touch survivors, sparse tiles allocate, dense tiles add recycle pressure.");
    ImGui::TextDisabled("Surfel radii now chase a target screen size and the lookup can hold multiple surfels per cell.");
    ImGui::TextDisabled("Owner-cell collapse is disabled; count/insert rebuild the lookup from every live surfel.");
    ImGui::TextDisabled("Changing surfel capacity or list entries / surfel rebuilds the persistent surfel buffers.");
  }

  const bool surfelPersistentLayoutChanged =
      globalillumination::surfels::surfelStackBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelStackBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelPositionRadiusBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelPositionRadiusBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelNormalLifecycleBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelNormalLifecycleBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelAttachmentBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelAttachmentBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelLifecycleBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelLifecycleBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelRecycleClaimBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelRecycleClaimBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelOwnerCellBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelOwnerCellBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelGridBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelGridBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelGridWriteCursorBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelGridWriteCursorBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelListBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelListBufferSize(surfelRuntimeSettings) ||
      globalillumination::surfels::surfelPrefixSegmentsBufferSize(initialSurfelRuntimeSettings) != globalillumination::surfels::surfelPrefixSegmentsBufferSize(surfelRuntimeSettings) ||
      initialSurfelHierarchySettings.gridCapacity != surfelHierarchySettings.gridCapacity;
  result.rebuildRequested |= surfelPersistentLayoutChanged;

  if (ImGui::CollapsingHeader("TSR", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Checkbox("Enable TSR", &editableRebuildSettings.enableTsr);
    if (!editableRebuildSettings.enableTaa)
    {
      ImGui::TextDisabled("TSR requires TAA. With TAA off, the renderer falls back to native resolution.");
    }
    ImGui::TextDisabled("Use 1.00 for native TAA; lower values enable temporal upscaling.");
    const bool tsrScaleEditable = editableRebuildSettings.enableTaa && editableRebuildSettings.enableTsr;
    if (!tsrScaleEditable)
      ImGui::BeginDisabled();
    ImGui::SliderFloat("TSR render scale", &editableRebuildSettings.temporalRenderScale, 0.50f, 1.00f, "%.2fx");
    if (!tsrScaleEditable)
      ImGui::EndDisabled();
    const bool taaJitterEditable = editableRebuildSettings.enableTaa;
    if (!taaJitterEditable)
      ImGui::BeginDisabled();
    ImGui::SliderScalar("TSR jitter samples", ImGuiDataType_U32, &editableRebuildSettings.temporalJitterSamples, &kOne, &kOneTwentyEight);
    ImGui::TextDisabled("Bigger jitter is not always better. Moderate values usually upscale more cleanly.");
    ImGui::SliderFloat("TSR jitter scale", &editableRebuildSettings.temporalJitterScale, 0.0f, 2.0f, "%.3f px");
    if (!taaJitterEditable)
      ImGui::EndDisabled();
  }

  if (ImGui::CollapsingHeader("TAA Resolve", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Checkbox("Enable TAA", &editableRebuildSettings.enableTaa);
    if (!editableRebuildSettings.enableTaa)
      ImGui::BeginDisabled();
    ImGui::SliderScalar("TAA accumulation frames", ImGuiDataType_U32, &editableRebuildSettings.taaAccumulationFrames, &kOne, &kSixtyFour);
    ImGui::SliderFloat("TAA motion scale", &editableRebuildSettings.taaMotionScale, 0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("TAA depth threshold", &editableRebuildSettings.taaDepthThreshold, 0.0001f, 0.0500f, "%.4f");
    ImGui::SliderFloat("TAA normal threshold", &editableRebuildSettings.taaNormalThreshold, 0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("TAA reactive scale", &editableRebuildSettings.taaReactiveScale, 0.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("TAA edge scale", &editableRebuildSettings.taaEdgeScale, 0.0f, 1000.0f, "%.1f");
    ImGui::SliderFloat("TAA sharpen", &editableRebuildSettings.taaSharpenStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("TAA detail boost", &editableRebuildSettings.taaDetailBoost, 0.0f, 1.0f, "%.2f");
    if (!editableRebuildSettings.enableTaa)
      ImGui::EndDisabled();
  }

  if (ImGui::CollapsingHeader("Prepass Culling", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SliderScalar("Prepass hierarchy levels", ImGuiDataType_U32, &editableRebuildSettings.prepassMaxHierarchyLevels, &kOne, &kSixteen);
    ImGui::SliderScalar("Prepass queue (MiB)", ImGuiDataType_U32, &editableRebuildSettings.prepassClusterQueueMiB, &kOne, &kSixtyFour);
    ImGui::Checkbox("Prepass frustum culling", &editableRebuildSettings.prepassEnableFrustumCulling);
    ImGui::Checkbox("Prepass occlusion culling", &editableRebuildSettings.prepassEnableOcclusionCulling);
  }

  if (ImGui::CollapsingHeader("Final Culling", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SliderScalar("Final hierarchy levels", ImGuiDataType_U32, &editableRebuildSettings.finalMaxHierarchyLevels, &kOne, &kSixteen);
    ImGui::SliderScalar("Final queue (MiB)", ImGuiDataType_U32, &editableRebuildSettings.finalClusterQueueMiB, &kOne, &kSixtyFour);
    ImGui::Checkbox("Final frustum culling", &editableRebuildSettings.finalEnableFrustumCulling);
    ImGui::Checkbox("Final occlusion culling", &editableRebuildSettings.finalEnableOcclusionCulling);
    ImGui::Checkbox("Streaming priority updates", &editableRebuildSettings.finalEnableStreamingPriorityUpdates);
  }

  if (ImGui::CollapsingHeader("Draw Passes", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SliderScalar("Depth prepass max clusters", ImGuiDataType_U32, &editableRebuildSettings.depthPrePassMaxDrawnClusters, &kThirtyTwo, &kFourKi);
    ImGui::SliderScalar("Hardware draw max clusters", ImGuiDataType_U32, &editableRebuildSettings.hardwareDrawMaxDrawnClusters, &kThirtyTwo, &kFourKi);
  }

  if (ImGui::CollapsingHeader("Material Pass", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SliderScalar("Feedback capacity", ImGuiDataType_U32, &editableRebuildSettings.materialFeedbackRequestCapacity, &kOneKi, &kSixtyFiveKi);
    ImGui::SliderScalar("Feedback stride", ImGuiDataType_U32, &editableRebuildSettings.materialFeedbackStride, &kOne, &kEight);
    ImGui::SliderScalar("Max uploads/frame", ImGuiDataType_U32, &editableRebuildSettings.materialMaxUploadsPerFrame, &kSixteen, &kTwoKi);
    ImGui::SliderScalar("Tile size", ImGuiDataType_U32, &editableRebuildSettings.materialTileSize, &kThirtyTwo, &kTwoFiftySix);
    ImGui::SliderScalar("Tile workgroup size", ImGuiDataType_U32, &editableRebuildSettings.materialTileWorkgroupSize, &kEight, &kThirtyTwo);
  }

  if (ImGui::CollapsingHeader("Virtual Shadow Map", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::SliderScalar("Atlas resolution", ImGuiDataType_U32, &editableRebuildSettings.vsmPhysicalAtlasResolution, &kFourKi, &kSixteenKi);
    ImGui::SliderScalar("Physical page size", ImGuiDataType_U32, &editableRebuildSettings.vsmPhysicalPageSize, &kSixtyFour, &kTwoFiftySix);
    ImGui::SliderScalar("Cascade count", ImGuiDataType_U32, &editableRebuildSettings.vsmCascadeCount, &kOne, &kEight);
    ImGui::Checkbox("Auto directional first extent", &editableRebuildSettings.vsmDirectionalFirstCascadeWorldExtentAuto);
    if (!editableRebuildSettings.vsmDirectionalFirstCascadeWorldExtentAuto)
    {
      ImGui::SliderFloat("Directional first extent", &editableRebuildSettings.vsmDirectionalFirstCascadeWorldExtent, 64.0f, 8192.0f, "%.1f");
    }
    ImGui::SliderFloat("Directional extent scale", &editableRebuildSettings.vsmDirectionalCascadeWorldExtentScale, 1.0f, 8.0f, "%.2fx");
    ImGui::SliderFloat("Page world scale", &editableRebuildSettings.vsmPageWorldScale, 0.25f, 8.0f, "%.2fx");
    ImGui::SliderScalar("Dirty pages/frame", ImGuiDataType_U32, &editableRebuildSettings.vsmMaxDirtyPagesPerFrame, &kSixtyFour, &kFourKi);
    ImGui::SliderScalar("Fallback pages/frame", ImGuiDataType_U32, &editableRebuildSettings.vsmMaxFallbackPagesPerFrame, &kSixteen, &kFiveTwelve);
    ImGui::SliderScalar("Hierarchy queue cap", ImGuiDataType_U32, &editableRebuildSettings.vsmHierarchyQueueCapacity, &kOneKi, &kFourMi);
    ImGui::SliderScalar("Cluster queue cap", ImGuiDataType_U32, &editableRebuildSettings.vsmClusterQueueCapacity, &kOneKi, &kFourMi);
    ImGui::SliderScalar("Visible cluster draws", ImGuiDataType_U32, &editableRebuildSettings.vsmMaxVisibleClusterDraws, &kEightKi, &kTwoSixtyTwoKi);
    using ShadowMapType = VirtualShadowMapManager::Settings::ShadowMapType;
    if (ImGui::BeginCombo("Shadow map type", shadowMapTypeLabel(editableRebuildSettings.vsmShadowMapType)))
    {
      static constexpr ShadowMapType kShadowMapTypes[] = {
        ShadowMapType::VirtualShadowMap,
        ShadowMapType::ReflexiveShadowMap,
      };
      for (ShadowMapType type : kShadowMapTypes)
      {
        const bool isSelected = editableRebuildSettings.vsmShadowMapType == type;
        if (ImGui::Selectable(shadowMapTypeLabel(type), isSelected))
        {
          editableRebuildSettings.vsmShadowMapType = type;
        }
        if (isSelected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    editableRebuildSettings.vsmRasterMode = VirtualShadowMapDrawPass::RasterMode::ClipSpaceAtlasDepth;
    ImGui::TextDisabled("Raster mode: Per-page depth atlas");
    ImGui::Checkbox("Enable VSM table debug pass", &editableRebuildSettings.vsmEnableTableDebugPass);
    ImGui::Checkbox("Enable VSM atlas debug pass", &editableRebuildSettings.vsmEnableAtlasDebugPass);
  }

  if (ImGui::CollapsingHeader("Screen-Space Pass", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr uint32_t kScales[] = {1u, 2u, 4u};
    int currentScaleIndex = 0;
    for (size_t i = 0u; i < std::size(kScales); ++i)
    {
      if (editableRebuildSettings.screenSpaceShadowScale == kScales[i])
      {
        currentScaleIndex = static_cast<int>(i);
        break;
      }
    }
    if (ImGui::SliderInt("SSS render scale", &currentScaleIndex, 0, static_cast<int>(std::size(kScales)) - 1, currentScaleIndex == 0 ? "1x" : (currentScaleIndex == 1 ? "2x" : "4x")))
    {
      editableRebuildSettings.screenSpaceShadowScale = kScales[static_cast<size_t>(currentScaleIndex)];
    }

    ImGui::SeparatorText("Screen Space Ray Trace");
    result.runtimeSettingsChanged |= ImGui::Checkbox("Enable SSRT", &screenSpaceRayTraceEnabled);
    if (!kRendererEnableSoftwareRayTracingPath)
    {
      screenSpaceRayTraceScreenTracesEnabled = true;
      screenSpaceRayTraceForceWorldFallback = false;
      screenSpaceRayTraceTraceAllForcedFallbackRays = false;
      screenSpaceRayTraceSoftwareFallbackEnabled = false;
      screenSpaceRayTraceSoftwareFallbackTraceStats = false;
    }
    if (ImGui::Checkbox("SSRT screen traces", &screenSpaceRayTraceScreenTracesEnabled))
    {
      result.runtimeSettingsChanged = true;
      result.rebuildRequested = true;
      if (screenSpaceRayTraceScreenTracesEnabled)
      {
        screenSpaceRayTraceForceWorldFallback = false;
        screenSpaceRayTraceTraceAllForcedFallbackRays = false;
      }
      else
      {
        screenSpaceRayTraceForceWorldFallback = true;
        screenSpaceRayTraceTraceAllForcedFallbackRays = true;
        ensureScreenSpaceRayTraceFallback();
      }
    }
    if (!screenSpaceRayTraceScreenTracesEnabled)
    {
      screenSpaceRayTraceForceWorldFallback = true;
      screenSpaceRayTraceTraceAllForcedFallbackRays = true;
    }
    if (ImGui::Checkbox("SSRT force world fallback", &screenSpaceRayTraceForceWorldFallback))
    {
      result.runtimeSettingsChanged = true;
      if (screenSpaceRayTraceForceWorldFallback && !screenSpaceRayTraceSdfFallbackEnabled && !screenSpaceRayTraceSoftwareFallbackEnabled)
      {
        ensureScreenSpaceRayTraceFallback();
        result.rebuildRequested = true;
      }
    }
    if (ImGui::Checkbox("SSRT trace all forced fallback rays", &screenSpaceRayTraceTraceAllForcedFallbackRays))
    {
      result.runtimeSettingsChanged = true;
      result.rebuildRequested = true;
      if (screenSpaceRayTraceTraceAllForcedFallbackRays)
      {
        screenSpaceRayTraceForceWorldFallback = true;
        ensureScreenSpaceRayTraceFallback();
      }
    }
    static constexpr uint32_t kRayTraceTileSizes[] = {1u, 2u, 4u, 8u, 16u};
    int rayTraceTileIndex = 0;
    for (size_t i = 0u; i < std::size(kRayTraceTileSizes); ++i)
    {
      if (screenSpaceRayTraceTileSize == kRayTraceTileSizes[i])
      {
        rayTraceTileIndex = static_cast<int>(i);
        break;
      }
    }
    if (ImGui::SliderInt("SSRT tile", &rayTraceTileIndex, 0, static_cast<int>(std::size(kRayTraceTileSizes)) - 1))
    {
      screenSpaceRayTraceTileSize = kRayTraceTileSizes[static_cast<size_t>(rayTraceTileIndex)];
      result.runtimeSettingsChanged = true;
      if (screenSpaceRayTraceTraceAllForcedFallbackRays)
        result.rebuildRequested = true;
    }
    ImGui::SameLine();
    ImGui::Text("%ux%u", screenSpaceRayTraceTileSize, screenSpaceRayTraceTileSize);
    if (ImGui::SliderScalar("SSRT rays / tile", ImGuiDataType_U32, &screenSpaceRayTraceRaysPerTile, &kOne, &kOneTwentyEight))
    {
      result.runtimeSettingsChanged = true;
      if (screenSpaceRayTraceTraceAllForcedFallbackRays)
        result.rebuildRequested = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderScalar("SSRT max steps", ImGuiDataType_U32, &screenSpaceRayTraceMaxSteps, &kOne, &kOneTwentyEight);
    const uint32_t forcedFallbackRayCount = computeScreenSpaceRayTraceTotalRayBudget(
        screenSpaceRayTraceViewportWidth,
        screenSpaceRayTraceViewportHeight,
        screenSpaceRayTraceTileSize,
        screenSpaceRayTraceRaysPerTile);
    if (screenSpaceRayTraceTraceAllForcedFallbackRays)
    {
      screenSpaceRayTraceMaxFailedRays = forcedFallbackRayCount;
      screenSpaceRayTraceSoftwareFallbackBudget = forcedFallbackRayCount;
      ImGui::TextDisabled("Forced fallback budget: %u rays", forcedFallbackRayCount);
    }
    if (screenSpaceRayTraceTraceAllForcedFallbackRays)
      ImGui::BeginDisabled();
    if (ImGui::SliderScalar("SSRT failed ray cap", ImGuiDataType_U32, &screenSpaceRayTraceMaxFailedRays, &kOneKi, &kFourMi))
    {
      result.rebuildRequested = true;
    }
    bool sdfFallbackAvailable = editableRebuildSettings.enableSignedDistanceFields;
    if (!sdfFallbackAvailable)
      screenSpaceRayTraceSdfFallbackEnabled = false;
    if (!sdfFallbackAvailable)
      ImGui::BeginDisabled();
    if (ImGui::Checkbox("SSRT SDF fallback", &screenSpaceRayTraceSdfFallbackEnabled))
    {
      result.runtimeSettingsChanged = true;
      result.rebuildRequested = true;
      if (screenSpaceRayTraceSdfFallbackEnabled)
        screenSpaceRayTraceSoftwareFallbackEnabled = false;
    }
    if (!sdfFallbackAvailable)
      ImGui::EndDisabled();
    if (!kRendererEnableSoftwareRayTracingPath)
      ImGui::BeginDisabled();
    if (ImGui::Checkbox("SSRT SWRT fallback", &screenSpaceRayTraceSoftwareFallbackEnabled))
    {
      result.runtimeSettingsChanged = true;
      result.rebuildRequested = true;
      if (screenSpaceRayTraceSoftwareFallbackEnabled)
        screenSpaceRayTraceSdfFallbackEnabled = false;
    }
    result.runtimeSettingsChanged |= ImGui::Checkbox("SSRT SWRT trace stats", &screenSpaceRayTraceSoftwareFallbackTraceStats);
    ImGui::TextDisabled("Trace stats use the debug SWRT shader so TLAS/BLAS visit counters are visible.");
    if (screenSpaceRayTraceForceWorldFallback && !screenSpaceRayTraceSdfFallbackEnabled && !screenSpaceRayTraceSoftwareFallbackEnabled)
    {
      ensureScreenSpaceRayTraceFallback();
      result.rebuildRequested = true;
    }
    if (ImGui::SliderScalar("SSRT SWRT budget", ImGuiDataType_U32, &screenSpaceRayTraceSoftwareFallbackBudget, &kOneKi, &kFourMi))
    {
      result.rebuildRequested = true;
    }
    if (!kRendererEnableSoftwareRayTracingPath)
      ImGui::EndDisabled();
    if (screenSpaceRayTraceTraceAllForcedFallbackRays)
      ImGui::EndDisabled();
    if (!kRendererEnableSoftwareRayTracingPath)
    {
      ImGui::TextDisabled("Software ray tracing fallback is disabled for the SDF-only path.");
    }
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT min distance", &screenSpaceRayTraceMinDistance, 0.0001f, 8.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT max distance", &screenSpaceRayTraceMaxDistance, 0.01f, 100000.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
    if (screenSpaceRayTraceMinDistance > screenSpaceRayTraceMaxDistance)
    {
      std::swap(screenSpaceRayTraceMinDistance, screenSpaceRayTraceMaxDistance);
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT thickness", &screenSpaceRayTraceSurfaceThickness, 0.00001f, 0.05f, "%.5f", ImGuiSliderFlags_Logarithmic);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT origin bias", &screenSpaceRayTraceOriginBias, 0.0f, 1.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("SSRT SWRT fallback uses the SSRT min/max distance.");
    if (screenSpaceRayTraceForceWorldFallback)
      ImGui::TextDisabled("SSRT traversal is bypassed; valid screen rays are emitted as SWRT fallback rays.");
    if (!screenSpaceRayTraceScreenTracesEnabled)
      ImGui::TextDisabled("Screen traversal is disabled; all generated rays go directly to the selected acceleration fallback.");
    if (screenSpaceRayTraceTraceAllForcedFallbackRays)
      ImGui::TextDisabled("All emitted SSRT rays are stored and traced by the SWRT fallback pass.");
    ImGui::Text(
        "SSRT off-screen rays %u / %u (%.2f%%)",
        screenSpaceRayTraceStats.offscreenRays,
        screenSpaceRayTraceStats.totalRays,
        screenSpaceRayTraceStats.offscreenPercent);
    ImGui::TextDisabled(
        "SSRT failed rays %u / %u%s",
        screenSpaceRayTraceStats.failedRays,
        screenSpaceRayTraceStats.capacity,
        screenSpaceRayTraceStats.overflow != 0u ? " overflow" : "");
    ImGui::TextDisabled(
        "SSRT world fallback rays %u  hit %u  miss %u  invalid %u",
        screenFallbackTraceStats.totalRays,
        screenFallbackTraceStats.hitRays,
        screenFallbackTraceStats.missRays,
        screenFallbackTraceStats.invalidDirectionRays);
  }

  if (ImGui::CollapsingHeader("Signed Distance Fields", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Checkbox("Enable GPUSignedDistanceFields", &editableRebuildSettings.enableSignedDistanceFields);
    ImGui::TextDisabled("Disables the whole SDF build and debug path from the render graph after rebuild.");
    static constexpr uint32_t kSurfaceCachePageResolutions[] = {0u, 1u, 2u, 4u, 8u};
    const auto surfaceCacheResolutionLabel = [](const uint32_t resolution) -> const char *
    {
      switch (resolution)
      {
      case 0u:
        return "Disabled";
      case 1u:
        return "1x1";
      case 2u:
        return "2x2";
      case 4u:
        return "4x4";
      case 8u:
        return "8x8";
      default:
        return "Invalid";
      }
    };
    int surfaceCachePageResolutionIndex = 0;
    for (size_t i = 0u; i < std::size(kSurfaceCachePageResolutions); ++i)
    {
      if (editableRebuildSettings.sdfSurfaceCachePageResolution == kSurfaceCachePageResolutions[i])
      {
        surfaceCachePageResolutionIndex = static_cast<int>(i);
        break;
      }
    }
    if (ImGui::BeginCombo("Surface cache page", surfaceCacheResolutionLabel(editableRebuildSettings.sdfSurfaceCachePageResolution)))
    {
      for (size_t i = 0u; i < std::size(kSurfaceCachePageResolutions); ++i)
      {
        const bool selected = surfaceCachePageResolutionIndex == static_cast<int>(i);
        if (ImGui::Selectable(surfaceCacheResolutionLabel(kSurfaceCachePageResolutions[i]), selected))
        {
          editableRebuildSettings.sdfSurfaceCachePageResolution = kSurfaceCachePageResolutions[i];
          surfaceCachePageResolutionIndex = static_cast<int>(i);
        }
        if (selected)
        {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    const uint32_t surfaceCacheTexelsPerPage =
        editableRebuildSettings.sdfSurfaceCachePageResolution * editableRebuildSettings.sdfSurfaceCachePageResolution;
    if (editableRebuildSettings.sdfSurfaceCachePageResolution == 0u)
    {
      ImGui::TextDisabled("Surface cache atlas and shading trace fallback will be disabled after rebuild.");
    }
    else
    {
      ImGui::TextDisabled("Higher page sizes rebuild the graph and scale resident/scratch cost by %ux.", surfaceCacheTexelsPerPage);
    }
    result.runtimeSettingsChanged |= ImGui::Checkbox("Trace surface-cache materials", &sdfTraceSurfaceCacheMaterialsEnabled);
    ImGui::TextDisabled("Adds sampled SDF page material ids and UVs to SDF hit records for surfel and GI shading.");
    const uint32_t worldCwbvhTraversalMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
    int traceBackendIndex = 0;
    if (editableRebuildSettings.enableSoftwareRayTracing)
    {
      traceBackendIndex = softwareRayTraceTraversalMode == worldCwbvhTraversalMode ? 2 : 1;
    }
    if (ImGui::Combo("Trace backend", &traceBackendIndex, "SDF\0BVH Local\0BVH World CWBVH\0"))
    {
      switch (traceBackendIndex)
      {
      case 0:
        editableRebuildSettings.enableSignedDistanceFields = true;
        editableRebuildSettings.enableSoftwareRayTracing = false;
        editableRebuildSettings.displayOutput = RendererDisplayOutput::SignedDistanceFieldsTrace;
        break;
      case 1:
        editableRebuildSettings.enableSoftwareRayTracing = true;
        softwareRayTraceTraversalMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance);
        editableRebuildSettings.displayOutput = RendererDisplayOutput::SoftwareRayTrace;
        break;
      case 2:
        editableRebuildSettings.enableSoftwareRayTracing = true;
        softwareRayTraceTraversalMode = worldCwbvhTraversalMode;
        softwareRayTracePrimitiveMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle);
        softwareRayTraceTlasMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH);
        editableRebuildSettings.displayOutput = RendererDisplayOutput::SoftwareRayTrace;
        break;
      default:
        break;
      }
      result.runtimeSettingsChanged = true;
      result.rebuildRequested = true;
    }
    ImGui::TextDisabled("SDF uses the brick tracer; BVH modes use the software triangle tracer and can be swapped from the debug output.");
    if (!kRendererEnableSoftwareRayTracingPath)
    {
      editableRebuildSettings.enableSoftwareRayTracing = false;
      if (rendererDisplayOutputIsSoftwareRayTracing(editableRebuildSettings.displayOutput))
      {
        editableRebuildSettings.displayOutput = RendererDisplayOutput::ShadedColor;
        result.rebuildRequested = true;
      }
      ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Enable Software Ray Tracing", &editableRebuildSettings.enableSoftwareRayTracing) && !editableRebuildSettings.enableSoftwareRayTracing &&
        rendererDisplayOutputIsSoftwareRayTracing(editableRebuildSettings.displayOutput))
    {
      editableRebuildSettings.displayOutput = RendererDisplayOutput::ShadedColor;
    }
    if (!kRendererEnableSoftwareRayTracingPath)
    {
      ImGui::EndDisabled();
      ImGui::TextDisabled("Software ray tracing is disabled for the SDF-only path.");
    }
    ImGui::TextDisabled("Builds fallback-scene BLAS/TLAS and enables camera-ray software tracing outputs after rebuild.");
  }

  if (kRendererEnableSoftwareRayTracingPath && editableRebuildSettings.enableSoftwareRayTracing && ImGui::CollapsingHeader("Software Ray Tracing", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::TextDisabled("SWRT output writes closest triangle id and scene instance id per pixel.");
    if (ImGui::TreeNodeEx("SSRT Fallback Controls", ImGuiTreeNodeFlags_DefaultOpen))
    {
      ImGui::PushID("SoftwareRayTracingSSRTControls");
      if (ImGui::Checkbox("Enable SSRT", &screenSpaceRayTraceEnabled))
        result.runtimeSettingsChanged = true;
      if (ImGui::Checkbox("SSRT screen traces", &screenSpaceRayTraceScreenTracesEnabled))
      {
        result.runtimeSettingsChanged = true;
        result.rebuildRequested = true;
        if (screenSpaceRayTraceScreenTracesEnabled)
        {
          screenSpaceRayTraceForceWorldFallback = false;
          screenSpaceRayTraceTraceAllForcedFallbackRays = false;
        }
        else
        {
          screenSpaceRayTraceForceWorldFallback = true;
          screenSpaceRayTraceTraceAllForcedFallbackRays = true;
          ensureScreenSpaceRayTraceFallback();
        }
      }
      if (!screenSpaceRayTraceScreenTracesEnabled)
      {
        screenSpaceRayTraceForceWorldFallback = true;
        screenSpaceRayTraceTraceAllForcedFallbackRays = true;
      }
      if (ImGui::Checkbox("SSRT force world fallback", &screenSpaceRayTraceForceWorldFallback))
      {
        result.runtimeSettingsChanged = true;
        if (screenSpaceRayTraceForceWorldFallback && !screenSpaceRayTraceSdfFallbackEnabled && !screenSpaceRayTraceSoftwareFallbackEnabled)
        {
          ensureScreenSpaceRayTraceFallback();
          result.rebuildRequested = true;
        }
      }
      if (ImGui::Checkbox("SSRT trace all forced fallback rays", &screenSpaceRayTraceTraceAllForcedFallbackRays))
      {
        result.runtimeSettingsChanged = true;
        result.rebuildRequested = true;
        if (screenSpaceRayTraceTraceAllForcedFallbackRays)
        {
          screenSpaceRayTraceForceWorldFallback = true;
          ensureScreenSpaceRayTraceFallback();
        }
      }

      static constexpr uint32_t kRayTraceTileSizes[] = {1u, 2u, 4u, 8u, 16u};
      int rayTraceTileIndex = 0;
      for (size_t i = 0u; i < std::size(kRayTraceTileSizes); ++i)
      {
        if (screenSpaceRayTraceTileSize == kRayTraceTileSizes[i])
        {
          rayTraceTileIndex = static_cast<int>(i);
          break;
        }
      }
      if (ImGui::SliderInt("SSRT tile", &rayTraceTileIndex, 0, static_cast<int>(std::size(kRayTraceTileSizes)) - 1))
      {
        screenSpaceRayTraceTileSize = kRayTraceTileSizes[static_cast<size_t>(rayTraceTileIndex)];
        result.runtimeSettingsChanged = true;
        if (screenSpaceRayTraceTraceAllForcedFallbackRays)
          result.rebuildRequested = true;
      }
      ImGui::SameLine();
      ImGui::Text("%ux%u", screenSpaceRayTraceTileSize, screenSpaceRayTraceTileSize);
      if (ImGui::SliderScalar("SSRT rays / tile", ImGuiDataType_U32, &screenSpaceRayTraceRaysPerTile, &kOne, &kOneTwentyEight))
      {
        result.runtimeSettingsChanged = true;
        if (screenSpaceRayTraceTraceAllForcedFallbackRays)
          result.rebuildRequested = true;
      }
      result.runtimeSettingsChanged |= ImGui::SliderScalar("SSRT max steps", ImGuiDataType_U32, &screenSpaceRayTraceMaxSteps, &kOne, &kOneTwentyEight);
      const uint32_t forcedFallbackRayCount = computeScreenSpaceRayTraceTotalRayBudget(
          screenSpaceRayTraceViewportWidth,
          screenSpaceRayTraceViewportHeight,
          screenSpaceRayTraceTileSize,
          screenSpaceRayTraceRaysPerTile);
      if (screenSpaceRayTraceTraceAllForcedFallbackRays)
      {
        screenSpaceRayTraceMaxFailedRays = forcedFallbackRayCount;
        screenSpaceRayTraceSoftwareFallbackBudget = forcedFallbackRayCount;
        ImGui::TextDisabled("Forced fallback budget: %u rays", forcedFallbackRayCount);
        ImGui::BeginDisabled();
      }
      if (ImGui::SliderScalar("SSRT failed ray cap", ImGuiDataType_U32, &screenSpaceRayTraceMaxFailedRays, &kOneKi, &kFourMi))
        result.rebuildRequested = true;
      bool sdfFallbackAvailable = editableRebuildSettings.enableSignedDistanceFields;
      if (!sdfFallbackAvailable)
        screenSpaceRayTraceSdfFallbackEnabled = false;
      if (!sdfFallbackAvailable)
        ImGui::BeginDisabled();
      if (ImGui::Checkbox("SSRT SDF fallback", &screenSpaceRayTraceSdfFallbackEnabled))
      {
        result.runtimeSettingsChanged = true;
        result.rebuildRequested = true;
        if (screenSpaceRayTraceSdfFallbackEnabled)
          screenSpaceRayTraceSoftwareFallbackEnabled = false;
      }
      if (!sdfFallbackAvailable)
        ImGui::EndDisabled();
      if (ImGui::Checkbox("SSRT SWRT fallback", &screenSpaceRayTraceSoftwareFallbackEnabled))
      {
        result.runtimeSettingsChanged = true;
        result.rebuildRequested = true;
        if (screenSpaceRayTraceSoftwareFallbackEnabled)
          screenSpaceRayTraceSdfFallbackEnabled = false;
      }
      result.runtimeSettingsChanged |= ImGui::Checkbox("SSRT SWRT trace stats", &screenSpaceRayTraceSoftwareFallbackTraceStats);
      ImGui::TextDisabled("Trace stats use the debug SWRT shader so TLAS/BLAS visit counters are visible.");
      if (screenSpaceRayTraceForceWorldFallback && !screenSpaceRayTraceSdfFallbackEnabled && !screenSpaceRayTraceSoftwareFallbackEnabled)
      {
        ensureScreenSpaceRayTraceFallback();
        result.rebuildRequested = true;
      }
      if (ImGui::SliderScalar("SSRT SWRT budget", ImGuiDataType_U32, &screenSpaceRayTraceSoftwareFallbackBudget, &kOneKi, &kFourMi))
        result.rebuildRequested = true;
      if (screenSpaceRayTraceTraceAllForcedFallbackRays)
        ImGui::EndDisabled();
      result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT min distance", &screenSpaceRayTraceMinDistance, 0.0001f, 8.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
      result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT max distance", &screenSpaceRayTraceMaxDistance, 0.01f, 100000.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
      if (screenSpaceRayTraceMinDistance > screenSpaceRayTraceMaxDistance)
      {
        std::swap(screenSpaceRayTraceMinDistance, screenSpaceRayTraceMaxDistance);
        result.runtimeSettingsChanged = true;
      }
      result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT thickness", &screenSpaceRayTraceSurfaceThickness, 0.00001f, 0.05f, "%.5f", ImGuiSliderFlags_Logarithmic);
      result.runtimeSettingsChanged |= ImGui::SliderFloat("SSRT origin bias", &screenSpaceRayTraceOriginBias, 0.0f, 1.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
      if (!screenSpaceRayTraceScreenTracesEnabled)
        ImGui::TextDisabled("Screen traversal is disabled; all generated rays go directly to the selected acceleration fallback.");
      ImGui::Text(
          "SSRT rays off-screen %u / %u (%.2f%%)  failed %u / %u%s",
          screenSpaceRayTraceStats.offscreenRays,
          screenSpaceRayTraceStats.totalRays,
          screenSpaceRayTraceStats.offscreenPercent,
          screenSpaceRayTraceStats.failedRays,
          screenSpaceRayTraceStats.capacity,
          screenSpaceRayTraceStats.overflow != 0u ? " overflow" : "");
      ImGui::Text(
          "SSRT SWRT fallback rays %u  hit %u  miss %u  invalid %u",
          screenFallbackTraceStats.totalRays,
          screenFallbackTraceStats.hitRays,
          screenFallbackTraceStats.missRays,
          screenFallbackTraceStats.invalidDirectionRays);
      ImGui::PopID();
      ImGui::TreePop();
    }

    static constexpr uint32_t kTraceResolutionDivisors[] = {1u, 2u, 4u, 8u};
    int traceResolutionIndex = 0;
    for (size_t i = 0u; i < std::size(kTraceResolutionDivisors); ++i)
    {
      if (editableRebuildSettings.softwareRayTraceResolutionDivisor == kTraceResolutionDivisors[i])
      {
        traceResolutionIndex = static_cast<int>(i);
        break;
      }
    }
    if (ImGui::SliderInt("SWRT output resolution", &traceResolutionIndex, 0, static_cast<int>(std::size(kTraceResolutionDivisors)) - 1, traceResolutionIndex == 0 ? "1x" : (traceResolutionIndex == 1 ? "1/2" : (traceResolutionIndex == 2 ? "1/4" : "1/8"))))
    {
      editableRebuildSettings.softwareRayTraceResolutionDivisor = kTraceResolutionDivisors[static_cast<size_t>(traceResolutionIndex)];
      result.rebuildRequested = true;
    }
    ImGui::TextDisabled("Lower resolutions rebuild the SWRT output and reduce camera rays quadratically.");
    int traversalModeIndex = static_cast<int>(std::min<uint32_t>(softwareRayTraceTraversalMode, 1u));
    if (ImGui::Combo("Traversal", &traversalModeIndex, "Local instance\0World CWBVH\0"))
    {
      softwareRayTraceTraversalMode = static_cast<uint32_t>(traversalModeIndex);
      result.rebuildRequested = true;
    }
    const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
    if (useWorldCwbvhTraversal)
    {
      softwareRayTracePrimitiveMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle);
      softwareRayTraceTlasMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH);
    }
    ImGui::BeginDisabled(useWorldCwbvhTraversal);
    int primitiveModeIndex = static_cast<int>(std::min<uint32_t>(softwareRayTracePrimitiveMode, 1u));
    bool primitiveModeChanged = false;
    if (ImGui::Combo("Primitive", &primitiveModeIndex, "Triangle CWBVH\0RBVH Cards\0"))
    {
      softwareRayTracePrimitiveMode = static_cast<uint32_t>(primitiveModeIndex);
      primitiveModeChanged = true;
      result.rebuildRequested = true;
    }
    if (primitiveModeChanged)
    {
      ImGui::TextDisabled("Primitive changes apply after renderer rebuild.");
    }
    int tlasModeIndex = static_cast<int>(std::min<uint32_t>(softwareRayTraceTlasMode, 1u));
    if (ImGui::Combo("TLAS", &tlasModeIndex, "CWBVH8\0CPU BVH4\0"))
    {
      softwareRayTraceTlasMode = static_cast<uint32_t>(tlasModeIndex);
      result.rebuildRequested = true;
    }
    ImGui::EndDisabled();
    if (softwareRayTracePrimitiveMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::RBVHCard))
    {
      static constexpr uint32_t kMaxRbvhLod = softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u;
      if (ImGui::SliderScalar("RBVH fallback file LOD", ImGuiDataType_U32, &softwareRayTraceFallbackLod, &kZero, &kMaxRbvhLod))
      {
        result.rebuildRequested = true;
      }
      if (ImGui::SliderScalar("RBVH card LOD", ImGuiDataType_U32, &softwareRayTraceRbvhLod, &kZero, &kMaxRbvhLod))
      {
        result.rebuildRequested = true;
      }
      ImGui::TextDisabled("LOD changes rebuild the renderer because they replace the RBVH BLAS and atlas texture.");
    }
    int shaderVariantIndex = static_cast<int>(std::min<uint32_t>(softwareRayTraceShaderVariant, 2u));
    if (ImGui::Combo("Shader variant", &shaderVariantIndex, "Auto\0Release\0Debug\0"))
    {
      softwareRayTraceShaderVariant = static_cast<uint32_t>(shaderVariantIndex);
      result.rebuildRequested = true;
    }
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SWRT min distance", &softwareRayTraceMinDistance, 0.0001f, 8.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
    result.runtimeSettingsChanged |= ImGui::SliderFloat("SWRT max distance", &softwareRayTraceMaxDistance, 1.0f, 100000.0f, "%.1f m", ImGuiSliderFlags_Logarithmic);
    if (softwareRayTraceMinDistance > softwareRayTraceMaxDistance)
    {
      std::swap(softwareRayTraceMinDistance, softwareRayTraceMaxDistance);
      result.runtimeSettingsChanged = true;
    }
    result.runtimeSettingsChanged |= ImGui::Checkbox("Traversal early-out", &softwareRayTraceTraversalEarlyOut);
    if (softwareRayTraceTraversalEarlyOut)
    {
      result.runtimeSettingsChanged |= ImGui::SliderFloat("Early-out bias", &softwareRayTraceTraversalEarlyOutBias, 0.0f, 10.0f, "%.4f m", ImGuiSliderFlags_Logarithmic);
    }
    result.runtimeSettingsChanged |= ImGui::SliderScalar("Heatmap max iterations", ImGuiDataType_U32, &softwareRayTraceHeatmapMaxIterations, &kOne, &kTenThousand);
    result.runtimeSettingsChanged |= ImGui::Checkbox("Only selected TLAS instance", &softwareRayTraceIsolateTLASInstance);
    if (softwareRayTraceTLASCandidateInstanceCount > 0u)
    {
      const uint32_t maxTLASInstanceIndex = softwareRayTraceTLASCandidateInstanceCount - 1u;
      softwareRayTraceSelectedTLASInstance = std::min(softwareRayTraceSelectedTLASInstance, maxTLASInstanceIndex);
      result.runtimeSettingsChanged |= ImGui::SliderScalar("TLAS instance", ImGuiDataType_U32, &softwareRayTraceSelectedTLASInstance, &kZero, &maxTLASInstanceIndex);
      ImGui::TextDisabled(
          "TLAS candidates %u  filtered %u  selected scene instance %u", softwareRayTraceTLASCandidateInstanceCount, softwareRayTraceTLASFilteredInstanceCount, softwareRayTraceSelectedTLASSceneInstanceId);
    }
    else
    {
      softwareRayTraceSelectedTLASInstance = 0u;
      ImGui::TextDisabled("TLAS candidates 0");
    }
    result.runtimeSettingsChanged |= ImGui::Checkbox("Overlay TLAS bounds", &softwareRayTraceOverlayTLASBounds);
    if (softwareRayTraceOverlayTLASBounds)
    {
      result.runtimeSettingsChanged |= ImGui::Checkbox("TLAS CWBVH bounds", &softwareRayTraceOverlayTLASCwbvhBounds);
      result.runtimeSettingsChanged |= ImGui::SliderScalar("TLAS level", ImGuiDataType_U32, &softwareRayTraceOverlayTLASLevel, &kZero, &kThirtyTwo);
      result.runtimeSettingsChanged |= ImGui::SliderFloat("Bounds thickness", &softwareRayTraceOverlayLineThickness, 0.5f, 6.0f, "%.2f px");
      ImGui::TextDisabled("Higher levels are more expensive because the overlay is evaluated per pixel in the trace pass.");
    }

    ImGui::SeparatorText("SWRT Stats");
    ImGui::Text("Trace resolution 1/%u", editableRebuildSettings.softwareRayTraceResolutionDivisor);
    ImGui::Text("Trace implementation Direct");
    ImGui::Text("Shader variant %s", softwareRayTraceShaderVariant == 1u ? "Release" : (softwareRayTraceShaderVariant == 2u ? "Debug" : "Auto"));
    ImGui::Text("Dynamic ray cursor %u", softwareRayTraceStats.dynamicRayFetchCursor);
    ImGui::Text(
        "Node visits TLAS %u  BLAS %u  world BLAS %u  tri tests %u",
        softwareRayTraceStats.tlasNodeVisits,
        softwareRayTraceStats.blasNodeVisits,
        softwareRayTraceStats.worldBlasNodeVisits,
        softwareRayTraceStats.triangleTests);
    ImGui::SeparatorText("SWRT BVH");
    ImGui::Text("Traversal mode %s", softwareRayTraceTraversalMode == 1u ? "World CWBVH" : "Local instance");
    ImGui::Text("Primitive mode %s", softwareRayTracePrimitiveMode == 1u ? "RBVH Cards" : "Triangle CWBVH");
    if (softwareRayTracePrimitiveMode == 1u)
    {
      ImGui::Text("RBVH fallback file LOD %u  card LOD %u", softwareRayTraceFallbackLod, softwareRayTraceRbvhLod);
    }
    ImGui::Text("TLAS mode %s", softwareRayTraceTlasMode == 1u ? "CPU BVH4" : "CWBVH8");
    ImGui::Text("TLAS instances %u  canonical nodes %u  CWBVH nodes %u  BVH4 nodes %u", softwareRayTraceTLASInstanceCount, softwareRayTraceTLASCanonicalNodeCount, softwareRayTraceTLASCwbvhNodeCount, softwareRayTraceTLASBvh4NodeCount);
    ImGui::TextDisabled(
        "TLAS bridge candidates %u  filtered %u  valid %u  skipped missing BLAS %u  skipped bounds %u",
        softwareRayTraceTLASCandidateInstanceCount,
        softwareRayTraceTLASFilteredInstanceCount,
        softwareRayTraceTLASValidInstanceCount,
        softwareRayTraceTLASSkippedMissingBlasCount,
        softwareRayTraceTLASSkippedInvalidBoundsCount);
    ImGui::Text("TLAS BVH4 SAH build %.3f  current %.3f  growth %.2fx%s", softwareRayTraceTLASBvh4BuildSahScore, softwareRayTraceTLASBvh4CurrentSahScore, softwareRayTraceTLASBvh4SahGrowth, softwareRayTraceTLASBvh4RebuiltForSah ? " rebuilt" : "");
    ImGui::Text("BLAS count %u  canonical nodes %u  CWBVH nodes %u", softwareRayTraceBLASCount, softwareRayTraceBLASCanonicalNodeCount, softwareRayTraceBLASCwbvhNodeCount);
    ImGui::Text("BLAS primitives %u", softwareRayTraceBLASPrimitiveCount);
    ImGui::TextDisabled(
        "BLAS bridge prepared %u  fallback loaded %u  missing %u  skipped missing %u",
        softwareRayTraceBLASPreparedCount,
        softwareRayTraceBLASAvailableFallbackCount,
        softwareRayTraceBLASMissingFallbackCount,
        softwareRayTraceBLASSkippedMissingFallbackCount);
    if (softwareRayTracePrimitiveMode == 1u)
    {
      ImGui::Text("RBVH cards %u", softwareRayTraceBLASRbvhCardCount);
    }
  }

  const bool stagedSettingsChanged = editableRebuildSettings != appliedRebuildSettings;
  const bool stagedSettingsValid = isRendererRebuildSettingsValid(editableRebuildSettings);
  if (!stagedSettingsValid)
  {
    ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.42f, 1.0f), "Pass settings are invalid. Temporal values must stay in range and page/atlas settings must be supported.");
  }
  else if (stagedSettingsChanged)
  {
    ImGui::TextColored(ImVec4(0.82f, 0.86f, 0.55f, 1.0f), "Compile-time pass settings changed. Rebuild to apply them.");
  }
  else
  {
    ImGui::TextDisabled("Pass settings match the active render graph.");
  }

  if (ImGui::Button("Reset Pending Pass Settings"))
  {
    editableRebuildSettings = appliedRebuildSettings;
  }
  ImGui::SameLine();
  const bool rebuildDisabled = !stagedSettingsChanged || !stagedSettingsValid;
  if (rebuildDisabled)
  {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Recompile / Restart Renderer"))
  {
    result.rebuildRequested = true;
  }
  if (rebuildDisabled)
  {
    ImGui::EndDisabled();
  }
  ImGui::End();
  return result;
}

static void buildSignedDistanceFieldsOverlay(
    const gpusigneddistancefields::GPUSignedDistanceFieldsManager &manager,
    engine::SceneManager &sceneManager,
    gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager &sceneBridge,
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Settings &traceSettings,
    sdf::rendering::SDFTraceDebugPass::Settings &sparseSdfTraceDebugSettings,
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass::Settings &atlasSettings,
    const gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass *fallbackScenePreparePass,
    bool traceDebugPassActive,
    bool traceDebugPassEnabled,
    bool traceDebugStatsResourcesValid,
    uint64_t traceDebugRecordCalls,
    uint64_t traceDebugDispatches,
    uint64_t traceDebugSkippedDisabled,
    const gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::LocalTraceStats &localTraceStats,
    bool &fallbackSceneFrustumCullingEnabled,
    const SDFFrameStats &sdfStats)
{
  auto settings = sceneBridge.getSettings();
  bool settingsChanged = false;
  const uint32_t atlasResolution = manager.getSettings().atlasBrickCapacityPerAxis * gpusigneddistancefields::GPUSignedDistanceFieldsManager::BRICK_RESOLUTION;
  const auto &bridgeDiagnostics = sceneBridge.getDiagnostics();
  const auto &staticPathResources = manager.getPathResources(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static);
  const auto &skinnedPathResources = manager.getPathResources(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned);
  const auto &sharedResources = manager.getSharedResources();
  const auto &surfaceCacheLayout = sharedResources.surfaceCacheLayout;
  const auto &fallbackMeshTable = manager.getUploadedMeshTable();
  uint64_t fallbackTriangleCount = 0u;
  for (const auto &mesh : fallbackMeshTable)
    fallbackTriangleCount += mesh.triangleCount;
  static constexpr uint32_t kZero = 0u;
  static constexpr uint32_t kOne = 1u;
  static constexpr uint32_t kTraceMaxStepLimit = 1024u;
  const uint32_t atlasResolutionMinusOne = atlasResolution > 0u ? atlasResolution - 1u : 0u;

  ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

  const float outerMargin = 16.0f;
  const float windowWidth = std::min(420.0f, viewport->WorkSize.x - outerMargin * 2.0f);
  const float windowX = viewport->WorkPos.x + viewport->WorkSize.x - windowWidth - outerMargin;
  const float windowY = viewport->WorkPos.y + 160.0f;

  ImGui::SetNextWindowPos(ImVec2(windowX, windowY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(windowWidth, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);

  ImGui::Begin("Signed Distance Fields", nullptr, windowFlags);
  ImGui::SetWindowFontScale(0.9f);
  ImGui::TextUnformatted("GPUSignedDistanceFields build controls");
  ImGui::TextDisabled("Static meshes build once. Dynamic meshes requeue only after transform or skinning changes.");

  int updateBudget = static_cast<int>(settings.maxMeshUpdatesPerFrame);
  if (ImGui::SliderInt("Mesh updates/frame", &updateBudget, 0, 128))
  {
    settings.maxMeshUpdatesPerFrame = static_cast<uint32_t>(std::max(updateBudget, 0));
    settingsChanged = true;
  }

  ImGui::Text("Pending %u  Updated last frame %u", sceneBridge.getPendingUpdateCount(), sceneBridge.getLastFrameUpdatedCount());
  ImGui::Text("Fallback cache loaded %u  missing %u", bridgeDiagnostics.availableFallbackMeshCount, bridgeDiagnostics.missingFallbackMeshCount);
  ImGui::Text("Uploaded instances static %u  skinned %u", staticPathResources.instanceCount, skinnedPathResources.instanceCount);

  ImGui::SeparatorText("Local Instance Trace");
  ImGui::Checkbox("Collect local ray stats", &traceSettings.localTraceStatsEnabled);
  ImGui::Text(
      "Trace pass %s  enabled %s  stats %s  mode %u",
      traceDebugPassActive ? "active" : "inactive",
      traceDebugPassEnabled ? "yes" : "no",
      traceDebugStatsResourcesValid ? "valid" : "missing",
      static_cast<uint32_t>(traceSettings.mode));
  ImGui::Text(
      "Record calls %llu  dispatches %llu  disabled skips %llu",
      static_cast<unsigned long long>(traceDebugRecordCalls),
      static_cast<unsigned long long>(traceDebugDispatches),
      static_cast<unsigned long long>(traceDebugSkippedDisabled));
  ImGui::Text("Max distance %.1f  max steps %u", traceSettings.maxTraceDistance, traceSettings.maxSteps);
  ImGui::Text("Ray pixels total %u  local %u  disabled %u", localTraceStats.totalPixels, localTraceStats.localModePixels, localTraceStats.disabledPixels);
  ImGui::Text("Cache pixels resident %u  missing %u", localTraceStats.cachePixels, localTraceStats.noCachePixels);
  ImGui::Text("Cascade tests %u  hits %u  refs %u", localTraceStats.cascadeTests, localTraceStats.cascadeHits, localTraceStats.cascadeInstanceRefs);
  ImGui::Text("Instance tests %u  AABB hits %u", localTraceStats.instanceTests, localTraceStats.instanceAabbHits);
  ImGui::Text("SDF samples %u  sample hits %u", localTraceStats.sdfSamples, localTraceStats.sdfHits);
  ImGui::Text("Pixels hit %u  miss %u  albedo samples %u", localTraceStats.hitPixels, localTraceStats.noHitPixels, localTraceStats.albedoSamples);
  ImGui::Text("Cascade debug pixels %u  hits %u", localTraceStats.cascadeDebugPixels, localTraceStats.cascadeDebugHits);

  ImGui::SeparatorText("Fallback Scene");
  ImGui::Text(
      "Geometry meshes %zu  vertices %zu  indices %zu  triangles %llu",
      fallbackMeshTable.size(),
      manager.getUploadedVertices().size(),
      manager.getUploadedIndices().size(),
      static_cast<unsigned long long>(fallbackTriangleCount));
  ImGui::Text(
      "Instances static %u/%u active  skinned %u/%u active",
      staticPathResources.activeInstanceCount,
      staticPathResources.instanceCount,
      skinnedPathResources.activeInstanceCount,
      skinnedPathResources.instanceCount);
  ImGui::Text(
      "Prepared %u  skipped empty %u runtime %u fallback %u scene %u bounds %u",
      bridgeDiagnostics.preparedInstanceCount,
      bridgeDiagnostics.skippedEmptySceneInstanceCount,
      bridgeDiagnostics.skippedMissingRuntimeDataCount,
      bridgeDiagnostics.skippedMissingFallbackMeshCount,
      bridgeDiagnostics.skippedMissingSceneInstanceCount,
      bridgeDiagnostics.skippedInvalidBoundsCount);
  if (fallbackScenePreparePass != nullptr)
  {
    auto countVisibleCommands = [](const auto &commands) -> std::pair<uint32_t, uint64_t>
    {
      uint32_t visibleDraws = 0u;
      uint64_t visibleTriangles = 0u;
      for (const auto &command : commands)
      {
        if (command.instanceCount == 0u || command.vertexCount == 0u)
          continue;
        ++visibleDraws;
        visibleTriangles += command.vertexCount / 3u;
      }
      return {visibleDraws, visibleTriangles};
    };
    const auto [staticVisibleDraws, staticVisibleTriangles] =
        countVisibleCommands(fallbackScenePreparePass->getPreparedCommands(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static));
    const auto [skinnedVisibleDraws, skinnedVisibleTriangles] =
        countVisibleCommands(fallbackScenePreparePass->getPreparedCommands(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned));
    ImGui::Text(
        "Prepared draws static %u/%u  skinned %u/%u",
        staticVisibleDraws,
        fallbackScenePreparePass->getIndirectDrawCount(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static),
        skinnedVisibleDraws,
        fallbackScenePreparePass->getIndirectDrawCount(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned));
    ImGui::Text(
        "Prepared triangles static %llu  skinned %llu",
        static_cast<unsigned long long>(staticVisibleTriangles),
        static_cast<unsigned long long>(skinnedVisibleTriangles));
  }
  else
  {
    ImGui::TextDisabled("Fallback scene debug pass is not compiled for the current output.");
  }

  ImGui::SeparatorText("Surface Cache");
  if (manager.getSettings().surfaceCachePageResolution == 0u)
  {
    ImGui::TextDisabled("Surface cache disabled.");
  }
  else
  {
    const uint64_t surfaceCacheResidentBytes = sharedResources.surfaceCacheMaterialReferenceAtlasSize;
    const std::string residentText = formatBytes(surfaceCacheResidentBytes);
    ImGui::Text(
        "Page %ux%u  pages %u  row %u",
        surfaceCacheLayout.pageResolution,
        surfaceCacheLayout.pageResolution,
        surfaceCacheLayout.pageCount,
        surfaceCacheLayout.pagesPerRow);
    ImGui::Text("Material ref atlas %ux%u  resident %s", surfaceCacheLayout.atlasWidth, surfaceCacheLayout.atlasHeight, residentText.c_str());
  }

  ImGui::SeparatorText("Sync CPU");
  ImGui::Text("Total %.3f ms", bridgeDiagnostics.syncTotalMs);
  ImGui::Text("Clipmap %.3f  Walk %.3f  Evict %.3f", bridgeDiagnostics.syncClipmapUpdateMs, bridgeDiagnostics.syncSceneWalkMs, bridgeDiagnostics.syncEvictMs);
  ImGui::Text(
      "Geom upload %.3f  Rebuild jobs %.3f  Scroll jobs %.3f  Job upload %.3f",
      bridgeDiagnostics.syncSharedGeometryUploadMs,
      bridgeDiagnostics.syncRebuildJobsMs,
      bridgeDiagnostics.syncScrollJobsMs,
      bridgeDiagnostics.syncJobUploadMs);
  ImGui::Text(
      "Walk detail: lookup %.3f  bounds %.3f  inst upload %.3f  bone upload %.3f",
      bridgeDiagnostics.syncMeshLookupMs,
      bridgeDiagnostics.syncBoundsMs,
      bridgeDiagnostics.syncInstanceUploadMs,
      bridgeDiagnostics.syncBoneUploadMs);

  ImGui::SeparatorText("Brick Allocator");
  // Actual allocated = maxBricks minus free bricks collected this frame by CollectClearBricksCS.
  const uint32_t allocatedBricks = (sdfStats.maxBricks > sdfStats.freeBricks) ? (sdfStats.maxBricks - sdfStats.freeBricks) : 0u;
  if (sdfStats.maxBricks > 0u)
  {
    const float usageRatio = static_cast<float>(allocatedBricks) / static_cast<float>(sdfStats.maxBricks);
    ImGui::Text("Allocated %u / %u  (%.1f%%)", allocatedBricks, sdfStats.maxBricks, usageRatio * 100.0f);
    ImGui::ProgressBar(usageRatio, ImVec2(-1.0f, 0.0f));
  }
  else
  {
    ImGui::Text("Allocated %u", allocatedBricks);
  }
  ImGui::Text("Dirty %u  Free %u  Clear %u  Merge %u", sdfStats.dirtyBricks, sdfStats.freeBricks, sdfStats.clearBricks, sdfStats.mergeBricks);

  // Circular history buffers for brick counts over time (last 128 frames)
  static constexpr int kHistorySize = 128;
  static float brickCountHistory[kHistorySize] = {};
  static float dirtyBrickHistory[kHistorySize] = {};
  static int historyOffset = 0;
  static bool historyInitialized = false;

  if (!historyInitialized)
  {
    std::fill(std::begin(brickCountHistory), std::end(brickCountHistory), 0.0f);
    std::fill(std::begin(dirtyBrickHistory), std::end(dirtyBrickHistory), 0.0f);
    historyInitialized = true;
  }

  if (sdfStats.maxBricks > 0u)
  {
    brickCountHistory[historyOffset] = static_cast<float>(allocatedBricks);
    dirtyBrickHistory[historyOffset] = static_cast<float>(sdfStats.dirtyBricks);
    historyOffset = (historyOffset + 1) % kHistorySize;

    const float graphMax = static_cast<float>(sdfStats.maxBricks);
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%u", allocatedBricks);
    ImGui::PlotLines("##brick_count", brickCountHistory, kHistorySize, historyOffset, overlay, 0.0f, graphMax, ImVec2(-1.0f, 48.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextDisabled("Alloc");

    std::snprintf(overlay, sizeof(overlay), "%u", sdfStats.dirtyBricks);
    ImGui::PlotLines("##dirty_bricks", dirtyBrickHistory, kHistorySize, historyOffset, overlay, 0.0f, graphMax, ImVec2(-1.0f, 48.0f));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextDisabled("Dirty");
  }

  if (!bridgeDiagnostics.lastLoadedFallbackPath.empty())
  {
    ImGui::TextWrapped("Last loaded fallback: %s", bridgeDiagnostics.lastLoadedFallbackPath.c_str());
  }
  if (!bridgeDiagnostics.lastMissingFallbackPath.empty())
  {
    ImGui::TextWrapped("Last missing fallback: %s", bridgeDiagnostics.lastMissingFallbackPath.c_str());
  }

  ImGui::SeparatorText("Visualization");
  int traceMode = static_cast<int>(traceSettings.mode);
  if (ImGui::Combo(
	          "Trace Mode",
	          &traceMode,
	          "Raymarch Surface\0Step Count Heatmap\0Material Id\0Material UV\0Material UV Window\0Evaluated Material Albedo\0Per-instance SDF Raymarch\0Per-instance SDF Normal\0Per-instance SDF Albedo\0Per-instance SDF Cascades\0Per-instance SDF Cascade Instances\0Per-instance SDF Blinn\0Per-instance SDF Step Heatmap\0"))
  {
    traceSettings.mode = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode>(traceMode);
  }
  if (traceSettings.mode == gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode::StepCount ||
      traceSettings.mode == gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Mode::LocalInstanceStepCount)
  {
    ImGui::ColorEdit3("Trace heat low", traceSettings.traceHeatmapLowColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
    ImGui::ColorEdit3("Trace heat mid", traceSettings.traceHeatmapMidColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
    ImGui::ColorEdit3("Trace heat high", traceSettings.traceHeatmapHighColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  }
  ImGui::SliderScalar("Trace max steps", ImGuiDataType_U32, &traceSettings.maxSteps, &kOne, &kTraceMaxStepLimit);
  ImGui::SliderFloat("Trace max distance", &traceSettings.maxTraceDistance, 1.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Trace hit scale", &traceSettings.hitThresholdScale, 0.25f, 2.0f, "%.3f");
  ImGui::SliderFloat("Empty-space step", &traceSettings.emptySpaceStepScale, 0.1f, 4.0f, "%.2f");

  ImGui::SeparatorText("Sparse SDF Trace");
  ImGui::SliderScalar("Sparse max steps", ImGuiDataType_U32, &sparseSdfTraceDebugSettings.maxSteps, &kOne, &kTraceMaxStepLimit);
  ImGui::SliderFloat("Sparse max distance", &sparseSdfTraceDebugSettings.maxTraceDistance, 1.0f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Sparse hit scale", &sparseSdfTraceDebugSettings.hitThresholdScale, 0.05f, 1.0f, "%.3f");
  ImGui::SliderFloat("Sparse step scale", &sparseSdfTraceDebugSettings.stepScale, 0.1f, 1.5f, "%.2f");
  ImGui::SliderFloat("Sparse min step", &sparseSdfTraceDebugSettings.minStepScale, 0.01f, 1.0f, "%.3f");
  ImGui::DragFloat("Sparse heat low count", &sparseSdfTraceDebugSettings.traceHeatmapLowCount, 1.0f, 0.0f, 4096.0f, "%.0f");
  ImGui::DragFloat("Sparse heat mid count", &sparseSdfTraceDebugSettings.traceHeatmapMidCount, 1.0f, 1.0f, 4096.0f, "%.0f");
  ImGui::DragFloat("Sparse heat high count", &sparseSdfTraceDebugSettings.traceHeatmapHighCount, 1.0f, 2.0f, 4096.0f, "%.0f");
  sparseSdfTraceDebugSettings.traceHeatmapMidCount = std::max(sparseSdfTraceDebugSettings.traceHeatmapMidCount, sparseSdfTraceDebugSettings.traceHeatmapLowCount + 1.0f);
  sparseSdfTraceDebugSettings.traceHeatmapHighCount = std::max(sparseSdfTraceDebugSettings.traceHeatmapHighCount, sparseSdfTraceDebugSettings.traceHeatmapMidCount + 1.0f);
  ImGui::ColorEdit3("Sparse heat low", sparseSdfTraceDebugSettings.traceHeatmapLowColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  ImGui::ColorEdit3("Sparse heat mid", sparseSdfTraceDebugSettings.traceHeatmapMidColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
  ImGui::ColorEdit3("Sparse heat high", sparseSdfTraceDebugSettings.traceHeatmapHighColor.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);

  int cascadeOverride = traceSettings.cascadeOverride;
  if (ImGui::SliderInt("Trace cascade override", &cascadeOverride, -1, static_cast<int>(manager.getCascadeCount()) - 1, cascadeOverride < 0 ? "Auto" : "%d"))
  {
    traceSettings.cascadeOverride = cascadeOverride;
  }
  ImGui::Checkbox("Fallback frustum culling", &fallbackSceneFrustumCullingEnabled);
  ImGui::TextDisabled("Fallback scene rendering is compiled and recorded only while a fallback scene display output is selected.");

  int atlasAxis = static_cast<int>(atlasSettings.axis);
  if (ImGui::Combo("Atlas axis", &atlasAxis, "X\0Y\0Z\0"))
  {
    atlasSettings.axis = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass::Axis>(atlasAxis);
  }
  atlasSettings.sliceIndex = std::min(atlasSettings.sliceIndex, atlasResolution > 0u ? atlasResolution - 1u : 0u);
  if (atlasResolution > 0u)
  {
    ImGui::SliderScalar("Atlas slice", ImGuiDataType_U32, &atlasSettings.sliceIndex, &kZero, &atlasResolutionMinusOne);
  }
  ImGui::Checkbox("Atlas brick grid", &atlasSettings.showBrickGrid);

  if (settingsChanged)
  {
    sceneBridge.setSettings(settings);
  }

  ImGui::SeparatorText("Mesh Instances");
  if (sceneManager.getObjectInstanceCount() == 0u)
  {
    ImGui::TextDisabled("No virtual-geometry instances are registered.");
    ImGui::End();
    return;
  }

  for (uint32_t denseIndex = 0u; denseIndex < sceneManager.getObjectInstanceCount(); ++denseIndex)
  {
    const InstanceId instanceId = sceneManager.getObjectInstanceIdByDenseIndex(denseIndex);
    if (instanceId == INVALID_INSTANCE_ID)
    {
      continue;
    }

    auto *record = sceneManager.getObjectInstanceRecordByDenseIndex(denseIndex);
    if (record == nullptr)
      continue;

    const bool isQueued = sceneBridge.isQueued(instanceId);
    const bool hasCompletedInitialBuild = sceneBridge.hasCompletedInitialBuild(instanceId);
    const bool isDynamic = sceneManager.isObjectInstanceSdfDynamic(instanceId);
    const std::string header = record->objectName + "##gpusdf_instance_" + std::to_string(instanceId);
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
      continue;
    }

    ImGui::PushID(static_cast<int>(instanceId));
    bool dynamic = isDynamic;
    if (ImGui::Checkbox("Dynamic", &dynamic))
    {
      sceneManager.setObjectInstanceSdfDynamic(instanceId, dynamic);
    }
    ImGui::SameLine();
    if (ImGui::Button("Queue rebuild"))
    {
      sceneManager.requestObjectInstanceSdfRebuild(instanceId);
    }

    ImGui::Text("Instance %u  Build %s  Queued %s", instanceId, hasCompletedInitialBuild ? "complete" : "pending", isQueued ? "yes" : "no");
    ImGui::TextDisabled(
        "Content rev %llu  Manual rev %llu",
        static_cast<unsigned long long>(record->gpuSignedDistanceFields.contentRevision),
        static_cast<unsigned long long>(record->gpuSignedDistanceFields.rebuildRevision));
    if (!dynamic)
    {
      ImGui::TextDisabled("Static mode ignores transform and skinning changes until you queue a rebuild.");
    }
    ImGui::PopID();
  }

  ImGui::End();
}

static double findPassGpuTimeMs(const std::vector<PassTimingSummary> &passTimings, const char *passName)
{
  for (const PassTimingSummary &pass : passTimings)
  {
    if (pass.name == passName)
    {
      return pass.gpuTimeMs;
    }
  }
  return 0.0;
}

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
     << " sdf=" << cpuFrame.sdfSyncMs << "ms"
     << " swrtBlas=" << cpuFrame.softwareBlasSync.wallMs << "ms"
     << " swrtTlas=" << cpuFrame.softwareTlasSync.wallMs << "ms"
     << " swrtTlasRebuilt=" << (cpuFrame.softwareTlasBuild.rebuilt ? "yes" : "no")
     << " swrtTlasRefit=" << (cpuFrame.softwareTlasBuild.refitted ? "yes" : "no")
     << " swrtTlasPartial=" << (cpuFrame.softwareTlasBuild.partialRebuilt ? "yes" : "no")
     << " swrtTlasDirty=" << cpuFrame.softwareTlasBuild.dirtyInstanceCount
     << " swrtTlasBvh4=" << cpuFrame.softwareTlasBuild.bvh4NodeCount << "/" << cpuFrame.softwareTlasBuild.bvh4LeafCount
     << " swrtTlasBvh4Sah=" << cpuFrame.softwareTlasBuild.bvh4BuildSahScore << "/" << cpuFrame.softwareTlasBuild.bvh4CurrentSahScore << "x" << cpuFrame.softwareTlasBuild.bvh4SahGrowth
     << " swrtTlasBvh4RebuiltSah=" << (cpuFrame.softwareTlasBuild.bvh4RebuiltForSah ? "yes" : "no")
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
  appendOperationSummary(opsA, "sdf", cpuFrame.sdfSync);
  opsA << " ";
  appendOperationSummary(opsA, "swrtBlas", cpuFrame.softwareBlasSync);
  opsA << " ";
  appendOperationSummary(opsA, "swrtTlas", cpuFrame.softwareTlasSync);
  opsA << " ";
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
  pages << "[VSM] frame=" << frameIndex << " renderPages=" << stats.renderPages << " normal=" << stats.normalRenderPages << " fallback=" << stats.fallbackRenderPages << " uniqueDrawnPages=" << stats.uniqueDrawnPages
        << " avgClustersPerPage=" << std::fixed << std::setprecision(2) << stats.averageClustersPerDrawnPage << " maxClustersPerPage=" << stats.maxClustersPerDrawnPage << " drawCommands=" << stats.totalDrawCommands
        << " executed=" << stats.executedDrawCommands << " tris=" << stats.totalDrawTriangles << " overflow=" << stats.drawCommandOverflow;
  os::Logger::log(pages.str());

  std::ostringstream residency;
  residency << "[VSM] frame=" << frameIndex << " vpt alloc=" << stats.allocatedPages << " valid=" << stats.validPages << " dirty=" << stats.dirtyPages << " visible=" << stats.visiblePages
            << " fallback=" << stats.fallbackPages << " phys used=" << stats.usedPhysicalPages << "/" << stats.totalPhysicalPages << " free=" << stats.freePhysicalPages << " queues h=" << stats.hierarchyQueueSize
            << " read=" << stats.readQueueSize << " cluster=" << stats.clusterQueueSize;
  os::Logger::log(residency.str());
}

static math::Quatf multiplyQuaternions(const math::Quatf &a, const math::Quatf &b)
{
  return math::Quatf(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
      .normalize();
}

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

struct ScreenProbeReadbackBuffers
{
  ReadbackBufferSet traceStats;
};

struct ScreenSpaceRayTraceReadbackBuffers
{
  ReadbackBufferSet traceStats;
};

struct SurfelReadbackBuffers
{
  ReadbackBufferSet traceStats;
};

struct SoftwareRayTraceReadbackBuffers
{
  ReadbackBufferSet traceStats;
  ReadbackBufferSet screenFallbackTraceStats;
};

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

static uint32_t nextPowerOfTwo(uint32_t value)
{
  uint32_t result = 1u;
  while (result < std::max(1u, value))
  {
    result <<= 1u;
  }
  return result;
}

struct ImperfectShadowMapVPL
{
  math::Vec3f position = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f normal = math::Vec3f(0.0f, 1.0f, 0.0f);
  math::Vec3f flux = math::Vec3f(1.0f, 1.0f, 1.0f);
  float range = 1.0f;
  float fluxWeight = 1.0f;
  VirtualShadowMapManager::LightId lightId = VirtualShadowMapManager::INVALID_LIGHT_ID;
};

static uint32_t computeIsmVplFacePageTableResolution(uint32_t pagesPerFace)
{
  const uint32_t requestedSide = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(std::max(pagesPerFace, 1u)))));
  return std::min(nextPowerOfTwo(requestedSide), VirtualShadowMapManager::DEFAULT_PAGE_TABLE_RESOLUTION);
}

struct IsmVplSurfaceTriangle
{
  math::Vec3f p0 = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f p1 = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f p2 = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f normal = math::Vec3f(0.0f, 1.0f, 0.0f);
  float area = 0.0f;
};

static std::vector<ImperfectShadowMapVPL> buildDirectionalLightSphereProxyVPLs(
    uint32_t activeVpls,
    const math::Vec3f &sceneCenter,
    float sceneRadius,
    const math::Vec3f &lightDirection,
    const math::Vec3f &lightColor,
    float lightIntensity)
{
  std::vector<ImperfectShadowMapVPL> vpls;
  vpls.reserve(activeVpls);
  if (activeVpls == 0u)
  {
    return vpls;
  }

  const math::Vec3f incoming = lightDirection.normalize();
  const math::Vec3f directionToLight = (incoming * -1.0f).normalize();
  constexpr float kGoldenAngle = 2.39996322972865332f;
  const uint32_t candidateCount = std::max(activeVpls * 4u, activeVpls + 16u);
  const float radius = std::max(sceneRadius, 0.1f);

  for (uint32_t candidateIndex = 0u; candidateIndex < candidateCount && vpls.size() < activeVpls; ++candidateIndex)
  {
    const float u = (static_cast<float>(candidateIndex) + 0.5f) / static_cast<float>(candidateCount);
    const float z = 1.0f - 2.0f * u;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const float theta = kGoldenAngle * static_cast<float>(candidateIndex);
    math::Vec3f normal(std::cos(theta) * r, z, std::sin(theta) * r);
    if (normal.dot(directionToLight) <= 0.05f)
    {
      continue;
    }

    const float ndotl = std::max(normal.dot(directionToLight), 0.0f);
    ImperfectShadowMapVPL vpl;
    vpl.normal = normal.normalize();
    vpl.position = sceneCenter + vpl.normal * (radius * 0.85f);
    vpl.range = std::max(radius * 1.75f, 1.0f);
    vpl.fluxWeight = ndotl / static_cast<float>(activeVpls);
    vpl.flux = lightColor * (std::max(lightIntensity, 0.0f) * vpl.fluxWeight);
    vpls.push_back(vpl);
  }

  while (vpls.size() < activeVpls)
  {
    const uint32_t fallbackIndex = static_cast<uint32_t>(vpls.size());
    const float theta = kGoldenAngle * static_cast<float>(fallbackIndex);
    math::Vec3f tangentA = std::abs(directionToLight[1]) < 0.95f ? directionToLight.cross(math::Vec3f(0.0f, 1.0f, 0.0f)).normalize() : directionToLight.cross(math::Vec3f(1.0f, 0.0f, 0.0f)).normalize();
    math::Vec3f tangentB = tangentA.cross(directionToLight).normalize();
    const float diskRadius = std::sqrt((static_cast<float>(fallbackIndex) + 0.5f) / static_cast<float>(activeVpls));
    const math::Vec3f normal = (directionToLight * 0.75f + tangentA * (std::cos(theta) * diskRadius) + tangentB * (std::sin(theta) * diskRadius)).normalize();

    ImperfectShadowMapVPL vpl;
    vpl.normal = normal;
    vpl.position = sceneCenter + normal * (radius * 0.85f);
    vpl.range = std::max(radius * 1.75f, 1.0f);
    vpl.fluxWeight = 1.0f / static_cast<float>(activeVpls);
    vpl.flux = lightColor * (std::max(lightIntensity, 0.0f) * vpl.fluxWeight);
    vpls.push_back(vpl);
  }

  return vpls;
}

static bool intersectRayTriangle(
    const math::Vec3f &rayOrigin,
    const math::Vec3f &rayDirection,
    const IsmVplSurfaceTriangle &triangle,
    float &outT)
{
  const math::Vec3f edge0 = triangle.p1 - triangle.p0;
  const math::Vec3f edge1 = triangle.p2 - triangle.p0;
  const math::Vec3f p = rayDirection.cross(edge1);
  const float det = edge0.dot(p);
  if (std::abs(det) <= 1.0e-7f)
    return false;

  const float invDet = 1.0f / det;
  const math::Vec3f t = rayOrigin - triangle.p0;
  const float u = t.dot(p) * invDet;
  if (u < 0.0f || u > 1.0f)
    return false;

  const math::Vec3f q = t.cross(edge0);
  const float v = rayDirection.dot(q) * invDet;
  if (v < 0.0f || u + v > 1.0f)
    return false;

  const float hitT = edge1.dot(q) * invDet;
  if (hitT <= 1.0e-5f)
    return false;

  outT = hitT;
  return true;
}

static void appendIsmVplSurfaceTriangles(const FallbackMeshData &mesh, std::vector<IsmVplSurfaceTriangle> &triangles)
{
  for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
  {
    const uint32_t i0 = mesh.indices[indexOffset + 0u];
    const uint32_t i1 = mesh.indices[indexOffset + 1u];
    const uint32_t i2 = mesh.indices[indexOffset + 2u];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
      continue;

    const FallbackMeshVertex &v0 = mesh.vertices[i0];
    const FallbackMeshVertex &v1 = mesh.vertices[i1];
    const FallbackMeshVertex &v2 = mesh.vertices[i2];
    const math::Vec3f p0(v0.position[0], v0.position[1], v0.position[2]);
    const math::Vec3f p1(v1.position[0], v1.position[1], v1.position[2]);
    const math::Vec3f p2(v2.position[0], v2.position[1], v2.position[2]);
    const math::Vec3f edge0 = p1 - p0;
    const math::Vec3f edge1 = p2 - p0;
    const math::Vec3f cross = edge0.cross(edge1);
    const float crossLength = cross.length();
    if (!std::isfinite(crossLength) || crossLength <= 1.0e-7f)
      continue;

    const math::Vec3f triangleNormal = cross * (1.0f / crossLength);
    math::Vec3f vertexNormal(
        v0.normal[0] + v1.normal[0] + v2.normal[0],
        v0.normal[1] + v1.normal[1] + v2.normal[1],
        v0.normal[2] + v1.normal[2] + v2.normal[2]);
    const float vertexNormalLength = vertexNormal.length();
    math::Vec3f normal = vertexNormalLength > 1.0e-6f ? vertexNormal * (1.0f / vertexNormalLength) : triangleNormal;
    if (normal.dot(triangleNormal) < 0.0f)
      normal = normal * -1.0f;

    IsmVplSurfaceTriangle triangle;
    triangle.p0 = p0;
    triangle.p1 = p1;
    triangle.p2 = p2;
    triangle.normal = normal;
    triangle.area = crossLength * 0.5f;
    triangles.push_back(triangle);
  }
}

static std::vector<IsmVplSurfaceTriangle> loadIsmVplSurfaceTriangles(const std::vector<fs::path> &fallbackMeshPaths)
{
  std::vector<IsmVplSurfaceTriangle> triangles;
  for (const fs::path &path : fallbackMeshPaths)
  {
    std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> sceneEntries;
    if (softwarebvh::SoftwareRayTraceSceneFile::load(path.string(), sceneEntries))
    {
      for (const softwarebvh::SoftwareRayTraceSceneInstanceData &entry : sceneEntries)
      {
        if (entry.lodLevel == 0u)
          appendIsmVplSurfaceTriangles(entry.fallbackMesh, triangles);
      }
      continue;
    }

    std::vector<FallbackMeshData> fallbackMeshes;
    if (FallbackMeshFile::load(path.string(), fallbackMeshes))
    {
      for (const FallbackMeshData &mesh : fallbackMeshes)
        appendIsmVplSurfaceTriangles(mesh, triangles);
      continue;
    }

    FallbackMeshData fallbackMesh;
    if (FallbackMeshFile::load(path.string(), fallbackMesh))
      appendIsmVplSurfaceTriangles(fallbackMesh, triangles);
  }
  return triangles;
}

static std::vector<IsmVplSurfaceTriangle> buildWorldIsmVplSurfaceTriangles(
    const std::vector<IsmVplSurfaceTriangle> &surfaceTriangles,
    const math::Vec3f &objectPosition,
    const math::Quatf &objectRotation)
{
  std::vector<IsmVplSurfaceTriangle> worldTriangles;
  worldTriangles.reserve(surfaceTriangles.size());
  for (const IsmVplSurfaceTriangle &triangle : surfaceTriangles)
  {
    IsmVplSurfaceTriangle worldTriangle;
    worldTriangle.p0 = objectPosition + objectRotation * triangle.p0;
    worldTriangle.p1 = objectPosition + objectRotation * triangle.p1;
    worldTriangle.p2 = objectPosition + objectRotation * triangle.p2;
    worldTriangle.normal = (objectRotation * triangle.normal).normalize();
    worldTriangle.area = triangle.area;
    worldTriangles.push_back(worldTriangle);
  }
  return worldTriangles;
}

static std::vector<ImperfectShadowMapVPL> buildDirectionalLightRsmRaycastVPLs(
    uint32_t activeVpls,
    const std::vector<IsmVplSurfaceTriangle> &worldTriangles,
    const math::Vec3f &sceneCenter,
    float sceneRadius,
    const math::Vec3f &lightDirection,
    const math::Vec3f &lightColor,
    float lightIntensity,
    IsmVplBuildStats *outStats)
{
  if (activeVpls == 0u || worldTriangles.empty())
    return {};

  struct HitCandidate
  {
    math::Vec3f position = math::Vec3f(0.0f, 0.0f, 0.0f);
    math::Vec3f normal = math::Vec3f(0.0f, 1.0f, 0.0f);
    float weight = 0.0f;
    float cumulativeWeight = 0.0f;
    uint32_t x = 0u;
    uint32_t y = 0u;
  };

  const math::Vec3f incoming = lightDirection.normalize();
  const math::Vec3f directionToLight = (incoming * -1.0f).normalize();
  const math::Vec3f helper = std::abs(incoming[1]) < 0.95f ? math::Vec3f(0.0f, 1.0f, 0.0f) : math::Vec3f(1.0f, 0.0f, 0.0f);
  const math::Vec3f lightRight = helper.cross(incoming).normalize();
  const math::Vec3f lightUp = incoming.cross(lightRight).normalize();
  const float radius = std::max(sceneRadius, 0.1f);
  const float extent = radius * 1.05f;
  const float startDistance = radius * 2.25f;
  const float maxDistance = radius * 4.5f;
  const uint32_t requestedRayCount = std::max(activeVpls * 32u, 256u);
  const uint32_t gridSide = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(requestedRayCount)))));
  const uint32_t rayCount = gridSide * gridSide;
  const float surfaceBias = std::max(radius * 0.0002f, 0.002f);

  std::vector<HitCandidate> hits;
  hits.reserve(rayCount);
  float totalWeight = 0.0f;
  for (uint32_t y = 0u; y < gridSide; ++y)
  {
    for (uint32_t x = 0u; x < gridSide; ++x)
    {
      const uint32_t rayIndex = y * gridSide + x;
      const float jitterX = haltonSequence(rayIndex + 1u, 2u) - 0.5f;
      const float jitterY = haltonSequence(rayIndex + 1u, 3u) - 0.5f;
      const float u = ((static_cast<float>(x) + 0.5f + jitterX * 0.75f) / static_cast<float>(gridSide)) * 2.0f - 1.0f;
      const float v = ((static_cast<float>(y) + 0.5f + jitterY * 0.75f) / static_cast<float>(gridSide)) * 2.0f - 1.0f;
      const math::Vec3f rayOrigin = sceneCenter - incoming * startDistance + lightRight * (u * extent) + lightUp * (v * extent);

      float closestT = maxDistance;
      const IsmVplSurfaceTriangle *closestTriangle = nullptr;
      for (const IsmVplSurfaceTriangle &triangle : worldTriangles)
      {
        const float ndotl = triangle.normal.dot(directionToLight);
        if (ndotl <= 0.02f)
          continue;

        float hitT = 0.0f;
        if (intersectRayTriangle(rayOrigin, incoming, triangle, hitT) && hitT < closestT)
        {
          closestT = hitT;
          closestTriangle = &triangle;
        }
      }

      if (closestTriangle == nullptr)
        continue;

      const float ndotl = std::max(closestTriangle->normal.dot(directionToLight), 0.0f);
      const float weight = ndotl;
      if (!std::isfinite(weight) || weight <= 0.0f)
        continue;

      totalWeight += weight;
      hits.push_back(HitCandidate{
          .position = rayOrigin + incoming * closestT + closestTriangle->normal * surfaceBias,
          .normal = closestTriangle->normal,
          .weight = weight,
          .cumulativeWeight = totalWeight,
          .x = x,
          .y = y,
      });
    }
  }

  if (outStats != nullptr)
  {
    outStats->rsmRayCount = rayCount;
    outStats->rsmHitCount = static_cast<uint32_t>(hits.size());
  }

  if (hits.empty() || totalWeight <= 0.0f)
    return {};

  std::vector<ImperfectShadowMapVPL> vpls;
  vpls.reserve(activeVpls);

  auto appendVpl = [&](const HitCandidate &hit, float representativeWeight)
  {
    ImperfectShadowMapVPL vpl;
    vpl.position = hit.position;
    vpl.normal = hit.normal;
    vpl.range = std::max(radius * 1.75f, 1.0f);
    vpl.fluxWeight = std::max(representativeWeight, 0.0f);
    vpl.flux = lightColor * (std::max(lightIntensity, 0.0f) * vpl.fluxWeight);
    vpls.push_back(vpl);
  };

  struct Stratum
  {
    uint32_t x0 = 0u;
    uint32_t x1 = 0u;
    uint32_t y0 = 0u;
    uint32_t y1 = 0u;
    float weight = 0.0f;
    const HitCandidate *selectedHit = nullptr;
  };

  const uint32_t stratumSide = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(activeVpls)))));
  std::vector<Stratum> strata;
  strata.reserve(stratumSide * stratumSide);
  for (uint32_t sy = 0u; sy < stratumSide; ++sy)
  {
    for (uint32_t sx = 0u; sx < stratumSide; ++sx)
    {
      Stratum stratum;
      stratum.x0 = static_cast<uint32_t>((static_cast<uint64_t>(sx) * gridSide) / stratumSide);
      stratum.x1 = static_cast<uint32_t>((static_cast<uint64_t>(sx + 1u) * gridSide) / stratumSide);
      stratum.y0 = static_cast<uint32_t>((static_cast<uint64_t>(sy) * gridSide) / stratumSide);
      stratum.y1 = static_cast<uint32_t>((static_cast<uint64_t>(sy + 1u) * gridSide) / stratumSide);

      for (const HitCandidate &hit : hits)
      {
        if (hit.x < stratum.x0 || hit.x >= stratum.x1 || hit.y < stratum.y0 || hit.y >= stratum.y1)
          continue;
        stratum.weight += hit.weight;
      }

      if (stratum.weight <= 0.0f)
        continue;

      const float targetWeight = haltonSequence(static_cast<uint32_t>(strata.size()) + 1u, 5u) * stratum.weight;
      float cumulativeWeight = 0.0f;
      for (const HitCandidate &hit : hits)
      {
        if (hit.x < stratum.x0 || hit.x >= stratum.x1 || hit.y < stratum.y0 || hit.y >= stratum.y1)
          continue;
        cumulativeWeight += hit.weight;
        if (cumulativeWeight >= targetWeight)
        {
          stratum.selectedHit = &hit;
          break;
        }
      }

      if (stratum.selectedHit == nullptr)
      {
        for (const HitCandidate &hit : hits)
        {
          if (hit.x >= stratum.x0 && hit.x < stratum.x1 && hit.y >= stratum.y0 && hit.y < stratum.y1)
          {
            stratum.selectedHit = &hit;
            break;
          }
        }
      }
      strata.push_back(stratum);
    }
  }

  for (const Stratum &stratum : strata)
  {
    if (vpls.size() >= activeVpls)
      break;
    if (stratum.selectedHit != nullptr)
      appendVpl(*stratum.selectedHit, stratum.weight / totalWeight);
  }

  uint32_t candidateCursor = 0u;
  while (vpls.size() < activeVpls)
  {
    const float targetWeight = (static_cast<float>(vpls.size()) + 0.5f) * totalWeight / static_cast<float>(activeVpls);
    while (candidateCursor + 1u < hits.size() && hits[candidateCursor].cumulativeWeight < targetWeight)
      ++candidateCursor;
    appendVpl(hits[candidateCursor], hits[candidateCursor].weight / totalWeight);
  }

  float selectedWeightSum = 0.0f;
  for (const ImperfectShadowMapVPL &vpl : vpls)
    selectedWeightSum += std::max(vpl.fluxWeight, 0.0f);
  if (selectedWeightSum > 1.0e-6f)
  {
    const float invSelectedWeightSum = 1.0f / selectedWeightSum;
    for (ImperfectShadowMapVPL &vpl : vpls)
    {
      vpl.fluxWeight *= invSelectedWeightSum;
      vpl.flux = lightColor * (std::max(lightIntensity, 0.0f) * vpl.fluxWeight);
    }
  }

  return vpls;
}

static void updateImperfectShadowMapVplFlux(
    std::vector<ImperfectShadowMapVPL> &vpls,
    const math::Vec3f &lightColor,
    float lightIntensity)
{
  const float clampedIntensity = std::max(lightIntensity, 0.0f);
  for (ImperfectShadowMapVPL &vpl : vpls)
    vpl.flux = lightColor * (clampedIntensity * std::max(vpl.fluxWeight, 0.0f));
}

static std::vector<ImperfectShadowMapVPL> buildDirectionalLightSurfaceVPLs(
    uint32_t activeVpls,
    const std::vector<IsmVplSurfaceTriangle> &surfaceTriangles,
    const math::Vec3f &objectPosition,
    const math::Quatf &objectRotation,
    const math::Vec3f &sceneCenter,
    float sceneRadius,
    const math::Vec3f &lightDirection,
    const math::Vec3f &lightColor,
    float lightIntensity,
    IsmVplBuildStats *outStats = nullptr)
{
  if (activeVpls == 0u)
    return {};
  if (surfaceTriangles.empty())
  {
    if (outStats != nullptr)
      outStats->spawnMode = IsmVplSpawnMode::SphereFallback;
    return buildDirectionalLightSphereProxyVPLs(activeVpls, sceneCenter, sceneRadius, lightDirection, lightColor, lightIntensity);
  }

  if (outStats != nullptr)
  {
    outStats->spawnMode = IsmVplSpawnMode::DirectionalRsmRaycast;
    outStats->surfaceTriangleCount = static_cast<uint32_t>(std::min<size_t>(surfaceTriangles.size(), std::numeric_limits<uint32_t>::max()));
  }

  std::vector<IsmVplSurfaceTriangle> worldTriangles = buildWorldIsmVplSurfaceTriangles(surfaceTriangles, objectPosition, objectRotation);
  std::vector<ImperfectShadowMapVPL> rsmVpls = buildDirectionalLightRsmRaycastVPLs(
      activeVpls,
      worldTriangles,
      sceneCenter,
      sceneRadius,
      lightDirection,
      lightColor,
      lightIntensity,
      outStats);
  if (!rsmVpls.empty())
    return rsmVpls;

  struct Candidate
  {
    uint32_t triangleIndex = 0u;
    math::Vec3f worldNormal = math::Vec3f(0.0f, 1.0f, 0.0f);
    float weight = 0.0f;
    float cumulativeWeight = 0.0f;
  };

  const math::Vec3f incoming = lightDirection.normalize();
  const math::Vec3f directionToLight = (incoming * -1.0f).normalize();
  std::vector<Candidate> candidates;
  candidates.reserve(surfaceTriangles.size());
  float totalWeight = 0.0f;
  for (uint32_t triangleIndex = 0u; triangleIndex < surfaceTriangles.size(); ++triangleIndex)
  {
    const IsmVplSurfaceTriangle &triangle = surfaceTriangles[triangleIndex];
    const math::Vec3f worldNormal = (objectRotation * triangle.normal).normalize();
    const float ndotl = worldNormal.dot(directionToLight);
    if (ndotl <= 0.02f)
      continue;

    const float weight = triangle.area * ndotl;
    if (!std::isfinite(weight) || weight <= 0.0f)
      continue;

    totalWeight += weight;
    candidates.push_back(Candidate{
        .triangleIndex = triangleIndex,
        .worldNormal = worldNormal,
        .weight = weight,
        .cumulativeWeight = totalWeight,
    });
  }

  if (candidates.empty() || totalWeight <= 0.0f)
  {
    if (outStats != nullptr)
      outStats->spawnMode = IsmVplSpawnMode::SphereFallback;
    return buildDirectionalLightSphereProxyVPLs(activeVpls, sceneCenter, sceneRadius, lightDirection, lightColor, lightIntensity);
  }

  if (outStats != nullptr)
    outStats->spawnMode = IsmVplSpawnMode::SurfaceImportanceFallback;

  std::vector<ImperfectShadowMapVPL> vpls;
  vpls.reserve(activeVpls);
  uint32_t candidateCursor = 0u;
  const float surfaceBias = std::max(sceneRadius * 0.0002f, 0.002f);
  for (uint32_t vplIndex = 0u; vplIndex < activeVpls; ++vplIndex)
  {
    const float targetWeight = (static_cast<float>(vplIndex) + 0.5f) * totalWeight / static_cast<float>(activeVpls);
    while (candidateCursor + 1u < candidates.size() && candidates[candidateCursor].cumulativeWeight < targetWeight)
      ++candidateCursor;

    const Candidate &candidate = candidates[candidateCursor];
    const IsmVplSurfaceTriangle &triangle = surfaceTriangles[candidate.triangleIndex];
    const float r0 = std::clamp(haltonSequence(vplIndex + 1u, 2u), 0.0f, 0.999999f);
    const float r1 = std::clamp(haltonSequence(vplIndex + 1u, 3u), 0.0f, 0.999999f);
    const float sr0 = std::sqrt(r0);
    const float b0 = 1.0f - sr0;
    const float b1 = (1.0f - r1) * sr0;
    const float b2 = r1 * sr0;
    const math::Vec3f localPosition = triangle.p0 * b0 + triangle.p1 * b1 + triangle.p2 * b2;

    ImperfectShadowMapVPL vpl;
    vpl.normal = candidate.worldNormal;
    vpl.position = objectPosition + objectRotation * localPosition + vpl.normal * surfaceBias;
    vpl.range = std::max(sceneRadius * 1.75f, 1.0f);
    vpl.fluxWeight = candidate.weight / totalWeight;
    vpl.flux = lightColor * (std::max(lightIntensity, 0.0f) * vpl.fluxWeight);
    vpls.push_back(vpl);
  }

  return vpls;
}

class ImperfectShadowMapVplPointDebugPass : public rendering::Pass
{
public:
  static constexpr uint32_t kMaxDebugVpls = 64u;

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    math::Mat4f viewProjection = math::Mat4f::identity();
    int32_t selectedVpl = -1;
    float pointRadiusPixels = 5.0f;
    float exposure = 8.0f;
    bool enabled = false;
  };

  explicit ImperfectShadowMapVplPointDebugPass(rendering::Texture outputTexture, Settings settings) : outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~ImperfectShadowMapVplPointDebugPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (vplBuffer_.isValid())
      renderGraph->deleteBuffer(vplBuffer_);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    settings_.width = std::max(settings_.width, 1u);
    settings_.height = std::max(settings_.height, 1u);
    settings_.pointRadiusPixels = std::clamp(settings_.pointRadiusPixels, 1.0f, 64.0f);
    settings_.exposure = std::max(settings_.exposure, 0.0f);
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void setVPLs(const std::vector<ImperfectShadowMapVPL> &vpls)
  {
    vplCount_ = std::min<uint32_t>(static_cast<uint32_t>(vpls.size()), kMaxDebugVpls);
    vplUpload_.fill({});
    for (uint32_t i = 0u; i < vplCount_; ++i)
    {
      const ImperfectShadowMapVPL &vpl = vpls[i];
      vplUpload_[i].positionRange[0] = vpl.position[0];
      vplUpload_[i].positionRange[1] = vpl.position[1];
      vplUpload_[i].positionRange[2] = vpl.position[2];
      vplUpload_[i].positionRange[3] = vpl.range;
      vplUpload_[i].normalPdf[0] = vpl.normal[0];
      vplUpload_[i].normalPdf[1] = vpl.normal[1];
      vplUpload_[i].normalPdf[2] = vpl.normal[2];
      vplUpload_[i].normalPdf[3] = vplCount_ > 0u ? 1.0f / static_cast<float>(vplCount_) : 0.0f;
      vplUpload_[i].fluxLight[0] = vpl.flux[0];
      vplUpload_[i].fluxLight[1] = vpl.flux[1];
      vplUpload_[i].fluxLight[2] = vpl.flux[2];
      vplUpload_[i].fluxLight[3] = static_cast<float>(vpl.lightId);
    }
    if (vplBuffer_.isValid())
    {
      writeVPLs();
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();
    writeVPLs();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.width + 7u) / 8u, (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct VPLGPU
  {
    float positionRange[4]{};
    float normalPdf[4]{};
    float fluxLight[4]{};
  };

  struct Uniforms
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    uint32_t viewportVplCountSelected[4]{1u, 1u, 0u, 0u};
    float radiusExposureEnabled[4]{5.0f, 8.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) % 16u == 0u, "ISM VPL debug uniforms must be 16-byte aligned");

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    vplBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_VPLs.buffer",
            .size = sizeof(VPLGPU) * kMaxDebugVpls,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vpls", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .storageTextures =
                    {
                        {.name = "outputTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                        {.binding = 0, .bufferView = makeStorageBufferView(uniformBuffer_, sizeof(Uniforms))},
                        {.binding = 1, .bufferView = makeStorageBufferView(vplBuffer_, sizeof(VPLGPU) * kMaxDebugVpls)},
                    },
                .storageTextures =
                    {
                        {.binding = 2, .textureView = makeStorageColorView(outputTexture_)},
                    },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/imperfect-shadow-map-vpl-points-debug-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeUniforms()
  {
    if (!uniformBuffer_.isValid())
      return;
    Uniforms uniforms{};
    uniforms.viewProjection = settings_.viewProjection;
    uniforms.viewportVplCountSelected[0] = std::max(settings_.width, 1u);
    uniforms.viewportVplCountSelected[1] = std::max(settings_.height, 1u);
    uniforms.viewportVplCountSelected[2] = vplCount_;
    uniforms.viewportVplCountSelected[3] = settings_.selectedVpl >= 0 ? static_cast<uint32_t>(settings_.selectedVpl + 1) : 0u;
    uniforms.radiusExposureEnabled[0] = settings_.pointRadiusPixels;
    uniforms.radiusExposureEnabled[1] = settings_.exposure;
    uniforms.radiusExposureEnabled[2] = settings_.enabled ? 1.0f : 0.0f;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  void writeVPLs()
  {
    if (!vplBuffer_.isValid())
      return;
    renderGraph->bufferWrite(vplBuffer_, 0u, sizeof(VPLGPU) * kMaxDebugVpls, vplUpload_.data());
  }

  rendering::Texture outputTexture_{};
  Settings settings_{};
  uint32_t vplCount_ = 0u;
  std::array<VPLGPU, kMaxDebugVpls> vplUpload_{};
  rendering::Buffer uniformBuffer_{};
  rendering::Buffer vplBuffer_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
};

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
  std::vector<fs::path> fallbackMeshPaths;
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

static fs::path buildRendererMaterialOutputDirectory(const fs::path &virtualGeometryPath);

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

static fs::path buildRendererSoftwareRaytraceScenePath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + ".softwareraytrace");
}

static fs::path buildRendererVSDFPath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + ".vsdf");
}

static fs::path buildRendererSparseVSDFPath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + ".sparse.vsdf");
}

static fs::path buildRendererVSDFShadingDirectory(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + "_vsdf_shading");
}

static fs::path buildRendererSceneMetaPath(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + ".meta");
}

static std::string escapeJsonString(const std::string &value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value)
  {
    switch (c)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

static void writeRendererSceneMetaFile(
    const RendererAssetPaths &paths,
    const VirtualGeometryEncodedData &encoded,
    const std::vector<fs::path> &softwareRaytraceFiles)
{
  const fs::path metaPath = buildRendererSceneMetaPath(paths.virtualGeometryPath);
  std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
  if (!file.is_open())
    throw std::runtime_error("Failed to write scene metadata file: " + metaPath.string());

  file << "{\n";
  file << "  \"version\": 1,\n";
  file << "  \"asset\": \"" << escapeJsonString(paths.assetPath.string()) << "\",\n";
  file << "  \"virtualGeometry\": \"" << escapeJsonString(paths.virtualGeometryPath.string()) << "\",\n";
  file << "  \"objectName\": \"" << escapeJsonString(paths.objectName) << "\",\n";
  file << "  \"shapeCount\": " << encoded.shapes.size() << ",\n";
  file << "  \"sceneInstanceIdPolicy\": \"virtualgeometry-shape-instance-id\",\n";
  file << "  \"raytrace\": {\n";
  file << "    \"primitive\": \"triangle-cwbvh\",\n";
  file << "    \"files\": [\n";
  for (size_t fileIndex = 0u; fileIndex < softwareRaytraceFiles.size(); ++fileIndex)
  {
    file << "      \"" << escapeJsonString(softwareRaytraceFiles[fileIndex].string()) << "\"";
    if (fileIndex + 1u < softwareRaytraceFiles.size())
      file << ",";
    file << "\n";
  }
  file << "    ]\n";
  file << "  }\n";
  file << "}\n";
}

static bool rendererHasSoftwareRaytraceSceneFile(const fs::path &virtualGeometryPath, const uint32_t expectedMeshCount)
{
  const fs::path scenePath = buildRendererSoftwareRaytraceScenePath(virtualGeometryPath);
  if (!fs::exists(scenePath))
    return false;

  softwarebvh::SoftwareRayTraceSceneFileHeader header{};
  if (!softwarebvh::SoftwareRayTraceSceneFile::loadHeader(scenePath.string(), header))
    return false;
  uint32_t lod0EntryCount = 0u;
  for (const softwarebvh::SoftwareRayTraceSceneFileEntryHeader &entry : header.entries)
  {
    if (entry.lodLevel == 0u)
      ++lod0EntryCount;
  }
  if (lod0EntryCount != expectedMeshCount)
  {
    os::Logger::logf("[SWRT] software raytrace scene lod0 entry mismatch for %s: expected=%u actual=%u total=%zu", scenePath.string().c_str(), expectedMeshCount, lod0EntryCount, header.entries.size());
    return false;
  }
  return true;
}

#if 0
static bool rendererHasVSDFSceneFile(const fs::path &virtualGeometryPath, const uint32_t expectedMeshCount)
{
  const fs::path vsdfPath = buildRendererVSDFPath(virtualGeometryPath);
  if (!fs::exists(vsdfPath))
    return false;

  VSDFFileHeader header{};
  if (!VSDFFile::loadHeader(vsdfPath.string(), header))
    return false;

  if (header.meshCount != expectedMeshCount || header.instanceCount != expectedMeshCount || header.cardCount == 0u || header.pageCount == 0u || header.shadingImageCount == 0u)
  {
    os::Logger::logf(
        "[VSDF] runtime cache metadata mismatch for %s: expectedMeshes=%u meshes=%u instances=%u cards=%u pages=%u images=%u",
        vsdfPath.string().c_str(),
        expectedMeshCount,
        header.meshCount,
        header.instanceCount,
        header.cardCount,
        header.pageCount,
        header.shadingImageCount);
    return false;
  }

  VSDFSceneData scene;
  if (!VSDFFile::load(vsdfPath.string(), scene))
    return false;
  bool hasSourceGeometryBakedShading = false;
  bool hasMeshDistanceSDF = !scene.sdfChunks.empty();
  static constexpr uint32_t kRequiredSdfFlags =
      VSDFSdfChunkFlag_MeshUnsignedDistance |
      VSDFSdfChunkFlag_PreciseUnsignedDistance |
      VSDFSdfChunkFlag_MeshOrientedSurfaceDistance |
      VSDFSdfChunkFlag_LocalNarrowBandDistance |
      VSDFSdfChunkFlag_WorldErrorGrid |
      VSDFSdfChunkFlag_ValidatedShellMinimum |
      VSDFSdfChunkFlag_ExplicitOpenSurfaceValidity |
      VSDFSdfChunkFlag_FeatureLocalNarrowBand |
      VSDFSdfChunkFlag_ProjectedOpenSurfaceDistance |
      VSDFSdfChunkFlag_AxisTightOpenBounds |
      VSDFSdfChunkFlag_OpenSurfaceGuardBand |
      VSDFSdfChunkFlag_SparseOctreeBrick8;
  for (const VSDFSdfChunkPayload &chunk : scene.sdfChunks)
  {
    const bool isSignedDistance = (chunk.metadata.flags & VSDFSdfChunkFlag_MeshSignedDistance) != 0u;
    const bool hasStaleConservativeOpenSurfaceDilation = (chunk.metadata.flags & VSDFSdfChunkFlag_ConservativeOpenSurfaceDilation) != 0u;
    const bool hasExpandedSdfBounds = (chunk.metadata.flags & VSDFSdfChunkFlag_ExpandedSdfBounds) != 0u;
    const bool hasSurfaceAxisPreservingGrid = (chunk.metadata.flags & VSDFSdfChunkFlag_SurfaceAxisPreservingGrid) != 0u;
    const bool hasLowPolyWorldErrorGrid = (chunk.metadata.flags & VSDFSdfChunkFlag_LowPolyWorldErrorGrid) != 0u;
    const bool hasFeatureAxisGrid = (chunk.metadata.flags & VSDFSdfChunkFlag_FeatureAxisGrid) != 0u;
    const bool hasAnisotropicGrid = (chunk.metadata.flags & VSDFSdfChunkFlag_AnisotropicGrid) != 0u;
    if ((chunk.metadata.flags & kRequiredSdfFlags) != kRequiredSdfFlags || isSignedDistance || hasStaleConservativeOpenSurfaceDilation || !hasExpandedSdfBounds || !hasSurfaceAxisPreservingGrid || !hasLowPolyWorldErrorGrid || !hasFeatureAxisGrid || !hasAnisotropicGrid)
    {
      hasMeshDistanceSDF = false;
      break;
    }
  }
  for (const VSDFShadingImage &image : scene.shadingImages)
  {
    if (image.parameter == VSDFShadingParameter_AlbedoOpacity &&
        (image.flags & VSDFShadingImageFlag_GeometryBaked) != 0u &&
        (image.flags & VSDFShadingImageFlag_SourceGeometry) != 0u &&
        (image.flags & VSDFShadingImageFlag_WorldSizedCards) != 0u)
    {
      hasSourceGeometryBakedShading = true;
      break;
    }
  }
  if (!hasSourceGeometryBakedShading)
  {
    os::Logger::logf("[VSDF] runtime cache does not use source-geometry world-sized shading pages, regenerating: %s", vsdfPath.string().c_str());
    return false;
  }
  if (!hasMeshDistanceSDF)
  {
    os::Logger::logf("[VSDF] runtime cache uses stale SDF chunks, regenerating adaptive mesh-distance SDFs: %s", vsdfPath.string().c_str());
    return false;
  }
  return true;
}
#endif

static std::vector<fs::path> collectRendererFallbackMeshPaths(const fs::path &virtualGeometryPath, uint32_t maxLods = 16u)
{
  std::vector<fs::path> paths;
  (void)maxLods;
  const fs::path scenePath = buildRendererSoftwareRaytraceScenePath(virtualGeometryPath);
  if (fs::exists(scenePath))
    paths.push_back(scenePath);
  return paths;
}

static fs::path buildRendererMaterialOutputDirectory(const fs::path &virtualGeometryPath)
{
  return virtualGeometryPath.parent_path() / (virtualGeometryPath.stem().string() + "_materials");
}

static bool rendererHasUsableOBJMaterials(const std::vector<std::string> &materialFiles)
{
  bool foundTexturedMaterial = false;
  for (const std::string &materialFile : materialFiles)
  {
    if (materialFile.empty())
      continue;

    VirtualTextureSystem::PreparedMaterial preparedMaterial;
    if (!VirtualMaterialFile::load(materialFile, preparedMaterial))
      return false;

    foundTexturedMaterial = foundTexturedMaterial || preparedMaterial.textureCount != 0u;
  }

  return foundTexturedMaterial;
}

static VirtualGeometrySourceAsset loadRendererSourceAsset(const fs::path &assetPath, const fs::path &virtualGeometryPath)
{
  if (isOBJAssetPath(assetPath))
    return VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), buildRendererMaterialOutputDirectory(virtualGeometryPath).string());
  if (isGLTFAssetPath(assetPath))
    return VirtualGeometryBuilder::loadAssetFromGLTFFile(assetPath.string());
  throw std::runtime_error("Unsupported asset type: " + assetPath.string());
}

static virtualgeometry::SparseSDFBuildSettings makeRendererSparseSDFBuildSettings()
{
  virtualgeometry::SparseSDFBuildSettings settings{};
  settings.maxLevel = 4u;
  settings.requestedError = 0.5f;
  settings.narrowBandVoxels = 6u;
  settings.maxRefinedCellsPerBrick = sdf::VSDF_BRICK_CELL_COUNT;
  settings.progressLogging = false;
  settings.sdfWorkerCount = std::max(1u, std::thread::hardware_concurrency());
  return settings;
}

static bool rendererSparseVSDFMatchesSettings(const sdf::SDFSceneFile &file, const virtualgeometry::SparseSDFBuildSettings &settings)
{
  if (file.header().instanceCount == 0u || file.instances().empty())
    return false;
  if (file.header().buildSettingsHash != virtualgeometry::SparseSDFBuilder::buildSettingsHash(settings))
    return false;

  for (const sdf::VSDFInstanceDirectoryEntry &entry : file.instances())
  {
    if (entry.distanceFormat != sdf::VSDF_DISTANCE_R8_SNORM)
      return false;
    if (entry.maxLevel < settings.maxLevel)
      return false;
    if (entry.requestedError <= 0.0f || entry.requestedError > settings.requestedError * 1.05f)
      return false;
  }
  return true;
}

static bool ensureRendererSparseVSDFScene(const RendererAssetPaths &paths, fs::path *outPath)
{
  const fs::path sparseVSDFPath = buildRendererSparseVSDFPath(paths.virtualGeometryPath);
  if (outPath != nullptr)
    *outPath = sparseVSDFPath;

  const virtualgeometry::SparseSDFBuildSettings settings = makeRendererSparseSDFBuildSettings();
  std::string error;
  sdf::SDFSceneFile existing;
  if (fs::exists(sparseVSDFPath) && existing.load(sparseVSDFPath.string(), &error))
  {
    if (rendererSparseVSDFMatchesSettings(existing, settings))
      return true;

    os::Logger::warningf(
        "[SparseSDF] cached sparse VSDF was built with stale trace settings, rebuilding: %s",
        sparseVSDFPath.string().c_str());
  }
  else if (fs::exists(sparseVSDFPath))
  {
    os::Logger::warningf("[SparseSDF] cached sparse VSDF is incompatible, rebuilding: %s (%s)", sparseVSDFPath.string().c_str(), error.c_str());
  }
  else
  {
    os::Logger::logf("[SparseSDF] generating sparse per-instance SDF scene: %s", sparseVSDFPath.string().c_str());
  }

  const VirtualGeometrySourceAsset sourceAsset = loadRendererSourceAsset(paths.assetPath, paths.virtualGeometryPath);
  if (!virtualgeometry::SparseSDFBuilder::writeVSDF(sparseVSDFPath.string(), sourceAsset, settings, &error))
  {
    os::Logger::warningf("[SparseSDF] failed to write sparse VSDF scene: %s (%s)", sparseVSDFPath.string().c_str(), error.c_str());
    return false;
  }

  os::Logger::logf("[SparseSDF] wrote sparse per-instance SDF scene: %s", sparseVSDFPath.string().c_str());
  return true;
}

static bool rendererFallbackMeshesMatchProxy(
    const fs::path &virtualGeometryPath,
    const std::vector<FallbackMeshData> &proxyMeshes)
{
  const auto meshesMatchProxy =
      [&proxyMeshes](const std::vector<FallbackMeshData> &existingMeshes)
      {
        if (existingMeshes.size() != proxyMeshes.size())
          return false;

        for (size_t meshIndex = 0u; meshIndex < proxyMeshes.size(); ++meshIndex)
        {
          const FallbackMeshData &expected = proxyMeshes[meshIndex];
          const FallbackMeshData &actual = existingMeshes[meshIndex];
          if (actual.vertices.size() != expected.vertices.size() ||
              actual.indices.size() != expected.indices.size() ||
              actual.sections.size() != expected.sections.size())
          {
            return false;
          }
        }

        return true;
      };
  const auto meshMatchesProxy =
      [&proxyMeshes](const FallbackMeshData &existingMesh, const uint32_t meshIndex)
      {
        if (meshIndex >= static_cast<uint32_t>(proxyMeshes.size()))
          return false;

        const FallbackMeshData &expected = proxyMeshes[meshIndex];
        return existingMesh.vertices.size() == expected.vertices.size() &&
               existingMesh.indices.size() == expected.indices.size() &&
               existingMesh.sections.size() == expected.sections.size();
      };

  {
    const fs::path scenePath = buildRendererSoftwareRaytraceScenePath(virtualGeometryPath);
    std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> existingSceneEntries;
    if (!softwarebvh::SoftwareRayTraceSceneFile::load(scenePath.string(), existingSceneEntries))
    {
      return false;
    }

    std::vector<FallbackMeshData> existingMeshes;
    existingMeshes.reserve(proxyMeshes.size());
    for (softwarebvh::SoftwareRayTraceSceneInstanceData &entry : existingSceneEntries)
    {
      if (entry.lodLevel == 0u)
        existingMeshes.push_back(std::move(entry.fallbackMesh));
    }
    if (!meshesMatchProxy(existingMeshes))
      return false;
  }

  return true;
}

static void writeRendererFallbackMeshes(
    const fs::path &virtualGeometryPath,
    const std::vector<FallbackMeshData> &fallbackLODs)
{
  std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> sceneEntries;
  sceneEntries.reserve(fallbackLODs.size());
  std::unordered_map<uint32_t, uint32_t> shapeIndexByLod;
  for (const FallbackMeshData &fallbackLOD : fallbackLODs)
  {
    const uint32_t shapeIndex = shapeIndexByLod[fallbackLOD.lodLevel]++;
    sceneEntries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(
        fallbackLOD,
        INVALID_INSTANCE_ID,
        INVALID_SCENE_INSTANCE_ID,
        shapeIndex,
        virtualGeometryPath.string()));
  }

  const fs::path scenePath = buildRendererSoftwareRaytraceScenePath(virtualGeometryPath);
  if (!softwarebvh::SoftwareRayTraceSceneFile::save(scenePath.string(), sceneEntries))
    throw std::runtime_error("Failed to write software raytrace scene file: " + scenePath.string());
}

static uint8_t quantizeUnorm8(float value)
{
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static uint32_t packRGBA8(float r, float g, float b, float a)
{
  return static_cast<uint32_t>(quantizeUnorm8(r)) |
         (static_cast<uint32_t>(quantizeUnorm8(g)) << 8u) |
         (static_cast<uint32_t>(quantizeUnorm8(b)) << 16u) |
         (static_cast<uint32_t>(quantizeUnorm8(a)) << 24u);
}

#if 0
static VSDFBounds makeVSDFBounds(const float minValue[3], const float maxValue[3])
{
  VSDFBounds bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = minValue[axis];
    bounds.max[axis] = maxValue[axis];
  }
  return bounds;
}

static void includeVSDFBounds(VSDFBounds &bounds, const VSDFBounds &other)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = std::min(bounds.min[axis], other.min[axis]);
    bounds.max[axis] = std::max(bounds.max[axis], other.max[axis]);
  }
}

static math::Vec3f fallbackVertexPosition(const FallbackMeshVertex &vertex)
{
  return math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]);
}

static float dot3(const math::Vec3f &a, const math::Vec3f &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float lengthSquared3(const math::Vec3f &v)
{
  return dot3(v, v);
}

static float distanceSquaredPointAabb(const math::Vec3f &point, const VSDFBounds &bounds)
{
  float distanceSquared = 0.0f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float lower = bounds.min[axis] - point[axis];
    const float upper = point[axis] - bounds.max[axis];
    const float outside = std::max({lower, upper, 0.0f});
    distanceSquared += outside * outside;
  }
  return distanceSquared;
}

static VSDFBounds triangleBounds(const math::Vec3f &a, const math::Vec3f &b, const math::Vec3f &c)
{
  VSDFBounds bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = std::min({a[axis], b[axis], c[axis]});
    bounds.max[axis] = std::max({a[axis], b[axis], c[axis]});
  }
  return bounds;
}

static VSDFBounds emptyVSDFBounds()
{
  VSDFBounds bounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = std::numeric_limits<float>::max();
    bounds.max[axis] = -std::numeric_limits<float>::max();
  }
  return bounds;
}

static void includeVSDFPoint(VSDFBounds &bounds, const math::Vec3f &point)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
    bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
  }
}

static math::Vec3f centerVSDFBounds(const VSDFBounds &bounds)
{
  return math::Vec3f(
      (bounds.min[0] + bounds.max[0]) * 0.5f,
      (bounds.min[1] + bounds.max[1]) * 0.5f,
      (bounds.min[2] + bounds.max[2]) * 0.5f);
}

static float radiusVSDFBounds(const VSDFBounds &bounds, const math::Vec3f &center)
{
  float radiusSquared = 0.0f;
  for (uint32_t z = 0u; z < 2u; ++z)
  {
    for (uint32_t y = 0u; y < 2u; ++y)
    {
      for (uint32_t x = 0u; x < 2u; ++x)
      {
        const math::Vec3f corner(
            x == 0u ? bounds.min[0] : bounds.max[0],
            y == 0u ? bounds.min[1] : bounds.max[1],
            z == 0u ? bounds.min[2] : bounds.max[2]);
        radiusSquared = std::max(radiusSquared, lengthSquared3(corner - center));
      }
    }
  }
  return std::sqrt(radiusSquared);
}

static float signedSolidAngleTriangle(const math::Vec3f &p, const math::Vec3f &a, const math::Vec3f &b, const math::Vec3f &c)
{
  const math::Vec3f va = a - p;
  const math::Vec3f vb = b - p;
  const math::Vec3f vc = c - p;
  const float la = std::sqrt(std::max(lengthSquared3(va), 1.0e-30f));
  const float lb = std::sqrt(std::max(lengthSquared3(vb), 1.0e-30f));
  const float lc = std::sqrt(std::max(lengthSquared3(vc), 1.0e-30f));
  const float numerator = dot3(va, vb.cross(vc));
  const float denominator =
      la * lb * lc +
      dot3(va, vb) * lc +
      dot3(vb, vc) * la +
      dot3(vc, va) * lb;
  return 2.0f * std::atan2(numerator, denominator);
}

static float distanceSquaredPointTriangle(const math::Vec3f &p, const math::Vec3f &a, const math::Vec3f &b, const math::Vec3f &c)
{
  const math::Vec3f ab = b - a;
  const math::Vec3f ac = c - a;
  const math::Vec3f ap = p - a;
  const float d1 = dot3(ab, ap);
  const float d2 = dot3(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f)
    return lengthSquared3(ap);

  const math::Vec3f bp = p - b;
  const float d3 = dot3(ab, bp);
  const float d4 = dot3(ac, bp);
  if (d3 >= 0.0f && d4 <= d3)
    return lengthSquared3(bp);

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
  {
    const float v = d1 / (d1 - d3);
    return lengthSquared3(p - (a + ab * v));
  }

  const math::Vec3f cp = p - c;
  const float d5 = dot3(ab, cp);
  const float d6 = dot3(ac, cp);
  if (d6 >= 0.0f && d5 <= d6)
    return lengthSquared3(cp);

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
  {
    const float w = d2 / (d2 - d6);
    return lengthSquared3(p - (a + ac * w));
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
  {
    const math::Vec3f bc = c - b;
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return lengthSquared3(p - (b + bc * w));
  }

  const float denom = 1.0f / std::max(va + vb + vc, 1.0e-12f);
  const float v = vb * denom;
  const float w = vc * denom;
  return lengthSquared3(p - (a + ab * v + ac * w));
}

struct RendererMeshSDFTopology
{
  bool closed = false;
  bool watertight = false;
  bool thinSurfaceLike = false;
  uint32_t validTriangleCount = 0u;
  uint32_t boundaryEdgeCount = 0u;
  uint32_t nonManifoldEdgeCount = 0u;
  float signedVolume = 0.0f;
};

static RendererMeshSDFTopology classifyRendererMeshSDFTopology(const FallbackMeshData &mesh)
{
  struct QuantizedPositionKey
  {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const QuantizedPositionKey &other) const
    {
      return x == other.x && y == other.y && z == other.z;
    }
  };

  struct QuantizedPositionHash
  {
    size_t operator()(const QuantizedPositionKey &key) const
    {
      uint64_t h = static_cast<uint32_t>(key.x);
      h = h * 0x9e3779b185ebca87ull + static_cast<uint32_t>(key.y);
      h = h * 0xc2b2ae3d27d4eb4full + static_cast<uint32_t>(key.z);
      return static_cast<size_t>(h ^ (h >> 32u));
    }
  };

  RendererMeshSDFTopology topology;
  const float extentX = std::max(mesh.boundsMax[0] - mesh.boundsMin[0], 1.0e-3f);
  const float extentY = std::max(mesh.boundsMax[1] - mesh.boundsMin[1], 1.0e-3f);
  const float extentZ = std::max(mesh.boundsMax[2] - mesh.boundsMin[2], 1.0e-3f);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  const float quantizationScale = 1048576.0f / maxExtent;

  std::unordered_map<QuantizedPositionKey, uint32_t, QuantizedPositionHash> vertexIdByPosition;
  vertexIdByPosition.reserve(mesh.vertices.size());
  std::vector<uint32_t> canonicalVertexIds(mesh.vertices.size(), 0u);
  for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(mesh.vertices.size()); ++vertexIndex)
  {
    const FallbackMeshVertex &vertex = mesh.vertices[vertexIndex];
    const QuantizedPositionKey key{
        static_cast<int32_t>(std::llround((vertex.position[0] - mesh.boundsMin[0]) * quantizationScale)),
        static_cast<int32_t>(std::llround((vertex.position[1] - mesh.boundsMin[1]) * quantizationScale)),
        static_cast<int32_t>(std::llround((vertex.position[2] - mesh.boundsMin[2]) * quantizationScale))};
    const auto inserted = vertexIdByPosition.emplace(key, static_cast<uint32_t>(vertexIdByPosition.size()));
    canonicalVertexIds[vertexIndex] = inserted.first->second;
  }

  std::unordered_map<uint64_t, uint32_t> edgeCounts;
  edgeCounts.reserve(mesh.indices.size());
  auto addEdge = [&edgeCounts](uint32_t a, uint32_t b)
  {
    if (a > b)
      std::swap(a, b);
    const uint64_t key = (static_cast<uint64_t>(a) << 32u) | static_cast<uint64_t>(b);
    ++edgeCounts[key];
  };

  for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
  {
    const uint32_t i0 = mesh.indices[indexOffset + 0u];
    const uint32_t i1 = mesh.indices[indexOffset + 1u];
    const uint32_t i2 = mesh.indices[indexOffset + 2u];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
      continue;

    const math::Vec3f a = fallbackVertexPosition(mesh.vertices[i0]);
    const math::Vec3f b = fallbackVertexPosition(mesh.vertices[i1]);
    const math::Vec3f c = fallbackVertexPosition(mesh.vertices[i2]);
    if (lengthSquared3((b - a).cross(c - a)) <= 1.0e-16f)
      continue;

    ++topology.validTriangleCount;
    topology.signedVolume += dot3(a, b.cross(c)) / 6.0f;
    const uint32_t c0 = canonicalVertexIds[i0];
    const uint32_t c1 = canonicalVertexIds[i1];
    const uint32_t c2 = canonicalVertexIds[i2];
    if (c0 == c1 || c1 == c2 || c2 == c0)
      continue;
    addEdge(c0, c1);
    addEdge(c1, c2);
    addEdge(c2, c0);
  }

  for (const auto &edge : edgeCounts)
  {
    if (edge.second == 1u)
      ++topology.boundaryEdgeCount;
    else if (edge.second != 2u)
      ++topology.nonManifoldEdgeCount;
  }

  const float boundsVolume = extentX * extentY * extentZ;
  const float minExtent = std::min({extentX, extentY, extentZ});
  topology.watertight =
      topology.validTriangleCount != 0u &&
      topology.boundaryEdgeCount == 0u &&
      topology.nonManifoldEdgeCount == 0u &&
      std::abs(topology.signedVolume) > boundsVolume * 1.0e-5f;
  topology.thinSurfaceLike = topology.watertight && minExtent < maxExtent * 0.025f;
  topology.closed = topology.watertight;
  return topology;
}

static uint32_t chooseRendererMeshSDFBrickResolution(const FallbackMeshData &mesh, const uint32_t meshCount, const RendererMeshSDFTopology &topology)
{
  const uint32_t triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  if (topology.closed && topology.thinSurfaceLike)
    return triangleCount <= 128u ? 128u : 96u;
  if (topology.closed && meshCount <= 2u && triangleCount >= 512u)
    return 96u;
  const float extentX = std::max(mesh.boundsMax[0] - mesh.boundsMin[0], 1.0e-3f);
  const float extentY = std::max(mesh.boundsMax[1] - mesh.boundsMin[1], 1.0e-3f);
  const float extentZ = std::max(mesh.boundsMax[2] - mesh.boundsMin[2], 1.0e-3f);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  const float minExtent = std::min({extentX, extentY, extentZ});
  if (!topology.closed)
  {
    uint32_t resolution = 96u;
    if (minExtent < maxExtent * 0.05f || triangleCount <= 128u)
      resolution = 128u;

    float largestTriangleArea = 0.0f;
    for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
    {
      const uint32_t i0 = mesh.indices[indexOffset + 0u];
      const uint32_t i1 = mesh.indices[indexOffset + 1u];
      const uint32_t i2 = mesh.indices[indexOffset + 2u];
      if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
        continue;

      const math::Vec3f a = fallbackVertexPosition(mesh.vertices[i0]);
      const math::Vec3f b = fallbackVertexPosition(mesh.vertices[i1]);
      const math::Vec3f c = fallbackVertexPosition(mesh.vertices[i2]);
      largestTriangleArea = std::max(largestTriangleArea, std::sqrt(lengthSquared3((b - a).cross(c - a))) * 0.5f);
    }

    const float largestTriangleLinearExtent = std::sqrt(std::max(largestTriangleArea, 0.0f));
    const bool lowPolySurface = triangleCount <= 128u;
    const bool veryLowPolySurface = triangleCount <= 64u;
    const bool thinOpenSurface = minExtent < maxExtent * 0.025f;
    const bool largeTriangleDominated = largestTriangleLinearExtent > maxExtent * 0.20f;
    if ((lowPolySurface && largeTriangleDominated) || (thinOpenSurface && lowPolySurface))
      resolution = std::max(resolution, 192u);
    if ((veryLowPolySurface && largeTriangleDominated) || (thinOpenSurface && veryLowPolySurface))
      resolution = std::max(resolution, 256u);

    return resolution;
  }
  return 32u;
}

static float rendererMeshLargestTriangleLinearExtent(const FallbackMeshData &mesh)
{
  float largestTriangleArea = 0.0f;
  for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
  {
    const uint32_t i0 = mesh.indices[indexOffset + 0u];
    const uint32_t i1 = mesh.indices[indexOffset + 1u];
    const uint32_t i2 = mesh.indices[indexOffset + 2u];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
      continue;

    const math::Vec3f a = fallbackVertexPosition(mesh.vertices[i0]);
    const math::Vec3f b = fallbackVertexPosition(mesh.vertices[i1]);
    const math::Vec3f c = fallbackVertexPosition(mesh.vertices[i2]);
    largestTriangleArea = std::max(largestTriangleArea, std::sqrt(lengthSquared3((b - a).cross(c - a))) * 0.5f);
  }
  return std::sqrt(std::max(largestTriangleArea, 0.0f));
}

static std::array<uint32_t, 3u> rendererMeshAxisFeatureCounts(const FallbackMeshData &mesh)
{
  std::array<std::vector<int32_t>, 3u> coordinates;
  const float extentX = std::max(mesh.boundsMax[0] - mesh.boundsMin[0], 1.0e-3f);
  const float extentY = std::max(mesh.boundsMax[1] - mesh.boundsMin[1], 1.0e-3f);
  const float extentZ = std::max(mesh.boundsMax[2] - mesh.boundsMin[2], 1.0e-3f);
  const float extents[3] = {extentX, extentY, extentZ};
  const float maxExtent = std::max({extentX, extentY, extentZ});
  for (const FallbackMeshVertex &vertex : mesh.vertices)
  {
    const math::Vec3f p = fallbackVertexPosition(vertex);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float quantizationStep = std::max({extents[axis] / 2048.0f, maxExtent * 1.0e-5f, 1.0e-4f});
      coordinates[axis].push_back(static_cast<int32_t>(std::floor((p[axis] - mesh.boundsMin[axis]) / quantizationStep + 0.5f)));
    }
  }

  std::array<uint32_t, 3u> result{1u, 1u, 1u};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    std::vector<int32_t> &axisCoordinates = coordinates[axis];
    std::sort(axisCoordinates.begin(), axisCoordinates.end());
    axisCoordinates.erase(std::unique(axisCoordinates.begin(), axisCoordinates.end()), axisCoordinates.end());
    result[axis] = static_cast<uint32_t>(axisCoordinates.size());
  }
  return result;
}

static float rendererSDFBoundsMaxExtent(const VSDFBounds &bounds)
{
  return std::max({
      bounds.max[0] - bounds.min[0],
      bounds.max[1] - bounds.min[1],
      bounds.max[2] - bounds.min[2],
      1.0e-3f});
}

static float rendererMeshSDFTargetCellSize(const float meshMaxExtent)
{
  return std::max(meshMaxExtent / std::max(gRendererVSDFBuildSettings.targetMajorAxisCells, 1.0f), std::max(gRendererVSDFBuildSettings.minTargetCellSize, 1.0e-6f));
}

static uint32_t rendererResolutionForWorldError(
    const float extent,
    const float targetCellSize,
    const uint32_t maxAxisResolution)
{
  return std::clamp(static_cast<uint32_t>(std::ceil(extent / std::max(targetCellSize, 1.0e-6f))), 8u, maxAxisResolution);
}

struct RendererSDFGridResolution
{
  uint32_t x = 8u;
  uint32_t y = 8u;
  uint32_t z = 8u;
};

static RendererSDFGridResolution chooseRendererMeshSDFGridResolution(
    const FallbackMeshData &mesh,
    const uint32_t meshCount,
    const RendererMeshSDFTopology &topology)
{
  const uint32_t legacyMajorResolution = chooseRendererMeshSDFBrickResolution(mesh, meshCount, topology);
  const float extentX = std::max(mesh.boundsMax[0] - mesh.boundsMin[0], 1.0e-3f);
  const float extentY = std::max(mesh.boundsMax[1] - mesh.boundsMin[1], 1.0e-3f);
  const float extentZ = std::max(mesh.boundsMax[2] - mesh.boundsMin[2], 1.0e-3f);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  const uint32_t triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  const float minExtent = std::min({extentX, extentY, extentZ});
  const bool sheetLikeOpenSurface = !topology.closed && minExtent < maxExtent * 0.035f;
  const bool thinClosedSurface = topology.closed && topology.thinSurfaceLike;
  const bool surfaceLikeThinGrid = sheetLikeOpenSurface || thinClosedSurface;
  const float largestTriangleLinearExtent = rendererMeshLargestTriangleLinearExtent(mesh);
  const bool largeTriangleDominated = largestTriangleLinearExtent > maxExtent * 0.15f;
  const bool detailSensitiveOpenSurface = !topology.closed && (sheetLikeOpenSurface || largeTriangleDominated || triangleCount <= 256u);
  const float targetCellSize = rendererMeshSDFTargetCellSize(maxExtent);
  const uint32_t maxAxisResolution = topology.closed
                                         ? (thinClosedSurface ? gRendererVSDFBuildSettings.closedMaxAxisResolution : std::max(legacyMajorResolution, 96u))
                                         : (detailSensitiveOpenSurface ? gRendererVSDFBuildSettings.detailOpenMaxAxisResolution : gRendererVSDFBuildSettings.openMaxAxisResolution);
  RendererSDFGridResolution result{
      rendererResolutionForWorldError(extentX, targetCellSize, maxAxisResolution),
      rendererResolutionForWorldError(extentY, targetCellSize, maxAxisResolution),
      rendererResolutionForWorldError(extentZ, targetCellSize, maxAxisResolution)};
  const std::array<uint32_t, 3u> axisFeatureCounts = rendererMeshAxisFeatureCounts(mesh);
  uint32_t *resultAxes[3] = {&result.x, &result.y, &result.z};
  const float extents[3] = {extentX, extentY, extentZ};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const bool hasAxisFeatures = axisFeatureCounts[axis] >= 4u || extents[axis] > maxExtent * 0.035f;
    if (!hasAxisFeatures)
      continue;
    const uint32_t errorResolution = rendererResolutionForWorldError(extents[axis], targetCellSize, maxAxisResolution);
    const uint32_t featureResolution = std::clamp(axisFeatureCounts[axis] * (detailSensitiveOpenSurface ? 3u : 2u), 8u, maxAxisResolution);
    const uint32_t featureCap = std::clamp(static_cast<uint32_t>(std::ceil(static_cast<float>(errorResolution) * std::max(gRendererVSDFBuildSettings.featureAxisResolutionCapScale, 1.0f))), 8u, maxAxisResolution);
    *resultAxes[axis] = std::max(*resultAxes[axis], std::min(featureResolution, featureCap));
  }
  if (surfaceLikeThinGrid)
  {
    uint32_t thinAxis = 0u;
    if (extents[1] < extents[thinAxis])
      thinAxis = 1u;
    if (extents[2] < extents[thinAxis])
      thinAxis = 2u;
    *resultAxes[thinAxis] = std::max(*resultAxes[thinAxis], topology.closed ? 16u : 12u);
  }

  uint32_t maxMeshVoxels = (topology.closed ? std::max(gRendererVSDFBuildSettings.closedVoxelBudgetKi, 1u) : std::max(gRendererVSDFBuildSettings.openVoxelBudgetKi, 1u)) * 1024u;
  if (detailSensitiveOpenSurface)
    maxMeshVoxels = std::max(maxMeshVoxels, std::max(gRendererVSDFBuildSettings.detailOpenVoxelBudgetKi, 1u) * 1024u);
  if (surfaceLikeThinGrid)
    maxMeshVoxels = std::max(maxMeshVoxels, std::max(gRendererVSDFBuildSettings.thinSurfaceVoxelBudgetKi, 1u) * 1024u);
  const uint64_t voxelCount = static_cast<uint64_t>(result.x) * result.y * result.z;
  if (voxelCount > maxMeshVoxels)
  {
    if (surfaceLikeThinGrid)
    {
      uint32_t *axes[3] = {&result.x, &result.y, &result.z};
      uint32_t thinAxis = 0u;
      if (extents[1] < extents[thinAxis])
        thinAxis = 1u;
      if (extents[2] < extents[thinAxis])
        thinAxis = 2u;

      const bool paperThinAxis = extents[thinAxis] < std::max(targetCellSize * 2.5f, maxExtent * 0.004f) && axisFeatureCounts[thinAxis] <= 3u;
      if (paperThinAxis)
        *axes[thinAxis] = std::clamp(*axes[thinAxis], topology.closed ? 16u : 12u, topology.closed ? 32u : 16u);
      else
        *axes[thinAxis] = std::max(*axes[thinAxis], std::clamp(axisFeatureCounts[thinAxis] * 3u, 16u, std::min<uint32_t>(maxAxisResolution, 128u)));
      const uint64_t thinResolution = std::max<uint32_t>(*axes[thinAxis], 1u);
      const uint64_t planarBudget = std::max<uint64_t>(maxMeshVoxels / thinResolution, 64u);
      uint32_t tangentAxis0 = thinAxis == 0u ? 1u : 0u;
      uint32_t tangentAxis1 = thinAxis == 2u ? 1u : 2u;
      if (tangentAxis0 == thinAxis)
        tangentAxis0 = 2u;
      if (tangentAxis1 == thinAxis || tangentAxis1 == tangentAxis0)
        tangentAxis1 = 3u - thinAxis - tangentAxis0;

      const uint64_t planarVoxelCount = static_cast<uint64_t>(*axes[tangentAxis0]) * *axes[tangentAxis1];
      if (planarVoxelCount > planarBudget)
      {
        const float scale = std::sqrt(static_cast<float>(planarVoxelCount) / static_cast<float>(planarBudget));
        auto scaleAxis = [scale](const uint32_t value) -> uint32_t
        {
          return std::max(8u, static_cast<uint32_t>(std::floor(static_cast<float>(value) / scale)));
        };
        *axes[tangentAxis0] = scaleAxis(*axes[tangentAxis0]);
        *axes[tangentAxis1] = scaleAxis(*axes[tangentAxis1]);
      }
    }
    else
    {
      const float scale = std::cbrt(static_cast<float>(voxelCount) / static_cast<float>(maxMeshVoxels));
      auto scaleAxis = [scale](const uint32_t value) -> uint32_t
      {
        return std::max(8u, static_cast<uint32_t>(std::floor(static_cast<float>(value) / scale)));
      };
      result.x = scaleAxis(result.x);
      result.y = scaleAxis(result.y);
      result.z = scaleAxis(result.z);
    }
  }
  return result;
}

static VSDFBounds expandRendererMeshSDFBounds(const VSDFBounds &bounds, const RendererSDFGridResolution &gridResolution, const bool closed)
{
  VSDFBounds expanded = bounds;
  const float extentX = std::max(bounds.max[0] - bounds.min[0], 1.0e-3f);
  const float extentY = std::max(bounds.max[1] - bounds.min[1], 1.0e-3f);
  const float extentZ = std::max(bounds.max[2] - bounds.min[2], 1.0e-3f);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  const uint32_t majorResolution = std::max({gridResolution.x, gridResolution.y, gridResolution.z, 1u});
  const float majorCell = maxExtent / static_cast<float>(majorResolution);
  const float minThinExtent = majorCell * 3.0f;
  const float extents[3] = {extentX, extentY, extentZ};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float samplingPad = majorCell * (closed ? std::max(gRendererVSDFBuildSettings.closedBoundsPadCells, 0.0f) : std::max(gRendererVSDFBuildSettings.openBoundsPadCells, 0.0f));
    const float basePad = std::max({extents[axis] * 0.001f, maxExtent * 1.0e-5f, samplingPad});
    const float thinAxisPad = !closed && extents[axis] < minThinExtent ? (minThinExtent - extents[axis]) * 0.5f + majorCell * 0.5f : 0.0f;
    const float pad = std::max(basePad, thinAxisPad);
    expanded.min[axis] -= pad;
    expanded.max[axis] += pad;
  }
  return expanded;
}

static RendererSDFGridResolution refineRendererOpenMeshSDFGridResolutionForBounds(
    const VSDFBounds &sourceBounds,
    const VSDFBounds &expandedBounds,
    RendererSDFGridResolution gridResolution,
    const bool closed)
{
  if (closed)
    return gridResolution;

  const float sourceExtentX = std::max(sourceBounds.max[0] - sourceBounds.min[0], 1.0e-3f);
  const float sourceExtentY = std::max(sourceBounds.max[1] - sourceBounds.min[1], 1.0e-3f);
  const float sourceExtentZ = std::max(sourceBounds.max[2] - sourceBounds.min[2], 1.0e-3f);
  const float maxSourceExtent = std::max({sourceExtentX, sourceExtentY, sourceExtentZ});
  const uint32_t majorResolution = std::max({gridResolution.x, gridResolution.y, gridResolution.z, 1u});
  const float targetCellSize = std::max(maxSourceExtent / static_cast<float>(majorResolution), 1.0e-6f);
  const float sourceExtents[3] = {sourceExtentX, sourceExtentY, sourceExtentZ};
  const float expandedExtents[3] = {
      std::max(expandedBounds.max[0] - expandedBounds.min[0], 1.0e-3f),
      std::max(expandedBounds.max[1] - expandedBounds.min[1], 1.0e-3f),
      std::max(expandedBounds.max[2] - expandedBounds.min[2], 1.0e-3f)};
  uint32_t *axes[3] = {&gridResolution.x, &gridResolution.y, &gridResolution.z};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const bool thinSourceAxis = sourceExtents[axis] < maxSourceExtent * 0.05f;
    const bool heavilyPaddedAxis = expandedExtents[axis] > sourceExtents[axis] * 1.75f;
    uint32_t requiredResolution = static_cast<uint32_t>(std::ceil(expandedExtents[axis] / targetCellSize));
    if (thinSourceAxis || heavilyPaddedAxis)
      requiredResolution = std::max(requiredResolution, 16u);
    *axes[axis] = std::clamp(std::max(*axes[axis], requiredResolution), 8u, 512u);
  }
  return gridResolution;
}

static float chooseRendererMeshSDFNarrowBand(
    const VSDFBounds &bounds,
    const RendererSDFGridResolution &gridResolution,
    const RendererMeshSDFTopology &topology)
{
  const float extentX = std::max(bounds.max[0] - bounds.min[0], 1.0e-3f);
  const float extentY = std::max(bounds.max[1] - bounds.min[1], 1.0e-3f);
  const float extentZ = std::max(bounds.max[2] - bounds.min[2], 1.0e-3f);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  const float legacyNarrowBand = maxExtent * 0.25f;
  if (topology.closed && !topology.thinSurfaceLike)
    return std::max(legacyNarrowBand, 1.0e-3f);

  const float cellX = extentX / static_cast<float>(std::max(gridResolution.x, 1u));
  const float cellY = extentY / static_cast<float>(std::max(gridResolution.y, 1u));
  const float cellZ = extentZ / static_cast<float>(std::max(gridResolution.z, 1u));
  const float maxCell = std::max({cellX, cellY, cellZ, 1.0e-3f});
  const float cellBand = maxCell * (topology.closed ? std::max(gRendererVSDFBuildSettings.closedThinNarrowBandCells, 1.0f) : std::max(gRendererVSDFBuildSettings.openNarrowBandCells, 1.0f));
  return std::clamp(cellBand, 1.0e-3f, legacyNarrowBand);
}

static std::vector<uint8_t> buildRendererMeshSDFBrick(
    const FallbackMeshData &mesh,
    const VSDFBounds &bounds,
    RendererSDFGridResolution gridResolution,
    float narrowBandDistance,
    bool closedTopology,
    bool encodeSignedDistance,
    uint32_t meshIndex,
    uint32_t meshCount)
{
  gridResolution.x = std::max(8u, gridResolution.x);
  gridResolution.y = std::max(8u, gridResolution.y);
  gridResolution.z = std::max(8u, gridResolution.z);
  std::vector<uint8_t> data(static_cast<size_t>(gridResolution.x) * gridResolution.y * gridResolution.z, 255u);
  const float extentX = std::max(bounds.max[0] - bounds.min[0], 1.0e-3f);
  const float extentY = std::max(bounds.max[1] - bounds.min[1], 1.0e-3f);
  const float extentZ = std::max(bounds.max[2] - bounds.min[2], 1.0e-3f);
  const float narrowBand = std::max(narrowBandDistance, 1.0e-3f);
  const float cellX = extentX / static_cast<float>(std::max(gridResolution.x, 1u));
  const float cellY = extentY / static_cast<float>(std::max(gridResolution.y, 1u));
  const float cellZ = extentZ / static_cast<float>(std::max(gridResolution.z, 1u));
  const float medianCell = std::max(std::min(cellX, cellY), std::min(std::max(cellX, cellY), cellZ));
  const float unsignedConservativeBias = encodeSignedDistance ? 0.0f : medianCell * std::max(gRendererVSDFBuildSettings.unsignedConservativeBiasCells, 0.0f);
  const float unsignedSearchBand = narrowBand + unsignedConservativeBias + medianCell * std::max(gRendererVSDFBuildSettings.unsignedGuardBandCells, 0.0f);

  struct BakeTriangle
  {
    math::Vec3f a;
    math::Vec3f b;
    math::Vec3f c;
    VSDFBounds bounds;
    math::Vec3f center;
    math::Vec3f normal;
    float area = 0.0f;
  };

  struct BakeBVHNode
  {
    VSDFBounds bounds;
    uint32_t firstTriangle = 0u;
    uint32_t triangleCount = 0u;
    uint32_t leftChild = UINT32_MAX;
    uint32_t rightChild = UINT32_MAX;
    math::Vec3f windingCenter;
    math::Vec3f windingAreaNormal;
    float windingArea = 0.0f;
    float windingRadius = 0.0f;
  };

  std::vector<BakeTriangle> triangles;
  triangles.reserve(mesh.indices.size() / 3u);
  float signedVolume = 0.0f;
  for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
  {
    const uint32_t i0 = mesh.indices[indexOffset + 0u];
    const uint32_t i1 = mesh.indices[indexOffset + 1u];
    const uint32_t i2 = mesh.indices[indexOffset + 2u];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
      continue;

    BakeTriangle triangle;
    triangle.a = fallbackVertexPosition(mesh.vertices[i0]);
    triangle.b = fallbackVertexPosition(mesh.vertices[i1]);
    triangle.c = fallbackVertexPosition(mesh.vertices[i2]);
    triangle.bounds = triangleBounds(triangle.a, triangle.b, triangle.c);
    triangle.center = centerVSDFBounds(triangle.bounds);
    const math::Vec3f geometricNormal = (triangle.b - triangle.a).cross(triangle.c - triangle.a);
    const float normalLengthSquared = lengthSquared3(geometricNormal);
    if (normalLengthSquared <= 1.0e-16f)
      continue;
    const float normalLength = std::sqrt(normalLengthSquared);
    triangle.normal = geometricNormal * (1.0f / normalLength);
    triangle.area = normalLength * 0.5f;
    signedVolume += dot3(triangle.a, triangle.b.cross(triangle.c)) / 6.0f;
    triangles.push_back(triangle);
  }
  if (closedTopology && signedVolume < 0.0f)
  {
    for (BakeTriangle &triangle : triangles)
      triangle.normal = triangle.normal * -1.0f;
  }

  os::Logger::logf(
      "[VSDF] emit mesh %u/%u: vertices=%zu indices=%zu triangles=%zu brick=%ux%ux%u encoding=%s signedVolume=%.3f narrowBand=%.3f bounds=(%.3f %.3f %.3f)-(%.3f %.3f %.3f)",
      meshIndex + 1u,
      meshCount,
      mesh.vertices.size(),
      mesh.indices.size(),
      triangles.size(),
      gridResolution.x,
      gridResolution.y,
      gridResolution.z,
      encodeSignedDistance ? "signed" : "unsigned",
      signedVolume,
      narrowBand,
      bounds.min[0],
      bounds.min[1],
      bounds.min[2],
      bounds.max[0],
      bounds.max[1],
      bounds.max[2]);

  if (triangles.empty())
  {
    os::Logger::warningf("[VSDF] emit mesh %u/%u: no valid triangles, writing empty SDF brick", meshIndex + 1u, meshCount);
    return data;
  }

  std::vector<uint32_t> triangleIndices(triangles.size());
  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(triangleIndices.size()); ++triangleIndex)
    triangleIndices[triangleIndex] = triangleIndex;

  std::vector<BakeBVHNode> bvhNodes;
  bvhNodes.reserve(triangles.size() * 2u);
  std::function<uint32_t(uint32_t, uint32_t)> buildBVH =
      [&](const uint32_t firstTriangle, const uint32_t triangleCount) -> uint32_t
  {
    BakeBVHNode node;
    node.bounds = emptyVSDFBounds();
    VSDFBounds centroidBounds = emptyVSDFBounds();
    for (uint32_t offset = 0u; offset < triangleCount; ++offset)
    {
      const BakeTriangle &triangle = triangles[triangleIndices[firstTriangle + offset]];
      includeVSDFBounds(node.bounds, triangle.bounds);
      includeVSDFPoint(centroidBounds, triangle.center);
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(bvhNodes.size());
    bvhNodes.push_back(node);
    static constexpr uint32_t kLeafTriangleCount = 8u;
    if (triangleCount <= kLeafTriangleCount)
    {
      BakeBVHNode &leaf = bvhNodes[nodeIndex];
      leaf.firstTriangle = firstTriangle;
      leaf.triangleCount = triangleCount;
      math::Vec3f weightedCenter(0.0f, 0.0f, 0.0f);
      math::Vec3f areaNormal(0.0f, 0.0f, 0.0f);
      float area = 0.0f;
      for (uint32_t offset = 0u; offset < triangleCount; ++offset)
      {
        const BakeTriangle &triangle = triangles[triangleIndices[firstTriangle + offset]];
        weightedCenter = weightedCenter + triangle.center * triangle.area;
        areaNormal = areaNormal + triangle.normal * triangle.area;
        area += triangle.area;
      }
      leaf.windingArea = area;
      leaf.windingCenter = area > 1.0e-12f ? weightedCenter * (1.0f / area) : centerVSDFBounds(leaf.bounds);
      leaf.windingAreaNormal = areaNormal;
      leaf.windingRadius = radiusVSDFBounds(leaf.bounds, leaf.windingCenter);
      return nodeIndex;
    }

    const math::Vec3f centroidExtent(
        centroidBounds.max[0] - centroidBounds.min[0],
        centroidBounds.max[1] - centroidBounds.min[1],
        centroidBounds.max[2] - centroidBounds.min[2]);
    uint32_t splitAxis = 0u;
    if (centroidExtent[1] > centroidExtent[splitAxis])
      splitAxis = 1u;
    if (centroidExtent[2] > centroidExtent[splitAxis])
      splitAxis = 2u;

    const uint32_t leftCount = triangleCount / 2u;
    const uint32_t middle = firstTriangle + leftCount;
    std::nth_element(
        triangleIndices.begin() + firstTriangle,
        triangleIndices.begin() + middle,
        triangleIndices.begin() + firstTriangle + triangleCount,
        [&](const uint32_t lhs, const uint32_t rhs)
        {
          return triangles[lhs].center[splitAxis] < triangles[rhs].center[splitAxis];
        });

    bvhNodes[nodeIndex].leftChild = buildBVH(firstTriangle, leftCount);
    bvhNodes[nodeIndex].rightChild = buildBVH(middle, triangleCount - leftCount);
    BakeBVHNode &inner = bvhNodes[nodeIndex];
    const BakeBVHNode &left = bvhNodes[inner.leftChild];
    const BakeBVHNode &right = bvhNodes[inner.rightChild];
    inner.windingArea = left.windingArea + right.windingArea;
    if (inner.windingArea > 1.0e-12f)
    {
      inner.windingCenter = (left.windingCenter * left.windingArea + right.windingCenter * right.windingArea) * (1.0f / inner.windingArea);
    }
    else
    {
      inner.windingCenter = centerVSDFBounds(inner.bounds);
    }
    inner.windingAreaNormal = left.windingAreaNormal + right.windingAreaNormal;
    inner.windingRadius = radiusVSDFBounds(inner.bounds, inner.windingCenter);
    return nodeIndex;
  };

  const uint32_t bvhRoot = buildBVH(0u, static_cast<uint32_t>(triangles.size()));

  struct NearestTriangleResult
  {
    float distanceSquared = 0.0f;
    float planeSide = 1.0f;
    bool found = false;
  };

  auto nearestTriangleDistance = [&](const math::Vec3f &p, const float initialMinDistanceSquared) -> NearestTriangleResult
  {
    NearestTriangleResult result;
    result.distanceSquared = initialMinDistanceSquared;
    result.planeSide = 1.0f;
    result.found = false;
    std::array<uint32_t, 2048u> stack{};
    uint32_t stackCount = 0u;
    stack[stackCount++] = bvhRoot;
    while (stackCount != 0u)
    {
      const BakeBVHNode &node = bvhNodes[stack[--stackCount]];
      if (distanceSquaredPointAabb(p, node.bounds) > result.distanceSquared)
        continue;

      if (node.triangleCount != 0u)
      {
        for (uint32_t offset = 0u; offset < node.triangleCount; ++offset)
        {
          const BakeTriangle &triangle = triangles[triangleIndices[node.firstTriangle + offset]];
          if (distanceSquaredPointAabb(p, triangle.bounds) > result.distanceSquared)
            continue;
          const float distanceSquared = distanceSquaredPointTriangle(p, triangle.a, triangle.b, triangle.c);
          if (distanceSquared < result.distanceSquared)
          {
            result.distanceSquared = distanceSquared;
            result.planeSide = dot3(p - triangle.a, triangle.normal) < 0.0f ? -1.0f : 1.0f;
            result.found = true;
          }
        }
        continue;
      }

      const uint32_t leftChild = node.leftChild;
      const uint32_t rightChild = node.rightChild;
      const float leftDistanceSquared = leftChild != UINT32_MAX ? distanceSquaredPointAabb(p, bvhNodes[leftChild].bounds) : std::numeric_limits<float>::max();
      const float rightDistanceSquared = rightChild != UINT32_MAX ? distanceSquaredPointAabb(p, bvhNodes[rightChild].bounds) : std::numeric_limits<float>::max();
      if (leftDistanceSquared < rightDistanceSquared)
      {
        if (rightDistanceSquared <= result.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = rightChild;
        if (leftDistanceSquared <= result.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = leftChild;
      }
      else
      {
        if (leftDistanceSquared <= result.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = leftChild;
        if (rightDistanceSquared <= result.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = rightChild;
      }
    }
    return result;
  };

  const float windingOrientation = closedTopology && signedVolume < 0.0f ? -1.0f : 1.0f;
  auto fastWindingNumber = [&](const math::Vec3f &p) -> float
  {
    static constexpr float kInvFourPi = 0.07957747154594767f;
    static constexpr float kOpeningAngle = 0.5f;
    float solidAngle = 0.0f;
    std::array<uint32_t, 2048u> stack{};
    uint32_t stackCount = 0u;
    stack[stackCount++] = bvhRoot;
    while (stackCount != 0u)
    {
      const BakeBVHNode &node = bvhNodes[stack[--stackCount]];
      const math::Vec3f toCluster = node.windingCenter - p;
      const float distanceSquared = std::max(lengthSquared3(toCluster), 1.0e-20f);
      const bool leaf = node.triangleCount != 0u;
      const bool useApproximation = !leaf && node.windingRadius * node.windingRadius <= kOpeningAngle * kOpeningAngle * distanceSquared;
      if (useApproximation)
      {
        solidAngle += dot3(node.windingAreaNormal, toCluster) / (distanceSquared * std::sqrt(distanceSquared));
        continue;
      }

      if (leaf)
      {
        for (uint32_t offset = 0u; offset < node.triangleCount; ++offset)
        {
          const BakeTriangle &triangle = triangles[triangleIndices[node.firstTriangle + offset]];
          solidAngle += windingOrientation * signedSolidAngleTriangle(p, triangle.a, triangle.b, triangle.c);
        }
        continue;
      }

      if (node.leftChild != UINT32_MAX && stackCount < stack.size())
        stack[stackCount++] = node.leftChild;
      if (node.rightChild != UINT32_MAX && stackCount < stack.size())
        stack[stackCount++] = node.rightChild;
    }
    return solidAngle * kInvFourPi;
  };

  auto closedMeshSign = [&](const math::Vec3f &p, const float fallbackPlaneSide) -> float
  {
    const float windingNumber = fastWindingNumber(p);
    const float windingMagnitude = std::abs(windingNumber);
    if (windingMagnitude <= 0.35f)
      return 1.0f;
    if (windingMagnitude >= 0.5f)
      return -1.0f;
    return fallbackPlaneSide;
  };

  const auto bakeStart = lib::time::TimeSpan::now();
  os::Logger::logf(
      "[VSDF] emit mesh %u/%u: triangle bvh nodes=%zu leaves<=8",
      meshIndex + 1u,
      meshCount,
      bvhNodes.size());

  const uint32_t rowCount = gridResolution.y * gridResolution.z;
  const uint32_t hardwareWorkerCount = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t rowWorkerCount = meshCount <= 2u ? std::max(1u, std::min({hardwareWorkerCount, rowCount, 12u})) : 1u;
  std::atomic<uint32_t> nextRow{0u};
  std::atomic<uint32_t> completedRows{0u};
  auto bakeRows = [&]()
  {
    for (;;)
    {
      const uint32_t row = nextRow.fetch_add(1u);
      if (row >= rowCount)
        break;

      const uint32_t z = row / gridResolution.y;
      const uint32_t y = row - z * gridResolution.y;
      for (uint32_t x = 0u; x < gridResolution.x; ++x)
      {
        const float px = bounds.min[0] + (static_cast<float>(x) + 0.5f) / static_cast<float>(gridResolution.x) * extentX;
        const float py = bounds.min[1] + (static_cast<float>(y) + 0.5f) / static_cast<float>(gridResolution.y) * extentY;
        const float pz = bounds.min[2] + (static_cast<float>(z) + 0.5f) / static_cast<float>(gridResolution.z) * extentZ;
        const math::Vec3f p(px, py, pz);
        const NearestTriangleResult nearest = nearestTriangleDistance(
            p,
            encodeSignedDistance ? std::numeric_limits<float>::max() : unsignedSearchBand * unsignedSearchBand);
        const float distance = std::sqrt(nearest.distanceSquared);
        const float sign = encodeSignedDistance ? closedMeshSign(p, nearest.planeSide) : 1.0f;
        const float storedDistance = encodeSignedDistance ? distance : std::max(distance - unsignedConservativeBias, 0.0f);
        const float encoded = encodeSignedDistance
                                  ? std::clamp(0.5f + storedDistance * sign / std::max(narrowBand, 1.0e-3f) * 0.5f, 0.0f, 1.0f)
                                  : std::clamp(storedDistance / std::max(narrowBand, 1.0e-3f), 0.0f, 1.0f);
        const uint8_t encodedByte = encodeSignedDistance
                                        ? quantizeUnorm8(encoded)
                                        : (nearest.found ? std::min<uint8_t>(quantizeUnorm8(encoded), 254u) : 255u);
        data[(static_cast<size_t>(z) * gridResolution.y + y) * gridResolution.x + x] = encodedByte;
      }

      const uint32_t completed = completedRows.fetch_add(1u) + 1u;
      if ((completed % 512u) == 0u || completed == rowCount)
      {
        os::Logger::logf(
            "[VSDF] emit mesh %u/%u: rows %u/%u elapsed=%.1f ms",
            meshIndex + 1u,
            meshCount,
            completed,
            rowCount,
            (lib::time::TimeSpan::now() - bakeStart).milliseconds());
      }
    }
  };

  if (rowWorkerCount == 1u)
  {
    bakeRows();
  }
  else
  {
    std::vector<std::thread> rowWorkers;
    rowWorkers.reserve(rowWorkerCount);
    for (uint32_t workerIndex = 0u; workerIndex < rowWorkerCount; ++workerIndex)
      rowWorkers.emplace_back(bakeRows);
    for (std::thread &worker : rowWorkers)
      worker.join();
  }

  os::Logger::logf(
      "[VSDF] emit mesh %u/%u: finished SDF brick in %.1f ms",
      meshIndex + 1u,
      meshCount,
      (lib::time::TimeSpan::now() - bakeStart).milliseconds());
  return data;
}

static std::vector<uint8_t> packRendererMeshSDFSparseBrick8(
    const std::vector<uint8_t> &denseData,
    const RendererSDFGridResolution &gridResolution,
    uint32_t &outResidentBrickCount)
{
  static constexpr uint32_t kSparseBrickDim = 8u;
  static constexpr uint32_t kSparseBrickCellCount = kSparseBrickDim * kSparseBrickDim * kSparseBrickDim;
  const uint32_t brickGridX = (gridResolution.x + kSparseBrickDim - 1u) / kSparseBrickDim;
  const uint32_t brickGridY = (gridResolution.y + kSparseBrickDim - 1u) / kSparseBrickDim;
  const uint32_t brickGridZ = (gridResolution.z + kSparseBrickDim - 1u) / kSparseBrickDim;
  const uint64_t brickGridCount64 = static_cast<uint64_t>(brickGridX) * brickGridY * brickGridZ;
  if (brickGridCount64 == 0u || brickGridCount64 > std::numeric_limits<uint32_t>::max())
  {
    outResidentBrickCount = 0u;
    return {};
  }

  const uint32_t brickGridCount = static_cast<uint32_t>(brickGridCount64);
  std::vector<uint32_t> brickTable(brickGridCount, UINT32_MAX);
  std::vector<uint8_t> residentBrickData;
  residentBrickData.reserve(denseData.size());

  auto denseIndex = [&](uint32_t x, uint32_t y, uint32_t z) -> size_t
  {
    return (static_cast<size_t>(z) * gridResolution.y + y) * gridResolution.x + x;
  };

  auto brickTableIndex = [&](uint32_t bx, uint32_t by, uint32_t bz) -> uint32_t
  {
    return (bz * brickGridY + by) * brickGridX + bx;
  };

  for (uint32_t bz = 0u; bz < brickGridZ; ++bz)
  {
    for (uint32_t by = 0u; by < brickGridY; ++by)
    {
      for (uint32_t bx = 0u; bx < brickGridX; ++bx)
      {
        bool keepBrick = false;
        for (uint32_t lz = 0u; lz < kSparseBrickDim && !keepBrick; ++lz)
        {
          const uint32_t z = bz * kSparseBrickDim + lz;
          if (z >= gridResolution.z)
            continue;
          for (uint32_t ly = 0u; ly < kSparseBrickDim && !keepBrick; ++ly)
          {
            const uint32_t y = by * kSparseBrickDim + ly;
            if (y >= gridResolution.y)
              continue;
            for (uint32_t lx = 0u; lx < kSparseBrickDim; ++lx)
            {
              const uint32_t x = bx * kSparseBrickDim + lx;
              if (x >= gridResolution.x)
                continue;
              if (denseData[denseIndex(x, y, z)] != 255u)
              {
                keepBrick = true;
                break;
              }
            }
          }
        }
        if (!keepBrick)
          continue;

        const uint32_t residentBrickIndex = static_cast<uint32_t>(residentBrickData.size() / kSparseBrickCellCount);
        brickTable[brickTableIndex(bx, by, bz)] = residentBrickIndex;
        const size_t brickDataOffset = residentBrickData.size();
        residentBrickData.resize(residentBrickData.size() + kSparseBrickCellCount, 255u);
        for (uint32_t lz = 0u; lz < kSparseBrickDim; ++lz)
        {
          const uint32_t z = bz * kSparseBrickDim + lz;
          if (z >= gridResolution.z)
            continue;
          for (uint32_t ly = 0u; ly < kSparseBrickDim; ++ly)
          {
            const uint32_t y = by * kSparseBrickDim + ly;
            if (y >= gridResolution.y)
              continue;
            for (uint32_t lx = 0u; lx < kSparseBrickDim; ++lx)
            {
              const uint32_t x = bx * kSparseBrickDim + lx;
              if (x >= gridResolution.x)
                continue;
              const uint32_t localIndex = (lz * kSparseBrickDim + ly) * kSparseBrickDim + lx;
              residentBrickData[brickDataOffset + localIndex] = denseData[denseIndex(x, y, z)];
            }
          }
        }
      }
    }
  }

  outResidentBrickCount = static_cast<uint32_t>(residentBrickData.size() / kSparseBrickCellCount);
  std::vector<uint8_t> payload;
  payload.reserve(6u * sizeof(uint32_t) + brickTable.size() * sizeof(uint32_t) + residentBrickData.size());
  auto appendU32 = [&](uint32_t value)
  {
    payload.push_back(static_cast<uint8_t>(value & 0xffu));
    payload.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    payload.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    payload.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
  };
  appendU32(0x42534456u); // VSDB
  appendU32(kSparseBrickDim);
  appendU32(brickGridX);
  appendU32(brickGridY);
  appendU32(brickGridZ);
  appendU32(outResidentBrickCount);
  for (uint32_t brickIndex : brickTable)
    appendU32(brickIndex);
  payload.insert(payload.end(), residentBrickData.begin(), residentBrickData.end());
  return payload;
}

struct RendererAdaptiveOctreeSDFPayload
{
  std::vector<uint8_t> data;
  RendererSDFGridResolution virtualGridResolution;
  uint32_t residentBrickCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t leafCount = 0u;
  uint32_t maxDepthReached = 0u;
};

static RendererAdaptiveOctreeSDFPayload buildRendererMeshSDFAdaptiveOctreeBrick8(
    const FallbackMeshData &mesh,
    const VSDFBounds &bounds,
    float narrowBandDistance,
    uint32_t meshIndex,
    uint32_t meshCount)
{
  static constexpr uint32_t kRootDim = 4u;
  static constexpr uint32_t kRootNodeCount = kRootDim * kRootDim * kRootDim;
  static constexpr uint32_t kBrickDim = 8u;
  static constexpr uint32_t kBrickCellCount = kBrickDim * kBrickDim * kBrickDim;
  static constexpr uint32_t kHeaderBytes = 32u;
  static constexpr uint32_t kNodeLeafFlag = 1u << 0u;
  static constexpr uint32_t kNodeResidentFlag = 1u << 1u;

  struct BakeTriangle
  {
    math::Vec3f a;
    math::Vec3f b;
    math::Vec3f c;
    VSDFBounds bounds;
    math::Vec3f center;
    math::Vec3f normal;
    float area = 0.0f;
  };

  struct BakeBVHNode
  {
    VSDFBounds bounds;
    uint32_t firstTriangle = 0u;
    uint32_t triangleCount = 0u;
    uint32_t leftChild = UINT32_MAX;
    uint32_t rightChild = UINT32_MAX;
  };

  struct OctreeNode
  {
    uint32_t childBase = UINT32_MAX;
    uint32_t brickIndex = UINT32_MAX;
    uint32_t flags = kNodeLeafFlag;
  };

  RendererAdaptiveOctreeSDFPayload result;
  const uint32_t maxDepth = std::clamp(gRendererVSDFBuildSettings.adaptiveMaxDepth, 1u, 6u);
  const uint32_t virtualAxisResolution = kRootDim * kBrickDim * (1u << maxDepth);
  result.virtualGridResolution = RendererSDFGridResolution{virtualAxisResolution, virtualAxisResolution, virtualAxisResolution};

  const float extentX = std::max(bounds.max[0] - bounds.min[0], 1.0e-3f);
  const float extentY = std::max(bounds.max[1] - bounds.min[1], 1.0e-3f);
  const float extentZ = std::max(bounds.max[2] - bounds.min[2], 1.0e-3f);
  const float narrowBand = std::max(narrowBandDistance, 1.0e-3f);
  const float targetError = std::max(gRendererVSDFBuildSettings.adaptiveTargetError, 1.0e-5f);
  const float finestCellX = extentX / static_cast<float>(virtualAxisResolution);
  const float finestCellY = extentY / static_cast<float>(virtualAxisResolution);
  const float finestCellZ = extentZ / static_cast<float>(virtualAxisResolution);
  const float finestMedianCell = std::max(std::min(finestCellX, finestCellY), std::min(std::max(finestCellX, finestCellY), finestCellZ));
  const float unsignedConservativeBias = finestMedianCell * std::max(gRendererVSDFBuildSettings.unsignedConservativeBiasCells, 0.0f);
  const float unsignedSearchBand = narrowBand + unsignedConservativeBias + std::max(targetError, finestMedianCell * std::max(gRendererVSDFBuildSettings.unsignedGuardBandCells, 0.0f));

  std::vector<BakeTriangle> triangles;
  triangles.reserve(mesh.indices.size() / 3u);
  for (uint32_t indexOffset = 0u; indexOffset + 2u < mesh.indices.size(); indexOffset += 3u)
  {
    const uint32_t i0 = mesh.indices[indexOffset + 0u];
    const uint32_t i1 = mesh.indices[indexOffset + 1u];
    const uint32_t i2 = mesh.indices[indexOffset + 2u];
    if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
      continue;

    BakeTriangle triangle;
    triangle.a = fallbackVertexPosition(mesh.vertices[i0]);
    triangle.b = fallbackVertexPosition(mesh.vertices[i1]);
    triangle.c = fallbackVertexPosition(mesh.vertices[i2]);
    triangle.bounds = triangleBounds(triangle.a, triangle.b, triangle.c);
    triangle.center = centerVSDFBounds(triangle.bounds);
    const math::Vec3f geometricNormal = (triangle.b - triangle.a).cross(triangle.c - triangle.a);
    const float normalLengthSquared = lengthSquared3(geometricNormal);
    if (normalLengthSquared <= 1.0e-16f)
      continue;
    const float normalLength = std::sqrt(normalLengthSquared);
    triangle.normal = geometricNormal * (1.0f / normalLength);
    triangle.area = normalLength * 0.5f;
    triangles.push_back(triangle);
  }

  os::Logger::logf(
      "[VSDF] emit mesh %u/%u: adaptive octree root=4^3 leaf=8^3 maxDepth=%u virtual=%ux%ux%u targetError=%.6f narrowBand=%.3f triangles=%zu",
      meshIndex + 1u,
      meshCount,
      maxDepth,
      result.virtualGridResolution.x,
      result.virtualGridResolution.y,
      result.virtualGridResolution.z,
      targetError,
      narrowBand,
      triangles.size());

  std::vector<OctreeNode> octreeNodes(kRootNodeCount);
  std::vector<uint8_t> residentBrickData;
  if (triangles.empty())
  {
    result.nodeCount = static_cast<uint32_t>(octreeNodes.size());
    result.leafCount = result.nodeCount;
  }

  std::vector<uint32_t> triangleIndices(triangles.size());
  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(triangleIndices.size()); ++triangleIndex)
    triangleIndices[triangleIndex] = triangleIndex;

  std::vector<BakeBVHNode> bvhNodes;
  bvhNodes.reserve(triangles.size() * 2u);
  std::function<uint32_t(uint32_t, uint32_t)> buildBVH =
      [&](const uint32_t firstTriangle, const uint32_t triangleCount) -> uint32_t
  {
    BakeBVHNode node;
    node.bounds = emptyVSDFBounds();
    VSDFBounds centroidBounds = emptyVSDFBounds();
    for (uint32_t offset = 0u; offset < triangleCount; ++offset)
    {
      const BakeTriangle &triangle = triangles[triangleIndices[firstTriangle + offset]];
      includeVSDFBounds(node.bounds, triangle.bounds);
      includeVSDFPoint(centroidBounds, triangle.center);
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(bvhNodes.size());
    bvhNodes.push_back(node);
    static constexpr uint32_t kLeafTriangleCount = 8u;
    if (triangleCount <= kLeafTriangleCount)
    {
      BakeBVHNode &leaf = bvhNodes[nodeIndex];
      leaf.firstTriangle = firstTriangle;
      leaf.triangleCount = triangleCount;
      return nodeIndex;
    }

    const math::Vec3f centroidExtent(
        centroidBounds.max[0] - centroidBounds.min[0],
        centroidBounds.max[1] - centroidBounds.min[1],
        centroidBounds.max[2] - centroidBounds.min[2]);
    uint32_t splitAxis = 0u;
    if (centroidExtent[1] > centroidExtent[splitAxis])
      splitAxis = 1u;
    if (centroidExtent[2] > centroidExtent[splitAxis])
      splitAxis = 2u;

    const uint32_t leftCount = triangleCount / 2u;
    const uint32_t middle = firstTriangle + leftCount;
    std::nth_element(
        triangleIndices.begin() + firstTriangle,
        triangleIndices.begin() + middle,
        triangleIndices.begin() + firstTriangle + triangleCount,
        [&](const uint32_t lhs, const uint32_t rhs)
        {
          return triangles[lhs].center[splitAxis] < triangles[rhs].center[splitAxis];
        });

    bvhNodes[nodeIndex].leftChild = buildBVH(firstTriangle, leftCount);
    bvhNodes[nodeIndex].rightChild = buildBVH(middle, triangleCount - leftCount);
    return nodeIndex;
  };

  const uint32_t bvhRoot = triangles.empty() ? UINT32_MAX : buildBVH(0u, static_cast<uint32_t>(triangles.size()));

  struct NearestTriangleResult
  {
    float distanceSquared = 0.0f;
    bool found = false;
  };

  auto nearestTriangleDistance = [&](const math::Vec3f &p, const float initialMinDistanceSquared) -> NearestTriangleResult
  {
    NearestTriangleResult nearest;
    nearest.distanceSquared = initialMinDistanceSquared;
    nearest.found = false;
    if (bvhRoot == UINT32_MAX)
      return nearest;

    std::array<uint32_t, 2048u> stack{};
    uint32_t stackCount = 0u;
    stack[stackCount++] = bvhRoot;
    while (stackCount != 0u)
    {
      const BakeBVHNode &node = bvhNodes[stack[--stackCount]];
      if (distanceSquaredPointAabb(p, node.bounds) > nearest.distanceSquared)
        continue;

      if (node.triangleCount != 0u)
      {
        for (uint32_t offset = 0u; offset < node.triangleCount; ++offset)
        {
          const BakeTriangle &triangle = triangles[triangleIndices[node.firstTriangle + offset]];
          if (distanceSquaredPointAabb(p, triangle.bounds) > nearest.distanceSquared)
            continue;
          const float distanceSquared = distanceSquaredPointTriangle(p, triangle.a, triangle.b, triangle.c);
          if (distanceSquared < nearest.distanceSquared)
          {
            nearest.distanceSquared = distanceSquared;
            nearest.found = true;
          }
        }
        continue;
      }

      const uint32_t leftChild = node.leftChild;
      const uint32_t rightChild = node.rightChild;
      const float leftDistanceSquared = leftChild != UINT32_MAX ? distanceSquaredPointAabb(p, bvhNodes[leftChild].bounds) : std::numeric_limits<float>::max();
      const float rightDistanceSquared = rightChild != UINT32_MAX ? distanceSquaredPointAabb(p, bvhNodes[rightChild].bounds) : std::numeric_limits<float>::max();
      if (leftDistanceSquared < rightDistanceSquared)
      {
        if (rightDistanceSquared <= nearest.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = rightChild;
        if (leftDistanceSquared <= nearest.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = leftChild;
      }
      else
      {
        if (leftDistanceSquared <= nearest.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = leftChild;
        if (rightDistanceSquared <= nearest.distanceSquared && stackCount < stack.size())
          stack[stackCount++] = rightChild;
      }
    }
    return nearest;
  };

  auto boundsOverlap = [](const VSDFBounds &a, const VSDFBounds &b) -> bool
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (a.max[axis] < b.min[axis] || a.min[axis] > b.max[axis])
        return false;
    }
    return true;
  };

  auto expandedBounds = [](VSDFBounds value, const float pad) -> VSDFBounds
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      value.min[axis] -= pad;
      value.max[axis] += pad;
    }
    return value;
  };

  auto collectNodeSurfaceTriangles = [&](const VSDFBounds &nodeBounds, std::vector<uint32_t> &outTriangleIndices)
  {
    outTriangleIndices.clear();
    const VSDFBounds queryBounds = expandedBounds(nodeBounds, narrowBand + targetError + finestMedianCell * 2.0f);
    if (bvhRoot == UINT32_MAX)
      return;

    std::array<uint32_t, 2048u> stack{};
    uint32_t stackCount = 0u;
    stack[stackCount++] = bvhRoot;
    while (stackCount != 0u)
    {
      const BakeBVHNode &node = bvhNodes[stack[--stackCount]];
      if (!boundsOverlap(queryBounds, node.bounds))
        continue;

      if (node.triangleCount != 0u)
      {
        for (uint32_t offset = 0u; offset < node.triangleCount; ++offset)
        {
          const uint32_t triangleIndex = triangleIndices[node.firstTriangle + offset];
          const BakeTriangle &triangle = triangles[triangleIndex];
          if (boundsOverlap(queryBounds, triangle.bounds))
            outTriangleIndices.push_back(triangleIndex);
        }
        continue;
      }

      if (node.leftChild != UINT32_MAX && stackCount < stack.size())
        stack[stackCount++] = node.leftChild;
      if (node.rightChild != UINT32_MAX && stackCount < stack.size())
        stack[stackCount++] = node.rightChild;
    }
  };

  auto pointInBounds = [](const VSDFBounds &nodeBounds, const float ux, const float uy, const float uz) -> math::Vec3f
  {
    return math::Vec3f(
        nodeBounds.min[0] + (nodeBounds.max[0] - nodeBounds.min[0]) * ux,
        nodeBounds.min[1] + (nodeBounds.max[1] - nodeBounds.min[1]) * uy,
        nodeBounds.min[2] + (nodeBounds.max[2] - nodeBounds.min[2]) * uz);
  };

  auto distanceAt = [&](const math::Vec3f &p) -> float
  {
    const NearestTriangleResult nearest = nearestTriangleDistance(p, unsignedSearchBand * unsignedSearchBand);
    if (!nearest.found)
      return unsignedSearchBand;
    return std::sqrt(nearest.distanceSquared);
  };

  auto closestPointOnTriangle = [](const math::Vec3f &p, const math::Vec3f &a, const math::Vec3f &b, const math::Vec3f &c) -> math::Vec3f
  {
    const math::Vec3f ab = b - a;
    const math::Vec3f ac = c - a;
    const math::Vec3f ap = p - a;
    const float d1 = dot3(ab, ap);
    const float d2 = dot3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
      return a;

    const math::Vec3f bp = p - b;
    const float d3 = dot3(ab, bp);
    const float d4 = dot3(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
      return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
      const float v = d1 / (d1 - d3);
      return a + ab * v;
    }

    const math::Vec3f cp = p - c;
    const float d5 = dot3(ab, cp);
    const float d6 = dot3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
      return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
      const float w = d2 / (d2 - d6);
      return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
      const math::Vec3f bc = c - b;
      const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      return b + bc * w;
    }

    const float denom = 1.0f / std::max(va + vb + vc, 1.0e-12f);
    const float v = vb * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
  };

  auto trilinear = [](const std::array<float, 8u> &corner, const float x, const float y, const float z) -> float
  {
    const float c00 = corner[0] * (1.0f - x) + corner[1] * x;
    const float c10 = corner[2] * (1.0f - x) + corner[3] * x;
    const float c01 = corner[4] * (1.0f - x) + corner[5] * x;
    const float c11 = corner[6] * (1.0f - x) + corner[7] * x;
    const float c0 = c00 * (1.0f - y) + c10 * y;
    const float c1 = c01 * (1.0f - y) + c11 * y;
    return c0 * (1.0f - z) + c1 * z;
  };

  auto pointInsideBounds = [](const VSDFBounds &nodeBounds, const math::Vec3f &p, const float pad) -> bool
  {
    return p[0] >= nodeBounds.min[0] - pad && p[0] <= nodeBounds.max[0] + pad &&
           p[1] >= nodeBounds.min[1] - pad && p[1] <= nodeBounds.max[1] + pad &&
           p[2] >= nodeBounds.min[2] - pad && p[2] <= nodeBounds.max[2] + pad;
  };

  auto pointBoundsUVW = [](const VSDFBounds &nodeBounds, const math::Vec3f &p) -> math::Vec3f
  {
    return math::Vec3f(
        std::clamp((p[0] - nodeBounds.min[0]) / std::max(nodeBounds.max[0] - nodeBounds.min[0], 1.0e-6f), 0.0f, 1.0f),
        std::clamp((p[1] - nodeBounds.min[1]) / std::max(nodeBounds.max[1] - nodeBounds.min[1], 1.0e-6f), 0.0f, 1.0f),
        std::clamp((p[2] - nodeBounds.min[2]) / std::max(nodeBounds.max[2] - nodeBounds.min[2], 1.0e-6f), 0.0f, 1.0f));
  };

  auto nodeSurfaceError = [&](const VSDFBounds &nodeBounds, const std::vector<uint32_t> &overlappingTriangles) -> float
  {
    std::array<float, 8u> corner{};
    corner[0] = distanceAt(pointInBounds(nodeBounds, 0.0f, 0.0f, 0.0f));
    corner[1] = distanceAt(pointInBounds(nodeBounds, 1.0f, 0.0f, 0.0f));
    corner[2] = distanceAt(pointInBounds(nodeBounds, 0.0f, 1.0f, 0.0f));
    corner[3] = distanceAt(pointInBounds(nodeBounds, 1.0f, 1.0f, 0.0f));
    corner[4] = distanceAt(pointInBounds(nodeBounds, 0.0f, 0.0f, 1.0f));
    corner[5] = distanceAt(pointInBounds(nodeBounds, 1.0f, 0.0f, 1.0f));
    corner[6] = distanceAt(pointInBounds(nodeBounds, 0.0f, 1.0f, 1.0f));
    corner[7] = distanceAt(pointInBounds(nodeBounds, 1.0f, 1.0f, 1.0f));

    float maxError = 0.0f;
    bool sampledSurface = false;
    const float samplePad = narrowBand + targetError + finestMedianCell * 2.0f;
    auto accumulateSurfacePoint = [&](const math::Vec3f &p)
    {
      if (!pointInsideBounds(nodeBounds, p, samplePad))
        return;
      const math::Vec3f uvw = pointBoundsUVW(nodeBounds, p);
      maxError = std::max(maxError, trilinear(corner, uvw[0], uvw[1], uvw[2]));
      sampledSurface = true;
    };

    const math::Vec3f nodeCenter = centerVSDFBounds(nodeBounds);
    for (const uint32_t triangleIndex : overlappingTriangles)
    {
      const BakeTriangle &triangle = triangles[triangleIndex];
      accumulateSurfacePoint(triangle.a);
      accumulateSurfacePoint(triangle.b);
      accumulateSurfacePoint(triangle.c);
      accumulateSurfacePoint((triangle.a + triangle.b + triangle.c) * (1.0f / 3.0f));
      accumulateSurfacePoint(closestPointOnTriangle(nodeCenter, triangle.a, triangle.b, triangle.c));
    }
    return sampledSurface ? maxError : targetError * 2.0f;
  };

  auto buildBrickData = [&](const VSDFBounds &nodeBounds) -> std::array<uint8_t, kBrickCellCount>
  {
    std::array<uint8_t, kBrickCellCount> brickData{};
    brickData.fill(255u);
    for (uint32_t z = 0u; z < kBrickDim; ++z)
    {
      for (uint32_t y = 0u; y < kBrickDim; ++y)
      {
        for (uint32_t x = 0u; x < kBrickDim; ++x)
        {
          const float ux = (static_cast<float>(x) + 0.5f) / static_cast<float>(kBrickDim);
          const float uy = (static_cast<float>(y) + 0.5f) / static_cast<float>(kBrickDim);
          const float uz = (static_cast<float>(z) + 0.5f) / static_cast<float>(kBrickDim);
          const math::Vec3f p = pointInBounds(nodeBounds, ux, uy, uz);
          const NearestTriangleResult nearest = nearestTriangleDistance(p, std::numeric_limits<float>::max());
          if (!nearest.found)
            continue;
          const float distance = std::sqrt(nearest.distanceSquared);
          const float storedDistance = std::max(distance - unsignedConservativeBias, 0.0f);
          const float encoded = std::clamp(storedDistance / narrowBand, 0.0f, 1.0f);
          brickData[(z * kBrickDim + y) * kBrickDim + x] = std::min<uint8_t>(quantizeUnorm8(encoded), 254u);
        }
      }
    }
    return brickData;
  };

  const auto bakeStart = lib::time::TimeSpan::now();
  std::atomic<uint32_t> visitedNodeCount{0u};
  static constexpr uint32_t kAdaptiveProgressNodeInterval = 4096u;
  struct PendingOctreeNode
  {
    uint32_t nodeIndex = 0u;
    uint32_t depth = 0u;
    VSDFBounds bounds;
  };

  struct ProcessedOctreeNode
  {
    bool touchesSurface = false;
    bool shouldSplit = false;
    bool hasResidentBrick = false;
    std::array<uint8_t, kBrickCellCount> brickData{};
  };

  auto childBoundsFor = [](const VSDFBounds &parentBounds, const uint32_t child) -> VSDFBounds
  {
    VSDFBounds childBounds = parentBounds;
    const math::Vec3f center = centerVSDFBounds(parentBounds);
    if ((child & 1u) == 0u)
      childBounds.max[0] = center[0];
    else
      childBounds.min[0] = center[0];
    if ((child & 2u) == 0u)
      childBounds.max[1] = center[1];
    else
      childBounds.min[1] = center[1];
    if ((child & 4u) == 0u)
      childBounds.max[2] = center[2];
    else
      childBounds.min[2] = center[2];
    return childBounds;
  };

  if (!triangles.empty())
  {
    const auto buildRootBounds = [&](const uint32_t x, const uint32_t y, const uint32_t z) -> VSDFBounds
    {
      VSDFBounds rootBounds = bounds;
      rootBounds.min[0] = bounds.min[0] + extentX * static_cast<float>(x) / static_cast<float>(kRootDim);
      rootBounds.max[0] = bounds.min[0] + extentX * static_cast<float>(x + 1u) / static_cast<float>(kRootDim);
      rootBounds.min[1] = bounds.min[1] + extentY * static_cast<float>(y) / static_cast<float>(kRootDim);
      rootBounds.max[1] = bounds.min[1] + extentY * static_cast<float>(y + 1u) / static_cast<float>(kRootDim);
      rootBounds.min[2] = bounds.min[2] + extentZ * static_cast<float>(z) / static_cast<float>(kRootDim);
      rootBounds.max[2] = bounds.min[2] + extentZ * static_cast<float>(z + 1u) / static_cast<float>(kRootDim);
      return rootBounds;
    };

    std::vector<PendingOctreeNode> frontier;
    frontier.reserve(kRootNodeCount);
    for (uint32_t z = 0u; z < kRootDim; ++z)
    {
      for (uint32_t y = 0u; y < kRootDim; ++y)
      {
        for (uint32_t x = 0u; x < kRootDim; ++x)
        {
          const uint32_t rootIndex = (z * kRootDim + y) * kRootDim + x;
          frontier.push_back(PendingOctreeNode{
              .nodeIndex = rootIndex,
              .depth = 0u,
              .bounds = buildRootBounds(x, y, z),
          });
        }
      }
    }

    const uint32_t hardwareWorkerCount = std::max(1u, std::thread::hardware_concurrency());
    while (!frontier.empty())
    {
      const uint32_t frontierDepth = frontier.front().depth;
      result.maxDepthReached = std::max(result.maxDepthReached, frontierDepth);
      std::vector<ProcessedOctreeNode> processed(frontier.size());
      std::atomic<uint32_t> nextFrontierIndex{0u};
      std::atomic<uint32_t> completedFrontierNodes{0u};
      const uint32_t workerCount = std::max(1u, std::min<uint32_t>(hardwareWorkerCount, static_cast<uint32_t>(frontier.size())));
      auto processFrontier = [&]()
      {
        for (;;)
        {
          const uint32_t frontierIndex = nextFrontierIndex.fetch_add(1u);
          if (frontierIndex >= frontier.size())
            break;

          const PendingOctreeNode &pending = frontier[frontierIndex];
          ProcessedOctreeNode nodeResult;
          std::vector<uint32_t> overlappingTriangles;
          collectNodeSurfaceTriangles(pending.bounds, overlappingTriangles);
          nodeResult.touchesSurface = !overlappingTriangles.empty();
          if (nodeResult.touchesSurface)
          {
            const bool canSplit = pending.depth < maxDepth;
            const float surfaceError = canSplit ? nodeSurfaceError(pending.bounds, overlappingTriangles) : 0.0f;
            nodeResult.shouldSplit = canSplit && surfaceError > targetError;
            nodeResult.hasResidentBrick = true;
            nodeResult.brickData = buildBrickData(pending.bounds);
          }
          processed[frontierIndex] = std::move(nodeResult);

          const uint32_t totalVisited = visitedNodeCount.fetch_add(1u) + 1u;
          const uint32_t completed = completedFrontierNodes.fetch_add(1u) + 1u;
          if ((completed % kAdaptiveProgressNodeInterval) == 0u || completed == frontier.size())
          {
            os::Logger::logf(
                "[VSDF] emit mesh %u/%u: adaptive depth %u frontier %u/%zu totalVisited=%u allocatedNodes=%zu leaves=%u residentBricks=%zu workers=%u elapsed=%.1f ms",
                meshIndex + 1u,
                meshCount,
                pending.depth,
                completed,
                frontier.size(),
                totalVisited,
                octreeNodes.size(),
                result.leafCount,
                residentBrickData.size() / kBrickCellCount,
                workerCount,
                (lib::time::TimeSpan::now() - bakeStart).milliseconds());
          }
        }
      };

      if (workerCount == 1u)
      {
        processFrontier();
      }
      else
      {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex)
          workers.emplace_back(processFrontier);
        for (std::thread &worker : workers)
          worker.join();
      }

      std::vector<PendingOctreeNode> nextFrontier;
      for (uint32_t frontierIndex = 0u; frontierIndex < static_cast<uint32_t>(frontier.size()); ++frontierIndex)
      {
        const PendingOctreeNode &pending = frontier[frontierIndex];
        const ProcessedOctreeNode &nodeResult = processed[frontierIndex];
        if (!nodeResult.touchesSurface)
        {
          octreeNodes[pending.nodeIndex] = OctreeNode{};
          ++result.leafCount;
          continue;
        }
        if (nodeResult.shouldSplit)
        {
          const uint32_t childBase = static_cast<uint32_t>(octreeNodes.size());
          octreeNodes[pending.nodeIndex].childBase = childBase;
          octreeNodes[pending.nodeIndex].flags = nodeResult.hasResidentBrick ? kNodeResidentFlag : 0u;
          if (nodeResult.hasResidentBrick)
          {
            const uint32_t brickIndex = static_cast<uint32_t>(residentBrickData.size() / kBrickCellCount);
            residentBrickData.insert(residentBrickData.end(), nodeResult.brickData.begin(), nodeResult.brickData.end());
            octreeNodes[pending.nodeIndex].brickIndex = brickIndex;
          }
          octreeNodes.resize(octreeNodes.size() + 8u);
          for (uint32_t child = 0u; child < 8u; ++child)
          {
            nextFrontier.push_back(PendingOctreeNode{
                .nodeIndex = childBase + child,
                .depth = pending.depth + 1u,
                .bounds = childBoundsFor(pending.bounds, child),
            });
          }
          continue;
        }

        if (nodeResult.hasResidentBrick)
        {
          const uint32_t brickIndex = static_cast<uint32_t>(residentBrickData.size() / kBrickCellCount);
          residentBrickData.insert(residentBrickData.end(), nodeResult.brickData.begin(), nodeResult.brickData.end());
          octreeNodes[pending.nodeIndex].brickIndex = brickIndex;
          octreeNodes[pending.nodeIndex].flags = kNodeLeafFlag | kNodeResidentFlag;
        }
        else
        {
          octreeNodes[pending.nodeIndex] = OctreeNode{};
        }
        ++result.leafCount;
      }

      os::Logger::logf(
          "[VSDF] emit mesh %u/%u: adaptive depth %u complete frontier=%zu next=%zu visitedNodes=%u allocatedNodes=%zu leaves=%u residentBricks=%zu elapsed=%.1f ms",
          meshIndex + 1u,
          meshCount,
          frontierDepth,
          frontier.size(),
          nextFrontier.size(),
          visitedNodeCount.load(),
          octreeNodes.size(),
          result.leafCount,
          residentBrickData.size() / kBrickCellCount,
          (lib::time::TimeSpan::now() - bakeStart).milliseconds());
      frontier = std::move(nextFrontier);
    }

    os::Logger::logf(
        "[VSDF] emit mesh %u/%u: adaptive octree nodes=%zu leaves=%u residentBricks=%zu maxDepthReached=%u elapsed=%.1f ms",
        meshIndex + 1u,
        meshCount,
        octreeNodes.size(),
        result.leafCount,
        residentBrickData.size() / kBrickCellCount,
        result.maxDepthReached,
        (lib::time::TimeSpan::now() - bakeStart).milliseconds());
  }

  result.nodeCount = static_cast<uint32_t>(octreeNodes.size());
  result.residentBrickCount = static_cast<uint32_t>(residentBrickData.size() / kBrickCellCount);
  const uint32_t nodeTableOffset = kHeaderBytes;
  const uint32_t brickDataOffset = nodeTableOffset + result.nodeCount * 12u;

  result.data.reserve(kHeaderBytes + octreeNodes.size() * 8u + residentBrickData.size());
  auto appendU32 = [&](uint32_t value)
  {
    result.data.push_back(static_cast<uint8_t>(value & 0xffu));
    result.data.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    result.data.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    result.data.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
  };
  appendU32(0x544f5356u); // VSOT
  appendU32(kBrickDim);
  appendU32(kRootDim);
  appendU32(maxDepth);
  appendU32(result.nodeCount);
  appendU32(result.residentBrickCount);
  appendU32(nodeTableOffset);
  appendU32(brickDataOffset);
  for (const OctreeNode &node : octreeNodes)
  {
    appendU32(node.childBase);
    appendU32(node.brickIndex);
    appendU32(node.flags);
  }
  result.data.insert(result.data.end(), residentBrickData.begin(), residentBrickData.end());
  return result;
}

static bool writeRendererVSDFScene(
    const fs::path &assetPath,
    const fs::path &virtualGeometryPath,
    const std::vector<FallbackMeshData> &vsdfMeshes)
{
  if (vsdfMeshes.empty())
    return false;

  const auto emitStart = lib::time::TimeSpan::now();
  os::Logger::logf(
      "[VSDF] emit start: asset=%s output=%s meshes=%zu",
      assetPath.string().c_str(),
      buildRendererVSDFPath(virtualGeometryPath).string().c_str(),
      vsdfMeshes.size());

  VSDFSceneData scene;
  scene.sourceAssetPath = assetPath.string();
  scene.unitScale = 100.0f;
  scene.sceneBounds = makeVSDFBounds(vsdfMeshes.front().boundsMin, vsdfMeshes.front().boundsMax);
  for (const FallbackMeshData &mesh : vsdfMeshes)
    includeVSDFBounds(scene.sceneBounds, makeVSDFBounds(mesh.boundsMin, mesh.boundsMax));
  const std::vector<FallbackMeshData> &emitMeshes = vsdfMeshes;

  std::unordered_map<std::string, uint32_t> materialIndexByKey;
  std::vector<uint32_t> instanceMaterialIndices;
  std::vector<VSDFSurfaceCacheMaterial> cacheMaterials;
  instanceMaterialIndices.reserve(emitMeshes.size());

  auto internMaterial = [&](const std::string &key, const std::string &sourcePath, const VSDFSurfaceCacheMaterial &cacheMaterial) -> uint32_t
  {
    const auto found = materialIndexByKey.find(key);
    if (found != materialIndexByKey.end())
      return found->second;

    const uint32_t materialIndex = static_cast<uint32_t>(scene.materials.size());
    materialIndexByKey.emplace(key, materialIndex);
    VSDFMaterialEntry material;
    material.name = key;
    material.sourcePath = sourcePath;
    scene.materials.push_back(material);
    cacheMaterials.push_back(cacheMaterial);
    return materialIndex;
  };

  struct BakedRendererSDFChunk
  {
    VSDFSdfChunkPayload chunk;
    RendererSDFGridResolution gridResolution;
    VSDFBounds localBounds;
    float narrowBandDistance = 0.0f;
    bool valid = false;
  };

  std::vector<BakedRendererSDFChunk> bakedSdfChunks(emitMeshes.size());
  std::atomic<uint32_t> nextSdfMesh{0u};
  std::atomic<uint32_t> completedSdfMeshes{0u};
  std::atomic<bool> sdfBakeFailed{false};
  const uint32_t meshCount = static_cast<uint32_t>(emitMeshes.size());
  const uint32_t sdfWorkerCount = 1u;
  os::Logger::logf(
      "[VSDF] emit sdf: begin sequential instance bake meshes=%u workers=%u internalOctreeWorkers=hardware elapsed=%.1f ms",
      meshCount,
      sdfWorkerCount,
      (lib::time::TimeSpan::now() - emitStart).milliseconds());

  std::vector<std::thread> sdfWorkers;
  sdfWorkers.reserve(sdfWorkerCount);
  for (uint32_t workerIndex = 0u; workerIndex < sdfWorkerCount; ++workerIndex)
  {
    sdfWorkers.emplace_back(
        [&]()
        {
          for (;;)
          {
            const uint32_t meshIndex = nextSdfMesh.fetch_add(1u);
            if (meshIndex >= meshCount)
              break;

            try
            {
              const FallbackMeshData &fallback = emitMeshes[meshIndex];
              const VSDFBounds sourceLocalBounds = makeVSDFBounds(fallback.boundsMin, fallback.boundsMax);
              RendererMeshSDFTopology topology = classifyRendererMeshSDFTopology(fallback);
              const bool sourceWasClosed = topology.closed;
              topology.closed = false;
              topology.watertight = false;
              const bool useSignedDistance = false;
              const char *topologyLabel = sourceWasClosed
                                              ? (topology.thinSurfaceLike ? "closed-thin-unsigned-surface" : "closed-unsigned-surface")
                                              : "soup-feature-unsigned";
              RendererSDFGridResolution gridResolution = chooseRendererMeshSDFGridResolution(fallback, meshCount, topology);
              VSDFBounds localBounds = expandRendererMeshSDFBounds(sourceLocalBounds, gridResolution, false);
              gridResolution = refineRendererOpenMeshSDFGridResolutionForBounds(sourceLocalBounds, localBounds, gridResolution, false);
              localBounds = expandRendererMeshSDFBounds(sourceLocalBounds, gridResolution, false);
              const uint32_t brickResolution = std::max({gridResolution.x, gridResolution.y, gridResolution.z});
              const float narrowBandDistance = chooseRendererMeshSDFNarrowBand(localBounds, gridResolution, topology);
              os::Logger::logf(
                  "[VSDF] emit sdf: mesh %u/%u topology=%s triangles=%u boundaryEdges=%u nonManifoldEdges=%u signedVolume=%.3f grid=%ux%ux%u brickMax=%u narrowBand=%.3f",
                  meshIndex + 1u,
                  meshCount,
                  topologyLabel,
                  topology.validTriangleCount,
                  topology.boundaryEdgeCount,
                  topology.nonManifoldEdgeCount,
                  topology.signedVolume,
                  gridResolution.x,
                  gridResolution.y,
                  gridResolution.z,
                  brickResolution,
                  narrowBandDistance);

              VSDFSdfChunkPayload chunk;
              chunk.metadata.meshIndex = meshIndex;
              chunk.metadata.mipLevel = 0u;
              chunk.metadata.firstBrick = 0u;
              chunk.metadata.brickCount = 0u;
              chunk.metadata.compression = VSDFCompression_LZ4;
              chunk.metadata.flags = VSDFSdfChunkFlag_MeshUnsignedDistance |
                                     VSDFSdfChunkFlag_PreciseUnsignedDistance |
                                     VSDFSdfChunkFlag_MeshOrientedSurfaceDistance |
                                     VSDFSdfChunkFlag_LocalNarrowBandDistance |
                                     VSDFSdfChunkFlag_WorldErrorGrid |
                                     VSDFSdfChunkFlag_ValidatedShellMinimum |
                                     VSDFSdfChunkFlag_ExpandedSdfBounds |
                                     VSDFSdfChunkFlag_SurfaceAxisPreservingGrid |
                                     VSDFSdfChunkFlag_LowPolyWorldErrorGrid |
                                     VSDFSdfChunkFlag_FeatureAxisGrid |
                                     VSDFSdfChunkFlag_ExplicitOpenSurfaceValidity |
                                     VSDFSdfChunkFlag_FeatureLocalNarrowBand |
                                     VSDFSdfChunkFlag_ProjectedOpenSurfaceDistance |
                                     VSDFSdfChunkFlag_AnisotropicGrid |
                                     VSDFSdfChunkFlag_AxisTightOpenBounds |
                                     VSDFSdfChunkFlag_OpenSurfaceGuardBand |
                                     VSDFSdfChunkFlag_SparseOctreeBrick8;
              const RendererAdaptiveOctreeSDFPayload adaptiveSdf = buildRendererMeshSDFAdaptiveOctreeBrick8(fallback, localBounds, narrowBandDistance, meshIndex, meshCount);
              gridResolution = adaptiveSdf.virtualGridResolution;
              chunk.metadata.brickResolutionX = gridResolution.x;
              chunk.metadata.brickResolutionY = gridResolution.y;
              chunk.metadata.brickResolutionZ = gridResolution.z;
              chunk.metadata.brickCount = adaptiveSdf.residentBrickCount;
              chunk.data = adaptiveSdf.data;
              const uint64_t sparseBytes = static_cast<uint64_t>(chunk.data.size());
              os::Logger::logf(
                  "[VSDF] emit mesh %u/%u: adaptive sparse octree residentBricks=%u nodes=%u leaves=%u payload=%llu virtual=%ux%ux%u maxDepthReached=%u",
                  meshIndex + 1u,
                  meshCount,
                  chunk.metadata.brickCount,
                  adaptiveSdf.nodeCount,
                  adaptiveSdf.leafCount,
                  static_cast<unsigned long long>(sparseBytes),
                  gridResolution.x,
                  gridResolution.y,
                  gridResolution.z,
                  adaptiveSdf.maxDepthReached);

              bakedSdfChunks[meshIndex].chunk = std::move(chunk);
              bakedSdfChunks[meshIndex].gridResolution = gridResolution;
              bakedSdfChunks[meshIndex].localBounds = localBounds;
              bakedSdfChunks[meshIndex].narrowBandDistance = narrowBandDistance;
              bakedSdfChunks[meshIndex].valid = true;
              const uint32_t completed = completedSdfMeshes.fetch_add(1u) + 1u;
              os::Logger::logf(
                  "[VSDF] emit sdf: completed mesh %u/%u total=%u/%u elapsed=%.1f ms",
                  meshIndex + 1u,
                  meshCount,
                  completed,
                  meshCount,
                  (lib::time::TimeSpan::now() - emitStart).milliseconds());
            }
            catch (const std::exception &exception)
            {
              sdfBakeFailed.store(true);
              os::Logger::warningf("[VSDF] emit sdf: mesh %u/%u failed: %s", meshIndex + 1u, meshCount, exception.what());
            }
            catch (...)
            {
              sdfBakeFailed.store(true);
              os::Logger::warningf("[VSDF] emit sdf: mesh %u/%u failed with unknown exception", meshIndex + 1u, meshCount);
            }
          }
        });
  }
  for (std::thread &worker : sdfWorkers)
    worker.join();

  for (uint32_t meshIndex = 0u; meshIndex < meshCount; ++meshIndex)
  {
    if (!bakedSdfChunks[meshIndex].valid)
      sdfBakeFailed.store(true);
  }
  if (sdfBakeFailed.load())
  {
    os::Logger::warningf("[VSDF] emit sdf: failed after %.1f ms", (lib::time::TimeSpan::now() - emitStart).milliseconds());
    return false;
  }
  os::Logger::logf("[VSDF] emit sdf: done chunks=%zu elapsed=%.1f ms", bakedSdfChunks.size(), (lib::time::TimeSpan::now() - emitStart).milliseconds());

  for (uint32_t meshIndex = 0u; meshIndex < static_cast<uint32_t>(emitMeshes.size()); ++meshIndex)
  {
    const FallbackMeshData &fallback = emitMeshes[meshIndex];
    const VSDFBounds localBounds = bakedSdfChunks[meshIndex].localBounds;
    includeVSDFBounds(scene.sceneBounds, localBounds);

    VSDFMeshEntry mesh;
    mesh.localBounds = localBounds;
    mesh.mipCount = 1u;
    mesh.brickResolution = std::max({bakedSdfChunks[meshIndex].gridResolution.x, bakedSdfChunks[meshIndex].gridResolution.y, bakedSdfChunks[meshIndex].gridResolution.z});
    mesh.narrowBandDistance = std::max(bakedSdfChunks[meshIndex].narrowBandDistance, 1.0e-3f);
    mesh.firstSdfChunk = static_cast<uint32_t>(scene.sdfChunks.size());
    mesh.sdfChunkCount = 1u;
    scene.meshes.push_back(mesh);

    scene.sdfChunks.push_back(std::move(bakedSdfChunks[meshIndex].chunk));

    VSDFInstanceEntry instance;
    instance.meshIndex = meshIndex;
    instance.localToWorld = math::Mat4f::identity();
    instance.worldToLocal = math::Mat4f::identity();
    instance.worldBounds = localBounds;
    scene.instances.push_back(instance);

    VSDFSurfaceCacheMaterial cacheMaterial;
    if (!fallback.vertices.empty())
    {
      const FallbackMeshVertex &vertex = fallback.vertices.front();
      cacheMaterial.albedoOpacity = packRGBA8(vertex.surfaceBaseColorOpacity[0], vertex.surfaceBaseColorOpacity[1], vertex.surfaceBaseColorOpacity[2], vertex.surfaceBaseColorOpacity[3]);
      cacheMaterial.normalRoughness = packRGBA8(vertex.surfaceNormalRoughness[0] * 0.5f + 0.5f, vertex.surfaceNormalRoughness[1] * 0.5f + 0.5f, vertex.surfaceNormalRoughness[2] * 0.5f + 0.5f, vertex.surfaceNormalRoughness[3]);
      cacheMaterial.emissiveMetallic = packRGBA8(vertex.surfaceEmissiveMetallic[0], vertex.surfaceEmissiveMetallic[1], vertex.surfaceEmissiveMetallic[2], vertex.surfaceEmissiveMetallic[3]);
    }

    const uint32_t localMaterialIndex = fallback.sections.empty() ? 0u : fallback.sections.front().materialIndex;
    const std::string materialSourcePath = localMaterialIndex < fallback.materialFiles.size() ? fallback.materialFiles[localMaterialIndex] : std::string{};
    const std::string materialKey = materialSourcePath.empty() ? ("mesh_" + std::to_string(meshIndex) + "_material_" + std::to_string(localMaterialIndex)) : materialSourcePath;
    instanceMaterialIndices.push_back(internMaterial(materialKey, materialSourcePath, cacheMaterial));
    os::Logger::logf(
        "[VSDF] emit mesh %u/%zu: scene records ready chunks=%zu materials=%zu elapsed=%.1f ms",
        meshIndex + 1u,
        emitMeshes.size(),
        scene.sdfChunks.size(),
        scene.materials.size(),
        (lib::time::TimeSpan::now() - emitStart).milliseconds());
  }

  VSDFSurfaceCacheBuildSettings surfaceSettings;
  surfaceSettings.layoutId = 1u;
  surfaceSettings.maxOfflineImageResolution = 512u;
  surfaceSettings.pageSize = 64u;
  surfaceSettings.minCardResolution = 64u;
  surfaceSettings.maxCardResolution = 512u;
  surfaceSettings.texelsPerWorldUnit = 64.0f;
  surfaceSettings.sourceGeometry = true;
  os::Logger::logf(
      "[VSDF] emit cards: begin meshes=%zu instances=%zu materials=%zu elapsed=%.1f ms",
      scene.meshes.size(),
      scene.instances.size(),
      cacheMaterials.size(),
      (lib::time::TimeSpan::now() - emitStart).milliseconds());
  if (!VSDFSurfaceCacheBuilder::buildInstanceCardsFromFallbackMeshes(scene, instanceMaterialIndices, cacheMaterials, emitMeshes, surfaceSettings))
  {
    os::Logger::warningf("[VSDF] emit cards: failed after %.1f ms", (lib::time::TimeSpan::now() - emitStart).milliseconds());
    return false;
  }
  os::Logger::logf(
      "[VSDF] emit cards: done cards=%zu pages=%zu images=%zu elapsed=%.1f ms",
      scene.cards.size(),
      scene.pages.size(),
      scene.shadingImages.size(),
      (lib::time::TimeSpan::now() - emitStart).milliseconds());

  const fs::path vsdfPath = buildRendererVSDFPath(virtualGeometryPath);
  os::Logger::logf(
      "[VSDF] emit save: begin chunks=%zu uncompressedSdfBytes=%llu path=%s",
      scene.sdfChunks.size(),
      static_cast<unsigned long long>(scene.sdfChunks.size()) * 32ull * 32ull * 32ull,
      vsdfPath.string().c_str());
  if (!VSDFFile::save(vsdfPath.string(), scene))
  {
    os::Logger::warningf("[VSDF] emit save: failed after %.1f ms", (lib::time::TimeSpan::now() - emitStart).milliseconds());
    return false;
  }
  os::Logger::logf("[VSDF] emit save: done elapsed=%.1f ms", (lib::time::TimeSpan::now() - emitStart).milliseconds());

  const fs::path imageDirectory = buildRendererVSDFShadingDirectory(virtualGeometryPath);
  os::Logger::logf("[VSDF] emit images: begin directory=%s", imageDirectory.string().c_str());
  (void)VSDFSurfaceCacheBuilder::saveShadingImages(scene, imageDirectory.string(), virtualGeometryPath.stem().string());
  os::Logger::logf("[VSDF] wrote per-instance SDF runtime cache: %s totalElapsed=%.1f ms", vsdfPath.string().c_str(), (lib::time::TimeSpan::now() - emitStart).milliseconds());
  return true;
}

static void ensureRendererVSDFScene(
    const fs::path &assetPath,
    const fs::path &virtualGeometryPath,
    const std::vector<FallbackMeshData> &vsdfMeshes)
{
  const uint32_t expectedMeshCount = static_cast<uint32_t>(vsdfMeshes.size());
  if (expectedMeshCount != 0u && rendererHasVSDFSceneFile(virtualGeometryPath, expectedMeshCount))
    return;

  os::Logger::logf("[VSDF] generating missing per-instance SDF cache: %s", buildRendererVSDFPath(virtualGeometryPath).string().c_str());
  if (!writeRendererVSDFScene(assetPath, virtualGeometryPath, vsdfMeshes))
    os::Logger::warningf("[VSDF] failed to generate per-instance SDF cache for %s", assetPath.string().c_str());
}

static void ensureRendererVSDFShadingImages(const fs::path &virtualGeometryPath)
{
  const fs::path imageDirectory = buildRendererVSDFShadingDirectory(virtualGeometryPath);
  const fs::path sentinel = imageDirectory / (virtualGeometryPath.stem().string() + ".layout1.albedo-opacity.tga");
  if (fs::exists(sentinel))
    return;

  VSDFSceneData scene;
  if (!VSDFFile::load(buildRendererVSDFPath(virtualGeometryPath).string(), scene))
    return;

  if (!VSDFSurfaceCacheBuilder::saveShadingImages(scene, imageDirectory.string(), virtualGeometryPath.stem().string()))
    os::Logger::warningf("[VSDF] failed to export shading image pages for %s", virtualGeometryPath.string().c_str());
}

static bool regenerateRendererVSDFScene(
    const RendererAssetPaths &paths,
    PreparedRendererAsset &prepared)
{
  os::Logger::logf(
      "[VSDF] manual regenerate: asset=%s targetCells=%.0f minCell=%.5f openMax=%u detailMax=%u openBudgetKi=%u detailBudgetKi=%u thinBudgetKi=%u",
      paths.assetPath.string().c_str(),
      gRendererVSDFBuildSettings.targetMajorAxisCells,
      gRendererVSDFBuildSettings.minTargetCellSize,
      gRendererVSDFBuildSettings.openMaxAxisResolution,
      gRendererVSDFBuildSettings.detailOpenMaxAxisResolution,
      gRendererVSDFBuildSettings.openVoxelBudgetKi,
      gRendererVSDFBuildSettings.detailOpenVoxelBudgetKi,
      gRendererVSDFBuildSettings.thinSurfaceVoxelBudgetKi);

  const VirtualGeometrySourceAsset sourceAsset = loadRendererSourceAsset(paths.assetPath, paths.virtualGeometryPath);
  const std::vector<FallbackMeshData> originalVSDFMeshes = FallbackMeshBuilder::buildSDFOriginalPerShape(sourceAsset);
  if (!writeRendererVSDFScene(paths.assetPath, paths.virtualGeometryPath, originalVSDFMeshes))
  {
    os::Logger::warningf("[VSDF] manual regenerate failed for %s", paths.assetPath.string().c_str());
    return false;
  }

  prepared.vsdfPath = buildRendererVSDFPath(paths.virtualGeometryPath);
  prepared.vsdfRuntimeCache = VSDFRuntimeCache{};
  prepared.hasVSDFRuntimeCache = prepared.vsdfRuntimeCache.load(prepared.vsdfPath.string());
  if (!prepared.hasVSDFRuntimeCache)
  {
    os::Logger::warningf("[VSDF] manual regenerate wrote cache but failed to reload runtime data: %s", prepared.vsdfPath.string().c_str());
    return false;
  }

  const auto &info = prepared.vsdfRuntimeCache.getRuntimeInfo();
  os::Logger::logf(
      "[VSDF] manual regenerate loaded runtime cache: instances=%u cards=%u pages=%u sheets=%u",
      info.instanceCount,
      info.cardCount,
      info.pageCount,
      info.sheetCount);
  return true;
}
#endif

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
      const bool missingObjMaterials = isOBJAssetPath(assetPath) && objAssetReferencesMaterialLibrary(assetPath) && (encoded.materialFiles.empty() || !rendererHasUsableOBJMaterials(encoded.materialFiles));
      const uint32_t expectedFallbackMeshCount = std::max<uint32_t>(1u, static_cast<uint32_t>(encoded.shapes.size()));
      const bool missingSoftwareRaytraceScene = !rendererHasSoftwareRaytraceSceneFile(virtualGeometryPath, expectedFallbackMeshCount);
      if (!missingObjMaterials && !missingSoftwareRaytraceScene)
      {
        const VirtualGeometrySourceAsset sourceAsset = loadRendererSourceAsset(assetPath, virtualGeometryPath);
        const std::vector<FallbackMeshData> proxyFallbackMeshes = FallbackMeshBuilder::buildSDFProxyPerShape(sourceAsset);
        if (!rendererFallbackMeshesMatchProxy(virtualGeometryPath, proxyFallbackMeshes))
        {
        os::Logger::logf("[1] Cached virtual geometry has stale software raytrace data, regenerating proxy software RT data: %s", virtualGeometryPath.string().c_str());
          writeRendererFallbackMeshes(virtualGeometryPath, proxyFallbackMeshes);
        }
        return encoded;
      }

      if (missingObjMaterials)
      {
        os::Logger::logf("[1] Cached OBJ virtual geometry has missing or invalid generated materials, rebuilding: %s", virtualGeometryPath.string().c_str());
      }
      else
      {
        const VirtualGeometrySourceAsset sourceAsset = loadRendererSourceAsset(assetPath, virtualGeometryPath);
        const std::vector<FallbackMeshData> proxyFallbackMeshes = FallbackMeshBuilder::buildSDFProxyPerShape(sourceAsset);
        const bool staleSoftwareRaytraceScene = !missingSoftwareRaytraceScene && !rendererFallbackMeshesMatchProxy(virtualGeometryPath, proxyFallbackMeshes);
        if (missingSoftwareRaytraceScene || staleSoftwareRaytraceScene)
        {
          os::Logger::logf("[1] Cached virtual geometry is missing or has stale software raytrace scene data, generating it: %s", virtualGeometryPath.string().c_str());
          writeRendererFallbackMeshes(virtualGeometryPath, proxyFallbackMeshes);
        }
        return encoded;
      }

      os::Logger::logf("[1] Re-encoding virtual geometry asset: %s", virtualGeometryPath.string().c_str());
    }
    else
    {
      os::Logger::logf("[1] Cached virtual geometry is incompatible, rebuilding: %s", virtualGeometryPath.string().c_str());
    }
  }

  os::Logger::logf("[1] Encoding asset: %s", assetPath.string().c_str());

  const VirtualGeometrySourceAsset sourceAsset = loadRendererSourceAsset(assetPath, virtualGeometryPath);
  const VirtualGeometryBuildData build = VirtualGeometryBuilder::build(sourceAsset);

  encoded = VirtualGeometryCompressor::encode(build, quantizationConfig);

  VirtualGeometryFile writer(virtualGeometryPath.string(), /*write=*/true);
  if (!writer.isOpen())
    throw std::runtime_error("Cannot open VG file for writing: " + virtualGeometryPath.string());
  if (!writer.write(encoded, build.pages, MESHLET_LZ4))
    throw std::runtime_error("Failed to write VG file: " + virtualGeometryPath.string());
  const std::vector<FallbackMeshData> proxyFallbackMeshes = FallbackMeshBuilder::buildSDFProxyPerShape(sourceAsset);
  writeRendererFallbackMeshes(virtualGeometryPath, proxyFallbackMeshes);

  return encoded;
}

static PreparedRendererAsset prepareRendererAsset(const RendererAssetPaths &paths, const QuantizationConfig &quantizationConfig)
{
  PreparedRendererAsset prepared;
  prepared.encoded = loadOrBuildVirtualGeometryAsset(paths.assetPath, paths.virtualGeometryPath, quantizationConfig);
  prepared.fallbackMeshPaths = collectRendererFallbackMeshPaths(paths.virtualGeometryPath);
  writeRendererSceneMetaFile(paths, prepared.encoded, prepared.fallbackMeshPaths);
  if (prepared.fallbackMeshPaths.empty())
  {
    os::Logger::warningf("[asset] no software RT scene file was found for %s", paths.virtualGeometryPath.string().c_str());
  }
  else
  {
    os::Logger::logf("[asset] software RT scene file: %s", prepared.fallbackMeshPaths.front().string().c_str());
  }
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

  PreparedRendererAsset preparedAsset = prepareRendererAsset(assetPaths, qcfg);
  const VirtualGeometryEncodedData &encoded = preparedAsset.encoded;

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf(
      "[asset] source=%s vg=%s pages=%u hierarchy=%u animations=%zu fallbackMeshes=%zu",
      assetPaths.assetPath.string().c_str(),
      assetPaths.virtualGeometryPath.string().c_str(),
      pageCount,
      hierarchySize,
      preparedAsset.animationPaths.size(),
      preparedAsset.fallbackMeshPaths.size());
  if (assetPaths.isGLTF && encoded.materialFiles.empty())
  {
    os::Logger::warning("[asset] GLTF source has no generated virtual material files; raster GBuffer Base Color/Normal will use fallback VT-free values until GLTF material VT generation is implemented.");
  }

  // --------------------------------------------------------------------------
  // [2] Window + RHI
  // --------------------------------------------------------------------------
  os::Logger::log("[2] Creating window and Vulkan RHI...");

  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "Virtual Geometry", 1280, 720);
  auto *nativeWindow = static_cast<window::sdl3::SDL3Window *>(window);
  WindowTitleStats windowTitleStats{"Virtual Geometry"};

  DeviceRequiredLimits limits = {0, 0, 0};
  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle | DeviceFeatures::DeviceFeatures_Timestamp |
                            DeviceFeatures::DeviceFeatures_ClipDistances;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {}, launchOptions.enableVulkanValidationLayers);
  os::Logger::logf("[Vulkan] validation layers: %s", launchOptions.enableVulkanValidationLayers ? "enabled" : "disabled");

  {
    auto surfaces = std::vector<VkSurfaceKHR>();
    surfaces.push_back(window->getVulkanSurface(rhi->getInstance()));
    rhi->init(surfaces);
  }

  SwapChain swapChain = rhi->createSwapChain(0, window->getWidth(), window->getHeight());
  const uint32_t maxFramesInFlight = 1; // std::max(1u, rhi->getSwapChainImagesCount(swapChain));

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

  const uint32_t initialDisplayWidth = rhi->getSwapChainImagesWidth(swapChain);
  const uint32_t initialDisplayHeight = rhi->getSwapChainImagesHeight(swapChain);
  RendererRebuildSettings appliedRebuildSettings{};
  RendererRebuildSettings editableRebuildSettings = appliedRebuildSettings;
  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager::Settings sdfSceneManagerSettings{};
  sdfSceneManagerSettings.maxMeshUpdatesPerFrame = 128u;
  gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::Settings sdfTraceDebugSettings{};
  sdf::rendering::SDFTraceDebugPass::Settings sparseSdfTraceDebugSettings{};
  gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass::Settings sdfAtlasDebugSettings{};
  bool sdfFallbackSceneFrustumCullingEnabled = false;

  // --------------------------------------------------------------------------
  const uint32_t kHierarchyBufBytes = std::max(hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
  const uint64_t kPagesBufBytes = 256ull * 1024 * 1024;

  // --------------------------------------------------------------------------
  // [4] Camera
  // --------------------------------------------------------------------------
  os::Logger::log("[4] Configuring camera...\n");
  os::print("[4] Configuring camera...");
  constexpr bool kReverseZ = true;
  constexpr float kClearDepth = 0.0f;
  constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
  const float initialAspect = static_cast<float>(initialDisplayWidth) / static_cast<float>(initialDisplayHeight);
  constexpr float kNear = 0.1f;
  constexpr float kFar = 10000.0f;
  float softwareRayTraceMinDistance = 0.001f;
  float softwareRayTraceMaxDistance = 3000.0f;
  uint32_t softwareRayTraceHeatmapMaxIterations = 128u;
  uint32_t softwareRayTracePrimitiveMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle);
  uint32_t softwareRayTraceTraversalMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance);
  uint32_t softwareRayTraceTlasMode = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH);
  uint32_t softwareRayTraceFallbackLod = softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u;
  uint32_t softwareRayTraceRbvhLod = 0u;
  uint32_t softwareRayTraceShaderVariant = static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant::Auto);
  bool softwareRayTraceOverlayTLASBounds = false;
  bool softwareRayTraceOverlayTLASCwbvhBounds = false;
  uint32_t softwareRayTraceOverlayTLASLevel = 0u;
  float softwareRayTraceOverlayLineThickness = 1.5f;
  bool softwareRayTraceTraversalEarlyOut = false;
  float softwareRayTraceTraversalEarlyOutBias = 0.0f;
  bool softwareRayTraceIsolateTLASInstance = false;
  uint32_t softwareRayTraceSelectedTLASInstance = 0u;
  bool screenSpaceRayTraceEnabled = true;
  bool screenSpaceRayTraceScreenTracesEnabled = true;
  bool screenSpaceRayTraceForceWorldFallback = false;
  bool screenSpaceRayTraceTraceAllForcedFallbackRays = false;
  uint32_t screenSpaceRayTraceTileSize = 1u;
  uint32_t screenSpaceRayTraceRaysPerTile = 1u;
  uint32_t screenSpaceRayTraceMaxSteps = 64u;
  uint32_t screenSpaceRayTraceMaxFailedRays = 1u << 20u;
  bool screenSpaceRayTraceSdfFallbackEnabled = false;
  bool screenSpaceRayTraceSoftwareFallbackEnabled = kRendererEnableSoftwareRayTracingPath;
  bool screenSpaceRayTraceSoftwareFallbackTraceStats = false;
  uint32_t screenSpaceRayTraceSoftwareFallbackBudget = 8192u;
  float screenSpaceRayTraceMinDistance = 0.01f;
  float screenSpaceRayTraceMaxDistance = 3000.0f;
  float screenSpaceRayTraceSurfaceThickness = 0.0015f;
  float screenSpaceRayTraceOriginBias = 0.01f;

  auto syncForcedScreenSpaceFallbackBudget = [&](uint32_t width, uint32_t height)
  {
    if (screenSpaceRayTraceScreenTracesEnabled && !screenSpaceRayTraceTraceAllForcedFallbackRays)
      return;
    const uint32_t forcedBudget = effectiveScreenSpaceRayTraceFallbackBudget(
        true,
        screenSpaceRayTraceSoftwareFallbackBudget,
        width,
        height,
        screenSpaceRayTraceTileSize,
        screenSpaceRayTraceRaysPerTile);
    screenSpaceRayTraceForceWorldFallback = true;
    if (!screenSpaceRayTraceSdfFallbackEnabled && !screenSpaceRayTraceSoftwareFallbackEnabled)
    {
      if (kRendererEnableSoftwareRayTracingPath)
        screenSpaceRayTraceSoftwareFallbackEnabled = true;
      else if (editableRebuildSettings.enableSignedDistanceFields)
        screenSpaceRayTraceSdfFallbackEnabled = true;
    }
    screenSpaceRayTraceSoftwareFallbackBudget = forcedBudget;
    screenSpaceRayTraceMaxFailedRays = forcedBudget;
  };

  constexpr float kMinError = 1.0f;
  float prepassLodErrorThreshold = kMinError;
  float geometryLodErrorThreshold = kMinError;
  float vsmLodErrorThreshold = kMinError;
  float finalSoftwarePixelsPerTriangleThreshold = editableRebuildSettings.finalSoftwarePixelsPerTriangleThreshold;

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
  const float halfFovX = std::atan(std::tan(halfFovY) * initialAspect);
  const float limitingHalfFov = std::min(halfFovX, halfFovY);
  constexpr float kFitPadding = 1.08f;
  const float kCamDist = (kRadius / std::tan(limitingHalfFov)) * kFitPadding;
  const math::Vec3f camPos(objectCentre[0], objectCentre[1], objectCentre[2] + kCamDist);

  rendering::Camera cam(kFovY, camPos, math::Vec3f(0.0f, 0.0f, -1.0f), kReverseZ);
  cam.setAspectRatio(initialAspect);
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
  math::Vec3f previousCameraPosition = camPos;
  math::Quatf cameraRotation = math::Quatf::identity();
  math::Quatf previousCameraRotation = cameraRotation;
  math::Mat4f previousViewMatrix = cam.getViewMatrix();
  math::Mat4f previousProjectionMatrix = cam.getProjectionMatrix();
  math::Vec2f previousTaaJitter(0.0f);
  bool hasPreviousCameraFrame = false;
  bool hasTaaHistory = false;
  bool hasRsmTemporalHistory = false;
  bool hasRsmDenoiserHistory = false;
  float objectPitch = 0.0f;
  float objectYaw = 0.0f;
  math::Quatf objectRotation = math::Quatf::identity();
  const math::Vec3f objectPosition(0.0f, 0.0f, 0.0f);
  math::Vec3f lightDirection = math::Vec3f(-0.35f, -1.0f, -0.25f).normalize();
  math::Vec3f directionalLightColor(1.0f, 1.0f, 1.0f);
  math::Vec3f pointLightColor(1.0f, 0.92f, 0.82f);
  float directionalLightIntensity = 1.0f;
  float pointLightIntensity = 1.0f;
  bool directionalLightEnabled = true;
  bool pointLightEnabled = true;
  float pointLightRange = std::max(kRadius * 2.0f, 8.0f);
  bool hasRsmTemporalPayloadSignature = false;
  math::Quatf previousRsmTemporalObjectRotation = objectRotation;
  math::Vec3f previousRsmTemporalLightDirection = lightDirection;
  math::Vec3f previousRsmTemporalDirectionalColor = directionalLightColor;
  math::Vec3f previousRsmTemporalPointColor = pointLightColor;
  float previousRsmTemporalDirectionalIntensity = directionalLightIntensity;
  float previousRsmTemporalPointIntensity = pointLightIntensity;
  float previousRsmTemporalPointRange = pointLightRange;
  bool previousRsmTemporalDirectionalEnabled = directionalLightEnabled;
  bool previousRsmTemporalPointEnabled = pointLightEnabled;
  uint32_t shadowFilterTaps = 4u;
  uint32_t fallbackShadowFilterTaps = 16u;
  uint32_t pcssBlockerSearchTaps = 5u;
  float directionalShadowBias = 0.0015f;
  float spotShadowBias = 0.0015f;
  float pointShadowBias = 0.0000005f;
  float directionalSlopeScaleBias = 1.0f;
  float spotSlopeScaleBias = 1.0f;
  float pointSlopeScaleBias = 0.5f;
  float directionalMaxShadowBias = 0.006f;
  float spotMaxShadowBias = 0.006f;
  float pointMaxShadowBias = 0.0025f;
  float pcfRadiusTexels = 1.5f;
  float pcssLightRadiusTexels = 4.0f;
  float pcssMaxPenumbraTexels = 12.0f;
  float pcssPenumbraScale = 1.0f;
  float vsmLocalLightMipTransitionScale = 2.0f;
  float vsmLocalLightMipBias = 0.0f;
  bool shadowPcfEnabled = true;
  ScreenSpaceSoftShadowSettings screenSpaceSoftShadowSettings;
  float directionalNormalBiasTexels = 1.0f;
  float spotNormalBiasTexels = 1.0f;
  float pointNormalBiasTexels = 0.25f;
  bool normalLightingEnabled = true;
  float normalLightingThreshold = 0.0f;
  float normalLightingExponent = 1.0f;
  std::array<float, 3> shadowAmbientColor = {0.0f, 0.0f, 0.0f};
  bool screenSpaceShadowEnabled = true;
  float screenSpaceSurfaceThickness = 0.005f;
  float screenSpaceBilinearThreshold = 0.02f;
  float screenSpaceShadowContrast = 4.0f;
  std::array<float, 2> screenSpaceDepthBounds = {0.0f, 1.0f};
  bool screenSpaceIgnoreEdgePixels = false;
  bool screenSpaceUsePrecisionOffset = true;
  bool screenSpaceBilinearSamplingOffsetMode = false;
  bool screenSpaceUseEarlyOut = true;
  bool screenSpaceTreatSkippedEdgeSamplesAsLit = false;
  bool screenSpaceDebugOutputEdgeMask = false;
  bool screenSpaceDebugOutputThreadIndex = false;
  bool screenSpaceDebugOutputWaveIndex = false;
  bool vsmPagesOverlayEnabled = false;
  float vsmPagesOverlayOpacity = 0.45f;
  bool spatialRadianceCacheOverlayEnabled = false;
  float spatialRadianceCacheOverlayOpacity = 0.50f;
  bool ismVplPointsOverlayEnabled = false;
  float ismVplPointsOverlayOpacity = 0.90f;
  bool worldGILightGridOverlayEnabled = false;
  float worldGILightGridOverlayOpacity = 0.55f;
  int worldGILightGridOverlayCascade = -1;
  float worldGILightGridOverlayLineWidth = 0.035f;
  bool screenProbeScreenSpaceTracingEnabled = true;
  float screenProbeRayMaxDistance = 128.0f;
  float screenProbeRayOriginBias = 0.0f;
  uint32_t screenProbeScreenSpaceMaxSteps = 48u;
  float screenProbeScreenSpaceStepPixels = 2.0f;
  float screenProbeScreenSpaceDepthThickness = 0.05f;
  float screenProbeScreenSpaceRelativeThickness = 0.02f;
  bool screenProbeSdfFallbackEnabled = true;
  uint32_t screenProbeSdfMaxSteps = 128u;
  float screenProbeSdfMaxDistance = 64.0f;
  float screenProbeSdfHitThresholdScale = 0.75f;
  float screenProbeSdfEmptySpaceStepScale = 0.85f;
  float screenProbeSdfScreenNormalMatchBaseThreshold = 0.05f;
  float screenProbeSdfScreenNormalMatchCascadeScale = 0.5f;
  bool sdfTraceSurfaceCacheMaterialsEnabled = true;
  bool screenProbeSkyEnabled = false;
  uint32_t screenProbeGuidedCandidateCount = 2u;
  float screenProbeTemporalBlend = 0.90f;
  float screenProbeHistoryWeightScale = 1.25f;
  float screenProbeRecursiveBounceBlend = 1.0f;
  float screenProbeDisocclusionThreshold = 0.35f;
  float screenProbePlaneDistanceThreshold = 0.10f;
  float screenProbeNormalThreshold = 0.90f;
  float screenProbeResolveFirstCacheBias = 1.0f;
  float screenProbeResolveSecondCacheBias = 0.35f;
  float screenProbeResolvePlaneDistanceScale = 12.0f;
  float screenProbeResolveNormalWeightExponent = 8.0f;
  globalillumination::surfels::SurfelHierarchySettings surfelHierarchySettings{};
  globalillumination::surfels::SurfelRuntimeSettings surfelRuntimeSettings{};
  uint32_t surfelAllocatedCount = 0u;
  bool surfelHierarchyFreeze = false;
  bool surfelHierarchyFreezeActive = false;
  math::Vec3f surfelFrozenCameraPosition = cameraPosition;
  math::Quatf surfelFrozenCameraRotation = cameraRotation;
  int vsmDebugLayer = -1;
  int ismVplDebugLayer = -1;
  int ismVplDebugSelectedVpl = -1;
  int ismVplProjectedLightIndex = 0;
  float ismVplDebugPointRadius = 5.0f;
  float ismVplDebugExposure = 8.0f;
  math::Mat4f ismVplDebugViewProjection = math::Mat4f::identity();
  int vsmProjectedLightIndex = 0;

  os::Logger::log(
      "[controls] Middle-drag / Secondary-drag = camera look   Up/Down = camera forward/backward   "
      "WASD = object rotate   L = align directional light to camera");

  SDL_SetWindowRelativeMouseMode(nativeWindow->sdlWindow, false);
  SDL_ShowCursor();
  os::print("[5]");

  rendering::animation::AnimationPlayer animationPlayer(encoded.skeleton);
  bool hasActiveAnimation = false;
  std::string activeAnimationName = preparedAsset.defaultAnimationName;
  float animationTimeSeconds = 0.0f;
  for (const fs::path &animationPath : preparedAsset.animationPaths)
  {
    if (!animationPlayer.loadAnimation(animationPath.string()))
      throw std::runtime_error("Failed to load animation file: " + animationPath.string());
  }
  bool showStatsOverlay = true;
  uint32_t selectedDebugTargetSlot = rendererDisplayOutputTargetSlot(editableRebuildSettings.displayOutput);

  bool restartRendererRequested = true;
  bool recreateSwapChainRequested = false;
  while (!window->shouldClose() && restartRendererRequested)
  {
    restartRendererRequested = false;
    const uint32_t kVW = rhi->getSwapChainImagesWidth(swapChain);
    const uint32_t kVH = rhi->getSwapChainImagesHeight(swapChain);
    const float kAspect = static_cast<float>(kVW) / static_cast<float>(kVH);
    cam.setAspectRatio(kAspect);
    cam.updateMatrices();
    const bool kTaaEnabled = editableRebuildSettings.enableTaa;
    const bool kTsrEnabled = kTaaEnabled && editableRebuildSettings.enableTsr;
    const float kTemporalRenderScale = kTsrEnabled ? editableRebuildSettings.temporalRenderScale : 1.0f;
    const uint32_t kTemporalJitterSamples = editableRebuildSettings.temporalJitterSamples;
    const float kTemporalJitterScale = kTaaEnabled ? editableRebuildSettings.temporalJitterScale : 0.0f;
    const uint32_t kTemporalAccumulationFrames = editableRebuildSettings.taaAccumulationFrames;
    const uint32_t kRenderWidth = computeScaledViewportExtent(kVW, kTemporalRenderScale);
    const uint32_t kRenderHeight = computeScaledViewportExtent(kVH, kTemporalRenderScale);
    const uint32_t kHiZWidth = nextPowerOfTwo(kRenderWidth);
    const uint32_t kHiZHeight = nextPowerOfTwo(kRenderHeight);
    const uint32_t kHiZLvls = computeMipCount(kHiZWidth, kHiZHeight);
    os::Logger::logf(
        "[TSR/TAA] taa=%s tsr=%s display=%ux%u render=%ux%u scale=%.2f jitterSamples=%u jitterScale=%.3f accumulationFrames=%u motionScale=%.3f depthThreshold=%.4f normalThreshold=%.3f reactiveScale=%.2f edgeScale=%.1f "
        "sharpen=%.2f detailBoost=%.2f",
        kTaaEnabled ? "on" : "off",
        kTsrEnabled ? "on" : "off",
        kVW,
        kVH,
        kRenderWidth,
        kRenderHeight,
        kTemporalRenderScale,
        kTemporalJitterSamples,
        kTemporalJitterScale,
        kTemporalAccumulationFrames,
        editableRebuildSettings.taaMotionScale,
        editableRebuildSettings.taaDepthThreshold,
        editableRebuildSettings.taaNormalThreshold,
        editableRebuildSettings.taaReactiveScale,
        editableRebuildSettings.taaEdgeScale,
        editableRebuildSettings.taaSharpenStrength,
        editableRebuildSettings.taaDetailBoost);
    auto renderGraphHolder = std::make_unique<RenderGraph>(
        rhi,
        RenderGraph::Settings{
          .maxTimers = 1024u,
          .maxFramesInFlight = maxFramesInFlight,
        });
    RenderGraph *renderGraph = renderGraphHolder.get();
    renderGraph->addSwapChainImages(swapChain);

    std::vector<Pass *> ownedPasses;
    std::vector<rendering::Texture> ownedTextures;
    std::vector<rendering::Buffer> ownedBuffers;
    std::vector<std::vector<rendering::TextureId>> ownedTextureOverrideIds;
    std::vector<std::vector<rendering::BufferId>> ownedBufferOverrideIds;
    auto ownTexture = [&](Texture texture) -> Texture
    {
      ownedTextures.push_back(texture);
      return texture;
    };
    auto ownBuffer = [&](Buffer buffer) -> Buffer
    {
      ownedBuffers.push_back(buffer);
      return buffer;
    };
    auto ownPass = [&](Pass *pass) -> Pass *
    {
      if (pass != nullptr)
      {
        ownedPasses.push_back(pass);
      }
      return pass;
    };

    // ------------------------------------------------------------------------
    // [3] Scene
    // ------------------------------------------------------------------------
    const bool sdfTraceOutputRequested =
        rendererDisplayOutputTargetSlot(editableRebuildSettings.displayOutput) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace;
    const bool sparseSdfTraceOutputRequested =
        sdfTraceOutputRequested || rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput);
    const bool sdfAtlasOutputRequested =
        rendererDisplayOutputTargetSlot(editableRebuildSettings.displayOutput) == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas;
    const bool sdfSystemRequested =
        editableRebuildSettings.enableSignedDistanceFields ||
        rendererDisplayOutputUsesFallbackScene(editableRebuildSettings.displayOutput) ||
        sdfTraceOutputRequested ||
        sdfAtlasOutputRequested ||
        screenSpaceRayTraceSdfFallbackEnabled;
    os::Logger::log("[3] Building VirtualGeometryScene...\n");
    os::print("[3] Building VirtualGeometryScene...\n");
    VirtualGeometryScene scene(renderGraph, kHierarchyBufBytes, kPagesBufBytes);
    engine::SceneManager sceneManager;
    sceneManager.bindVirtualScene(&scene);
    gpusigneddistancefields::GPUSignedDistanceFieldsManager *gpuSignedDistanceFieldsManager = nullptr;
    gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager *gpuSignedDistanceFieldsSceneManager = nullptr;
    std::unique_ptr<sdf::SDFSceneManager> sparseSdfSceneManager;
    softwarebvh::SoftwareBLASManager *softwareBlasManager = nullptr;
    softwarebvh::SoftwareBLASSceneBridge *softwareBlasSceneBridge = nullptr;
    softwarebvh::SoftwareTLASManager *softwareTlasManager = nullptr;
    softwarebvh::SoftwareTLASSceneBridge *softwareTlasSceneBridge = nullptr;
    if (sdfSystemRequested)
    {
      gpusigneddistancefields::GPUSignedDistanceFieldsManager::Settings sdfManagerSettings{};
      // Give the SDF system a larger atlas and broader per-cascade build budgets for heavy scenes.
      sdfManagerSettings.atlasBrickCapacityPerAxis = 64u;
      sdfManagerSettings.maxBricks = 196608u;
      sdfManagerSettings.maxJobsPerCascade = 8192u;
      sdfManagerSettings.maxReferences = 1u << 20u;
      sdfManagerSettings.maxBricksPerBake = 16384u;
      // The SDF debug/shading path consumes baked fallback vertex attributes
      // through the per-brick-face surface cache.
      sdfManagerSettings.surfaceCachePageResolution = editableRebuildSettings.sdfSurfaceCachePageResolution;
      const float sdfCoarsestCascadeScale = std::pow(sdfManagerSettings.cascadeVoxelSizeScale, static_cast<float>(std::max<uint32_t>(sdfManagerSettings.cascadeCount, 1u) - 1u));
      const float sdfOuterHalfExtent = 0.5f * static_cast<float>(gpusigneddistancefields::GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION) * sdfManagerSettings.firstCascadeVoxelSize * sdfCoarsestCascadeScale;
      const float sdfRequiredOuterHalfExtent = std::max(kCamDist + kRadius, sdfOuterHalfExtent);
      if (sdfRequiredOuterHalfExtent > sdfOuterHalfExtent)
      {
        sdfManagerSettings.firstCascadeVoxelSize = sdfRequiredOuterHalfExtent / (0.5f * static_cast<float>(gpusigneddistancefields::GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION) * sdfCoarsestCascadeScale);
      }

      sceneManager.createSignedDistanceFieldScene(renderGraph, sdfManagerSettings, sdfSceneManagerSettings);
      gpuSignedDistanceFieldsManager = sceneManager.getSignedDistanceFieldManager();
      gpuSignedDistanceFieldsSceneManager = sceneManager.getSignedDistanceFieldSceneManager();
	      sdfTraceDebugSettings.width = kVW;
	      sdfTraceDebugSettings.height = kVH;
	      sdfTraceDebugSettings.surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled;
	      sdfTraceDebugSettings.feedbackRequestCapacity = editableRebuildSettings.materialFeedbackRequestCapacity;
	      sdfTraceDebugSettings.feedbackUploadBudget = editableRebuildSettings.materialMaxUploadsPerFrame;
      sdfTraceDebugSettings.mode = rendererDisplayOutputSdfTraceMode(editableRebuildSettings.displayOutput, sdfTraceDebugSettings.mode);
      if (rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput))
        sdfTraceDebugSettings.maxSteps = std::max(sdfTraceDebugSettings.maxSteps, 256u);
      sparseSdfTraceDebugSettings.width = kVW;
      sparseSdfTraceDebugSettings.height = kVH;
      sparseSdfTraceDebugSettings.mode = rendererDisplayOutputSparseSdfTraceMode(editableRebuildSettings.displayOutput, sparseSdfTraceDebugSettings.mode);
      sparseSdfTraceDebugSettings.maxTraceDistance = sdfTraceDebugSettings.maxTraceDistance;
      sdfAtlasDebugSettings.width = kVW;
      sdfAtlasDebugSettings.height = kVH;
      const uint32_t atlasResolution = gpuSignedDistanceFieldsManager->getSettings().atlasBrickCapacityPerAxis * gpusigneddistancefields::GPUSignedDistanceFieldsManager::BRICK_RESOLUTION;
      if (atlasResolution > 0u)
      {
        sdfAtlasDebugSettings.sliceIndex = std::min(sdfAtlasDebugSettings.sliceIndex, atlasResolution - 1u);
      }
      const float sdfFinalOuterHalfExtent =
          0.5f * static_cast<float>(gpusigneddistancefields::GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION) * gpuSignedDistanceFieldsManager->getSettings().firstCascadeVoxelSize * sdfCoarsestCascadeScale;
      os::Logger::logf(
          "[GPUSignedDistanceFields] Settings: cascades=%u cascadeResolution=%u firstVoxel=%.3f atlas=%ux%ux%u outerHalfExtent=%.3f initialCameraDistance=%.3f sceneRadius=%.3f",
          gpuSignedDistanceFieldsManager->getSettings().cascadeCount,
          gpusigneddistancefields::GPUSignedDistanceFieldsManager::CASCADE_RESOLUTION,
          gpuSignedDistanceFieldsManager->getSettings().firstCascadeVoxelSize,
          atlasResolution,
          atlasResolution,
          atlasResolution,
          sdfFinalOuterHalfExtent,
          kCamDist,
          kRadius);
    }
    if (sparseSdfTraceOutputRequested)
    {
      fs::path sparseVSDFPath;
      if (ensureRendererSparseVSDFScene(assetPaths, &sparseVSDFPath))
      {
        sparseSdfSceneManager = std::make_unique<sdf::SDFSceneManager>(renderGraph);
        std::string error;
        if (!sparseSdfSceneManager->loadVSDF(sparseVSDFPath.string(), &error))
        {
          os::Logger::warningf("[SparseSDF] failed to load sparse VSDF scene: %s (%s)", sparseVSDFPath.string().c_str(), error.c_str());
          sparseSdfSceneManager.reset();
        }
        else
        {
          sparseSdfSceneManager->uploadGPUResources();
          os::Logger::logf(
              "[SparseSDF] loaded sparse VSDF scene: instances=%u blases=%u tlasNodes=%u",
              sparseSdfSceneManager->instanceCount(),
              sparseSdfSceneManager->blasCount(),
              sparseSdfSceneManager->tlasNodeCount());
        }
      }
    }
    const bool useRsmDiffuseGI = editableRebuildSettings.enableDiffuseGI && editableRebuildSettings.diffuseGIOption == DiffuseGIOption::ReflexiveShadowMaps;
    const bool useRsmScreenOutput = rendererDisplayOutputUsesRsmScreenOutput(editableRebuildSettings.displayOutput);
    const bool useRsmScreenPass = useRsmDiffuseGI || useRsmScreenOutput;
    const bool screenSpaceRayTracePathEnabled = !useRsmScreenPass;

    if (kRendererEnableSoftwareRayTracingPath && (editableRebuildSettings.enableSoftwareRayTracing || (screenSpaceRayTracePathEnabled && screenSpaceRayTraceSoftwareFallbackEnabled)))
    {
      sceneManager.createSoftwareRaytraceScene(renderGraph);
      softwareBlasManager = sceneManager.getSoftwareBLASManager();
      softwareBlasSceneBridge = sceneManager.getSoftwareBLASSceneBridge();
      softwareTlasManager = sceneManager.getSoftwareTLASManager();
      softwareTlasSceneBridge = sceneManager.getSoftwareTLASSceneBridge();
    }
    os::Logger::log("[3] Register object\n");
    sceneManager.registerObjectForStreaming(assetPaths.objectName, assetPaths.virtualGeometryPath.string());

    const uint32_t kVSMScreenSpaceShadowScale = editableRebuildSettings.screenSpaceShadowScale;
    const uint32_t kVSMScreenSpaceShadowWidth = (kRenderWidth + kVSMScreenSpaceShadowScale - 1u) / kVSMScreenSpaceShadowScale;
    const uint32_t kVSMScreenSpaceShadowHeight = (kRenderHeight + kVSMScreenSpaceShadowScale - 1u) / kVSMScreenSpaceShadowScale;
    const uint32_t kSoftwareRayTraceResolutionDivisor = std::max(editableRebuildSettings.softwareRayTraceResolutionDivisor, 1u);
    const uint32_t kSoftwareRayTraceWidth = std::max(1u, (kVW + kSoftwareRayTraceResolutionDivisor - 1u) / kSoftwareRayTraceResolutionDivisor);
    const uint32_t kSoftwareRayTraceHeight = std::max(1u, (kVH + kSoftwareRayTraceResolutionDivisor - 1u) / kSoftwareRayTraceResolutionDivisor);
    const float directionalFirstCascadeWorldExtent = editableRebuildSettings.vsmDirectionalFirstCascadeWorldExtentAuto ? std::max(kRadius, 64.0f) : editableRebuildSettings.vsmDirectionalFirstCascadeWorldExtent;
    const bool useIsmVplDiffuseGI = editableRebuildSettings.enableDiffuseGI && editableRebuildSettings.diffuseGIOption == DiffuseGIOption::ImperfectShadowMaps;
    const bool useIsmVplDebugOutput = rendererDisplayOutputUsesIsmVplOutput(editableRebuildSettings.displayOutput);
    const bool useIsmVplPath = !useRsmScreenPass && (useIsmVplDiffuseGI || useIsmVplDebugOutput);
    VirtualShadowMapManager vsmManager(
        renderGraph,
        VirtualShadowMapManager::Settings{
          .virtualPageTableResolution = 32u,
          .physicalAtlasResolution = editableRebuildSettings.vsmPhysicalAtlasResolution,
          .rsmAtlasResolution = editableRebuildSettings.vsmRsmAtlasResolution,
          .physicalPageSize = editableRebuildSettings.vsmPhysicalPageSize,
          .cascadeCount = editableRebuildSettings.vsmCascadeCount,
          .maxDirectionalLights = 1u,
          .maxPointLights = 1u,
          .maxSoftShadowLayers = screenSpaceSoftShadowSettings.maxSoftShadowLayers,
          .maxAllocationRequestsPerFrame = editableRebuildSettings.vsmMaxDirtyPagesPerFrame,
          .maxFutureAllocationRequests = editableRebuildSettings.vsmMaxFallbackPagesPerFrame,
          .firstCascadeWorldExtent = directionalFirstCascadeWorldExtent,
          .cascadeWorldExtentScale = editableRebuildSettings.vsmDirectionalCascadeWorldExtentScale,
          .pageWorldScale = editableRebuildSettings.vsmPageWorldScale,
          .lightDistance = std::max(kRadius, 64.0f),
          .reverseZ = true,
          .rsmPayloadEnabled = useRsmScreenPass,
          .shadowMapType = editableRebuildSettings.vsmShadowMapType,
        });
    const VirtualShadowMapManager::LightId vsmLightId = vsmManager.createDirectionalLight(lightDirection, directionalLightColor, directionalLightIntensity);
    const math::Vec3f pointLightPosition = objectPosition + objectCentre;
    const VirtualShadowMapManager::LightId vsmPointLightId = vsmManager.createPointLight(
        pointLightPosition,
        pointLightRange,
        pointLightColor,
        pointLightIntensity);
    if (useRsmScreenPass)
    {
      vsmManager.queueInvalidateVisiblePages();
    }
    const uint32_t ismVplPageTableResolution = computeIsmVplFacePageTableResolution(editableRebuildSettings.ismGiPagesPerVplFace);
    const uint32_t ismVplActualPagesPerFace = ismVplPageTableResolution * ismVplPageTableResolution;
    const uint32_t ismVplActiveCount = useIsmVplPath && editableRebuildSettings.ismGiEnableVpls ? editableRebuildSettings.ismGiActiveVpls : 0u;
    VirtualShadowMapManager ismVplShadowManager(
        renderGraph,
        VirtualShadowMapManager::Settings{
          .virtualPageTableResolution = ismVplPageTableResolution,
          .physicalAtlasResolution = editableRebuildSettings.ismGiPhysicalAtlasResolution,
          .physicalPageSize = editableRebuildSettings.ismGiPhysicalPageSize,
          .cascadeCount = 1u,
          .maxDirectionalLights = 0u,
          .maxPointLights = std::max(ismVplActiveCount, 1u),
          .maxSoftShadowLayers = std::max(ismVplActiveCount, 1u),
          .maxAllocationRequestsPerFrame = std::max(ismVplActiveCount * VirtualShadowMapManager::POINT_LIGHT_FACE_COUNT * ismVplActualPagesPerFace, 64u),
          .maxFutureAllocationRequests = std::max(ismVplActiveCount * VirtualShadowMapManager::POINT_LIGHT_FACE_COUNT * ismVplActualPagesPerFace * 2u, 128u),
          .firstCascadeWorldExtent = directionalFirstCascadeWorldExtent,
          .cascadeWorldExtentScale = 1.0f,
          .pageWorldScale = 1.0f,
          .lightDistance = std::max(kRadius, 64.0f),
          .reverseZ = true,
          .resourceNamePrefix = "ImperfectShadowMapVPL",
        });
    const std::vector<IsmVplSurfaceTriangle> ismVplSurfaceTriangles = useIsmVplPath ? loadIsmVplSurfaceTriangles(preparedAsset.fallbackMeshPaths) : std::vector<IsmVplSurfaceTriangle>{};
    if (useIsmVplPath)
    {
      os::Logger::logf("[ISM VPL] loaded %zu surface triangles for VPL sampling", ismVplSurfaceTriangles.size());
    }
    IsmVplBuildStats ismVplBuildStats{};
    std::vector<ImperfectShadowMapVPL> ismVpls = buildDirectionalLightSurfaceVPLs(
        ismVplActiveCount,
        ismVplSurfaceTriangles,
        objectPosition,
        objectRotation,
        objectPosition + objectRotation * objectCentre,
        kRadius,
        lightDirection,
        directionalLightColor,
        directionalLightEnabled ? directionalLightIntensity : 0.0f,
        &ismVplBuildStats);
    for (ImperfectShadowMapVPL &vpl : ismVpls)
    {
      const float fluxIntensity = std::max(std::max(vpl.flux[0], vpl.flux[1]), vpl.flux[2]);
      const math::Vec3f vplColor = fluxIntensity > 1e-5f ? vpl.flux * (1.0f / fluxIntensity) : math::Vec3f(1.0f, 1.0f, 1.0f);
      vpl.lightId = ismVplShadowManager.createPointLight(vpl.position, vpl.range, vplColor, fluxIntensity);
    }
    os::Logger::logf(
        "[ISM VPL] active=%u pageTable=%ux%u pagesPerFace=%u atlas=%u pageSize=%u",
        ismVplActiveCount,
        ismVplPageTableResolution,
        ismVplPageTableResolution,
        ismVplActualPagesPerFace,
        editableRebuildSettings.ismGiPhysicalAtlasResolution,
        editableRebuildSettings.ismGiPhysicalPageSize);
    sceneManager.setStreamingResidencyInvalidationCallback(
        [&](const AABB &bounds)
	        {
	          vsmManager.queueInvalidateRegion(bounds);
	          if (useIsmVplPath)
	          {
	            ismVplShadowManager.queueInvalidateRegion(bounds);
	          }
        });

    InstanceId instId = sceneManager.instantiateObjectInstance(assetPaths.objectName, objectPosition, objectRotation, 1.0f);
    sceneManager.updateInstanceBuffer(true);
    if (sparseSdfSceneManager != nullptr)
      sparseSdfSceneManager->syncInstanceTransforms(scene, instId);
    hasActiveAnimation = false;
    if (!activeAnimationName.empty())
    {
      hasActiveAnimation = sceneManager.applyAnimationFrame(instId, animationPlayer, activeAnimationName, animationTimeSeconds, true);
      if (hasActiveAnimation)
      {
        os::Logger::logf("[animation] playing clip: %s", activeAnimationName.c_str());
      }
    }
    if (gpuSignedDistanceFieldsManager != nullptr &&
        gpuSignedDistanceFieldsSceneManager != nullptr &&
        (editableRebuildSettings.enableSignedDistanceFields || rendererDisplayOutputUsesFallbackScene(editableRebuildSettings.displayOutput)))
    {
      sceneManager.syncSignedDistanceFieldScene(cam.getPosition());
      const auto &sdfDiagnostics = gpuSignedDistanceFieldsSceneManager->getDiagnostics();
      os::Logger::logf(
          "[GPUSignedDistanceFields] Warmed fallback scene after graph build: output=%s fallbackMeshes loaded=%u missing=%u prepared=%u staticInstances=%u skinnedInstances=%u uploadedMeshes=%u uploadedVertices=%u uploadedIndices=%u skipped(emptyIds=%u runtime=%u fallback=%u scene=%u bounds=%u)",
          rendererDisplayOutputLabel(editableRebuildSettings.displayOutput),
          sdfDiagnostics.availableFallbackMeshCount,
          sdfDiagnostics.missingFallbackMeshCount,
          sdfDiagnostics.preparedInstanceCount,
          sdfDiagnostics.staticInstanceCount,
          sdfDiagnostics.skinnedInstanceCount,
          sdfDiagnostics.uploadedMeshCount,
          sdfDiagnostics.uploadedVertexCount,
          sdfDiagnostics.uploadedIndexCount,
          sdfDiagnostics.skippedEmptySceneInstanceCount,
          sdfDiagnostics.skippedMissingRuntimeDataCount,
          sdfDiagnostics.skippedMissingFallbackMeshCount,
          sdfDiagnostics.skippedMissingSceneInstanceCount,
          sdfDiagnostics.skippedInvalidBoundsCount);
    }

    // ------------------------------------------------------------------------
    // [5] Render targets
    // ------------------------------------------------------------------------
    os::Logger::log("[5] Creating render targets...");
    os::print("[5] Creating render targets...\n");
    Texture depthTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "DepthTexture",
          .format = Format::Format_Depth32Float,
          .height = kRenderHeight,
          .width = kRenderWidth,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_TransferSrc,
        }));

    auto makeDepthTex = [&](const char *name) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = Format::Format_Depth32Float,
            .height = kRenderHeight,
            .width = kRenderWidth,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_TransferSrc,
          }));
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
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = fmt,
            .height = kRenderHeight,
            .width = kRenderWidth,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
          }));
    };

    auto makeDebugStorageTex = [&](const char *name, Format fmt) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = fmt,
            .height = kRenderHeight,
            .width = kRenderWidth,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage |
                     ImageUsage::ImageUsage_TransferSrc | ImageUsage::ImageUsage_TransferDst,
          }));
    };

    auto makeDisplayDebugStorageTex = [&](const char *name, Format fmt) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = fmt,
            .height = kVH,
            .width = kVW,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
          }));
    };

    auto makeSizedDebugStorageTex = [&](const char *name, Format fmt, uint32_t width, uint32_t height) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = fmt,
            .height = height,
            .width = width,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
            .frameLocal = maxFramesInFlight > 1u,
          }));
    };

    auto makeDisplayDepthTex = [&](const char *name) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = Format::Format_Depth32Float,
            .height = kVH,
            .width = kVW,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_DepthStencilAttachment,
            .frameLocal = maxFramesInFlight > 1u,
          }));
    };

    auto makeSampledDepthCopyTex = [&](const char *name, uint32_t width, uint32_t height) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .format = Format::Format_Depth32Float,
            .height = height,
            .width = width,
            .memoryProperties = BufferUsage::BufferUsage_None,
            .mipLevels = 1u,
            .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_TransferSrc | ImageUsage::ImageUsage_TransferDst,
          }));
    };

    Texture colorTexture = makeDebugStorageTex("MaterialColorTexture", Format::Format_RGBA16Float);
    Texture baseColorTexture = makeDebugStorageTex("MaterialBaseColorTexture", Format::Format_RGBA16Float);
    Texture materialPropertiesTexture = makeDebugStorageTex("MaterialPropertiesTexture", Format::Format_RGBA16Float);
    Texture emissiveTexture = makeDebugStorageTex("MaterialEmissiveTexture", Format::Format_RGBA16Float);
    Texture geometryNormalTexture = makeDebugStorageTex("GeometryNormalTexture", Format::Format_RGBA16Float);
    Texture normalTexture = makeDebugStorageTex("MaterialNormalTexture", Format::Format_RGBA16Float);
    Texture packedGeometryIdsLoTexture = makeColorTex("PackedGeometryIdsLoTexture", Format::Format_R32Uint);
    Texture packedGeometryIdsHiTexture = makeColorTex("PackedGeometryIdsHiTexture", Format::Format_R32Uint);
    Texture materialIdTexture = makeColorTex("MaterialIdTexture", Format::Format_R32Uint);
    Texture materialUVTexture = makeColorTex("MaterialUVTexture", Format::Format_RG32Float);
    Texture motionVectorTexture = makeColorTex("MotionVectorTexture", Format::Format_RG16Float);
    Texture taaColorTexture = makeDisplayDebugStorageTex("TAAColorTexture", Format::Format_RGBA16Float);
    Texture taaDisocclusionMaskTexture = makeDisplayDebugStorageTex("TAADisocclusionMaskTexture", Format::Format_R16Float);
    Texture taaHistoryColorTexture = makeDisplayDebugStorageTex("TAAHistoryColorTexture", Format::Format_RGBA16Float);
    Texture taaHistoryDepthTexture = makeDisplayDebugStorageTex("TAAHistoryDepthTexture", Format::Format_R32Float);
    Texture taaHistoryIdsTexture = makeDisplayDebugStorageTex("TAAHistoryIdsTexture", Format::Format_RG32Uint);
    Texture taaHistoryNormalTexture = makeDisplayDebugStorageTex("TAAHistoryNormalTexture", Format::Format_RGBA16Float);
    const bool useTaaHistoryRing = maxFramesInFlight > 1u;
    Texture taaHistoryColorWriteTexture = taaHistoryColorTexture;
    Texture taaHistoryDepthWriteTexture = taaHistoryDepthTexture;
    Texture taaHistoryIdsWriteTexture = taaHistoryIdsTexture;
    Texture taaHistoryNormalWriteTexture = taaHistoryNormalTexture;
    if (useTaaHistoryRing)
    {
      taaHistoryColorWriteTexture = makeDisplayDebugStorageTex("TAAHistoryColorWriteTexture", Format::Format_RGBA16Float);
      taaHistoryDepthWriteTexture = makeDisplayDebugStorageTex("TAAHistoryDepthWriteTexture", Format::Format_R32Float);
      taaHistoryIdsWriteTexture = makeDisplayDebugStorageTex("TAAHistoryIdsWriteTexture", Format::Format_RG32Uint);
      taaHistoryNormalWriteTexture = makeDisplayDebugStorageTex("TAAHistoryNormalWriteTexture", Format::Format_RGBA16Float);
    }
    Texture spatialRadianceCacheDebugTexture = makeDebugStorageTex("SpatialRadianceCacheDebugTexture", Format::Format_RGBA16Float);
    Texture spatialRadianceReservoirCacheDebugTexture = makeDebugStorageTex("SpatialRadianceReservoirCacheDebugTexture", Format::Format_RGBA16Float);
    Texture surfelHierarchyDebugTexture = makeDebugStorageTex("SurfelHierarchyDebugTexture", Format::Format_RGBA16Float);
    Texture surfelDirectDrawDebugTexture = makeDebugStorageTex("SurfelDirectDrawDebugTexture", Format::Format_RGBA16Float);
    Texture surfelHeatmapDebugTexture = makeDebugStorageTex("SurfelHeatmapDebugTexture", Format::Format_RGBA16Float);
    constexpr uint32_t kSurfelLightingRaysPerSurfel = 2u;
    constexpr uint32_t kSurfelLightingRayOffset = 0u;
    const uint32_t kSurfelLightingRayCount = surfelRuntimeSettings.surfelCapacity * kSurfelLightingRaysPerSurfel;
    Texture worldGIDiffuseSeedTexture = makeDebugStorageTex("WorldGIDiffuseSeedTexture", Format::Format_RGBA16Float);
    Texture worldGIGlossySeedTexture = makeDebugStorageTex("WorldGIGlossySeedTexture", Format::Format_RGBA16Float);
    Texture worldGIDiffuseLightingTexture = makeDebugStorageTex("WorldGIDiffuseLightingTexture", Format::Format_RGBA16Float);
    Texture worldGIGlossyRadianceTexture = makeDebugStorageTex("WorldGIGlossyRadianceTexture", Format::Format_RGBA16Float);
    Texture worldGIIndirectLightingTexture = makeDebugStorageTex("WorldGIIndirectLightingTexture", Format::Format_RGBA16Float);
    Texture worldGILightGridOverlayTexture = makeDebugStorageTex("WorldGILightGridOverlayTexture", Format::Format_RGBA16Float);
    const uint32_t worldGIRayResolutionDivisor = std::max(1u, editableRebuildSettings.worldGIRayResolutionDivisor);
    const uint32_t worldGIRayGridWidth = std::max(1u, (kRenderWidth + worldGIRayResolutionDivisor - 1u) / worldGIRayResolutionDivisor);
    const uint32_t worldGIRayGridHeight = std::max(1u, (kRenderHeight + worldGIRayResolutionDivisor - 1u) / worldGIRayResolutionDivisor);
    const uint32_t worldGIRaysPerPixel = std::clamp(editableRebuildSettings.worldGIRaysPerPixel, 1u, 8u);
    const uint32_t worldGIRayCount = worldGIRayGridWidth * worldGIRayGridHeight * worldGIRaysPerPixel;
    const uint32_t worldGILightGridResolution = std::clamp(editableRebuildSettings.worldGILightGridResolution, 1u, 64u);
    const uint32_t worldGILightGridCascadeCount = std::clamp(editableRebuildSettings.worldGILightGridCascadeCount, 1u, globalillumination::worldgi::kWorldGILightGridMaxCascades);
    const uint32_t worldGILightGridMaxItemsPerCell = std::clamp(editableRebuildSettings.worldGILightGridMaxItemsPerCell, 1u, 256u);
    constexpr uint32_t kWorldGILightCapacity = globalillumination::worldgi::kWorldGILightGridMaxLights;
    Buffer worldGIRayBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGIRayBuffer",
          .size = globalillumination::worldgi::worldGIRayBufferSize(worldGIRayCount),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGISdfRayBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGISdfRayBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getInputRayBufferSize(worldGIRayCount),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGISdfTraceHitBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGISdfTraceHitBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getOutputHitBufferSize(worldGIRayCount),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGITraceShadingBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGITraceShadingBuffer",
          .size = globalillumination::traceShadingParametersBufferSize(worldGIRayCount),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGITraceSurfaceBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGITraceSurfaceBuffer",
          .size = globalillumination::worldgi::giTraceSurfaceBufferSize(worldGIRayCount),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGIFailedRayFallbackBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGIFailedRayFallbackBuffer",
          .size = 32ull + static_cast<uint64_t>(worldGIRayCount) * 48ull,
          .usage = BufferUsage::BufferUsage_Storage,
        }));
    Buffer worldGITraceStatsBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGITraceStatsBuffer",
          .size = gpusigneddistancefields::gpgpu::getTraceStatsBufferSize(),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGILightBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGILightBuffer",
          .size = globalillumination::worldgi::worldGILightBufferSize(kWorldGILightCapacity),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGILightGridCellBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGILightGridCellBuffer",
          .size = globalillumination::worldgi::worldGILightGridCellBufferSize(worldGILightGridResolution, worldGILightGridCascadeCount),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer worldGILightGridItemIdBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldGILightGridItemIdBuffer",
          .size = globalillumination::worldgi::worldGILightGridItemBufferSize(worldGILightGridResolution, worldGILightGridCascadeCount, worldGILightGridMaxItemsPerCell),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture rsmIndirectDiffuseRawTexture = makeDebugStorageTex("RSMIndirectDiffuseRawTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseTexture = makeDebugStorageTex("RSMIndirectDiffuseTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseHistoryTexture = makeDebugStorageTex("RSMIndirectDiffuseHistoryTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseDenoiseTemporalTexture = makeDebugStorageTex("RSMIndirectDiffuseDenoiseTemporalTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseDenoisePingTexture = makeDebugStorageTex("RSMIndirectDiffuseDenoisePingTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseDenoisedTexture = makeDebugStorageTex("RSMIndirectDiffuseDenoisedTexture", Format::Format_RGBA16Float);
    Texture rsmIndirectDiffuseDenoisedHistoryTexture = makeDebugStorageTex("RSMIndirectDiffuseDenoisedHistoryTexture", Format::Format_RGBA16Float);
    Texture rsmDenoiseHistoryDepthTexture = makeSampledDepthCopyTex("RSMDenoiseHistoryDepthTexture", kRenderWidth, kRenderHeight);
    Texture rsmDenoiseHistoryNormalTexture = makeDebugStorageTex("RSMDenoiseHistoryNormalTexture", Format::Format_RGBA16Float);
    Buffer surfelStackBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelStackBuffer",
          .size = globalillumination::surfels::surfelStackBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        }));
    syncForcedScreenSpaceFallbackBudget(kRenderWidth, kRenderHeight);
    const uint32_t screenSpaceRayTraceEffectiveFallbackBudget = effectiveScreenSpaceRayTraceFallbackBudget(
        !screenSpaceRayTraceScreenTracesEnabled || screenSpaceRayTraceTraceAllForcedFallbackRays,
        screenSpaceRayTraceSoftwareFallbackBudget,
        kRenderWidth,
        kRenderHeight,
        screenSpaceRayTraceTileSize,
        screenSpaceRayTraceRaysPerTile);
    const uint32_t screenSpaceRayTraceEffectiveMaxFailedRays = (!screenSpaceRayTraceScreenTracesEnabled || screenSpaceRayTraceTraceAllForcedFallbackRays)
        ? screenSpaceRayTraceEffectiveFallbackBudget
        : std::max(1u, screenSpaceRayTraceMaxFailedRays);
    Buffer worldHitShadingBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "WorldHitShadingBuffer",
          .size = globalillumination::worldHitShadingBufferSize(screenSpaceRayTraceEffectiveFallbackBudget),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer screenSpaceHitShadingBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "ScreenSpaceHitShadingBuffer",
          .size = globalillumination::traceShadingParametersBufferSize(screenSpaceRayTraceEffectiveMaxFailedRays),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    const uint32_t softwareRayTraceDebugRayCount = kSoftwareRayTraceWidth * kSoftwareRayTraceHeight;
    Buffer softwareRayTraceShadingBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SoftwareRayTraceShadingBuffer",
          .size = globalillumination::worldHitShadingBufferSize(softwareRayTraceDebugRayCount),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer softwareRayTraceReceiverRayBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SoftwareRayTraceReceiverRayBuffer",
          .size = 32ull,
          .usage = BufferUsage::BufferUsage_Storage,
        }));
    Buffer screenSpaceRayTraceSdfFallbackRayBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "ScreenSpaceRayTraceSdfFallbackRayBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getInputRayBufferSize(screenSpaceRayTraceEffectiveFallbackBudget),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer screenSpaceRayTraceSdfFallbackHitBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "ScreenSpaceRayTraceSdfFallbackHitBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getOutputHitBufferSize(screenSpaceRayTraceEffectiveFallbackBudget),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer surfelPositionRadiusBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelPositionRadiusBuffer",
          .size = globalillumination::surfels::surfelPositionRadiusBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer surfelNormalLifecycleBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelNormalLifecycleBuffer",
          .size = globalillumination::surfels::surfelNormalLifecycleBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer surfelAttachmentBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelAttachmentBuffer",
          .size = globalillumination::surfels::surfelAttachmentBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer surfelLifecycleBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelLifecycleBuffer",
          .size = globalillumination::surfels::surfelLifecycleBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer surfelMaterialBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelMaterialBuffer",
          .size = globalillumination::surfels::surfelMaterialBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    Buffer surfelIrradianceBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelIrradianceBuffer",
          .size = globalillumination::surfels::surfelIrradianceBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        }));
    std::array<Buffer, 2> surfelReservoirBuffers{};
    std::array<Buffer, 2> surfelDirectReservoirBuffers{};
    for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
    {
      const std::string suffix = surfelFrameIndex == 0u ? "A" : "B";
      surfelReservoirBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelReservoirBuffer" + suffix,
            .size = globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
      surfelDirectReservoirBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelDirectReservoirBuffer" + suffix,
            .size = globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
    }
    Buffer surfelRecycleClaimBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelRecycleClaimBuffer",
          .size = globalillumination::surfels::surfelRecycleClaimBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
        }));
    std::array<Buffer, 2> surfelOwnerCellBuffers{};
    std::array<Buffer, 2> surfelGridBuffers{};
    std::array<Buffer, 2> surfelGridWriteCursorBuffers{};
    std::array<Buffer, 2> surfelListBuffers{};
    std::array<Buffer, 2> surfelPrefixSegmentsBuffers{};
    for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
    {
      const std::string suffix = surfelFrameIndex == 0u ? "A" : "B";
      surfelOwnerCellBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelOwnerCellBuffer" + suffix,
            .size = globalillumination::surfels::surfelOwnerCellBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
      surfelGridBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelGridBuffer" + suffix,
            .size = globalillumination::surfels::surfelGridBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
      surfelGridWriteCursorBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelGridWriteCursorBuffer" + suffix,
            .size = globalillumination::surfels::surfelGridWriteCursorBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
      surfelListBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelListBuffer" + suffix,
            .size = globalillumination::surfels::surfelListBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
      surfelPrefixSegmentsBuffers[surfelFrameIndex] = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = "SurfelPrefixSegmentsBuffer" + suffix,
            .size = globalillumination::surfels::surfelPrefixSegmentsBufferSize(surfelRuntimeSettings),
            .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
          }));
    }
    Buffer surfelStackReadbackBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelStackReadbackBuffer",
          .size = sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        }));
    Buffer surfelIrradianceReadbackBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelIrradianceReadbackBuffer",
          .size = globalillumination::surfels::surfelIrradianceBufferSize(surfelRuntimeSettings),
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        }));

    {
      std::vector<uint32_t> surfelStackInit(1u + std::max(1u, surfelRuntimeSettings.surfelCapacity), 0u);
      surfelStackInit[0] = std::max(1u, surfelRuntimeSettings.surfelCapacity);
      for (uint32_t surfelId = 0u; surfelId < surfelRuntimeSettings.surfelCapacity; ++surfelId)
        surfelStackInit[1u + surfelId] = surfelId;
      renderGraph->bufferWrite(surfelStackBuffer, 0u, surfelStackInit.size() * sizeof(uint32_t), surfelStackInit.data());

      std::vector<uint8_t> zeroBytes;
      auto zeroBuffer = [&](const Buffer &buffer, uint64_t size)
      {
        zeroBytes.assign(static_cast<size_t>(size), 0u);
        renderGraph->bufferWrite(buffer, 0u, size, zeroBytes.data());
      };

      zeroBuffer(surfelPositionRadiusBuffer, globalillumination::surfels::surfelPositionRadiusBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelNormalLifecycleBuffer, globalillumination::surfels::surfelNormalLifecycleBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelAttachmentBuffer, globalillumination::surfels::surfelAttachmentBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelLifecycleBuffer, globalillumination::surfels::surfelLifecycleBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelMaterialBuffer, globalillumination::surfels::surfelMaterialBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelIrradianceBuffer, globalillumination::surfels::surfelIrradianceBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelReservoirBuffers[0u], globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelReservoirBuffers[1u], globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelDirectReservoirBuffers[0u], globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelDirectReservoirBuffers[1u], globalillumination::surfels::surfelReservoirBufferSize(surfelRuntimeSettings));
      zeroBuffer(surfelRecycleClaimBuffer, globalillumination::surfels::surfelRecycleClaimBufferSize(surfelRuntimeSettings));
      for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
      {
        zeroBuffer(surfelOwnerCellBuffers[surfelFrameIndex], globalillumination::surfels::surfelOwnerCellBufferSize(surfelRuntimeSettings));
        zeroBuffer(surfelGridBuffers[surfelFrameIndex], globalillumination::surfels::surfelGridBufferSize(surfelRuntimeSettings));
        zeroBuffer(surfelGridWriteCursorBuffers[surfelFrameIndex], globalillumination::surfels::surfelGridWriteCursorBufferSize(surfelRuntimeSettings));
        zeroBuffer(surfelListBuffers[surfelFrameIndex], globalillumination::surfels::surfelListBufferSize(surfelRuntimeSettings));
        zeroBuffer(surfelPrefixSegmentsBuffers[surfelFrameIndex], globalillumination::surfels::surfelPrefixSegmentsBufferSize(surfelRuntimeSettings));
      }
    }
    Buffer giSdfRayBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "GISdfRayBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getInputRayBufferSize(kSurfelLightingRayCount),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer giSdfTraceHitBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "GISdfTraceHitBuffer",
          .size = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::getOutputHitBufferSize(kSurfelLightingRayCount),
          .usage = BufferUsage::BufferUsage_Storage,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Buffer surfelTraceStatsBuffer = ownBuffer(renderGraph->createBuffer(
        BufferInfo{
          .name = "SurfelTraceStatsBuffer",
          .size = gpusigneddistancefields::gpgpu::getTraceStatsBufferSize(),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
          .frameLocal = maxFramesInFlight > 1u,
        }));

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

    Texture swapChainTexture = ownTexture(renderGraph->createTexture(
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
        }));

    Texture previousDepthHZBTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "PreviousDepthHZBTexture",
          .height = kHiZHeight,
          .width = kHiZWidth,
          .format = Format::Format_R32Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_None,
          .mipLevels = kHiZLvls,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        }));

#ifdef COLLECT_FRAME_STATISTICS
    Texture hardwareFrameStatisticsHeatmapTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "HardwareFrameStatisticsHeatmapTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_ColorAttachment,
        }));
#endif

    Texture virtualShadowMapPagesDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapPagesDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture virtualShadowMapTableDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapTableDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture virtualShadowMapAtlasDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapAtlasDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture imperfectShadowMapVplPagesDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "ImperfectShadowMapVplPagesDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture imperfectShadowMapVplTableDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "ImperfectShadowMapVplTableDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture imperfectShadowMapVplAtlasDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "ImperfectShadowMapVplAtlasDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture imperfectShadowMapVplPointsDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "ImperfectShadowMapVplPointsDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    constexpr uint32_t kVSMClipmapStateDebugSize = 512u;
    Texture virtualShadowMapTableClip0DebugTexture = makeSizedDebugStorageTex("VirtualShadowMapTableClip0DebugTexture", Format::Format_RGBA16Float, kVSMClipmapStateDebugSize, kVSMClipmapStateDebugSize);
    Texture virtualShadowMapTableClip1DebugTexture = makeSizedDebugStorageTex("VirtualShadowMapTableClip1DebugTexture", Format::Format_RGBA16Float, kVSMClipmapStateDebugSize, kVSMClipmapStateDebugSize);
    Texture virtualShadowMapScreenSpaceShadowTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapScreenSpaceShadowTexture",
          .height = kVSMScreenSpaceShadowHeight,
          .width = kVSMScreenSpaceShadowWidth,
          .format = Format::Format_R32Float,
          .depth = 1,
          .arrayLayers = vsmManager.getSettings().maxDirectionalLights,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture virtualShadowMapScreenSpaceSoftShadowDebugTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapScreenSpaceSoftShadowDebugTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture virtualShadowMapShadowLightingTexture = ownTexture(renderGraph->createTexture(
        TextureInfo{
          .name = "VirtualShadowMapShadowLightingTexture",
          .height = kRenderHeight,
          .width = kRenderWidth,
          .format = Format::Format_RGBA16Float,
          .depth = 1,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .mipLevels = 1,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .frameLocal = maxFramesInFlight > 1u,
        }));
    Texture signedDistanceFieldsTraceDebugTexture{};
    Texture signedDistanceFieldsAtlasDebugTexture{};
    Texture signedDistanceFieldsFallbackSceneDebugTexture{};
    Texture signedDistanceFieldsFallbackSceneDepthTexture{};
    Texture softwareRayTraceDebugTexture{};
    Texture softwareRayTraceAlbedoDebugTexture{};
    if (editableRebuildSettings.enableSignedDistanceFields || sdfTraceOutputRequested)
    {
      signedDistanceFieldsTraceDebugTexture = ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = "SignedDistanceFieldsTraceDebugTexture",
            .height = kVH,
            .width = kVW,
            .format = Format::Format_RGBA16Float,
            .depth = 1,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .mipLevels = 1,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .frameLocal = maxFramesInFlight > 1u,
          }));
    }
    if (editableRebuildSettings.enableSignedDistanceFields)
    {
      signedDistanceFieldsAtlasDebugTexture = ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = "SignedDistanceFieldsAtlasDebugTexture",
            .height = kVH,
            .width = kVW,
            .format = Format::Format_RGBA16Float,
            .depth = 1,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .mipLevels = 1,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .frameLocal = maxFramesInFlight > 1u,
          }));
    }
    if (sdfSystemRequested)
    {
      signedDistanceFieldsFallbackSceneDebugTexture = makeDisplayDebugStorageTex("SignedDistanceFieldsFallbackSceneDebugTexture", Format::Format_RGBA16Float);
      signedDistanceFieldsFallbackSceneDepthTexture = makeDisplayDepthTex("SignedDistanceFieldsFallbackSceneDepthTexture");
    }
    const bool allocateSoftwareRayTraceDebugTextures =
        kRendererEnableSoftwareRayTracingPath &&
        (editableRebuildSettings.enableSoftwareRayTracing || screenSpaceRayTraceSoftwareFallbackEnabled);
    if (allocateSoftwareRayTraceDebugTextures)
    {
      softwareRayTraceDebugTexture = makeSizedDebugStorageTex("SoftwareRayTraceDebugTexture", Format::Format_RGBA16Float, kSoftwareRayTraceWidth, kSoftwareRayTraceHeight);
      softwareRayTraceAlbedoDebugTexture = makeSizedDebugStorageTex("SoftwareRayTraceAlbedoDebugTexture", Format::Format_RGBA16Float, kSoftwareRayTraceWidth, kSoftwareRayTraceHeight);
    }
    auto makePerFrameOverrideTextureIds = [&](const std::string &baseName, Format format, BufferUsage memoryProperties, ImageUsage usage, uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t arrayLayers = 1u)
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
    auto makePerFrameOverrideBufferIds = [&](const std::string &baseName, uint64_t size, BufferUsage usage, const void *initialData = nullptr) -> std::vector<BufferId>
    {
      std::vector<BufferId> ids(maxFramesInFlight, BufferId::Invalid);
      for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
      {
        ids[frameSlot] = rhi->createBuffer(
            BufferInfo{
              .name = baseName + ".override_frame" + std::to_string(frameSlot),
              .size = size,
              .usage = usage,
            });
        if (initialData != nullptr)
        {
          rhi->bufferWrite(ids[frameSlot], 0u, size, const_cast<void *>(initialData));
        }
      }
      return ids;
    };
    const std::vector<TextureId> perFrameDepthTextureIds = makePerFrameOverrideTextureIds(
        depthTexture.name, Format::Format_Depth32Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFrameColorTextureIds = makePerFrameOverrideTextureIds(
        colorTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameBaseColorTextureIds = makePerFrameOverrideTextureIds(
        baseColorTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameGeometryNormalTextureIds = makePerFrameOverrideTextureIds(
        geometryNormalTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameNormalTextureIds = makePerFrameOverrideTextureIds(
        normalTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_Storage,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFramePackedGeometryIdsLoTextureIds = makePerFrameOverrideTextureIds(
        packedGeometryIdsLoTexture.name, Format::Format_R32Uint, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFramePackedGeometryIdsHiTextureIds = makePerFrameOverrideTextureIds(
        packedGeometryIdsHiTexture.name, Format::Format_R32Uint, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFrameMaterialIdTextureIds = makePerFrameOverrideTextureIds(
        materialIdTexture.name, Format::Format_R32Uint, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFrameMaterialUVTextureIds = makePerFrameOverrideTextureIds(
        materialUVTexture.name, Format::Format_RG32Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFrameMotionVectorTextureIds = makePerFrameOverrideTextureIds(
        motionVectorTexture.name, Format::Format_RG16Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled, kRenderWidth, kRenderHeight, 1u);
    const std::vector<TextureId> perFrameTaaColorTextureIds =
        makePerFrameOverrideTextureIds(taaColorTexture.name, Format::Format_RGBA16Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
    const std::vector<TextureId> perFrameTaaDisocclusionMaskTextureIds =
        makePerFrameOverrideTextureIds(taaDisocclusionMaskTexture.name, Format::Format_R16Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
    std::vector<TextureId> perFrameTaaHistoryColorTextureIds;
    std::vector<TextureId> perFrameTaaHistoryDepthTextureIds;
    std::vector<TextureId> perFrameTaaHistoryIdsTextureIds;
    std::vector<TextureId> perFrameTaaHistoryNormalTextureIds;
    if (useTaaHistoryRing)
    {
      perFrameTaaHistoryColorTextureIds =
          makePerFrameOverrideTextureIds(taaHistoryColorTexture.name, Format::Format_RGBA16Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
      perFrameTaaHistoryDepthTextureIds =
          makePerFrameOverrideTextureIds(taaHistoryDepthTexture.name, Format::Format_R32Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
      perFrameTaaHistoryIdsTextureIds =
          makePerFrameOverrideTextureIds(taaHistoryIdsTexture.name, Format::Format_RG32Uint, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
      perFrameTaaHistoryNormalTextureIds =
          makePerFrameOverrideTextureIds(taaHistoryNormalTexture.name, Format::Format_RGBA16Float, BufferUsage::BufferUsage_None, ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled, kVW, kVH, 1u);
    }
    const std::vector<TextureId> perFrameSpatialRadianceCacheDebugTextureIds = makePerFrameOverrideTextureIds(
        spatialRadianceCacheDebugTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_ColorAttachment,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameSpatialRadianceReservoirCacheDebugTextureIds = makePerFrameOverrideTextureIds(
        spatialRadianceReservoirCacheDebugTexture.name,
        Format::Format_RGBA16Float,
        BufferUsage::BufferUsage_None,
        ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled | ImageUsage::ImageUsage_ColorAttachment,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFramePreviousDepthHZBTextureIds = makePerFrameOverrideTextureIds(
        previousDepthHZBTexture.name,
        Format::Format_R32Float,
        rendering::BufferUsage::BufferUsage_None,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kHiZWidth,
        kHiZHeight,
        kHiZLvls);
    const std::vector<TextureId> perFrameVsmPagesDebugTextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapPagesDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameVsmTableDebugTextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapTableDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameIsmVplPagesDebugTextureIds = makePerFrameOverrideTextureIds(
        imperfectShadowMapVplPagesDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameIsmVplTableDebugTextureIds = makePerFrameOverrideTextureIds(
        imperfectShadowMapVplTableDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameIsmVplAtlasDebugTextureIds = makePerFrameOverrideTextureIds(
        imperfectShadowMapVplAtlasDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameIsmVplPointsDebugTextureIds = makePerFrameOverrideTextureIds(
        imperfectShadowMapVplPointsDebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameVsmShadowLightingTextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapShadowLightingTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kRenderWidth,
        kRenderHeight,
        1u);
    const std::vector<TextureId> perFrameVsmScreenSpaceShadowTextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapScreenSpaceShadowTexture.name,
        Format::Format_R32Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kVSMScreenSpaceShadowWidth,
        kVSMScreenSpaceShadowHeight,
        1u,
        vsmManager.getSettings().maxDirectionalLights);
    const std::vector<TextureId> perFrameVsmTableClip0TextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapTableClip0DebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kVSMClipmapStateDebugSize,
        kVSMClipmapStateDebugSize,
        1u);
    const std::vector<TextureId> perFrameVsmTableClip1TextureIds = makePerFrameOverrideTextureIds(
        virtualShadowMapTableClip1DebugTexture.name,
        Format::Format_RGBA16Float,
        rendering::BufferUsage::BufferUsage_Storage,
        rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
        kVSMClipmapStateDebugSize,
        kVSMClipmapStateDebugSize,
        1u);
    ownedTextureOverrideIds.push_back(perFrameDepthTextureIds);
    ownedTextureOverrideIds.push_back(perFrameColorTextureIds);
    ownedTextureOverrideIds.push_back(perFrameBaseColorTextureIds);
    ownedTextureOverrideIds.push_back(perFrameGeometryNormalTextureIds);
    ownedTextureOverrideIds.push_back(perFrameNormalTextureIds);
    ownedTextureOverrideIds.push_back(perFramePackedGeometryIdsLoTextureIds);
    ownedTextureOverrideIds.push_back(perFramePackedGeometryIdsHiTextureIds);
    ownedTextureOverrideIds.push_back(perFrameMaterialIdTextureIds);
    ownedTextureOverrideIds.push_back(perFrameMaterialUVTextureIds);
    ownedTextureOverrideIds.push_back(perFrameMotionVectorTextureIds);
    ownedTextureOverrideIds.push_back(perFrameTaaColorTextureIds);
    ownedTextureOverrideIds.push_back(perFrameTaaDisocclusionMaskTextureIds);
    if (useTaaHistoryRing)
    {
      ownedTextureOverrideIds.push_back(perFrameTaaHistoryColorTextureIds);
      ownedTextureOverrideIds.push_back(perFrameTaaHistoryDepthTextureIds);
      ownedTextureOverrideIds.push_back(perFrameTaaHistoryIdsTextureIds);
      ownedTextureOverrideIds.push_back(perFrameTaaHistoryNormalTextureIds);
    }
    ownedTextureOverrideIds.push_back(perFrameSpatialRadianceCacheDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameSpatialRadianceReservoirCacheDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFramePreviousDepthHZBTextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmPagesDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmTableDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameIsmVplPagesDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameIsmVplTableDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameIsmVplAtlasDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameIsmVplPointsDebugTextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmShadowLightingTextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmScreenSpaceShadowTextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmTableClip0TextureIds);
    ownedTextureOverrideIds.push_back(perFrameVsmTableClip1TextureIds);
    VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
      .depthView = depthView,
      .depthTexture = depthTexture,
      .packedGeometryIdsLoView = makeColorView(packedGeometryIdsLoTexture),
      .packedGeometryIdsHiView = makeColorView(packedGeometryIdsHiTexture),
      .materialIdView = makeColorView(materialIdTexture),
      .materialUVView = makeColorView(materialUVTexture),
      .baseColorView = makeColorView(baseColorTexture),
      .materialPropertiesView = makeColorView(materialPropertiesTexture),
      .emissiveView = makeColorView(emissiveTexture),
      .baseColorTexture = baseColorTexture,
      .materialPropertiesTexture = materialPropertiesTexture,
      .emissiveTexture = emissiveTexture,
      .geometryNormalView = makeColorView(geometryNormalTexture),
      .geometryNormalTexture = geometryNormalTexture,
      .normalView = makeColorView(normalTexture),
      .normalTexture = normalTexture,
      .motionVectorView = makeColorView(motionVectorTexture),
      .colorView = makeColorView(colorTexture),
      .colorTexture = colorTexture,
      .motionVectorTexture = motionVectorTexture,
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
    prepassCullingSettings.maxHierarchyLevels = editableRebuildSettings.prepassMaxHierarchyLevels;
    prepassCullingSettings.clustersQueueBufferSize = editableRebuildSettings.prepassClusterQueueMiB * 1024u * 1024u;
    prepassCullingSettings.errorTreshhold = prepassLodErrorThreshold;
    prepassCullingSettings.viewPortWidth = kRenderWidth;
    prepassCullingSettings.viewPortHeight = kRenderHeight;
    prepassCullingSettings.hiZMipLevels = kHiZLvls;
    prepassCullingSettings.useExternalHiZTexture = true;
    prepassCullingSettings.enableFrustumCulling = editableRebuildSettings.prepassEnableFrustumCulling;
    prepassCullingSettings.enableOcclusionCulling = editableRebuildSettings.prepassEnableOcclusionCulling;

    VirtualGeometryCullingMultipleDispatchesPass::Settings cullingSettings;
    cullingSettings.maxHierarchyLevels = editableRebuildSettings.finalMaxHierarchyLevels;
    cullingSettings.clustersQueueBufferSize = editableRebuildSettings.finalClusterQueueMiB * 1024u * 1024u;
    cullingSettings.errorTreshhold = geometryLodErrorThreshold;
    cullingSettings.viewPortWidth = kRenderWidth;
    cullingSettings.viewPortHeight = kRenderHeight;
    cullingSettings.hiZMipLevels = kHiZLvls;
    cullingSettings.useExternalHiZTexture = true;
    cullingSettings.enableFrustumCulling = editableRebuildSettings.finalEnableFrustumCulling;
    cullingSettings.enableOcclusionCulling = editableRebuildSettings.finalEnableOcclusionCulling;
    cullingSettings.enableStreamingPriorityUpdates = editableRebuildSettings.finalEnableStreamingPriorityUpdates;
    cullingSettings.swPixelsPerTriangleThreshold = finalSoftwarePixelsPerTriangleThreshold;

    VirtualGeometryHardwareDrawPass::Settings hwSettings;
    hwSettings.viewPortWidth = kRenderWidth;
    hwSettings.viewPortHeight = kRenderHeight;
    hwSettings.colorFormat = rhi->getSwapChainFormat(swapChain);
    hwSettings.motionVectorFormat = Format::Format_RG16Float;
    hwSettings.depthFormat = Format::Format_Depth32Float;
    hwSettings.maxDrawnClusters = editableRebuildSettings.hardwareDrawMaxDrawnClusters;

    VirtualGeometryDepthPrePassDrawPass::Settings depthPrePassSettings;
    depthPrePassSettings.viewPortWidth = kRenderWidth;
    depthPrePassSettings.viewPortHeight = kRenderHeight;
    depthPrePassSettings.maxDrawnClusters = editableRebuildSettings.depthPrePassMaxDrawnClusters;
    depthPrePassSettings.depthFormat = Format::Format_Depth32Float;

    VirtualGeometryDepthPyramidPass::Settings depthPyramidSettings;
    depthPyramidSettings.width = kRenderWidth;
    depthPyramidSettings.height = kRenderHeight;
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
    rendererSettings.materialPassSettings.viewPortWidth = kRenderWidth;
    rendererSettings.materialPassSettings.viewPortHeight = kRenderHeight;
    rendererSettings.materialPassSettings.outputViewPortWidth = kVW;
    rendererSettings.materialPassSettings.outputViewPortHeight = kVH;
    rendererSettings.materialPassSettings.colorFormat = Format::Format_RGBA16Float;
    rendererSettings.materialPassSettings.depthFormat = Format::Format_Depth32Float;
    rendererSettings.materialPassSettings.feedbackRequestCapacity = editableRebuildSettings.materialFeedbackRequestCapacity;
    rendererSettings.materialPassSettings.feedbackStride = editableRebuildSettings.materialFeedbackStride;
    rendererSettings.materialPassSettings.maxUploadsPerFrame = editableRebuildSettings.materialMaxUploadsPerFrame;
    rendererSettings.materialPassSettings.tileSize = editableRebuildSettings.materialTileSize;
    rendererSettings.materialPassSettings.tileWorkgroupSize = editableRebuildSettings.materialTileWorkgroupSize;
    rendererSettings.materialPassSettings.temporalRenderScale = kTemporalRenderScale;
    rendererSettings.materialPassSettings.shaderOverridePath = rendererDisplayOutputMaterialShaderOverride(editableRebuildSettings.displayOutput);
    rendererSettings.screenSpaceRayTraceSettings.enabled = screenSpaceRayTracePathEnabled && screenSpaceRayTraceEnabled;
    rendererSettings.screenSpaceRayTraceSettings.screenTraceEnabled = screenSpaceRayTracePathEnabled && screenSpaceRayTraceScreenTracesEnabled;
    rendererSettings.screenSpaceRayTraceSettings.forceWorldFallback = screenSpaceRayTracePathEnabled && (screenSpaceRayTraceForceWorldFallback || !screenSpaceRayTraceScreenTracesEnabled);
    rendererSettings.screenSpaceRayTraceSettings.width = kRenderWidth;
    rendererSettings.screenSpaceRayTraceSettings.height = kRenderHeight;
    rendererSettings.screenSpaceRayTraceSettings.hzbWidth = kHiZWidth;
    rendererSettings.screenSpaceRayTraceSettings.hzbHeight = kHiZHeight;
    rendererSettings.screenSpaceRayTraceSettings.hzbMipCount = kHiZLvls;
    rendererSettings.screenSpaceRayTraceSettings.tileSize = screenSpaceRayTraceTileSize;
    rendererSettings.screenSpaceRayTraceSettings.raysPerTile = screenSpaceRayTraceRaysPerTile;
    rendererSettings.screenSpaceRayTraceSettings.maxSteps = screenSpaceRayTraceMaxSteps;
    if (screenSpaceRayTracePathEnabled && (!screenSpaceRayTraceScreenTracesEnabled || screenSpaceRayTraceTraceAllForcedFallbackRays))
    {
      syncForcedScreenSpaceFallbackBudget(kRenderWidth, kRenderHeight);
      rendererSettings.screenSpaceRayTraceSettings.forceWorldFallback = true;
      rendererSettings.screenSpaceRayTraceSettings.screenTraceEnabled = screenSpaceRayTraceScreenTracesEnabled;
    }
    rendererSettings.screenSpaceRayTraceSettings.maxFailedRays = screenSpaceRayTraceEffectiveMaxFailedRays;
    rendererSettings.screenSpaceRayTraceSettings.minTraceDistance = screenSpaceRayTraceMinDistance;
    rendererSettings.screenSpaceRayTraceSettings.maxTraceDistance = screenSpaceRayTraceMaxDistance;
    rendererSettings.screenSpaceRayTraceSettings.surfaceThickness = screenSpaceRayTraceSurfaceThickness;
    rendererSettings.screenSpaceRayTraceSettings.originBias = screenSpaceRayTraceOriginBias;
    rendererSettings.registerScreenSpaceRayTracePass = screenSpaceRayTracePathEnabled;
    rendererSettings.registerMaterialPass = false;

    auto *rendererPass = static_cast<VirtualGeometryRendererPass *>(ownPass(renderGraph->registerPass<VirtualGeometryRendererPass>(
        "virtualGeometryRendererPass", 0, scene, frameTarget, prePassTarget, virtualShadowMapShadowLightingTexture, previousDepthHZBTexture, kClearDepth, rendererSettings)));
    auto *screenSpaceRayTracePass = rendererPass->getScreenSpaceRayTracePass();
    VirtualShadowMapPass::Settings vsmPassSettings;
    vsmPassSettings.bookkeepingSettings.depthTextureWidth = kRenderWidth;
    vsmPassSettings.bookkeepingSettings.depthTextureHeight = kRenderHeight;
    vsmPassSettings.enableTableDebugPass = editableRebuildSettings.vsmEnableTableDebugPass || editableRebuildSettings.displayOutput == RendererDisplayOutput::VsmTable;
    vsmPassSettings.tableDebugSettings.width = kRenderWidth;
    vsmPassSettings.tableDebugSettings.height = kRenderHeight;
    vsmPassSettings.tableDebugSettings.debugLayer = vsmDebugLayer;
    vsmPassSettings.enableAtlasDebugPass = editableRebuildSettings.vsmEnableAtlasDebugPass || editableRebuildSettings.displayOutput == RendererDisplayOutput::VsmAtlas;
    vsmPassSettings.atlasDebugSettings.width = kRenderWidth;
    vsmPassSettings.atlasDebugSettings.height = kRenderHeight;
    vsmPassSettings.atlasDebugSettings.debugLayer = vsmDebugLayer;
    auto configureBookkeepingShadowSettings = [&](auto &settings)
    {
      settings.shadowFilterTaps = shadowPcfEnabled ? shadowFilterTaps : 1u;
      settings.pcfRadiusTexels = pcfRadiusTexels;
      settings.localLightMipTransitionScale = vsmLocalLightMipTransitionScale;
      settings.localLightMipBias = vsmLocalLightMipBias;
      settings.projectedLightIndex = vsmProjectedLightIndex;
    };
    auto configureShadowSettings = [&](auto &settings)
    {
      settings.directionalShadowBias = directionalShadowBias;
      settings.spotShadowBias = spotShadowBias;
      settings.pointShadowBias = pointShadowBias;
      settings.directionalSlopeScaleBias = directionalSlopeScaleBias;
      settings.spotSlopeScaleBias = spotSlopeScaleBias;
      settings.pointSlopeScaleBias = pointSlopeScaleBias;
      settings.directionalMaxShadowBias = directionalMaxShadowBias;
      settings.spotMaxShadowBias = spotMaxShadowBias;
      settings.pointMaxShadowBias = pointMaxShadowBias;
      settings.shadowFilterTaps = shadowPcfEnabled ? shadowFilterTaps : 1u;
      settings.pcfRadiusTexels = pcfRadiusTexels;
      settings.fallbackShadowFilterTaps = shadowPcfEnabled ? fallbackShadowFilterTaps : 1u;
      settings.fallbackPcfRadiusTexels = pcfRadiusTexels;
      settings.pcssBlockerSearchTaps = pcssBlockerSearchTaps;
      settings.pcssLightRadiusTexels = pcssLightRadiusTexels;
      settings.pcssMaxPenumbraTexels = pcssMaxPenumbraTexels;
      settings.pcssPenumbraScale = pcssPenumbraScale;
      settings.directionalNormalBiasTexels = directionalNormalBiasTexels;
      settings.spotNormalBiasTexels = spotNormalBiasTexels;
      settings.pointNormalBiasTexels = pointNormalBiasTexels;
      settings.normalLightingEnabled = normalLightingEnabled;
      settings.normalLightingThreshold = normalLightingThreshold;
      settings.normalLightingExponent = normalLightingExponent;
      settings.ambientShadowColor = shadowAmbientColor;
      settings.contactShadowSamples = 0u;
      settings.contactShadowDistance = 24.0f;
      settings.contactShadowThickness = 2.0f;
      settings.contactShadowIntensity = 1.0f;
      settings.contactShadowStartBias = 1.0f;
      settings.localLightMipTransitionScale = vsmLocalLightMipTransitionScale;
      settings.localLightMipBias = vsmLocalLightMipBias;
      settings.screenSpaceShadowEnabled = screenSpaceShadowEnabled;
      settings.softShadowMethod = editableRebuildSettings.softShadowMethod;
    };
    configureBookkeepingShadowSettings(vsmPassSettings.bookkeepingSettings);
    auto configureScreenSpaceShadowSettings = [&](auto &settings)
    {
      settings.width = kVSMScreenSpaceShadowWidth;
      settings.height = kVSMScreenSpaceShadowHeight;
      settings.surfaceThickness = screenSpaceSurfaceThickness;
      settings.bilinearThreshold = screenSpaceBilinearThreshold;
      settings.shadowContrast = screenSpaceShadowContrast;
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
    vsmPassSettings.softShadowMethod = editableRebuildSettings.softShadowMethod;
    vsmPassSettings.screenSpaceSoftShadowSettings.width = kRenderWidth;
    vsmPassSettings.screenSpaceSoftShadowSettings.height = kRenderHeight;
    vsmPassSettings.screenSpaceSoftShadowSettings.resolutionScale = sanitizeScreenSpaceSoftShadowResolutionScale(screenSpaceSoftShadowSettings.resolutionScale);
    vsmPassSettings.screenSpaceSoftShadowSettings.maxSoftShadowLayers = std::max(1u, screenSpaceSoftShadowSettings.maxSoftShadowLayers);
    vsmPassSettings.screenSpaceSoftShadowSettings.kernelSize = sanitizeScreenSpaceSoftShadowKernelSize(screenSpaceSoftShadowSettings.kernelSize);
    vsmPassSettings.screenSpaceSoftShadowSettings.blockerSearchTaps = sanitizeScreenSpaceSoftShadowBlockerSearchTaps(screenSpaceSoftShadowSettings.blockerSearchTaps);
    vsmPassSettings.screenSpaceSoftShadowSettings.useMinFilter = screenSpaceSoftShadowSettings.useMinFilter;
    vsmPassSettings.screenSpaceSoftShadowSettings.defaultLightRadiusWorld = screenSpaceSoftShadowSettings.defaultLightRadiusWorld;
    vsmPassSettings.screenSpaceSoftShadowSettings.penumbraScale = screenSpaceSoftShadowSettings.penumbraScale;
    vsmPassSettings.screenSpaceSoftShadowSettings.grazingThreshold = screenSpaceSoftShadowSettings.grazingThreshold;
    vsmPassSettings.screenSpaceSoftShadowSettings.depthRejectThreshold = screenSpaceSoftShadowSettings.depthRejectThreshold;
    vsmPassSettings.screenSpaceSoftShadowSettings.translucentShadowsEnabled = screenSpaceSoftShadowSettings.translucentShadowsEnabled;
    vsmPassSettings.screenSpaceSoftShadowSettings.debugMode = rendererDisplayOutputSsssDebugMode(editableRebuildSettings.displayOutput, screenSpaceSoftShadowSettings.debugMode);
    vsmPassSettings.screenSpaceSoftShadowSettings.debugLayer = screenSpaceSoftShadowSettings.debugLayer;
    vsmPassSettings.screenSpaceSoftShadowSettings.debugValueScale = screenSpaceSoftShadowSettings.debugValueScale;
    vsmPassSettings.screenSpaceSoftShadowSettings.directionalShadowBias = directionalShadowBias;
    vsmPassSettings.screenSpaceSoftShadowSettings.spotShadowBias = spotShadowBias;
    vsmPassSettings.screenSpaceSoftShadowSettings.pointShadowBias = pointShadowBias;
    vsmPassSettings.screenSpaceSoftShadowSettings.localLightMipTransitionScale = vsmLocalLightMipTransitionScale;
    vsmPassSettings.screenSpaceSoftShadowSettings.localLightMipBias = vsmLocalLightMipBias;
    vsmPassSettings.drawSettings.maxHierarchyLevels = editableRebuildSettings.finalMaxHierarchyLevels;
    vsmPassSettings.drawSettings.hierarchyQueueCapacity = editableRebuildSettings.vsmHierarchyQueueCapacity;
    vsmPassSettings.drawSettings.clusterQueueCapacity = editableRebuildSettings.vsmClusterQueueCapacity;
    vsmPassSettings.drawSettings.maxVisibleClusterDraws = editableRebuildSettings.vsmMaxVisibleClusterDraws;
    vsmPassSettings.drawSettings.scratchClipmapCount = editableRebuildSettings.vsmScratchClipmapCount;
    vsmPassSettings.drawSettings.rasterMode = editableRebuildSettings.vsmRasterMode;
    vsmPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
    vsmPassSettings.shadowPcfSettings.width = kRenderWidth;
    vsmPassSettings.shadowPcfSettings.height = kRenderHeight;
    configureShadowSettings(vsmPassSettings.shadowPcfSettings);
    auto *vsmPass = renderGraph->registerPass<VirtualShadowMapPass>(
        "virtualShadowMapPass",
        31,
        scene,
        vsmManager,
        rendererPass->getSceneDepthTexture(),
        rendererPass->getGeometryNormalTexture(),
        virtualShadowMapPagesDebugTexture,
        virtualShadowMapTableDebugTexture,
        virtualShadowMapAtlasDebugTexture,
        virtualShadowMapScreenSpaceShadowTexture,
        virtualShadowMapScreenSpaceSoftShadowDebugTexture,
        virtualShadowMapShadowLightingTexture,
        vsmPassSettings);
	    ownPass(vsmPass);
    constexpr uint32_t kRsmGiResolvePassIndex = 70u;
    constexpr uint32_t kRsmGiTemporalResolvePassIndex = 71u;
    constexpr uint32_t kRsmGiHistoryCopyPassIndex = 72u;
    constexpr uint32_t kRsmGiDenoisePassIndex = 73u;
    constexpr uint32_t kRsmGiAtrousPassCount = 16u;
    constexpr uint32_t kRsmGiAtrousPassBaseIndex = 74u;
    constexpr uint32_t kRsmGiDenoiseHistoryCopyPassIndex = kRsmGiAtrousPassBaseIndex + kRsmGiAtrousPassCount;
    constexpr uint32_t kRsmGiDenoiseDepthHistoryCopyPassIndex = kRsmGiDenoiseHistoryCopyPassIndex + 1u;
    constexpr uint32_t kRsmGiDenoiseNormalHistoryCopyPassIndex = kRsmGiDenoiseDepthHistoryCopyPassIndex + 1u;

	    VirtualShadowMapRsmIndirectDiffusePass *rsmIndirectDiffusePass = nullptr;
	    if (useRsmScreenPass)
	    {
	      rsmIndirectDiffusePass = static_cast<VirtualShadowMapRsmIndirectDiffusePass *>(ownPass(renderGraph->registerPass<VirtualShadowMapRsmIndirectDiffusePass>(
	          "virtualShadowMapRsmIndirectDiffusePass",
	          kRsmGiResolvePassIndex,
	          vsmManager,
          rendererPass->getSceneDepthTexture(),
          rendererPass->getGeometryNormalTexture(),
	          rsmIndirectDiffuseRawTexture,
          VirtualShadowMapRsmIndirectDiffusePass::Settings{
            .width = kRenderWidth,
            .height = kRenderHeight,
            .sampleCount = std::clamp(editableRebuildSettings.rsmGiSampleCount, 1u, 64u),
            .frameIndex = 0u,
            .debugMode = rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput),
            .sampleRadiusWorld = editableRebuildSettings.rsmGiSampleRadiusMeters,
            .intensity = editableRebuildSettings.rsmGiIntensity,
            .normalBias = editableRebuildSettings.rsmGiNormalBias,
            .minDistance = editableRebuildSettings.rsmGiMinDistance,
            .maxDistance = editableRebuildSettings.rsmGiMaxDistance,
            .localLightMipTransitionScale = vsmLocalLightMipTransitionScale,
            .localLightMipBias = vsmLocalLightMipBias,
            .fireflyClamp = editableRebuildSettings.rsmGiFireflyClamp,
            .enabled = true,
          })));
    }
    else
    {
      rendering::passes::ColorQuadPass::Settings rsmIndirectClearSettings;
      rsmIndirectClearSettings.viewPortWidth = kRenderWidth;
      rsmIndirectClearSettings.viewPortHeight = kRenderHeight;
      rsmIndirectClearSettings.region = Rect2D(0, 0, kRenderWidth, kRenderHeight);
      rsmIndirectClearSettings.color = Color::rgba(0, 0, 0, 1);
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearRsmIndirectDiffuseRaw", kRsmGiResolvePassIndex, rsmIndirectDiffuseRawTexture, Format::Format_RGBA16Float, rsmIndirectClearSettings));
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearRsmIndirectDiffuse", kRsmGiTemporalResolvePassIndex, rsmIndirectDiffuseTexture, Format::Format_RGBA16Float, rsmIndirectClearSettings));
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearRsmIndirectDiffuseHistory", kRsmGiHistoryCopyPassIndex, rsmIndirectDiffuseHistoryTexture, Format::Format_RGBA16Float, rsmIndirectClearSettings));
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearRsmIndirectDiffuseDenoiseTemporal", kRsmGiDenoisePassIndex, rsmIndirectDiffuseDenoiseTemporalTexture, Format::Format_RGBA16Float, rsmIndirectClearSettings));
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearRsmIndirectDiffuseDenoisedHistory", kRsmGiAtrousPassBaseIndex, rsmIndirectDiffuseDenoisedHistoryTexture, Format::Format_RGBA16Float, rsmIndirectClearSettings));
    }
    VirtualShadowMapPass::Settings ismVplPassSettings;
    ismVplPassSettings.bookkeepingSettings.depthTextureWidth = kRenderWidth;
    ismVplPassSettings.bookkeepingSettings.depthTextureHeight = kRenderHeight;
    configureBookkeepingShadowSettings(ismVplPassSettings.bookkeepingSettings);
    ismVplPassSettings.bookkeepingSettings.projectedLightIndex = ismVplProjectedLightIndex;
    ismVplPassSettings.enableTableDebugPass = useIsmVplDebugOutput;
    ismVplPassSettings.tableDebugSettings.width = kRenderWidth;
    ismVplPassSettings.tableDebugSettings.height = kRenderHeight;
    ismVplPassSettings.tableDebugSettings.debugLayer = ismVplDebugLayer;
    ismVplPassSettings.enableAtlasDebugPass = useIsmVplDebugOutput;
    ismVplPassSettings.atlasDebugSettings.width = kRenderWidth;
    ismVplPassSettings.atlasDebugSettings.height = kRenderHeight;
    ismVplPassSettings.atlasDebugSettings.debugLayer = ismVplDebugLayer;
    ismVplPassSettings.enableScreenSpaceShadowPass = false;
    ismVplPassSettings.enableShadowPcfPass = false;
    ismVplPassSettings.drawSettings.maxHierarchyLevels = editableRebuildSettings.finalMaxHierarchyLevels;
    ismVplPassSettings.drawSettings.hierarchyQueueCapacity = editableRebuildSettings.vsmHierarchyQueueCapacity;
    ismVplPassSettings.drawSettings.clusterQueueCapacity = editableRebuildSettings.vsmClusterQueueCapacity;
    ismVplPassSettings.drawSettings.maxVisibleClusterDraws = editableRebuildSettings.vsmMaxVisibleClusterDraws;
    ismVplPassSettings.drawSettings.scratchClipmapCount = editableRebuildSettings.vsmScratchClipmapCount;
    ismVplPassSettings.drawSettings.rasterMode = VirtualShadowMapDrawPass::RasterMode::ImperfectPointSplat;
    ismVplPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
    VirtualShadowMapPass *ismVplPass = nullptr;
    ImperfectShadowMapVplPointDebugPass *ismVplPointDebugPass = nullptr;
    if (useIsmVplPath)
    {
      ismVplPass = renderGraph->registerPass<VirtualShadowMapPass>(
          "imperfectShadowMapVplPass",
          38,
          scene,
          ismVplShadowManager,
          rendererPass->getSceneDepthTexture(),
          rendererPass->getGeometryNormalTexture(),
          imperfectShadowMapVplPagesDebugTexture,
          imperfectShadowMapVplTableDebugTexture,
          imperfectShadowMapVplAtlasDebugTexture,
          virtualShadowMapScreenSpaceShadowTexture,
          virtualShadowMapScreenSpaceSoftShadowDebugTexture,
          virtualShadowMapShadowLightingTexture,
          ismVplPassSettings);
      ownPass(ismVplPass);
      ismVplPointDebugPass = static_cast<ImperfectShadowMapVplPointDebugPass *>(ownPass(renderGraph->registerPass<ImperfectShadowMapVplPointDebugPass>(
          "imperfectShadowMapVplPointDebugPass",
          39,
          imperfectShadowMapVplPointsDebugTexture,
          ImperfectShadowMapVplPointDebugPass::Settings{
            .width = kRenderWidth,
            .height = kRenderHeight,
            .viewProjection = math::Mat4f::identity(),
            .selectedVpl = -1,
            .pointRadiusPixels = 5.0f,
            .exposure = 8.0f,
            .enabled = false,
          })));
    }
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsPass *gpuSignedDistanceFieldsPass = nullptr;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass *gpuSignedDistanceFieldsFallbackDebugPreparePass = nullptr;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass *gpuSignedDistanceFieldsFallbackDebugDrawPass = nullptr;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass *gpuSignedDistanceFieldsTraceDebugPass = nullptr;
    sdf::rendering::SDFTraceDebugPass *sparseSdfTraceDebugPass = nullptr;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass *gpuSignedDistanceFieldsAtlasDebugPass = nullptr;
    rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t> *softwareTlasSortPass = nullptr;
    softwarebvh::rendering::SoftwareTLASPass *softwareTlasRefitCanonicalPass = nullptr;
    softwarebvh::rendering::SoftwareTLASPass *softwareTlasRefitCwbvhPass = nullptr;
    std::array<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *, 7> softwareWorldBlasUpdatePasses{{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}};
    bool worldCwbvhWorldBlasBakePending = false;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *screenSpaceRayTraceSdfFallbackTracePass = nullptr;
    ScreenSpaceRayTraceSdfFallbackPreparePass *screenSpaceRayTraceSdfFallbackPreparePass = nullptr;
    globalillumination::SDFRayTraceEvaluateMaterialPass *sdfHitShadingResolvePass = nullptr;
    globalillumination::ScreenSpaceHitShadingResolvePass *screenSpaceHitShadingResolvePass = nullptr;
    softwarebvh::rendering::SoftwareRayTracePass *screenSpaceRayTraceSoftwareFallbackPass = nullptr;
    ScreenSpaceRayTraceSoftwareFallbackPreparePass *screenSpaceRayTraceSoftwareFallbackPreparePass = nullptr;
    softwarebvh::rendering::SoftwareRayTracePass *softwareRayTracePass = nullptr;
    constexpr bool kUseGpuSoftwareTlasBuild = false;
    constexpr bool kRefitWorldCwbvhBlasOnTransform = false;
    auto makeSoftwareRaytraceBlasSceneSettings =
        [&]()
    {
      const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
      softwarebvh::SoftwareBLASSceneBridge::Settings bridgeSettings{};
      if (softwareBlasSceneBridge != nullptr)
        bridgeSettings = softwareBlasSceneBridge->getSettings();
      bridgeSettings.primitiveMode = useWorldCwbvhTraversal
                                        ? softwarebvh::SoftwareBLASSceneBridge::PrimitiveMode::Triangle
                                        : static_cast<softwarebvh::SoftwareBLASSceneBridge::PrimitiveMode>(std::min<uint32_t>(softwareRayTracePrimitiveMode, 1u));
      bridgeSettings.fallbackLod = std::min<uint32_t>(softwareRayTraceFallbackLod, softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
      bridgeSettings.rbvhLod = std::min<uint32_t>(softwareRayTraceRbvhLod, softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
      bridgeSettings.preferRuntimeRbvhBuilds = true;
      return bridgeSettings;
    };
    auto makeSoftwareRaytraceTlasFilter =
        [&]()
    {
      return softwarebvh::SoftwareTLASSceneBridge::DebugInstanceFilter{
          .isolateSingleInstance = softwareRayTraceIsolateTLASInstance,
          .selectedInstanceIndex = softwareRayTraceSelectedTLASInstance,
      };
    };
    if (gpuSignedDistanceFieldsManager)
    {
      const uint32_t gpuSignedDistanceFieldsPassIndex = 34u;
      const uint32_t gpuSignedDistanceFieldsDebugPassIndex =
          gpuSignedDistanceFieldsPassIndex + gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsPass::maxChildPassCount(gpuSignedDistanceFieldsManager->getCascadeCount()) + 1u;
      if (editableRebuildSettings.enableSignedDistanceFields)
      {
        gpuSignedDistanceFieldsPass = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsPass *>(
            ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsPass>("gpuSignedDistanceFieldsPass", gpuSignedDistanceFieldsPassIndex, *gpuSignedDistanceFieldsManager)));
      }
      gpuSignedDistanceFieldsFallbackDebugPreparePass =
          static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass>(
              "gpuSignedDistanceFieldsFallbackDebugPreparePass", gpuSignedDistanceFieldsDebugPassIndex, *gpuSignedDistanceFieldsManager)));
      gpuSignedDistanceFieldsFallbackDebugDrawPass =
          static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass>(
              "gpuSignedDistanceFieldsFallbackDebugDrawPass",
              gpuSignedDistanceFieldsDebugPassIndex + 1u,
              *gpuSignedDistanceFieldsManager,
              *gpuSignedDistanceFieldsFallbackDebugPreparePass,
              signedDistanceFieldsFallbackSceneDebugTexture,
              signedDistanceFieldsFallbackSceneDepthTexture,
              Format::Format_RGBA16Float,
              gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugDrawPass::Settings{
                .width = kVW,
                .height = kVH,
                .reverseZ = cam.isReverseZ(),
                .mode = rendererDisplayOutputFallbackSceneMode(editableRebuildSettings.displayOutput),
              })));
      if (sparseSdfSceneManager != nullptr)
      {
        ownPass(renderGraph->registerPass<sdf::rendering::SDFSceneGridClearPass>(
            "sparseSdfSceneGridClearPass", gpuSignedDistanceFieldsDebugPassIndex + 2u, *sparseSdfSceneManager));
        ownPass(renderGraph->registerPass<sdf::rendering::SDFSceneGridBuildPass>(
            "sparseSdfSceneGridBuildPass", gpuSignedDistanceFieldsDebugPassIndex + 3u, *sparseSdfSceneManager));
        sparseSdfTraceDebugPass =
            static_cast<sdf::rendering::SDFTraceDebugPass *>(ownPass(renderGraph->registerPass<sdf::rendering::SDFTraceDebugPass>(
                "sparseSdfTraceDebugPass", gpuSignedDistanceFieldsDebugPassIndex + 4u, *sparseSdfSceneManager, signedDistanceFieldsTraceDebugTexture, sparseSdfTraceDebugSettings)));
        sparseSdfTraceDebugPass->setEnabled(false);
      }
      if (editableRebuildSettings.enableSignedDistanceFields || sdfTraceOutputRequested)
      {
	        gpuSignedDistanceFieldsTraceDebugPass =
	            static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass>(
	                "gpuSignedDistanceFieldsTraceDebugPass", gpuSignedDistanceFieldsDebugPassIndex + 5u, *gpuSignedDistanceFieldsManager, signedDistanceFieldsTraceDebugTexture, sdfTraceDebugSettings, &scene)));
        gpuSignedDistanceFieldsTraceDebugPass->setEnabled(false);
      }
      if (editableRebuildSettings.enableSignedDistanceFields || sdfAtlasOutputRequested)
      {
        gpuSignedDistanceFieldsAtlasDebugPass =
            static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsAtlasDebugPass>(
                "gpuSignedDistanceFieldsAtlasDebugPass", gpuSignedDistanceFieldsDebugPassIndex + 6u, *gpuSignedDistanceFieldsManager, signedDistanceFieldsAtlasDebugTexture, sdfAtlasDebugSettings)));
      }
    }
    if (softwareTlasManager && softwareBlasManager)
    {
      if (softwareBlasSceneBridge != nullptr)
      {
        const auto bridgeSettings = makeSoftwareRaytraceBlasSceneSettings();
        sceneManager.syncSoftwareRaytraceScene(bridgeSettings, makeSoftwareRaytraceTlasFilter());
        const auto &swrtBlasDiagnostics = softwareBlasSceneBridge->getDiagnostics();
        const auto &swrtBlasBuild = softwareBlasManager->getLastBuildSummary();
        os::Logger::logf(
            "[SWRT] pre-record sync traversal=%s primitive=%s fallbackLod=%u rbvhLod=%u prepared=%u unique=%u skippedMissingFallback=%u rebuild=%s runtimeRbvhBuilds=%u originalSources=%u proxySources=%u sourceTriangles=%llu buildMs=%.2f blas=%u cards=%u atlas=%ux%u texels=%u",
            softwareRayTraceTraversalMode == 1u ? "WorldCWBVH" : "LocalInstance",
            bridgeSettings.primitiveMode == softwarebvh::SoftwareBLASSceneBridge::PrimitiveMode::RBVHCard ? "RBVHCard" : "Triangle",
            bridgeSettings.fallbackLod,
            bridgeSettings.rbvhLod,
            swrtBlasDiagnostics.preparedMeshCount,
            swrtBlasDiagnostics.uniqueMeshCount,
            swrtBlasDiagnostics.skippedMissingFallbackMeshCount,
            swrtBlasDiagnostics.requestedRebuild ? "yes" : "no",
            swrtBlasDiagnostics.runtimeRbvhBuildCount,
            swrtBlasDiagnostics.runtimeRbvhOriginalSourceCount,
            swrtBlasDiagnostics.runtimeRbvhFallbackSourceCount,
            static_cast<unsigned long long>(swrtBlasDiagnostics.runtimeRbvhSourceTriangleCount),
            swrtBlasDiagnostics.runtimeRbvhBuildMilliseconds,
            swrtBlasBuild.blasCount,
            swrtBlasBuild.rbvhCardCount,
            softwareBlasManager->getRbvhDepthAtlasTextureWidth(),
            softwareBlasManager->getRbvhDepthAtlasTextureHeight(),
            softwareBlasManager->getRbvhDepthAtlasTextureTexelCount());
      }
      if (kUseGpuSoftwareTlasBuild)
      {
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasPrepareRebuildPass", 35u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::PrepareRebuild));
        softwareTlasSortPass = static_cast<rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t> *>(ownPass(renderGraph->registerPass<rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t>>(
            "softwareTlasSortPass",
            36u,
            softwareTlasManager->getMaxInstanceCount(),
            softwareTlasManager->getBuildMortonCodesBuffer(),
            softwareTlasManager->getBuildInstanceIndicesBuffer(),
            softwareTlasManager->getBuildSortCountBuffer(),
            softwareTlasManager->getBuildSortIndirectArgsBuffer())));
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasClearBuildFlagsPass", 37u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::ClearBuildFlags));
        ownPass(
            renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasBuildHierarchyLeavesPass", 38u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::BuildHierarchyLeaves));
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasBuildHierarchyPass", 39u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::BuildHierarchy));
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasRefitHierarchyPass", 40u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::RefitHierarchy));
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasInitializeCwbvhPass", 41u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::InitializeCWBVH));
        ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasBuildCwbvhPass", 42u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::BuildCWBVH));
        softwareTlasRefitCanonicalPass = static_cast<softwarebvh::rendering::SoftwareTLASPass *>(
            ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasRefitCanonicalPass", 43u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::RefitCanonical)));
        softwareTlasRefitCwbvhPass = static_cast<softwarebvh::rendering::SoftwareTLASPass *>(
            ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareTLASPass>("softwareTlasRefitCwbvhPass", 44u, *softwareTlasManager, softwarebvh::rendering::SoftwareTLASPass::Stage::RefitCWBVH)));
      }

      softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Buffers worldBlasBuffers{};
      softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Settings worldBlasSettings{};
      const bool buildWorldCwbvhBuffers = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
      if (buildWorldCwbvhBuffers)
      {
        const std::vector<softwarebvh::SoftwareRayTraceScene::SourceBLAS> worldSources = softwareBlasManager->buildRayTraceSceneSources();
        const std::vector<softwarebvh::SoftwareTLASManager::InstanceBuildInput> &tlasInputs = softwareTlasManager->getInstanceInputs();
        std::vector<softwarebvh::SoftwareRayTraceScene::InstanceInput> worldInstances;
        std::vector<softwarebvh::SoftwareRayTraceScene::InstanceTransformGPU> worldInstanceTransforms;
        worldInstances.reserve(tlasInputs.size());
        worldInstanceTransforms.resize(tlasInputs.size());
        for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(tlasInputs.size()); ++instanceIndex)
        {
          const auto &input = tlasInputs[instanceIndex];
          worldInstances.push_back(
              softwarebvh::SoftwareRayTraceScene::InstanceInput{
                  .instanceIndex = instanceIndex,
                  .sceneInstanceId = input.instanceId,
                  .blasHandle = input.blasHandle,
                  .flags = input.flags,
                  .transformDirty = true,
              });
          const math::Mat4f inverseTransform = input.transform.inverse();
          std::memcpy(worldInstanceTransforms[instanceIndex].transform, input.transform.data, sizeof(worldInstanceTransforms[instanceIndex].transform));
          std::memcpy(worldInstanceTransforms[instanceIndex].inverseTransform, inverseTransform.data, sizeof(worldInstanceTransforms[instanceIndex].inverseTransform));
        }

        const uint32_t tlasCwbvhNodeCount = std::max(softwareTlasManager->getCwbvhNodeCount(), 1u);
        softwarebvh::SoftwareRayTraceScene::Settings sceneSettings = softwarebvh::SoftwareRayTraceScene::estimateSettings(tlasCwbvhNodeCount, worldSources, worldInstances, 64u);
        softwarebvh::SoftwareRayTraceScene worldScene(sceneSettings);
        const softwarebvh::SoftwareRayTraceScene::Plan worldPlan = worldScene.buildPlan(tlasCwbvhNodeCount, worldSources, worldInstances);
        if (worldPlan.valid)
        {
          const std::vector<softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU> worldRanges = softwarebvh::SoftwareRayTraceScene::buildInstanceRangeGPU(worldPlan);
          const std::vector<softwarebvh::SoftwareRayTraceScene::TriangleUpdateItemGPU> triangleItems = softwarebvh::SoftwareRayTraceScene::buildTriangleUpdateItems(worldPlan, worldSources);
          const std::vector<softwarebvh::SoftwareRayTraceScene::BVH8UpdateItemGPU> bvh8Items = softwarebvh::SoftwareRayTraceScene::buildBVH8UpdateItems(worldPlan, worldSources);
          const std::vector<softwarebvh::SoftwareRayTraceScene::LeafRemapUpdateItemGPU> leafItems = softwarebvh::SoftwareRayTraceScene::buildLeafRemapUpdateItems(worldPlan, worldSources);

          const BufferUsage storageUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc | BufferUsage::BufferUsage_CopyDst;
          auto makeWorldBuffer =
              [&](const char *name, const uint64_t size, const BufferUsage extraUsage = BufferUsage::BufferUsage_None)
          {
            return ownBuffer(renderGraph->createBuffer(BufferInfo{.name = name, .size = std::max<uint64_t>(size, 4u), .usage = storageUsage | extraUsage}));
          };
          auto uploadToWorldBuffer =
              [&](const char *name, const Buffer &destination, const void *data, const uint64_t size)
          {
            if (data == nullptr || size == 0u)
              return;
            Buffer uploadBuffer = ownBuffer(renderGraph->createBuffer(
                BufferInfo{
                    .name = std::string(name) + ".upload",
                    .size = std::max<uint64_t>(size, 4u),
                    .usage = BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
                }));
            renderGraph->bufferWrite(uploadBuffer, 0u, size, const_cast<void *>(data));
            ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
                std::string(name) + ".copy",
                45u,
                uploadBuffer,
                0u,
                size,
                destination,
                0u,
                size));
          };

          worldBlasSettings.instanceRangeCount = static_cast<uint32_t>(worldRanges.size());
          worldBlasSettings.triangleUpdateItemCount = static_cast<uint32_t>(triangleItems.size());
          worldBlasSettings.bvh8UpdateItemCount = static_cast<uint32_t>(bvh8Items.size());
          worldBlasSettings.leafRemapUpdateItemCount = static_cast<uint32_t>(leafItems.size());
          worldBlasSettings.maxInstanceRangeCount = std::max<uint32_t>(worldBlasSettings.instanceRangeCount, 1u);
          worldBlasSettings.maxUpdateInstanceCount = std::max<uint32_t>(static_cast<uint32_t>(worldPlan.updateInstanceRangeIndices.size()), 1u);
          worldBlasSettings.maxTriangleUpdateItemCount = std::max<uint32_t>(worldBlasSettings.triangleUpdateItemCount, 1u);
          worldBlasSettings.maxBVH8UpdateItemCount = std::max<uint32_t>(worldBlasSettings.bvh8UpdateItemCount, 1u);
          worldBlasSettings.maxLeafRemapUpdateItemCount = std::max<uint32_t>(worldBlasSettings.leafRemapUpdateItemCount, 1u);
          worldBlasSettings.maxInstanceTransformCount = std::max<uint32_t>(static_cast<uint32_t>(worldInstanceTransforms.size()), 1u);
          worldBlasSettings.maxSourceTriangleCount = std::max<uint32_t>(softwareBlasManager->getLastBuildSummary().primitiveCount, 1u);
          worldBlasSettings.maxWorldTriangleCount = std::max<uint32_t>(worldPlan.worldTriangleCount, 1u);
          worldBlasSettings.maxSourceBVH8NodeCount = std::max<uint32_t>(softwareBlasManager->getSourceBVH8NodeCount(), 1u);
          worldBlasSettings.maxWorldBVH8NodeCount = std::max<uint32_t>(worldPlan.worldBVH8NodeCount, 1u);
          worldBlasSettings.maxSourceLeafRemapCount = std::max<uint32_t>(static_cast<uint32_t>(softwareBlasManager->getGPULeafRemapBufferSize() / sizeof(uint32_t)), 1u);
          worldBlasSettings.maxWorldLeafRemapCount = std::max<uint32_t>(worldPlan.worldLeafRemapCount, 1u);
          worldBlasSettings.maxSceneCWBVHNodeCount = std::max<uint32_t>(worldPlan.sceneCWBVHNodeCount, tlasCwbvhNodeCount);
          worldBlasSettings.workgroupSize = 64u;
          worldBlasSettings.useIndirectDispatch = true;
          worldBlasSettings.enabled = true;
          worldCwbvhWorldBlasBakePending = true;

          worldBlasBuffers.instanceRanges = makeWorldBuffer("SoftwareWorldBLAS_InstanceRanges.buffer", static_cast<uint64_t>(worldBlasSettings.maxInstanceRangeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU));
          worldBlasBuffers.updateInstanceRangeIndices = makeWorldBuffer("SoftwareWorldBLAS_UpdateInstanceRanges.buffer", static_cast<uint64_t>(worldBlasSettings.maxUpdateInstanceCount) * sizeof(uint32_t));
          worldBlasBuffers.updateCounters = makeWorldBuffer("SoftwareWorldBLAS_UpdateCounters.buffer", sizeof(softwarebvh::SoftwareRayTraceScene::UpdateCountersGPU));
          worldBlasBuffers.dispatchArgs = makeWorldBuffer("SoftwareWorldBLAS_DispatchArgs.buffer", 4u * sizeof(softwarebvh::SoftwareRayTraceScene::DispatchIndirectArgsGPU), BufferUsage::BufferUsage_Indirect);
          worldBlasBuffers.triangleUpdateItems = makeWorldBuffer("SoftwareWorldBLAS_TriangleItems.buffer", static_cast<uint64_t>(worldBlasSettings.maxTriangleUpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriangleUpdateItemGPU));
          worldBlasBuffers.bvh8UpdateItems = makeWorldBuffer("SoftwareWorldBLAS_BVH8Items.buffer", static_cast<uint64_t>(worldBlasSettings.maxBVH8UpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8UpdateItemGPU));
          worldBlasBuffers.leafRemapUpdateItems = makeWorldBuffer("SoftwareWorldBLAS_LeafItems.buffer", static_cast<uint64_t>(worldBlasSettings.maxLeafRemapUpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::LeafRemapUpdateItemGPU));
          worldBlasBuffers.instanceTransforms = makeWorldBuffer("SoftwareWorldBLAS_InstanceTransforms.buffer", static_cast<uint64_t>(worldBlasSettings.maxInstanceTransformCount) * sizeof(softwarebvh::SoftwareRayTraceScene::InstanceTransformGPU));
          worldBlasBuffers.sourceTriangles = softwareBlasManager->getTriangleTransformsBuffer();
          worldBlasBuffers.worldTriangles = makeWorldBuffer("SoftwareWorldBLAS_WorldTriangles.buffer", static_cast<uint64_t>(worldBlasSettings.maxWorldTriangleCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU));
          worldBlasBuffers.sourceBVH8Nodes = softwareBlasManager->getSourceBVH8NodesBuffer();
          worldBlasBuffers.worldBVH8Nodes = makeWorldBuffer("SoftwareWorldBLAS_WorldBVH8Nodes.buffer", static_cast<uint64_t>(worldBlasSettings.maxWorldBVH8NodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8NodeGPU));
          worldBlasBuffers.sourceLeafRemap = softwareBlasManager->getGPULeafRemapBuffer();
          worldBlasBuffers.worldLeafRemap = makeWorldBuffer("SoftwareWorldBLAS_WorldLeafRemap.buffer", static_cast<uint64_t>(worldBlasSettings.maxWorldLeafRemapCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldLeafEntryGPU));
          worldBlasBuffers.sceneCWBVHNodes = makeWorldBuffer("SoftwareWorldBLAS_SceneCWBVHNodes.buffer", static_cast<uint64_t>(worldBlasSettings.maxSceneCWBVHNodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU));
          worldBlasBuffers.sourcePrimitiveRemap = softwareBlasManager->getPrimitiveRemapBuffer();

          uploadToWorldBuffer(
              "SoftwareWorldBLAS_InstanceRanges",
              worldBlasBuffers.instanceRanges,
              worldRanges.data(),
              worldRanges.size() * sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU));
          uploadToWorldBuffer(
              "SoftwareWorldBLAS_InstanceTransforms",
              worldBlasBuffers.instanceTransforms,
              worldInstanceTransforms.data(),
              worldInstanceTransforms.size() * sizeof(softwarebvh::SoftwareRayTraceScene::InstanceTransformGPU));
          uploadToWorldBuffer(
              "SoftwareWorldBLAS_TriangleItems",
              worldBlasBuffers.triangleUpdateItems,
              triangleItems.data(),
              triangleItems.size() * sizeof(softwarebvh::SoftwareRayTraceScene::TriangleUpdateItemGPU));
          uploadToWorldBuffer(
              "SoftwareWorldBLAS_BVH8Items",
              worldBlasBuffers.bvh8UpdateItems,
              bvh8Items.data(),
              bvh8Items.size() * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8UpdateItemGPU));
          uploadToWorldBuffer(
              "SoftwareWorldBLAS_LeafItems",
              worldBlasBuffers.leafRemapUpdateItems,
              leafItems.data(),
              leafItems.size() * sizeof(softwarebvh::SoftwareRayTraceScene::LeafRemapUpdateItemGPU));

          const uint64_t tlasCopyBytes = static_cast<uint64_t>(tlasCwbvhNodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU);
          ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>("softwareWorldCopyTlasCwbvhPass", 45u, softwareTlasManager->getCwbvhNodesBuffer(), 0u, tlasCopyBytes, worldBlasBuffers.sceneCWBVHNodes, 0u, tlasCopyBytes));
          softwareWorldBlasUpdatePasses[0] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldClearCountersPass", 45u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::ClearCounters, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[1] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldPrepareUpdateJobsPass", 46u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::PrepareUpdateJobs, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[2] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldPrepareUpdateDispatchesPass", 46u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::PrepareUpdateDispatches, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[3] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldTransformTrianglesPass", 47u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::TransformTriangles, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[4] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldRefitBVH8Pass", 47u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::RefitWorldBVH8, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[5] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldCopyLeafRemapPass", 47u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::CopyLeafRemap, worldBlasBuffers, worldBlasSettings)));
          softwareWorldBlasUpdatePasses[6] = static_cast<softwarebvh::rendering::SoftwareWorldBLASUpdatePass *>(ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareWorldBLASUpdatePass>("softwareWorldCompressCWBVHPass", 47u, softwarebvh::rendering::SoftwareWorldBLASUpdatePass::Stage::CompressBVH8ToCWBVH, worldBlasBuffers, worldBlasSettings)));
        }
        else
        {
          os::Logger::warningf("[SWRT] World CWBVH plan invalid: %s", worldPlan.error.c_str());
        }
      }

      auto makeSoftwareTraceSettings =
          [&]()
      {
        const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
        softwarebvh::rendering::SoftwareRayTracePass::Settings softwareTraceSettings{};
        softwareTraceSettings.width = kSoftwareRayTraceWidth;
        softwareTraceSettings.height = kSoftwareRayTraceHeight;
        softwareTraceSettings.enabled = editableRebuildSettings.enableSoftwareRayTracing && rendererDisplayOutputUsesSoftwareRayTracing(editableRebuildSettings.displayOutput);
        softwareTraceSettings.generateCameraRays = true;
        softwareTraceSettings.enableDynamicRayFetch = false;
        softwareTraceSettings.rayCount = 0u;
        softwareTraceSettings.debugMode = rendererDisplayOutputSoftwareRayTraceDebugMode(editableRebuildSettings.displayOutput);
        softwareTraceSettings.primitiveMode = useWorldCwbvhTraversal
                                                 ? softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle
                                                 : static_cast<softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode>(std::min<uint32_t>(softwareRayTracePrimitiveMode, 1u));
        softwareTraceSettings.traversalMode = useWorldCwbvhTraversal
                                                 ? softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH
                                                 : softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance;
        softwareTraceSettings.tlasMode = useWorldCwbvhTraversal
                                            ? softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH
                                            : static_cast<softwarebvh::rendering::SoftwareRayTracePass::TLASMode>(std::min<uint32_t>(softwareRayTraceTlasMode, 1u));
        softwareTraceSettings.enableRaySorting =
            softwareTraceSettings.traversalMode == softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH ||
            softwareTraceSettings.tlasMode == softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH;
        softwareTraceSettings.rbvhLod = std::min<uint32_t>(softwareRayTraceRbvhLod, softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
        return softwareTraceSettings;
      };
      auto makeWorldCwbvhTraceBuffers =
          [&]()
      {
        softwarebvh::rendering::SoftwareRayTracePass::WorldCWBVHBufferSet buffers{};
        if (!buildWorldCwbvhBuffers || !worldBlasBuffers.sceneCWBVHNodes.isValid())
          return buffers;
        buffers.sceneCWBVHNodes = worldBlasBuffers.sceneCWBVHNodes;
        buffers.sceneCWBVHNodesSize = static_cast<uint64_t>(worldBlasSettings.maxSceneCWBVHNodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU);
        buffers.worldTriangleTransforms = worldBlasBuffers.worldTriangles;
        buffers.worldTriangleTransformsSize = static_cast<uint64_t>(worldBlasSettings.maxWorldTriangleCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU);
        buffers.worldLeafRemap = worldBlasBuffers.worldLeafRemap;
        buffers.worldLeafRemapSize = static_cast<uint64_t>(worldBlasSettings.maxWorldLeafRemapCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldLeafEntryGPU);
        buffers.worldInstanceRanges = worldBlasBuffers.instanceRanges;
        buffers.worldInstanceRangesSize = static_cast<uint64_t>(worldBlasSettings.maxInstanceRangeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU);
        return buffers;
      };
      if (screenSpaceRayTracePass != nullptr && screenSpaceRayTraceSoftwareFallbackEnabled)
      {
        const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
        auto fallbackTraceSettings = makeSoftwareTraceSettings();
        fallbackTraceSettings.enabled = screenSpaceRayTraceSoftwareFallbackEnabled;
        fallbackTraceSettings.generateCameraRays = false;
        fallbackTraceSettings.width = 1u;
        fallbackTraceSettings.height = 1u;
        fallbackTraceSettings.rayCount = screenSpaceRayTraceEffectiveFallbackBudget;
        fallbackTraceSettings.tMin = screenSpaceRayTraceMinDistance;
        fallbackTraceSettings.tMax = screenSpaceRayTraceMaxDistance;
        fallbackTraceSettings.primitiveMode = softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle;
        fallbackTraceSettings.traversalMode = useWorldCwbvhTraversal
                                                 ? softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH
                                                 : softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance;
        fallbackTraceSettings.tlasMode = useWorldCwbvhTraversal
                                            ? softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH
                                            : static_cast<softwarebvh::rendering::SoftwareRayTracePass::TLASMode>(std::min<uint32_t>(softwareRayTraceTlasMode, 1u));
        fallbackTraceSettings.enableRaySorting =
            fallbackTraceSettings.traversalMode == softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH ||
            fallbackTraceSettings.tlasMode == softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH;
        fallbackTraceSettings.shaderVariant = screenSpaceRayTraceSoftwareFallbackTraceStats
            ? softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant::Debug
            : softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant::Release;
        screenSpaceRayTraceSoftwareFallbackPass = static_cast<softwarebvh::rendering::SoftwareRayTracePass *>(
            ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareRayTracePass>("screenSpaceRayTraceSoftwareFallbackPass", useWorldCwbvhTraversal ? 48u : 46u, *softwareTlasManager, *softwareBlasManager, fallbackTraceSettings, makeWorldCwbvhTraceBuffers())));
        screenSpaceRayTraceSoftwareFallbackPreparePass = static_cast<ScreenSpaceRayTraceSoftwareFallbackPreparePass *>(ownPass(renderGraph->registerPass<ScreenSpaceRayTraceSoftwareFallbackPreparePass>(
            "screenSpaceRayTraceSoftwareFallbackPreparePass",
            45u,
            screenSpaceRayTracePass->getFailedRaysBuffer(),
            screenSpaceRayTraceSoftwareFallbackPass->getRayInputBuffer(),
            ScreenSpaceRayTraceSoftwareFallbackPreparePass::Settings{
                .enabled = screenSpaceRayTraceSoftwareFallbackEnabled,
                .budget = screenSpaceRayTraceEffectiveFallbackBudget,
                .maxFailedRays = screenSpaceRayTraceEffectiveMaxFailedRays,
                .tMin = screenSpaceRayTraceMinDistance,
                .tMax = screenSpaceRayTraceMaxDistance,
            })));
        screenSpaceRayTraceSoftwareFallbackPass->setTraceDispatchIndirectBuffer(screenSpaceRayTraceSoftwareFallbackPreparePass->getDispatchIndirectArgsBuffer());
      }
      softwarebvh::rendering::SoftwareRayTracePass::Settings softwareTraceSettings = makeSoftwareTraceSettings();
      softwareRayTracePass = static_cast<softwarebvh::rendering::SoftwareRayTracePass *>(
          ownPass(renderGraph->registerPass<softwarebvh::rendering::SoftwareRayTracePass>("softwareRayTracePass", 48u, *softwareTlasManager, *softwareBlasManager, softwareRayTraceDebugTexture, softwareTraceSettings, makeWorldCwbvhTraceBuffers())));
    }
    if (screenSpaceRayTracePass != nullptr && gpuSignedDistanceFieldsManager != nullptr && editableRebuildSettings.enableSignedDistanceFields && screenSpaceRayTraceSdfFallbackEnabled)
    {
      screenSpaceRayTraceSdfFallbackPreparePass = static_cast<ScreenSpaceRayTraceSdfFallbackPreparePass *>(ownPass(renderGraph->registerPass<ScreenSpaceRayTraceSdfFallbackPreparePass>(
          "screenSpaceRayTraceSdfFallbackPreparePass",
          45u,
          screenSpaceRayTracePass->getFailedRaysBuffer(),
          screenSpaceRayTraceSdfFallbackRayBuffer,
          ScreenSpaceRayTraceSdfFallbackPreparePass::Settings{
              .enabled = screenSpaceRayTraceSdfFallbackEnabled,
              .budget = screenSpaceRayTraceEffectiveFallbackBudget,
              .maxFailedRays = screenSpaceRayTraceEffectiveMaxFailedRays,
              .tMin = screenSpaceRayTraceMinDistance,
              .tMax = screenSpaceRayTraceMaxDistance,
          })));
      screenSpaceRayTraceSdfFallbackTracePass = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass>(
          "screenSpaceRayTraceSdfFallbackTracePass",
          46u,
          *gpuSignedDistanceFieldsManager,
          screenSpaceRayTraceSdfFallbackRayBuffer,
          screenSpaceRayTraceSdfFallbackHitBuffer,
          gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
              .rayCount = screenSpaceRayTraceEffectiveFallbackBudget,
              .maxSteps = screenProbeSdfMaxSteps,
              .maxTraceDistance = screenSpaceRayTraceMaxDistance,
              .hitThresholdScale = screenProbeSdfHitThresholdScale,
              .emptySpaceStepScale = screenProbeSdfEmptySpaceStepScale,
              .normalBias = screenSpaceRayTraceOriginBias,
              .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
          })));
      screenSpaceRayTraceSdfFallbackTracePass->setEnabled(screenSpaceRayTraceSdfFallbackEnabled);
      sdfHitShadingResolvePass = static_cast<globalillumination::SDFRayTraceEvaluateMaterialPass *>(ownPass(renderGraph->registerPass<globalillumination::SDFRayTraceEvaluateMaterialPass>(
          "sdfHitShadingResolvePass",
          47u,
          scene,
          screenSpaceRayTraceSdfFallbackRayBuffer,
          screenSpaceRayTraceSdfFallbackHitBuffer,
          screenSpaceRayTracePass->getFailedRaysBuffer(),
          geometryNormalTexture,
          baseColorTexture,
          worldHitShadingBuffer,
          globalillumination::SDFRayTraceEvaluateMaterialPass::Settings{
              .rayCount = screenSpaceRayTraceEffectiveFallbackBudget,
              .enabled = screenSpaceRayTraceSdfFallbackEnabled,
              .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
              .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
          })));
      sdfHitShadingResolvePass->setDispatchIndirectBuffer(screenSpaceRayTraceSdfFallbackPreparePass->getDispatchIndirectArgsBuffer());
    }
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
    // The earliest renderer node is emitted from the page-decode pass command buffer,
    // so target that actual node prefix instead of the logical pre-culling pass name.
    renderGraph->addInterFrameDependency("virtualGeometryRendererPass.pageDecode", "virtualGeometryMaterialDispatchPass");
    if (softwareRayTracePass != nullptr)
    {
      if (kUseGpuSoftwareTlasBuild)
      {
        renderGraph->addInterFrameDependency("softwareTlasPrepareRebuildPass", "softwareRayTracePass");
      }
      renderGraph->addInterFrameDependency("softwareRbvhAtlasUploadPass", "softwareRayTracePass");
      renderGraph->addInterFrameDependency("softwareRayTracePass", "softwareRayTracePass");
    }
    if (screenSpaceRayTraceSoftwareFallbackPass != nullptr)
    {
      if (kUseGpuSoftwareTlasBuild)
      {
        renderGraph->addInterFrameDependency("softwareTlasPrepareRebuildPass", "screenSpaceRayTraceSoftwareFallbackPass");
      }
      renderGraph->addInterFrameDependency("softwareRbvhAtlasUploadPass", "screenSpaceRayTraceSoftwareFallbackPass");
      renderGraph->addInterFrameDependency("screenSpaceRayTraceSoftwareFallbackPass", "screenSpaceRayTraceSoftwareFallbackPass");
    }
    if (gpuSignedDistanceFieldsPass != nullptr && gpuSignedDistanceFieldsManager->getCascadeCount() > 0u)
    {
      // The SDF clipmap build mutates persistent shared state (atlas, brick metadata,
      // combined brick maps / AABB tree), so the next frame's first mutation must wait
      // for the previous frame's last combine step to finish. The combine pass no longer
      // uses per-cascade child names here, so target the actual final node name.
      renderGraph->addInterFrameDependency("gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsPass.combine.buildTreeAabb");
      if (gpuSignedDistanceFieldsFallbackDebugPreparePass != nullptr)
        renderGraph->addInterFrameDependency("gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsFallbackDebugPreparePass");
      if (gpuSignedDistanceFieldsFallbackDebugDrawPass != nullptr)
        renderGraph->addInterFrameDependency("gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsFallbackDebugDrawPass");
      renderGraph->addInterFrameDependency("gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsTraceDebugPass");
      renderGraph->addInterFrameDependency("gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsAtlasDebugPass");
    }
    vsmPass->getBookkeepingPass()->setDebugOutputEnabled(false);
    if (auto *pass = vsmPass->getTableDebugPass())
      pass->setEnabled(false);
    if (auto *pass = vsmPass->getAtlasDebugPass())
      pass->setEnabled(false);
    if (auto *pass = vsmPass->getScreenSpaceShadowPass())
      pass->setEnabled(screenSpaceShadowEnabled);
    if (ismVplPass != nullptr)
    {
      ismVplPass->getBookkeepingPass()->setDebugOutputEnabled(false);
      if (auto *pass = ismVplPass->getTableDebugPass())
        pass->setEnabled(false);
      if (auto *pass = ismVplPass->getAtlasDebugPass())
        pass->setEnabled(false);
    }
    if (gpuSignedDistanceFieldsFallbackDebugPreparePass != nullptr)
    {
      gpuSignedDistanceFieldsFallbackDebugPreparePass->setEnabled(false);
    }
    if (gpuSignedDistanceFieldsFallbackDebugDrawPass != nullptr)
    {
      gpuSignedDistanceFieldsFallbackDebugDrawPass->setEnabled(false);
    }
    if (gpuSignedDistanceFieldsTraceDebugPass != nullptr)
    {
      gpuSignedDistanceFieldsTraceDebugPass->setEnabled(false);
    }
    if (gpuSignedDistanceFieldsAtlasDebugPass != nullptr)
    {
      gpuSignedDistanceFieldsAtlasDebugPass->setEnabled(false);
    }
    auto applyShadowFilterSettings = [&]()
    {
      configureBookkeepingShadowSettings(vsmPassSettings.bookkeepingSettings);
      configureShadowSettings(vsmPassSettings.shadowPcfSettings);
      configureScreenSpaceShadowSettings(vsmPassSettings.screenSpaceShadowSettings);
      vsmPassSettings.softShadowMethod = editableRebuildSettings.softShadowMethod;
      vsmPassSettings.screenSpaceSoftShadowSettings.width = kRenderWidth;
      vsmPassSettings.screenSpaceSoftShadowSettings.height = kRenderHeight;
      vsmPassSettings.screenSpaceSoftShadowSettings.resolutionScale = sanitizeScreenSpaceSoftShadowResolutionScale(screenSpaceSoftShadowSettings.resolutionScale);
      vsmPassSettings.screenSpaceSoftShadowSettings.maxSoftShadowLayers = std::max(1u, screenSpaceSoftShadowSettings.maxSoftShadowLayers);
      vsmPassSettings.screenSpaceSoftShadowSettings.kernelSize = sanitizeScreenSpaceSoftShadowKernelSize(screenSpaceSoftShadowSettings.kernelSize);
      vsmPassSettings.screenSpaceSoftShadowSettings.blockerSearchTaps = sanitizeScreenSpaceSoftShadowBlockerSearchTaps(screenSpaceSoftShadowSettings.blockerSearchTaps);
      vsmPassSettings.screenSpaceSoftShadowSettings.useMinFilter = screenSpaceSoftShadowSettings.useMinFilter;
      vsmPassSettings.screenSpaceSoftShadowSettings.defaultLightRadiusWorld = screenSpaceSoftShadowSettings.defaultLightRadiusWorld;
      vsmPassSettings.screenSpaceSoftShadowSettings.penumbraScale = screenSpaceSoftShadowSettings.penumbraScale;
      vsmPassSettings.screenSpaceSoftShadowSettings.grazingThreshold = screenSpaceSoftShadowSettings.grazingThreshold;
      vsmPassSettings.screenSpaceSoftShadowSettings.depthRejectThreshold = screenSpaceSoftShadowSettings.depthRejectThreshold;
      vsmPassSettings.screenSpaceSoftShadowSettings.translucentShadowsEnabled = screenSpaceSoftShadowSettings.translucentShadowsEnabled;
      vsmPassSettings.screenSpaceSoftShadowSettings.debugMode = rendererDisplayOutputSsssDebugMode(editableRebuildSettings.displayOutput, screenSpaceSoftShadowSettings.debugMode);
      vsmPassSettings.screenSpaceSoftShadowSettings.debugLayer = screenSpaceSoftShadowSettings.debugLayer;
      vsmPassSettings.screenSpaceSoftShadowSettings.debugValueScale = screenSpaceSoftShadowSettings.debugValueScale;
      vsmPassSettings.screenSpaceSoftShadowSettings.directionalShadowBias = directionalShadowBias;
      vsmPassSettings.screenSpaceSoftShadowSettings.spotShadowBias = spotShadowBias;
      vsmPassSettings.screenSpaceSoftShadowSettings.pointShadowBias = pointShadowBias;
      vsmPassSettings.screenSpaceSoftShadowSettings.localLightMipTransitionScale = vsmLocalLightMipTransitionScale;
      vsmPassSettings.screenSpaceSoftShadowSettings.localLightMipBias = vsmLocalLightMipBias;
      vsmManager.setMaxSoftShadowLayers(vsmPassSettings.screenSpaceSoftShadowSettings.maxSoftShadowLayers);
      vsmPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
      if (auto *pass = vsmPass->getBookkeepingPass())
      {
        pass->setSettings(vsmPassSettings.bookkeepingSettings);
      }
      if (auto *pass = vsmPass->getShadowPcfPass())
      {
        pass->setSettings(vsmPassSettings.shadowPcfSettings);
        pass->setEnabled(true);
      }
      if (auto *pass = vsmPass->getDrawPass())
      {
        pass->setLodErrorThreshold(vsmLodErrorThreshold);
      }
      if (auto *pass = vsmPass->getScreenSpaceShadowPass())
      {
        pass->setSettings(vsmPassSettings.screenSpaceShadowSettings);
        pass->setEnabled(screenSpaceShadowEnabled);
      }
      if (auto *pass = vsmPass->getScreenSpaceSoftShadowPass())
      {
        pass->setSettings(vsmPassSettings.screenSpaceSoftShadowSettings);
      }
      if (ismVplPass != nullptr)
      {
        configureBookkeepingShadowSettings(ismVplPassSettings.bookkeepingSettings);
        ismVplPassSettings.bookkeepingSettings.projectedLightIndex = ismVplProjectedLightIndex;
        ismVplPassSettings.drawSettings.lodErrorThreshold = vsmLodErrorThreshold;
        if (auto *pass = ismVplPass->getBookkeepingPass())
        {
          pass->setSettings(ismVplPassSettings.bookkeepingSettings);
        }
        if (auto *pass = ismVplPass->getDrawPass())
        {
          pass->setLodErrorThreshold(vsmLodErrorThreshold);
        }
      }
    };
    auto logShadowFilterSettings = [&]()
    {
      const char *softShadowMethodName = "Hard";
      switch (editableRebuildSettings.softShadowMethod)
      {
      case SoftShadowMethod::PCF:
        softShadowMethodName = "PCF";
        break;
      case SoftShadowMethod::PCSS:
        softShadowMethodName = "PCSS";
        break;
      case SoftShadowMethod::LayeredScreenSpaceSoftShadows:
        softShadowMethodName = "SSSS";
        break;
      case SoftShadowMethod::Hard:
      default:
        softShadowMethodName = "Hard";
        break;
      }
      os::Logger::logf(
          "[Shadow] lod prepass=%.2f geometry=%.2f vsm=%.2f | filter=%s enabled=%s taps=%u radius=%.2f fallbackTaps=%u | pcssBlockerTaps=%u pcssRadius=%.2f pcssMax=%.2f pcssScale=%.2f | sss=%s thickness=%.4f bilinear=%.3f contrast=%.2f samples=60 edgeSkip=%s skippedEdgesLit=%s offsetMode=%s",
          prepassLodErrorThreshold,
          geometryLodErrorThreshold,
          vsmLodErrorThreshold,
          softShadowMethodName,
          shadowPcfEnabled ? "yes" : "no",
          shadowFilterTaps,
          pcfRadiusTexels,
          fallbackShadowFilterTaps,
          pcssBlockerSearchTaps,
          pcssLightRadiusTexels,
          pcssMaxPenumbraTexels,
          pcssPenumbraScale,
          screenSpaceShadowEnabled ? "yes" : "no",
          screenSpaceSurfaceThickness,
          screenSpaceBilinearThreshold,
          screenSpaceShadowContrast,
          screenSpaceIgnoreEdgePixels ? "yes" : "no",
          screenSpaceTreatSkippedEdgeSamplesAsLit ? "yes" : "no",
          screenSpaceBilinearSamplingOffsetMode ? "alt" : "default");
      os::Logger::logf("[Shadow][LocalVSM] projectedLight=%d transitionScale=%.2f mipBias=%.2f", vsmProjectedLightIndex, vsmLocalLightMipTransitionScale, vsmLocalLightMipBias);
      os::Logger::logf(
          "[Shadow][SSSS] maxLayers=%u activeSoftLayers=%u overflow=%u graphDegree=%u directionalLayer=%u directionalFallback=%s pointLayer=%u pointFallback=%s",
          vsmManager.getMaxSoftShadowLayerCount(),
          vsmManager.getActiveSoftShadowLayerCount(),
          vsmManager.getSoftShadowOverflowCount(),
          vsmManager.getSoftShadowMaxGraphDegree(),
          vsmManager.getSoftShadowLayerForLight(vsmLightId),
          vsmManager.isSoftShadowFallbackLight(vsmLightId) ? "yes" : "no",
          vsmManager.getSoftShadowLayerForLight(vsmPointLightId),
          vsmManager.isSoftShadowFallbackLight(vsmPointLightId) ? "yes" : "no");
      os::Logger::logf("[Shadow][ISM] projectedVpl=%d", ismVplProjectedLightIndex);
    };
    applyShadowFilterSettings();
    logShadowFilterSettings();
    os::Logger::log(
        "[Shadow] controls: overlay sliders tune VSM bias/PCF plus screen-space shadow thickness/edge handling/debug, F2 toggles PCF, -=/+=tap count, [/] = PCF radius, display output is selected in ImGui and applied on "
        "renderer rebuild");
    if (editableRebuildSettings.vsmEnableTableDebugPass)
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
      ownPass(vsmTableClip0Pass);
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
      ownPass(vsmTableClip1Pass);
      vsmTableClip0Pass->setEnabled(true);
      vsmTableClip1Pass->setEnabled(true);
    }

    auto *preCullingPass = rendererPass->getPreCullingPass();
    auto *depthPrePass = rendererPass->getDepthPrePass();
    auto *finalCullingPass = rendererPass->getFinalCullingPass();
    auto *hwDrawPass = rendererPass->getHardwareDrawPass();
    auto materialPass = std::make_unique<MaterialPass>(scene, frameTarget, virtualShadowMapShadowLightingTexture, rendererSettings.materialPassSettings);
    materialPass->initialize(*renderGraph, "virtualGeometryMaterialPass");
    ownPass(renderGraph->registerPass<MaterialSetupPass>("virtualGeometryMaterialSetupPass", 40, *materialPass));
    ownPass(renderGraph->registerPass<MaterialPreparePass>("virtualGeometryMaterialPreparePass", 41, *materialPass));
    ownPass(renderGraph->registerPass<MaterialDispatchPass>("virtualGeometryMaterialDispatchPass", 42, *materialPass));
	    VirtualGeometryTAAResolvePass *taaResolvePass = nullptr;
	    VirtualGeometryTAAHistoryPass *taaHistoryPass = nullptr;
	    VirtualShadowMapRsmTemporalResolvePass *rsmTemporalResolvePass = nullptr;
    rendering::gpgpu::SvgfDenoisePass *rsmGiDenoisePass = nullptr;
    std::array<rendering::gpgpu::SvgfAtrousFilterPass *, kRsmGiAtrousPassCount> rsmGiAtrousFilterPasses{};
    std::array<globalillumination::surfels::SurfelTracePreparePass *, 2> surfelTracePreparePasses{{nullptr, nullptr}};
    TraceStatsClearPass *surfelTraceStatsClearPass = nullptr;
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *surfelSdfTracePass = nullptr;
    globalillumination::WorldHitShadingResolvePass *worldHitShadingResolvePass = nullptr;
    globalillumination::WorldHitShadingResolvePass *softwareRayTraceShadingResolvePass = nullptr;
    globalillumination::TraceShadingAlbedoDebugPass *softwareRayTraceAlbedoDebugPass = nullptr;
    if (screenSpaceRayTracePass != nullptr && screenSpaceRayTraceEnabled && screenSpaceRayTraceScreenTracesEnabled && !screenSpaceRayTraceForceWorldFallback)
    {
      screenSpaceHitShadingResolvePass = static_cast<globalillumination::ScreenSpaceHitShadingResolvePass *>(ownPass(renderGraph->registerPass<globalillumination::ScreenSpaceHitShadingResolvePass>(
          "screenSpaceHitShadingResolvePass",
          52u,
          screenSpaceRayTracePass->getScreenHitsBuffer(),
          geometryNormalTexture,
          baseColorTexture,
          normalTexture,
          materialIdTexture,
          materialUVTexture,
          screenSpaceHitShadingBuffer,
          globalillumination::ScreenSpaceHitShadingResolvePass::Settings{
            .hitCapacity = screenSpaceRayTraceEffectiveMaxFailedRays,
            .enabled = screenSpaceRayTraceEnabled && screenSpaceRayTraceScreenTracesEnabled && !screenSpaceRayTraceForceWorldFallback,
          })));
    }
    globalillumination::surfels::SurfelHierarchyDebugPass *surfelHierarchyDebugPass = nullptr;
    std::array<globalillumination::surfels::SurfelDirectDrawDebugPass *, 2> surfelDirectDrawDebugPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelHeatmapDebugPass *, 2> surfelHeatmapDebugPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelScreenSpaceSpawnPass *, 2> surfelScreenSpaceSpawnPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationClearPass *, 2> surfelAccelerationClearPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationCountPass *, 2> surfelAccelerationOwnerResolvePasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationCountPass *, 2> surfelAccelerationCountPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationPrefixSumPass *, 2> surfelAccelerationPrefixSumPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationPrefixSegmentsPass *, 2> surfelAccelerationPrefixSegmentsPasses{{nullptr, nullptr}};
    std::array<globalillumination::surfels::SurfelAccelerationPrefixMergePass *, 2> surfelAccelerationPrefixMergePasses{{nullptr, nullptr}};
	    std::array<globalillumination::surfels::SurfelAccelerationInsertPass *, 2> surfelAccelerationInsertPasses{{nullptr, nullptr}};
	    std::array<globalillumination::surfels::SurfelLightingUpdatePass *, 2> surfelLightingUpdatePasses{{nullptr, nullptr}};
	    globalillumination::worldgi::WorldGILightGridUpdatePass *worldGILightGridUpdatePass = nullptr;
	    globalillumination::worldgi::WorldGILightGridDebugPass *worldGILightGridDebugPass = nullptr;
	    globalillumination::worldgi::WorldGIRayGenPass *worldGIRayGenPass = nullptr;
	    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *worldGISdfTracePass = nullptr;
	    globalillumination::SDFRayTraceEvaluateMaterialPass *worldGISdfMaterialResolvePass = nullptr;
	    globalillumination::worldgi::WorldGITraceSurfaceResolvePass *worldGITraceSurfaceResolvePass = nullptr;
	    globalillumination::worldgi::WorldGIHitShadingPass *worldGIHitShadingPass = nullptr;
	    globalillumination::worldgi::WorldGIComposePass *worldGIComposePass = nullptr;
    const bool useSurfels = false;
    const bool useWorldGI = editableRebuildSettings.enableWorldGI;
    auto makeFallbackColorTexture = [&](const std::string &name) -> Texture
    {
      return ownTexture(renderGraph->createTexture(
          TextureInfo{
            .name = name,
            .width = 1u,
            .height = 1u,
            .depth = 1u,
            .mipLevels = 1u,
            .format = Format::Format_RGBA16Float,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Sampled,
          }));
    };
    const Texture fallbackColorTexture = makeFallbackColorTexture("SelectedDebugTargetFallbackColor");
    if (kTaaEnabled)
    {
      VirtualGeometryTAAResolvePass::Settings taaResolveSettings;
      taaResolveSettings.sourceViewPortWidth = kRenderWidth;
      taaResolveSettings.sourceViewPortHeight = kRenderHeight;
      taaResolveSettings.outputViewPortWidth = kVW;
      taaResolveSettings.outputViewPortHeight = kVH;
      taaResolveSettings.baseWeight = std::clamp(1.0f - 1.0f / static_cast<float>(kTemporalAccumulationFrames), 0.0f, 0.98f);
      taaResolveSettings.motionScale = editableRebuildSettings.taaMotionScale;
      taaResolveSettings.depthThreshold = editableRebuildSettings.taaDepthThreshold;
      taaResolveSettings.normalThreshold = editableRebuildSettings.taaNormalThreshold;
      taaResolveSettings.reactiveScale = editableRebuildSettings.taaReactiveScale;
      taaResolveSettings.edgeScale = editableRebuildSettings.taaEdgeScale;
      taaResolveSettings.sharpenStrength = editableRebuildSettings.taaSharpenStrength;
      taaResolveSettings.detailBoost = editableRebuildSettings.taaDetailBoost;
      taaResolvePass = static_cast<VirtualGeometryTAAResolvePass *>(ownPass(renderGraph->registerPass<VirtualGeometryTAAResolvePass>(
          "virtualGeometryTAAResolvePass",
          43,
          scene,
          rendererPass->getColorTexture(),
          rendererPass->getGeometryNormalTexture(),
          rendererPass->getSceneDepthTexture(),
          rendererPass->getMotionVectorTexture(),
          packedGeometryIdsLoTexture,
          packedGeometryIdsHiTexture,
          taaHistoryColorTexture,
          taaHistoryDepthTexture,
          taaHistoryIdsTexture,
          taaHistoryNormalTexture,
          taaColorTexture,
          taaDisocclusionMaskTexture,
          taaResolveSettings)));
      VirtualGeometryTAAHistoryPass::Settings taaHistorySettings;
      taaHistorySettings.sourceViewPortWidth = kRenderWidth;
      taaHistorySettings.sourceViewPortHeight = kRenderHeight;
      taaHistorySettings.outputViewPortWidth = kVW;
      taaHistorySettings.outputViewPortHeight = kVH;
      taaHistoryPass = static_cast<VirtualGeometryTAAHistoryPass *>(ownPass(renderGraph->registerPass<VirtualGeometryTAAHistoryPass>(
          "virtualGeometryTAAHistoryPass",
          44,
          scene,
          taaColorTexture,
          rendererPass->getGeometryNormalTexture(),
          rendererPass->getSceneDepthTexture(),
          packedGeometryIdsLoTexture,
          packedGeometryIdsHiTexture,
          taaHistoryColorWriteTexture,
          taaHistoryDepthWriteTexture,
          taaHistoryIdsWriteTexture,
          taaHistoryNormalWriteTexture,
	          taaHistorySettings)));
	    }
	    if (useRsmScreenPass)
	    {
	      const auto rsmDebugMode = rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput);
	      rsmTemporalResolvePass = static_cast<VirtualShadowMapRsmTemporalResolvePass *>(ownPass(renderGraph->registerPass<VirtualShadowMapRsmTemporalResolvePass>(
	          "virtualShadowMapRsmTemporalResolvePass",
	          kRsmGiTemporalResolvePassIndex,
	          rsmIndirectDiffuseRawTexture,
	          rsmIndirectDiffuseHistoryTexture,
	          rendererPass->getMotionVectorTexture(),
	          taaDisocclusionMaskTexture,
	          rendererPass->getSceneDepthTexture(),
	          rendererPass->getGeometryNormalTexture(),
	          rsmDenoiseHistoryDepthTexture,
	          rsmDenoiseHistoryNormalTexture,
	          rsmIndirectDiffuseTexture,
	          VirtualShadowMapRsmTemporalResolvePass::Settings{
	            .width = kRenderWidth,
	            .height = kRenderHeight,
	            .debugMode = rsmDebugMode,
	            .historyWeight = std::clamp(1.0f - 1.0f / static_cast<float>(std::max(editableRebuildSettings.rsmGiTemporalFrames, 1u)), 0.0f, 0.98f),
	            .disocclusionWeight = editableRebuildSettings.rsmGiTemporalDisocclusionWeight,
	            .depthSigma = editableRebuildSettings.rsmGiTemporalDepthSigma,
		            .normalSigma = editableRebuildSettings.rsmGiTemporalNormalSigma,
		            .validRatioMin = editableRebuildSettings.rsmGiValidRatioMin,
		            .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiTemporalFrames, 1u)),
		            .fireflyClamp = editableRebuildSettings.rsmGiFireflyClamp,
		            .enabled = editableRebuildSettings.rsmGiTemporalAccumulation && rsmDebugMode == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse,
		            .historyValid = false,
		          })));
      ownPass(renderGraph->registerPass<rendering::gpgpu::TextureCopyPass>(
          "copyRsmIndirectDiffuseHistory",
          kRsmGiHistoryCopyPassIndex,
          rsmIndirectDiffuseTexture,
          rsmIndirectDiffuseHistoryTexture,
          rendering::gpgpu::TextureCopyPass::Aspect::Color));
      rsmGiDenoisePass = static_cast<rendering::gpgpu::SvgfDenoisePass *>(ownPass(renderGraph->registerPass<rendering::gpgpu::SvgfDenoisePass>(
          "rsmGiSvgfDenoisePass",
          kRsmGiDenoisePassIndex,
          rsmIndirectDiffuseTexture,
          rsmIndirectDiffuseDenoisedHistoryTexture,
          rendererPass->getMotionVectorTexture(),
          taaDisocclusionMaskTexture,
          rendererPass->getSceneDepthTexture(),
          rendererPass->getGeometryNormalTexture(),
          rsmDenoiseHistoryDepthTexture,
          rsmDenoiseHistoryNormalTexture,
          rsmIndirectDiffuseDenoiseTemporalTexture,
          rendering::gpgpu::SvgfDenoisePass::Settings{
            .width = kRenderWidth,
            .height = kRenderHeight,
            .currentJitter = {0.0f, 0.0f},
            .previousJitter = {0.0f, 0.0f},
            .historyWeight = std::clamp(1.0f - 1.0f / static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)), 0.0f, 0.99f),
            .depthSigma = editableRebuildSettings.rsmGiDenoiserDepthSigma,
            .normalSigma = editableRebuildSettings.rsmGiDenoiserNormalSigma,
            .luminanceSigma = editableRebuildSettings.rsmGiDenoiserLuminanceSigma,
            .validRatioMin = editableRebuildSettings.rsmGiDenoiserValidRatioMin,
            .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)),
            .fireflyClamp = editableRebuildSettings.rsmGiDenoiserFireflyClamp,
            .spatialSigma = editableRebuildSettings.rsmGiDenoiserSpatialSigma,
            .spatialRadius = editableRebuildSettings.rsmGiDenoiserSpatialRadius,
            .spatialStrength = editableRebuildSettings.rsmGiDenoiserSpatialStrength,
            .disocclusionWeight = editableRebuildSettings.rsmGiDenoiserDisocclusionWeight,
            .minCurrentWeight = editableRebuildSettings.rsmGiDenoiserMinCurrentWeight,
            .enabled = editableRebuildSettings.rsmGiDenoiserEnabled && rsmDebugMode == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse,
            .historyValid = false,
          })));
      const auto atrousSourceForIndex = [&](uint32_t atrousIndex) -> Texture
      {
        if (atrousIndex == 0u)
        {
          return rsmIndirectDiffuseDenoiseTemporalTexture;
        }
        return (atrousIndex % 2u) == 1u ? rsmIndirectDiffuseDenoisePingTexture : rsmIndirectDiffuseDenoisedTexture;
      };
      const auto atrousOutputForIndex = [&](uint32_t atrousIndex) -> Texture
      {
        return (atrousIndex % 2u) == 0u ? rsmIndirectDiffuseDenoisePingTexture : rsmIndirectDiffuseDenoisedTexture;
      };
      for (uint32_t atrousIndex = 0u; atrousIndex < static_cast<uint32_t>(rsmGiAtrousFilterPasses.size()); ++atrousIndex)
      {
        const uint32_t baseStep = std::max(1u, static_cast<uint32_t>(std::lround(editableRebuildSettings.rsmGiDenoiserSpatialRadius)));
        rsmGiAtrousFilterPasses[atrousIndex] = static_cast<rendering::gpgpu::SvgfAtrousFilterPass *>(ownPass(renderGraph->registerPass<rendering::gpgpu::SvgfAtrousFilterPass>(
            "rsmGiSvgfAtrousPass" + std::to_string(atrousIndex),
            kRsmGiAtrousPassBaseIndex + atrousIndex,
            atrousSourceForIndex(atrousIndex),
            rendererPass->getSceneDepthTexture(),
            rendererPass->getGeometryNormalTexture(),
            atrousOutputForIndex(atrousIndex),
            rendering::gpgpu::SvgfAtrousFilterPass::Settings{
              .width = kRenderWidth,
              .height = kRenderHeight,
              .stepWidth = baseStep << atrousIndex,
              .depthSigma = editableRebuildSettings.rsmGiDenoiserDepthSigma,
              .normalSigma = editableRebuildSettings.rsmGiDenoiserNormalSigma,
              .luminanceSigma = editableRebuildSettings.rsmGiDenoiserLuminanceSigma,
              .fireflyClamp = editableRebuildSettings.rsmGiDenoiserFireflyClamp,
              .spatialStrength = editableRebuildSettings.rsmGiDenoiserSpatialStrength,
              .varianceSigma = editableRebuildSettings.rsmGiDenoiserSpatialSigma,
              .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)),
              .enabled = editableRebuildSettings.rsmGiDenoiserEnabled &&
                         rsmDebugMode == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse &&
                         atrousIndex < std::clamp(editableRebuildSettings.rsmGiDenoiserAtrousIterations, 1u, kRsmGiAtrousPassCount),
            })));
      }
      ownPass(renderGraph->registerPass<rendering::gpgpu::TextureCopyPass>(
          "copyRsmIndirectDiffuseDenoisedHistory",
          kRsmGiDenoiseHistoryCopyPassIndex,
          rsmIndirectDiffuseDenoiseTemporalTexture,
          rsmIndirectDiffuseDenoisedHistoryTexture,
          rendering::gpgpu::TextureCopyPass::Aspect::Color));
      ownPass(renderGraph->registerPass<rendering::gpgpu::TextureCopyPass>(
          "copyRsmDenoiseHistoryDepth",
          kRsmGiDenoiseDepthHistoryCopyPassIndex,
          rendererPass->getSceneDepthTexture(),
          rsmDenoiseHistoryDepthTexture,
          rendering::gpgpu::TextureCopyPass::Aspect::Depth));
      ownPass(renderGraph->registerPass<rendering::gpgpu::TextureCopyPass>(
          "copyRsmDenoiseHistoryNormal",
          kRsmGiDenoiseNormalHistoryCopyPassIndex,
          rendererPass->getGeometryNormalTexture(),
          rsmDenoiseHistoryNormalTexture,
          rendering::gpgpu::TextureCopyPass::Aspect::Color));
	    }
	    rendering::passes::ColorQuadPass::Settings screenProbeClearSettings;
    screenProbeClearSettings.viewPortWidth = kRenderWidth;
    screenProbeClearSettings.viewPortHeight = kRenderHeight;
    screenProbeClearSettings.region = Rect2D(0, 0, kRenderWidth, kRenderHeight);
    screenProbeClearSettings.color = Color::rgba(0, 0, 0, 0);
    ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearSpatialRadianceCacheDebug", 46, spatialRadianceCacheDebugTexture, Format::Format_RGBA16Float, screenProbeClearSettings));
    ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearSpatialRadianceReservoirCacheDebug", 46, spatialRadianceReservoirCacheDebugTexture, Format::Format_RGBA16Float, screenProbeClearSettings));
    rendering::passes::ColorQuadPass::Settings surfelHierarchyClearSettings = screenProbeClearSettings;
    surfelHierarchyClearSettings.color = Color::rgba(1, 0, 1, 1);
    if (screenSpaceRayTraceSoftwareFallbackPass != nullptr && screenSpaceRayTraceSoftwareFallbackEnabled)
    {
      worldHitShadingResolvePass = static_cast<globalillumination::WorldHitShadingResolvePass *>(ownPass(renderGraph->registerPass<globalillumination::WorldHitShadingResolvePass>(
          "worldHitShadingResolvePass",
          52u,
          scene,
          *softwareTlasManager,
          *softwareBlasManager,
          screenSpaceRayTraceSoftwareFallbackPass->getRayInputBuffer(),
          screenSpaceRayTraceSoftwareFallbackPass->getHitOutputBuffer(),
          screenSpaceRayTracePass->getFailedRaysBuffer(),
          geometryNormalTexture,
          baseColorTexture,
          worldHitShadingBuffer,
          globalillumination::WorldHitShadingResolvePass::Settings{
            .rayCount = screenSpaceRayTraceEffectiveFallbackBudget,
            .enabled = kRendererEnableWorldHitShadingResolve && screenSpaceRayTraceSoftwareFallbackEnabled,
            .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
            .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
          })));
      if (screenSpaceRayTraceSoftwareFallbackPreparePass != nullptr)
      {
        worldHitShadingResolvePass->setDispatchIndirectBuffer(screenSpaceRayTraceSoftwareFallbackPreparePass->getDispatchIndirectArgsBuffer());
      }
    }
    if (softwareRayTracePass != nullptr)
    {
      const bool softwareRayTraceAlbedoDebugEnabled = rendererDisplayOutputIsSoftwareRayTracingAlbedo(editableRebuildSettings.displayOutput);
      softwareRayTraceShadingResolvePass = static_cast<globalillumination::WorldHitShadingResolvePass *>(ownPass(renderGraph->registerPass<globalillumination::WorldHitShadingResolvePass>(
          "softwareRayTraceHitShadingResolvePass",
          52u,
          scene,
          *softwareTlasManager,
          *softwareBlasManager,
          softwareRayTracePass->getRayInputBuffer(),
          softwareRayTracePass->getHitOutputBuffer(),
          softwareRayTraceReceiverRayBuffer,
          geometryNormalTexture,
          baseColorTexture,
          softwareRayTraceShadingBuffer,
          globalillumination::WorldHitShadingResolvePass::Settings{
            .rayCount = softwareRayTraceDebugRayCount,
            .enabled = kRendererEnableWorldHitShadingResolve && softwareRayTraceAlbedoDebugEnabled,
            .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
            .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
            .receiverPixelWidth = kSoftwareRayTraceWidth,
          })));
      softwareRayTraceAlbedoDebugPass = static_cast<globalillumination::TraceShadingAlbedoDebugPass *>(ownPass(renderGraph->registerPass<globalillumination::TraceShadingAlbedoDebugPass>(
          "softwareRayTraceAlbedoDebugPass",
          53u,
          softwareRayTraceShadingBuffer,
          softwareRayTraceAlbedoDebugTexture,
          globalillumination::TraceShadingAlbedoDebugPass::Settings{
            .width = kSoftwareRayTraceWidth,
            .height = kSoftwareRayTraceHeight,
            .rayCount = softwareRayTraceDebugRayCount,
            .enabled = kRendererEnableWorldHitShadingResolve && softwareRayTraceAlbedoDebugEnabled,
          })));
    }
    if (useSurfels)
    {
      ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearSurfelHierarchyDebug", 46, surfelHierarchyDebugTexture, Format::Format_RGBA16Float, surfelHierarchyClearSettings));
      surfelHierarchySettings.viewPortWidth = kRenderWidth;
      surfelHierarchySettings.viewPortHeight = kRenderHeight;
      surfelHierarchyDebugPass = static_cast<globalillumination::surfels::SurfelHierarchyDebugPass *>(ownPass(
          renderGraph->registerPass<globalillumination::surfels::SurfelHierarchyDebugPass>("surfelHierarchyDebugPass", 55u, rendererPass->getSceneDepthTexture(), surfelHierarchyDebugTexture, surfelHierarchySettings)));
    }
    uint32_t surfelPassIndex = useRsmScreenPass ? (kRsmGiDenoiseNormalHistoryCopyPassIndex + 1u) : 56u;
    const auto computeSurfelAllocationLimit = [&](uint32_t surfelCapacity) -> uint32_t
    {
      const uint32_t clampedAllocatedCount = std::min(surfelAllocatedCount, surfelCapacity);
      return static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(surfelCapacity), static_cast<uint64_t>(clampedAllocatedCount) + static_cast<uint64_t>(surfelRuntimeSettings.allocationBudgetPerFrame)));
    };
    if (useSurfels)
    {
      for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
      {
        const std::string suffix = surfelFrameIndex == 0u ? "A" : "B";
        surfelAccelerationClearPasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationClearPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationClearPass>(
                "surfelAccelerationClearPass" + suffix,
                surfelPassIndex++,
                surfelGridBuffers[surfelFrameIndex],
                surfelGridWriteCursorBuffers[surfelFrameIndex],
                surfelOwnerCellBuffers[surfelFrameIndex],
                surfelRuntimeSettings)));
        surfelAccelerationOwnerResolvePasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationCountPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationCountPass>(
                "surfelAccelerationOwnerResolvePass" + suffix,
                surfelPassIndex++,
                scene,
                surfelStackBuffer,
                surfelPositionRadiusBuffer,
                surfelNormalLifecycleBuffer,
                surfelAttachmentBuffer,
                surfelLifecycleBuffer,
                surfelRecycleClaimBuffer,
                surfelOwnerCellBuffers[surfelFrameIndex],
                surfelGridBuffers[surfelFrameIndex],
                surfelGridBuffers[1u - surfelFrameIndex],
                surfelRuntimeSettings)));
        surfelAccelerationCountPasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationCountPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationCountPass>(
                "surfelAccelerationCountPass" + suffix,
                surfelPassIndex++,
                scene,
                surfelStackBuffer,
                surfelPositionRadiusBuffer,
                surfelNormalLifecycleBuffer,
                surfelAttachmentBuffer,
                surfelLifecycleBuffer,
                surfelRecycleClaimBuffer,
                surfelOwnerCellBuffers[surfelFrameIndex],
                surfelGridBuffers[surfelFrameIndex],
                surfelGridBuffers[1u - surfelFrameIndex],
                surfelRuntimeSettings)));
        surfelAccelerationPrefixSumPasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationPrefixSumPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationPrefixSumPass>(
                "surfelAccelerationPrefixSumPass" + suffix, surfelPassIndex++, surfelGridBuffers[surfelFrameIndex], surfelPrefixSegmentsBuffers[surfelFrameIndex], surfelRuntimeSettings)));
        surfelAccelerationPrefixSegmentsPasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationPrefixSegmentsPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationPrefixSegmentsPass>(
                "surfelAccelerationPrefixSegmentsPass" + suffix, surfelPassIndex++, surfelPrefixSegmentsBuffers[surfelFrameIndex], surfelRuntimeSettings)));
        surfelAccelerationPrefixMergePasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationPrefixMergePass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationPrefixMergePass>(
                "surfelAccelerationPrefixMergePass" + suffix,
                surfelPassIndex++,
                surfelGridBuffers[surfelFrameIndex],
                surfelGridWriteCursorBuffers[surfelFrameIndex],
                surfelPrefixSegmentsBuffers[surfelFrameIndex],
                surfelRuntimeSettings)));
        surfelAccelerationInsertPasses[surfelFrameIndex] =
            static_cast<globalillumination::surfels::SurfelAccelerationInsertPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelAccelerationInsertPass>(
                "surfelAccelerationInsertPass" + suffix,
                surfelPassIndex++,
                surfelPositionRadiusBuffer,
                surfelOwnerCellBuffers[surfelFrameIndex],
                surfelGridBuffers[surfelFrameIndex],
                surfelGridWriteCursorBuffers[surfelFrameIndex],
                surfelListBuffers[surfelFrameIndex],
                surfelRuntimeSettings)));
        surfelScreenSpaceSpawnPasses[surfelFrameIndex] = static_cast<globalillumination::surfels::SurfelScreenSpaceSpawnPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelScreenSpaceSpawnPass>(
            "surfelScreenSpaceSpawnPass" + suffix,
            surfelPassIndex++,
            scene,
            surfelStackBuffer,
            surfelGridWriteCursorBuffers[surfelFrameIndex],
            surfelRecycleClaimBuffer,
            surfelOwnerCellBuffers[surfelFrameIndex],
            surfelGridBuffers[surfelFrameIndex],
            surfelListBuffers[surfelFrameIndex],
            surfelPositionRadiusBuffer,
            surfelNormalLifecycleBuffer,
            surfelAttachmentBuffer,
            surfelLifecycleBuffer,
            surfelMaterialBuffer,
            surfelIrradianceBuffer,
            surfelReservoirBuffers[1u - surfelFrameIndex],
            surfelReservoirBuffers[surfelFrameIndex],
            surfelDirectReservoirBuffers[1u - surfelFrameIndex],
            surfelDirectReservoirBuffers[surfelFrameIndex],
            rendererPass->getSceneDepthTexture(),
            rendererPass->getGeometryNormalTexture(),
            virtualShadowMapShadowLightingTexture,
            rendererPass->getBaseColorTexture(),
            rendererPass->getNormalTexture(),
            packedGeometryIdsLoTexture,
            packedGeometryIdsHiTexture,
            globalillumination::surfels::SurfelScreenSpaceSpawnPass::Settings{
              .viewPortWidth = kRenderWidth,
              .viewPortHeight = kRenderHeight,
              .frameIndex = 0u,
              .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
              .surfelAllocationLimit = computeSurfelAllocationLimit(surfelRuntimeSettings.surfelCapacity),
              .gridCapacity = surfelRuntimeSettings.gridCapacity,
              .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
              .enabled = 0u,
              .spawnDownsample = surfelRuntimeSettings.spawnDownsample,
              .targetSurfelsPerTile = surfelRuntimeSettings.targetSurfelsPerTile,
              .maxSurfelsPerTile = surfelRuntimeSettings.maxSurfelsPerTile,
              .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
              .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
              .gridScale = surfelRuntimeSettings.gridScale,
              .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
              .targetScreenRadiusPixels = surfelRuntimeSettings.targetScreenRadiusPixels,
              .lookupRadiusScale = surfelRuntimeSettings.lookupRadiusScale,
              .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
              .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
            })));
        surfelTracePreparePasses[surfelFrameIndex] = static_cast<globalillumination::surfels::SurfelTracePreparePass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelTracePreparePass>(
            "surfelTracePreparePass" + suffix,
            surfelPassIndex++,
            surfelPositionRadiusBuffer,
            surfelNormalLifecycleBuffer,
            surfelMaterialBuffer,
            surfelDirectReservoirBuffers[1u - surfelFrameIndex],
            surfelDirectReservoirBuffers[surfelFrameIndex],
            vsmManager.getDirectionalLightsBuffer(),
            vsmManager.getDirectionalLightsBufferSize(),
            rendererPass->getSceneDepthTexture(),
            rendererPass->getGeometryNormalTexture(),
            giSdfRayBuffer,
            globalillumination::surfels::SurfelTracePreparePass::Settings{
              .viewProjection = math::Mat4f::identity(),
              .inverseViewProjection = math::Mat4f::identity(),
              .viewPortWidth = kRenderWidth,
              .viewPortHeight = kRenderHeight,
              .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
              .rayOffset = kSurfelLightingRayOffset,
              .frameIndex = 0u,
              .updatePeriodFrames = surfelRuntimeSettings.lightingUpdatePeriodFrames,
              .activeLightCount = vsmManager.getActiveLightCount(),
              .reverseZ = cam.isReverseZ(),
              .maxTraceDistance = surfelRuntimeSettings.lightingMaxTraceDistance,
              .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
              .viewBias = surfelRuntimeSettings.lightingTraceViewBias,
              .directionalLightDirection = {lightDirection[0], lightDirection[1], lightDirection[2]},
              .directionalLightIntensity = directionalLightEnabled ? directionalLightIntensity : 0.0f,
              .directionalLightColor = {directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]},
              .pointLightPosition = {pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]},
              .pointLightRange = pointLightRange,
              .pointLightColor = {pointLightColor[0], pointLightColor[1], pointLightColor[2]},
              .pointLightIntensity = pointLightEnabled ? pointLightIntensity : 0.0f,
            })));
        surfelDirectDrawDebugPasses[surfelFrameIndex] = static_cast<globalillumination::surfels::SurfelDirectDrawDebugPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelDirectDrawDebugPass>(
            "surfelDirectDrawDebugPass" + suffix,
            surfelPassIndex++,
            surfelGridBuffers[surfelFrameIndex],
            surfelListBuffers[surfelFrameIndex],
            surfelPositionRadiusBuffer,
            surfelNormalLifecycleBuffer,
            surfelMaterialBuffer,
            surfelIrradianceBuffer,
            rendererPass->getSceneDepthTexture(),
            rendererPass->getGeometryNormalTexture(),
            rendererPass->getColorTexture(),
            surfelDirectDrawDebugTexture,
            globalillumination::surfels::SurfelDirectDrawDebugPass::Settings{
              .viewPortWidth = kRenderWidth,
              .viewPortHeight = kRenderHeight,
              .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
              .gridCapacity = surfelRuntimeSettings.gridCapacity,
              .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
              .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
              .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
              .gridScale = surfelRuntimeSettings.gridScale,
              .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
              .lookupRadiusScale = surfelRuntimeSettings.lookupRadiusScale,
              .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
              .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
              .debugMode = static_cast<uint32_t>(globalillumination::surfels::SurfelDirectDrawDebugPass::DebugMode::Coverage),
            })));
        surfelHeatmapDebugPasses[surfelFrameIndex] = static_cast<globalillumination::surfels::SurfelHeatmapDebugPass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelHeatmapDebugPass>(
            "surfelHeatmapDebugPass" + suffix,
            surfelPassIndex++,
            surfelGridBuffers[surfelFrameIndex],
            rendererPass->getSceneDepthTexture(),
            surfelHeatmapDebugTexture,
            globalillumination::surfels::SurfelHeatmapDebugPass::Settings{
              .viewPortWidth = kRenderWidth,
              .viewPortHeight = kRenderHeight,
              .gridCapacity = surfelRuntimeSettings.gridCapacity,
              .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
              .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
              .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
              .gridScale = surfelRuntimeSettings.gridScale,
              .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
              .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
              .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
            })));
      }
    }
    if (useSurfels && editableRebuildSettings.enableSignedDistanceFields && gpuSignedDistanceFieldsManager != nullptr)
    {
      surfelTraceStatsClearPass = static_cast<TraceStatsClearPass *>(ownPass(renderGraph->registerPass<TraceStatsClearPass>("surfelTraceStatsClearPass", surfelPassIndex++, surfelTraceStatsBuffer)));
      surfelSdfTracePass = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass>(
          "surfelSdfTracePass",
          surfelPassIndex++,
          *gpuSignedDistanceFieldsManager,
          giSdfRayBuffer,
          giSdfTraceHitBuffer,
          gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
            .rayCount = kSurfelLightingRayCount,
            .rayOffset = kSurfelLightingRayOffset,
            .hitOffset = kSurfelLightingRayOffset,
            .maxSteps = surfelRuntimeSettings.lightingTraceMaxSteps,
            .maxTraceDistance = surfelRuntimeSettings.lightingMaxTraceDistance,
            .hitThresholdScale = surfelRuntimeSettings.lightingTraceHitThresholdScale,
            .emptySpaceStepScale = surfelRuntimeSettings.lightingTraceEmptySpaceStepScale,
            .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
            .cascadeOverride = surfelRuntimeSettings.lightingTraceCascadeOverride,
            .traceStatsEnabled = true,
            .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
          },
          surfelTraceStatsBuffer)));
      surfelTraceStatsClearPass->setEnabled(useSurfels);
      surfelSdfTracePass->setEnabled(useSurfels);
    }
    if (useSurfels)
    {
      for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
      {
        const std::string suffix = surfelFrameIndex == 0u ? "A" : "B";
        surfelLightingUpdatePasses[surfelFrameIndex] = static_cast<globalillumination::surfels::SurfelLightingUpdatePass *>(ownPass(renderGraph->registerPass<globalillumination::surfels::SurfelLightingUpdatePass>(
            "surfelLightingUpdatePass" + suffix,
            surfelPassIndex++,
            surfelGridBuffers[surfelFrameIndex],
            surfelListBuffers[surfelFrameIndex],
            surfelPositionRadiusBuffer,
            surfelNormalLifecycleBuffer,
            surfelMaterialBuffer,
            surfelIrradianceBuffer,
            surfelReservoirBuffers[1u - surfelFrameIndex],
            surfelReservoirBuffers[surfelFrameIndex],
            surfelDirectReservoirBuffers[surfelFrameIndex],
            giSdfTraceHitBuffer,
            rendererPass->getSceneDepthTexture(),
            rendererPass->getGeometryNormalTexture(),
            virtualShadowMapShadowLightingTexture,
            vsmManager.getDirectionalLightsBuffer(),
            vsmManager.getDirectionalLightsBufferSize(),
            vsmManager.getCascadeStatesBuffer(),
            vsmManager.getCascadeStatesBufferSize(),
            vsmManager.getCascadeMatricesBuffer(),
            vsmManager.getCascadeMatricesBufferSize(),
            vsmManager.getCameraStateBuffer(),
            vsmManager.getCameraStateBufferSize(),
            vsmManager.getVirtualPageTableBuffer(),
            vsmManager.getVirtualPageTableBufferSize(),
            vsmManager.getShadowAtlasTexture(),
            globalillumination::surfels::SurfelLightingUpdatePass::Settings{
              .viewProjection = math::Mat4f::identity(),
              .inverseViewProjection = math::Mat4f::identity(),
              .viewPortWidth = kRenderWidth,
              .viewPortHeight = kRenderHeight,
              .frameIndex = 0u,
              .reverseZ = cam.isReverseZ(),
              .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
              .traceHitOffset = kSurfelLightingRayOffset,
              .updatePeriodFrames = surfelRuntimeSettings.lightingUpdatePeriodFrames,
              .gridCapacity = surfelRuntimeSettings.gridCapacity,
              .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
              .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
              .cameraPosition = {cameraPosition[0], cameraPosition[1], cameraPosition[2]},
              .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
              .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
              .hysteresis = surfelRuntimeSettings.lightingHysteresis,
              .irradianceDecay = surfelRuntimeSettings.lightingIrradianceDecay,
              .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
              .viewBias = surfelRuntimeSettings.lightingTraceViewBias,
              .energyPreservation = surfelRuntimeSettings.lightingEnergyPreservation,
              .directionalShadowEnabled = directionalLightEnabled,
              .directionalShadowReverseZ = cam.isReverseZ(),
              .directionalShadowPageTableResolution = vsmManager.getVirtualPageTableResolution(),
              .directionalShadowActiveLayers = vsmManager.getActiveLayerCount(),
              .directionalShadowActiveLights = vsmManager.getActiveLightCount(),
              .directionalShadowPhysicalPageSize = vsmManager.getPhysicalPageResolution(),
              .directionalShadowCascadeCount = vsmManager.getCascadeCount(),
              .directionalShadowFallbackCascadeOffset = vsmManager.getFallbackCascadeOffset(),
              .directionalShadowFirstCascadeWorldExtent = vsmManager.getFirstCascadeWorldExtent(),
              .directionalShadowBias = directionalShadowBias,
              .directionalShadowMaxBias = directionalMaxShadowBias,
              .directionalShadowPcfRadiusTexels = pcfRadiusTexels,
              .directionalShadowNormalBiasTexels = directionalNormalBiasTexels,
              .directionalShadowFilterTaps = shadowPcfEnabled ? shadowFilterTaps : 1u,
              .directionalLightDirection = {lightDirection[0], lightDirection[1], lightDirection[2]},
              .directionalLightIntensity = directionalLightIntensity,
              .directionalLightColor = {directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]},
              .defaultDiffuseAlbedo = surfelRuntimeSettings.lightingDefaultDiffuseAlbedo,
              .pointLightPosition = {pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]},
              .pointLightRange = pointLightRange,
              .pointLightColor = {pointLightColor[0], pointLightColor[1], pointLightColor[2]},
              .pointLightIntensity = pointLightEnabled ? pointLightIntensity : 0.0f,
            })));
      }
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>("surfelStackReadbackPass", surfelPassIndex++, surfelStackBuffer, 0u, sizeof(uint32_t), surfelStackReadbackBuffer, 0u, sizeof(uint32_t)));
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "surfelIrradianceReadbackPass",
          surfelPassIndex++,
          surfelIrradianceBuffer,
          0u,
          globalillumination::surfels::surfelIrradianceBufferSize(surfelRuntimeSettings),
          surfelIrradianceReadbackBuffer,
          0u,
          globalillumination::surfels::surfelIrradianceBufferSize(surfelRuntimeSettings)));
    }

    const uint32_t worldGIBaseIndex = surfelPassIndex;
    const uint32_t screenSpaceRayTraceStatsReadbackPassIndex = 30u;
    const uint32_t surfelStatsReadbackPassIndex = worldGIBaseIndex + 8u;
    const uint32_t softwareRayTraceStatsReadbackPassIndex = surfelStatsReadbackPassIndex + 1u;
    const uint32_t screenFallbackTraceStatsReadbackPassIndex = softwareRayTraceStatsReadbackPassIndex + 1u;
    const uint32_t finalPresentBaseIndex = screenFallbackTraceStatsReadbackPassIndex + 1u;
    {
      worldGILightGridUpdatePass = static_cast<globalillumination::worldgi::WorldGILightGridUpdatePass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGILightGridUpdatePass>(
          "worldGILightGridUpdatePass",
          worldGIBaseIndex,
          worldGILightBuffer,
          worldGILightGridCellBuffer,
          worldGILightGridItemIdBuffer,
          globalillumination::worldgi::WorldGILightGridUpdatePass::Settings{
            .gridResolution = worldGILightGridResolution,
            .cascadeCount = worldGILightGridCascadeCount,
            .maxItemsPerCell = worldGILightGridMaxItemsPerCell,
            .lightCapacity = kWorldGILightCapacity,
            .frameIndex = 0u,
            .enabled = useWorldGI && editableRebuildSettings.worldGILightGridEnabled,
            .gridCenter = cameraPosition,
            .baseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
            .cascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
            .directionalLightDirection = lightDirection,
            .directionalLightColor = math::Vec3f(directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]),
            .directionalLightIntensity = directionalLightIntensity,
            .directionalLightEnabled = directionalLightEnabled,
            .pointLightPosition = math::Vec3f(pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]),
            .pointLightColor = math::Vec3f(pointLightColor[0], pointLightColor[1], pointLightColor[2]),
            .pointLightRange = pointLightRange,
            .pointLightIntensity = pointLightIntensity,
            .pointLightEnabled = pointLightEnabled,
          })));
      worldGIRayGenPass = static_cast<globalillumination::worldgi::WorldGIRayGenPass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGIRayGenPass>(
          "worldGIRayGenPass",
          worldGIBaseIndex + 1u,
          rendererPass->getSceneDepthTexture(),
          rendererPass->getGeometryNormalTexture(),
          baseColorTexture,
          materialPropertiesTexture,
          worldGIRayBuffer,
          worldGISdfRayBuffer,
          globalillumination::worldgi::WorldGIRayGenPass::Settings{
            .viewPortWidth = kRenderWidth,
            .viewPortHeight = kRenderHeight,
            .rayGridWidth = worldGIRayGridWidth,
            .rayGridHeight = worldGIRayGridHeight,
            .raysPerPixel = worldGIRaysPerPixel,
            .frameIndex = 0u,
            .enabled = useWorldGI,
            .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
            .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
            .reverseZ = cam.isReverseZ(),
            .minTraceDistance = editableRebuildSettings.worldGIRayMinDistance,
            .maxTraceDistance = editableRebuildSettings.worldGIRayMaxDistance,
            .normalBias = editableRebuildSettings.worldGIRayNormalBias,
            .glossyMaxRoughness = editableRebuildSettings.worldGIGlossyMaxRoughness,
          })));
      if (gpuSignedDistanceFieldsManager != nullptr && editableRebuildSettings.enableSignedDistanceFields)
      {
        worldGISdfTracePass = static_cast<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass *>(ownPass(renderGraph->registerPass<gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass>(
            "worldGISdfTracePass",
            worldGIBaseIndex + 2u,
            *gpuSignedDistanceFieldsManager,
            worldGISdfRayBuffer,
            worldGISdfTraceHitBuffer,
            gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
              .rayCount = worldGIRayCount,
              .rayOffset = 0u,
              .hitOffset = 0u,
              .maxSteps = editableRebuildSettings.worldGIRayMaxSteps,
              .maxTraceDistance = editableRebuildSettings.worldGIRayMaxDistance,
              .hitThresholdScale = screenProbeSdfHitThresholdScale,
              .emptySpaceStepScale = screenProbeSdfEmptySpaceStepScale,
              .normalBias = editableRebuildSettings.worldGIRayNormalBias,
              .cascadeOverride = -1,
              .traceStatsEnabled = false,
              .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
            },
            worldGITraceStatsBuffer)));
        worldGISdfMaterialResolvePass = static_cast<globalillumination::SDFRayTraceEvaluateMaterialPass *>(ownPass(renderGraph->registerPass<globalillumination::SDFRayTraceEvaluateMaterialPass>(
            "worldGISdfMaterialResolvePass",
            worldGIBaseIndex + 3u,
            scene,
            worldGISdfRayBuffer,
            worldGISdfTraceHitBuffer,
            worldGIFailedRayFallbackBuffer,
            rendererPass->getGeometryNormalTexture(),
            baseColorTexture,
            worldGITraceShadingBuffer,
            globalillumination::SDFRayTraceEvaluateMaterialPass::Settings{
              .rayCount = worldGIRayCount,
              .enabled = useWorldGI,
              .feedbackRequestCapacity = 0u,
              .feedbackUploadBudget = 0u,
              .virtualTextureSamplingEnabled = false,
            })));
        worldGITraceSurfaceResolvePass = static_cast<globalillumination::worldgi::WorldGITraceSurfaceResolvePass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGITraceSurfaceResolvePass>(
            "worldGITraceSurfaceResolvePass",
            worldGIBaseIndex + 4u,
            worldGITraceShadingBuffer,
            worldGITraceSurfaceBuffer,
            globalillumination::worldgi::WorldGITraceSurfaceResolvePass::Settings{
              .surfaceCount = worldGIRayCount,
              .sourceOverride = globalillumination::worldgi::GITraceSurfaceSource_SDF,
              .enabled = useWorldGI,
            })));
      }
      worldGIHitShadingPass = static_cast<globalillumination::worldgi::WorldGIHitShadingPass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGIHitShadingPass>(
          "worldGIHitShadingPass",
          worldGIBaseIndex + 5u,
          worldGIRayBuffer,
          worldGITraceSurfaceBuffer,
          worldGILightBuffer,
          worldGILightGridCellBuffer,
          worldGILightGridItemIdBuffer,
          worldGIDiffuseSeedTexture,
          worldGIGlossySeedTexture,
          globalillumination::worldgi::WorldGIHitShadingPass::Settings{
            .viewPortWidth = kRenderWidth,
            .viewPortHeight = kRenderHeight,
            .rayGridWidth = worldGIRayGridWidth,
            .rayGridHeight = worldGIRayGridHeight,
            .raysPerPixel = worldGIRaysPerPixel,
            .enabled = useWorldGI && editableRebuildSettings.enableSignedDistanceFields,
            .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
            .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
            .lightGridEnabled = editableRebuildSettings.worldGILightGridEnabled,
            .lightGridResolution = worldGILightGridResolution,
            .lightGridCascadeCount = worldGILightGridCascadeCount,
            .lightGridMaxItemsPerCell = worldGILightGridMaxItemsPerCell,
            .lightCount = kWorldGILightCapacity,
            .lightGridCenter = cameraPosition,
            .lightGridBaseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
            .lightGridCascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
            .clampValue = 32.0f,
            .diffuseScale = 1.0f,
            .glossyScale = 1.0f,
          })));

      worldGIComposePass = static_cast<globalillumination::worldgi::WorldGIComposePass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGIComposePass>(
          "worldGIComposePass",
          worldGIBaseIndex + 6u,
          worldGIDiffuseSeedTexture,
          worldGIGlossySeedTexture,
          materialPropertiesTexture,
          worldGIDiffuseLightingTexture,
          worldGIGlossyRadianceTexture,
          worldGIIndirectLightingTexture,
          globalillumination::worldgi::WorldGIComposePass::Settings{
            .viewPortWidth = kRenderWidth,
            .viewPortHeight = kRenderHeight,
            .enabled = useWorldGI,
            .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
            .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
            .indirectEnabled = editableRebuildSettings.worldGIIndirectEnabled,
            .diffuseWeight = editableRebuildSettings.worldGIDiffuseWeight,
            .glossyWeight = editableRebuildSettings.worldGIGlossyWeight,
            .indirectIntensity = editableRebuildSettings.worldGIIndirectIntensity,
            .clampValue = 32.0f,
            .glossyMaxRoughness = editableRebuildSettings.worldGIGlossyMaxRoughness,
          })));
      worldGILightGridDebugPass = static_cast<globalillumination::worldgi::WorldGILightGridDebugPass *>(ownPass(renderGraph->registerPass<globalillumination::worldgi::WorldGILightGridDebugPass>(
          "worldGILightGridDebugPass",
          worldGIBaseIndex + 7u,
          rendererPass->getSceneDepthTexture(),
          worldGILightGridOverlayTexture,
          globalillumination::worldgi::WorldGILightGridDebugPass::Settings{
            .viewPortWidth = kRenderWidth,
            .viewPortHeight = kRenderHeight,
            .enabled = worldGILightGridOverlayEnabled,
            .reverseZ = cam.isReverseZ(),
            .gridResolution = worldGILightGridResolution,
            .cascadeCount = worldGILightGridCascadeCount,
            .selectedCascade = worldGILightGridOverlayCascade,
            .lineWidth = worldGILightGridOverlayLineWidth,
            .gridCenter = cameraPosition,
            .baseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
            .cascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
          })));
    }
    auto *vsmDrawPass = vsmPass->getDrawPass();

    auto makeReadbackBuffer = [&](const std::string &name, uint64_t size) -> ReadbackBufferSet
    {
      ReadbackBufferSet bufferSet{};
      bufferSet.handle = ownBuffer(renderGraph->createBuffer(
          BufferInfo{
            .name = name,
            .size = size,
            .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
          }));
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
    const uint32_t activeVSMPageCount = vsmManager.getVirtualPageTableResolution() * vsmManager.getVirtualPageTableResolution() * std::max(1u, vsmManager.getActiveLayerCount());
    const uint64_t vsmPageClusterCountsBytes = static_cast<uint64_t>(activeVSMPageCount) * sizeof(uint32_t);
    const uint64_t vsmDrawIndirectBytes = static_cast<uint64_t>(vsmDrawPass->getVisibleClusterDrawCapacity()) * 4u * sizeof(uint32_t);
    VSMReadbackBuffers vsmReadbackBuffers{
      .allocatorCounters = makeReadbackBuffer("VSMAllocatorCounters.readback", vsmAllocatorCountersBytes),
      .drawCounters = makeReadbackBuffer("VSMDrawCounters.readback", vsmDrawCountersBytes),
      .virtualPageTable = makeReadbackBuffer("VSMVirtualPageTable.readback", vsmVirtualPageTableBytes),
      .pageClusterCounts = makeReadbackBuffer("VSMPageClusterCounts.readback", vsmPageClusterCountsBytes),
      .drawIndirect = makeReadbackBuffer("VSMDrawIndirect.readback", vsmDrawIndirectBytes),
    };
    ScreenSpaceRayTraceReadbackBuffers screenSpaceRayTraceReadbackBuffers{
      .traceStats = makeReadbackBuffer("ScreenSpaceRayTraceStats.readback", sizeof(ScreenSpaceRayTraceStatsReadback)),
    };
    SurfelReadbackBuffers surfelReadbackBuffers{
      .traceStats = makeReadbackBuffer("SurfelTraceStats.readback", gpusigneddistancefields::gpgpu::getTraceStatsBufferSize()),
    };
    SoftwareRayTraceReadbackBuffers softwareRayTraceReadbackBuffers{
      .traceStats = makeReadbackBuffer("SoftwareRayTraceStats.readback", sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU)),
      .screenFallbackTraceStats = makeReadbackBuffer("ScreenFallbackSoftwareRayTraceStats.readback", sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU)),
    };

    if (screenSpaceRayTracePass != nullptr)
    {
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "copyScreenSpaceRayTraceStatsReadback",
          screenSpaceRayTraceStatsReadbackPassIndex,
          screenSpaceRayTracePass->getFailedRaysBuffer(),
          0u,
          sizeof(ScreenSpaceRayTraceStatsReadback),
          screenSpaceRayTraceReadbackBuffers.traceStats.handle,
          0u,
          sizeof(ScreenSpaceRayTraceStatsReadback)));
    }
    if (useSurfels && editableRebuildSettings.enableSignedDistanceFields && gpuSignedDistanceFieldsManager != nullptr)
    {
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "copySurfelTraceStatsReadback",
          surfelStatsReadbackPassIndex,
          surfelTraceStatsBuffer,
          0u,
          gpusigneddistancefields::gpgpu::getTraceStatsBufferSize(),
          surfelReadbackBuffers.traceStats.handle,
          0u,
          gpusigneddistancefields::gpgpu::getTraceStatsBufferSize()));
    }
    if (softwareRayTracePass != nullptr)
    {
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "copySoftwareRayTraceStatsReadback",
          softwareRayTraceStatsReadbackPassIndex,
          softwareRayTracePass->getStatsBuffer(),
          0u,
          sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU),
          softwareRayTraceReadbackBuffers.traceStats.handle,
          0u,
          sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU)));
    }
    if (screenSpaceRayTraceSoftwareFallbackPass != nullptr)
    {
      ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "copyScreenFallbackRayTraceStatsReadback",
          screenFallbackTraceStatsReadbackPassIndex,
          screenSpaceRayTraceSoftwareFallbackPass->getStatsBuffer(),
          0u,
          sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU),
          softwareRayTraceReadbackBuffers.screenFallbackTraceStats.handle,
          0u,
          sizeof(softwarebvh::rendering::SoftwareRayTracePass::StatsGPU)));
    }
    ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "copyVSMAllocatorCountersReadback", 34, vsmManager.getAllocatorCountersBuffer(), 0, vsmAllocatorCountersBytes, vsmReadbackBuffers.allocatorCounters.handle, 0, vsmAllocatorCountersBytes));
    ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "copyVSMDrawCountersReadback", 35, vsmDrawPass->getCullingStatisticsBuffer(), 0, vsmDrawCountersBytes, vsmReadbackBuffers.drawCounters.handle, 0, vsmDrawCountersBytes));
    ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "copyVSMVirtualPageTableReadback", 36, vsmManager.getVirtualPageTableBuffer(), 0, vsmVirtualPageTableBytes, vsmReadbackBuffers.virtualPageTable.handle, 0, vsmVirtualPageTableBytes));
    ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "copyVSMPageClusterCountsReadback", 37, vsmDrawPass->getPageClusterCountsBuffer(), 0, vsmPageClusterCountsBytes, vsmReadbackBuffers.pageClusterCounts.handle, 0, vsmPageClusterCountsBytes));
    ownPass(renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "copyVSMDrawIndirectReadback", 38, vsmDrawPass->getShadowDrawIndirectBuffer(), 0, vsmDrawIndirectBytes, vsmReadbackBuffers.drawIndirect.handle, 0, vsmDrawIndirectBytes));

#ifdef COLLECT_FRAME_STATISTICS
    ownPass(renderGraph->registerPass<rendering::passes::FrameStatisticsHeatmapPass>(
        "hardwareFrameStatisticsHeatmapPass",
        30,
        hwDrawPass->getFrameStatisticsBuffer(),
        hardwareFrameStatisticsHeatmapTexture,
        rendering::passes::FrameStatisticsHeatmapPass::Settings{
          .width = kRenderWidth,
          .height = kRenderHeight,
        }));
#endif

    // --------------------------------------------------------------------------
    // [7] Swapchain clear pass + selected debug target view
    // --------------------------------------------------------------------------
    rendering::passes::ColorQuadPass::Settings clearSettings;
    clearSettings.viewPortWidth = kVW;
    clearSettings.viewPortHeight = kVH;
    clearSettings.region = Rect2D(0, 0, kVW, kVH);
    clearSettings.color = Color::rgba(0, 0, 0, 1);
    ownPass(renderGraph->registerPass<rendering::passes::ColorQuadPass>("clearSwapchain", finalPresentBaseIndex, swapChainTexture, rhi->getSwapChainFormat(swapChain), clearSettings));
    const Texture shadedColorTexture = (kTaaEnabled && taaResolvePass != nullptr) ? taaResolvePass->getOutputTexture() : rendererPass->getColorTexture();
    const Texture selectedSoftwareRayTraceDebugTexture =
        rendererDisplayOutputIsSoftwareRayTracingAlbedo(editableRebuildSettings.displayOutput) && softwareRayTraceAlbedoDebugTexture.isValid()
            ? softwareRayTraceAlbedoDebugTexture
            : softwareRayTraceDebugTexture;
    SelectedDebugTargetPass::Inputs displayInputs{
      .shadedColor = shadedColorTexture,
      .debugColor = rendererPass->getColorTexture(),
      .baseColor = baseColorTexture,
      .geometryNormal = geometryNormalTexture,
      .normal = normalTexture,
      .prepassDepth = rendererPass->getPrepassDepthHZBTexture(),
      .sceneDepth = rendererPass->getSceneDepthTexture(),
#ifdef COLLECT_FRAME_STATISTICS
      .overdraw = hardwareFrameStatisticsHeatmapTexture,
#else
      .overdraw = fallbackColorTexture,
#endif
      .materialId = rendererPass->getMaterialIdTexture(),
      .motionVectors = rendererPass->getMotionVectorTexture(),
      .virtualShadowMapPages = virtualShadowMapPagesDebugTexture,
      .virtualShadowMapTable = virtualShadowMapTableDebugTexture,
      .virtualShadowMapAtlas = virtualShadowMapAtlasDebugTexture,
      .imperfectShadowMapVplPages = imperfectShadowMapVplPagesDebugTexture,
      .imperfectShadowMapVplTable = imperfectShadowMapVplTableDebugTexture,
      .imperfectShadowMapVplAtlas = imperfectShadowMapVplAtlasDebugTexture,
      .imperfectShadowMapVplPoints = imperfectShadowMapVplPointsDebugTexture,
      .virtualShadowMapShadowLighting = virtualShadowMapShadowLightingTexture,
      .virtualShadowMapScreenSpaceShadow = virtualShadowMapScreenSpaceShadowTexture,
      .virtualShadowMapScreenSpaceSoftShadow = virtualShadowMapScreenSpaceSoftShadowDebugTexture,
      .packedGeometryIdsLo = packedGeometryIdsLoTexture,
      .packedGeometryIdsHi = packedGeometryIdsHiTexture,
      .signedDistanceFieldsTrace = signedDistanceFieldsTraceDebugTexture.isValid() ? signedDistanceFieldsTraceDebugTexture : fallbackColorTexture,
      .signedDistanceFieldsAtlas = editableRebuildSettings.enableSignedDistanceFields ? signedDistanceFieldsAtlasDebugTexture : fallbackColorTexture,
      .signedDistanceFieldsSurfaceCacheAlbedoAtlas = fallbackColorTexture,
      .signedDistanceFieldsSurfaceCacheNormalAtlas = fallbackColorTexture,
      .signedDistanceFieldsFallbackScene = gpuSignedDistanceFieldsFallbackDebugDrawPass != nullptr ? signedDistanceFieldsFallbackSceneDebugTexture : fallbackColorTexture,
      .softwareRayTrace = (kRendererEnableSoftwareRayTracingPath && editableRebuildSettings.enableSoftwareRayTracing) ? selectedSoftwareRayTraceDebugTexture : fallbackColorTexture,
      .disocclusionMask = (kTaaEnabled && taaResolvePass != nullptr) ? taaResolvePass->getDisocclusionMaskTexture() : fallbackColorTexture,
      .screenProbes = fallbackColorTexture,
      .spatialRadianceCache = fallbackColorTexture,
      .screenProbeIndirectDiffuse = fallbackColorTexture,
      .screenProbeBentNormal = fallbackColorTexture,
      .screenProbeAmbientOcclusion = fallbackColorTexture,
      .screenProbeAODebug = fallbackColorTexture,
      .rsmIndirectDiffuse = rsmIndirectDiffuseTexture,
      .rsmIndirectDiffuseDenoised = rsmIndirectDiffuseDenoisedTexture,
      .worldGIDiffuseLighting = worldGIDiffuseLightingTexture,
      .worldGIGlossyRadiance = worldGIGlossyRadianceTexture,
      .worldGIIndirectLighting = worldGIIndirectLightingTexture,
      .spatialRadianceReservoirCache = fallbackColorTexture,
      .surfelHierarchy = surfelHierarchyDebugTexture,
      .surfelDirectDraw = surfelDirectDrawDebugTexture,
      .surfelHeatmap = surfelHeatmapDebugTexture,
      .screenSpaceRayTraceSteps = rendererPass->getScreenSpaceRayTraceStepTexture(),
    };

    auto *selectedDebugTargetPass = static_cast<SelectedDebugTargetPass *>(ownPass(renderGraph->registerPass<SelectedDebugTargetPass>(
        "selectedDebugTargetPass",
        finalPresentBaseIndex + 1u,
        swapChainTexture,
        rhi->getSwapChainFormat(swapChain),
        displayInputs,
        SelectedDebugTargetPass::Settings{
          .viewPortWidth = kVW,
          .viewPortHeight = kVH,
        })));
    auto *vsmPagesOverlayPass = static_cast<VsmPagesOverlayPass *>(ownPass(renderGraph->registerPass<VsmPagesOverlayPass>(
        "vsmPagesOverlayPass",
        finalPresentBaseIndex + 2u,
        swapChainTexture,
        rhi->getSwapChainFormat(swapChain),
        virtualShadowMapPagesDebugTexture,
        VsmPagesOverlayPass::Settings{
          .viewPortWidth = kVW,
          .viewPortHeight = kVH,
        })));
    auto *spatialRadianceCacheOverlayPass = static_cast<VsmPagesOverlayPass *>(ownPass(renderGraph->registerPass<VsmPagesOverlayPass>(
        "spatialRadianceCacheOverlayPass",
        finalPresentBaseIndex + 3u,
        swapChainTexture,
        rhi->getSwapChainFormat(swapChain),
        spatialRadianceCacheDebugTexture,
        VsmPagesOverlayPass::Settings{
          .viewPortWidth = kVW,
          .viewPortHeight = kVH,
        })));
    VsmPagesOverlayPass *ismVplPointsOverlayPass = nullptr;
    if (useIsmVplPath)
    {
      ismVplPointsOverlayPass = static_cast<VsmPagesOverlayPass *>(ownPass(renderGraph->registerPass<VsmPagesOverlayPass>(
          "ismVplPointsOverlayPass",
          finalPresentBaseIndex + 4u,
          swapChainTexture,
          rhi->getSwapChainFormat(swapChain),
          imperfectShadowMapVplPointsDebugTexture,
          VsmPagesOverlayPass::Settings{
            .viewPortWidth = kVW,
            .viewPortHeight = kVH,
          })));
    }
    auto *worldGILightGridOverlayPass = static_cast<VsmPagesOverlayPass *>(ownPass(renderGraph->registerPass<VsmPagesOverlayPass>(
        "worldGILightGridOverlayPass",
        finalPresentBaseIndex + 5u,
        swapChainTexture,
        rhi->getSwapChainFormat(swapChain),
        worldGILightGridOverlayTexture,
        VsmPagesOverlayPass::Settings{
          .viewPortWidth = kVW,
          .viewPortHeight = kVH,
        })));
    auto updateVsmPagesOverlay = [&]()
    {
      const bool showVsmPagesOverlay = vsmPagesOverlayEnabled && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_ShadedColor;
      const bool showVsmPagesOutput = selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_VirtualShadowMapPages;
      vsmPagesOverlayPass->setEnabled(showVsmPagesOverlay);
      vsmPagesOverlayPass->setOpacity(vsmPagesOverlayOpacity);
      vsmPass->getBookkeepingPass()->setDebugOutputEnabled(showVsmPagesOverlay || showVsmPagesOutput);
    };
    auto updateSpatialRadianceCacheOverlay = [&]()
    {
      const bool showSpatialRadianceCacheOverlay = spatialRadianceCacheOverlayEnabled && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_ShadedColor;
      spatialRadianceCacheOverlayPass->setEnabled(showSpatialRadianceCacheOverlay);
      spatialRadianceCacheOverlayPass->setOpacity(spatialRadianceCacheOverlayOpacity);
    };
    auto updateIsmVplPointsOverlay = [&]()
    {
      const bool showIsmVplPointsOverlay = ismVplPointsOverlayEnabled && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_ShadedColor;
      if (ismVplPointsOverlayPass != nullptr)
      {
        ismVplPointsOverlayPass->setEnabled(showIsmVplPointsOverlay);
        ismVplPointsOverlayPass->setOpacity(ismVplPointsOverlayOpacity);
      }
    };
    auto updateWorldGILightGridOverlay = [&]()
    {
      const bool showWorldGIGridOverlay = worldGILightGridOverlayEnabled;
      worldGILightGridOverlayPass->setEnabled(showWorldGIGridOverlay);
      worldGILightGridOverlayPass->setOpacity(worldGILightGridOverlayOpacity);
    };
    auto updateSelectedDebugTarget = [&](uint32_t selectedSlot)
    {
      selectedDebugTargetSlot = selectedSlot;
      selectedDebugTargetPass->setSelectedSlot(selectedSlot);

      const bool showVsmTable = selectedSlot == SelectedDebugTargetPass::TargetSlot_VirtualShadowMapTable;
      const bool showVsmAtlas = selectedSlot == SelectedDebugTargetPass::TargetSlot_VirtualShadowMapAtlas;
      const bool showIsmVplPages = selectedSlot == SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplPages;
      const bool showIsmVplTable = selectedSlot == SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplTable;
      const bool showIsmVplAtlas = selectedSlot == SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplAtlas;
      const bool showIsmVplPoints = selectedSlot == SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplPoints;
      const bool showSdfFallbackScene = rendererDisplayOutputUsesFallbackScene(editableRebuildSettings.displayOutput);
      const bool showSdfTrace = selectedSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace;
      const bool showSdfAtlas = selectedSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas;
      const bool showSoftwareTrace =
          editableRebuildSettings.enableSoftwareRayTracing && (selectedSlot == SelectedDebugTargetPass::TargetSlot_SoftwareRayTrace || selectedSlot == SelectedDebugTargetPass::TargetSlot_SoftwareRayTraceHeatmap);
      updateVsmPagesOverlay();
      updateSpatialRadianceCacheOverlay();
      updateIsmVplPointsOverlay();
      updateWorldGILightGridOverlay();
      if (auto *pass = vsmPass->getTableDebugPass())
      {
        pass->setDebugLayer(vsmDebugLayer);
        pass->setEnabled(showVsmTable);
      }
      if (auto *pass = vsmPass->getAtlasDebugPass())
      {
        pass->setDebugLayer(vsmDebugLayer);
        pass->setEnabled(showVsmAtlas);
      }
      if (ismVplPass != nullptr)
      {
        if (auto *pass = ismVplPass->getTableDebugPass())
        {
          pass->setDebugLayer(ismVplDebugLayer);
          pass->setEnabled(showIsmVplTable);
        }
        ismVplPass->getBookkeepingPass()->setDebugOutputEnabled(showIsmVplPages);
        if (auto *pass = ismVplPass->getAtlasDebugPass())
        {
          pass->setDebugLayer(ismVplDebugLayer);
          pass->setEnabled(showIsmVplAtlas);
        }
      }
      if (ismVplPointDebugPass != nullptr)
      {
        ismVplPointDebugPass->setSettings(
            ImperfectShadowMapVplPointDebugPass::Settings{
                .width = kRenderWidth,
                .height = kRenderHeight,
                .viewProjection = ismVplDebugViewProjection,
                .selectedVpl = ismVplDebugSelectedVpl,
                .pointRadiusPixels = ismVplDebugPointRadius,
                .exposure = ismVplDebugExposure,
                .enabled = showIsmVplPoints || (ismVplPointsOverlayEnabled && selectedSlot == SelectedDebugTargetPass::TargetSlot_ShadedColor),
            });
      }
      if (gpuSignedDistanceFieldsFallbackDebugPreparePass != nullptr)
        gpuSignedDistanceFieldsFallbackDebugPreparePass->setEnabled(showSdfFallbackScene);
      if (gpuSignedDistanceFieldsFallbackDebugDrawPass != nullptr)
      {
        gpuSignedDistanceFieldsFallbackDebugDrawPass->setMode(rendererDisplayOutputFallbackSceneMode(editableRebuildSettings.displayOutput));
        gpuSignedDistanceFieldsFallbackDebugDrawPass->setEnabled(showSdfFallbackScene);
      }
      const auto selectedLegacyTraceMode = rendererDisplayOutputSdfTraceMode(editableRebuildSettings.displayOutput, sdfTraceDebugSettings.mode);
      const bool showSparseSdfTrace =
          showSdfTrace &&
          sparseSdfTraceDebugPass != nullptr &&
          (rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput) || sdfTraceModeUsesLocalInstanceSdf(selectedLegacyTraceMode));
      if (sparseSdfTraceDebugPass != nullptr)
      {
        sparseSdfTraceDebugSettings.mode =
            rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput)
                ? rendererDisplayOutputSparseSdfTraceMode(editableRebuildSettings.displayOutput, sparseSdfTraceDebugSettings.mode)
                : sparseSdfTraceModeFromLegacyTraceMode(selectedLegacyTraceMode, sparseSdfTraceDebugSettings.mode);
        sparseSdfTraceDebugPass->setSettings(sparseSdfTraceDebugSettings);
        sparseSdfTraceDebugPass->setEnabled(showSparseSdfTrace);
      }
	      if (gpuSignedDistanceFieldsTraceDebugPass != nullptr)
	      {
	        sdfTraceDebugSettings.mode = rendererDisplayOutputSdfTraceMode(editableRebuildSettings.displayOutput, sdfTraceDebugSettings.mode);
	        if (rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput))
	          sdfTraceDebugSettings.maxSteps = std::max(sdfTraceDebugSettings.maxSteps, 256u);
	        gpuSignedDistanceFieldsTraceDebugPass->setSettings(sdfTraceDebugSettings);
	        gpuSignedDistanceFieldsTraceDebugPass->setEnabled(showSdfTrace && !showSparseSdfTrace);
	      }
      if (gpuSignedDistanceFieldsAtlasDebugPass != nullptr)
        gpuSignedDistanceFieldsAtlasDebugPass->setEnabled(showSdfAtlas);
      if (softwareRayTracePass != nullptr)
        softwareRayTracePass->setEnabled(showSoftwareTrace);
    };
    updateSelectedDebugTarget(rendererDisplayOutputTargetSlot(editableRebuildSettings.displayOutput));

    if (softwareBlasSceneBridge != nullptr)
    {
      const auto bridgeSettings = makeSoftwareRaytraceBlasSceneSettings();
      sceneManager.syncSoftwareRaytraceScene(bridgeSettings, makeSoftwareRaytraceTlasFilter());
      const auto &swrtBlasDiagnostics = softwareBlasSceneBridge->getDiagnostics();
      const auto &swrtBlasBuild = softwareBlasManager->getLastBuildSummary();
      os::Logger::logf(
          "[SWRT] graph build traversal=%s primitive=%s fallbackLod=%u rbvhLod=%u prepared=%u unique=%u skippedMissingFallback=%u rebuild=%s runtimeRbvhBuilds=%u originalSources=%u proxySources=%u sourceTriangles=%llu buildMs=%.2f blas=%u cards=%u atlas=%ux%u texels=%u",
          softwareRayTraceTraversalMode == 1u ? "WorldCWBVH" : "LocalInstance",
          bridgeSettings.primitiveMode == softwarebvh::SoftwareBLASSceneBridge::PrimitiveMode::RBVHCard ? "RBVHCard" : "Triangle",
          bridgeSettings.fallbackLod,
          bridgeSettings.rbvhLod,
          swrtBlasDiagnostics.preparedMeshCount,
          swrtBlasDiagnostics.uniqueMeshCount,
          swrtBlasDiagnostics.skippedMissingFallbackMeshCount,
          swrtBlasDiagnostics.requestedRebuild ? "yes" : "no",
          swrtBlasDiagnostics.runtimeRbvhBuildCount,
          swrtBlasDiagnostics.runtimeRbvhOriginalSourceCount,
          swrtBlasDiagnostics.runtimeRbvhFallbackSourceCount,
          static_cast<unsigned long long>(swrtBlasDiagnostics.runtimeRbvhSourceTriangleCount),
          swrtBlasDiagnostics.runtimeRbvhBuildMilliseconds,
          swrtBlasBuild.blasCount,
          swrtBlasBuild.rbvhCardCount,
          softwareBlasManager->getRbvhDepthAtlasTextureWidth(),
          softwareBlasManager->getRbvhDepthAtlasTextureHeight(),
          softwareBlasManager->getRbvhDepthAtlasTextureTexelCount());
    }

    os::Logger::log("[8] Compiling render graph...");
    renderGraph->compile();
    if (softwareTlasSortPass != nullptr)
    {
      softwareTlasSortPass->ensureDispatchTimerAllocated();
    }
    std::vector<std::vector<RenderGraph::CommandBufferHandle>> swapChainInitCommandBufferHandles;
    std::vector<std::vector<RenderGraph::CommandBufferHandle>> swapChainCommandBufferHandles;
    swapChainInitCommandBufferHandles.resize(maxFramesInFlight);
    swapChainCommandBufferHandles.resize(maxFramesInFlight);
    for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
    {
      auto &initFrameHandles = swapChainInitCommandBufferHandles[frameSlot];
      auto &frameHandles = swapChainCommandBufferHandles[frameSlot];
      initFrameHandles.reserve(rhi->getSwapChainImagesCount(swapChain));
      frameHandles.reserve(rhi->getSwapChainImagesCount(swapChain));
      for (uint32_t imageIndex = 0u; imageIndex < rhi->getSwapChainImagesCount(swapChain); ++imageIndex)
      {
        RenderGraph::Overrides overrides;
        const auto swapChainView = rhi->getSwapChainTextureView(swapChain, imageIndex);
        const uint32_t previousFrameSlot = (frameSlot + maxFramesInFlight - 1u) % maxFramesInFlight;
        sceneManager.appendFrameOverrides(overrides, frameSlot);
        preCullingPass->appendFrameOverrides(overrides, frameSlot);
        depthPrePass->appendFrameOverrides(overrides, frameSlot);
        finalCullingPass->appendFrameOverrides(overrides, frameSlot);
        hwDrawPass->appendFrameOverrides(overrides, frameSlot);
        if (screenSpaceRayTracePass != nullptr)
          screenSpaceRayTracePass->appendFrameOverrides(overrides, frameSlot);
        materialPass->appendFrameOverrides(overrides, frameSlot);
        if (taaResolvePass != nullptr)
          taaResolvePass->appendFrameOverrides(overrides, frameSlot);
        if (taaHistoryPass != nullptr)
          taaHistoryPass->appendFrameOverrides(overrides, frameSlot);
	        if (surfelHierarchyDebugPass != nullptr)
	          surfelHierarchyDebugPass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGILightGridUpdatePass != nullptr)
	          worldGILightGridUpdatePass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGIRayGenPass != nullptr)
	          worldGIRayGenPass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGISdfTracePass != nullptr)
	          worldGISdfTracePass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGISdfMaterialResolvePass != nullptr)
	          worldGISdfMaterialResolvePass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGIHitShadingPass != nullptr)
	          worldGIHitShadingPass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGIComposePass != nullptr)
	          worldGIComposePass->appendFrameOverrides(overrides, frameSlot);
	        if (worldGILightGridDebugPass != nullptr)
	          worldGILightGridDebugPass->appendFrameOverrides(overrides, frameSlot);
        if (gpuSignedDistanceFieldsPass != nullptr)
          gpuSignedDistanceFieldsPass->appendFrameOverrides(overrides, frameSlot);
        if (gpuSignedDistanceFieldsTraceDebugPass != nullptr)
          gpuSignedDistanceFieldsTraceDebugPass->appendFrameOverrides(overrides, frameSlot);
        if (sdfHitShadingResolvePass != nullptr)
          sdfHitShadingResolvePass->appendFrameOverrides(overrides, frameSlot);
        if (worldHitShadingResolvePass != nullptr)
          worldHitShadingResolvePass->appendFrameOverrides(overrides, frameSlot);
        if (softwareRayTraceShadingResolvePass != nullptr)
          softwareRayTraceShadingResolvePass->appendFrameOverrides(overrides, frameSlot);
        if (softwareTlasRefitCanonicalPass != nullptr)
          softwareTlasRefitCanonicalPass->appendFrameOverrides(overrides, frameSlot);
        if (softwareTlasRefitCwbvhPass != nullptr)
          softwareTlasRefitCwbvhPass->appendFrameOverrides(overrides, frameSlot);
        if (screenSpaceRayTraceSoftwareFallbackPass != nullptr)
          screenSpaceRayTraceSoftwareFallbackPass->appendFrameOverrides(overrides, frameSlot);
        if (softwareRayTracePass != nullptr)
          softwareRayTracePass->appendFrameOverrides(overrides, frameSlot);
        vsmManager.appendFrameOverrides(overrides, frameSlot);
        overrides.bufferOverrides.emplace(vsmReadbackBuffers.allocatorCounters.handle.name, RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.allocatorCounters.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(vsmReadbackBuffers.drawCounters.handle.name, RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.drawCounters.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(vsmReadbackBuffers.virtualPageTable.handle.name, RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.virtualPageTable.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(vsmReadbackBuffers.pageClusterCounts.handle.name, RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.pageClusterCounts.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(vsmReadbackBuffers.drawIndirect.handle.name, RenderGraph::BufferOverride{.bufferId = vsmReadbackBuffers.drawIndirect.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(screenSpaceRayTraceReadbackBuffers.traceStats.handle.name, RenderGraph::BufferOverride{.bufferId = screenSpaceRayTraceReadbackBuffers.traceStats.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(surfelReadbackBuffers.traceStats.handle.name, RenderGraph::BufferOverride{.bufferId = surfelReadbackBuffers.traceStats.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(softwareRayTraceReadbackBuffers.traceStats.handle.name, RenderGraph::BufferOverride{.bufferId = softwareRayTraceReadbackBuffers.traceStats.frameIds[frameSlot]});
        overrides.bufferOverrides.emplace(softwareRayTraceReadbackBuffers.screenFallbackTraceStats.handle.name, RenderGraph::BufferOverride{.bufferId = softwareRayTraceReadbackBuffers.screenFallbackTraceStats.frameIds[frameSlot]});
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
            baseColorTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameBaseColorTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            geometryNormalTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameGeometryNormalTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            normalTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameNormalTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            packedGeometryIdsLoTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFramePackedGeometryIdsLoTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            packedGeometryIdsHiTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFramePackedGeometryIdsHiTextureIds[frameSlot],
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
            motionVectorTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameMotionVectorTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            taaDisocclusionMaskTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameTaaDisocclusionMaskTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            spatialRadianceCacheDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameSpatialRadianceCacheDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            spatialRadianceReservoirCacheDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameSpatialRadianceReservoirCacheDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        if (taaResolvePass != nullptr && taaHistoryPass != nullptr)
        {
          overrides.textureOverrides.emplace(
              taaColorTexture.name,
              RenderGraph::TextureOverride{
                .textureId = perFrameTaaColorTextureIds[frameSlot],
                .layout = rendering::ResourceLayout::UNDEFINED,
              });
          if (useTaaHistoryRing)
          {
            // Resolve samples the previous history slot while the current frame writes
            // a different slot, avoiding the need to serialize the pass across frames.
            overrides.textureOverrides.emplace(
                taaHistoryColorTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryColorTextureIds[previousFrameSlot],
                  .layout = rendering::ResourceLayout::GENERAL,
                });
            overrides.textureOverrides.emplace(
                taaHistoryDepthTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryDepthTextureIds[previousFrameSlot],
                  .layout = rendering::ResourceLayout::GENERAL,
                });
            overrides.textureOverrides.emplace(
                taaHistoryIdsTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryIdsTextureIds[previousFrameSlot],
                  .layout = rendering::ResourceLayout::GENERAL,
                });
            overrides.textureOverrides.emplace(
                taaHistoryNormalTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryNormalTextureIds[previousFrameSlot],
                  .layout = rendering::ResourceLayout::GENERAL,
                });
            overrides.textureOverrides.emplace(
                taaHistoryColorWriteTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryColorTextureIds[frameSlot],
                  .layout = rendering::ResourceLayout::UNDEFINED,
                });
            overrides.textureOverrides.emplace(
                taaHistoryDepthWriteTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryDepthTextureIds[frameSlot],
                  .layout = rendering::ResourceLayout::UNDEFINED,
                });
            overrides.textureOverrides.emplace(
                taaHistoryIdsWriteTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryIdsTextureIds[frameSlot],
                  .layout = rendering::ResourceLayout::UNDEFINED,
                });
            overrides.textureOverrides.emplace(
                taaHistoryNormalWriteTexture.name,
                RenderGraph::TextureOverride{
                  .textureId = perFrameTaaHistoryNormalTextureIds[frameSlot],
                  .layout = rendering::ResourceLayout::UNDEFINED,
                });
          }
        }
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
            imperfectShadowMapVplPagesDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameIsmVplPagesDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            imperfectShadowMapVplTableDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameIsmVplTableDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            imperfectShadowMapVplAtlasDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameIsmVplAtlasDebugTextureIds[frameSlot],
              .layout = rendering::ResourceLayout::UNDEFINED,
            });
        overrides.textureOverrides.emplace(
            imperfectShadowMapVplPointsDebugTexture.name,
            RenderGraph::TextureOverride{
              .textureId = perFrameIsmVplPointsDebugTextureIds[frameSlot],
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
        initFrameHandles.push_back(renderGraph->createCommandBuffer(overrides, frameSlot, false));
        frameHandles.push_back(renderGraph->createCommandBuffer(overrides, frameSlot, true));
      }
    }
    {
      os::Logger::logf("[Display] active output=%s", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput));
    }
    appliedRebuildSettings = editableRebuildSettings;

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
    bool statsOverlayLatch = false;
    OverlayPreparationWorker overlayPreparationWorker{renderGraph};
    std::shared_ptr<const PreparedOverlayState> latestOverlaySnapshot{};
    SurfelIrradianceDebugStats latestSurfelIrradianceDebugStats{};
    ScreenProbeTraceStats latestScreenProbeTraceStats{};
    ScreenSpaceRayTraceStats latestScreenSpaceRayTraceStats{};
    gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTraceDebugPass::LocalTraceStats latestSdfLocalTraceStats{};
    std::deque<PendingFrameDiagnostics> pendingFrameDiagnostics;
    auto updateFrameMetricCollection = [&]()
    {
      renderGraph->setTimerReadbackEnabled(showStatsOverlay && !isPaused);
    };
    updateFrameMetricCollection();
    uint32_t lastFallbackSceneDiagnosticsLogDisplayFrame = 0u;
    uint32_t lastSwrtDiagnosticsLogDisplayFrame = 0u;
    uint32_t lastVirtualTextureDiagnosticsLogDisplayFrame = 0u;

    auto logFallbackSceneDiagnostics = [&](const PendingFrameDiagnostics &pending)
    {
      if (!pending.fallbackScene.capture || gpuSignedDistanceFieldsFallbackDebugPreparePass == nullptr)
      {
        return;
      }

      using DrawIndirectCommandGPU = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass::DrawIndirectCommandGPU;
      struct PathSummary
      {
        uint32_t totalCommands = 0u;
        uint32_t activeCommands = 0u;
        uint32_t zeroCommands = 0u;
        uint32_t invalidInstanceRefs = 0u;
        uint32_t invalidMeshRefs = 0u;
        uint32_t invalidVertexRanges = 0u;
        uint32_t invalidIndexRanges = 0u;
        std::vector<DrawIndirectCommandGPU> commands{};
      };

      const auto summarizePath =
          [&](gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path path, uint32_t drawCount, const std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> &instances) -> PathSummary
      {
        PathSummary summary{};
        summary.totalCommands = drawCount;
        summary.commands = path == gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned ? pending.fallbackScene.preparedSkinnedCommands : pending.fallbackScene.preparedStaticCommands;
        for (const DrawIndirectCommandGPU &command : summary.commands)
        {
          if (command.instanceCount == 0u || command.vertexCount == 0u)
          {
            ++summary.zeroCommands;
            continue;
          }

          ++summary.activeCommands;
          if (command.firstInstance >= instances.size())
          {
            ++summary.invalidInstanceRefs;
            continue;
          }

          const auto &instance = instances[command.firstInstance];
          if (instance.meshIndex >= pending.fallbackScene.meshTable.size())
          {
            ++summary.invalidMeshRefs;
            continue;
          }

          const auto &mesh = pending.fallbackScene.meshTable[instance.meshIndex];
          if (mesh.vertexOffset + mesh.vertexCount > pending.fallbackScene.uploadedVertexCount)
          {
            ++summary.invalidVertexRanges;
          }
          if (mesh.indexOffset + mesh.triangleCount * 3u > pending.fallbackScene.uploadedIndexCount)
          {
            ++summary.invalidIndexRanges;
          }
        }
        return summary;
      };

      const PathSummary staticSummary = summarizePath(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static, pending.fallbackScene.staticDrawCount, pending.fallbackScene.staticInstances);
      const PathSummary skinnedSummary = summarizePath(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned, pending.fallbackScene.skinnedDrawCount, pending.fallbackScene.skinnedInstances);

      const bool hasProblems = (staticSummary.activeCommands == 0u && skinnedSummary.activeCommands == 0u) || staticSummary.invalidInstanceRefs > 0u || staticSummary.invalidMeshRefs > 0u ||
                               staticSummary.invalidVertexRanges > 0u || staticSummary.invalidIndexRanges > 0u || skinnedSummary.invalidInstanceRefs > 0u || skinnedSummary.invalidMeshRefs > 0u ||
                               skinnedSummary.invalidVertexRanges > 0u || skinnedSummary.invalidIndexRanges > 0u;
      const bool periodicLog = lastFallbackSceneDiagnosticsLogDisplayFrame == 0u || pending.displayFrameIndex >= lastFallbackSceneDiagnosticsLogDisplayFrame + 30u;
      if (!hasProblems && !periodicLog)
      {
        return;
      }

      lastFallbackSceneDiagnosticsLogDisplayFrame = pending.displayFrameIndex;

      const auto &bridgeDiagnostics = pending.fallbackScene.bridgeDiagnostics;
      os::Logger::logf(
          "[FallbackScene] frame=%u prepare=%s culling=%s fallbackMeshes loaded=%u missing=%u prepared=%u staticInstances=%zu skinnedInstances=%zu uploadedMeshes=%zu uploadedVertices=%u uploadedIndices=%u "
          "uploadedBoneWeights=%u",
          pending.displayFrameIndex,
          pending.fallbackScene.preparePassEnabled ? "on" : "off",
          pending.fallbackScene.frustumCullingEnabled ? "on" : "off",
          bridgeDiagnostics.availableFallbackMeshCount,
          bridgeDiagnostics.missingFallbackMeshCount,
          bridgeDiagnostics.preparedInstanceCount,
          pending.fallbackScene.staticInstances.size(),
          pending.fallbackScene.skinnedInstances.size(),
          pending.fallbackScene.meshTable.size(),
          pending.fallbackScene.uploadedVertexCount,
          pending.fallbackScene.uploadedIndexCount,
          pending.fallbackScene.uploadedBoneWeightCount);
      os::Logger::logf(
          "[FallbackScene][Static] commands active=%u/%u zero=%u invalidInstance=%u invalidMesh=%u invalidVertexRange=%u invalidIndexRange=%u",
          staticSummary.activeCommands,
          staticSummary.totalCommands,
          staticSummary.zeroCommands,
          staticSummary.invalidInstanceRefs,
          staticSummary.invalidMeshRefs,
          staticSummary.invalidVertexRanges,
          staticSummary.invalidIndexRanges);
      os::Logger::logf(
          "[FallbackScene][Skinned] commands active=%u/%u zero=%u invalidInstance=%u invalidMesh=%u invalidVertexRange=%u invalidIndexRange=%u",
          skinnedSummary.activeCommands,
          skinnedSummary.totalCommands,
          skinnedSummary.zeroCommands,
          skinnedSummary.invalidInstanceRefs,
          skinnedSummary.invalidMeshRefs,
          skinnedSummary.invalidVertexRanges,
          skinnedSummary.invalidIndexRanges);

      const uint32_t meshEntriesToLog = std::min<uint32_t>(static_cast<uint32_t>(pending.fallbackScene.meshTable.size()), 4u);
      for (uint32_t meshIndex = 0u; meshIndex < meshEntriesToLog; ++meshIndex)
      {
        const auto &mesh = pending.fallbackScene.meshTable[meshIndex];
        os::Logger::logf(
            "[FallbackScene][Mesh %u] vertexOffset=%u vertexCount=%u indexOffset=%u triangleCount=%u boneWeightsPerVertex=%u flags=0x%X",
            meshIndex,
            mesh.vertexOffset,
            mesh.vertexCount,
            mesh.indexOffset,
            mesh.triangleCount,
            mesh.boneWeightsPerVertex,
            mesh.flags);
      }

      const auto logInstanceDetails = [&](const char *label, const std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> &instances)
      {
        const uint32_t instanceCountToLog = std::min<uint32_t>(static_cast<uint32_t>(instances.size()), 4u);
        for (uint32_t instanceIndex = 0u; instanceIndex < instanceCountToLog; ++instanceIndex)
        {
          const auto &instance = instances[instanceIndex];
          os::Logger::logf(
              "[FallbackScene][%s Instance %u] meshIndex=%u flags=0x%X boneCount=%u aabbMin=(%.3f, %.3f, %.3f) aabbMax=(%.3f, %.3f, %.3f)",
              label,
              instanceIndex,
              instance.meshIndex,
              instance.flags,
              instance.boneCount,
              instance.aabbMin[0],
              instance.aabbMin[1],
              instance.aabbMin[2],
              instance.aabbMax[0],
              instance.aabbMax[1],
              instance.aabbMax[2]);
        }
      };
      logInstanceDetails("Static", pending.fallbackScene.staticInstances);
      logInstanceDetails("Skinned", pending.fallbackScene.skinnedInstances);

      const auto logCommandDetails = [&](const char *label, const PathSummary &summary, const std::vector<gpusigneddistancefields::GPUSignedDistanceFieldsManager::InstanceGPU> &instances)
      {
        uint32_t loggedCommands = 0u;
        for (uint32_t commandIndex = 0u; commandIndex < summary.commands.size() && loggedCommands < 4u; ++commandIndex)
        {
          const DrawIndirectCommandGPU &command = summary.commands[commandIndex];
          if (summary.activeCommands > 0u && (command.instanceCount == 0u || command.vertexCount == 0u))
          {
            continue;
          }

          std::string meshInfo = "mesh=<invalid>";
          if (command.firstInstance < instances.size())
          {
            const auto &instance = instances[command.firstInstance];
            if (instance.meshIndex < pending.fallbackScene.meshTable.size())
            {
              const auto &mesh = pending.fallbackScene.meshTable[instance.meshIndex];
              std::ostringstream oss;
              oss << "meshIndex=" << instance.meshIndex << " triCount=" << mesh.triangleCount << " vertexOffset=" << mesh.vertexOffset << " vertexCount=" << mesh.vertexCount << " indexOffset=" << mesh.indexOffset;
              meshInfo = oss.str();
            }
          }

          os::Logger::logf(
              "[FallbackScene][%s Cmd %u] vertexCount=%u instanceCount=%u firstVertex=%u firstInstance=%u %s",
              label,
              commandIndex,
              command.vertexCount,
              command.instanceCount,
              command.firstVertex,
              command.firstInstance,
              meshInfo.c_str());
          ++loggedCommands;
        }
      };
      logCommandDetails("Static", staticSummary, pending.fallbackScene.staticInstances);
      logCommandDetails("Skinned", skinnedSummary, pending.fallbackScene.skinnedInstances);

      if (pending.fallbackScene.frustumCullingEnabled && staticSummary.activeCommands == 0u && !pending.fallbackScene.staticInstances.empty())
      {
        const uint32_t instancesToInspect = std::min<uint32_t>(static_cast<uint32_t>(pending.fallbackScene.staticInstances.size()), 4u);
        for (uint32_t instanceIndex = 0u; instanceIndex < instancesToInspect; ++instanceIndex)
        {
          const auto &instance = pending.fallbackScene.staticInstances[instanceIndex];
          uint32_t rejectingPlaneIndex = UINT32_MAX;
          float rejectingDistance = 0.0f;
          const bool visible = gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsFallbackDebugPreparePass::isVisible(pending.fallbackScene.frustumPlanes, instance, &rejectingPlaneIndex, &rejectingDistance);
          os::Logger::logf("[FallbackScene][Static CPUVisibility %u] visible=%s rejectingPlane=%u rejectingDistance=%.6f", instanceIndex, visible ? "yes" : "no", rejectingPlaneIndex, rejectingDistance);
        }

        for (uint32_t planeIndex = 0u; planeIndex < pending.fallbackScene.frustumPlanes.size(); ++planeIndex)
        {
          const auto &plane = pending.fallbackScene.frustumPlanes[planeIndex];
          os::Logger::logf("[FallbackScene][Plane %u] (%.6f, %.6f, %.6f, %.6f)", planeIndex, plane[0], plane[1], plane[2], plane[3]);
        }
      }
    };

    sceneManager.updatePageStreaming(assetPaths.objectName, 1u);
    sceneManager.updateInstanceBuffer();

    auto finalizePendingFrame = [&](PendingFrameDiagnostics &pending)
    {
      const auto waitStart = lib::time::TimeSpan::now();
      renderGraph->waitFrame(pending.frame);
      pending.cpuFrame.waitMs = (lib::time::TimeSpan::now() - waitStart).milliseconds();
      pending.cpuFrame.waitBlockMs = renderGraph->getLastWaitFrameBlockSpanMs();
      pending.cpuFrame.timerReadbackMs = 0.0;
      renderGraph->setCurrentFrameIndex(pending.frame.frameIndex);
      latestScreenProbeTraceStats = {};
      latestScreenSpaceRayTraceStats = {};
      if (gpuSignedDistanceFieldsTraceDebugPass != nullptr)
        latestSdfLocalTraceStats = gpuSignedDistanceFieldsTraceDebugPass->processLocalTraceStats();
      else
        latestSdfLocalTraceStats = {};
      if (screenSpaceRayTracePass != nullptr && pending.frame.frameIndex < screenSpaceRayTraceReadbackBuffers.traceStats.frameIds.size())
      {
        const std::vector<ScreenSpaceRayTraceStatsReadback> readback =
            readBufferElements<ScreenSpaceRayTraceStatsReadback>(rhi, screenSpaceRayTraceReadbackBuffers.traceStats.frameIds[pending.frame.frameIndex], 1u);
        if (!readback.empty())
        {
          latestScreenSpaceRayTraceStats = decodeScreenSpaceRayTraceStats(readback[0]);
        }
      }
      if (useSurfels)
      {
        const std::vector<uint32_t> surfelStackReadback = readBufferElements<uint32_t>(renderGraph, surfelStackReadbackBuffer, 1u);
        if (!surfelStackReadback.empty())
          surfelAllocatedCount = surfelRuntimeSettings.surfelCapacity - std::min(surfelStackReadback[0], surfelRuntimeSettings.surfelCapacity);

        const std::vector<globalillumination::surfels::SurfelIrradianceGPU> surfelIrradianceReadback =
            readBufferElements<globalillumination::surfels::SurfelIrradianceGPU>(renderGraph, surfelIrradianceReadbackBuffer, surfelRuntimeSettings.surfelCapacity);
        latestSurfelIrradianceDebugStats = summarizeSurfelIrradiance(surfelIrradianceReadback);
      }
      else
      {
        surfelAllocatedCount = 0u;
        latestSurfelIrradianceDebugStats = {};
      }

      logFallbackSceneDiagnostics(pending);

      measureCpuOperation(
          renderGraph,
          pending.cpuFrame.vtFeedback,
          [&]()
          {
            materialPass->processFeedback();
            if (worldHitShadingResolvePass != nullptr)
              worldHitShadingResolvePass->processFeedback();
            if (softwareRayTraceShadingResolvePass != nullptr)
              softwareRayTraceShadingResolvePass->processFeedback();
            if (sdfHitShadingResolvePass != nullptr)
              sdfHitShadingResolvePass->processFeedback();
            if (worldGISdfMaterialResolvePass != nullptr)
              worldGISdfMaterialResolvePass->processFeedback(editableRebuildSettings.materialMaxUploadsPerFrame);
            if (gpuSignedDistanceFieldsTraceDebugPass != nullptr && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace)
              gpuSignedDistanceFieldsTraceDebugPass->processFeedback();
          });
      {
        const VirtualTextureFrameStats vtStats = buildVirtualTextureFrameStats(scene.getVirtualTextureSystem(), materialPass.get());
        const bool suspiciousFeedback = vtStats.textureCount > 0u && (vtStats.feedback.requested == 0u || (vtStats.feedback.deduplicated > 0u && vtStats.feedback.uploaded == 0u && vtStats.residentPages < vtStats.pageTableEntryCount));
        const bool shouldLogVtDiagnostics = suspiciousFeedback && (lastVirtualTextureDiagnosticsLogDisplayFrame == 0u || pending.displayFrameIndex >= lastVirtualTextureDiagnosticsLogDisplayFrame + 60u);
        if (shouldLogVtDiagnostics)
        {
          lastVirtualTextureDiagnosticsLogDisplayFrame = pending.displayFrameIndex;
          os::Logger::logf(
              "[VT] frame=%u feedback(requested=%u dedup=%u uploaded=%u evicted=%u overflow=%u) resident=%u/%u textures=%u materials=%u atlasPages=%ux%u pageSize=%u dirty=%s",
              pending.displayFrameIndex,
              vtStats.feedback.requested,
              vtStats.feedback.deduplicated,
              vtStats.feedback.uploaded,
              vtStats.feedback.evicted,
              vtStats.feedback.overflowed,
              vtStats.residentPages,
              vtStats.pageTableEntryCount,
              vtStats.textureCount,
              vtStats.materialCount,
              vtStats.physicalPagesPerAxis,
              vtStats.physicalPagesPerAxis,
              vtStats.pageSize,
              vtStats.hasDirtyResidency ? "yes" : "no");
        }
      }
      measureCpuOperation(
          renderGraph,
          pending.cpuFrame.sceneStreaming,
          [&]()
          {
            sceneManager.updatePageStreaming(assetPaths.objectName, 1u);
          });
      pending.cpuFrame.streamingMs = pending.cpuFrame.sceneStreaming.wallMs;
      pending.cpuFrame.vsmResetInvalidationsMs = measureWallTimeMs(
          [&]()
          {
            vsmManager.resetInvalidations();
          });
      measureCpuOperation(
          renderGraph,
          pending.cpuFrame.instanceUpload,
          [&]()
          {
            sceneManager.updateInstanceBuffer();
          });
      pending.cpuFrame.instanceUploadMs = pending.cpuFrame.instanceUpload.wallMs;

      if (showStatsOverlay && pending.collectFrameMetrics)
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
        if (gpuSignedDistanceFieldsManager)
        {
          input.sdfContextCountersBuffer = gpuSignedDistanceFieldsManager->getSharedResources().contextCounters;
          input.sdfMaxBricks = gpuSignedDistanceFieldsManager->getSettings().maxBricks;
        }
        input.collectSurfelTraceStats = useSurfels && editableRebuildSettings.enableSignedDistanceFields && gpuSignedDistanceFieldsManager != nullptr;
        input.surfelTraceStatsBufferId = surfelReadbackBuffers.traceStats.frameIds[pending.frame.frameIndex];
        input.collectSoftwareRayTraceStats = softwareRayTracePass != nullptr && rendererDisplayOutputUsesSoftwareRayTracing(editableRebuildSettings.displayOutput);
        input.softwareRayTraceStatsBufferId = softwareRayTraceReadbackBuffers.traceStats.frameIds[pending.frame.frameIndex];
        input.collectScreenFallbackTraceStats = screenSpaceRayTraceSoftwareFallbackPass != nullptr;
        input.screenFallbackTraceStatsBufferId = softwareRayTraceReadbackBuffers.screenFallbackTraceStats.frameIds[pending.frame.frameIndex];
        overlayPreparationWorker.enqueue(std::move(input));
      }
    };

    while (!window->shouldClose() && !restartRendererRequested)
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
      if (nativeWindow->width > 0u && nativeWindow->height > 0u && (nativeWindow->width != kVW || nativeWindow->height != kVH))
      {
        os::Logger::logf("[Restart] detected window size change: swapchain=%ux%u window=%ux%u; rebuilding renderer and recreating swapchain", kVW, kVH, nativeWindow->width, nativeWindow->height);
        restartRendererRequested = true;
        recreateSwapChainRequested = true;
        continue;
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

      preCullingPass->settings.errorTreshhold = prepassLodErrorThreshold;
      finalCullingPass->settings.errorTreshhold = geometryLodErrorThreshold;
      finalCullingPass->settings.swPixelsPerTriangleThreshold = finalSoftwarePixelsPerTriangleThreshold;

      if (statsOverlayKey)
      {
        if (!statsOverlayLatch)
        {
          statsOverlayLatch = true;
          showStatsOverlay = !showStatsOverlay;
          updateFrameMetricCollection();
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
          updateFrameMetricCollection();
        }
      }
      else
      {
        pauseLatch = false;
      }

      bool lightDirectionChangedThisFrame = false;
      if (lightKey)
      {
        if (!lightLatch)
        {
          lightLatch = true;
          lightDirection = (cameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f)).normalize();
          lightDirectionChangedThisFrame = true;
          os::Logger::logf("[Light] directional light aligned to camera forward: (%.3f, %.3f, %.3f)", lightDirection[0], lightDirection[1], lightDirection[2]);
        }
      }
      else
      {
        lightLatch = false;
      }

      bool shadowSettingsDirty = false;

      if (shadowTapDecreaseKey)
      {
        if (!shadowTapDecreaseLatch)
        {
          shadowTapDecreaseLatch = true;
          const int tapIndex = shadowPcfTapCountToIndex(shadowFilterTaps);
          shadowFilterTaps = shadowPcfTapIndexToCount(tapIndex - 1);
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
          const int tapIndex = shadowPcfTapCountToIndex(shadowFilterTaps);
          shadowFilterTaps = shadowPcfTapIndexToCount(tapIndex + 1);
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
        updateVsmPagesOverlay();
        updateSpatialRadianceCacheOverlay();
        updateWorldGILightGridOverlay();
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
      }
      else if (pendingFrameDiagnostics.size() >= maxFramesInFlight)
      {
        finalizePendingFrame(pendingFrameDiagnostics.front());
        pendingFrameDiagnostics.pop_front();
      }

      renderGraph->clearFrameStats();
      const RenderGraph::CpuStats renderGraphCpuStart = renderGraph->getCpuStats();
      const auto frameStart = lib::time::TimeSpan::now();
      const float dtSec = isPaused ? 0.0f : (deltaTime / 1000.0f);
      const uint32_t frameSlot = frameCounter % maxFramesInFlight;
      renderGraph->setCurrentFrameIndex(frameSlot);

      const auto simulationStart = lib::time::TimeSpan::now();
      if (!isPaused && manualLookInput)
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

      if (!isPaused && manualCamInput)
      {
        const math::Vec3f cameraForward = (cameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f)).normalize();
        if (moveForward)
          cameraPosition = cameraPosition + cameraForward * (kCamSpeed * dtSec);
        if (moveBackward)
          cameraPosition = cameraPosition - cameraForward * (kCamSpeed * dtSec);
      }

      if (!isPaused && manualObjectInput)
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
        const bool objectTransformUpdated = sceneManager.updateObjectInstanceTransform(instId, objectPosition, objectRotation, 1.0f);
        if (objectTransformUpdated && sparseSdfSceneManager != nullptr)
          sparseSdfSceneManager->syncInstanceTransforms(scene, instId);
      }
      bool animationUpdatedThisFrame = false;
      if (!isPaused && hasActiveAnimation)
      {
        cpuFrame.animationMs = measureWallTimeMs(
            [&]()
            {
              animationTimeSeconds += dtSec;
              if (!sceneManager.applyAnimationFrame(instId, animationPlayer, activeAnimationName, animationTimeSeconds, true))
                throw std::runtime_error("Failed to apply animation frame for clip: " + activeAnimationName);
            });
        animationUpdatedThisFrame = true;
      }
      const auto rsmVec3Changed = [](const math::Vec3f &a, const math::Vec3f &b, float epsilon)
      {
        const math::Vec3f delta = a - b;
        return delta.dot(delta) > epsilon * epsilon;
      };
      const auto rsmQuatChanged = [](const math::Quatf &a, const math::Quatf &b, float epsilon)
      {
        const float dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        return 1.0f - std::min(dot, 1.0f) > epsilon;
      };
      const auto rsmFloatChanged = [](float a, float b, float epsilon)
      {
        return std::abs(a - b) > epsilon;
      };
      const bool rsmTemporalPayloadChanged =
          !hasRsmTemporalPayloadSignature ||
          lightDirectionChangedThisFrame ||
          manualObjectInput ||
          animationUpdatedThisFrame ||
          rsmQuatChanged(objectRotation, previousRsmTemporalObjectRotation, 1.0e-5f) ||
          rsmVec3Changed(lightDirection, previousRsmTemporalLightDirection, 1.0e-4f) ||
          rsmVec3Changed(directionalLightColor, previousRsmTemporalDirectionalColor, 1.0e-4f) ||
          rsmVec3Changed(pointLightColor, previousRsmTemporalPointColor, 1.0e-4f) ||
          rsmFloatChanged(directionalLightIntensity, previousRsmTemporalDirectionalIntensity, 1.0e-4f) ||
          rsmFloatChanged(pointLightIntensity, previousRsmTemporalPointIntensity, 1.0e-4f) ||
          rsmFloatChanged(pointLightRange, previousRsmTemporalPointRange, 1.0e-4f) ||
          directionalLightEnabled != previousRsmTemporalDirectionalEnabled ||
          pointLightEnabled != previousRsmTemporalPointEnabled;
      cpuFrame.simulationMs = (lib::time::TimeSpan::now() - simulationStart).milliseconds();
      sceneManager.prepareFrame(frameSlot);

      // --- Rebuild camera ------------------------------------------------------
      const auto cameraBuildStart = lib::time::TimeSpan::now();
      cam.setPosition(cameraPosition);
      cam.setRotation(cameraRotation);
      cam.updateMatrices();
      cpuFrame.cameraBuildMs = (lib::time::TimeSpan::now() - cameraBuildStart).milliseconds();

      const bool shouldSyncSignedDistanceFieldsScene =
          gpuSignedDistanceFieldsSceneManager != nullptr &&
          (editableRebuildSettings.enableSignedDistanceFields || rendererDisplayOutputUsesFallbackScene(editableRebuildSettings.displayOutput));
      if (shouldSyncSignedDistanceFieldsScene)
      {
        measureCpuOperation(
            renderGraph,
            cpuFrame.sdfSync,
            [&]()
            {
              sceneManager.syncSignedDistanceFieldScene(cam.getPosition());
            });
      }
      else
      {
        cpuFrame.sdfSync = {};
      }
      cpuFrame.sdfSyncMs = cpuFrame.sdfSync.wallMs;
      if (softwareBlasSceneBridge)
      {
        measureCpuOperation(
            renderGraph,
            cpuFrame.softwareBlasSync,
            [&]()
            {
              sceneManager.syncSoftwareBLASScene(makeSoftwareRaytraceBlasSceneSettings());
            });
      }
      else
      {
        cpuFrame.softwareBlasSync = {};
      }
      if (softwareTlasSceneBridge)
      {
        measureCpuOperation(
            renderGraph,
            cpuFrame.softwareTlasSync,
            [&]()
            {
              sceneManager.syncSoftwareTLASScene(makeSoftwareRaytraceTlasFilter());
            });
        cpuFrame.softwareTlasBuild = softwareTlasManager != nullptr ? softwareTlasManager->getLastBuildSummary() : softwarebvh::SoftwareTLASManager::BuildSummary{};
      }
      else
      {
        cpuFrame.softwareTlasSync = {};
        cpuFrame.softwareTlasBuild = {};
      }

      // --- Upload uniforms to GPU passes ---------------------------------------
      const math::Mat4f &view = cam.getViewMatrix();
      const math::Mat4f &proj = cam.getProjectionMatrix();
      const math::Vec2f taaJitter = kTaaEnabled ? computeTaaJitter(frameCounter, kTemporalJitterSamples, kTemporalJitterScale, kTemporalRenderScale) : math::Vec2f(0.0f);
      const math::Mat4f projShading = kTaaEnabled ? applyProjectionJitter(proj, taaJitter, kRenderWidth, kRenderHeight) : proj;
      const math::Vec3f &cp = cam.getPosition();
      const bool taaHistoryValid = kTaaEnabled && hasTaaHistory && !(useRsmScreenPass && rsmTemporalPayloadChanged);

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
            syncForcedScreenSpaceFallbackBudget(kRenderWidth, kRenderHeight);
            const uint32_t currentScreenSpaceFallbackBudget = effectiveScreenSpaceRayTraceFallbackBudget(
                !screenSpaceRayTraceScreenTracesEnabled || screenSpaceRayTraceTraceAllForcedFallbackRays,
                screenSpaceRayTraceSoftwareFallbackBudget,
                kRenderWidth,
                kRenderHeight,
                screenSpaceRayTraceTileSize,
                screenSpaceRayTraceRaysPerTile);
            const uint32_t currentScreenSpaceMaxFailedRays = (!screenSpaceRayTraceScreenTracesEnabled || screenSpaceRayTraceTraceAllForcedFallbackRays)
                ? currentScreenSpaceFallbackBudget
                : std::max(1u, screenSpaceRayTraceMaxFailedRays);
            if (softwareTlasManager != nullptr)
            {
              const auto &softwareTlasSummary = softwareTlasManager->getLastBuildSummary();
              const bool worldCwbvhTraversalActive = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
              const bool worldBlasUpdateEnabled =
                  worldCwbvhTraversalActive &&
                  (worldCwbvhWorldBlasBakePending ||
                   softwareTlasSummary.rebuilt ||
                   (kRefitWorldCwbvhBlasOnTransform && softwareTlasSummary.refitted));
              for (softwarebvh::rendering::SoftwareWorldBLASUpdatePass *pass : softwareWorldBlasUpdatePasses)
              {
                if (pass == nullptr)
                  continue;
                auto settings = pass->getSettings();
                settings.enabled = worldBlasUpdateEnabled;
                pass->setSettings(settings);
              }
            }
            // Keep culling in the internal render-resolution space so LOD matches
            // the actual shading resolution, but use the stable unjittered
            // projection to avoid render-scale-dependent frustum/projection wobble
            // at the screen edges.
            cpuFrame.preCullingUniformMs = measureWallTimeMs(
                [&]()
                {
                  preCullingPass->updateUniforms(view.data, proj.data, vp4.data, kRenderWidth, kRenderHeight, kNear, kFar, kHiZLvls);
                });
            cpuFrame.depthPreUniformMs = measureWallTimeMs(
                [&]()
                {
                  depthPrePass->updateUniforms(view.data, projShading.data, vp4.data, kRenderWidth, kRenderHeight, kNear, kFar, kHiZLvls, geometryLodErrorThreshold);
                });
            cpuFrame.finalCullingUniformMs = measureWallTimeMs(
                [&]()
                {
                  finalCullingPass->updateUniforms(view.data, proj.data, vp4.data, kRenderWidth, kRenderHeight, kNear, kFar, kHiZLvls);
                });
            cpuFrame.hardwareDrawUniformMs = measureWallTimeMs(
                [&]()
                {
                  hwDrawPass->updateUniforms(
                      view.data, projShading.data, vp4.data, kRenderWidth, kRenderHeight, kNear, kFar, kHiZLvls, geometryLodErrorThreshold, previousViewMatrix.data, previousProjectionMatrix.data, hasPreviousCameraFrame);
                });
            cpuFrame.materialCameraMs = measureWallTimeMs(
                [&]()
                {
                  materialPass->updateCamera(view, projShading, kRenderWidth, kRenderHeight);
                });
            if (screenSpaceRayTracePass != nullptr)
              measureWallTimeMs(
                  [&]()
                  {
                    ScreenSpaceRayTracePass::Settings ssrtSettings = screenSpaceRayTracePass->getSettings();
                    ssrtSettings.enabled = screenSpaceRayTraceEnabled;
                    ssrtSettings.screenTraceEnabled = screenSpaceRayTraceScreenTracesEnabled;
                    ssrtSettings.forceWorldFallback = screenSpaceRayTraceForceWorldFallback || !screenSpaceRayTraceScreenTracesEnabled;
                    ssrtSettings.tileSize = screenSpaceRayTraceTileSize;
                    ssrtSettings.raysPerTile = screenSpaceRayTraceRaysPerTile;
                    ssrtSettings.maxSteps = screenSpaceRayTraceMaxSteps;
                    ssrtSettings.maxFailedRays = currentScreenSpaceMaxFailedRays;
                    ssrtSettings.minTraceDistance = screenSpaceRayTraceMinDistance;
                    ssrtSettings.maxTraceDistance = screenSpaceRayTraceMaxDistance;
                    ssrtSettings.surfaceThickness = screenSpaceRayTraceSurfaceThickness;
                    ssrtSettings.originBias = screenSpaceRayTraceOriginBias;
                    screenSpaceRayTracePass->setSettings(ssrtSettings);
                    screenSpaceRayTracePass->updateUniforms(projShading * view, (projShading * view).inverse(), frameCounter, cam.isReverseZ());
                  });
            if (screenSpaceHitShadingResolvePass != nullptr)
            {
              screenSpaceHitShadingResolvePass->setSettings(
                  globalillumination::ScreenSpaceHitShadingResolvePass::Settings{
                    .hitCapacity = currentScreenSpaceMaxFailedRays,
                    .enabled = screenSpaceRayTraceEnabled && screenSpaceRayTraceScreenTracesEnabled && !screenSpaceRayTraceForceWorldFallback,
                  });
            }
            cpuFrame.materialBeginFrameMs = measureWallTimeMs(
                [&]()
                {
                  materialPass->beginFrame();
                });
            if (taaResolvePass != nullptr)
              measureWallTimeMs(
                  [&]()
                  {
                    taaResolvePass->updateUniforms(taaHistoryValid, taaJitter, previousTaaJitter);
                  });
            if (taaHistoryPass != nullptr)
              measureWallTimeMs(
                  [&]()
                  {
                    taaHistoryPass->updateUniforms(taaJitter);
                  });
            if (screenSpaceRayTraceSdfFallbackPreparePass != nullptr)
            {
              screenSpaceRayTraceSdfFallbackPreparePass->setSettings(
                  ScreenSpaceRayTraceSdfFallbackPreparePass::Settings{
                      .enabled = screenSpaceRayTraceSdfFallbackEnabled,
                      .budget = currentScreenSpaceFallbackBudget,
                      .maxFailedRays = currentScreenSpaceMaxFailedRays,
                      .tMin = screenSpaceRayTraceMinDistance,
                      .tMax = screenSpaceRayTraceMaxDistance,
                  });
            }
            if (screenSpaceRayTraceSdfFallbackTracePass != nullptr)
            {
              screenSpaceRayTraceSdfFallbackTracePass->setEnabled(screenSpaceRayTraceSdfFallbackEnabled);
              screenSpaceRayTraceSdfFallbackTracePass->setSettings(
                  gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
                      .rayCount = currentScreenSpaceFallbackBudget,
                      .maxSteps = screenProbeSdfMaxSteps,
                      .maxTraceDistance = screenSpaceRayTraceMaxDistance,
                      .hitThresholdScale = screenProbeSdfHitThresholdScale,
                      .emptySpaceStepScale = screenProbeSdfEmptySpaceStepScale,
                      .normalBias = screenSpaceRayTraceOriginBias,
                      .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
                  });
            }
            if (sdfHitShadingResolvePass != nullptr)
            {
              sdfHitShadingResolvePass->setSettings(
                  globalillumination::SDFRayTraceEvaluateMaterialPass::Settings{
                      .rayCount = currentScreenSpaceFallbackBudget,
                      .enabled = screenSpaceRayTraceSdfFallbackEnabled,
                      .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
                      .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
                  });
            }
            if (softwareRayTracePass != nullptr)
            {
              if (screenSpaceRayTraceSoftwareFallbackPass != nullptr)
              {
                const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
                auto fallbackTraceSettings = screenSpaceRayTraceSoftwareFallbackPass->getSettings();
                fallbackTraceSettings.enabled = screenSpaceRayTraceSoftwareFallbackEnabled;
                fallbackTraceSettings.rayCount = currentScreenSpaceFallbackBudget;
                fallbackTraceSettings.tMin = screenSpaceRayTraceMinDistance;
                fallbackTraceSettings.tMax = screenSpaceRayTraceMaxDistance;
                fallbackTraceSettings.enableTraversalEarlyOut = softwareRayTraceTraversalEarlyOut;
                fallbackTraceSettings.traversalEarlyOutDistanceBias = softwareRayTraceTraversalEarlyOutBias;
                fallbackTraceSettings.primitiveMode = softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle;
                fallbackTraceSettings.traversalMode = useWorldCwbvhTraversal
                                                         ? softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH
                                                         : softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance;
                fallbackTraceSettings.tlasMode = useWorldCwbvhTraversal
                                                    ? softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH
                                                    : static_cast<softwarebvh::rendering::SoftwareRayTracePass::TLASMode>(std::min<uint32_t>(softwareRayTraceTlasMode, 1u));
                fallbackTraceSettings.enableRaySorting =
                    fallbackTraceSettings.traversalMode == softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH ||
                    fallbackTraceSettings.tlasMode == softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH;
                fallbackTraceSettings.rbvhLod = std::min<uint32_t>(softwareRayTraceRbvhLod, softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
                fallbackTraceSettings.shaderVariant = screenSpaceRayTraceSoftwareFallbackTraceStats
                    ? softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant::Debug
                    : softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant::Release;
                screenSpaceRayTraceSoftwareFallbackPass->setSettings(fallbackTraceSettings);
              }
              if (screenSpaceRayTraceSoftwareFallbackPreparePass != nullptr)
              {
                screenSpaceRayTraceSoftwareFallbackPreparePass->setSettings(
                    ScreenSpaceRayTraceSoftwareFallbackPreparePass::Settings{
                        .enabled = screenSpaceRayTraceSoftwareFallbackEnabled,
                        .budget = currentScreenSpaceFallbackBudget,
                        .maxFailedRays = currentScreenSpaceMaxFailedRays,
                        .tMin = screenSpaceRayTraceMinDistance,
                        .tMax = screenSpaceRayTraceMaxDistance,
                    });
              }
              measureWallTimeMs(
                  [&]()
                  {
                    auto softwareTraceSettings = softwareRayTracePass->getSettings();
                    softwareTraceSettings.enabled = editableRebuildSettings.enableSoftwareRayTracing && rendererDisplayOutputUsesSoftwareRayTracing(editableRebuildSettings.displayOutput);
                    softwareTraceSettings.width = kSoftwareRayTraceWidth;
                    softwareTraceSettings.height = kSoftwareRayTraceHeight;
                    softwareTraceSettings.generateCameraRays = true;
                    softwareTraceSettings.enableDynamicRayFetch = false;
                    softwareTraceSettings.rayCount = 0u;
                    softwareTraceSettings.cameraPosition = cam.getPosition();
                    softwareTraceSettings.cameraForward = cam.getForward();
                    softwareTraceSettings.cameraRight = cam.getRight();
                    softwareTraceSettings.cameraUp = cam.getUp();
                    softwareTraceSettings.verticalFovRadians = cam.getFov();
                    softwareTraceSettings.reverseZ = cam.isReverseZ();
                    softwareTraceSettings.inverseViewProjection = (projShading * view).inverse();
                    softwareTraceSettings.useExplicitInverseViewProjection = true;
                    softwareTraceSettings.tMin = softwareRayTraceMinDistance;
                    softwareTraceSettings.tMax = softwareRayTraceMaxDistance;
                    softwareTraceSettings.heatmapMaxIterations = softwareRayTraceHeatmapMaxIterations;
                    softwareTraceSettings.overlayTLASBounds = softwareRayTraceOverlayTLASBounds;
                    softwareTraceSettings.overlayTLASCwbvhBounds = softwareRayTraceOverlayTLASCwbvhBounds;
                    softwareTraceSettings.overlayTLASLevel = softwareRayTraceOverlayTLASLevel;
                    softwareTraceSettings.overlayLineThickness = softwareRayTraceOverlayLineThickness;
                    softwareTraceSettings.enableTraversalEarlyOut = softwareRayTraceTraversalEarlyOut;
                    softwareTraceSettings.traversalEarlyOutDistanceBias = softwareRayTraceTraversalEarlyOutBias;
                    const bool useWorldCwbvhTraversal = softwareRayTraceTraversalMode == static_cast<uint32_t>(softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH);
                    softwareTraceSettings.primitiveMode = useWorldCwbvhTraversal
                                                             ? softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode::Triangle
                                                             : static_cast<softwarebvh::rendering::SoftwareRayTracePass::PrimitiveMode>(std::min<uint32_t>(softwareRayTracePrimitiveMode, 1u));
                    softwareTraceSettings.traversalMode = useWorldCwbvhTraversal
                                                             ? softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH
                                                             : softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::LocalInstance;
                    softwareTraceSettings.tlasMode = useWorldCwbvhTraversal
                                                        ? softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH
                                                        : static_cast<softwarebvh::rendering::SoftwareRayTracePass::TLASMode>(std::min<uint32_t>(softwareRayTraceTlasMode, 1u));
                    softwareTraceSettings.enableRaySorting =
                        softwareTraceSettings.traversalMode == softwarebvh::rendering::SoftwareRayTracePass::TraversalMode::WorldCWBVH ||
                        softwareTraceSettings.tlasMode == softwarebvh::rendering::SoftwareRayTracePass::TLASMode::CWBVH;
                    softwareTraceSettings.rbvhLod = std::min<uint32_t>(softwareRayTraceRbvhLod, softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
                    softwareTraceSettings.shaderVariant = static_cast<softwarebvh::rendering::SoftwareRayTracePass::ShaderVariant>(std::min<uint32_t>(softwareRayTraceShaderVariant, 2u));
                    softwareTraceSettings.debugMode = rendererDisplayOutputSoftwareRayTraceDebugMode(editableRebuildSettings.displayOutput);
                    softwareRayTracePass->setSettings(softwareTraceSettings);
                    const bool softwareRayTraceAlbedoDebugEnabled = rendererDisplayOutputIsSoftwareRayTracingAlbedo(editableRebuildSettings.displayOutput);
                    if (softwareRayTraceShadingResolvePass != nullptr)
                    {
                      softwareRayTraceShadingResolvePass->setSettings(
                          globalillumination::WorldHitShadingResolvePass::Settings{
                            .rayCount = softwareRayTraceDebugRayCount,
                            .enabled = kRendererEnableWorldHitShadingResolve && softwareRayTraceAlbedoDebugEnabled,
                            .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
                            .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
                            .receiverPixelWidth = kSoftwareRayTraceWidth,
                          });
                    }
                    if (softwareRayTraceAlbedoDebugPass != nullptr)
                    {
                      softwareRayTraceAlbedoDebugPass->setSettings(
                          globalillumination::TraceShadingAlbedoDebugPass::Settings{
                            .width = kSoftwareRayTraceWidth,
                            .height = kSoftwareRayTraceHeight,
                            .rayCount = softwareRayTraceDebugRayCount,
                            .enabled = kRendererEnableWorldHitShadingResolve && softwareRayTraceAlbedoDebugEnabled,
                          });
                    }
                  });
            }
            if (surfelSdfTracePass != nullptr)
            {
              measureWallTimeMs(
                  [&]()
                  {
                    if (surfelTraceStatsClearPass != nullptr)
                    {
                      surfelTraceStatsClearPass->setEnabled(useSurfels);
                    }
                    surfelSdfTracePass->setEnabled(useSurfels);
                    surfelSdfTracePass->setSettings(
                        gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
                          .rayCount = kSurfelLightingRayCount,
                          .rayOffset = kSurfelLightingRayOffset,
                          .hitOffset = kSurfelLightingRayOffset,
                          .maxSteps = surfelRuntimeSettings.lightingTraceMaxSteps,
                          .maxTraceDistance = surfelRuntimeSettings.lightingMaxTraceDistance,
                          .hitThresholdScale = surfelRuntimeSettings.lightingTraceHitThresholdScale,
                          .emptySpaceStepScale = surfelRuntimeSettings.lightingTraceEmptySpaceStepScale,
                          .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
                          .cascadeOverride = surfelRuntimeSettings.lightingTraceCascadeOverride,
                          .traceStatsEnabled = true,
                          .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
                        });
                  });
            }
            cpuFrame.vsmLightSetupMs = measureWallTimeMs(
                [&]()
                {
                  vsmManager.setDirectionalLightDirection(vsmLightId, lightDirection);
                  vsmManager.setLightColor(vsmLightId, directionalLightColor);
                  vsmManager.setLightIntensity(vsmLightId, directionalLightEnabled ? directionalLightIntensity : 0.0f);
	                  vsmManager.setLightColor(vsmPointLightId, pointLightColor);
	                  vsmManager.setLightIntensity(vsmPointLightId, pointLightEnabled ? pointLightIntensity : 0.0f);
	                  vsmManager.setPointLightRange(vsmPointLightId, pointLightRange);
	                  if (useIsmVplPath && (manualObjectInput || animationUpdatedThisFrame || lightDirectionChangedThisFrame))
	                  {
                    std::vector<ImperfectShadowMapVPL> updatedVpls = buildDirectionalLightSurfaceVPLs(
                        ismVplActiveCount,
                        ismVplSurfaceTriangles,
                        objectPosition,
                        objectRotation,
                        objectPosition + objectRotation * objectCentre,
                        kRadius,
                        lightDirection,
                        directionalLightColor,
                        directionalLightEnabled ? directionalLightIntensity : 0.0f,
                        &ismVplBuildStats);
                    for (uint32_t vplIndex = 0u; vplIndex < std::min<uint32_t>(static_cast<uint32_t>(ismVpls.size()), static_cast<uint32_t>(updatedVpls.size())); ++vplIndex)
                    {
                      updatedVpls[vplIndex].lightId = ismVpls[vplIndex].lightId;
                      ismVpls[vplIndex] = updatedVpls[vplIndex];
                    }
                  }
                  else if (useIsmVplPath)
                  {
                    updateImperfectShadowMapVplFlux(
                        ismVpls,
                        directionalLightColor,
                        directionalLightEnabled ? directionalLightIntensity : 0.0f);
                  }
                  if (useIsmVplPath)
                  {
                    for (ImperfectShadowMapVPL &vpl : ismVpls)
                    {
                      if (vpl.lightId == VirtualShadowMapManager::INVALID_LIGHT_ID)
                        continue;
                      const float fluxIntensity = std::max(std::max(vpl.flux[0], vpl.flux[1]), vpl.flux[2]);
                      const math::Vec3f vplColor = fluxIntensity > 1e-5f ? vpl.flux * (1.0f / fluxIntensity) : math::Vec3f(1.0f, 1.0f, 1.0f);
                      ismVplShadowManager.setPointLightPosition(vpl.lightId, vpl.position);
                      ismVplShadowManager.setPointLightRange(vpl.lightId, vpl.range);
                      ismVplShadowManager.setLightColor(vpl.lightId, vplColor);
                      ismVplShadowManager.setLightIntensity(vpl.lightId, fluxIntensity);
                    }
                  }
                });
            cpuFrame.vsmUpdateMs = measureWallTimeMs(
                [&]()
	                {
	                  vsmManager.update(cam, &projShading);
	                  if (useIsmVplPath)
	                  {
	                    ismVplShadowManager.update(cam, &projShading);
	                  }
	                });
            if (ismVplPointDebugPass != nullptr)
            {
              const math::Mat4f viewProjection = projShading * view;
              ismVplDebugViewProjection = viewProjection;
              ismVplPointDebugPass->setVPLs(ismVpls);
              ismVplPointDebugPass->setSettings(
                  ImperfectShadowMapVplPointDebugPass::Settings{
                      .width = kRenderWidth,
                      .height = kRenderHeight,
                      .viewProjection = viewProjection,
                      .selectedVpl = ismVplDebugSelectedVpl,
                      .pointRadiusPixels = ismVplDebugPointRadius,
                      .exposure = ismVplDebugExposure,
                      .enabled = selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_ImperfectShadowMapVplPoints ||
                                 (ismVplPointsOverlayEnabled && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_ShadedColor),
                  });
            }
            if (auto *pass = vsmPass->getScreenSpaceShadowPass())
            {
              const math::Mat4f viewProjection = projShading * view;
              measureWallTimeMs(
                  [&]()
                  {
                    pass->updateRuntimeState(viewProjection, cam.isReverseZ());
                  });
            }
            if (rsmIndirectDiffusePass != nullptr)
            {
              rsmIndirectDiffusePass->setSettings(
                  VirtualShadowMapRsmIndirectDiffusePass::Settings{
                    .width = kRenderWidth,
                    .height = kRenderHeight,
                    .sampleCount = std::clamp(editableRebuildSettings.rsmGiSampleCount, 1u, 64u),
                    .frameIndex = frameCounter,
                    .debugMode = rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput),
                    .sampleRadiusWorld = editableRebuildSettings.rsmGiSampleRadiusMeters,
                    .intensity = editableRebuildSettings.rsmGiIntensity,
                    .normalBias = editableRebuildSettings.rsmGiNormalBias,
                    .minDistance = editableRebuildSettings.rsmGiMinDistance,
                    .maxDistance = editableRebuildSettings.rsmGiMaxDistance,
                    .localLightMipTransitionScale = vsmLocalLightMipTransitionScale,
                    .localLightMipBias = vsmLocalLightMipBias,
                    .fireflyClamp = editableRebuildSettings.rsmGiFireflyClamp,
                    .enabled = useRsmScreenPass,
	                  });
	            }
	            if (rsmTemporalResolvePass != nullptr)
	            {
	              const auto rsmDebugMode = rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput);
	              rsmTemporalResolvePass->setSettings(
	                  VirtualShadowMapRsmTemporalResolvePass::Settings{
	                    .width = kRenderWidth,
	                    .height = kRenderHeight,
	                    .currentJitter = {taaJitter[0], taaJitter[1]},
	                    .previousJitter = {previousTaaJitter[0], previousTaaJitter[1]},
	                    .debugMode = rsmDebugMode,
	                    .historyWeight = std::clamp(1.0f - 1.0f / static_cast<float>(std::max(editableRebuildSettings.rsmGiTemporalFrames, 1u)), 0.0f, 0.98f),
	                    .disocclusionWeight = editableRebuildSettings.rsmGiTemporalDisocclusionWeight,
	                    .depthSigma = editableRebuildSettings.rsmGiTemporalDepthSigma,
		                    .normalSigma = editableRebuildSettings.rsmGiTemporalNormalSigma,
		                    .validRatioMin = editableRebuildSettings.rsmGiValidRatioMin,
		                    .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiTemporalFrames, 1u)),
		                    .fireflyClamp = editableRebuildSettings.rsmGiFireflyClamp,
		                    .enabled = useRsmScreenPass && editableRebuildSettings.rsmGiTemporalAccumulation && rsmDebugMode == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse,
		                    .historyValid = hasRsmTemporalHistory && !rsmTemporalPayloadChanged && useRsmScreenPass && editableRebuildSettings.rsmGiTemporalAccumulation,
		                  });
	            }
            if (rsmGiDenoisePass != nullptr)
            {
              const auto rsmDebugMode = rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput);
              const bool rsmDenoiserActive = useRsmScreenPass && editableRebuildSettings.rsmGiDenoiserEnabled && rsmDebugMode == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse;
              rsmGiDenoisePass->setSettings(
                  rendering::gpgpu::SvgfDenoisePass::Settings{
                    .width = kRenderWidth,
                    .height = kRenderHeight,
                    .currentJitter = {taaJitter[0], taaJitter[1]},
                    .previousJitter = {previousTaaJitter[0], previousTaaJitter[1]},
                    .historyWeight = std::clamp(1.0f - 1.0f / static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)), 0.0f, 0.99f),
                    .depthSigma = editableRebuildSettings.rsmGiDenoiserDepthSigma,
                    .normalSigma = editableRebuildSettings.rsmGiDenoiserNormalSigma,
                    .luminanceSigma = editableRebuildSettings.rsmGiDenoiserLuminanceSigma,
                    .validRatioMin = editableRebuildSettings.rsmGiDenoiserValidRatioMin,
                    .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)),
                    .fireflyClamp = editableRebuildSettings.rsmGiDenoiserFireflyClamp,
                    .spatialSigma = editableRebuildSettings.rsmGiDenoiserSpatialSigma,
                    .spatialRadius = editableRebuildSettings.rsmGiDenoiserSpatialRadius,
                    .spatialStrength = editableRebuildSettings.rsmGiDenoiserSpatialStrength,
                    .disocclusionWeight = editableRebuildSettings.rsmGiDenoiserDisocclusionWeight,
                    .minCurrentWeight = editableRebuildSettings.rsmGiDenoiserMinCurrentWeight,
                    .enabled = rsmDenoiserActive,
                    .historyValid = hasRsmDenoiserHistory && !rsmTemporalPayloadChanged && rsmDenoiserActive,
                  });
              for (uint32_t atrousIndex = 0u; atrousIndex < static_cast<uint32_t>(rsmGiAtrousFilterPasses.size()); ++atrousIndex)
              {
                if (rsmGiAtrousFilterPasses[atrousIndex] == nullptr)
                {
                  continue;
                }
                const uint32_t baseStep = std::max(1u, static_cast<uint32_t>(std::lround(editableRebuildSettings.rsmGiDenoiserSpatialRadius)));
                rsmGiAtrousFilterPasses[atrousIndex]->setSettings(
                    rendering::gpgpu::SvgfAtrousFilterPass::Settings{
                      .width = kRenderWidth,
                      .height = kRenderHeight,
                      .stepWidth = baseStep << atrousIndex,
                      .depthSigma = editableRebuildSettings.rsmGiDenoiserDepthSigma,
                      .normalSigma = editableRebuildSettings.rsmGiDenoiserNormalSigma,
                      .luminanceSigma = editableRebuildSettings.rsmGiDenoiserLuminanceSigma,
                      .fireflyClamp = editableRebuildSettings.rsmGiDenoiserFireflyClamp,
                      .spatialStrength = editableRebuildSettings.rsmGiDenoiserSpatialStrength,
                      .varianceSigma = editableRebuildSettings.rsmGiDenoiserSpatialSigma,
                      .maxHistoryFrames = static_cast<float>(std::max(editableRebuildSettings.rsmGiDenoiserFrames, 1u)),
                      .enabled = rsmDenoiserActive && atrousIndex < std::clamp(editableRebuildSettings.rsmGiDenoiserAtrousIterations, 1u, kRsmGiAtrousPassCount),
                    });
              }
            }
            const math::Mat4f surfelLightingViewProjection = projShading * view;
            const math::Mat4f surfelLightingInverseViewProjection = surfelLightingViewProjection.inverse();
            for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
            {
              const bool surfelPassEnabled = surfelFrameIndex == (frameCounter & 1u);
              if (surfelTracePreparePasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelTracePreparePasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelTracePreparePasses[surfelFrameIndex]->setSettings(
                          globalillumination::surfels::SurfelTracePreparePass::Settings{
                            .viewProjection = surfelLightingViewProjection,
                            .inverseViewProjection = surfelLightingInverseViewProjection,
                            .viewPortWidth = kRenderWidth,
                            .viewPortHeight = kRenderHeight,
                            .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
                            .rayOffset = kSurfelLightingRayOffset,
                            .frameIndex = frameCounter,
                            .updatePeriodFrames = surfelRuntimeSettings.lightingUpdatePeriodFrames,
                            .activeLightCount = vsmManager.getActiveLightCount(),
                            .reverseZ = cam.isReverseZ(),
                            .maxTraceDistance = surfelRuntimeSettings.lightingMaxTraceDistance,
                            .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
                            .viewBias = surfelRuntimeSettings.lightingTraceViewBias,
                            .directionalLightDirection = {lightDirection[0], lightDirection[1], lightDirection[2]},
                            .directionalLightIntensity = directionalLightEnabled ? directionalLightIntensity : 0.0f,
                            .directionalLightColor = {directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]},
                            .pointLightPosition = {pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]},
                            .pointLightRange = pointLightRange,
                            .pointLightColor = {pointLightColor[0], pointLightColor[1], pointLightColor[2]},
                            .pointLightIntensity = pointLightEnabled ? pointLightIntensity : 0.0f,
                          });
                    });
              }
              if (surfelLightingUpdatePasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelLightingUpdatePasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelLightingUpdatePasses[surfelFrameIndex]->setSettings(
                          globalillumination::surfels::SurfelLightingUpdatePass::Settings{
                            .viewProjection = surfelLightingViewProjection,
                            .inverseViewProjection = surfelLightingInverseViewProjection,
                            .viewPortWidth = kRenderWidth,
                            .viewPortHeight = kRenderHeight,
                            .frameIndex = frameCounter,
                            .reverseZ = cam.isReverseZ(),
                            .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
                            .traceHitOffset = kSurfelLightingRayOffset,
                            .updatePeriodFrames = surfelRuntimeSettings.lightingUpdatePeriodFrames,
                            .gridCapacity = surfelRuntimeSettings.gridCapacity,
                            .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
                            .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
                            .cameraPosition = {cameraPosition[0], cameraPosition[1], cameraPosition[2]},
                            .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
                            .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
                            .hysteresis = surfelRuntimeSettings.lightingHysteresis,
                            .irradianceDecay = surfelRuntimeSettings.lightingIrradianceDecay,
                            .normalBias = surfelRuntimeSettings.lightingTraceNormalBias,
                            .viewBias = surfelRuntimeSettings.lightingTraceViewBias,
                            .energyPreservation = surfelRuntimeSettings.lightingEnergyPreservation,
                            .directionalShadowEnabled = directionalLightEnabled,
                            .directionalShadowReverseZ = cam.isReverseZ(),
                            .directionalShadowPageTableResolution = vsmManager.getVirtualPageTableResolution(),
                            .directionalShadowActiveLayers = vsmManager.getActiveLayerCount(),
                            .directionalShadowActiveLights = vsmManager.getActiveLightCount(),
                            .directionalShadowPhysicalPageSize = vsmManager.getPhysicalPageResolution(),
                            .directionalShadowCascadeCount = vsmManager.getCascadeCount(),
                            .directionalShadowFallbackCascadeOffset = vsmManager.getFallbackCascadeOffset(),
                            .directionalShadowFirstCascadeWorldExtent = vsmManager.getFirstCascadeWorldExtent(),
                            .directionalShadowBias = directionalShadowBias,
                            .directionalShadowMaxBias = directionalMaxShadowBias,
                            .directionalShadowPcfRadiusTexels = pcfRadiusTexels,
                            .directionalShadowNormalBiasTexels = directionalNormalBiasTexels,
                            .directionalShadowFilterTaps = shadowPcfEnabled ? shadowFilterTaps : 1u,
                            .directionalLightDirection = {lightDirection[0], lightDirection[1], lightDirection[2]},
                            .directionalLightIntensity = directionalLightIntensity,
                            .directionalLightColor = {directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]},
                            .defaultDiffuseAlbedo = surfelRuntimeSettings.lightingDefaultDiffuseAlbedo,
                            .pointLightPosition = {pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]},
                            .pointLightRange = pointLightRange,
                            .pointLightColor = {pointLightColor[0], pointLightColor[1], pointLightColor[2]},
                            .pointLightIntensity = pointLightEnabled ? pointLightIntensity : 0.0f,
                          });
	                  });
	              }
	            }
	            if (worldGILightGridUpdatePass != nullptr)
	            {
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGILightGridUpdatePass->setSettings(
	                        globalillumination::worldgi::WorldGILightGridUpdatePass::Settings{
	                          .gridResolution = worldGILightGridResolution,
	                          .cascadeCount = worldGILightGridCascadeCount,
	                          .maxItemsPerCell = worldGILightGridMaxItemsPerCell,
	                          .lightCapacity = kWorldGILightCapacity,
	                          .frameIndex = frameCounter,
	                          .enabled = editableRebuildSettings.enableWorldGI && editableRebuildSettings.worldGILightGridEnabled,
	                          .gridCenter = cameraPosition,
	                          .baseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
	                          .cascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
	                          .directionalLightDirection = lightDirection,
	                          .directionalLightColor = math::Vec3f(directionalLightColor[0], directionalLightColor[1], directionalLightColor[2]),
	                          .directionalLightIntensity = directionalLightIntensity,
	                          .directionalLightEnabled = directionalLightEnabled,
	                          .pointLightPosition = math::Vec3f(pointLightPosition[0], pointLightPosition[1], pointLightPosition[2]),
	                          .pointLightColor = math::Vec3f(pointLightColor[0], pointLightColor[1], pointLightColor[2]),
	                          .pointLightRange = pointLightRange,
	                          .pointLightIntensity = pointLightIntensity,
	                          .pointLightEnabled = pointLightEnabled,
	                        });
	                  });
	            }
	            if (worldGIRayGenPass != nullptr)
	            {
	              const math::Mat4f viewProjection = projShading * view;
	              const math::Mat4f inverseViewProjection = viewProjection.inverse();
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGIRayGenPass->setSettings(
	                        globalillumination::worldgi::WorldGIRayGenPass::Settings{
	                          .viewPortWidth = kRenderWidth,
	                          .viewPortHeight = kRenderHeight,
	                          .rayGridWidth = worldGIRayGridWidth,
	                          .rayGridHeight = worldGIRayGridHeight,
	                          .raysPerPixel = worldGIRaysPerPixel,
	                          .frameIndex = frameCounter,
	                          .enabled = editableRebuildSettings.enableWorldGI,
	                          .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
	                          .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
	                          .reverseZ = cam.isReverseZ(),
	                          .minTraceDistance = editableRebuildSettings.worldGIRayMinDistance,
	                          .maxTraceDistance = editableRebuildSettings.worldGIRayMaxDistance,
	                          .normalBias = editableRebuildSettings.worldGIRayNormalBias,
	                          .glossyMaxRoughness = editableRebuildSettings.worldGIGlossyMaxRoughness,
	                        });
	                    worldGIRayGenPass->updateUniforms(viewProjection, inverseViewProjection, cameraPosition);
	                  });
	            }
	            if (worldGISdfTracePass != nullptr)
	            {
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGISdfTracePass->setEnabled(editableRebuildSettings.enableWorldGI && editableRebuildSettings.enableSignedDistanceFields);
	                    worldGISdfTracePass->setSettings(
	                        gpusigneddistancefields::gpgpu::GPUSignedDistanceFieldsTracePass::Settings{
	                          .rayCount = worldGIRayCount,
	                          .rayOffset = 0u,
	                          .hitOffset = 0u,
	                          .maxSteps = editableRebuildSettings.worldGIRayMaxSteps,
	                          .maxTraceDistance = editableRebuildSettings.worldGIRayMaxDistance,
	                          .hitThresholdScale = screenProbeSdfHitThresholdScale,
	                          .emptySpaceStepScale = screenProbeSdfEmptySpaceStepScale,
	                          .normalBias = editableRebuildSettings.worldGIRayNormalBias,
	                          .cascadeOverride = -1,
	                          .traceStatsEnabled = false,
	                          .surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled,
	                        });
	                  });
	            }
	            if (worldGISdfMaterialResolvePass != nullptr)
	            {
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGISdfMaterialResolvePass->setSettings(
	                        globalillumination::SDFRayTraceEvaluateMaterialPass::Settings{
	                          .rayCount = worldGIRayCount,
	                          .enabled = editableRebuildSettings.enableWorldGI && editableRebuildSettings.enableSignedDistanceFields,
	                          .feedbackRequestCapacity = 0u,
	                          .feedbackUploadBudget = 0u,
	                          .virtualTextureSamplingEnabled = false,
	                        });
	                  });
	            }
	            if (worldGITraceSurfaceResolvePass != nullptr)
	            {
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGITraceSurfaceResolvePass->setSettings(
	                        globalillumination::worldgi::WorldGITraceSurfaceResolvePass::Settings{
	                          .surfaceCount = worldGIRayCount,
	                          .sourceOverride = globalillumination::worldgi::GITraceSurfaceSource_SDF,
	                          .enabled = editableRebuildSettings.enableWorldGI && editableRebuildSettings.enableSignedDistanceFields,
	                        });
	                  });
	            }
	            if (worldGIHitShadingPass != nullptr)
	            {
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGIHitShadingPass->setSettings(
	                        globalillumination::worldgi::WorldGIHitShadingPass::Settings{
	                          .viewPortWidth = kRenderWidth,
	                          .viewPortHeight = kRenderHeight,
	                          .rayGridWidth = worldGIRayGridWidth,
	                          .rayGridHeight = worldGIRayGridHeight,
	                          .raysPerPixel = worldGIRaysPerPixel,
	                          .enabled = editableRebuildSettings.enableWorldGI && editableRebuildSettings.enableSignedDistanceFields,
	                          .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
	                          .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
	                          .lightGridEnabled = editableRebuildSettings.worldGILightGridEnabled,
	                          .lightGridResolution = worldGILightGridResolution,
	                          .lightGridCascadeCount = worldGILightGridCascadeCount,
	                          .lightGridMaxItemsPerCell = worldGILightGridMaxItemsPerCell,
	                          .lightCount = kWorldGILightCapacity,
	                          .lightGridCenter = cameraPosition,
	                          .lightGridBaseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
	                          .lightGridCascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
	                          .clampValue = 32.0f,
	                          .diffuseScale = 1.0f,
	                          .glossyScale = 1.0f,
	                        });
	                  });
	            }
            if (worldGIComposePass != nullptr)
            {
              measureWallTimeMs(
                  [&]()
                  {
                    worldGIComposePass->setSettings(
                        globalillumination::worldgi::WorldGIComposePass::Settings{
                          .viewPortWidth = kRenderWidth,
                          .viewPortHeight = kRenderHeight,
                          .enabled = editableRebuildSettings.enableWorldGI,
                          .diffuseEnabled = editableRebuildSettings.worldGIDiffuseEnabled,
                          .glossyEnabled = editableRebuildSettings.worldGIGlossyEnabled,
                          .indirectEnabled = editableRebuildSettings.worldGIIndirectEnabled,
                          .diffuseWeight = editableRebuildSettings.worldGIDiffuseWeight,
                          .glossyWeight = editableRebuildSettings.worldGIGlossyWeight,
                          .indirectIntensity = editableRebuildSettings.worldGIIndirectIntensity,
                          .clampValue = 32.0f,
                          .glossyMaxRoughness = editableRebuildSettings.worldGIGlossyMaxRoughness,
                        });
                  });
            }
	            if (worldGILightGridDebugPass != nullptr)
	            {
	              const math::Mat4f viewProjection = projShading * view;
	              const math::Mat4f inverseViewProjection = viewProjection.inverse();
	              measureWallTimeMs(
	                  [&]()
	                  {
	                    worldGILightGridDebugPass->setSettings(
	                        globalillumination::worldgi::WorldGILightGridDebugPass::Settings{
	                          .viewPortWidth = kRenderWidth,
	                          .viewPortHeight = kRenderHeight,
	                          .enabled = worldGILightGridOverlayEnabled,
	                          .reverseZ = cam.isReverseZ(),
	                          .gridResolution = worldGILightGridResolution,
	                          .cascadeCount = worldGILightGridCascadeCount,
	                          .selectedCascade = worldGILightGridOverlayCascade,
	                          .lineWidth = worldGILightGridOverlayLineWidth,
	                          .gridCenter = cameraPosition,
	                          .baseExtent = editableRebuildSettings.worldGILightGridBaseExtent,
	                          .cascadeScale = editableRebuildSettings.worldGILightGridCascadeScale,
	                        });
	                    worldGILightGridDebugPass->updateUniforms(inverseViewProjection);
	                  });
	            }
            if (surfelHierarchyDebugPass != nullptr)
            {
              const bool shouldFreezeSurfelHierarchy = surfelHierarchyFreeze && rendererDisplayOutputUsesSurfelHierarchy(editableRebuildSettings.displayOutput);
              if (shouldFreezeSurfelHierarchy)
              {
                if (!surfelHierarchyFreezeActive)
                {
                  surfelFrozenCameraPosition = cameraPosition;
                  surfelFrozenCameraRotation = cameraRotation;
                  surfelHierarchyFreezeActive = true;
                }
              }
              else
              {
                surfelHierarchyFreezeActive = false;
              }

              const math::Vec3f surfelHierarchyCameraPosition = surfelHierarchyFreezeActive ? surfelFrozenCameraPosition : cameraPosition;
              const math::Quatf surfelHierarchyCameraRotation = surfelHierarchyFreezeActive ? surfelFrozenCameraRotation : cameraRotation;
              const math::Vec3f surfelHierarchyCameraForward = surfelHierarchyFreezeActive ? (surfelHierarchyCameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f)) : cam.getForward();
              const math::Vec3f surfelHierarchyCameraRight = surfelHierarchyFreezeActive ? (surfelHierarchyCameraRotation * math::Vec3f(1.0f, 0.0f, 0.0f)) : cam.getRight();
              const math::Vec3f surfelHierarchyCameraUp = surfelHierarchyFreezeActive ? (surfelHierarchyCameraRotation * math::Vec3f(0.0f, 1.0f, 0.0f)) : cam.getUp();
              const math::Mat4f surfelHierarchyView = math::Mat4f::lookAt(surfelHierarchyCameraPosition, surfelHierarchyCameraPosition + surfelHierarchyCameraForward, surfelHierarchyCameraUp);
              const math::Mat4f surfelHierarchyInverseViewProjection = (projShading * surfelHierarchyView).inverse();
              measureWallTimeMs(
                  [&]()
                  {
                    surfelHierarchySettings.viewPortWidth = kRenderWidth;
                    surfelHierarchySettings.viewPortHeight = kRenderHeight;
                    surfelHierarchyDebugPass->setSettings(surfelHierarchySettings);
                    surfelHierarchyDebugPass->updateUniforms(
                        surfelHierarchyInverseViewProjection,
                        surfelHierarchyCameraPosition,
                        surfelHierarchyCameraRight,
                        surfelHierarchyCameraUp,
                        surfelHierarchyCameraForward,
                        cam.isReverseZ(),
                        rendererDisplayOutputSurfelHierarchyDebugMode(editableRebuildSettings.displayOutput));
                  });
            }
            surfelHierarchySettings.lookupCellResolution = std::max(1u, surfelHierarchySettings.lookupCellResolution);
            surfelHierarchySettings.centralGridHalfExtent = std::max(surfelHierarchySettings.centralGridHalfExtent, 0.01f);
            surfelHierarchySettings.gridScale = globalillumination::surfels::surfelHierarchyGridScaleFromHalfExtent(surfelHierarchySettings);
            surfelHierarchySettings.gridCapacity = globalillumination::surfels::surfelHierarchyLookupCellCount(surfelHierarchySettings);
            surfelRuntimeSettings.gridScale = surfelHierarchySettings.gridScale;
            surfelRuntimeSettings.lookupCellResolution = surfelHierarchySettings.lookupCellResolution;
            surfelRuntimeSettings.centralGridHalfExtent = surfelHierarchySettings.centralGridHalfExtent;
            surfelRuntimeSettings.frustumFarDistance = surfelHierarchySettings.frustumFarDistance;
            surfelRuntimeSettings.gridCapacity = surfelHierarchySettings.gridCapacity;
            if (worldHitShadingResolvePass != nullptr)
            {
              worldHitShadingResolvePass->setSettings(
                  globalillumination::WorldHitShadingResolvePass::Settings{
                    .rayCount = currentScreenSpaceFallbackBudget,
                    .enabled = kRendererEnableWorldHitShadingResolve && screenSpaceRayTraceSoftwareFallbackEnabled,
                    .feedbackRequestCapacity = rendererSettings.materialPassSettings.feedbackRequestCapacity,
                    .feedbackUploadBudget = rendererSettings.materialPassSettings.maxUploadsPerFrame,
                  });
            }
            const uint32_t surfelCurrentFrameIndex = frameCounter & 1u;
            const math::Mat4f surfelSpawnInverseViewProjection = (projShading * view).inverse();
            const float surfelWorldUnitsPerPixelAtUnitDepth = 2.0f / (std::max(std::abs(projShading.at(1, 1)), 1.0e-6f) * static_cast<float>(kRenderHeight));
            const math::Vec3f surfelCameraRight = cam.getRight();
            const math::Vec3f surfelCameraUp = cam.getUp();
            const math::Vec3f surfelCameraForward = cam.getForward();
            const math::Vec3f previousSurfelCameraRight = previousCameraRotation * math::Vec3f(1.0f, 0.0f, 0.0f);
            const math::Vec3f previousSurfelCameraUp = previousCameraRotation * math::Vec3f(0.0f, 1.0f, 0.0f);
            const math::Vec3f previousSurfelCameraForward = previousCameraRotation * math::Vec3f(0.0f, 0.0f, -1.0f);
            for (uint32_t surfelFrameIndex = 0u; surfelFrameIndex < 2u; ++surfelFrameIndex)
            {
              const bool surfelPassEnabled = surfelFrameIndex == surfelCurrentFrameIndex;
              if (surfelScreenSpaceSpawnPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelScreenSpaceSpawnPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelScreenSpaceSpawnPasses[surfelFrameIndex]->setSettings(
                          globalillumination::surfels::SurfelScreenSpaceSpawnPass::Settings{
                            .viewPortWidth = kRenderWidth,
                            .viewPortHeight = kRenderHeight,
                            .frameIndex = frameCounter,
                            .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
                            .surfelAllocationLimit = computeSurfelAllocationLimit(surfelRuntimeSettings.surfelCapacity),
                            .gridCapacity = surfelRuntimeSettings.gridCapacity,
                            .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
                            .enabled = (surfelPassEnabled && useSurfels) ? 1u : 0u,
                            .spawnDownsample = surfelRuntimeSettings.spawnDownsample,
                            .targetSurfelsPerTile = surfelRuntimeSettings.targetSurfelsPerTile,
                            .maxSurfelsPerTile = surfelRuntimeSettings.maxSurfelsPerTile,
                            .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
                            .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
                            .gridScale = surfelRuntimeSettings.gridScale,
                            .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
                            .targetScreenRadiusPixels = surfelRuntimeSettings.targetScreenRadiusPixels,
                            .lookupRadiusScale = surfelRuntimeSettings.lookupRadiusScale,
                            .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
                            .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
                            .recycleCoverageThreshold = surfelRuntimeSettings.recycleCoverageThreshold,
                            .recycleFreeListPressureBias = surfelRuntimeSettings.recycleFreeListPressureBias,
                            .recycleMaxAgeFrames = surfelRuntimeSettings.recycleMaxAgeFrames,
                          });
                      surfelScreenSpaceSpawnPasses[surfelFrameIndex]->updateUniforms(
                          surfelSpawnInverseViewProjection,
                          cameraPosition,
                          previousCameraPosition,
                          surfelCameraRight,
                          surfelCameraUp,
                          surfelCameraForward,
                          previousSurfelCameraRight,
                          previousSurfelCameraUp,
                          previousSurfelCameraForward,
                          surfelWorldUnitsPerPixelAtUnitDepth,
                          cam.isReverseZ());
                    });
              }
              if (surfelAccelerationClearPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationClearPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationClearPasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                    });
              }
              if (surfelAccelerationCountPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationCountPasses[surfelFrameIndex]->setOwnerResolveOnly(false);
                      surfelAccelerationCountPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationCountPasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                      surfelAccelerationCountPasses[surfelFrameIndex]->updateUniforms(
                          frameCounter, cameraPosition, previousCameraPosition, surfelCameraRight, surfelCameraUp, surfelCameraForward, kRenderWidth, kRenderHeight, surfelWorldUnitsPerPixelAtUnitDepth);
                    });
              }
              if (surfelAccelerationOwnerResolvePasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationOwnerResolvePasses[surfelFrameIndex]->setOwnerResolveOnly(true);
                      surfelAccelerationOwnerResolvePasses[surfelFrameIndex]->setEnabled(false);
                      surfelAccelerationOwnerResolvePasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                      surfelAccelerationOwnerResolvePasses[surfelFrameIndex]->updateUniforms(
                          frameCounter, cameraPosition, previousCameraPosition, surfelCameraRight, surfelCameraUp, surfelCameraForward, kRenderWidth, kRenderHeight, surfelWorldUnitsPerPixelAtUnitDepth);
                    });
              }
              if (surfelAccelerationPrefixSumPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationPrefixSumPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationPrefixSumPasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                    });
              }
              if (surfelAccelerationPrefixSegmentsPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationPrefixSegmentsPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationPrefixSegmentsPasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                    });
              }
              if (surfelAccelerationPrefixMergePasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationPrefixMergePasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationPrefixMergePasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                    });
              }
              if (surfelAccelerationInsertPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      surfelAccelerationInsertPasses[surfelFrameIndex]->setEnabled(surfelPassEnabled && useSurfels);
                      surfelAccelerationInsertPasses[surfelFrameIndex]->setSettings(surfelRuntimeSettings);
                      surfelAccelerationInsertPasses[surfelFrameIndex]->updateUniforms(cameraPosition, surfelCameraRight, surfelCameraUp, surfelCameraForward);
                    });
              }
              if (surfelDirectDrawDebugPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      const bool surfelDirectDrawEnabled = useSurfels && surfelPassEnabled && rendererDisplayOutputUsesSurfelDirectDraw(editableRebuildSettings.displayOutput);
                      surfelDirectDrawDebugPasses[surfelFrameIndex]->setEnabled(surfelDirectDrawEnabled);
                      surfelDirectDrawDebugPasses[surfelFrameIndex]->setSettings(
                          globalillumination::surfels::SurfelDirectDrawDebugPass::Settings{
                            .viewPortWidth = kRenderWidth,
                            .viewPortHeight = kRenderHeight,
                            .surfelCapacity = surfelRuntimeSettings.surfelCapacity,
                            .gridCapacity = surfelRuntimeSettings.gridCapacity,
                            .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
                            .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
                            .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
                            .gridScale = surfelRuntimeSettings.gridScale,
                            .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
                            .lookupRadiusScale = surfelRuntimeSettings.lookupRadiusScale,
                            .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
                            .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
                            .debugMode = static_cast<uint32_t>(rendererDisplayOutputSurfelDirectDrawDebugMode(editableRebuildSettings.displayOutput)),
                          });
                      surfelDirectDrawDebugPasses[surfelFrameIndex]->updateUniforms(surfelSpawnInverseViewProjection, cameraPosition, surfelCameraRight, surfelCameraUp, surfelCameraForward, cam.isReverseZ());
                    });
              }
              if (surfelHeatmapDebugPasses[surfelFrameIndex] != nullptr)
              {
                measureWallTimeMs(
                    [&]()
                    {
                      const bool surfelHeatmapEnabled = useSurfels && surfelPassEnabled && rendererDisplayOutputUsesSurfelHeatmap(editableRebuildSettings.displayOutput);
                      surfelHeatmapDebugPasses[surfelFrameIndex]->setEnabled(surfelHeatmapEnabled);
                      surfelHeatmapDebugPasses[surfelFrameIndex]->setSettings(
                          globalillumination::surfels::SurfelHeatmapDebugPass::Settings{
                            .viewPortWidth = kRenderWidth,
                            .viewPortHeight = kRenderHeight,
                            .gridCapacity = surfelRuntimeSettings.gridCapacity,
                            .listCapacity = globalillumination::surfels::surfelListCapacity(surfelRuntimeSettings),
                            .lookupCellResolution = surfelRuntimeSettings.lookupCellResolution,
                            .centralGridHalfExtent = surfelRuntimeSettings.centralGridHalfExtent,
                            .gridScale = surfelRuntimeSettings.gridScale,
                            .frustumFarDistance = surfelRuntimeSettings.frustumFarDistance,
                            .spawnMinRadius = surfelRuntimeSettings.spawnMinRadius,
                            .spawnDepthRadiusScale = surfelRuntimeSettings.spawnDepthRadiusScale,
                          });
                      surfelHeatmapDebugPasses[surfelFrameIndex]->updateUniforms(surfelSpawnInverseViewProjection, cameraPosition, surfelCameraRight, surfelCameraUp, surfelCameraForward, cam.isReverseZ());
                    });
              }
            }
            if (gpuSignedDistanceFieldsFallbackDebugPreparePass != nullptr)
            {
              const math::Mat4f viewProjection = projShading * view;
              measureWallTimeMs(
                  [&]()
                  {
                    gpuSignedDistanceFieldsFallbackDebugPreparePass->setFrustumCullingEnabled(sdfFallbackSceneFrustumCullingEnabled);
                    gpuSignedDistanceFieldsFallbackDebugPreparePass->updateRuntimeState(viewProjection);
                  });
            }
            if (gpuSignedDistanceFieldsFallbackDebugDrawPass != nullptr)
            {
              const math::Mat4f viewProjection = projShading * view;
              measureWallTimeMs(
                  [&]()
                  {
                    gpuSignedDistanceFieldsFallbackDebugDrawPass->setMode(rendererDisplayOutputFallbackSceneMode(editableRebuildSettings.displayOutput));
                    gpuSignedDistanceFieldsFallbackDebugDrawPass->updateRuntimeState(viewProjection, cam.isReverseZ());
                  });
            }
            if (gpuSignedDistanceFieldsTraceDebugPass != nullptr)
            {
              // Match the debug trace camera to the shaded scene-depth buffer.
              // The trace pass reconstructs world positions from sceneDepthTexture,
              // so it must use the same jittered projection that produced that depth.
              const math::Mat4f inverseViewProjection = (projShading * view).inverse();
              measureWallTimeMs(
                  [&]()
                  {
                    sdfTraceDebugSettings.surfaceCacheMaterialsEnabled = sdfTraceSurfaceCacheMaterialsEnabled;
                    if (rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput))
                      sdfTraceDebugSettings.maxSteps = std::max(sdfTraceDebugSettings.maxSteps, 256u);
                    gpuSignedDistanceFieldsTraceDebugPass->setSettings(sdfTraceDebugSettings);
                    gpuSignedDistanceFieldsTraceDebugPass->updateRuntimeState(inverseViewProjection, cam.getPosition(), cam.isReverseZ());
                  });
            }
            if (sparseSdfTraceDebugPass != nullptr)
            {
              const math::Mat4f inverseViewProjection = (projShading * view).inverse();
              measureWallTimeMs(
                  [&]()
                  {
                    const auto selectedLegacyTraceMode = rendererDisplayOutputSdfTraceMode(editableRebuildSettings.displayOutput, sdfTraceDebugSettings.mode);
                    sparseSdfTraceDebugSettings.mode =
                        rendererDisplayOutputUsesLocalInstanceSdf(editableRebuildSettings.displayOutput)
                            ? rendererDisplayOutputSparseSdfTraceMode(editableRebuildSettings.displayOutput, sparseSdfTraceDebugSettings.mode)
                            : sparseSdfTraceModeFromLegacyTraceMode(selectedLegacyTraceMode, sparseSdfTraceDebugSettings.mode);
                    sparseSdfTraceDebugPass->setSettings(sparseSdfTraceDebugSettings);
                    sparseSdfTraceDebugPass->updateRuntimeState(inverseViewProjection, cam.getPosition(), cam.isReverseZ());
                  });
            }
            if (gpuSignedDistanceFieldsAtlasDebugPass != nullptr)
            {
              measureWallTimeMs(
                  [&]()
                  {
                    gpuSignedDistanceFieldsAtlasDebugPass->setSettings(sdfAtlasDebugSettings);
                  });
            }

            const bool shadowContentChanged = manualObjectInput || animationUpdatedThisFrame;
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
	                    vsmManager.queueInvalidateRegion(shadowInvalidationBounds);
	                  });
	            }
          });
      cpuFrame.uniformUploadMs = cpuFrame.uniformUpload.wallMs;

      // --- Submit frame --------------------------------------------------------
      RenderGraph::Frame frame;
      frame.frameIndex = frameSlot;
      const auto acquireStart = lib::time::TimeSpan::now();
      auto currentSwapView = rhi->getCurrentSwapChainTextureView(swapChain);
      cpuFrame.acquireMs = (lib::time::TimeSpan::now() - acquireStart).milliseconds();

      if (gpuSignedDistanceFieldsPass)
        gpuSignedDistanceFieldsPass->preFrame();
      {
        const auto runStart = lib::time::TimeSpan::now();
        const RenderGraph::Frame *previousFrame = pendingFrameDiagnostics.empty() ? nullptr : &pendingFrameDiagnostics.back().frame;
        const auto &handleTable = previousFrame == nullptr ? swapChainInitCommandBufferHandles : swapChainCommandBufferHandles;
        renderGraph->run(frame, handleTable.at(frameSlot).at(currentSwapView.index), previousFrame);
        cpuFrame.renderRunMs = (lib::time::TimeSpan::now() - runStart).milliseconds();
      }
      worldCwbvhWorldBlasBakePending = false;
      if (gpuSignedDistanceFieldsPass != nullptr)
      {
        // The SDF build is serialized against the previous frame's last combine/debug
        // consumers via explicit inter-frame dependencies. Advance the CPU-visible
        // initialization state as soon as this frame is submitted so subsequent queued
        // frames do not keep re-entering the startup initialization path.
        gpuSignedDistanceFieldsPass->postFrame();
      }
      previousViewMatrix = view;
      previousProjectionMatrix = projShading;
      previousCameraPosition = cameraPosition;
      previousCameraRotation = cameraRotation;
      previousTaaJitter = taaJitter;
      hasPreviousCameraFrame = true;
      hasTaaHistory = kTaaEnabled;
      hasRsmTemporalHistory = useRsmScreenPass &&
                              editableRebuildSettings.rsmGiTemporalAccumulation &&
                              rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput) == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse;
      hasRsmDenoiserHistory = useRsmScreenPass &&
                              editableRebuildSettings.rsmGiDenoiserEnabled &&
                              rendererDisplayOutputRsmDebugMode(editableRebuildSettings.displayOutput) == VirtualShadowMapRsmIndirectDiffusePass::DebugMode::IndirectDiffuse;
      previousRsmTemporalObjectRotation = objectRotation;
      previousRsmTemporalLightDirection = lightDirection;
      previousRsmTemporalDirectionalColor = directionalLightColor;
      previousRsmTemporalPointColor = pointLightColor;
      previousRsmTemporalDirectionalIntensity = directionalLightIntensity;
      previousRsmTemporalPointIntensity = pointLightIntensity;
      previousRsmTemporalPointRange = pointLightRange;
      previousRsmTemporalDirectionalEnabled = directionalLightEnabled;
      previousRsmTemporalPointEnabled = pointLightEnabled;
      hasRsmTemporalPayloadSignature = true;

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
        const softwarebvh::SoftwareTLASManager::BuildSummary softwareTlasSummary = softwareTlasManager != nullptr ? softwareTlasManager->getLastBuildSummary() : softwarebvh::SoftwareTLASManager::BuildSummary{};
        const softwarebvh::SoftwareBLASManager::BuildSummary softwareBlasSummary = softwareBlasManager != nullptr ? softwareBlasManager->getLastBuildSummary() : softwarebvh::SoftwareBLASManager::BuildSummary{};
        const softwarebvh::SoftwareBLASSceneBridge::Diagnostics softwareBlasDiagnostics =
            softwareBlasSceneBridge != nullptr ? softwareBlasSceneBridge->getDiagnostics() : softwarebvh::SoftwareBLASSceneBridge::Diagnostics{};
        const softwarebvh::SoftwareTLASSceneBridge::Diagnostics softwareTlasDiagnostics =
            softwareTlasSceneBridge != nullptr ? softwareTlasSceneBridge->getDiagnostics() : softwarebvh::SoftwareTLASSceneBridge::Diagnostics{};
        const uint32_t ismVplRuntimeProxyCount = static_cast<uint32_t>(ismVpls.size());
        const uint32_t ismVplRuntimeAllocatedCount = static_cast<uint32_t>(std::count_if(
            ismVpls.begin(),
            ismVpls.end(),
            [](const ImperfectShadowMapVPL &vpl)
            {
              return vpl.lightId != VirtualShadowMapManager::INVALID_LIGHT_ID;
            }));
        const RendererDisplayOutput displayOutputBeforeControls = editableRebuildSettings.displayOutput;
        const RendererControlsOverlayResult controlsOverlayResult = buildShadowControlsOverlay(
            prepassLodErrorThreshold,
            geometryLodErrorThreshold,
            vsmLodErrorThreshold,
            finalSoftwarePixelsPerTriangleThreshold,
            directionalLightEnabled,
            directionalLightColor,
            directionalLightIntensity,
            pointLightEnabled,
            pointLightColor,
            pointLightIntensity,
            pointLightRange,
            shadowPcfEnabled,
            screenSpaceSoftShadowSettings,
            shadowFilterTaps,
            fallbackShadowFilterTaps,
            pcssBlockerSearchTaps,
            directionalShadowBias,
            spotShadowBias,
            pointShadowBias,
            directionalSlopeScaleBias,
            spotSlopeScaleBias,
            pointSlopeScaleBias,
            directionalMaxShadowBias,
            spotMaxShadowBias,
            pointMaxShadowBias,
            pcfRadiusTexels,
            pcssLightRadiusTexels,
            pcssMaxPenumbraTexels,
            pcssPenumbraScale,
            vsmLocalLightMipTransitionScale,
            vsmLocalLightMipBias,
            directionalNormalBiasTexels,
            spotNormalBiasTexels,
            pointNormalBiasTexels,
            normalLightingEnabled,
            normalLightingThreshold,
            normalLightingExponent,
            shadowAmbientColor,
            screenSpaceShadowEnabled,
            screenSpaceSurfaceThickness,
            screenSpaceBilinearThreshold,
            screenSpaceShadowContrast,
            screenSpaceDepthBounds,
            screenSpaceIgnoreEdgePixels,
            screenSpaceUsePrecisionOffset,
            screenSpaceBilinearSamplingOffsetMode,
            screenSpaceUseEarlyOut,
            screenSpaceTreatSkippedEdgeSamplesAsLit,
            screenSpaceDebugOutputEdgeMask,
            screenSpaceDebugOutputThreadIndex,
            screenSpaceDebugOutputWaveIndex,
            vsmPagesOverlayEnabled,
            vsmPagesOverlayOpacity,
            spatialRadianceCacheOverlayEnabled,
            spatialRadianceCacheOverlayOpacity,
            ismVplPointsOverlayEnabled,
            ismVplPointsOverlayOpacity,
            worldGILightGridOverlayEnabled,
            worldGILightGridOverlayOpacity,
            worldGILightGridOverlayCascade,
            worldGILightGridOverlayLineWidth,
            screenProbeScreenSpaceTracingEnabled,
            screenProbeRayMaxDistance,
            screenProbeRayOriginBias,
            screenProbeScreenSpaceMaxSteps,
            screenProbeScreenSpaceStepPixels,
            screenProbeScreenSpaceDepthThickness,
            screenProbeScreenSpaceRelativeThickness,
            screenProbeSdfFallbackEnabled,
            screenProbeSdfMaxSteps,
            screenProbeSdfMaxDistance,
            screenProbeSdfHitThresholdScale,
            screenProbeSdfEmptySpaceStepScale,
            screenProbeSdfScreenNormalMatchBaseThreshold,
            screenProbeSdfScreenNormalMatchCascadeScale,
            sdfTraceSurfaceCacheMaterialsEnabled,
            screenProbeSkyEnabled,
            screenProbeGuidedCandidateCount,
            screenProbeTemporalBlend,
            screenProbeHistoryWeightScale,
            screenProbeRecursiveBounceBlend,
            screenProbeDisocclusionThreshold,
            screenProbePlaneDistanceThreshold,
            screenProbeNormalThreshold,
            screenProbeResolveFirstCacheBias,
            screenProbeResolveSecondCacheBias,
            screenProbeResolvePlaneDistanceScale,
            screenProbeResolveNormalWeightExponent,
            surfelHierarchySettings,
            surfelRuntimeSettings,
            surfelHierarchyFreeze,
            surfelAllocatedCount,
            screenSpaceRayTraceEnabled,
            screenSpaceRayTraceScreenTracesEnabled,
            screenSpaceRayTraceForceWorldFallback,
            screenSpaceRayTraceTraceAllForcedFallbackRays,
            kRenderWidth,
            kRenderHeight,
            screenSpaceRayTraceTileSize,
            screenSpaceRayTraceRaysPerTile,
            screenSpaceRayTraceMaxSteps,
            screenSpaceRayTraceMaxFailedRays,
            screenSpaceRayTraceSdfFallbackEnabled,
            screenSpaceRayTraceSoftwareFallbackEnabled,
            screenSpaceRayTraceSoftwareFallbackTraceStats,
            screenSpaceRayTraceSoftwareFallbackBudget,
            screenSpaceRayTraceMinDistance,
            screenSpaceRayTraceMaxDistance,
            screenSpaceRayTraceSurfaceThickness,
            screenSpaceRayTraceOriginBias,
            latestScreenSpaceRayTraceStats,
            softwareRayTraceMinDistance,
            softwareRayTraceMaxDistance,
            softwareRayTraceHeatmapMaxIterations,
            softwareRayTracePrimitiveMode,
            softwareRayTraceTraversalMode,
            softwareRayTraceTlasMode,
            softwareRayTraceFallbackLod,
            softwareRayTraceRbvhLod,
            softwareRayTraceShaderVariant,
            softwareRayTraceOverlayTLASBounds,
            softwareRayTraceOverlayTLASCwbvhBounds,
            softwareRayTraceOverlayTLASLevel,
            softwareRayTraceOverlayLineThickness,
            softwareRayTraceTraversalEarlyOut,
            softwareRayTraceTraversalEarlyOutBias,
            softwareRayTraceIsolateTLASInstance,
            softwareRayTraceSelectedTLASInstance,
            softwareTlasDiagnostics.candidateInstanceCount,
            softwareTlasDiagnostics.filteredInstanceCount,
            softwareTlasDiagnostics.selectedSceneInstanceId,
            softwareTlasDiagnostics.validInstanceCount,
            softwareTlasDiagnostics.skippedMissingBlasCount,
            softwareTlasDiagnostics.skippedInvalidBoundsCount,
            softwareTlasManager != nullptr ? softwareTlasManager->getCanonicalNodeCount() : softwareTlasSummary.nodeCount,
            softwareTlasManager != nullptr ? softwareTlasManager->getCwbvhNodeCount() : 0u,
            softwareTlasManager != nullptr ? softwareTlasManager->getBvh4NodeCount() : 0u,
            softwareTlasSummary.instanceCount,
            softwareTlasSummary.bvh4BuildSahScore,
            softwareTlasSummary.bvh4CurrentSahScore,
            softwareTlasSummary.bvh4SahGrowth,
            softwareTlasSummary.bvh4RebuiltForSah,
            softwareBlasSummary.blasCount,
            softwareBlasManager != nullptr ? softwareBlasManager->getCanonicalNodeCount() : softwareBlasSummary.nodeCount,
            softwareBlasManager != nullptr ? softwareBlasManager->getGPUCWBVHNodeCount() : 0u,
            softwareBlasSummary.primitiveCount,
            softwareBlasSummary.rbvhCardCount,
            softwareBlasDiagnostics.preparedMeshCount,
            softwareBlasDiagnostics.availableFallbackMeshCount,
            softwareBlasDiagnostics.missingFallbackMeshCount,
            softwareBlasDiagnostics.skippedMissingFallbackMeshCount,
            latestScreenProbeTraceStats,
            latestOverlaySnapshot ? latestOverlaySnapshot->softwareRayTraceStats : SoftwareRayTraceFrameStats{},
            latestOverlaySnapshot ? latestOverlaySnapshot->screenFallbackTraceStats : SoftwareRayTraceFrameStats{},
            latestOverlaySnapshot ? latestOverlaySnapshot->surfelTraceStats : SurfelTraceStats{},
            latestSurfelIrradianceDebugStats,
            vsmDebugLayer,
            ismVplDebugLayer,
            ismVplDebugSelectedVpl,
            ismVplProjectedLightIndex,
            ismVplDebugPointRadius,
            ismVplDebugExposure,
            ismVplRuntimeProxyCount,
            ismVplRuntimeAllocatedCount,
            ismVplShadowManager.getActiveLightCount(),
            ismVplShadowManager.getActiveLayerCount(),
            ismVplShadowManager.getVirtualPageTableResolution(),
            ismVplShadowManager.getPhysicalPageTableResolution(),
            ismVplShadowManager.getPhysicalPageResolution(),
            ismVplBuildStats,
            vsmProjectedLightIndex,
            editableRebuildSettings,
            appliedRebuildSettings);
        if (softwareRayTracePass != nullptr && (selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SoftwareRayTrace || selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SoftwareRayTraceHeatmap))
        {
          const SoftwareRayTraceFrameStats swrtStats = latestOverlaySnapshot ? latestOverlaySnapshot->softwareRayTraceStats : SoftwareRayTraceFrameStats{};
          const auto &swrtSettings = softwareRayTracePass->getSettings();
          const softwarebvh::SoftwareBLASSceneBridge::Diagnostics swrtBlasDiagnostics =
              softwareBlasSceneBridge != nullptr ? softwareBlasSceneBridge->getDiagnostics() : softwarebvh::SoftwareBLASSceneBridge::Diagnostics{};
          const softwarebvh::SoftwareTLASSceneBridge::Diagnostics swrtTlasDiagnostics =
              softwareTlasSceneBridge != nullptr ? softwareTlasSceneBridge->getDiagnostics() : softwarebvh::SoftwareTLASSceneBridge::Diagnostics{};
          const bool shouldLogSwrtDiagnostics = swrtStats.totalRays == 0u || softwareBlasSummary.blasCount == 0u || softwareTlasSummary.instanceCount == 0u || swrtBlasDiagnostics.skippedMissingFallbackMeshCount > 0u ||
                                                swrtTlasDiagnostics.skippedMissingBlasCount > 0u || lastSwrtDiagnosticsLogDisplayFrame == 0u || displayFrameIndex >= lastSwrtDiagnosticsLogDisplayFrame + 30u;
          if (shouldLogSwrtDiagnostics)
          {
            lastSwrtDiagnosticsLogDisplayFrame = displayFrameIndex;
            os::Logger::logf(
                "[SWRT] frame=%u passEnabled=%s stats(total=%u hit=%u miss=%u invalid=%u tlasVisits=%u blasVisits=%u worldBlasVisits=%u triTests=%u lastHitValid=%u lastHitPrim=%u lastHitInst=%u) TLAS(instances=%u canonical=%u cwbvh=%u bvh4=%u bvh4Sah=%.3f/%.3f %.2fx rebuiltSah=%s) "
                "BLAS(count=%u canonical=%u cwbvh=%u primitives=%u rbvhCards=%u) BLASBridge(prepared=%u unique=%u loaded=%u missing=%u skippedFallback=%u skippedRuntime=%u rebuild=%s runtimeRbvhBuilds=%u originalSources=%u proxySources=%u sourceTriangles=%llu buildMs=%.2f) TLASBridge(prepared=%u valid=%u dirty=%u "
                "skippedObject=%u skippedSceneInstance=%u skippedBLAS=%u skippedBounds=%u rebuild=%s refit=%s)",
                displayFrameIndex,
                swrtSettings.enabled ? "on" : "off",
                swrtStats.totalRays,
                swrtStats.hitRays,
                swrtStats.missRays,
                swrtStats.invalidDirectionRays,
                swrtStats.tlasNodeVisits,
                swrtStats.blasNodeVisits,
                swrtStats.worldBlasNodeVisits,
                swrtStats.triangleTests,
                swrtStats.lastHitValid,
                swrtStats.lastHitPrimitive,
                swrtStats.lastHitInstance,
                softwareTlasSummary.instanceCount,
                softwareTlasSummary.nodeCount,
                softwareTlasManager != nullptr ? softwareTlasManager->getCwbvhNodeCount() : 0u,
                softwareTlasManager != nullptr ? softwareTlasManager->getBvh4NodeCount() : 0u,
                softwareTlasSummary.bvh4BuildSahScore,
                softwareTlasSummary.bvh4CurrentSahScore,
                softwareTlasSummary.bvh4SahGrowth,
                softwareTlasSummary.bvh4RebuiltForSah ? "yes" : "no",
                softwareBlasSummary.blasCount,
                softwareBlasSummary.nodeCount,
                softwareBlasManager != nullptr ? softwareBlasManager->getGPUCWBVHNodeCount() : 0u,
                softwareBlasSummary.primitiveCount,
                softwareBlasSummary.rbvhCardCount,
                swrtBlasDiagnostics.preparedMeshCount,
                swrtBlasDiagnostics.uniqueMeshCount,
                swrtBlasDiagnostics.availableFallbackMeshCount,
                swrtBlasDiagnostics.missingFallbackMeshCount,
                swrtBlasDiagnostics.skippedMissingFallbackMeshCount,
                swrtBlasDiagnostics.skippedMissingRuntimeDataCount,
                swrtBlasDiagnostics.requestedRebuild ? "yes" : "no",
                swrtBlasDiagnostics.runtimeRbvhBuildCount,
                swrtBlasDiagnostics.runtimeRbvhOriginalSourceCount,
                swrtBlasDiagnostics.runtimeRbvhFallbackSourceCount,
                static_cast<unsigned long long>(swrtBlasDiagnostics.runtimeRbvhSourceTriangleCount),
                swrtBlasDiagnostics.runtimeRbvhBuildMilliseconds,
                swrtTlasDiagnostics.preparedInstanceCount,
                swrtTlasDiagnostics.validInstanceCount,
                swrtTlasDiagnostics.dirtyInstanceCount,
                swrtTlasDiagnostics.skippedMissingObjectCount,
                swrtTlasDiagnostics.skippedMissingSceneInstanceCount,
                swrtTlasDiagnostics.skippedMissingBlasCount,
                swrtTlasDiagnostics.skippedInvalidBoundsCount,
                swrtTlasDiagnostics.requestedRebuild ? "yes" : "no",
                swrtTlasDiagnostics.requestedRefit ? "yes" : "no");
            if (!swrtBlasDiagnostics.lastLoadedFallbackPath.empty())
            {
              os::Logger::logf("[SWRT] last loaded fallback: %s", swrtBlasDiagnostics.lastLoadedFallbackPath.c_str());
            }
            if (!swrtBlasDiagnostics.lastMissingFallbackPath.empty())
            {
              os::Logger::warningf("[SWRT] last missing fallback: %s", swrtBlasDiagnostics.lastMissingFallbackPath.c_str());
            }
            if (!swrtBlasDiagnostics.lastRuntimeRbvhBuildSourcePath.empty())
            {
              os::Logger::logf("[SWRT][RBVH] last runtime build source: %s", swrtBlasDiagnostics.lastRuntimeRbvhBuildSourcePath.c_str());
            }
          }
        }
        if (editableRebuildSettings.displayOutput != displayOutputBeforeControls)
        {
          const uint32_t newDebugTargetSlot = rendererDisplayOutputTargetSlot(editableRebuildSettings.displayOutput);
          updateSelectedDebugTarget(newDebugTargetSlot);
          os::Logger::logf("[Display] runtime output=%s slot=%u", rendererDisplayOutputLabel(editableRebuildSettings.displayOutput), newDebugTargetSlot);
        }
        if (controlsOverlayResult.runtimeSettingsChanged)
        {
          applyShadowFilterSettings();
          updateSelectedDebugTarget(selectedDebugTargetSlot);
          updateVsmPagesOverlay();
          updateSpatialRadianceCacheOverlay();
          updateIsmVplPointsOverlay();
          updateWorldGILightGridOverlay();
          logShadowFilterSettings();
        }
        if (controlsOverlayResult.rebuildRequested)
        {
          restartRendererRequested = true;
        }
        const bool showSignedDistanceFieldsOverlay =
            editableRebuildSettings.enableSignedDistanceFields || rendererDisplayOutputUsesFallbackScene(editableRebuildSettings.displayOutput) ||
            selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace ||
            selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsAtlas ||
            selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsFallbackScene;
        if (showSignedDistanceFieldsOverlay && gpuSignedDistanceFieldsManager && gpuSignedDistanceFieldsSceneManager)
        {
          buildSignedDistanceFieldsOverlay(
              *gpuSignedDistanceFieldsManager,
              sceneManager,
              *gpuSignedDistanceFieldsSceneManager,
              sdfTraceDebugSettings,
              sparseSdfTraceDebugSettings,
              sdfAtlasDebugSettings,
              gpuSignedDistanceFieldsFallbackDebugPreparePass,
              gpuSignedDistanceFieldsTraceDebugPass != nullptr && selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsTrace,
              gpuSignedDistanceFieldsTraceDebugPass != nullptr && gpuSignedDistanceFieldsTraceDebugPass->isEnabled(),
              gpuSignedDistanceFieldsTraceDebugPass != nullptr && gpuSignedDistanceFieldsTraceDebugPass->hasLocalTraceStatsResources(),
              gpuSignedDistanceFieldsTraceDebugPass != nullptr ? gpuSignedDistanceFieldsTraceDebugPass->getRecordCallCount() : 0u,
              gpuSignedDistanceFieldsTraceDebugPass != nullptr ? gpuSignedDistanceFieldsTraceDebugPass->getRecordDispatchCount() : 0u,
              gpuSignedDistanceFieldsTraceDebugPass != nullptr ? gpuSignedDistanceFieldsTraceDebugPass->getRecordSkippedDisabledCount() : 0u,
              latestSdfLocalTraceStats,
              sdfFallbackSceneFrustumCullingEnabled,
              latestOverlaySnapshot ? latestOverlaySnapshot->sdfStats : SDFFrameStats{});
          sdfSceneManagerSettings = gpuSignedDistanceFieldsSceneManager->getSettings();
          updateSelectedDebugTarget(selectedDebugTargetSlot);
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
              buildVirtualTextureFrameStats(scene.getVirtualTextureSystem(), materialPass.get()),
              latestOverlaySnapshot->passTimings,
              latestOverlaySnapshot->overlayHistory);
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
        updateVsmPagesOverlay();
        updateSpatialRadianceCacheOverlay();
        updateWorldGILightGridOverlay();
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
      if (!isPaused)
      {
        windowTitleStats.push(nativeWindow->sdlWindow, deltaTime);
      }

      PendingFrameDiagnostics currentPending{};
      currentPending.frame = std::move(frame);
      currentPending.displayFrameIndex = displayFrameIndex;
      currentPending.frameDeltaMs = deltaTime;
      currentPending.collectFrameMetrics = !isPaused;
      currentPending.timerResolveSubmitted = currentPending.collectFrameMetrics && renderGraph->isTimerReadbackEnabled();
      currentPending.cpuFrame = cpuFrame;
      currentPending.renderGraphCpuStart = renderGraphCpuStart;
      currentPending.renderGraphRunTotalMs = renderGraph->getLastRunTotalMs();
      currentPending.renderGraphRunPhases = renderGraph->getLastRunPhaseRecords();
      currentPending.renderGraphRuntimeMetrics = renderGraph->getLastRuntimeMetricRecords();
      currentPending.renderGraphRunDebugStats = renderGraph->getLastRunDebugStats();
      currentPending.fallbackScene.capture = selectedDebugTargetSlot == SelectedDebugTargetPass::TargetSlot_SignedDistanceFieldsFallbackScene && gpuSignedDistanceFieldsManager != nullptr &&
                                             gpuSignedDistanceFieldsSceneManager != nullptr && gpuSignedDistanceFieldsFallbackDebugPreparePass != nullptr;
      if (currentPending.fallbackScene.capture)
      {
        currentPending.fallbackScene.preparePassEnabled = gpuSignedDistanceFieldsFallbackDebugPreparePass->isEnabled();
        currentPending.fallbackScene.frustumCullingEnabled = sdfFallbackSceneFrustumCullingEnabled;
        currentPending.fallbackScene.staticDrawCount = gpuSignedDistanceFieldsManager->getPathResources(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static).instanceCount;
        currentPending.fallbackScene.skinnedDrawCount = gpuSignedDistanceFieldsManager->getPathResources(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned).instanceCount;
        currentPending.fallbackScene.uploadedVertexCount = static_cast<uint32_t>(gpuSignedDistanceFieldsManager->getUploadedVertices().size());
        currentPending.fallbackScene.uploadedIndexCount = static_cast<uint32_t>(gpuSignedDistanceFieldsManager->getUploadedIndices().size());
        currentPending.fallbackScene.uploadedBoneWeightCount = static_cast<uint32_t>(gpuSignedDistanceFieldsManager->getUploadedBoneWeights().size());
        currentPending.fallbackScene.frustumPlanes = gpuSignedDistanceFieldsFallbackDebugPreparePass->getFrustumPlanes();
        currentPending.fallbackScene.bridgeDiagnostics = gpuSignedDistanceFieldsSceneManager->getDiagnostics();
        currentPending.fallbackScene.meshTable = gpuSignedDistanceFieldsManager->getUploadedMeshTable();
        currentPending.fallbackScene.staticInstances = gpuSignedDistanceFieldsManager->getUploadedInstances(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static);
        currentPending.fallbackScene.skinnedInstances = gpuSignedDistanceFieldsManager->getUploadedInstances(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned);
        currentPending.fallbackScene.preparedStaticCommands = gpuSignedDistanceFieldsFallbackDebugPreparePass->getPreparedCommands(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Static);
        currentPending.fallbackScene.preparedSkinnedCommands = gpuSignedDistanceFieldsFallbackDebugPreparePass->getPreparedCommands(gpusigneddistancefields::GPUSignedDistanceFieldsManager::Path::Skinned);
      }
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
    os::Logger::logf("[Restart] finalized pending frame diagnostics, remaining=%zu", pendingFrameDiagnostics.size());
    os::Logger::log("[Restart] stopping overlay preparation worker");
    overlayPreparationWorker.stop();
    os::Logger::log("[Restart] waiting for RHI idle before tearing down render graph resources");
    rhi->waitIdle();
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
    destroyReadbackBufferSet(screenSpaceRayTraceReadbackBuffers.traceStats);
    destroyReadbackBufferSet(surfelReadbackBuffers.traceStats);
    destroyReadbackBufferSet(softwareRayTraceReadbackBuffers.traceStats);
    destroyReadbackBufferSet(softwareRayTraceReadbackBuffers.screenFallbackTraceStats);
    const size_t totalRecordedCommandBufferCount = (swapChainInitCommandBufferHandles.size() + swapChainCommandBufferHandles.size()) * static_cast<size_t>(rhi->getSwapChainImagesCount(swapChain));
    os::Logger::logf("[Restart] destroying %zu recorded command buffers", totalRecordedCommandBufferCount);
    size_t destroyHandleIndex = 0u;
    for (const auto &frameHandles : swapChainInitCommandBufferHandles)
    {
      for (const RenderGraph::CommandBufferHandle handle : frameHandles)
      {
        renderGraph->destroyCommandBuffer(handle);
        ++destroyHandleIndex;
      }
    }
    for (const auto &frameHandles : swapChainCommandBufferHandles)
    {
      for (const RenderGraph::CommandBufferHandle handle : frameHandles)
      {
        renderGraph->destroyCommandBuffer(handle);
        ++destroyHandleIndex;
      }
    }
    os::Logger::log("[Restart] waiting for RHI idle after destroying recorded command buffers");
    rhi->waitIdle();
    os::Logger::logf("[Restart] deleting %zu passes", ownedPasses.size());
    size_t deletePassIndex = 0u;
    for (auto it = ownedPasses.rbegin(); it != ownedPasses.rend(); ++it)
    {
      if (*it != nullptr)
      {
        const char *passTypeName = typeid(**it).name();
        const std::string deletionContext = std::string(passTypeName) + " name=" + (*it)->getPassName();
        renderGraph->setDeletionContext(deletionContext);
        delete *it;
        renderGraph->clearDeletionContext();
      }
      ++deletePassIndex;
    }
    os::Logger::logf("[Restart] deleting %zu owned buffers", ownedBuffers.size());
    for (const Buffer &buffer : ownedBuffers)
    {
      if (buffer.isValid())
      {
        renderGraph->deleteBuffer(buffer);
      }
    }
    os::Logger::logf("[Restart] deleting %zu owned textures", ownedTextures.size());
    for (const Texture &texture : ownedTextures)
    {
      if (texture.isValid())
      {
        renderGraph->deleteTexture(texture);
      }
    }
    for (const auto &textureIds : ownedTextureOverrideIds)
    {
      for (const TextureId textureId : textureIds)
      {
        if (textureId != TextureId::Invalid)
        {
          rhi->deleteTexture(textureId);
        }
      }
    }
    for (const auto &bufferIds : ownedBufferOverrideIds)
    {
      for (const BufferId bufferId : bufferIds)
      {
        if (bufferId != BufferId::Invalid)
        {
          rhi->deleteBuffer(bufferId);
        }
      }
    }
    os::Logger::log("[Restart] removing swapchain images from render graph");
    renderGraph->removeSwapChainImages(swapChain);
    os::Logger::log("[Restart] waiting for RHI idle after render graph teardown");
    rhi->waitIdle();
    if (recreateSwapChainRequested && !window->shouldClose())
    {
      os::Logger::log("[Restart] destroying ImGui framebuffers for swapchain recreation");
      for (const auto &[_, framebuffer] : imguiFramebuffers)
      {
        if (framebuffer != VK_NULL_HANDLE)
        {
          vkDestroyFramebuffer(rhi->getDevice(), framebuffer, nullptr);
        }
      }
      imguiFramebuffers.clear();

      os::Logger::log("[Restart] recreating swapchain");
      rhi->destroySwapChain(swapChain);
      swapChain = rhi->createSwapChain(0, nativeWindow->width, nativeWindow->height);
      ImGui_ImplVulkan_SetMinImageCount(std::max(2u, rhi->getSwapChainImagesCount(swapChain)));
      recreateSwapChainRequested = false;
    }

    if (restartRendererRequested)
    {
      hasPreviousCameraFrame = false;
      hasTaaHistory = false;
      hasRsmTemporalHistory = false;
      hasRsmDenoiserHistory = false;
      hasRsmTemporalPayloadSignature = false;
      previousViewMatrix = cam.getViewMatrix();
      previousProjectionMatrix = cam.getProjectionMatrix();
      previousTaaJitter = math::Vec2f(0.0f);
    }
  }

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
  rhi->destroySwapChain(swapChain);
  os::Logger::shutdown();
  windowTitleStats.reset(nativeWindow->sdlWindow);
  delete window;
  delete rhi;

  return 0;
}
