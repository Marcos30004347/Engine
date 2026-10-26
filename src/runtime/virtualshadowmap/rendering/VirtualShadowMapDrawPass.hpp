#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/MaterialTextureResolver.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualShadowMapDrawPass : public rendering::Pass
{
public:
  enum class RasterMode : uint32_t
  {
    ScratchClipmapDepth = 0u,
    ClipSpaceAtlasDepth = 1u,
    ImperfectPointSplat = 2u,
  };

  struct Settings
  {
    uint32_t maxHierarchyLevels = 8u;
    uint32_t hierarchyQueueCapacity = 1024u * 1024u;
    uint32_t clusterQueueCapacity = 1024u * 1024u;
    uint32_t maxVisibleClusterDraws = 131072u;
    uint32_t scratchClipmapCount = 2u;
    float lodErrorThreshold = 1.0f;
    bool registerDirtyPageStencilPasses = false;
    RasterMode rasterMode = RasterMode::ClipSpaceAtlasDepth;
  };

  VirtualShadowMapDrawPass(VirtualGeometryScene &scene, VirtualShadowMapManager &manager, Settings settings)
      : scene_(scene), manager_(manager), settings_(settings)
  {
  }

  ~VirtualShadowMapDrawPass() override
  {
    const auto deleteComputePipelineIfValid = [&](const rendering::ComputePipeline &pipeline)
    {
      if (pipeline.isValid())
      {
        renderGraph->deleteComputePipeline(pipeline);
      }
    };
    const auto deleteGraphicsPipelineIfValid = [&](const rendering::GraphicsPipeline &pipeline)
    {
      if (pipeline.isValid())
      {
        renderGraph->deleteGraphicsPipeline(pipeline);
      }
    };
    const auto deleteShaderIfValid = [&](const rendering::Shader &shader)
    {
      if (shader.isValid())
      {
        renderGraph->deleteShader(shader);
      }
    };
    const auto deleteBindingGroupsIfValid = [&](const rendering::BindingGroups &bindingGroups)
    {
      if (bindingGroups.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups);
      }
    };
    const auto deleteBindingsLayoutIfValid = [&](const rendering::BindingsLayout &layout)
    {
      if (layout.isValid())
      {
        renderGraph->deleteBindingsLayout(layout);
      }
    };
    const auto deleteBufferIfValid = [&](const rendering::Buffer &buffer)
    {
      if (buffer.isValid())
      {
        renderGraph->deleteBuffer(buffer);
      }
    };
    const auto deleteTextureIfValid = [&](const rendering::Texture &texture)
    {
      if (texture.isValid())
      {
        renderGraph->deleteTexture(texture);
      }
    };

    deleteComputePipelineIfValid(buildHPBPipeline_);
    deleteShaderIfValid(hpbShader_);
    deleteBindingGroupsIfValid(hpbBaseBindingGroups_);
    deleteBufferIfValid(hpbUniformBuffer_);
    deleteBindingsLayoutIfValid(hpbLayout_);

    deleteComputePipelineIfValid(initCullingPipeline_);
    deleteComputePipelineIfValid(setupRootNodesPipeline_);
    deleteComputePipelineIfValid(prepareIndirectDispatchPipeline_);
    deleteComputePipelineIfValid(processHierarchyNodesPipeline_);
    deleteComputePipelineIfValid(prepareClusterDispatchPipeline_);
    deleteComputePipelineIfValid(processClustersPipeline_);
    deleteComputePipelineIfValid(prepareShadowDrawIndirectArgsPipeline_);
    deleteShaderIfValid(cullingShader_);
    deleteBindingGroupsIfValid(cullingBindingGroupPingA_);
    deleteBindingGroupsIfValid(cullingBindingGroupPingB_);
    deleteBindingsLayoutIfValid(cullingLayout_);
    deleteBufferIfValid(cullingUniformBuffer_);
    deleteBufferIfValid(cullingStatisticsBuffer_);
    deleteBufferIfValid(hierarchyQueueA_);
    deleteBufferIfValid(hierarchyQueueB_);
    deleteBufferIfValid(clusterQueueBuffer_);
    deleteBufferIfValid(indirectArgsBuffer_);
    deleteBufferIfValid(shadowDrawIndirectBuffer_);
    deleteBufferIfValid(shadowVisibleClusterInfosBuffer_);
    deleteBufferIfValid(processedPagesBuffer_);
    deleteBufferIfValid(pageClusterCountsBuffer_);
    deleteBufferIfValid(layerVisibleCountsBuffer_);
    deleteBufferIfValid(dirtyPageCountsBuffer_);
    deleteBufferIfValid(dirtyPageListBuffer_);
    deleteBufferIfValid(globalDirtyPageCountBuffer_);
    deleteBufferIfValid(globalDirtyPageListBuffer_);

    deleteGraphicsPipelineIfValid(clearScratchPipeline_);
    deleteGraphicsPipelineIfValid(drawScratchStencilPipeline_);
    deleteGraphicsPipelineIfValid(pageStencilMarkPipeline_);
    deleteGraphicsPipelineIfValid(pageStencilResetPipeline_);
    deleteShaderIfValid(drawScratchVertexShader_);
    deleteShaderIfValid(drawScratchFragmentShader_);
    deleteShaderIfValid(pageStencilVertexShader_);
    deleteBindingGroupsIfValid(drawScratchBindingGroups_);
    deleteBindingsLayoutIfValid(drawScratchLayout_);
    deleteBufferIfValid(drawScratchUniformBuffer_);
    deleteBufferIfValid(rsmMaterialBuffer_);
    deleteBufferIfValid(rsmVtAtlasInfoBuffer_);
    deleteBufferIfValid(rsmTextureEntriesBuffer_);
    deleteBufferIfValid(rsmVtPageTableBuffer_);
    deleteBufferIfValid(rsmVtPhysicalPagesBuffer_);
    deleteBindingGroupsIfValid(pageStencilBindingGroups_);
    deleteBindingsLayoutIfValid(pageStencilLayout_);
    deleteBufferIfValid(pageStencilUniformBuffer_);
    deleteBufferIfValid(pageClearDrawIndirectBuffer_);
    deleteBufferIfValid(pageStencilDrawIndirectBuffer_);

    deleteComputePipelineIfValid(resolvePagesPipeline_);
    deleteComputePipelineIfValid(clearRsmPagesPipeline_);
    deleteComputePipelineIfValid(clearDirtyPagesPipeline_);
    deleteComputePipelineIfValid(prepareResolveDispatchPipeline_);
    deleteShaderIfValid(resolvePagesShader_);
    for (const rendering::BindingGroups &bindingGroups : resolvePagesBindingGroups_)
    {
      deleteBindingGroupsIfValid(bindingGroups);
    }
    deleteBindingsLayoutIfValid(resolvePagesLayout_);
    deleteBufferIfValid(resolvePagesUniformBuffer_);
    deleteBufferIfValid(resolveDispatchArgsBuffer_);
    deleteBufferIfValid(pageOpDispatchArgsBuffer_);

    deleteComputePipelineIfValid(finishPagesPipeline_);
    deleteShaderIfValid(finishPagesShader_);
    deleteBindingGroupsIfValid(finishPagesBindingGroups_);
    deleteBindingsLayoutIfValid(finishPagesLayout_);
    deleteBufferIfValid(finishPagesUniformBuffer_);

    for (const rendering::Texture &scratchTexture : scratchClipmapTextures_)
    {
      deleteTextureIfValid(scratchTexture);
    }
    deleteTextureIfValid(scratchPageTexture_);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createHPBComputeResources();
    createCullingResources();
    createScratchResources();
    if (usesReflexiveShadowMapRaster())
    {
      createRsmMaterialResources();
    }
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
    uint32_t maxDirtyPagesPerFrame = 0u;
  };

  struct ScratchDrawUniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t activeLayers = 0u;
    uint32_t currentLayer = 0u;
    uint32_t scratchResolution = 0u;
    uint32_t physicalAtlasResolution = 0u;
    uint32_t reverseZ = 0u;
    uint32_t maxVisibleClusterDrawsPerLayer = 0u;
    uint32_t rsmAtlasResolution = 0u;
    uint32_t _padding0[3] = {0u, 0u, 0u};
  };

  struct RsmMaterialGPU
  {
    uint32_t materialId = 0u;
    uint32_t baseColorTextureId = Material::InvalidTextureId;
    uint32_t normalTextureId = Material::InvalidTextureId;
    uint32_t flags = 0u;
    float baseColorOpacity[4]{0.85f, 0.85f, 0.85f, 1.0f};
    float pbrFactors[4]{0.5f, 0.0f, 1.0f, 0.0f};
    float emissiveFactorAndStrength[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(RsmMaterialGPU) == 64u, "RsmMaterialGPU must match vsm-meshlet-vs.wgsl RsmMaterial");

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
    uint32_t physicalAtlasResolution = 0u;
    uint32_t reverseZ = 0u;
    uint32_t rsmAtlasResolution = 0u;
    uint32_t _padding0 = 0u;
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
    const uint64_t clusterCount = std::max<uint64_t>(1u, scene_.clusterAllocator.getAllocatedSize());
    const uint64_t layerCount = std::max<uint64_t>(1u, manager_.getActiveLayerCount());
    const uint64_t drawSlack = usesClipSpaceAtlasDepthRaster() ? 16u : 4u;
    const uint64_t recommended = clusterCount * layerCount * drawSlack;
    const uint64_t minimum = std::max<uint64_t>(1u, settings_.maxVisibleClusterDraws);
    return static_cast<uint32_t>(std::min<uint64_t>(std::max(recommended, minimum), std::numeric_limits<uint32_t>::max()));
  }

  bool usesClipSpaceAtlasDepthRaster() const
  {
    return settings_.rasterMode == RasterMode::ClipSpaceAtlasDepth ||
           settings_.rasterMode == RasterMode::ImperfectPointSplat;
  }

  bool usesImperfectShadowMapRaster() const
  {
    return settings_.rasterMode == RasterMode::ImperfectPointSplat;
  }

  bool usesReflexiveShadowMapRaster() const
  {
    return manager_.usesRsmPayload() && settings_.rasterMode == RasterMode::ClipSpaceAtlasDepth;
  }

  uint32_t getScratchClipmapCount() const
  {
    return std::max(1u, settings_.scratchClipmapCount);
  }

  uint32_t getShadowVisibleClusterInfoWordCount() const
  {
    return usesClipSpaceAtlasDepthRaster() ? 7u : 6u;
  }

  uint32_t getScratchResolution() const
  {
    return usesClipSpaceAtlasDepthRaster() ? manager_.getPhysicalPageResolution()
                                           : manager_.getVirtualPageTableResolution() * manager_.getPhysicalPageResolution();
  }

  bool shouldRegisterDirtyPageStencilPasses() const
  {
    return !usesClipSpaceAtlasDepthRaster() && settings_.registerDirtyPageStencilPasses;
  }

  rendering::Format getScratchTextureFormat() const
  {
    if (usesClipSpaceAtlasDepthRaster())
    {
      return rendering::Format::Format_Depth32Float;
    }
    return shouldRegisterDirtyPageStencilPasses() ? rendering::Format::Format_Depth32FloatStencil8
                                                  : rendering::Format::Format_Depth32Float;
  }

  uint32_t getAlignedUniformSize(uint32_t uniformSize) const
  {
    const uint32_t alignment = std::max<uint32_t>(1u, static_cast<uint32_t>(renderGraph->getRHI()->GetProperties().uniformBufferAlignment));
    return ((uniformSize + alignment - 1u) / alignment) * alignment;
  }

  void createRsmMaterialResources()
  {
    if (rsmMaterialBuffer_.isValid())
    {
      uploadRsmMaterialResources();
      uploadRsmVirtualTextureResources();
      return;
    }

    const auto &materialEntries = scene_.getVirtualTextureSystem().getMaterialEntries();
    uint32_t materialCount = static_cast<uint32_t>(materialEntries.size());
    for (const uint32_t materialId : scene_.getMaterialManager().getRegisteredMaterialIds())
    {
      materialCount = std::max(materialCount, materialId + 1u);
    }
    materialCount = std::max(materialCount, 1u);

    std::vector<RsmMaterialGPU> materials(materialCount);
    for (uint32_t materialIndex = 0u; materialIndex < materialCount; ++materialIndex)
    {
      RsmMaterialGPU &runtimeMaterial = materials[materialIndex];
      runtimeMaterial.materialId = materialIndex;
      const VirtualTextureSystem::MaterialEntryGPU emptyMaterialEntry{};
      const VirtualTextureSystem::MaterialEntryGPU &materialEntry =
          materialIndex < materialEntries.size() ? materialEntries[materialIndex] : emptyMaterialEntry;
      const Material *material = scene_.getMaterialManager().getMaterial(materialIndex);
      runtimeMaterial.baseColorTextureId = resolveBaseColorMaterialTextureId(material, materialEntry);
      runtimeMaterial.normalTextureId = resolveNormalMaterialTextureId(material, materialEntry);
      if (material != nullptr)
      {
        const float *baseColor = material->getBaseColorFactor();
        runtimeMaterial.baseColorOpacity[0] = baseColor[0];
        runtimeMaterial.baseColorOpacity[1] = baseColor[1];
        runtimeMaterial.baseColorOpacity[2] = baseColor[2];
        runtimeMaterial.baseColorOpacity[3] = baseColor[3];
        runtimeMaterial.pbrFactors[0] = material->getRoughnessFactor();
        runtimeMaterial.pbrFactors[1] = material->getMetallicFactor();
        runtimeMaterial.pbrFactors[2] = material->getOpacity();
        const float *emissive = material->getEmissiveFactor();
        runtimeMaterial.emissiveFactorAndStrength[0] = emissive[0];
        runtimeMaterial.emissiveFactorAndStrength[1] = emissive[1];
        runtimeMaterial.emissiveFactorAndStrength[2] = emissive[2];
        runtimeMaterial.emissiveFactorAndStrength[3] = material->getEmissiveStrength();
      }
    }

    rsmMaterialBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_RsmMaterials.buffer",
            .size = materials.size() * sizeof(RsmMaterialGPU),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    renderGraph->bufferWrite(rsmMaterialBuffer_, 0, materials.size() * sizeof(RsmMaterialGPU), materials.data());
    createRsmVirtualTextureResources();
  }

  void uploadRsmMaterialResources()
  {
    const auto &materialEntries = scene_.getVirtualTextureSystem().getMaterialEntries();
    uint32_t materialCount = static_cast<uint32_t>(materialEntries.size());
    for (const uint32_t materialId : scene_.getMaterialManager().getRegisteredMaterialIds())
    {
      materialCount = std::max(materialCount, materialId + 1u);
    }
    materialCount = std::max(materialCount, 1u);

    std::vector<RsmMaterialGPU> materials(materialCount);
    for (uint32_t materialIndex = 0u; materialIndex < materialCount; ++materialIndex)
    {
      RsmMaterialGPU &runtimeMaterial = materials[materialIndex];
      runtimeMaterial.materialId = materialIndex;
      const VirtualTextureSystem::MaterialEntryGPU emptyMaterialEntry{};
      const VirtualTextureSystem::MaterialEntryGPU &materialEntry =
          materialIndex < materialEntries.size() ? materialEntries[materialIndex] : emptyMaterialEntry;
      const Material *material = scene_.getMaterialManager().getMaterial(materialIndex);
      runtimeMaterial.baseColorTextureId = resolveBaseColorMaterialTextureId(material, materialEntry);
      runtimeMaterial.normalTextureId = resolveNormalMaterialTextureId(material, materialEntry);
      if (material != nullptr)
      {
        const float *baseColor = material->getBaseColorFactor();
        runtimeMaterial.baseColorOpacity[0] = baseColor[0];
        runtimeMaterial.baseColorOpacity[1] = baseColor[1];
        runtimeMaterial.baseColorOpacity[2] = baseColor[2];
        runtimeMaterial.baseColorOpacity[3] = baseColor[3];
        runtimeMaterial.pbrFactors[0] = material->getRoughnessFactor();
        runtimeMaterial.pbrFactors[1] = material->getMetallicFactor();
        runtimeMaterial.pbrFactors[2] = material->getOpacity();
        const float *emissive = material->getEmissiveFactor();
        runtimeMaterial.emissiveFactorAndStrength[0] = emissive[0];
        runtimeMaterial.emissiveFactorAndStrength[1] = emissive[1];
        runtimeMaterial.emissiveFactorAndStrength[2] = emissive[2];
        runtimeMaterial.emissiveFactorAndStrength[3] = material->getEmissiveStrength();
      }
    }

    const uint64_t materialBytes = materials.size() * sizeof(RsmMaterialGPU);
    if (materialBytes > renderGraph->getBufferSize(rsmMaterialBuffer_))
    {
      throw std::runtime_error("RSM material buffer outgrew its allocation; rebuild the render graph");
    }
    renderGraph->bufferWrite(rsmMaterialBuffer_, 0, materialBytes, materials.data());

  }

  void createRsmVirtualTextureResources()
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();

    rsmVtAtlasInfoBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_RsmVtAtlasInfo.buffer",
            .size = sizeof(VirtualTextureSystem::AtlasInfoGPU),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    rsmTextureEntriesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_RsmTextureEntries.buffer",
            .size = std::max<uint64_t>(sizeof(VirtualTextureSystem::TextureEntryGPU), textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU)),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    rsmVtPageTableBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_RsmVirtualTexturePageTable.buffer",
            .size = std::max<uint64_t>(sizeof(VirtualTextureSystem::PageTableEntryGPU), pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU)),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    rsmVtPhysicalPagesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_RsmVirtualTexturePhysicalPages.buffer",
            .size = std::max<uint64_t>(sizeof(uint32_t), physicalPages.size() * sizeof(uint32_t)),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });

    uploadRsmVirtualTextureResources();
  }

  void uploadRsmVirtualTextureResources()
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &atlasInfo = virtualTextures.getAtlasInfo();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();

    renderGraph->bufferWrite(rsmVtAtlasInfoBuffer_, 0, sizeof(VirtualTextureSystem::AtlasInfoGPU), const_cast<VirtualTextureSystem::AtlasInfoGPU *>(&atlasInfo));

    const uint64_t textureEntryBytes = textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU);
    if (textureEntryBytes > renderGraph->getBufferSize(rsmTextureEntriesBuffer_))
    {
      throw std::runtime_error("RSM virtual texture entry buffer outgrew its allocation; rebuild the render graph");
    }
    if (!textureEntries.empty())
    {
      renderGraph->bufferWrite(rsmTextureEntriesBuffer_, 0, textureEntryBytes, const_cast<VirtualTextureSystem::TextureEntryGPU *>(textureEntries.data()));
    }

    const uint64_t pageTableBytes = pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU);
    if (pageTableBytes > renderGraph->getBufferSize(rsmVtPageTableBuffer_))
    {
      throw std::runtime_error("RSM virtual texture page table buffer outgrew its allocation; rebuild the render graph");
    }
    if (!pageTableEntries.empty())
    {
      renderGraph->bufferWrite(rsmVtPageTableBuffer_, 0, pageTableBytes, const_cast<VirtualTextureSystem::PageTableEntryGPU *>(pageTableEntries.data()));
    }

    const uint64_t physicalPageBytes = physicalPages.size() * sizeof(uint32_t);
    if (physicalPageBytes > renderGraph->getBufferSize(rsmVtPhysicalPagesBuffer_))
    {
      throw std::runtime_error("RSM virtual texture physical page buffer outgrew its allocation; rebuild the render graph");
    }
    if (!physicalPages.empty())
    {
      renderGraph->bufferWrite(rsmVtPhysicalPagesBuffer_, 0, physicalPageBytes, const_cast<uint32_t *>(physicalPages.data()));
    }
  }

  static rendering::TextureView makeHPBSampledView(rendering::Texture texture, uint32_t baseMip, uint32_t mipCount)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = baseMip,
      .layerCount = 1,
      .levelCount = mipCount,
    };
  }

  static rendering::TextureView makeHPBStorageView(rendering::Texture texture, uint32_t baseMip)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = baseMip,
      .layerCount = 1,
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

  static rendering::TextureView makeScratchAttachmentView(rendering::Texture texture, rendering::Format format)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0,
      .flags = rendering::GetImageAspectFlags(format),
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

  static rendering::TextureView makeAtlasDepthAttachmentView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Depth,
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
      {.name = "cascadeMatrices", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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

    rendering::GroupInfo hpbGroupInfo{};
    hpbGroupInfo.name = "group0";
    hpbGroupInfo.buffers = {
      {.binding = 0, .bufferView = {.buffer = hpbUniformBuffer_, .offset = 0, .size = sizeof(HPBUniforms), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 1, .bufferView = {.buffer = manager_.getAllocatorCountersBuffer(), .offset = 0, .size = manager_.getAllocatorCountersBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 2, .bufferView = {.buffer = manager_.getAllocationRequestsBuffer(), .offset = 0, .size = manager_.getAllocationRequestsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 3, .bufferView = {.buffer = manager_.getFutureAllocationRequestsBuffer(), .offset = 0, .size = manager_.getFutureAllocationRequestsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
      {.binding = 10, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
    };
    hpbGroupInfo.storageTextures.reserve(kMaxHPBMipLevels);
    const uint32_t fallbackMip = std::max(0u, uniforms.hpbMipCount - 1u);
    for (uint32_t mipIndex = 0u; mipIndex < kMaxHPBMipLevels; ++mipIndex)
    {
      const uint32_t boundMip = std::min(mipIndex, fallbackMip);
      hpbGroupInfo.storageTextures.push_back({
          .binding = 4u + mipIndex,
          .textureView = makeHPBStorageView(manager_.getHierarchicalPageBoundsTexture(), boundMip),
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
      .maxDirtyPagesPerFrame = std::min<uint32_t>(
          std::max<uint32_t>(1u, manager_.getAllocationRequestCapacity()),
          manager_.getVirtualPageTableResolution() * manager_.getVirtualPageTableResolution() * std::max(1u, manager_.getActiveLayerCount())),
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
            .size = static_cast<uint64_t>(visibleClusterDrawCapacity_) * sizeof(uint32_t) * getShadowVisibleClusterInfoWordCount(),
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
    globalDirtyPageCountBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_GlobalDirtyPageCount.buffer",
            .size = sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    globalDirtyPageListBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_GlobalDirtyPageList.buffer",
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
                    {.name = "globalDirtyPageCount", .binding = 22, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "globalDirtyPageList", .binding = 23, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "virtualPageState", .binding = 24, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "hpbTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    const char *cullingSpirv = nullptr;
    if (usesClipSpaceAtlasDepthRaster())
    {
      if (usesImperfectShadowMapRaster())
      {
        cullingSpirv = "assets/shaders/spirv/vsm-culling-multipass-ism-per-page-draw_indirect_count_disabled.spirv";
      }
      else
      {
        cullingSpirv = "assets/shaders/spirv/vsm-culling-multipass-per-page-draw_indirect_count_disabled.spirv";
      }
    }
    else
    {
      cullingSpirv = "assets/shaders/spirv/vsm-culling-multipass-draw_indirect_count_disabled.spirv";
    }
    cullingShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Culling.shader",
            .layout = cullingLayout_,
            .src = os::io::readRelativeFile(cullingSpirv),
            .type = rendering::ShaderType::SpirV,
        });

    initCullingPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "init_sync", .layout = cullingLayout_, .name = passName + "_InitCulling.pipeline", .shader = cullingShader_});
    setupRootNodesPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "setup_root_nodes", .layout = cullingLayout_, .name = passName + "_SetupRootNodes.pipeline", .shader = cullingShader_});
    prepareIndirectDispatchPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "prepare_indirect_dispatch", .layout = cullingLayout_, .name = passName + "_PrepareIndirectDispatch.pipeline", .shader = cullingShader_});
    processHierarchyNodesPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "process_hierarchy_nodes", .layout = cullingLayout_, .name = passName + "_ProcessHierarchyNodes.pipeline", .shader = cullingShader_});
    prepareClusterDispatchPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "prepare_cluster_dispatch", .layout = cullingLayout_, .name = passName + "_PrepareClusterDispatch.pipeline", .shader = cullingShader_});
    processClustersPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "process_clusters", .layout = cullingLayout_, .name = passName + "_ProcessClusters.pipeline", .shader = cullingShader_});
    prepareShadowDrawIndirectArgsPipeline_ =
        renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "prepare_shadow_draw_indirect_args", .layout = cullingLayout_, .name = passName + "_PrepareShadowDrawIndirectArgs.pipeline", .shader = cullingShader_});

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
                          {.binding = 22, .bufferView = {.buffer = globalDirtyPageCountBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageCountBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 23, .bufferView = {.buffer = globalDirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 24, .bufferView = {.buffer = manager_.getVirtualPageStateBuffer(), .offset = 0, .size = manager_.getVirtualPageStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                      },
                      .textures = {
                          {.binding = 14, .textureView = makeHPBSampledView(manager_.getHierarchicalPageBoundsTexture(), 0u, manager_.getHierarchicalPageBoundsMipCount())},
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
    const rendering::Format scratchTextureFormat = getScratchTextureFormat();
    if (usesClipSpaceAtlasDepthRaster())
    {
      (void)scratchResolution;
      (void)scratchTextureFormat;
      return;
    }

    for (uint32_t scratchIndex = 0u; scratchIndex < getScratchClipmapCount(); ++scratchIndex)
    {
      scratchClipmapTextures_.push_back(createFrameLocalTexture(
          rendering::TextureInfo{
              .name = passName + "_ScratchClipmap" + std::to_string(scratchIndex) + ".texture",
              .format = scratchTextureFormat,
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
    const uint32_t totalLayers = std::max(1u, usesClipSpaceAtlasDepthRaster() ? 1u : manager_.getActiveLayerCount());
    pageStencilUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_PageStencilUniforms.buffer",
            .size = static_cast<uint64_t>(pageStencilUniformStride_) * totalLayers,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    if (!usesClipSpaceAtlasDepthRaster())
    {
      pageStencilDrawIndirectBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_PageStencilIndirect.buffer",
              .size = static_cast<uint64_t>(totalLayers) * 4u * sizeof(uint32_t),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
          });
    }
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

    if (usesClipSpaceAtlasDepthRaster())
    {
      pageStencilLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_PageAtlasClear.layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers = {
                      {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                      {.name = "virtualPageTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                      {.name = "globalDirtyPageList", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                  },
              }},
          });
    }
    else
    {
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
    }

    pageStencilBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = pageStencilLayout_,
            .name = passName + "_PageStencilBindingGroups",
            .groups = {
                rendering::GroupInfo{
                      .name = "group0",
                      .buffers = {
                          {.binding = 0, .bufferView = {.buffer = pageStencilUniformBuffer_, .offset = 0, .size = pageStencilUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                          usesClipSpaceAtlasDepthRaster()
                              ? rendering::BindingBuffer{.binding = 1, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}}
                              : rendering::BindingBuffer{.binding = 1, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          usesClipSpaceAtlasDepthRaster()
                              ? rendering::BindingBuffer{.binding = 2, .bufferView = {.buffer = globalDirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}}
                              : rendering::BindingBuffer{.binding = 2, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                      },
                  },
            },
        });

    pageStencilVertexShader_ = renderGraph->createShader(rendering::ShaderInfo{
        .name = passName + "_PageStencilVS.shader",
        .layout = pageStencilLayout_,
        .src = os::io::readRelativeFile(usesClipSpaceAtlasDepthRaster()
                                             ? "assets/shaders/spirv/vsm-scratch-stencil-vs-per-page-atlas-depth.spirv"
                                             : "assets/shaders/spirv/vsm-scratch-stencil-vs.spirv"),
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
        .format = getScratchTextureFormat(),
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
    clearPipelineInfo.name = passName + (usesClipSpaceAtlasDepthRaster() ? "_AtlasDepthPageClear.pipeline" : "_ScratchClear.pipeline");
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
      .format = getScratchTextureFormat(),
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = true,
      .depthWriteEnabled = true,
      .comparison = rendering::ComparisonOp::ComparisonOp_Always,
    };
    clearScratchPipeline_ = renderGraph->createGraphicsPipeline(clearPipelineInfo);
    if (shouldRegisterDirtyPageStencilPasses())
    {
      pageStencilMarkPipeline_ = makePageStencilPipeline("PageStencilMark", 1u);
      pageStencilResetPipeline_ = makePageStencilPipeline("PageStencilReset", 0u);
    }
  }

  void createScratchDrawResources()
  {
    if (usesClipSpaceAtlasDepthRaster() && (renderGraph->getRHI()->getFeatures() & rendering::DeviceFeatures::DeviceFeatures_ClipDistances) == 0u)
    {
      throw std::runtime_error("VirtualShadowMapDrawPass clip-space atlas-depth raster mode requires clip distance support");
    }

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

    if (usesClipSpaceAtlasDepthRaster())
    {
      rendering::BindingGroupLayout perPageGroupLayout{};
      perPageGroupLayout.buffers = {
          {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex | rendering::BindingVisibility::BindingVisibility_Fragment},
          {.name = "instances", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "scenePageTable", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "scenePagesBuffer", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "shadowVisibleClusterInfos", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "virtualPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "cascadeStates", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "cascadeMatrices", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "meshPartTransforms", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "decodedClusterGeometryBuffer", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "decodedVertexBuffer", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
          {.name = "decodedIndexBuffer", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
      };
      if (usesReflexiveShadowMapRaster())
      {
        perPageGroupLayout.buffers.insert(
            perPageGroupLayout.buffers.end(),
            {
                {.name = "directionalLights", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "rsmMaterials", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "rsmVtAtlasInfo", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "rsmTextureEntries", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "rsmVtPageTable", .binding = 20, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "rsmVtPhysicalPages", .binding = 21, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                 .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
            });
        perPageGroupLayout.storageTextures = {
            {.name = "rsmFluxAtlasTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
            {.name = "rsmNormalAtlasTexture", .binding = 15, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
            {.name = "rsmPositionAtlasTexture", .binding = 16, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
        };
      }

      drawScratchLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_ScratchDrawPerPage.layout",
              .groups = {perPageGroupLayout},
          });

      rendering::GroupInfo perPageGroupInfo{};
      perPageGroupInfo.name = "group0";
      perPageGroupInfo.buffers = {
          {.binding = 0, .bufferView = {.buffer = drawScratchUniformBuffer_, .offset = 0, .size = drawScratchUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 1, .bufferView = {.buffer = scene_.instanceBuffer, .offset = 0, .size = scene_.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData), .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 2, .bufferView = {.buffer = scene_.pageTableBuffer, .offset = 0, .size = scene_.pagesTableBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 3, .bufferView = {.buffer = scene_.pagesBuffer, .offset = 0, .size = scene_.pagesBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 4, .bufferView = {.buffer = shadowVisibleClusterInfosBuffer_, .offset = 0, .size = renderGraph->getBufferSize(shadowVisibleClusterInfosBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 5, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 6, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 7, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 8, .bufferView = {.buffer = scene_.meshPartTransformsBuffer, .offset = 0, .size = scene_.meshPartTransformsBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 10, .bufferView = {.buffer = scene_.decodedClusterGeometryBuffer, .offset = 0, .size = scene_.decodedClusterGeometryBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 11, .bufferView = {.buffer = scene_.decodedVertexBuffer, .offset = 0, .size = scene_.decodedVertexBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
          {.binding = 12, .bufferView = {.buffer = scene_.decodedIndexBuffer, .offset = 0, .size = scene_.decodedIndexBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
      };
      if (usesReflexiveShadowMapRaster())
      {
        perPageGroupInfo.buffers.insert(
            perPageGroupInfo.buffers.end(),
            {
                {.binding = 13, .bufferView = {.buffer = manager_.getDirectionalLightsBuffer(), .offset = 0, .size = manager_.getDirectionalLightsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                {.binding = 17, .bufferView = {.buffer = rsmMaterialBuffer_, .offset = 0, .size = renderGraph->getBufferSize(rsmMaterialBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                {.binding = 18, .bufferView = {.buffer = rsmVtAtlasInfoBuffer_, .offset = 0, .size = renderGraph->getBufferSize(rsmVtAtlasInfoBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                {.binding = 19, .bufferView = {.buffer = rsmTextureEntriesBuffer_, .offset = 0, .size = renderGraph->getBufferSize(rsmTextureEntriesBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                {.binding = 20, .bufferView = {.buffer = rsmVtPageTableBuffer_, .offset = 0, .size = renderGraph->getBufferSize(rsmVtPageTableBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                {.binding = 21, .bufferView = {.buffer = rsmVtPhysicalPagesBuffer_, .offset = 0, .size = renderGraph->getBufferSize(rsmVtPhysicalPagesBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
            });
        perPageGroupInfo.storageTextures = {
            {.binding = 14, .textureView = makeAtlasStorageView(manager_.getRsmFluxAtlasTexture())},
            {.binding = 15, .textureView = makeAtlasStorageView(manager_.getRsmNormalAtlasTexture())},
            {.binding = 16, .textureView = makeAtlasStorageView(manager_.getRsmPositionAtlasTexture())},
        };
      }

      drawScratchBindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = drawScratchLayout_,
              .name = passName + "_ScratchDrawPerPageBindingGroups",
              .groups = {perPageGroupInfo},
          });

      const char *meshletSpirv = nullptr;
      if (usesImperfectShadowMapRaster())
      {
        meshletSpirv = "assets/shaders/spirv/vsm-meshlet-vs-ism-per-page-draw_indirect_count_disabled.spirv";
      }
      else
      {
        meshletSpirv = usesReflexiveShadowMapRaster()
                           ? "assets/shaders/spirv/vsm-meshlet-vs-per-page-rsm-draw_indirect_count_disabled.spirv"
                           : "assets/shaders/spirv/vsm-meshlet-vs-per-page-draw_indirect_count_disabled.spirv";
      }
      drawScratchVertexShader_ = renderGraph->createShader(rendering::ShaderInfo{
          .name = passName + "_ScratchDrawPerPageVS.shader",
          .layout = drawScratchLayout_,
          .src = os::io::readRelativeFile(meshletSpirv),
          .type = rendering::ShaderType::SpirV,
      });
      rendering::GraphicsPipelineInfo pipelineInfo{};
      pipelineInfo.name = passName + "_AtlasDepthDrawPerPage.pipeline";
      pipelineInfo.layout = drawScratchLayout_;
      pipelineInfo.vertexStage.vertexLayoutElements = {};
      pipelineInfo.vertexStage.vertexShader = drawScratchVertexShader_;
      pipelineInfo.vertexStage.shaderEntry = "vs_main";
      pipelineInfo.vertexStage.cullType = rendering::CullMode::Back;
      pipelineInfo.vertexStage.winding = rendering::WindingOrder::CW;
      pipelineInfo.vertexStage.primitiveType = usesImperfectShadowMapRaster() ? rendering::PrimitiveType_Points
                                                                              : rendering::PrimitiveType_Triangles;
      pipelineInfo.fragmentStage.shaderEntry = usesReflexiveShadowMapRaster() ? "fs_main" : "";
      pipelineInfo.fragmentStage.fragmentShader = usesReflexiveShadowMapRaster() ? drawScratchVertexShader_ : rendering::Shader{};
      pipelineInfo.fragmentStage.depthAttatchment = {
        .enabled = true,
        .format = rendering::Format::Format_Depth32Float,
        .loadOp = rendering::LoadOp::LoadOp_Load,
        .storeOp = rendering::StoreOp::StoreOp_Store,
        .depthTestEnabled = true,
        .depthWriteEnabled = true,
        .comparison = manager_.getSettings().reverseZ ? rendering::ComparisonOp::ComparisonOp_GreaterOrEqual
                                                      : rendering::ComparisonOp::ComparisonOp_LessOrEqual,
      };
      drawScratchPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
      return;
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
                    {.name = "decodedClusterGeometryBuffer", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "decodedVertexBuffer", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    {.name = "decodedIndexBuffer", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
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
                        {.binding = 10, .bufferView = {.buffer = scene_.decodedClusterGeometryBuffer, .offset = 0, .size = scene_.decodedClusterGeometryBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 11, .bufferView = {.buffer = scene_.decodedVertexBuffer, .offset = 0, .size = scene_.decodedVertexBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 12, .bufferView = {.buffer = scene_.decodedIndexBuffer, .offset = 0, .size = scene_.decodedIndexBufferSize, .access = rendering::AccessPattern::SHADER_READ}},
                    },
                },
            },
        });

    const char *meshletSpirv = "assets/shaders/spirv/vsm-meshlet-vs-draw_indirect_count_disabled.spirv";
    drawScratchVertexShader_ = renderGraph->createShader(rendering::ShaderInfo{
        .name = passName + "_ScratchDrawVS.shader",
        .layout = drawScratchLayout_,
        .src = os::io::readRelativeFile(meshletSpirv),
        .type = rendering::ShaderType::SpirV,
    });

    rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_ScratchDraw.pipeline";
    pipelineInfo.layout = drawScratchLayout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = drawScratchVertexShader_;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = rendering::CullMode::Back;
    pipelineInfo.vertexStage.winding = rendering::WindingOrder::CW;
    pipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.shaderEntry = "";
    pipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = getScratchTextureFormat(),
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .depthTestEnabled = true,
      .depthWriteEnabled = true,
      .comparison = manager_.getSettings().reverseZ ? rendering::ComparisonOp::ComparisonOp_GreaterOrEqual
                                                    : rendering::ComparisonOp::ComparisonOp_LessOrEqual,
    };
    drawScratchPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);

    if (shouldRegisterDirtyPageStencilPasses())
    {
      pipelineInfo.name = passName + "_ScratchDrawStencil.pipeline";
      pipelineInfo.fragmentStage.depthAttatchment = {
        .enabled = true,
        .format = getScratchTextureFormat(),
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
  }

  void createResolveResources()
  {
    resolvePagesUniformStride_ = getAlignedUniformSize(sizeof(ResolveUniforms));
    const uint32_t totalLayers = std::max(1u, usesClipSpaceAtlasDepthRaster() ? 1u : manager_.getActiveLayerCount());
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

    if (usesClipSpaceAtlasDepthRaster())
    {
      resolvePagesLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_ResolvePerPage.layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers = {
                      {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "virtualPageTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "cascadeStates", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "dirtyPageCounts", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "dirtyPageList", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "cascadeMatrices", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "globalDirtyPageCount", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "globalDirtyPageList", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "resolveDispatchArgs", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "pageOpDispatchArgs", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "drawCounters", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "pageClearDrawIndirectArgs", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                       .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
                  .storageTextures = usesReflexiveShadowMapRaster()
                                         ? std::vector<rendering::BindingGroupLayoutStorageTextureEntry>{
                                               {.name = "rsmFluxAtlasTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                                               {.name = "rsmNormalAtlasTexture", .binding = 15, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                                               {.name = "rsmPositionAtlasTexture", .binding = 16, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                                           }
                                         : std::vector<rendering::BindingGroupLayoutStorageTextureEntry>{},
              }},
          });

      resolvePagesBindingGroups_.push_back(renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = resolvePagesLayout_,
              .name = passName + "_ResolvePerPageBindingGroups",
              .groups = {
                  rendering::GroupInfo{
                      .name = "group0",
                      .buffers = {
                          {.binding = 0, .bufferView = {.buffer = resolvePagesUniformBuffer_, .offset = 0, .size = resolvePagesUniformStride_, .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 1, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 2, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 3, .bufferView = {.buffer = dirtyPageCountsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageCountsBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 4, .bufferView = {.buffer = dirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(dirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 13, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 5, .bufferView = {.buffer = globalDirtyPageCountBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageCountBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 6, .bufferView = {.buffer = globalDirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 7, .bufferView = {.buffer = resolveDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(resolveDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 8, .bufferView = {.buffer = pageOpDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageOpDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 9, .bufferView = {.buffer = cullingStatisticsBuffer_, .offset = 0, .size = kCullingCounterCount * sizeof(uint32_t), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 10, .bufferView = {.buffer = pageClearDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageClearDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                      },
                      .storageTextures = usesReflexiveShadowMapRaster()
                                             ? std::vector<rendering::BindingStorageTextureInfo>{
                                                   {.binding = 14, .textureView = makeAtlasStorageView(manager_.getRsmFluxAtlasTexture())},
                                                   {.binding = 15, .textureView = makeAtlasStorageView(manager_.getRsmNormalAtlasTexture())},
                                                   {.binding = 16, .textureView = makeAtlasStorageView(manager_.getRsmPositionAtlasTexture())},
                                               }
                                             : std::vector<rendering::BindingStorageTextureInfo>{},
                  },
              },
          }));

      resolvePagesShader_ = renderGraph->createShader(
          rendering::ShaderInfo{
              .name = passName + "_ResolvePerPage.shader",
              .layout = resolvePagesLayout_,
              .src = os::io::readRelativeFile(usesReflexiveShadowMapRaster()
                                                  ? "assets/shaders/spirv/vsm-layer-resolve-per-page-rsm.spirv"
                                                  : "assets/shaders/spirv/vsm-layer-resolve-per-page.spirv"),
              .type = rendering::ShaderType::SpirV,
          });
      prepareResolveDispatchPipeline_ = renderGraph->createComputePipeline(
          rendering::ComputePipelineInfo{
              .entry = "prepare_dispatch",
              .layout = resolvePagesLayout_,
              .name = passName + "_ResolvePerPagePrepareDispatch.pipeline",
              .shader = resolvePagesShader_,
          });
      if (usesReflexiveShadowMapRaster())
      {
        clearRsmPagesPipeline_ = renderGraph->createComputePipeline(
            rendering::ComputePipelineInfo{
                .entry = "clear_rsm_pages",
                .layout = resolvePagesLayout_,
                .name = passName + "_ResolvePerPageClearRsm.pipeline",
                .shader = resolvePagesShader_,
            });
      }
      resolvePagesPipeline_ = renderGraph->createComputePipeline(
          rendering::ComputePipelineInfo{
              .entry = "main",
              .layout = resolvePagesLayout_,
              .name = passName + "_ResolvePerPage.pipeline",
              .shader = resolvePagesShader_,
          });
      return;
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
                    {.name = "cascadeMatrices", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "globalDirtyPageCount", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "globalDirtyPageList", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "resolveDispatchArgs", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageStencilDrawIndirectArgs", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageClearDrawIndirectArgs", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "pageOpDispatchArgs", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "scratchTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "atlasTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                          {.binding = 13, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 5, .bufferView = {.buffer = globalDirtyPageCountBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageCountBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 6, .bufferView = {.buffer = globalDirtyPageListBuffer_, .offset = 0, .size = renderGraph->getBufferSize(globalDirtyPageListBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 7, .bufferView = {.buffer = resolveDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(resolveDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 8, .bufferView = {.buffer = pageStencilDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageStencilDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 9, .bufferView = {.buffer = pageClearDrawIndirectBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageClearDrawIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                          {.binding = 12, .bufferView = {.buffer = pageOpDispatchArgsBuffer_, .offset = 0, .size = renderGraph->getBufferSize(pageOpDispatchArgsBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                      },
                      .textures = {
                          {.binding = 10, .textureView = makeScratchSampledView(scratchClipmapTextures_[scratchIndex])},
                      },
                      .storageTextures = {
                          {.binding = 11, .textureView = makeAtlasStorageView(manager_.getShadowAtlasTexture())},
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
    if (usesClipSpaceAtlasDepthRaster())
    {
      return;
    }

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
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .maxVisibleClusterDrawsPerLayer = visibleClusterDrawCapacityPerLayer_,
      .rsmAtlasResolution = manager_.getRsmAtlasResolution(),
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
      .physicalAtlasResolution = manager_.getSettings().physicalAtlasResolution,
      .reverseZ = manager_.getSettings().reverseZ ? 1u : 0u,
      .rsmAtlasResolution = manager_.getRsmAtlasResolution(),
    };
    renderGraph->bufferWrite(resolvePagesUniformBuffer_, offset, sizeof(uniforms), const_cast<ResolveUniforms *>(&uniforms));
  }

  void writePageStencilUniforms(uint32_t currentLayer, uint32_t offset)
  {
    const StencilPageUniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .currentLayer = currentLayer,
      .scratchResolution = usesClipSpaceAtlasDepthRaster() ? manager_.getSettings().physicalAtlasResolution : getScratchResolution(),
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
    const uint32_t activeLayers = std::max(1u, manager_.getActiveLayerCount());
    const uint32_t drawIndirectWordCount = activeLayers * 4u;
    const uint32_t totalPages = manager_.getVirtualPageTableResolution() * manager_.getVirtualPageTableResolution() * activeLayers;
    const uint32_t initItemCount = std::max(std::max(std::max(drawIndirectWordCount, kCullingCounterCount), totalPages), activeLayers);
    const uint32_t initDispatchX = std::max(1u, (initItemCount + kWorkgroupSize - 1u) / kWorkgroupSize);
    const uint32_t rootCount = static_cast<uint32_t>(scene_.getInstanceCount()) * activeLayers;
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

    commandBuffer.cmdBindComputePipeline(prepareShadowDrawIndirectArgsPipeline_);
    commandBuffer.cmdBindBindingGroups(cullingBindingGroupPingA_, nullptr, 0);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
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
      .view = makeScratchAttachmentView(scratchTexture, getScratchTextureFormat()),
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
    if (activeLayers == 0u)
    {
      return;
    }

    const uint32_t scratchResolution = getScratchResolution();
    const uint32_t stride = 4u * sizeof(uint32_t);

    if (usesClipSpaceAtlasDepthRaster())
    {
      commandBuffer.cmdBindComputePipeline(prepareResolveDispatchPipeline_);
      uint32_t prepareResolveDynamicOffset = 0u;
      commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[0], &prepareResolveDynamicOffset, 1);
      commandBuffer.cmdDispatch(1u, 1u, 1u);

      if (usesReflexiveShadowMapRaster())
      {
        commandBuffer.cmdBindComputePipeline(clearRsmPagesPipeline_);
        uint32_t clearRsmDynamicOffset = 0u;
        commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[0], &clearRsmDynamicOffset, 1);
        commandBuffer.cmdDispatchIndirect(pageOpDispatchArgsBuffer_, 0u);
      }

      const uint32_t atlasResolution = manager_.getSettings().physicalAtlasResolution;
      rendering::RenderPassInfo renderPass{};
      renderPass.name = passName + "_AtlasDepthPerPageRenderPass";
      renderPass.scissor = rendering::Rect2D(0, 0, atlasResolution, atlasResolution);
      renderPass.viewport = rendering::Viewport(atlasResolution, atlasResolution);
      renderPass.depthStencilAttachment = {
        .enabled = true,
        .name = manager_.getShadowAtlasTexture().name + "_DepthStencilView",
        .view = makeAtlasDepthAttachmentView(manager_.getShadowAtlasTexture()),
        .clearDepth = manager_.getSettings().reverseZ ? 0.0f : 1.0f,
        .clearStencil = 0u,
      };

      commandBuffer.cmdBindGraphicsPipeline(clearScratchPipeline_);
      commandBuffer.cmdBeginRenderPass(renderPass);

      uint32_t pageClearDynamicOffset = 0u;
      commandBuffer.cmdBindBindingGroups(pageStencilBindingGroups_, &pageClearDynamicOffset, 1);
      commandBuffer.cmdDrawIndirect(
          rendering::BufferView{
              .buffer = pageClearDrawIndirectBuffer_,
              .offset = 0u,
              .size = renderGraph->getBufferSize(pageClearDrawIndirectBuffer_),
              .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
          },
          0u,
          1u,
          4u * sizeof(uint32_t));

      commandBuffer.cmdBindGraphicsPipeline(drawScratchPipeline_);
      uint32_t drawDynamicOffset = 0u;
      commandBuffer.cmdBindBindingGroups(drawScratchBindingGroups_, &drawDynamicOffset, 1);

      commandBuffer.cmdDrawIndirect(
          rendering::BufferView{
              .buffer = shadowDrawIndirectBuffer_,
              .offset = 0u,
              .size = renderGraph->getBufferSize(shadowDrawIndirectBuffer_),
              .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
          },
          0u,
          1u,
          stride);

      commandBuffer.cmdEndRenderPass();

      commandBuffer.cmdBindComputePipeline(resolvePagesPipeline_);
      uint32_t resolveDynamicOffset = 0u;
      commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[0], &resolveDynamicOffset, 1);
      commandBuffer.cmdDispatchIndirect(resolveDispatchArgsBuffer_, 0u);
      return;
    }

    commandBuffer.cmdBindComputePipeline(prepareResolveDispatchPipeline_);
    uint32_t prepareResolveDynamicOffset = 0u;
    commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[0], &prepareResolveDynamicOffset, 1);
    commandBuffer.cmdDispatch(std::max(1u, (activeLayers + kResolveWorkgroupSize - 1u) / kResolveWorkgroupSize), 1u, 1u);

    if (scratchClipmapTextures_.empty())
    {
      return;
    }

    for (uint32_t layerIndex = 0u; layerIndex < activeLayers; ++layerIndex)
    {
      const uint32_t scratchIndex = layerIndex % scratchClipmapTextures_.size();
      const uint32_t drawUniformOffset = layerIndex * drawScratchUniformStride_;
      const uint32_t resolveUniformOffset = layerIndex * resolvePagesUniformStride_;
      const uint32_t indirectOffset = layerIndex * visibleClusterDrawCapacityPerLayer_ * stride;
      const uint64_t dispatchOffset = static_cast<uint64_t>(layerIndex) * sizeof(uint32_t) * 3u;
      const uint64_t stencilIndirectOffset = static_cast<uint64_t>(layerIndex) * 4u * sizeof(uint32_t);

      recordPageStencilPass(commandBuffer, clearScratchPipeline_, scratchClipmapTextures_[scratchIndex], pageClearDrawIndirectBuffer_, layerIndex, scratchResolution, stencilIndirectOffset);
      if (shouldRegisterDirtyPageStencilPasses())
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
        .view = makeScratchAttachmentView(scratchClipmapTextures_[scratchIndex], getScratchTextureFormat()),
        .clearDepth = manager_.getSettings().reverseZ ? 0.0f : 1.0f,
        .clearStencil = 0u,
      };

      commandBuffer.cmdBindGraphicsPipeline(shouldRegisterDirtyPageStencilPasses() ? drawScratchStencilPipeline_ : drawScratchPipeline_);
      commandBuffer.cmdBeginRenderPass(renderPass);
      uint32_t drawDynamicOffset = drawUniformOffset;
      commandBuffer.cmdBindBindingGroups(drawScratchBindingGroups_, &drawDynamicOffset, 1);

      commandBuffer.cmdDrawIndirect(
          rendering::BufferView{
              .buffer = shadowDrawIndirectBuffer_,
              .offset = indirectOffset,
              .size = renderGraph->getBufferSize(shadowDrawIndirectBuffer_) - indirectOffset,
              .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
          },
          indirectOffset,
          1u,
          stride);

      commandBuffer.cmdEndRenderPass();

      commandBuffer.cmdBindComputePipeline(resolvePagesPipeline_);
      uint32_t resolveDynamicOffset = resolveUniformOffset;
      commandBuffer.cmdBindBindingGroups(resolvePagesBindingGroups_[scratchIndex], &resolveDynamicOffset, 1);
      commandBuffer.cmdDispatchIndirect(pageOpDispatchArgsBuffer_, dispatchOffset);

      if (shouldRegisterDirtyPageStencilPasses())
      {
        recordPageStencilPass(commandBuffer, pageStencilResetPipeline_, scratchClipmapTextures_[scratchIndex], pageStencilDrawIndirectBuffer_, layerIndex, scratchResolution, stencilIndirectOffset);
      }
    }
  }

  void recordFinishPass(rendering::CommandRecorder &commandBuffer)
  {
    if (usesClipSpaceAtlasDepthRaster())
    {
      return;
    }

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
  rendering::ComputePipeline prepareShadowDrawIndirectArgsPipeline_;
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
  rendering::Buffer globalDirtyPageCountBuffer_;
  rendering::Buffer globalDirtyPageListBuffer_;

  std::vector<rendering::Texture> scratchClipmapTextures_;
  rendering::Texture scratchPageTexture_;

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
  rendering::Shader drawScratchFragmentShader_;
  rendering::GraphicsPipeline drawScratchPipeline_;
  rendering::GraphicsPipeline drawScratchStencilPipeline_;
  rendering::Buffer drawScratchUniformBuffer_;
  rendering::Buffer rsmMaterialBuffer_;
  rendering::Buffer rsmVtAtlasInfoBuffer_;
  rendering::Buffer rsmTextureEntriesBuffer_;
  rendering::Buffer rsmVtPageTableBuffer_;
  rendering::Buffer rsmVtPhysicalPagesBuffer_;
  uint32_t drawScratchUniformStride_ = 0u;

  rendering::BindingsLayout resolvePagesLayout_;
  std::vector<rendering::BindingGroups> resolvePagesBindingGroups_;
  rendering::Shader resolvePagesShader_;
  rendering::ComputePipeline prepareResolveDispatchPipeline_;
  rendering::ComputePipeline clearDirtyPagesPipeline_;
  rendering::ComputePipeline clearRsmPagesPipeline_;
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
