#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualShadowMapDrawPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t maxHierarchyLevels = 8u;
    uint32_t hierarchyQueueCapacity = 1024u * 1024u;
    uint32_t clusterQueueCapacity = 1024u * 1024u;
    uint32_t maxVisibleClusterDraws = 131072u;
    uint32_t scratchClipmapCount = 2u;
    float lodErrorThreshold = 1.0f;
    bool enableDirtyPageStencil = true;
  };

  VirtualShadowMapDrawPass(VirtualGeometryScene &scene, VirtualShadowMapManager &manager, Settings settings)
      : scene_(scene), manager_(manager), settings_(settings)
  {
  }

  ~VirtualShadowMapDrawPass() override
  {
    renderGraph->deleteComputePipeline(buildHPBPipeline_);
    renderGraph->deleteShader(hpbShader_);
    renderGraph->deleteBindingGroups(hpbBaseBindingGroups_);
    renderGraph->deleteBuffer(hpbUniformBuffer_);
    renderGraph->deleteBindingsLayout(hpbLayout_);

    renderGraph->deleteComputePipeline(initCullingPipeline_);
    renderGraph->deleteComputePipeline(setupRootNodesPipeline_);
    renderGraph->deleteComputePipeline(prepareIndirectDispatchPipeline_);
    renderGraph->deleteComputePipeline(processHierarchyNodesPipeline_);
    renderGraph->deleteComputePipeline(prepareClusterDispatchPipeline_);
    renderGraph->deleteComputePipeline(processClustersPipeline_);
    renderGraph->deleteShader(cullingShader_);
    renderGraph->deleteBindingGroups(cullingBindingGroupPingA_);
    renderGraph->deleteBindingGroups(cullingBindingGroupPingB_);
    renderGraph->deleteBindingsLayout(cullingLayout_);
    renderGraph->deleteBuffer(cullingUniformBuffer_);
    renderGraph->deleteBuffer(cullingStatisticsBuffer_);
    renderGraph->deleteBuffer(hierarchyQueueA_);
    renderGraph->deleteBuffer(hierarchyQueueB_);
    renderGraph->deleteBuffer(clusterQueueBuffer_);
    renderGraph->deleteBuffer(indirectArgsBuffer_);
    renderGraph->deleteBuffer(shadowDrawIndirectBuffer_);
    renderGraph->deleteBuffer(shadowVisibleClusterInfosBuffer_);
    renderGraph->deleteBuffer(processedPagesBuffer_);
    renderGraph->deleteBuffer(pageClusterCountsBuffer_);
    renderGraph->deleteBuffer(layerVisibleCountsBuffer_);
    renderGraph->deleteBuffer(dirtyPageCountsBuffer_);
    renderGraph->deleteBuffer(dirtyPageListBuffer_);

    renderGraph->deleteGraphicsPipeline(clearScratchPipeline_);
    renderGraph->deleteGraphicsPipeline(drawScratchStencilPipeline_);
    renderGraph->deleteGraphicsPipeline(pageStencilMarkPipeline_);
    renderGraph->deleteGraphicsPipeline(pageStencilResetPipeline_);
    renderGraph->deleteShader(drawScratchVertexShader_);
    renderGraph->deleteShader(pageStencilVertexShader_);
    renderGraph->deleteBindingGroups(drawScratchBindingGroups_);
    renderGraph->deleteBindingsLayout(drawScratchLayout_);
    renderGraph->deleteBuffer(drawScratchUniformBuffer_);
    renderGraph->deleteBindingGroups(pageStencilBindingGroups_);
    renderGraph->deleteBindingsLayout(pageStencilLayout_);
    renderGraph->deleteBuffer(pageStencilUniformBuffer_);
    renderGraph->deleteBuffer(pageClearDrawIndirectBuffer_);
    renderGraph->deleteBuffer(pageStencilDrawIndirectBuffer_);

    renderGraph->deleteComputePipeline(resolvePagesPipeline_);
    renderGraph->deleteComputePipeline(prepareResolveDispatchPipeline_);
    renderGraph->deleteShader(resolvePagesShader_);
    for (const rendering::BindingGroups &bindingGroups : resolvePagesBindingGroups_)
    {
      renderGraph->deleteBindingGroups(bindingGroups);
    }
    renderGraph->deleteBindingsLayout(resolvePagesLayout_);
    renderGraph->deleteBuffer(resolvePagesUniformBuffer_);
    renderGraph->deleteBuffer(resolveDispatchArgsBuffer_);
    renderGraph->deleteBuffer(pageOpDispatchArgsBuffer_);

    renderGraph->deleteComputePipeline(finishPagesPipeline_);
    renderGraph->deleteShader(finishPagesShader_);
    renderGraph->deleteBindingGroups(finishPagesBindingGroups_);
    renderGraph->deleteBindingsLayout(finishPagesLayout_);
    renderGraph->deleteBuffer(finishPagesUniformBuffer_);

    for (const rendering::Texture &scratchTexture : scratchClipmapTextures_)
    {
      renderGraph->deleteTexture(scratchTexture);
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createHPBComputeResources();
    createCullingResources();
    createScratchResources();
    createPageStencilResources();
    createScratchDrawResources();
    createResolveResources();
    createFinishResources();

    recordHPBPasses(commandBuffer);
    recordCullingPasses(commandBuffer);
    recordScratchPasses(commandBuffer);
    recordFinishPass(commandBuffer);
  }

  const rendering::Buffer &getCullingStatisticsBuffer() const
  {
    return cullingStatisticsBuffer_;
  }

  const rendering::Buffer &getShadowDrawIndirectBuffer() const
  {
    return shadowDrawIndirectBuffer_;
  }

  const rendering::Buffer &getShadowVisibleClusterInfosBuffer() const
  {
    return shadowVisibleClusterInfosBuffer_;
  }

  const rendering::Buffer &getPageClusterCountsBuffer() const
  {
    return pageClusterCountsBuffer_;
  }

  uint32_t getVisibleClusterDrawCapacity() const
  {
    return visibleClusterDrawCapacity_;
  }

  void setLodErrorThreshold(float lodErrorThreshold)
  {
    settings_.lodErrorThreshold = lodErrorThreshold;
  }

  void setDirtyPageStencilEnabled(bool enabled)
  {
    settings_.enableDirtyPageStencil = enabled;
  }

private:
  static constexpr uint32_t kQueueElementSize = sizeof(uint32_t) * 4u;
  static constexpr uint32_t kWorkgroupSize = 64u;
  static constexpr uint32_t kHierarchyPullPerWorkgroup = 64u;
  static constexpr uint32_t kCullingCounterCount = 8u;
  static constexpr uint32_t kMaxHPBMipLevels = 6u;
  static constexpr uint32_t kResolveWorkgroupSize = 64u;

  static constexpr uint32_t kHierarchyQueueSizeIndex = 0u;
  static constexpr uint32_t kClusterQueueSizeIndex = 1u;
  static constexpr uint32_t kReadQueueSizeIndex = 2u;
  static constexpr uint32_t kShadowVisibleClusterCountIndex = 3u;
  static constexpr uint32_t kShadowDrawOverflowCountIndex = 4u;

  struct HPBUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t activeLayers = 0u;
    uint32_t hpbMipCount = 0u;
    uint32_t requestCapacity = 0u;
    uint32_t futureRequestCapacity = 0u;
    uint32_t _padding0[3] = {0u, 0u, 0u};
  };

  struct CullingUniforms
  {
    uint32_t instancesCount = 0u;
    uint32_t activeLayers = 0u;
    uint32_t maxVisibleClusterDrawsPerLayer = 0u;
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t hpbMipCount = 0u;
    uint32_t maxScenePages = 0u;
    float lodErrorThreshold = 1.0f;
    uint32_t maxHierarchyNodes = 0u;
  };

  struct ScratchDrawUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t activeLayers = 0u;
    uint32_t currentLayer = 0u;
    uint32_t scratchResolution = 0u;
    uint32_t _padding0[3] = {0u, 0u, 0u};
  };

  struct StencilPageUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t currentLayer = 0u;
    uint32_t scratchResolution = 0u;
    float depthValue = 1.0f;
    float _padding0[3] = {0.0f, 0.0f, 0.0f};
  };

  struct ResolveUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t currentLayer = 0u;
    uint32_t activeLayers = 0u;
    uint32_t enableDirtyPageStencil = 1u;
    uint32_t _padding0[3] = {0u, 0u, 0u};
  };

  struct FinishUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t activeLayers = 0u;
    uint32_t currentLayer = 0u;
    uint32_t _padding0 = 0u;
  };

  uint32_t computeVisibleClusterDrawCapacity() const
  {
    const uint64_t clusterCount = std::max<uint64_t>(1u, scene_.nextClusterOffset);
    const uint64_t layerCount = std::max<uint64_t>(1u, manager_.getActiveLayerCount());
    const uint64_t recommended = clusterCount * layerCount * 4u;
    const uint64_t minimum = std::max<uint64_t>(1u, settings_.maxVisibleClusterDraws);
    return static_cast<uint32_t>(std::min<uint64_t>(std::max(recommended, minimum), std::numeric_limits<uint32_t>::max()));
  }

  uint32_t getScratchClipmapCount() const
  {
    return std::max(1u, settings_.scratchClipmapCount);
  }

  uint32_t getScratchResolution() const
  {
    return manager_.getVirtualPageTableResolution() * manager_.getPhysicalPageResolution();
  }

  uint32_t getAlignedUniformSize(uint32_t uniformSize) const
  {
    const uint32_t alignment = std::max<uint32_t>(1u, static_cast<uint32_t>(renderGraph->getRHI()->GetProperties().uniformBufferAlignment));
    return ((uniformSize + alignment - 1u) / alignment) * alignment;
  }

  static rendering::TextureView makeHPBSampledView(rendering::Texture texture, uint32_t baseMip, uint32_t mipCount, uint32_t layerCount)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = baseMip,
      .layerCount = layerCount,
      .levelCount = mipCount,
    };
  }

  static rendering::TextureView makeHPBStorageView(rendering::Texture texture, uint32_t baseMip, uint32_t layerCount)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = baseMip,
      .layerCount = layerCount,
      .levelCount = 1,
    };
  }

  static rendering::TextureView makeScratchSampledView(rendering::Texture texture)
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

  static rendering::TextureView makeScratchDepthStencilAttachmentView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0,
      .flags = static_cast<rendering::ImageAspectFlags>(
          static_cast<uint32_t>(rendering::ImageAspectFlags::Depth) |
          static_cast<uint32_t>(rendering::ImageAspectFlags::Stencil)),
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  static rendering::TextureView makeAtlasStorageView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  void createHPBComputeResources()
  {
    hpbUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_HPBUniforms.buffer",
            .size = sizeof(HPBUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    const HPBUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .hpbMipCount = std::min(manager_.getHierarchicalPageBoundsMipCount(), kMaxHPBMipLevels),
      .requestCapacity = manager_.getAllocationRequestCapacity(),
      .futureRequestCapacity = manager_.getFutureAllocationRequestCapacity(),
    };
    renderGraph->bufferWrite(hpbUniformBuffer_, 0, sizeof(uniforms), const_cast<HPBUniforms *>(&uniforms));

    rendering::BindingGroupLayout groupLayout{};
    groupLayout.buffers = {
      {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
      {.name = "allocatorCounters", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
      {.name = "allocationRequests", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
      {.name = "futureAllocationRequests", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
    };
    groupLayout.storageTextures.reserve(kMaxHPBMipLevels);
    for (uint32_t mipIndex = 0u; mipIndex < kMaxHPBMipLevels; ++mipIndex)
    {
      groupLayout.storageTextures.push_back({
          .name = "hpbMip" + std::to_string(mipIndex),
          .binding = 4u + mipIndex,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
      });
    }

    hpbLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_HPBLayout",
            .groups = {groupLayout},
        });

    const uint32_t layerCount = manager_.getMaxLayerCount();
    rendering::GroupInfo hpbGroupInfo{};
    hpbGroupInfo.name = "group0";
    hpbGroupInfo.buffers = {
      {.binding = 0, .bufferView = {.buffer = hpbUniformBuffer_, .offset = 0, .size = sizeof(HPBUniforms), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 1, .bufferView = {.buffer = manager_.getAllocatorCountersBuffer(), .offset = 0, .size = manager_.getAllocatorCountersBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 2, .bufferView = {.buffer = manager_.getAllocationRequestsBuffer(), .offset = 0, .size = manager_.getAllocationRequestsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 3, .bufferView = {.buffer = manager_.getFutureAllocationRequestsBuffer(), .offset = 0, .size = manager_.getFutureAllocationRequestsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
    };
    hpbGroupInfo.storageTextures.reserve(kMaxHPBMipLevels);
    const uint32_t fallbackMip = std::max(0u, uniforms.hpbMipCount - 1u);
    for (uint32_t mipIndex = 0u; mipIndex < kMaxHPBMipLevels; ++mipIndex)
    {
      const uint32_t boundMip = std::min(mipIndex, fallbackMip);
      hpbGroupInfo.storageTextures.push_back({
          .binding = 4u + mipIndex,
          .textureView = makeHPBStorageView(manager_.getHierarchicalPageBoundsTexture(), boundMip, layerCount),
      });
    }

    hpbBaseBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = hpbLayout_,
            .name = passName + "_HPBBaseBindingGroups",
            .groups = {hpbGroupInfo},
        });

    hpbShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_HPB.shader",
            .layout = hpbLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-draw.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    buildHPBPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "build_hpb_all_main",
            .layout = hpbLayout_,
            .name = passName + "_BuildHPB.pipeline",
            .shader = hpbShader_,
        });
  }

  void createCullingResources()
  {
    visibleClusterDrawCapacity_ = computeVisibleClusterDrawCapacity();
    const uint32_t activeLayers = std::max(1u, manager_.getActiveLayerCount());
    visibleClusterDrawCapacityPerLayer_ = std::max(1u, (visibleClusterDrawCapacity_ + activeLayers - 1u) / activeLayers);
    visibleClusterDrawCapacity_ = visibleClusterDrawCapacityPerLayer_ * activeLayers;
    cullingUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_CullingUniforms.buffer",
            .size = sizeof(CullingUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    const CullingUniforms uniforms{
      .instancesCount = static_cast<uint32_t>(scene_.getInstanceCount()),
      .activeLayers = manager_.getActiveLayerCount(),
      .maxVisibleClusterDrawsPerLayer = visibleClusterDrawCapacityPerLayer_,
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .hpbMipCount = manager_.getHierarchicalPageBoundsMipCount(),
      .maxScenePages = scene_.nextPageTableSlot,
      .lodErrorThreshold = settings_.lodErrorThreshold,
      .maxHierarchyNodes = static_cast<uint32_t>(scene_.hierarchyBufferSize / sizeof(VirtualGeometryHierarchy)),
    };
    renderGraph->bufferWrite(cullingUniformBuffer_, 0, sizeof(uniforms), const_cast<CullingUniforms *>(&uniforms));

    cullingStatisticsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_CullingCounters.buffer",
            .size = kCullingCounterCount * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });

    const uint64_t hierarchyQueueBytes = static_cast<uint64_t>(settings_.hierarchyQueueCapacity) * kQueueElementSize;
    const uint64_t clusterQueueBytes = static_cast<uint64_t>(settings_.clusterQueueCapacity) * kQueueElementSize;
    hierarchyQueueA_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_HierarchyQueueA.buffer",
            .size = hierarchyQueueBytes,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    hierarchyQueueB_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_HierarchyQueueB.buffer",
            .size = hierarchyQueueBytes,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    clusterQueueBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ClusterQueue.buffer",
            .size = clusterQueueBytes,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    indirectArgsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_IndirectArgs.buffer",
            .size = sizeof(uint32_t) * 3u,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    shadowDrawIndirectBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ShadowDrawIndirect.buffer",
            .size = static_cast<uint64_t>(visibleClusterDrawCapacity_) * 4u * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    shadowVisibleClusterInfosBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ShadowVisibleClusterInfos.buffer",
            .size = static_cast<uint64_t>(visibleClusterDrawCapacity_) * sizeof(uint32_t) * 6u,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    processedPagesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ProcessedPages.buffer",
            .size = static_cast<uint64_t>(manager_.getVirtualPageTableResolution()) * manager_.getVirtualPageTableResolution() * std::max(1u, manager_.getActiveLayerCount()) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    pageClusterCountsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageClusterCounts.buffer",
            .size = static_cast<uint64_t>(manager_.getVirtualPageTableResolution()) * manager_.getVirtualPageTableResolution() * std::max(1u, manager_.getActiveLayerCount()) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    layerVisibleCountsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_LayerVisibleCounts.buffer",
            .size = static_cast<uint64_t>(std::max(1u, manager_.getActiveLayerCount())) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    dirtyPageCountsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_DirtyPageCounts.buffer",
            .size = static_cast<uint64_t>(std::max(1u, manager_.getActiveLayerCount())) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    dirtyPageListBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_DirtyPageList.buffer",
            .size = static_cast<uint64_t>(manager_.getVirtualPageTableResolution()) * manager_.getVirtualPageTableResolution() * std::max(1u, manager_.getActiveLayerCount()) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        });

    cullingLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Culling.layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instances", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "hierarchy", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "counters", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "hierarchyQueueRead", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "hierarchyQueueWrite", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "clusterQueue", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "meshPartTransforms", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "indirectArgs", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "scenePageTable", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "scenePagesBuffer", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "virtualPageTable", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cascadeStates", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cascadeMatrices", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowDrawIndirectBuffer", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "shadowVisibleClusterInfos", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "processedPages", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageClusterCounts", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "layerVisibleCounts", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageCounts", .binding = 20, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageList", .binding = 21, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "hpbTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    cullingShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Culling.shader",
            .layout = cullingLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-culling-multipass.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    initCullingPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "init_sync", .layout = cullingLayout_, .name = passName + "_InitCulling.pipeline", .shader = cullingShader_});
    setupRootNodesPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "setup_root_nodes", .layout = cullingLayout_, .name = passName + "_SetupRootNodes.pipeline", .shader = cullingShader_});
    prepareIndirectDispatchPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "prepare_indirect_dispatch", .layout = cullingLayout_, .name = passName + "_PrepareIndirectDispatch.pipeline", .shader = cullingShader_});
    processHierarchyNodesPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "process_hierarchy_nodes", .layout = cullingLayout_, .name = passName + "_ProcessHierarchyNodes.pipeline", .shader = cullingShader_});
    prepareClusterDispatchPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "prepare_cluster_dispatch", .layout = cullingLayout_, .name = passName + "_PrepareClusterDispatch.pipeline", .shader = cullingShader_});
    processClustersPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "process_clusters", .layout = cullingLayout_, .name = passName + "_ProcessClusters.pipeline", .shader = cullingShader_});

    auto makeCullingBindingGroups = [&](const std::string &name, const rendering::Buffer &readQueue, const rendering::Buffer &writeQueue) -> rendering::BindingGroups
    {
      return renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = cullingLayout_,
              .name = passName + "_" + name,
              .groups = {
                  rendering::GroupInfo{
                      .name = "group0",
                      .buffers = {
                          {.binding = 0, .bufferView = {.buffer = cullingUniformBuffer_, .offset = 0, .size = sizeof(CullingUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 1, .bufferView = {.buffer = scene_.instanceBuffer, .offset = 0, .size = scene_.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 2, .bufferView = {.buffer = scene_.hierarchyBuffer, .offset = 0, .size = scene_.hierarchyBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 3, .bufferView = {.buffer = cullingStatisticsBuffer_, .offset = 0, .size = kCullingCounterCount * sizeof(uint32_t), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 4, .bufferView = {.buffer = readQueue, .offset = 0, .size = hierarchyQueueBytes, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 5, .bufferView = {.buffer = writeQueue, .offset = 0, .size = hierarchyQueueBytes, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 6, .bufferView = {.buffer = clusterQueueBuffer_, .offset = 0, .size = clusterQueueBytes, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 7, .bufferView = {.buffer = scene_.meshPartTransformsBuffer, .offset = 0, .size = scene_.meshPartTransformsBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 8, .bufferView = {.buffer = indirectArgsBuffer_, .offset = 0, .size = sizeof(uint32_t) * 3u, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 9, .bufferView = {.buffer = scene_.pageTableBuffer, .offset = 0, .size = scene_.pagesTableBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 10, .bufferView = {.buffer = scene_.pagesBuffer, .offset = 0, .size = scene_.pagesBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 11, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 12, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 13, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 15, .bufferView = {.buffer = shadowDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(shadowDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 16, .bufferView = {.buffer = shadowVisibleClusterInfosBuffer_, .offset = 0, .size = renderGraph->getBufferSize(shadowVisibleClusterInfosBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 17, .bufferView = {.buffer = processedPagesBuffer_, .offset = 0, .size = renderGraph->getBufferSize(processedPagesBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 18, .bufferView = {.buffer = pageClusterCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageClusterCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 19, .bufferView = {.buffer = layerVisibleCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(layerVisibleCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 20, .bufferView = {.buffer = dirtyPageCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 21, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                      },
                      .textures = {
                          {.binding = 14, .textureView = makeHPBSampledView(manager_.getHierarchicalPageBoundsTexture(), 0u, manager_.getHierarchicalPageBoundsMipCount(), manager_.getMaxLayerCount())},
                      },
                  },
              },
          });
    };

    cullingBindingGroupPingA_ = makeCullingBindingGroups("CullingBindingGroupPingA", hierarchyQueueA_, hierarchyQueueB_);
    cullingBindingGroupPingB_ = makeCullingBindingGroups("CullingBindingGroupPingB", hierarchyQueueB_, hierarchyQueueA_);
  }

  void createScratchResources()
  {
    const uint32_t scratchResolution = getScratchResolution();
    for (uint32_t scratchIndex = 0u; scratchIndex < getScratchClipmapCount(); ++scratchIndex)
    {
      scratchClipmapTextures_.push_back(createFrameLocalTexture(
          rendering::TextureInfo{
              .name = passName + "_ScratchClipmap" + std::to_string(scratchIndex) + ".texture",
              .format = rendering::Format::Format_Depth32FloatStencil8,
              .memoryProperties = rendering::BufferUsage::BufferUsage_None,
              .usage = rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_DepthStencilAttachment,
              .width = scratchResolution,
              .height = scratchResolution,
              .depth = 1u,
              .mipLevels = 1u,
          }));
    }
  }

  void createPageStencilResources()
  {
    pageStencilUniformStride_ = getAlignedUniformSize(sizeof(StencilPageUniforms));
    const uint32_t totalLayers = std::max(1u, manager_.getActiveLayerCount());
    pageStencilUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageStencilUniforms.buffer",
            .size = static_cast<uint64_t>(pageStencilUniformStride_) * totalLayers,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    pageStencilDrawIndirectBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageStencilIndirect.buffer",
            .size = static_cast<uint64_t>(totalLayers) * 4u * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    pageClearDrawIndirectBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageClearIndirect.buffer",
            .size = static_cast<uint64_t>(totalLayers) * 4u * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    for (uint32_t layerIndex = 0u; layerIndex < totalLayers; ++layerIndex)
    {
      writePageStencilUniforms(layerIndex, layerIndex * pageStencilUniformStride_);
    }

    pageStencilLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_PageStencil.layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "cascadeStates", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "dirtyPageList", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                },
            }},
        });

    pageStencilBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = pageStencilLayout_,
            .name = passName + "_PageStencilBindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = pageStencilUniformBuffer_, .offset = 0, .size = pageStencilUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                },
            },
        });

    pageStencilVertexShader_ = renderGraph->createShader(rendering::ShaderInfo{
        .name = passName + "_PageStencilVS.shader",
        .layout = pageStencilLayout_,
        .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-scratch-stencil-vs.spirv"),
        .type = rendering::ShaderType::SpirV,
    });

    auto makePageStencilPipeline = [&](const std::string &name, uint32_t stencilReference) -> rendering::GraphicsPipeline
    {
      rendering::GraphicsPipelineInfo pipelineInfo{};
      pipelineInfo.name = passName + "_" + name + ".pipeline";
      pipelineInfo.layout = pageStencilLayout_;
      pipelineInfo.vertexStage.vertexLayoutElements = {};
      pipelineInfo.vertexStage.vertexShader = pageStencilVertexShader_;
      pipelineInfo.vertexStage.shaderEntry = "vs_main";
      pipelineInfo.vertexStage.cullType = rendering::CullMode::None;
      pipelineInfo.vertexStage.winding = rendering::WindingOrder::CW;
      pipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
      pipelineInfo.fragmentStage.shaderEntry = "";
      pipelineInfo.fragmentStage.depthAttatchment = {
        .enabled = true,
        .format = rendering::Format::Format_Depth32FloatStencil8,
        .loadOp = rendering::LoadOp::LoadOp_Load,
        .storeOp = rendering::StoreOp::StoreOp_Store,
        .depthTestEnabled = false,
        .depthWriteEnabled = false,
        .comparison = rendering::ComparisonOp::ComparisonOp_Always,
        .stencilFront = {
          .enabled = true,
          .comparison = rendering::ComparisonOp::ComparisonOp_Always,
          .failOp = rendering::StencilOp::Keep,
          .passOp = rendering::StencilOp::Replace,
          .depthFailOp = rendering::StencilOp::Keep,
          .compareMask = 0xFFu,
          .writeMask = 0xFFu,
          .reference = stencilReference,
        },
        .stencilBack = {
          .enabled = true,
          .comparison = rendering::ComparisonOp::ComparisonOp_Always,
          .failOp = rendering::StencilOp::Keep,
          .passOp = rendering::StencilOp::Replace,
          .depthFailOp = rendering::StencilOp::Keep,
          .compareMask = 0xFFu,
          .writeMask = 0xFFu,
          .reference = stencilReference,
        },
      };
      return renderGraph->createGraphicsPipeline(pipelineInfo);
    };

    rendering::GraphicsPipelineInfo clearPipelineInfo{};
    clearPipelineInfo.name = passName + "_ScratchClear.pipeline";
    clearPipelineInfo.layout = pageStencilLayout_;
    clearPipelineInfo.vertexStage.vertexLayoutElements = {};
    clearPipelineInfo.vertexStage.vertexShader = pageStencilVertexShader_;
    clearPipelineInfo.vertexStage.shaderEntry = "vs_main";
    clearPipelineInfo.vertexStage.cullType = rendering::CullMode::None;
    clearPipelineInfo.vertexStage.winding = rendering::WindingOrder::CW;
    clearPipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    clearPipelineInfo.fragmentStage.shaderEntry = "";
    clearPipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = rendering::Format::Format_Depth32FloatStencil8,
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = true,
      .depthWriteEnabled = true,
      .comparison = rendering::ComparisonOp::ComparisonOp_Always,
    };
    clearScratchPipeline_ = renderGraph->createGraphicsPipeline(clearPipelineInfo);
    pageStencilMarkPipeline_ = makePageStencilPipeline("PageStencilMark", 1u);
    pageStencilResetPipeline_ = makePageStencilPipeline("PageStencilReset", 0u);
  }

  void createScratchDrawResources()
  {
    drawScratchUniformStride_ = getAlignedUniformSize(sizeof(ScratchDrawUniforms));
    const uint32_t totalLayers = std::max(1u, manager_.getActiveLayerCount());
    drawScratchUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ScratchDrawUniforms.buffer",
            .size = static_cast<uint64_t>(drawScratchUniformStride_) * totalLayers,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    for (uint32_t layerIndex = 0u; layerIndex < totalLayers; ++layerIndex)
    {
      writeScratchDrawUniforms(layerIndex, layerIndex * drawScratchUniformStride_);
    }

    drawScratchLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_ScratchDraw.layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "instances", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "scenePageTable", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "scenePagesBuffer", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "shadowVisibleClusterInfos", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "cascadeStates", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "cascadeMatrices", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "meshPartTransforms", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                },
            }},
        });

    drawScratchBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = drawScratchLayout_,
            .name = passName + "_ScratchDrawBindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = drawScratchUniformBuffer_, .offset = 0, .size = drawScratchUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = scene_.instanceBuffer, .offset = 0, .size = scene_.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = scene_.pageTableBuffer, .offset = 0, .size = scene_.pagesTableBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = scene_.pagesBuffer, .offset = 0, .size = scene_.pagesBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = shadowVisibleClusterInfosBuffer_, .offset = 0, .size = renderGraph->getBufferSize(shadowVisibleClusterInfosBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 7, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 8, .bufferView = {.buffer = scene_.meshPartTransformsBuffer, .offset = 0, .size = scene_.meshPartTransformsBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                    },
                },
            },
        });

    drawScratchVertexShader_ = renderGraph->createShader(rendering::ShaderInfo{
        .name = passName + "_ScratchDrawVS.shader",
        .layout = drawScratchLayout_,
        .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-meshlet-vs.spirv"),
        .type = rendering::ShaderType::SpirV,
    });

    rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_ScratchDraw.pipeline";
    pipelineInfo.layout = drawScratchLayout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = drawScratchVertexShader_;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = rendering::CullMode::Front;
    pipelineInfo.vertexStage.winding = rendering::WindingOrder::CW;
    pipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.shaderEntry = "";
    pipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = rendering::Format::Format_Depth32FloatStencil8,
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = true,
      .depthWriteEnabled = true,
      .comparison = manager_.getSettings().reverseZ ? rendering::ComparisonOp::ComparisonOp_GreaterOrEqual
                                                    : rendering::ComparisonOp::ComparisonOp_LessOrEqual,
    };
    drawScratchPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);

    pipelineInfo.name = passName + "_ScratchDrawStencil.pipeline";
    pipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = rendering::Format::Format_Depth32FloatStencil8,
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = true,
      .depthWriteEnabled = true,
      .comparison = manager_.getSettings().reverseZ ? rendering::ComparisonOp::ComparisonOp_GreaterOrEqual
                                                    : rendering::ComparisonOp::ComparisonOp_LessOrEqual,
      .stencilFront = {
        .enabled = true,
        .comparison = rendering::ComparisonOp::ComparisonOp_Equal,
        .failOp = rendering::StencilOp::Keep,
        .passOp = rendering::StencilOp::Keep,
        .depthFailOp = rendering::StencilOp::Keep,
        .compareMask = 0xFFu,
        .writeMask = 0u,
        .reference = 1u,
      },
      .stencilBack = {
        .enabled = true,
        .comparison = rendering::ComparisonOp::ComparisonOp_Equal,
        .failOp = rendering::StencilOp::Keep,
        .passOp = rendering::StencilOp::Keep,
        .depthFailOp = rendering::StencilOp::Keep,
        .compareMask = 0xFFu,
        .writeMask = 0u,
        .reference = 1u,
      },
    };
    drawScratchStencilPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  void createResolveResources()
  {
    resolvePagesUniformStride_ = getAlignedUniformSize(sizeof(ResolveUniforms));
    const uint32_t totalLayers = std::max(1u, manager_.getActiveLayerCount());
    resolveDispatchArgsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ResolveDispatchArgs.buffer",
            .size = static_cast<uint64_t>(totalLayers) * sizeof(uint32_t) * 3u,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    pageOpDispatchArgsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageOpDispatchArgs.buffer",
            .size = static_cast<uint64_t>(totalLayers) * sizeof(uint32_t) * 3u,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    resolvePagesUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ResolveUniforms.buffer",
            .size = static_cast<uint64_t>(resolvePagesUniformStride_) * totalLayers,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    for (uint32_t layerIndex = 0u; layerIndex < totalLayers; ++layerIndex)
    {
      writeResolveUniforms(layerIndex, layerIndex * resolvePagesUniformStride_);
    }

    resolvePagesLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Resolve.layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "virtualPageTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cascadeStates", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageCounts", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageList", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "resolveDispatchArgs", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageStencilDrawIndirectArgs", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageClearDrawIndirectArgs", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageOpDispatchArgs", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "scratchTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "atlasTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    resolvePagesBindingGroups_.reserve(scratchClipmapTextures_.size());
    for (uint32_t scratchIndex = 0u; scratchIndex < scratchClipmapTextures_.size(); ++scratchIndex)
    {
      resolvePagesBindingGroups_.push_back(renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = resolvePagesLayout_,
              .name = passName + "_ResolveBindingGroups" + std::to_string(scratchIndex),
              .groups = {
                  rendering::GroupInfo{
                      .name = "group0",
                      .buffers = {
                          {.binding = 0, .bufferView = {.buffer = resolvePagesUniformBuffer_, .offset = 0, .size = resolvePagesUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 1, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 2, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 3, .bufferView = {.buffer = dirtyPageCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 4, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 5, .bufferView = {.buffer = resolveDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(resolveDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 6, .bufferView = {.buffer = pageStencilDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageStencilDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 7, .bufferView = {.buffer = pageClearDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageClearDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 10, .bufferView = {.buffer = pageOpDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageOpDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                      },
                      .textures = {
                          {.binding = 8, .textureView = makeScratchSampledView(scratchClipmapTextures_[scratchIndex])},
                      },
                      .storageTextures = {
                          {.binding = 9, .textureView = makeAtlasStorageView(manager_.getShadowAtlasTexture())},
                      },
                  },
              },
          }));
    }

    resolvePagesShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Resolve.shader",
            .layout = resolvePagesLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-layer-resolve.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    resolvePagesPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = resolvePagesLayout_,
            .name = passName + "_Resolve.pipeline",
            .shader = resolvePagesShader_,
        });
    prepareResolveDispatchPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "prepare_dispatch",
            .layout = resolvePagesLayout_,
            .name = passName + "_ResolvePrepareDispatch.pipeline",
            .shader = resolvePagesShader_,
        });
  }

  void createFinishResources()
  {
    finishPagesUniformStride_ = getAlignedUniformSize(sizeof(FinishUniforms));
    const uint32_t totalLayers = std::max(1u, manager_.getActiveLayerCount());
    finishPagesUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_FinishUniforms.buffer",
            .size = static_cast<uint64_t>(finishPagesUniformStride_) * totalLayers,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    for (uint32_t layerIndex = 0u; layerIndex < totalLayers; ++layerIndex)
    {
      writeFinishUniforms(layerIndex, layerIndex * finishPagesUniformStride_);
    }

    finishPagesLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Finish.layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "drawCounters", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "virtualPageTable", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageCounts", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dirtyPageList", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    finishPagesBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = finishPagesLayout_,
            .name = passName + "_Finish.bindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = finishPagesUniformBuffer_, .offset = 0, .size = finishPagesUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = cullingStatisticsBuffer_, .offset = 0, .size = kCullingCounterCount * sizeof(uint32_t), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 3, .bufferView = {.buffer = dirtyPageCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                },
            },
        });

    finishPagesShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Finish.shader",
            .layout = finishPagesLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-finish.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    finishPagesPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "finish_drawn_pages_main",
            .layout = finishPagesLayout_,
            .name = passName + "_Finish.pipeline",
            .shader = finishPagesShader_,
        });
  }

  void writeScratchDrawUniforms(uint32_t currentLayer, uint32_t offset)
  {
    const ScratchDrawUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .currentLayer = currentLayer,
      .scratchResolution = getScratchResolution(),
    };
    renderGraph->bufferWrite(drawScratchUniformBuffer_, offset, sizeof(uniforms), const_cast<ScratchDrawUniforms *>(&uniforms));
  }

  void writeResolveUniforms(uint32_t currentLayer, uint32_t offset)
  {
    const ResolveUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .currentLayer = currentLayer,
      .activeLayers = manager_.getActiveLayerCount(),
      .enableDirtyPageStencil = settings_.enableDirtyPageStencil ? 1u : 0u,
    };
    renderGraph->bufferWrite(resolvePagesUniformBuffer_, offset, sizeof(uniforms), const_cast<ResolveUniforms *>(&uniforms));
  }

  void writePageStencilUniforms(uint32_t currentLayer, uint32_t offset)
  {
    const StencilPageUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .currentLayer = currentLayer,
      .scratchResolution = getScratchResolution(),
      .depthValue = manager_.getSettings().reverseZ ? 0.0f : 1.0f,
    };
    renderGraph->bufferWrite(pageStencilUniformBuffer_, offset, sizeof(uniforms), const_cast<StencilPageUniforms *>(&uniforms));
  }

  void writeFinishUniforms(uint32_t currentLayer, uint32_t offset)
  {
    const FinishUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .currentLayer = currentLayer,
      ._padding0 = 0u,
    };
    renderGraph->bufferWrite(finishPagesUniformBuffer_, offset, sizeof(uniforms), const_cast<FinishUniforms *>(&uniforms));
  }

  void recordHPBPasses(rendering::CommandRecorder &commandBuffer)
  {
    const uint32_t activeLayers = std::max(1u, manager_.getActiveLayerCount());
    commandBuffer.cmdBindComputePipeline(buildHPBPipeline_);
    commandBuffer.cmdBindBindingGroups(hpbBaseBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(1u, 1u, activeLayers);
  }

  void recordCullingPasses(rendering::CommandRecorder &commandBuffer)
  {
    const uint32_t drawIndirectWordCount = visibleClusterDrawCapacity_ * 4u;
    const uint32_t totalPages = manager_.getVirtualPageTableResolution() * manager_.getVirtualPageTableResolution() * std::max(1u, manager_.getActiveLayerCount());
    const uint32_t initItemCount = std::max(std::max(std::max(drawIndirectWordCount, kCullingCounterCount), totalPages), std::max(1u, manager_.getActiveLayerCount()));
    const uint32_t initDispatchX = std::max(1u, (initItemCount + kWorkgroupSize - 1u) / kWorkgroupSize);
    const uint32_t rootCount = static_cast<uint32_t>(scene_.getInstanceCount()) * std::max(1u, manager_.getActiveLayerCount());
    const uint32_t setupDispatchX = std::max(1u, (rootCount + kWorkgroupSize - 1u) / kWorkgroupSize);

    commandBuffer.cmdBindComputePipeline(initCullingPipeline_);
    commandBuffer.cmdBindBindingGroups(cullingBindingGroupPingA_, nullptr, 0);
    commandBuffer.cmdDispatch(initDispatchX, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(setupRootNodesPipeline_);
    commandBuffer.cmdBindBindingGroups(cullingBindingGroupPingA_, nullptr, 0);
    commandBuffer.cmdDispatch(setupDispatchX, 1u, 1u);

    for (uint32_t level = 0u; level < settings_.maxHierarchyLevels; ++level)
    {
      rendering::BindingGroups currentBindingGroup = (level % 2u) == 0u ? cullingBindingGroupPingB_ : cullingBindingGroupPingA_;
      commandBuffer.cmdBindComputePipeline(prepareIndirectDispatchPipeline_);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, nullptr, 0);
      commandBuffer.cmdDispatch(1u, 1u, 1u);

      commandBuffer.cmdBindComputePipeline(processHierarchyNodesPipeline_);
      commandBuffer.cmdBindBindingGroups(currentBindingGroup, nullptr, 0);
      commandBuffer.cmdDispatchIndirect(indirectArgsBuffer_, 0u);
    }

    commandBuffer.cmdBindComputePipeline(prepareClusterDispatchPipeline_);
    commandBuffer.cmdBindBindingGroups(cullingBindingGroupPingA_, nullptr, 0);
    commandBuffer.cmdDispatch(1u, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(processClustersPipeline_);
    commandBuffer.cmdBindBindingGroups(cullingBindingGroupPingA_, nullptr, 0);
    commandBuffer.cmdDispatchIndirect(indirectArgsBuffer_, 0u);
  }

  void recordPageStencilPass(
      rendering::CommandRecorder &commandBuffer,
      rendering::GraphicsPipeline pipeline,
      rendering::Texture scratchTexture,
      rendering::Buffer indirectBuffer,
      uint32_t layerIndex,
      uint32_t scratchResolution,
      uint64_t indirectOffset)
  {
    rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_PageStencil.layer" + std::to_string(layerIndex) + "." + pipeline.name;
    renderPass.scissor = rendering::Rect2D(0, 0, scratchResolution, scratchResolution);
    renderPass.viewport = rendering::Viewport(scratchResolution, scratchResolution);
    renderPass.depthStencilAttachment = {
      .enabled = true,
      .name = scratchTexture.name + "_DepthStencilView",
      .view = makeScratchDepthStencilAttachmentView(scratchTexture),
      .clearDepth = 1.0f,
      .clearStencil = 0u,
    };

    commandBuffer.cmdBindGraphicsPipeline(pipeline);
    commandBuffer.cmdBeginRenderPass(renderPass);
    uint32_t dynamicOffset = layerIndex * pageStencilUniformStride_;
    commandBuffer.cmdBindBindingGroups(pageStencilBindingGroups_, &dynamicOffset, 1);
    commandBuffer.cmdDrawIndirect(
        rendering::BufferView{
            .buffer = indirectBuffer,
            .offset = indirectOffset,
            .size = renderGraph->getBufferSize(indirectBuffer) - indirectOffset,
            .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
        },
        static_cast<uint32_t>(indirectOffset),
        1u,
        4u * sizeof(uint32_t));
    commandBuffer.cmdEndRenderPass();
  }

  void recordScratchPasses(rendering::CommandRecorder &commandBuffer)
  {
    const uint32_t activeLayers = manager_.getActiveLayerCount();
    if (activeLayers == 0u || scratchClipmapTextures_.empty())
    {
      return;
    }

    const uint32_t scratchResolution = getScratchResolution();
    const uint32_t stride = 4u * sizeof(uint32_t);

    commandBuffer.cmdBindComputePipeline(prepareResolveDispatchPipeline_);
    uint32_t prepareResolveDynamicOffset = 0u;
    commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[0], &prepareResolveDynamicOffset, 1);
    commandBuffer.cmdDispatch(std::max(1u, (activeLayers + kResolveWorkgroupSize - 1u) / kResolveWorkgroupSize), 1u, 1u);

    for (uint32_t layerIndex = 0u; layerIndex < activeLayers; ++layerIndex)
    {
      const uint32_t scratchIndex = layerIndex % scratchClipmapTextures_.size();
      const uint32_t drawUniformOffset = layerIndex * drawScratchUniformStride_;
      const uint32_t resolveUniformOffset = layerIndex * resolvePagesUniformStride_;
      const uint32_t indirectOffset = layerIndex * visibleClusterDrawCapacityPerLayer_ * stride;
      const uint32_t countOffset = layerIndex * sizeof(uint32_t);
      const uint64_t dispatchOffset = static_cast<uint64_t>(layerIndex) * sizeof(uint32_t) * 3u;
      const uint64_t stencilIndirectOffset = static_cast<uint64_t>(layerIndex) * 4u * sizeof(uint32_t);

      recordPageStencilPass(commandBuffer, clearScratchPipeline_, scratchClipmapTextures_[scratchIndex], pageClearDrawIndirectBuffer_, layerIndex, scratchResolution, stencilIndirectOffset);
      if (settings_.enableDirtyPageStencil)
      {
        recordPageStencilPass(commandBuffer, pageStencilMarkPipeline_, scratchClipmapTextures_[scratchIndex], pageStencilDrawIndirectBuffer_, layerIndex, scratchResolution, stencilIndirectOffset);
      }

      rendering::RenderPassInfo renderPass{};
      renderPass.name = passName + "_ScratchRenderPass.layer" + std::to_string(layerIndex);
      renderPass.scissor = rendering::Rect2D(0, 0, scratchResolution, scratchResolution);
      renderPass.viewport = rendering::Viewport(scratchResolution, scratchResolution);
      renderPass.depthStencilAttachment = {
        .enabled = true,
        .name = scratchClipmapTextures_[scratchIndex].name + "_DepthStencilView",
        .view = makeScratchDepthStencilAttachmentView(scratchClipmapTextures_[scratchIndex]),
        .clearDepth = manager_.getSettings().reverseZ ? 0.0f : 1.0f,
        .clearStencil = 0u,
      };

      commandBuffer.cmdBindGraphicsPipeline(settings_.enableDirtyPageStencil ? drawScratchStencilPipeline_ : drawScratchPipeline_);
      commandBuffer.cmdBeginRenderPass(renderPass);
      uint32_t drawDynamicOffset = drawUniformOffset;
      commandBuffer.cmdBindBindingGroups(drawScratchBindingGroups_, &drawDynamicOffset, 1);

      if ((renderGraph->getRHI()->getFeatures() & rendering::DeviceFeatures::DeviceFeatures_DrawIndirectCount) != 0u)
      {
        commandBuffer.cmdDrawIndirectCount(
            rendering::BufferView{
                .buffer = shadowDrawIndirectBuffer_,
                .offset = indirectOffset,
                .size = renderGraph->getBufferSize(shadowDrawIndirectBuffer_) - indirectOffset,
                .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
            },
            indirectOffset,
            rendering::BufferView{
                .buffer = layerVisibleCountsBuffer_,
                .offset = countOffset,
                .size = sizeof(uint32_t),
                .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
            },
            countOffset,
            visibleClusterDrawCapacityPerLayer_,
            stride);
      }
      else
      {
        commandBuffer.cmdDrawIndirect(
            rendering::BufferView{
                .buffer = shadowDrawIndirectBuffer_,
                .offset = indirectOffset,
                .size = renderGraph->getBufferSize(shadowDrawIndirectBuffer_) - indirectOffset,
                .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
            },
            indirectOffset,
            visibleClusterDrawCapacityPerLayer_,
            stride);
      }

      commandBuffer.cmdEndRenderPass();

      commandBuffer.cmdBindComputePipeline(resolvePagesPipeline_);
      uint32_t resolveDynamicOffset = resolveUniformOffset;
      commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[scratchIndex], &resolveDynamicOffset, 1);
      commandBuffer.cmdDispatchIndirect(pageOpDispatchArgsBuffer_, dispatchOffset);

      if (settings_.enableDirtyPageStencil)
      {
        recordPageStencilPass(commandBuffer, pageStencilResetPipeline_, scratchClipmapTextures_[scratchIndex], pageStencilDrawIndirectBuffer_, layerIndex, scratchResolution, stencilIndirectOffset);
      }
    }
  }

  void recordFinishPass(rendering::CommandRecorder &commandBuffer)
  {
    const uint32_t activeLayers = manager_.getActiveLayerCount();
    if (activeLayers == 0u)
    {
      return;
    }

    commandBuffer.cmdBindComputePipeline(finishPagesPipeline_);
    for (uint32_t layerIndex = 0u; layerIndex < activeLayers; ++layerIndex)
    {
      uint32_t finishDynamicOffset = layerIndex * finishPagesUniformStride_;
      commandBuffer.cmdBindBindingGroups(finishPagesBindingGroups_, &finishDynamicOffset, 1);
      commandBuffer.cmdDispatchIndirect(resolveDispatchArgsBuffer_, static_cast<uint64_t>(layerIndex) * sizeof(uint32_t) * 3u);
    }
  }

  VirtualGeometryScene &scene_;
  VirtualShadowMapManager &manager_;
  Settings settings_;

  uint32_t visibleClusterDrawCapacity_ = 0u;
  uint32_t visibleClusterDrawCapacityPerLayer_ = 0u;

  rendering::BindingsLayout hpbLayout_;
  rendering::BindingGroups hpbBaseBindingGroups_;
  rendering::Shader hpbShader_;
  rendering::ComputePipeline buildHPBPipeline_;
  rendering::Buffer hpbUniformBuffer_;

  rendering::BindingsLayout cullingLayout_;
  rendering::BindingGroups cullingBindingGroupPingA_;
  rendering::BindingGroups cullingBindingGroupPingB_;
  rendering::Shader cullingShader_;
  rendering::ComputePipeline initCullingPipeline_;
  rendering::ComputePipeline setupRootNodesPipeline_;
  rendering::ComputePipeline prepareIndirectDispatchPipeline_;
  rendering::ComputePipeline processHierarchyNodesPipeline_;
  rendering::ComputePipeline prepareClusterDispatchPipeline_;
  rendering::ComputePipeline processClustersPipeline_;
  rendering::Buffer cullingUniformBuffer_;
  rendering::Buffer cullingStatisticsBuffer_;
  rendering::Buffer hierarchyQueueA_;
  rendering::Buffer hierarchyQueueB_;
  rendering::Buffer clusterQueueBuffer_;
  rendering::Buffer indirectArgsBuffer_;
  rendering::Buffer shadowDrawIndirectBuffer_;
  rendering::Buffer shadowVisibleClusterInfosBuffer_;
  rendering::Buffer processedPagesBuffer_;
  rendering::Buffer pageClusterCountsBuffer_;
  rendering::Buffer layerVisibleCountsBuffer_;
  rendering::Buffer dirtyPageCountsBuffer_;
  rendering::Buffer dirtyPageListBuffer_;

  std::vector<rendering::Texture> scratchClipmapTextures_;

  rendering::BindingsLayout pageStencilLayout_;
  rendering::BindingGroups pageStencilBindingGroups_;
  rendering::Shader pageStencilVertexShader_;
  rendering::GraphicsPipeline clearScratchPipeline_;
  rendering::GraphicsPipeline pageStencilMarkPipeline_;
  rendering::GraphicsPipeline pageStencilResetPipeline_;
  rendering::Buffer pageStencilUniformBuffer_;
  rendering::Buffer pageClearDrawIndirectBuffer_;
  rendering::Buffer pageStencilDrawIndirectBuffer_;
  uint32_t pageStencilUniformStride_ = 0u;

  rendering::BindingsLayout drawScratchLayout_;
  rendering::BindingGroups drawScratchBindingGroups_;
  rendering::Shader drawScratchVertexShader_;
  rendering::GraphicsPipeline drawScratchPipeline_;
  rendering::GraphicsPipeline drawScratchStencilPipeline_;
  rendering::Buffer drawScratchUniformBuffer_;
  uint32_t drawScratchUniformStride_ = 0u;

  rendering::BindingsLayout resolvePagesLayout_;
  std::vector<rendering::BindingGroups> resolvePagesBindingGroups_;
  rendering::Shader resolvePagesShader_;
  rendering::ComputePipeline prepareResolveDispatchPipeline_;
  rendering::ComputePipeline resolvePagesPipeline_;
  rendering::Buffer resolvePagesUniformBuffer_;
  rendering::Buffer resolveDispatchArgsBuffer_;
  rendering::Buffer pageOpDispatchArgsBuffer_;
  uint32_t resolvePagesUniformStride_ = 0u;

  rendering::BindingsLayout finishPagesLayout_;
  rendering::BindingGroups finishPagesBindingGroups_;
  rendering::Shader finishPagesShader_;
  rendering::ComputePipeline finishPagesPipeline_;
  rendering::Buffer finishPagesUniformBuffer_;
  uint32_t finishPagesUniformStride_ = 0u;
};

} // namespace gpgpu
} // namespace virtualgeometry
