#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryMaterialPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    rendering::Format colorFormat = rendering::Format::Format_RGBA16Float;
    rendering::Format depthFormat = rendering::Format::Format_Depth32Float;
    uint32_t feedbackRequestCapacity = 65536u;
    uint32_t feedbackStride = 2u;
    uint32_t maxUploadsPerFrame = 32u;
    uint32_t tileSize = 128u;
    uint32_t tileWorkgroupSize = 8u;
    uint32_t maxMaterialsPerTile = 256u;
  };

  VirtualGeometryMaterialPass(VirtualGeometryScene &scene, VirtualGeometryHardwareDrawPass::FrameTarget frameTarget, rendering::Texture shadowLightingTexture, Settings settings)
      : scene_(scene), frameTarget_(frameTarget), shadowLightingTexture_(shadowLightingTexture), settings_(settings)
  {
  }

  ~VirtualGeometryMaterialPass() override
  {
    destroyFrameOverrideResources();
    renderGraph->deleteGraphicsPipeline(materialDepthPipeline_);
    renderGraph->deleteGraphicsPipeline(materialDrawPipeline_);
    renderGraph->deleteComputePipeline(prepareTileMaterialsPipeline_);
    renderGraph->deleteComputePipeline(prepareIndirectArgsPipeline_);
    renderGraph->deleteShader(fullscreenVertexShader_);
    renderGraph->deleteShader(materialDepthFragmentShader_);
    renderGraph->deleteShader(materialVertexShader_);
    renderGraph->deleteShader(materialFragmentShader_);
    renderGraph->deleteShader(materialTilesComputeShader_);
    renderGraph->deleteBindingGroups(materialDepthBindingGroups_);
    renderGraph->deleteBindingGroups(materialDrawBindingGroups_);
    renderGraph->deleteBindingGroups(computeBindingGroups_);
    renderGraph->deleteBindingsLayout(materialDepthLayout_);
    renderGraph->deleteBindingsLayout(materialDrawLayout_);
    renderGraph->deleteBindingsLayout(computeLayout_);
    renderGraph->deleteBuffer(quadUniformBuffer_);
    renderGraph->deleteBuffer(materialPassUniformBuffer_);
    renderGraph->deleteBuffer(tileUniformBuffer_);
    renderGraph->deleteBuffer(atlasInfoBuffer_);
    renderGraph->deleteBuffer(materialEntriesBuffer_);
    renderGraph->deleteBuffer(textureEntriesBuffer_);
    renderGraph->deleteBuffer(vtPageTableBuffer_);
    renderGraph->deleteBuffer(vtPhysicalPagesBuffer_);
    renderGraph->deleteBuffer(feedbackBuffer_);
    renderGraph->deleteBuffer(feedbackReadbackBuffer_);
    renderGraph->deleteBuffer(tileDrawEntriesBuffer_);
    renderGraph->deleteBuffer(tileDrawCountersBuffer_);
    renderGraph->deleteBuffer(materialDrawIndirectBuffer_);
    renderGraph->deleteTexture(materialDepthTexture_);
  }

  void beginFrame()
  {
    if (renderGraph == nullptr || !feedbackBuffer_.isValid())
      return;

    syncVirtualTextureState(!gpuDataUploaded_);

    VirtualTextureSystem::FeedbackHeaderGPU zeroHeader{};
    renderGraph->bufferWrite(feedbackBuffer_, 0, sizeof(zeroHeader), &zeroHeader);

    const std::array<uint32_t, 4> zeroCounters = {0u, 0u, 0u, 0u};
    writeBuffer(tileDrawCountersBuffer_, frameOverrideTileDrawCountersBufferIds_, 0, sizeof(zeroCounters), zeroCounters.data());

    const std::array<uint32_t, 4> zeroIndirect = {0u, 1u, 0u, 0u};
    writeBuffer(materialDrawIndirectBuffer_, frameOverrideMaterialDrawIndirectBufferIds_, 0, sizeof(zeroIndirect), zeroIndirect.data());
  }

  bool processFeedback(uint32_t uploadBudget = 0u)
  {
    if (renderGraph == nullptr || !feedbackReadbackBuffer_.isValid() || feedbackBufferSize_ == 0u)
      return false;

    VirtualTextureSystem::FeedbackHeaderGPU header{};
    renderGraph->bufferRead(
        feedbackReadbackBuffer_,
        0,
        sizeof(header),
        [&](const void *data)
        {
          std::memcpy(&header, data, sizeof(header));
        });

    lastFeedbackStats_ = {};
    lastFeedbackStats_.requested = header.requestCount;
    lastFeedbackStats_.overflowed = header.overflowCount;

    const uint32_t requestCount = std::min(header.requestCount, settings_.feedbackRequestCapacity);
    if (requestCount == 0u)
      return false;

    std::vector<VirtualTextureSystem::FeedbackRequestGPU> requests(requestCount);
    renderGraph->bufferRead(
        feedbackReadbackBuffer_,
        sizeof(VirtualTextureSystem::FeedbackHeaderGPU),
        static_cast<uint64_t>(requestCount) * sizeof(VirtualTextureSystem::FeedbackRequestGPU),
        [&](const void *data)
        {
          std::memcpy(requests.data(), data, static_cast<size_t>(requestCount) * sizeof(VirtualTextureSystem::FeedbackRequestGPU));
        });

    const bool changed = scene_.getVirtualTextureSystem().processFeedbackRequests(requests, uploadBudget == 0u ? settings_.maxUploadsPerFrame : uploadBudget, &lastFeedbackStats_);

    if (changed)
      syncVirtualTextureState(false);

    return changed;
  }

  const VirtualTextureSystem::FeedbackStats &getLastFeedbackStats() const
  {
    return lastFeedbackStats_;
  }

  void updateCamera(const math::Mat4f &, const math::Mat4f &, uint32_t, uint32_t)
  {
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    appendBufferOverride(overrides, tileUniformBuffer_, frameOverrideTileUniformBufferIds_, frameSlot);
    appendBufferOverride(overrides, tileDrawEntriesBuffer_, frameOverrideTileDrawEntriesBufferIds_, frameSlot);
    appendBufferOverride(overrides, tileDrawCountersBuffer_, frameOverrideTileDrawCountersBufferIds_, frameSlot);
    appendBufferOverride(overrides, materialDrawIndirectBuffer_, frameOverrideMaterialDrawIndirectBufferIds_, frameSlot);
    appendTextureOverride(overrides, materialDepthTexture_, frameOverrideMaterialDepthTextureIds_, frameSlot);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &materialEntries = virtualTextures.getMaterialEntries();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();

    const uint64_t atlasInfoSize = sizeof(VirtualTextureSystem::AtlasInfoGPU);
    const uint64_t materialEntriesSize = std::max<uint64_t>(sizeof(VirtualTextureSystem::MaterialEntryGPU), materialEntries.size() * sizeof(VirtualTextureSystem::MaterialEntryGPU));
    const uint64_t textureEntriesSize = std::max<uint64_t>(sizeof(VirtualTextureSystem::TextureEntryGPU), textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU));
    const uint64_t vtPageTableSize = std::max<uint64_t>(sizeof(VirtualTextureSystem::PageTableEntryGPU), pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU));
    const uint64_t vtPhysicalPagesSize = std::max<uint64_t>(sizeof(uint32_t), physicalPages.size() * sizeof(uint32_t));

    tilesX_ = (settings_.viewPortWidth + settings_.tileSize - 1u) / settings_.tileSize;
    tilesY_ = (settings_.viewPortHeight + settings_.tileSize - 1u) / settings_.tileSize;
    tileCount_ = tilesX_ * tilesY_;
    maxTileDrawEntries_ = std::max<uint32_t>(1u, tileCount_ * settings_.maxMaterialsPerTile);

    quadUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_QuadUniforms.buffer",
          .size = sizeof(QuadUniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    materialPassUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_MaterialPassUniforms.buffer",
          .size = sizeof(MaterialPassUniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    tileUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_TileUniforms.buffer",
          .size = sizeof(TileUniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    atlasInfoBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_AtlasInfo.buffer",
          .size = atlasInfoSize,
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    materialEntriesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_MaterialEntries.buffer",
          .size = materialEntriesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    textureEntriesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_TextureEntries.buffer",
          .size = textureEntriesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    vtPageTableBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_VirtualTexturePageTable.buffer",
          .size = vtPageTableSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    vtPhysicalPagesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_VirtualTexturePhysicalPages.buffer",
          .size = vtPhysicalPagesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });

    feedbackBufferSize_ = sizeof(VirtualTextureSystem::FeedbackHeaderGPU) + static_cast<uint64_t>(settings_.feedbackRequestCapacity) * sizeof(VirtualTextureSystem::FeedbackRequestGPU);
    feedbackBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_Feedback.buffer",
          .size = feedbackBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    feedbackReadbackBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_Feedback.readback",
          .size = feedbackBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
        });

    tileDrawEntriesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_TileDrawEntries.buffer",
          .size = static_cast<uint64_t>(maxTileDrawEntries_) * sizeof(TileMaterialDrawEntry),
          .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    tileDrawCountersBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_TileDrawCounters.buffer",
          .size = sizeof(TileDrawCounters),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    materialDrawIndirectBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_MaterialIndirect.buffer",
          .size = 4u * sizeof(uint32_t),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });

    materialDepthTexture_ = createFrameLocalTexture(
        rendering::TextureInfo{
          .name = passName + "_MaterialDepth.texture",
          .format = settings_.depthFormat,
          .height = settings_.viewPortHeight,
          .width = settings_.viewPortWidth,
          .depth = 1u,
          .mipLevels = 1u,
          .memoryProperties = rendering::BufferUsage::BufferUsage_None,
          .usage = rendering::ImageUsage::ImageUsage_DepthStencilAttachment,
        });
    initializeFrameOverrideResources();

    const QuadUniforms quadUniforms = {
      .ndcMinX = -1.0f,
      .ndcMinY = -1.0f,
      .ndcMaxX = 1.0f,
      .ndcMaxY = 1.0f,
      .uvMinX = 0.0f,
      .uvMinY = 0.0f,
      .uvMaxX = 1.0f,
      .uvMaxY = 1.0f,
    };
    renderGraph->bufferWrite(quadUniformBuffer_, 0, sizeof(QuadUniforms), const_cast<QuadUniforms *>(&quadUniforms));

    const MaterialPassUniforms materialUniforms = {
      .feedbackStride = settings_.feedbackStride,
      .feedbackRequestCapacity = settings_.feedbackRequestCapacity,
      ._padding0 = 0u,
      ._padding1 = 0u,
    };
    renderGraph->bufferWrite(materialPassUniformBuffer_, 0, sizeof(MaterialPassUniforms), const_cast<MaterialPassUniforms *>(&materialUniforms));

    createPipelinesAndBindings(atlasInfoSize, materialEntriesSize, textureEntriesSize, vtPageTableSize, vtPhysicalPagesSize);

    syncVirtualTextureState(true);
    beginFrame();

    commandBuffer.cmdBindComputePipeline(prepareTileMaterialsPipeline_);
    commandBuffer.cmdBindBindingGroups(computeBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(tilesX_, tilesY_, 1u);

    commandBuffer.cmdBindComputePipeline(prepareIndirectArgsPipeline_);
    commandBuffer.cmdBindBindingGroups(computeBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(1u, 1u, 1u);

    rendering::RenderPassInfo materialDepthRenderPass{};
    materialDepthRenderPass.name = passName + "_MaterialDepthRenderPass";
    materialDepthRenderPass.scissor = rendering::Rect2D(0, 0, settings_.viewPortWidth, settings_.viewPortHeight);
    materialDepthRenderPass.viewport = rendering::Viewport(settings_.viewPortWidth, settings_.viewPortHeight);
    materialDepthRenderPass.depthStencilAttachment = makeDepthAttachmentInfo(materialDepthTexture_, rendering::LoadOp::LoadOp_Clear, 0.0f);

    commandBuffer.cmdBindGraphicsPipeline(materialDepthPipeline_);
    commandBuffer.cmdBeginRenderPass(materialDepthRenderPass);
    commandBuffer.cmdBindBindingGroups(materialDepthBindingGroups_, nullptr, 0);
    commandBuffer.cmdDraw(6, 1, 0, 0);
    commandBuffer.cmdEndRenderPass();

    rendering::RenderPassInfo materialDrawRenderPass{};
    materialDrawRenderPass.name = passName + "_MaterialDrawRenderPass";
    materialDrawRenderPass.scissor = rendering::Rect2D(0, 0, settings_.viewPortWidth, settings_.viewPortHeight);
    materialDrawRenderPass.viewport = rendering::Viewport(settings_.viewPortWidth, settings_.viewPortHeight);
    materialDrawRenderPass.colorAttachments = {
      {
        .name = passName + "_Color",
        .view = frameTarget_.colorView,
        .clearValue = rendering::Color::rgba(0, 0, 0, 0),
      },
    };
    materialDrawRenderPass.depthStencilAttachment = makeDepthAttachmentInfo(materialDepthTexture_, rendering::LoadOp::LoadOp_Load, 0.0f);

    commandBuffer.cmdBindGraphicsPipeline(materialDrawPipeline_);
    commandBuffer.cmdBeginRenderPass(materialDrawRenderPass);
    commandBuffer.cmdBindBindingGroups(materialDrawBindingGroups_, nullptr, 0);
    commandBuffer.cmdDrawIndirect(
        rendering::BufferView{
          .buffer = materialDrawIndirectBuffer_,
          .offset = 0,
          .size = renderGraph->getBufferSize(materialDrawIndirectBuffer_),
          .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
        },
        0,
        1,
        4u * sizeof(uint32_t));
    commandBuffer.cmdEndRenderPass();

    commandBuffer.cmdCopyBuffer(
        rendering::BufferView{
          .buffer = feedbackBuffer_,
          .offset = 0,
          .size = feedbackBufferSize_,
          .access = rendering::AccessPattern::TRANSFER_READ,
        },
        rendering::BufferView{
          .buffer = feedbackReadbackBuffer_,
          .offset = 0,
          .size = feedbackBufferSize_,
          .access = rendering::AccessPattern::TRANSFER_WRITE,
        });
  }

private:
  struct QuadUniforms
  {
    float ndcMinX, ndcMinY;
    float ndcMaxX, ndcMaxY;
    float uvMinX, uvMinY;
    float uvMaxX, uvMaxY;
  };

  struct MaterialPassUniforms
  {
    uint32_t feedbackStride = 1u;
    uint32_t feedbackRequestCapacity = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };

  struct TileUniforms
  {
    uint32_t viewportWidth = 1u;
    uint32_t viewportHeight = 1u;
    uint32_t tileSize = 1u;
    uint32_t tilesX = 1u;
    uint32_t tilesY = 1u;
    uint32_t materialCount = 0u;
    uint32_t maxDrawEntries = 1u;
    uint32_t _padding0 = 0u;
  };

  struct TileMaterialDrawEntry
  {
    uint32_t tileX = 0u;
    uint32_t tileY = 0u;
    uint32_t materialId = 0u;
    uint32_t _padding0 = 0u;
  };

  struct TileDrawCounters
  {
    uint32_t drawCount = 0u;
    uint32_t overflowCount = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
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

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
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

  static rendering::DepthStencilAttachmentInfo makeDepthAttachmentInfo(rendering::Texture texture, rendering::LoadOp, float clearDepth)
  {
    return rendering::DepthStencilAttachmentInfo{
      .enabled = true,
      .name = texture.name + "_View",
      .view =
          rendering::TextureView{
              .texture = texture,
              .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
              .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
              .index = 0,
              .flags = rendering::ImageAspectFlags::Depth,
              .baseArrayLayer = 0,
              .baseMipLevel = 0,
              .layerCount = 1,
              .levelCount = 1,
          },
      .clearDepth = clearDepth,
      .clearStencil = 0u,
    };
  }

  void createPipelinesAndBindings(uint64_t atlasInfoSize, uint64_t materialEntriesSize, uint64_t textureEntriesSize, uint64_t vtPageTableSize, uint64_t vtPhysicalPagesSize)
  {
    computeLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_ComputeLayout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "tileUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "tileDrawCounters", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "tileDrawEntries", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialDrawIndirect", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .textures = {
                {.name = "sceneDepthTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialIdTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
          }},
        });

    materialDepthLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_MaterialDepthLayout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "quadUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                {.name = "tileUniforms", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
              },
              .textures = {
                {.name = "sceneDepthTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "materialIdTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
              },
          }},
        });

    materialDrawLayout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_MaterialDrawLayout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "tileUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                {.name = "materialPassUniforms", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "atlasInfo", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "materialEntries", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "textureEntries", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "vtPageTable", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "vtPhysicalPages", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "feedbackBuffer", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "tileDrawEntries", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
              },
              .textures = {
                {.name = "materialUVTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
                {.name = "shadowLightingTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Fragment},
              },
          }},
        });

    computeBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = computeLayout_,
          .name = passName + "_ComputeGroups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                {.binding = 0, .bufferView = {.buffer = tileUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(TileUniforms)}},
                {.binding = 1, .bufferView = {.buffer = tileDrawCountersBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = sizeof(TileDrawCounters)}},
                {.binding = 2, .bufferView = {.buffer = tileDrawEntriesBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph->getBufferSize(tileDrawEntriesBuffer_)}},
                {.binding = 3, .bufferView = {.buffer = materialDrawIndirectBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph->getBufferSize(materialDrawIndirectBuffer_)}},
              },
              .textures = {
                {.binding = 4, .textureView = makeSampledDepthView(frameTarget_.depthTexture)},
                {.binding = 5, .textureView = makeSampledColorView(frameTarget_.materialIdView.texture)},
              },
          }},
        });

    materialDepthBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = materialDepthLayout_,
          .name = passName + "_MaterialDepthGroups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                {.binding = 0, .bufferView = {.buffer = quadUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(QuadUniforms)}},
                {.binding = 1, .bufferView = {.buffer = tileUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(TileUniforms)}},
              },
              .textures = {
                {.binding = 2, .textureView = makeSampledDepthView(frameTarget_.depthTexture)},
                {.binding = 3, .textureView = makeSampledColorView(frameTarget_.materialIdView.texture)},
              },
          }},
        });

    materialDrawBindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = materialDrawLayout_,
          .name = passName + "_MaterialDrawGroups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                {.binding = 0, .bufferView = {.buffer = tileUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(TileUniforms)}},
                {.binding = 1, .bufferView = {.buffer = materialPassUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(MaterialPassUniforms)}},
                {.binding = 2, .bufferView = {.buffer = atlasInfoBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = atlasInfoSize}},
                {.binding = 3, .bufferView = {.buffer = materialEntriesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = materialEntriesSize}},
                {.binding = 4, .bufferView = {.buffer = textureEntriesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = textureEntriesSize}},
                {.binding = 5, .bufferView = {.buffer = vtPageTableBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = vtPageTableSize}},
                {.binding = 6, .bufferView = {.buffer = vtPhysicalPagesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = vtPhysicalPagesSize}},
                {.binding = 7, .bufferView = {.buffer = feedbackBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = feedbackBufferSize_}},
                {.binding = 8, .bufferView = {.buffer = tileDrawEntriesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph->getBufferSize(tileDrawEntriesBuffer_)}},
              },
              .textures = {
                {.binding = 9, .textureView = makeSampledColorView(frameTarget_.materialUVView.texture)},
                {.binding = 10, .textureView = makeSampledColorView(shadowLightingTexture_)},
              },
          }},
        });

    fullscreenVertexShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_FullscreenVS",
          .layout = materialDepthLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/renderToQuadPass-vs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });
    materialDepthFragmentShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_MaterialDepthFS",
          .layout = materialDepthLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-material-depth-fs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });
    materialVertexShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_MaterialVS",
          .layout = materialDrawLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-material-vs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });
    materialFragmentShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_MaterialFS",
          .layout = materialDrawLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-material-fs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });
    materialTilesComputeShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_MaterialTilesCS",
          .layout = computeLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-material-tiles-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    prepareTileMaterialsPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "prepareTileMaterials",
          .layout = computeLayout_,
          .name = passName + "_PrepareTileMaterials.pipeline",
          .shader = materialTilesComputeShader_,
        });
    prepareIndirectArgsPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "prepareMaterialIndirectArgs",
          .layout = computeLayout_,
          .name = passName + "_PrepareIndirectArgs.pipeline",
          .shader = materialTilesComputeShader_,
        });

    rendering::GraphicsPipelineInfo materialDepthPipelineInfo{};
    materialDepthPipelineInfo.name = passName + "_MaterialDepth.pipeline";
    materialDepthPipelineInfo.layout = materialDepthLayout_;
    materialDepthPipelineInfo.vertexStage.vertexShader = fullscreenVertexShader_;
    materialDepthPipelineInfo.vertexStage.shaderEntry = "vs_main";
    materialDepthPipelineInfo.vertexStage.cullType = rendering::CullMode::None;
    materialDepthPipelineInfo.vertexStage.winding = rendering::WindingOrder::CCW;
    materialDepthPipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    materialDepthPipelineInfo.fragmentStage.fragmentShader = materialDepthFragmentShader_;
    materialDepthPipelineInfo.fragmentStage.shaderEntry = "fs_main";
    materialDepthPipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = settings_.depthFormat,
      .loadOp = rendering::LoadOp::LoadOp_Clear,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .comparison = rendering::ComparisonOp::ComparisonOp_Always,
    };
    materialDepthPipeline_ = renderGraph->createGraphicsPipeline(materialDepthPipelineInfo);

    rendering::GraphicsPipelineInfo materialDrawPipelineInfo{};
    materialDrawPipelineInfo.name = passName + "_MaterialDraw.pipeline";
    materialDrawPipelineInfo.layout = materialDrawLayout_;
    materialDrawPipelineInfo.vertexStage.vertexShader = materialVertexShader_;
    materialDrawPipelineInfo.vertexStage.shaderEntry = "vs_main";
    materialDrawPipelineInfo.vertexStage.cullType = rendering::CullMode::None;
    materialDrawPipelineInfo.vertexStage.winding = rendering::WindingOrder::CCW;
    materialDrawPipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    materialDrawPipelineInfo.fragmentStage.fragmentShader = materialFragmentShader_;
    materialDrawPipelineInfo.fragmentStage.shaderEntry = "fs_main";
    materialDrawPipelineInfo.fragmentStage.colorAttatchments = {
      {
        .format = settings_.colorFormat,
        .loadOp = rendering::LoadOp::LoadOp_Clear,
        .storeOp = rendering::StoreOp::StoreOp_Store,
        .initialLayout = rendering::ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = rendering::ResourceLayout::COLOR_ATTACHMENT,
      },
    };
    materialDrawPipelineInfo.fragmentStage.depthAttatchment = {
      .enabled = true,
      .format = settings_.depthFormat,
      .loadOp = rendering::LoadOp::LoadOp_Load,
      .storeOp = rendering::StoreOp::StoreOp_Store,
      .comparison = rendering::ComparisonOp::ComparisonOp_Equal,
    };
    materialDrawPipeline_ = renderGraph->createGraphicsPipeline(materialDrawPipelineInfo);
  }

  void syncVirtualTextureState(bool forceFullUpload)
  {
    if (renderGraph == nullptr || !atlasInfoBuffer_.isValid())
      return;

    auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &atlasInfo = virtualTextures.getAtlasInfo();
    const auto &materialEntries = virtualTextures.getMaterialEntries();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();

    renderGraph->bufferWrite(atlasInfoBuffer_, 0, sizeof(VirtualTextureSystem::AtlasInfoGPU), const_cast<VirtualTextureSystem::AtlasInfoGPU *>(&atlasInfo));

    TileUniforms tileUniforms{};
    tileUniforms.viewportWidth = settings_.viewPortWidth;
    tileUniforms.viewportHeight = settings_.viewPortHeight;
    tileUniforms.tileSize = settings_.tileSize;
    tileUniforms.tilesX = tilesX_;
    tileUniforms.tilesY = tilesY_;
    tileUniforms.materialCount = atlasInfo.materialCount;
    tileUniforms.maxDrawEntries = maxTileDrawEntries_;
    writeBuffer(tileUniformBuffer_, frameOverrideTileUniformBufferIds_, 0, sizeof(TileUniforms), &tileUniforms);

    if (!materialEntries.empty())
      renderGraph->bufferWrite(materialEntriesBuffer_, 0, materialEntries.size() * sizeof(VirtualTextureSystem::MaterialEntryGPU), const_cast<VirtualTextureSystem::MaterialEntryGPU *>(materialEntries.data()));
    if (!textureEntries.empty())
      renderGraph->bufferWrite(textureEntriesBuffer_, 0, textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU), const_cast<VirtualTextureSystem::TextureEntryGPU *>(textureEntries.data()));

    if (forceFullUpload || !gpuDataUploaded_ || virtualTextures.hasDirtyResidency())
    {
      if (!pageTableEntries.empty())
        renderGraph->bufferWrite(vtPageTableBuffer_, 0, pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU), const_cast<VirtualTextureSystem::PageTableEntryGPU *>(pageTableEntries.data()));

      if (forceFullUpload || !gpuDataUploaded_)
      {
        if (!physicalPages.empty())
          renderGraph->bufferWrite(vtPhysicalPagesBuffer_, 0, physicalPages.size() * sizeof(uint32_t), const_cast<uint32_t *>(physicalPages.data()));
      }
      else
      {
        const uint64_t pageTexelCount = static_cast<uint64_t>(virtualTextures.getSettings().pageSize) * virtualTextures.getSettings().pageSize;
        for (uint32_t dirtyPhysicalPage : virtualTextures.getDirtyPhysicalPages())
        {
          const uint64_t pixelOffset = static_cast<uint64_t>(dirtyPhysicalPage) * pageTexelCount;
          renderGraph->bufferWrite(vtPhysicalPagesBuffer_, pixelOffset * sizeof(uint32_t), pageTexelCount * sizeof(uint32_t), const_cast<uint32_t *>(physicalPages.data() + pixelOffset));
        }
      }
      virtualTextures.clearDirtyTracking();
    }

    gpuDataUploaded_ = true;
  }

  VirtualGeometryScene &scene_;
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_;
  rendering::Texture shadowLightingTexture_;
  Settings settings_;
  VirtualTextureSystem::FeedbackStats lastFeedbackStats_{};

  uint32_t tilesX_ = 1u;
  uint32_t tilesY_ = 1u;
  uint32_t tileCount_ = 1u;
  uint32_t maxTileDrawEntries_ = 1u;

  rendering::BindingsLayout computeLayout_;
  rendering::BindingsLayout materialDepthLayout_;
  rendering::BindingsLayout materialDrawLayout_;
  rendering::BindingGroups computeBindingGroups_;
  rendering::BindingGroups materialDepthBindingGroups_;
  rendering::BindingGroups materialDrawBindingGroups_;

  rendering::Shader fullscreenVertexShader_;
  rendering::Shader materialDepthFragmentShader_;
  rendering::Shader materialVertexShader_;
  rendering::Shader materialFragmentShader_;
  rendering::Shader materialTilesComputeShader_;

  rendering::ComputePipeline prepareTileMaterialsPipeline_;
  rendering::ComputePipeline prepareIndirectArgsPipeline_;
  rendering::GraphicsPipeline materialDepthPipeline_;
  rendering::GraphicsPipeline materialDrawPipeline_;

  rendering::Buffer quadUniformBuffer_;
  rendering::Buffer materialPassUniformBuffer_;
  rendering::Buffer tileUniformBuffer_;
  std::vector<rendering::BufferId> frameOverrideTileUniformBufferIds_;
  rendering::Buffer atlasInfoBuffer_;
  rendering::Buffer materialEntriesBuffer_;
  rendering::Buffer textureEntriesBuffer_;
  rendering::Buffer vtPageTableBuffer_;
  rendering::Buffer vtPhysicalPagesBuffer_;
  rendering::Buffer feedbackBuffer_;
  rendering::Buffer feedbackReadbackBuffer_;
  rendering::Buffer tileDrawEntriesBuffer_;
  std::vector<rendering::BufferId> frameOverrideTileDrawEntriesBufferIds_;
  rendering::Buffer tileDrawCountersBuffer_;
  std::vector<rendering::BufferId> frameOverrideTileDrawCountersBufferIds_;
  rendering::Buffer materialDrawIndirectBuffer_;
  std::vector<rendering::BufferId> frameOverrideMaterialDrawIndirectBufferIds_;
  rendering::Texture materialDepthTexture_;
  std::vector<rendering::TextureId> frameOverrideMaterialDepthTextureIds_;
  uint64_t feedbackBufferSize_ = 0u;
  bool gpuDataUploaded_ = false;

  void initializeFrameOverrideBufferSet(std::vector<rendering::BufferId> &ids, const std::string &baseName, uint64_t size, rendering::BufferUsage usage)
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    ids.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      ids[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = baseName + ".override_frame" + std::to_string(frameSlot),
              .size = size,
              .usage = usage,
          });
    }
  }

  void initializeFrameOverrideTextureSet(std::vector<rendering::TextureId> &ids, const std::string &baseName, const rendering::TextureInfo &info)
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    ids.assign(frameCount, rendering::TextureId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      rendering::TextureInfo frameInfo = info;
      frameInfo.name = baseName + ".override_frame" + std::to_string(frameSlot);
      ids[frameSlot] = renderGraph->getRHI()->createTexture(frameInfo);
    }
  }

  void initializeFrameOverrideResources()
  {
    initializeFrameOverrideBufferSet(
        frameOverrideTileUniformBufferIds_,
        tileUniformBuffer_.name,
        sizeof(TileUniforms),
        rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBufferSet(
        frameOverrideTileDrawEntriesBufferIds_,
        tileDrawEntriesBuffer_.name,
        renderGraph->getBufferSize(tileDrawEntriesBuffer_),
        rendering::BufferUsage::BufferUsage_Storage);
    initializeFrameOverrideBufferSet(
        frameOverrideTileDrawCountersBufferIds_,
        tileDrawCountersBuffer_.name,
        sizeof(TileDrawCounters),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBufferSet(
        frameOverrideMaterialDrawIndirectBufferIds_,
        materialDrawIndirectBuffer_.name,
        4u * sizeof(uint32_t),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideTextureSet(
        frameOverrideMaterialDepthTextureIds_,
        materialDepthTexture_.name,
        rendering::TextureInfo{
            .format = settings_.depthFormat,
            .height = settings_.viewPortHeight,
            .width = settings_.viewPortWidth,
            .depth = 1u,
            .mipLevels = 1u,
            .memoryProperties = rendering::BufferUsage::BufferUsage_None,
            .usage = rendering::ImageUsage::ImageUsage_DepthStencilAttachment,
        });
  }

  void destroyFrameOverrideBufferSet(std::vector<rendering::BufferId> &ids)
  {
    if (renderGraph == nullptr)
      return;
    for (const rendering::BufferId bufferId : ids)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    ids.clear();
  }

  void destroyFrameOverrideTextureSet(std::vector<rendering::TextureId> &ids)
  {
    if (renderGraph == nullptr)
      return;
    for (const rendering::TextureId textureId : ids)
    {
      if (textureId != rendering::TextureId::Invalid)
      {
        renderGraph->getRHI()->deleteTexture(textureId);
      }
    }
    ids.clear();
  }

  void destroyFrameOverrideResources()
  {
    destroyFrameOverrideBufferSet(frameOverrideTileUniformBufferIds_);
    destroyFrameOverrideBufferSet(frameOverrideTileDrawEntriesBufferIds_);
    destroyFrameOverrideBufferSet(frameOverrideTileDrawCountersBufferIds_);
    destroyFrameOverrideBufferSet(frameOverrideMaterialDrawIndirectBufferIds_);
    destroyFrameOverrideTextureSet(frameOverrideMaterialDepthTextureIds_);
  }

  rendering::BufferId getFrameOverrideBufferId(const std::vector<rendering::BufferId> &ids, uint32_t frameSlot) const
  {
    if (ids.empty())
      return rendering::BufferId::Invalid;
    return ids[frameSlot % ids.size()];
  }

  rendering::BufferId getCurrentFrameOverrideBufferId(const std::vector<rendering::BufferId> &ids) const
  {
    return getFrameOverrideBufferId(ids, renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

  rendering::TextureId getFrameOverrideTextureId(const std::vector<rendering::TextureId> &ids, uint32_t frameSlot) const
  {
    if (ids.empty())
      return rendering::TextureId::Invalid;
    return ids[frameSlot % ids.size()];
  }

  void writeBuffer(const rendering::Buffer &buffer, const std::vector<rendering::BufferId> &overrideIds, uint64_t offset, uint64_t size, const void *data)
  {
    const rendering::BufferId overrideId = getCurrentFrameOverrideBufferId(overrideIds);
    if (overrideId == rendering::BufferId::Invalid)
    {
      renderGraph->bufferWrite(buffer, offset, size, const_cast<void *>(data));
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(overrideId, offset, size, const_cast<void *>(data));
    }
  }

  void appendBufferOverride(rendering::RenderGraph::Overrides &overrides, const rendering::Buffer &buffer, const std::vector<rendering::BufferId> &overrideIds, uint32_t frameSlot) const
  {
    const rendering::BufferId overrideId = getFrameOverrideBufferId(overrideIds, frameSlot);
    if (overrideId == rendering::BufferId::Invalid)
      return;
    overrides.bufferOverrides.emplace(buffer.name, rendering::RenderGraphBufferOverride{.bufferId = overrideId});
  }

  void appendTextureOverride(rendering::RenderGraph::Overrides &overrides, const rendering::Texture &texture, const std::vector<rendering::TextureId> &overrideIds, uint32_t frameSlot) const
  {
    const rendering::TextureId overrideId = getFrameOverrideTextureId(overrideIds, frameSlot);
    if (overrideId == rendering::TextureId::Invalid)
      return;
    overrides.textureOverrides.emplace(texture.name, rendering::RenderGraphTextureOverride{.textureId = overrideId, .layout = rendering::ResourceLayout::UNDEFINED});
  }
};

} // namespace gpgpu
} // namespace virtualgeometry
