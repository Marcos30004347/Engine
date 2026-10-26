#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "globalIllumination/WorldHitShadingTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"
#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/MaterialTextureResolver.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace globalillumination
{

class WorldHitShadingResolvePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t rayCount = 0u;
    bool enabled = true;
    uint32_t feedbackRequestCapacity = 16384u;
    uint32_t feedbackUploadBudget = 0u;
    uint32_t receiverPixelWidth = 0u;
  };

  WorldHitShadingResolvePass(
      virtualgeometry::VirtualGeometryScene &scene,
      const softwarebvh::SoftwareTLASManager &tlasManager,
      const softwarebvh::SoftwareBLASManager &blasManager,
      rendering::Buffer rayInputBuffer,
      rendering::Buffer hitOutputBuffer,
      rendering::Buffer failedRaysBuffer,
      rendering::Texture geometryNormalTexture,
      rendering::Texture baseColorTexture,
      rendering::Buffer worldHitShadingBuffer,
      Settings settings)
      : scene_(scene),
        tlasManager_(tlasManager),
        blasManager_(blasManager),
        rayInputBuffer_(rayInputBuffer),
        hitOutputBuffer_(hitOutputBuffer),
        failedRaysBuffer_(failedRaysBuffer),
        geometryNormalTexture_(geometryNormalTexture),
        baseColorTexture_(baseColorTexture),
        worldHitShadingBuffer_(worldHitShadingBuffer),
        settings_(settings)
  {
  }

  ~WorldHitShadingResolvePass() override
  {
    destroyFrameOverrideBuffers(frameOverrideUniformBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideMaterialRuntimeBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideAtlasInfoBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideTextureEntriesBufferIds_);
    destroyFrameOverrideBuffers(frameOverridePageTableBufferIds_);
    destroyFrameOverrideBuffers(frameOverridePhysicalPagesBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideFeedbackBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideFeedbackReadbackBufferIds_);
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
    if (materialRuntimeBuffer_.isValid())
      renderGraph->deleteBuffer(materialRuntimeBuffer_);
    if (atlasInfoBuffer_.isValid())
      renderGraph->deleteBuffer(atlasInfoBuffer_);
    if (textureEntriesBuffer_.isValid())
      renderGraph->deleteBuffer(textureEntriesBuffer_);
    if (vtPageTableBuffer_.isValid())
      renderGraph->deleteBuffer(vtPageTableBuffer_);
    if (vtPhysicalPagesBuffer_.isValid())
      renderGraph->deleteBuffer(vtPhysicalPagesBuffer_);
    if (feedbackBuffer_.isValid())
      renderGraph->deleteBuffer(feedbackBuffer_);
    if (feedbackReadbackBuffer_.isValid())
      renderGraph->deleteBuffer(feedbackReadbackBuffer_);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniformsAllFrames();
  }

  void setDispatchIndirectBuffer(rendering::Buffer dispatchIndirectBuffer)
  {
    dispatchIndirectBuffer_ = dispatchIndirectBuffer;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!settings_.enabled || settings_.rayCount == 0u)
      return;

    ensureResources();
    writeUniforms();
    syncMaterialStateAllFrames(false);
    const bool feedbackEnabled = settings_.feedbackRequestCapacity != 0u;
    if (feedbackEnabled)
      clearFeedback();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    if (dispatchIndirectBuffer_.isValid())
    {
      commandBuffer.cmdDispatchIndirect(dispatchIndirectBuffer_, 0u);
    }
    else
    {
      commandBuffer.cmdDispatch((std::max(1u, settings_.rayCount) + 63u) / 64u, 1u, 1u);
    }

    if (feedbackEnabled)
    {
      commandBuffer.cmdCopyBuffer(
          rendering::BufferView{
              .buffer = feedbackBuffer_,
              .offset = 0u,
              .size = feedbackBufferSize_,
              .access = rendering::AccessPattern::TRANSFER_READ,
          },
          rendering::BufferView{
              .buffer = feedbackReadbackBuffer_,
              .offset = 0u,
              .size = feedbackBufferSize_,
              .access = rendering::AccessPattern::TRANSFER_WRITE,
          });
    }
  }

  bool processFeedback(uint32_t uploadBudget = 0u)
  {
    if (!settings_.enabled || settings_.feedbackRequestCapacity == 0u)
      return false;

    if (!feedbackReadbackBuffer_.isValid() || feedbackBufferSize_ == 0u)
      return false;

    virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU header{};
    readFrameLocalBuffer(
        feedbackReadbackBuffer_,
        frameOverrideFeedbackReadbackBufferIds_,
        0u,
        sizeof(header),
        [&](const void *data)
        {
          std::memcpy(&header, data, sizeof(header));
        });

    lastFeedbackStats_ = {};
    lastFeedbackStats_.requested = header.requestCount;
    lastFeedbackStats_.overflowed = header.overflowCount;

    const uint32_t capacity = std::max(1u, settings_.feedbackRequestCapacity);
    const uint32_t requestCount = std::min(header.requestCount, capacity);
    if (requestCount == 0u)
    {
      syncMaterialStateAllFrames(false);
      clearFeedbackAllFrames();
      return false;
    }

    std::vector<virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU> requests(requestCount);
    readFrameLocalBuffer(
        feedbackReadbackBuffer_,
        frameOverrideFeedbackReadbackBufferIds_,
        sizeof(virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU),
        static_cast<uint64_t>(requestCount) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU),
        [&](const void *data)
        {
          std::memcpy(requests.data(), data, static_cast<size_t>(requestCount) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU));
        });

    uint32_t resolvedUploadBudget = uploadBudget != 0u ? uploadBudget : settings_.feedbackUploadBudget;
    if (resolvedUploadBudget == 0u)
    {
      const uint32_t baseBudget = std::max(1u, scene_.getVirtualTextureSystem().getSettings().maxUploadsPerFrame);
      const uint32_t pressureBudget = baseBudget + requestCount / 256u + header.overflowCount / 64u;
      resolvedUploadBudget = std::min(baseBudget * 4u, std::max(baseBudget, pressureBudget));
    }

    const bool changed = scene_.getVirtualTextureSystem().processFeedbackRequests(requests, resolvedUploadBudget, &lastFeedbackStats_);
    syncMaterialStateAllFrames(false);
    clearFeedbackAllFrames();
    return changed;
  }

  const virtualgeometry::VirtualTextureSystem::FeedbackStats &getLastFeedbackStats() const
  {
    return lastFeedbackStats_;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    appendFrameOverride(overrides, uniformBuffer_, frameOverrideUniformBufferIds_, frameSlot);
    appendFrameOverride(overrides, materialRuntimeBuffer_, frameOverrideMaterialRuntimeBufferIds_, frameSlot);
    appendFrameOverride(overrides, atlasInfoBuffer_, frameOverrideAtlasInfoBufferIds_, frameSlot);
    appendFrameOverride(overrides, textureEntriesBuffer_, frameOverrideTextureEntriesBufferIds_, frameSlot);
    appendFrameOverride(overrides, vtPageTableBuffer_, frameOverridePageTableBufferIds_, frameSlot);
    appendFrameOverride(overrides, vtPhysicalPagesBuffer_, frameOverridePhysicalPagesBufferIds_, frameSlot);
    appendFrameOverride(overrides, feedbackBuffer_, frameOverrideFeedbackBufferIds_, frameSlot);
    appendFrameOverride(overrides, feedbackReadbackBuffer_, frameOverrideFeedbackReadbackBufferIds_, frameSlot);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t controls[4]{0u, 0u, 0u, 0u};
  };

  static_assert(sizeof(Uniforms) == 16u, "WorldHitShadingResolvePass uniforms must match WGSL layout");

  struct MaterialRuntimeGPU
  {
    uint32_t materialId = 0u;
    uint32_t tileListBase = 0u;
    uint32_t baseColorTextureId = virtualgeometry::Material::InvalidTextureId;
    uint32_t normalTextureId = virtualgeometry::Material::InvalidTextureId;
    uint32_t hasNormalTexture = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
    uint32_t _padding2 = 0u;
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> pbrFactors{1.0f, 0.0f, 1.0f, 0.0f};
    std::array<float, 4> emissiveFactorAndStrength{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(MaterialRuntimeGPU) == 80u, "MaterialRuntimeGPU must match WGSL layout");

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = access,
    };
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const uint64_t materialCount = std::max<uint64_t>(1u, virtualTextures.getMaterialEntries().size());
    const uint64_t textureCount = std::max<uint64_t>(1u, virtualTextures.getTextureEntries().size());
    const uint64_t pageTableCount = std::max<uint64_t>(1u, virtualTextures.getPageTableEntries().size());
    const uint64_t physicalPageCount = std::max<uint64_t>(1u, virtualTextures.getPhysicalPagePixels().size());
    const uint64_t feedbackCapacity = std::max<uint64_t>(1u, settings_.feedbackRequestCapacity);
    feedbackBufferSize_ = sizeof(virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU) +
                          feedbackCapacity * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU);

    const bool materialBuffersTooSmall =
        (materialRuntimeBuffer_.isValid() && renderGraph->getBufferSize(materialRuntimeBuffer_) < materialCount * sizeof(MaterialRuntimeGPU)) ||
        (textureEntriesBuffer_.isValid() && renderGraph->getBufferSize(textureEntriesBuffer_) < textureCount * sizeof(virtualgeometry::VirtualTextureSystem::TextureEntryGPU)) ||
        (vtPageTableBuffer_.isValid() && renderGraph->getBufferSize(vtPageTableBuffer_) < pageTableCount * sizeof(virtualgeometry::VirtualTextureSystem::PageTableEntryGPU)) ||
        (vtPhysicalPagesBuffer_.isValid() && renderGraph->getBufferSize(vtPhysicalPagesBuffer_) < physicalPageCount * sizeof(uint32_t)) ||
        (feedbackBuffer_.isValid() && renderGraph->getBufferSize(feedbackBuffer_) < feedbackBufferSize_) ||
        (feedbackReadbackBuffer_.isValid() && renderGraph->getBufferSize(feedbackReadbackBuffer_) < feedbackBufferSize_);
    if (materialBuffersTooSmall)
    {
      if (bindingGroups_.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups_);
        bindingGroups_ = {};
      }
      if (materialRuntimeBuffer_.isValid())
      {
        renderGraph->deleteBuffer(materialRuntimeBuffer_);
        materialRuntimeBuffer_ = {};
      }
      if (textureEntriesBuffer_.isValid())
      {
        renderGraph->deleteBuffer(textureEntriesBuffer_);
        textureEntriesBuffer_ = {};
      }
      if (vtPageTableBuffer_.isValid())
      {
        renderGraph->deleteBuffer(vtPageTableBuffer_);
        vtPageTableBuffer_ = {};
      }
      if (vtPhysicalPagesBuffer_.isValid())
      {
        renderGraph->deleteBuffer(vtPhysicalPagesBuffer_);
        vtPhysicalPagesBuffer_ = {};
      }
      if (feedbackBuffer_.isValid())
      {
        renderGraph->deleteBuffer(feedbackBuffer_);
        feedbackBuffer_ = {};
      }
      if (feedbackReadbackBuffer_.isValid())
      {
        renderGraph->deleteBuffer(feedbackReadbackBuffer_);
        feedbackReadbackBuffer_ = {};
      }
      destroyFrameOverrideBuffers(frameOverrideMaterialRuntimeBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideAtlasInfoBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideTextureEntriesBufferIds_);
      destroyFrameOverrideBuffers(frameOverridePageTableBufferIds_);
      destroyFrameOverrideBuffers(frameOverridePhysicalPagesBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideFeedbackBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideFeedbackReadbackBufferIds_);
      frameOverrideMaterialRuntimeBufferIds_.clear();
      frameOverrideAtlasInfoBufferIds_.clear();
      frameOverrideTextureEntriesBufferIds_.clear();
      frameOverridePageTableBufferIds_.clear();
      frameOverridePhysicalPagesBufferIds_.clear();
      frameOverrideFeedbackBufferIds_.clear();
      frameOverrideFeedbackReadbackBufferIds_.clear();
    }

    bool materialStateBuffersCreated = false;
    bool feedbackBuffersCreated = false;

    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!atlasInfoBuffer_.isValid())
    {
      materialStateBuffersCreated = true;
      atlasInfoBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_AtlasInfo.buffer",
              .size = sizeof(virtualgeometry::VirtualTextureSystem::AtlasInfoGPU),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!materialRuntimeBuffer_.isValid())
    {
      materialStateBuffersCreated = true;
      materialRuntimeBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_MaterialRuntime.buffer",
              .size = materialCount * sizeof(MaterialRuntimeGPU),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!textureEntriesBuffer_.isValid())
    {
      materialStateBuffersCreated = true;
      textureEntriesBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_TextureEntries.buffer",
              .size = textureCount * sizeof(virtualgeometry::VirtualTextureSystem::TextureEntryGPU),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!vtPageTableBuffer_.isValid())
    {
      materialStateBuffersCreated = true;
      vtPageTableBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_VirtualTexturePageTable.buffer",
              .size = pageTableCount * sizeof(virtualgeometry::VirtualTextureSystem::PageTableEntryGPU),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!vtPhysicalPagesBuffer_.isValid())
    {
      materialStateBuffersCreated = true;
      vtPhysicalPagesBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_VirtualTexturePhysicalPages.buffer",
              .size = physicalPageCount * sizeof(uint32_t),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!feedbackBuffer_.isValid())
    {
      feedbackBuffersCreated = true;
      feedbackBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_VirtualTextureFeedback.buffer",
              .size = feedbackBufferSize_,
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
          });
    }
    if (!feedbackReadbackBuffer_.isValid())
    {
      feedbackBuffersCreated = true;
      feedbackReadbackBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_VirtualTextureFeedback.readback",
              .size = feedbackBufferSize_,
              .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
          });
    }
    initializeFrameOverrideBuffers(uniformBuffer_, rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push, frameOverrideUniformBufferIds_);
    initializeFrameOverrideBuffers(materialRuntimeBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverrideMaterialRuntimeBufferIds_);
    initializeFrameOverrideBuffers(atlasInfoBuffer_, rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push, frameOverrideAtlasInfoBufferIds_);
    initializeFrameOverrideBuffers(textureEntriesBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverrideTextureEntriesBufferIds_);
    initializeFrameOverrideBuffers(vtPageTableBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverridePageTableBufferIds_);
    initializeFrameOverrideBuffers(vtPhysicalPagesBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverridePhysicalPagesBufferIds_);
    initializeFrameOverrideBuffers(feedbackBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc, frameOverrideFeedbackBufferIds_);
    initializeFrameOverrideBuffers(feedbackReadbackBuffer_, rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull, frameOverrideFeedbackReadbackBufferIds_);
    writeUniformsAllFrames();
    syncMaterialStateAllFrames(materialStateBuffersCreated);
    if (feedbackBuffersCreated)
      clearFeedbackAllFrames();

    const uint64_t rayCount = static_cast<uint64_t>(std::max(1u, settings_.rayCount));
    if (!layout_.isValid())
    {
      layout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_Layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers = {
                      {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "rayInput", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "hitOutput", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "failedRays", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "tlasInstances", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "blasMetadata", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "fallbackVertices", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "fallbackIndices", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "worldHitShading", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "materialRuntime", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "atlasInfo", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "textureEntries", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "vtPageTable", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "vtPhysicalPages", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "feedbackBuffer", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "primitiveMaterialIds", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
                  .textures = {
                      {.name = "geometryNormalTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "baseColorTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              }},
          });
    }

    if (bindingGroups_.isValid() && bindingRayCount_ != rayCount)
    {
      renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = {};
    }
    if (!bindingGroups_.isValid())
    {
      bindingRayCount_ = rayCount;
      const uint64_t failedRaysSize = settings_.receiverPixelWidth != 0u ? 32ull : (32ull + rayCount * 48ull);
      bindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_Groups",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 1, .bufferView = makeBufferView(rayInputBuffer_, rayCount * sizeof(softwarebvh::rendering::SoftwareRayTracePass::TraceRayGPU), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 2, .bufferView = makeBufferView(hitOutputBuffer_, rayCount * sizeof(softwarebvh::rendering::SoftwareRayTracePass::HitOutputGPU), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 3, .bufferView = makeBufferView(failedRaysBuffer_, failedRaysSize, rendering::AccessPattern::SHADER_READ)},
                      {.binding = 4, .bufferView = makeBufferView(tlasManager_.getInstancesBuffer(), tlasManager_.getInstancesBufferSize(), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 5, .bufferView = makeBufferView(blasManager_.getTriangleMetadataBuffer(), blasManager_.getTriangleMetadataBufferSize(), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 6, .bufferView = makeBufferView(blasManager_.getVerticesBuffer(), blasManager_.getVerticesBufferSize(), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 7, .bufferView = makeBufferView(blasManager_.getIndicesBuffer(), blasManager_.getIndicesBufferSize(), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 8, .bufferView = makeBufferView(worldHitShadingBuffer_, worldHitShadingBufferSize(settings_.rayCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                      {.binding = 11, .bufferView = makeBufferView(materialRuntimeBuffer_, renderGraph->getBufferSize(materialRuntimeBuffer_), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 12, .bufferView = makeBufferView(atlasInfoBuffer_, renderGraph->getBufferSize(atlasInfoBuffer_), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 13, .bufferView = makeBufferView(textureEntriesBuffer_, renderGraph->getBufferSize(textureEntriesBuffer_), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 14, .bufferView = makeBufferView(vtPageTableBuffer_, renderGraph->getBufferSize(vtPageTableBuffer_), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 15, .bufferView = makeBufferView(vtPhysicalPagesBuffer_, renderGraph->getBufferSize(vtPhysicalPagesBuffer_), rendering::AccessPattern::SHADER_READ)},
                      {.binding = 16, .bufferView = makeBufferView(feedbackBuffer_, feedbackBufferSize_, rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                      {.binding = 17, .bufferView = makeBufferView(blasManager_.getPrimitiveMaterialIdsBuffer(), blasManager_.getPrimitiveMaterialIdsBufferSize(), rendering::AccessPattern::SHADER_READ)},
                  },
                  .textures = {
                      {.binding = 9, .textureView = makeSampledColorView(geometryNormalTexture_)},
                      {.binding = 10, .textureView = makeSampledColorView(baseColorTexture_)},
                  },
              }},
          });
    }

    if (!shader_.isValid())
    {
      shader_ = renderGraph->createShader(
          rendering::ShaderInfo{
              .name = passName + "_Shader",
              .layout = layout_,
              .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/world-hit-shading-resolve-cs.spirv"),
              .type = rendering::ShaderType::SpirV,
          });
    }

    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          rendering::ComputePipelineInfo{
              .entry = "cs_main",
              .layout = layout_,
              .name = passName + "_Pipeline",
              .shader = shader_,
          });
    }
  }

  void writeMaterialMetadata()
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &materialEntries = virtualTextures.getMaterialEntries();
    std::vector<MaterialRuntimeGPU> runtime(std::max<size_t>(1u, materialEntries.size()));
    for (uint32_t materialIndex = 0u; materialIndex < runtime.size(); ++materialIndex)
    {
      MaterialRuntimeGPU &material = runtime[materialIndex];
      material.materialId = materialIndex;
      const virtualgeometry::Material *definition = scene_.getMaterialManager().getMaterial(materialIndex);
      if (materialIndex < materialEntries.size())
        material.baseColorTextureId = virtualgeometry::resolveBaseColorMaterialTextureId(definition, materialEntries[materialIndex]);
      if (definition != nullptr)
      {
        const float *baseColorFactor = definition->getBaseColorFactor();
        std::copy(baseColorFactor, baseColorFactor + 4, material.baseColorFactor.begin());
        material.pbrFactors[0] = definition->getRoughnessFactor();
        material.pbrFactors[1] = definition->getMetallicFactor();
        material.pbrFactors[2] = definition->getOpacity();
        const float *emissiveFactor = definition->getEmissiveFactor();
        material.emissiveFactorAndStrength[0] = emissiveFactor[0];
        material.emissiveFactorAndStrength[1] = emissiveFactor[1];
        material.emissiveFactorAndStrength[2] = emissiveFactor[2];
        material.emissiveFactorAndStrength[3] = definition->getEmissiveStrength();
      }
    }

    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTable = virtualTextures.getPageTableEntries();
    const virtualgeometry::VirtualTextureSystem::TextureEntryGPU emptyTexture{};
    const virtualgeometry::VirtualTextureSystem::PageTableEntryGPU emptyPage{};
    const auto &atlasInfo = virtualTextures.getAtlasInfo();

    writeFrameLocalBuffer(materialRuntimeBuffer_, frameOverrideMaterialRuntimeBufferIds_, 0u, runtime.size() * sizeof(MaterialRuntimeGPU), runtime.data());
    writeFrameLocalBuffer(atlasInfoBuffer_, frameOverrideAtlasInfoBufferIds_, 0u, sizeof(atlasInfo), &atlasInfo);
    writeFrameLocalBuffer(
        textureEntriesBuffer_,
        frameOverrideTextureEntriesBufferIds_,
        0u,
        std::max<size_t>(1u, textureEntries.size()) * sizeof(emptyTexture),
        textureEntries.empty() ? &emptyTexture : textureEntries.data());
    writeFrameLocalBuffer(
        vtPageTableBuffer_,
        frameOverridePageTableBufferIds_,
        0u,
        std::max<size_t>(1u, pageTable.size()) * sizeof(emptyPage),
        pageTable.empty() ? &emptyPage : pageTable.data());
  }

  void writePhysicalPages(const std::vector<uint32_t> *dirtyPhysicalPages, bool fullUpload)
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();
    const uint32_t emptyPixel = 0xffffffffu;
    if (physicalPages.empty())
    {
      if (fullUpload)
        writeFrameLocalBuffer(vtPhysicalPagesBuffer_, frameOverridePhysicalPagesBufferIds_, 0u, sizeof(emptyPixel), &emptyPixel);
      return;
    }

    const uint64_t pageSize = std::max<uint32_t>(1u, virtualTextures.getSettings().pageSize);
    const uint64_t pageTexelCount = pageSize * pageSize;
    if (fullUpload || dirtyPhysicalPages == nullptr)
    {
      writeFrameLocalBuffer(
          vtPhysicalPagesBuffer_,
          frameOverridePhysicalPagesBufferIds_,
          0u,
          physicalPages.size() * sizeof(uint32_t),
          physicalPages.data());
      return;
    }

    const uint64_t totalTexels = physicalPages.size();
    for (const uint32_t physicalPage : *dirtyPhysicalPages)
    {
      const uint64_t texelOffset = static_cast<uint64_t>(physicalPage) * pageTexelCount;
      if (texelOffset >= totalTexels)
        continue;
      const uint64_t texelCount = std::min<uint64_t>(pageTexelCount, totalTexels - texelOffset);
      writeFrameLocalBuffer(
          vtPhysicalPagesBuffer_,
          frameOverridePhysicalPagesBufferIds_,
          texelOffset * sizeof(uint32_t),
          texelCount * sizeof(uint32_t),
          physicalPages.data() + texelOffset);
    }
  }

  void syncMaterialStateAllFrames(bool forceFullUpload)
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const uint64_t pageTableVersion = virtualTextures.getPageTableVersion();
    const uint64_t physicalPageVersion = virtualTextures.getPhysicalPageVersion();
    const bool metadataChanged = forceFullUpload || pageTableVersion != syncedPageTableVersion_;
    const bool physicalPagesChanged = forceFullUpload || physicalPageVersion != syncedPhysicalPageVersion_;
    if (!metadataChanged && !physicalPagesChanged)
      return;

    bool fullPhysicalUpload = forceFullUpload;
    pendingPhysicalPageUploads_.clear();
    if (physicalPagesChanged && !fullPhysicalUpload)
      fullPhysicalUpload = !virtualTextures.collectDirtyPhysicalPagesSince(syncedPhysicalPageVersion_, pendingPhysicalPageUploads_);
    const std::vector<uint32_t> *dirtyPages = fullPhysicalUpload ? nullptr : &pendingPhysicalPageUploads_;

    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
    const uint32_t originalFrameIndex = getCurrentFrameIndex();
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      renderGraph->setCurrentFrameIndex(frameSlot);
      if (metadataChanged)
        writeMaterialMetadata();
      if (physicalPagesChanged)
        writePhysicalPages(dirtyPages, fullPhysicalUpload);
    }
    renderGraph->setCurrentFrameIndex(originalFrameIndex);

    syncedPageTableVersion_ = pageTableVersion;
    syncedPhysicalPageVersion_ = physicalPageVersion;
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.controls[0] = settings_.enabled ? 1u : 0u;
    uniforms.controls[1] = settings_.rayCount;
    uniforms.controls[2] = settings_.feedbackRequestCapacity;
    uniforms.controls[3] = settings_.receiverPixelWidth;
    writeFrameLocalBuffer(uniformBuffer_, frameOverrideUniformBufferIds_, 0u, sizeof(uniforms), &uniforms);
  }

  void clearFeedback()
  {
    if (!feedbackBuffer_.isValid())
      return;
    virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU zeroHeader{};
    writeFrameLocalBuffer(feedbackBuffer_, frameOverrideFeedbackBufferIds_, 0u, sizeof(zeroHeader), &zeroHeader);
  }

  void appendFrameOverride(
      rendering::RenderGraph::Overrides &overrides,
      const rendering::Buffer &buffer,
      const std::vector<rendering::BufferId> &bufferIds,
      uint32_t frameSlot) const
  {
    if (!buffer.isValid() || bufferIds.empty())
      return;

    const rendering::BufferId bufferId = bufferIds[frameSlot % bufferIds.size()];
    if (bufferId != rendering::BufferId::Invalid)
      overrides.bufferOverrides.emplace(buffer.name, rendering::RenderGraphBufferOverride{.bufferId = bufferId});
  }

  void initializeFrameOverrideBuffers(
      const rendering::Buffer &buffer,
      rendering::BufferUsage usage,
      std::vector<rendering::BufferId> &bufferIds)
  {
    if (renderGraph == nullptr || !buffer.isValid())
      return;

    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    if (frameCount <= 1u || !bufferIds.empty())
      return;

    bufferIds.assign(frameCount, rendering::BufferId::Invalid);
    const uint64_t bufferSize = renderGraph->getBufferSize(buffer);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      bufferIds[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = buffer.name + ".override_frame" + std::to_string(frameSlot),
              .size = bufferSize,
              .usage = usage,
          });
    }
  }

  void destroyFrameOverrideBuffers(const std::vector<rendering::BufferId> &bufferIds) const
  {
    if (renderGraph == nullptr)
      return;

    for (const rendering::BufferId bufferId : bufferIds)
    {
      if (bufferId != rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }

  rendering::BufferId getFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds, uint32_t frameSlot) const
  {
    if (bufferIds.empty())
      return rendering::BufferId::Invalid;
    return bufferIds[frameSlot % bufferIds.size()];
  }

  rendering::BufferId getCurrentFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds) const
  {
    return getFrameOverrideBufferId(bufferIds, getCurrentFrameIndex());
  }

  void writeFrameLocalBuffer(
      const rendering::Buffer &buffer,
      const std::vector<rendering::BufferId> &bufferIds,
      uint64_t offset,
      uint64_t size,
      const void *data) const
  {
    const rendering::BufferId overrideBufferId = getCurrentFrameOverrideBufferId(bufferIds);
    if (overrideBufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->bufferWrite(overrideBufferId, offset, size, const_cast<void *>(data));
      return;
    }

    renderGraph->bufferWrite(buffer, offset, size, const_cast<void *>(data));
  }

  void readFrameLocalBuffer(
      const rendering::Buffer &buffer,
      const std::vector<rendering::BufferId> &bufferIds,
      uint64_t offset,
      uint64_t size,
      std::function<void(const void *)> callback) const
  {
    const rendering::BufferId overrideBufferId = getCurrentFrameOverrideBufferId(bufferIds);
    if (overrideBufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->bufferRead(overrideBufferId, offset, size, std::move(callback));
      return;
    }

    renderGraph->bufferRead(buffer, offset, size, std::move(callback));
  }

  void writeUniformsAllFrames()
  {
    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
    const uint32_t originalFrameIndex = getCurrentFrameIndex();
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      renderGraph->setCurrentFrameIndex(frameSlot);
      writeUniforms();
    }
    renderGraph->setCurrentFrameIndex(originalFrameIndex);
  }

  void clearFeedbackAllFrames()
  {
    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
    const uint32_t originalFrameIndex = getCurrentFrameIndex();
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      renderGraph->setCurrentFrameIndex(frameSlot);
      clearFeedback();
    }
    renderGraph->setCurrentFrameIndex(originalFrameIndex);
  }

  virtualgeometry::VirtualGeometryScene &scene_;
  const softwarebvh::SoftwareTLASManager &tlasManager_;
  const softwarebvh::SoftwareBLASManager &blasManager_;
  rendering::Buffer rayInputBuffer_{};
  rendering::Buffer hitOutputBuffer_{};
  rendering::Buffer failedRaysBuffer_{};
  rendering::Buffer dispatchIndirectBuffer_{};
  rendering::Texture geometryNormalTexture_{};
  rendering::Texture baseColorTexture_{};
  rendering::Buffer worldHitShadingBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  rendering::Buffer materialRuntimeBuffer_{};
  rendering::Buffer atlasInfoBuffer_{};
  rendering::Buffer textureEntriesBuffer_{};
  rendering::Buffer vtPageTableBuffer_{};
  rendering::Buffer vtPhysicalPagesBuffer_{};
  rendering::Buffer feedbackBuffer_{};
  rendering::Buffer feedbackReadbackBuffer_{};
  uint64_t feedbackBufferSize_ = 0u;
  uint64_t bindingRayCount_ = 0u;
  uint64_t syncedPageTableVersion_ = 0u;
  uint64_t syncedPhysicalPageVersion_ = 0u;
  virtualgeometry::VirtualTextureSystem::FeedbackStats lastFeedbackStats_{};
  std::vector<uint32_t> pendingPhysicalPageUploads_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideMaterialRuntimeBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideAtlasInfoBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideTextureEntriesBufferIds_{};
  std::vector<rendering::BufferId> frameOverridePageTableBufferIds_{};
  std::vector<rendering::BufferId> frameOverridePhysicalPagesBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideFeedbackBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideFeedbackReadbackBufferIds_{};
};

} // namespace globalillumination
