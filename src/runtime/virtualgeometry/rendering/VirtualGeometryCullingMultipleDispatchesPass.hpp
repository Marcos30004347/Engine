#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

using namespace rendering;

namespace virtualgeometry
{
namespace gpgpu
{

enum class HierarchyDebugReason : uint32_t
{
  NotVisited = 0u,
  EnqueuedChildren = 1u,
  EnqueuedClusters = 2u,
  CulledParentErrorTooSmall = 10u,
  CulledFrustum = 11u,
  CulledOcclusion = 12u,
  CulledNotInstalled = 13u,
  CulledInvalidChildStart = 14u,
  CulledProjectedTooSmall = 15u,
};

enum class ClusterDebugReason : uint32_t
{
  NotVisited = 0u,
  RenderedHardware = 1u,
  RenderedSoftware = 2u,
  InvalidQueueElement = 10u,
  InvalidPageIndex = 11u,
  PageNotInstalled = 12u,
  ZeroTriangleCount = 13u,
  LocalBitOutOfRange = 14u,
  DisabledByCutMask = 15u,
  SelfErrorTooHigh = 16u,
  ParentErrorTooLow = 17u,
  ConeCulled = 18u,
  ProjectedTooSmall = 19u,
};

struct HierarchyDebugRecord
{
  uint32_t reason;
  uint32_t parentErrorPxBits;
  uint32_t thresholdPxBits;
  uint32_t instanceIndex;
};
static_assert(sizeof(HierarchyDebugRecord) == 16u);

struct ClusterDebugRecord
{
  uint32_t reason;
  uint32_t selfErrorPxBits;
  uint32_t parentErrorPxBits;
  uint32_t nodeIndex;
};
static_assert(sizeof(ClusterDebugRecord) == 16u);

inline const char *hierarchyDebugReasonToString(HierarchyDebugReason reason)
{
  switch (reason)
  {
  case HierarchyDebugReason::NotVisited:
    return "NotVisited";
  case HierarchyDebugReason::EnqueuedChildren:
    return "EnqueuedChildren";
  case HierarchyDebugReason::EnqueuedClusters:
    return "EnqueuedClusters";
  case HierarchyDebugReason::CulledParentErrorTooSmall:
    return "CulledParentErrorTooSmall";
  case HierarchyDebugReason::CulledFrustum:
    return "CulledFrustum";
  case HierarchyDebugReason::CulledOcclusion:
    return "CulledOcclusion";
  case HierarchyDebugReason::CulledNotInstalled:
    return "CulledNotInstalled";
  case HierarchyDebugReason::CulledInvalidChildStart:
    return "CulledInvalidChildStart";
  case HierarchyDebugReason::CulledProjectedTooSmall:
    return "CulledProjectedTooSmall";
  }
  return "UnknownHierarchyReason";
}

inline const char *clusterDebugReasonToString(ClusterDebugReason reason)
{
  switch (reason)
  {
  case ClusterDebugReason::NotVisited:
    return "NotVisited";
  case ClusterDebugReason::RenderedHardware:
    return "RenderedHardware";
  case ClusterDebugReason::RenderedSoftware:
    return "RenderedSoftware";
  case ClusterDebugReason::InvalidQueueElement:
    return "InvalidQueueElement";
  case ClusterDebugReason::InvalidPageIndex:
    return "InvalidPageIndex";
  case ClusterDebugReason::PageNotInstalled:
    return "PageNotInstalled";
  case ClusterDebugReason::ZeroTriangleCount:
    return "ZeroTriangleCount";
  case ClusterDebugReason::LocalBitOutOfRange:
    return "LocalBitOutOfRange";
  case ClusterDebugReason::DisabledByCutMask:
    return "DisabledByCutMask";
  case ClusterDebugReason::SelfErrorTooHigh:
    return "SelfErrorTooHigh";
  case ClusterDebugReason::ParentErrorTooLow:
    return "ParentErrorTooLow";
  case ClusterDebugReason::ConeCulled:
    return "ConeCulled";
  case ClusterDebugReason::ProjectedTooSmall:
    return "ProjectedTooSmall";
  }
  return "UnknownClusterReason";
}

static constexpr uint32_t DEBUG_MAX_CLUSTERS_PER_PAGE = 512u;
static constexpr uint32_t SHADOW_HW_VISIBLE_CLUSTER_COUNT_INDEX = 5u;

class VirtualGeometryCullingMultipleDispatchesPass : public Pass
{
  rendering::Shader cullingShader;

  rendering::ComputePipeline initSyncPipeline;
  rendering::ComputePipeline setupRootNodesPipeline;
  rendering::ComputePipeline prepareIndirectDispatchPipeline;
  rendering::ComputePipeline processHierarchyNodesPipeline;
  rendering::ComputePipeline prepareClusterDispatchPipeline;
  rendering::ComputePipeline processClustersPipeline;
  // Only valid when DeviceFeatures_DrawIndirectCount is NOT supported.
  rendering::ComputePipeline prepareHWDrawIndirectArgsPipeline;
  rendering::BindingsLayout cullingLayout;

  rendering::BindingGroups bindingGroupPingA;
  rendering::BindingGroups bindingGroupPingB;

  rendering::Buffer cullingStatisticsBuffer;
  rendering::Buffer hierarchyQueueA;
  rendering::Buffer hierarchyQueueB;
  rendering::Buffer clusterQueueBuffer;
  rendering::Buffer indirectArgsBuffer;
  rendering::Buffer uniformsBuffer;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;

  rendering::Buffer hwDrawIndirectBuffer;
  rendering::Buffer hwVisibleClusterInfosBuffer;

  rendering::Buffer swDrawIndirectBuffer;
  rendering::Buffer swVisibleClusterInfosBuffer;

#ifdef DEBUG_BINDINGS
  rendering::Buffer hierarchyDebugBuffer;
  rendering::Buffer clusterDebugBuffer;
  uint32_t hierarchyDebugRecordCount = 0u;
  uint32_t clusterDebugRecordCount = 0u;
  uint64_t hierarchyDebugBufferSize = 0u;
  uint64_t clusterDebugBufferSize = 0u;
#endif
  rendering::Texture hierarchicalZBufferPiramidTexture;
  rendering::Sampler hierarchicalZBufferPiramidTextureSampler;
  rendering::Texture inputHiZTexture;
  bool ownsHiZTexture = true;

  VirtualGeometryScene &scene;

  static constexpr uint32_t QUEUE_ELEMENT_SIZE = sizeof(uint32_t) * 4;
  static constexpr uint32_t WORKGROUP_SIZE = 64u;
  static constexpr uint32_t COUNTER_COUNT = 16u;

public:
  struct Settings
  {
    uint32_t maxHierarchyLevels = 8;
    uint32_t clustersQueueBufferSize = 1024 * 1024 * 16;
    float errorTreshhold = 1.0f;
    uint32_t viewPortWidth = 1920;
    uint32_t viewPortHeight = 1080;
    uint32_t hiZMipLevels = 0; // 0 => auto from viewport
    bool useExternalHiZTexture = false;
    bool enableFrustumCulling = true;
    bool enableOcclusionCulling = true;
    bool enableStreamingPriorityUpdates = true;
    float swPixelsPerTriangleThreshold = 10.0f;
  };

  Settings settings;

  struct CullingUniforms
  {
    static constexpr uint32_t FLAG_FRUSTUM = 1u << 0u;
    static constexpr uint32_t FLAG_OCCLUSION = 1u << 1u;
    static constexpr uint32_t FLAG_STREAMING_PRIORITIES = 1u << 2u;

    float view[16];
    float proj[16];
    float viewPosition[4];
    uint32_t viewport[2];
    float error;
    uint32_t instances_count;
    uint32_t clusters_count;
    float nearPlane;
    float farPlane;
    uint32_t hiZLevels;
    uint32_t cullingFlags;
    uint32_t registeredPages;
    uint32_t streamingSelectionCount;
    uint32_t _padding[2];
  };

  uint64_t hwDrawIndirectBufferSize;
  uint64_t hwVisibleClusterInfosBufferSize;
  uint64_t swDrawIndirectBufferSize;
  uint64_t swVisibleClusterInfosBufferSize;

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

  VirtualGeometryCullingMultipleDispatchesPass(VirtualGeometryScene &scene, Settings settings, rendering::Texture hiZTexture) : scene(scene), settings(settings), inputHiZTexture(hiZTexture)
  {
  }

  ~VirtualGeometryCullingMultipleDispatchesPass()
  {
    destroyFrameOverrideBuffers();
    renderGraph->deleteComputePipeline(initSyncPipeline);
    renderGraph->deleteComputePipeline(setupRootNodesPipeline);
    renderGraph->deleteComputePipeline(prepareIndirectDispatchPipeline);
    renderGraph->deleteComputePipeline(processHierarchyNodesPipeline);
    renderGraph->deleteComputePipeline(prepareClusterDispatchPipeline);
    renderGraph->deleteComputePipeline(processClustersPipeline);
    if (prepareHWDrawIndirectArgsPipeline.name.size())
      renderGraph->deleteComputePipeline(prepareHWDrawIndirectArgsPipeline);
    renderGraph->deleteBindingGroups(bindingGroupPingA);
    renderGraph->deleteBindingGroups(bindingGroupPingB);
    renderGraph->deleteBindingsLayout(cullingLayout);
    renderGraph->deleteShader(cullingShader);

    renderGraph->deleteBuffer(cullingStatisticsBuffer);
    renderGraph->deleteBuffer(uniformsBuffer);
    renderGraph->deleteBuffer(hierarchyQueueA);
    renderGraph->deleteBuffer(hierarchyQueueB);
    renderGraph->deleteBuffer(clusterQueueBuffer);
    renderGraph->deleteBuffer(indirectArgsBuffer);

    renderGraph->deleteBuffer(hwDrawIndirectBuffer);
    renderGraph->deleteBuffer(hwVisibleClusterInfosBuffer);
    renderGraph->deleteBuffer(swDrawIndirectBuffer);
    renderGraph->deleteBuffer(swVisibleClusterInfosBuffer);
#ifdef DEBUG_BINDINGS
    renderGraph->deleteBuffer(hierarchyDebugBuffer);
    renderGraph->deleteBuffer(clusterDebugBuffer);
#endif
    if (ownsHiZTexture)
    {
      renderGraph->deleteTexture(hierarchicalZBufferPiramidTexture);
    }
    renderGraph->deleteSampler(hierarchicalZBufferPiramidTextureSampler);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    cullingStatisticsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Counters.buffer",
          .size = COUNTER_COUNT * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    uniformsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(CullingUniforms),
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    const uint64_t hierarchyQueueBytes = 1024u * 1024u * QUEUE_ELEMENT_SIZE;

    hierarchyQueueA = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_HierarchyQueueA.buffer",
          .size = hierarchyQueueBytes,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    hierarchyQueueB = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_HierarchyQueueB.buffer",
          .size = hierarchyQueueBytes,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    const uint64_t clusterQueueBytes = 1024u * 1024u * QUEUE_ELEMENT_SIZE;

    clusterQueueBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_ClusterQueue.buffer",
          .size = clusterQueueBytes,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    indirectArgsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_IndirectArgs.buffer",
          .size = sizeof(uint32_t) * 3,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });

    hwDrawIndirectBufferSize = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * 4u * sizeof(uint32_t);
    hwDrawIndirectBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_HWDrawIndirect.buffer",
          .size = hwDrawIndirectBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect | BufferUsage::BufferUsage_CopySrc,
        });

    hwVisibleClusterInfosBufferSize = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
    hwVisibleClusterInfosBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_HWVisibleClusterInfos.buffer",
          .size = hwVisibleClusterInfosBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    swDrawIndirectBufferSize = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * 4u * sizeof(uint32_t);
    swDrawIndirectBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_SWDrawIndirect.buffer",
          .size = swDrawIndirectBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect | BufferUsage::BufferUsage_CopySrc,
        });

    swVisibleClusterInfosBufferSize = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
    swVisibleClusterInfosBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_SWVisibleClusterInfos.buffer",
          .size = swVisibleClusterInfosBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

#ifdef DEBUG_BINDINGS
    hierarchyDebugRecordCount = static_cast<uint32_t>(scene.hierarchyBufferSize / sizeof(VirtualGeometryHierarchy));
    const uint32_t registeredDebugPages = std::max(1u, scene.nextPageTableSlot);
    clusterDebugRecordCount = registeredDebugPages * DEBUG_MAX_CLUSTERS_PER_PAGE;

    hierarchyDebugBufferSize = static_cast<uint64_t>(hierarchyDebugRecordCount) * sizeof(HierarchyDebugRecord);
    clusterDebugBufferSize = static_cast<uint64_t>(clusterDebugRecordCount) * sizeof(ClusterDebugRecord);

    hierarchyDebugBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_HierarchyDebug.buffer",
          .size = hierarchyDebugBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });

    clusterDebugBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_ClusterDebug.buffer",
          .size = clusterDebugBufferSize,
          .scratch = false,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
        });
#endif

    const uint32_t viewportMipLevels = computeMipCount(settings.viewPortWidth, settings.viewPortHeight);
    const uint32_t mipLevels = (settings.hiZMipLevels == 0u) ? viewportMipLevels : settings.hiZMipLevels;

    if (settings.useExternalHiZTexture && !inputHiZTexture.name.empty())
    {
      hierarchicalZBufferPiramidTexture = inputHiZTexture;
      ownsHiZTexture = false;
    }
    else
    {
      hierarchicalZBufferPiramidTexture = createFrameLocalTexture(
          TextureInfo{
            .name = passName + "_HiZPyramid.texture",
            .format = rendering::Format::Format_R32Float,
            .height = settings.viewPortHeight,
            .width = settings.viewPortWidth,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .mipLevels = mipLevels,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          });
      ownsHiZTexture = true;
    }

    hierarchicalZBufferPiramidTextureSampler = renderGraph->createSampler(
        SamplerInfo{
          .name = passName + "_HiZPyramidSampler.sampler",
          .addressModeU = rendering::SamplerAddressMode::ClampToEdge,
          .addressModeV = rendering::SamplerAddressMode::ClampToEdge,
          .addressModeW = rendering::SamplerAddressMode::ClampToEdge,
          .anisotropyEnable = false,
          .maxAnisotropy = 1.0f,
          .magFilter = rendering::Filter::Nearest,
          .maxLod = static_cast<float>(mipLevels),
          .minFilter = rendering::Filter::Nearest,
        });

    const bool drawIndirectCountDisabled = !(renderGraph->getRHI()->getFeatures() & DeviceFeatures::DeviceFeatures_DrawIndirectCount);
    const char *cullingSpirv = drawIndirectCountDisabled ? "assets/shaders/spirv/virtualgeometry-culling-multipass-draw_indirect_count_disabled.spirv" : "assets/shaders/spirv/virtualgeometry-culling-multipass.spirv";
    auto shaderSrc = os::io::readRelativeFile(cullingSpirv);

    cullingLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_cullingLayout.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "instances", .binding = 1, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "hierarchy", .binding = 2, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "counters", .binding = 3, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "hierarchyQueueRead", .binding = 4, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "hierarchyQueueWrite", .binding = 5, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "clusterQueue", .binding = 6, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "meshPartTransforms", .binding = 7, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "indirectArgs", .binding = 8, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "pageTable", .binding = 9, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "pagePriorities", .binding = 10, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "hwDrawIndirectBuffer", .binding = 13, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "hwVisibleClusterInfos", .binding = 14, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "pagesBuffer", .binding = 15, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "pageInstallCandidates", .binding = 20, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "pageEvictCandidates", .binding = 21, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "swDrawIndirectBuffer", .binding = 16, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "swVisibleClusterInfos", .binding = 17, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
#ifdef DEBUG_BINDINGS
                        {.name = "hierarchyDebugBuffer", .binding = 18, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
                        {.name = "clusterDebugBuffer", .binding = 19, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute},
#endif
                      },
                  .textures =
                      {
                        {.name = "hiZTexture", .binding = 11, .visibility = BindingVisibility::BindingVisibility_Compute},
                      },
                  .samplers =
                      {
                        {.name = "hiZSampler", .binding = 12, .visibility = BindingVisibility::BindingVisibility_Compute},
                      },
                },
              },
        });

    cullingShader = renderGraph->createShader(
        ShaderInfo{
          .name = passName + "_cullingShader.shader",
          .layout = cullingLayout,
          .src = shaderSrc,
          .type = ShaderType::SpirV,
        });

    initSyncPipeline = renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "initSync", .layout = cullingLayout, .name = passName + "_initSync.pipeline", .shader = cullingShader});
    setupRootNodesPipeline = renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "setupRootNodes", .layout = cullingLayout, .name = passName + "_setupRootNodes.pipeline", .shader = cullingShader});
    prepareIndirectDispatchPipeline =
        renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "prepareIndirectDispatch", .layout = cullingLayout, .name = passName + "_prepareIndirectDispatch.pipeline", .shader = cullingShader});
    processHierarchyNodesPipeline =
        renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "processHierarchyNodes", .layout = cullingLayout, .name = passName + "_processHierarchyNodes.pipeline", .shader = cullingShader});
    prepareClusterDispatchPipeline =
        renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "prepareClusterDispatch", .layout = cullingLayout, .name = passName + "_prepareClusterDispatch.pipeline", .shader = cullingShader});
    processClustersPipeline = renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "processClusters", .layout = cullingLayout, .name = passName + "_processClusters.pipeline", .shader = cullingShader});
    if (drawIndirectCountDisabled)
    {
      prepareHWDrawIndirectArgsPipeline =
          renderGraph->createComputePipeline(ComputePipelineInfo{.entry = "prepareHWDrawIndirectArgs", .layout = cullingLayout, .name = passName + "_prepareHWDrawIndirectArgs.pipeline", .shader = cullingShader});
    }
    auto makeBindingGroup = [&](const std::string &name, const rendering::Buffer &queueRead, const rendering::Buffer &queueWrite) -> rendering::BindingGroups
    {
      const uint64_t instanceBufferSize_ = scene.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData);
      const uint64_t hierarchyQueueBytes_ = 1024u * 1024u * QUEUE_ELEMENT_SIZE;
      const uint64_t clusterQueueBytes_ = 1024u * 1024u * QUEUE_ELEMENT_SIZE;

      return renderGraph->createBindingGroups(
          BindingGroupsInfo{
            .layout = cullingLayout,
            .name = passName + "_" + name,
            .groups =
                {
                  GroupInfo{
                    .name = "Group0",
                    .buffers =
                        {
                          {.binding = 0, .bufferView = {.buffer = uniformsBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(CullingUniforms)}},
                          {.binding = 1, .bufferView = {.buffer = scene.instanceBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = instanceBufferSize_}},
                          {.binding = 2, .bufferView = {.buffer = scene.hierarchyBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.hierarchyBufferSize}},
                          {.binding = 3,
                           .bufferView =
                               {.buffer = cullingStatisticsBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = COUNTER_COUNT * sizeof(uint32_t)}},
                          {.binding = 4,
                           .bufferView = {.buffer = queueRead, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = hierarchyQueueBytes_}},
                          {.binding = 5,
                           .bufferView = {.buffer = queueWrite, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = hierarchyQueueBytes_}},
                          {.binding = 6,
                           .bufferView =
                               {.buffer = clusterQueueBuffer, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = clusterQueueBytes_}},
                          {.binding = 7,
                           .bufferView =
                               {.buffer = scene.meshPartTransformsBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.meshPartTransformsBufferSize}},
                          {.binding = 8,
                           .bufferView =
                               {.buffer = indirectArgsBuffer, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = sizeof(uint32_t) * 3}},
                          {.binding = 9, .bufferView = {.buffer = scene.pageTableBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.pagesTableBufferSize}},
                          {.binding = 10,
                           .bufferView =
                               {.buffer = scene.pagePriorityBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = scene.pagePriorityBufferSize}},
                          {.binding = 13,
                           .bufferView =
                               {.buffer = hwDrawIndirectBuffer, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = hwDrawIndirectBufferSize}},
                          {.binding = 14,
                           .bufferView =
                               {.buffer = hwVisibleClusterInfosBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = hwVisibleClusterInfosBufferSize}},
                          {.binding = 15, .bufferView = {.buffer = scene.pagesBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.pagesBufferSize}},
                          {.binding = 20,
                           .bufferView =
                               {.buffer = scene.pageInstallCandidateBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = scene.pageInstallCandidateBufferSize}},
                          {.binding = 21,
                           .bufferView =
                               {.buffer = scene.pageEvictCandidateBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = scene.pageEvictCandidateBufferSize}},
                          {.binding = 16,
                           .bufferView =
                               {.buffer = swDrawIndirectBuffer, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = swDrawIndirectBufferSize}},
                          {.binding = 17,
                           .bufferView =
                               {.buffer = swVisibleClusterInfosBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = swVisibleClusterInfosBufferSize}},
#ifdef DEBUG_BINDINGS
                          {.binding = 18,
                           .bufferView =
                               {.buffer = hierarchyDebugBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = hierarchyDebugBufferSize}},
                          {.binding = 19,
                           .bufferView =
                               {.buffer = clusterDebugBuffer,
                                .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE,
                                .offset = 0,
                                .size = clusterDebugBufferSize}},
#endif
                        },
                    .textures =
                        {
                          {.binding = 11,
                           .textureView =
                               {.texture = hierarchicalZBufferPiramidTexture,
                                .access = AccessPattern::SHADER_READ,
                                .layout = ResourceLayout::SHADER_READ_ONLY,
                                .flags = rendering::ImageAspectFlags::Color,
                                .baseMipLevel = 0,
                                .levelCount = mipLevels,
                                .baseArrayLayer = 0,
                                .layerCount = 1}},
                        },
                    .samplers =
                        {
                          {.binding = 12,
                           .sampler = hierarchicalZBufferPiramidTextureSampler,
                           .view =
                               TextureView{
                                 .texture = hierarchicalZBufferPiramidTexture,
                                 .access = rendering::AccessPattern::SHADER_READ,
                                 .baseArrayLayer = 0,
                                 .baseMipLevel = 0,
                                 .layerCount = 1,
                                 .levelCount = mipLevels,
                                 .flags = rendering::ImageAspectFlags::Color,
                                 .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
                               }},
                        },
                  },
                },
          });
    };

    bindingGroupPingA = makeBindingGroup("bindingGroupPingA", hierarchyQueueA, hierarchyQueueB);
    bindingGroupPingB = makeBindingGroup("bindingGroupPingB", hierarchyQueueB, hierarchyQueueA);

    const uint32_t instanceCount = static_cast<uint32_t>(scene.getInstanceCount());
    const uint32_t setupWorkgroups = (instanceCount + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;

    commandBuffer.cmdBindComputePipeline(initSyncPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroupPingA, nullptr, 0);
    commandBuffer.cmdDispatch(1, 1, 1);

    commandBuffer.cmdBindComputePipeline(setupRootNodesPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroupPingA, nullptr, 0);
    commandBuffer.cmdDispatch(setupWorkgroups, 1, 1);

    for (uint32_t i = 0; i < settings.maxHierarchyLevels; ++i)
    {
      const bool usePingB = (i % 2 == 0);
      auto &currentGroup = usePingB ? bindingGroupPingB : bindingGroupPingA;

      commandBuffer.cmdBindComputePipeline(prepareIndirectDispatchPipeline);
      commandBuffer.cmdBindBindingGroups(currentGroup, nullptr, 0);
      commandBuffer.cmdDispatch(1, 1, 1);

      commandBuffer.cmdBindComputePipeline(processHierarchyNodesPipeline);
      commandBuffer.cmdBindBindingGroups(currentGroup, nullptr, 0);
      commandBuffer.cmdDispatchIndirect(indirectArgsBuffer, 0);
    }

    commandBuffer.cmdBindComputePipeline(prepareClusterDispatchPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroupPingA, nullptr, 0);
    commandBuffer.cmdDispatch(1, 1, 1);

    commandBuffer.cmdBindComputePipeline(processClustersPipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroupPingA, nullptr, 0);
    commandBuffer.cmdDispatchIndirect(indirectArgsBuffer, 0);

    if (drawIndirectCountDisabled)
    {
      // Write the single VkDrawIndirectCommand from the accumulated cluster count.
      commandBuffer.cmdBindComputePipeline(prepareHWDrawIndirectArgsPipeline);
      commandBuffer.cmdBindBindingGroups(bindingGroupPingA, nullptr, 0);
      commandBuffer.cmdDispatch(1, 1, 1);
    }
  }

  void updateUniforms(const float view[16], const float proj[16], const float viewPosition[4], uint32_t viewportW, uint32_t viewportH, float nearPlane, float farPlane, uint32_t hiZLevels)
  {
    CullingUniforms u{};
    std::memcpy(u.view, view, sizeof(float) * 16);
    std::memcpy(u.proj, proj, sizeof(float) * 16);
    std::memcpy(u.viewPosition, viewPosition, sizeof(float) * 4);
    u.viewport[0] = viewportW;
    u.viewport[1] = viewportH;
    u.error = settings.errorTreshhold;
    u.instances_count = static_cast<uint32_t>(scene.getInstanceCount());
    u.clusters_count = 0;
    u.nearPlane = nearPlane;
    u.farPlane = farPlane;
    u.hiZLevels = hiZLevels;
    u.cullingFlags = 0u;
    u.registeredPages = scene.nextPageTableSlot;
    u.streamingSelectionCount = VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT;
    if (settings.enableFrustumCulling)
      u.cullingFlags |= CullingUniforms::FLAG_FRUSTUM;
    if (settings.enableOcclusionCulling)
      u.cullingFlags |= CullingUniforms::FLAG_OCCLUSION;
    if (settings.enableStreamingPriorityUpdates)
      u.cullingFlags |= CullingUniforms::FLAG_STREAMING_PRIORITIES;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformsBuffer, 0, sizeof(CullingUniforms), &u);
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(getCurrentFrameOverrideUniformBufferId(), 0, sizeof(CullingUniforms), &u);
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return;

    overrides.bufferOverrides.emplace(uniformsBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  const rendering::Buffer &getHWDrawIndirectBuffer() const
  {
    return hwDrawIndirectBuffer;
  }
  const rendering::Buffer &getHWVisibleClusterInfosBuffer() const
  {
    return hwVisibleClusterInfosBuffer;
  }
  const rendering::Buffer &getSWDrawIndirectBuffer() const
  {
    return swDrawIndirectBuffer;
  }
  const rendering::Buffer &getSWVisibleClusterInfosBuffer() const
  {
    return swVisibleClusterInfosBuffer;
  }
  const rendering::Buffer &getCullingStatisticsBuffer() const
  {
    return cullingStatisticsBuffer;
  }
  const rendering::Buffer &getClusterQueueBuffer() const
  {
    return clusterQueueBuffer;
  }
  const rendering::Buffer &getHierarchyQueueA() const
  {
    return hierarchyQueueA;
  }
  const rendering::Buffer &getHierarchyQueueB() const
  {
    return hierarchyQueueB;
  }
  const rendering::Texture &getHiZTexture() const
  {
    return hierarchicalZBufferPiramidTexture;
  }
#ifdef DEBUG_BINDINGS
  const rendering::Buffer &getHierarchyDebugBuffer() const
  {
    return hierarchyDebugBuffer;
  }
  const rendering::Buffer &getClusterDebugBuffer() const
  {
    return clusterDebugBuffer;
  }
  uint64_t getHierarchyDebugBufferSize() const
  {
    return hierarchyDebugBufferSize;
  }
  uint64_t getClusterDebugBufferSize() const
  {
    return clusterDebugBufferSize;
  }
  uint32_t getHierarchyDebugRecordCount() const
  {
    return hierarchyDebugRecordCount;
  }
  uint32_t getClusterDebugRecordCount() const
  {
    return clusterDebugRecordCount;
  }
#endif
  const rendering::Buffer &getLastWrittenHierarchyQueue() const
  {
    const uint32_t n = settings.maxHierarchyLevels;
    return (n == 0u || n % 2u == 0u) ? hierarchyQueueB : hierarchyQueueA;
  }

  const uint64_t getHierarchyQueueCapacityBytes() const
  {
    return 1024u * 1024u * QUEUE_ELEMENT_SIZE;
  }

private:
  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          BufferInfo{
              .name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot),
              .size = sizeof(CullingUniforms),
              .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
          });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;

    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  rendering::BufferId getCurrentFrameOverrideUniformBufferId() const
  {
    return getFrameOverrideUniformBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }
};

} // namespace gpgpu
} // namespace virtualgeometry
