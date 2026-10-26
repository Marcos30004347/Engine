#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/MaterialTextureResolver.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class MaterialPass
{
public:
  struct Settings
  {
    uint32_t viewPortWidth = 1920u;
    uint32_t viewPortHeight = 1080u;
    uint32_t outputViewPortWidth = 1920u;
    uint32_t outputViewPortHeight = 1080u;
    rendering::Format colorFormat = rendering::Format::Format_RGBA16Float;
    rendering::Format depthFormat = rendering::Format::Format_Depth32Float;
    std::string shaderOverridePath;
    uint32_t feedbackRequestCapacity = 16384u;
    uint32_t feedbackStride = 2u;
    uint32_t maxUploadsPerFrame = 256u;
    uint32_t tileSize = 128u;
    uint32_t tileWorkgroupSize = 16u;
    float temporalRenderScale = 1.0f;
  };

  MaterialPass(VirtualGeometryScene &scene, VirtualGeometryHardwareDrawPass::FrameTarget frameTarget, rendering::Texture shadowLightingTexture, Settings settings)
      : scene_(scene), frameTarget_(frameTarget), shadowLightingTexture_(shadowLightingTexture), settings_(settings)
  {
  }

  ~MaterialPass()
  {
    if (renderGraph_ == nullptr)
      return;

    for (const auto &[_, bufferIds] : frameOverrideBufferIds_)
    {
      for (const rendering::BufferId bufferId : bufferIds)
      {
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph_->getRHI()->deleteBuffer(bufferId);
      }
    }

    for (auto pipeline : materialPipelines_)
      renderGraph_->deleteComputePipeline(pipeline);
    for (auto shader : materialShaders_)
      renderGraph_->deleteShader(shader);
    for (auto groups : materialBindingGroups_)
      renderGraph_->deleteBindingGroups(groups);
    for (auto buffer : materialRuntimeBuffers_)
      renderGraph_->deleteBuffer(buffer);

    renderGraph_->deleteComputePipeline(clearPipeline_);
    renderGraph_->deleteComputePipeline(cullPipeline_);
    renderGraph_->deleteComputePipeline(preparePipeline_);
    renderGraph_->deleteShader(materialPassShader_);
    renderGraph_->deleteBindingGroups(commonBindingGroups_);
    renderGraph_->deleteBindingsLayout(commonLayout_);
    renderGraph_->deleteBindingsLayout(materialLayout_);
    renderGraph_->deleteBuffer(passUniformBuffer_);
    renderGraph_->deleteBuffer(atlasInfoBuffer_);
    renderGraph_->deleteBuffer(materialTileBitsetBuffer_);
    renderGraph_->deleteBuffer(materialDispatchArgsBuffer_);
    renderGraph_->deleteBuffer(materialTileListBuffer_);
    renderGraph_->deleteBuffer(textureEntriesBuffer_);
    renderGraph_->deleteBuffer(vtPageTableBuffer_);
    renderGraph_->deleteBuffer(vtPhysicalPagesBuffer_);
    renderGraph_->deleteBuffer(feedbackBuffer_);
    renderGraph_->deleteBuffer(feedbackReadbackBuffer_);
  }

  void initialize(rendering::RenderGraph &renderGraph, const std::string &passName)
  {
    if (initialized_)
      return;

    renderGraph_ = &renderGraph;
    passName_ = passName;

    const auto &materialEntries = scene_.getVirtualTextureSystem().getMaterialEntries();
    const auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();
    materialDispatchCount_ = static_cast<uint32_t>(materialEntries.size());

    tilesX_ = (settings_.viewPortWidth + settings_.tileSize - 1u) / settings_.tileSize;
    tilesY_ = (settings_.viewPortHeight + settings_.tileSize - 1u) / settings_.tileSize;
    tileCount_ = tilesX_ * tilesY_;
    wordsPerMaterial_ = std::max(1u, (tileCount_ + 31u) / 32u);
    tileWorkgroupCountX_ = std::max(1u, (settings_.tileSize + std::max(1u, settings_.tileWorkgroupSize) - 1u) / std::max(1u, settings_.tileWorkgroupSize));
    tileWorkgroupCountY_ = std::max(1u, (settings_.tileSize + std::max(1u, settings_.tileWorkgroupSize) - 1u) / std::max(1u, settings_.tileWorkgroupSize));
    materialTileBitsetWords_ = materialDispatchCount_ * wordsPerMaterial_;

    passUniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_Uniforms.buffer",
          .size = sizeof(MaterialPassUniformsCPU),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    materialTileBitsetBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_MaterialTileBitset.buffer",
          .size = std::max<uint64_t>(sizeof(uint32_t), static_cast<uint64_t>(materialTileBitsetWords_) * sizeof(uint32_t)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    materialDispatchArgsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_MaterialDispatchArgs.buffer",
          .size = std::max<uint64_t>(sizeof(DispatchArgsCPU), static_cast<uint64_t>(std::max(1u, materialDispatchCount_)) * sizeof(DispatchArgsCPU)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push,
        });
    materialTileListBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_MaterialTileList.buffer",
          .size = std::max<uint64_t>(sizeof(uint32_t), static_cast<uint64_t>(std::max(1u, materialDispatchCount_)) * std::max<uint64_t>(1u, tileCount_) * sizeof(uint32_t)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    atlasInfoBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_AtlasInfo.buffer",
          .size = sizeof(VirtualTextureSystem::AtlasInfoGPU),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    textureEntriesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_TextureEntries.buffer",
          .size = std::max<uint64_t>(sizeof(VirtualTextureSystem::TextureEntryGPU), textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    vtPageTableBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_VirtualTexturePageTable.buffer",
          .size = std::max<uint64_t>(sizeof(VirtualTextureSystem::PageTableEntryGPU), pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    vtPhysicalPagesBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_VirtualTexturePhysicalPages.buffer",
          .size = std::max<uint64_t>(sizeof(uint32_t), physicalPages.size() * sizeof(uint32_t)),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    feedbackBufferSize_ = sizeof(VirtualTextureSystem::FeedbackHeaderGPU) + static_cast<uint64_t>(settings_.feedbackRequestCapacity) * sizeof(VirtualTextureSystem::FeedbackRequestGPU);
    feedbackBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_Feedback.buffer",
          .size = feedbackBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    feedbackReadbackBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName_ + "_Feedback.readback",
          .size = feedbackBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
        });

    initializeFrameOverrideBuffer(passUniformBuffer_, sizeof(MaterialPassUniformsCPU), rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        materialTileBitsetBuffer_,
        std::max<uint64_t>(sizeof(uint32_t), static_cast<uint64_t>(materialTileBitsetWords_) * sizeof(uint32_t)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        materialDispatchArgsBuffer_,
        std::max<uint64_t>(sizeof(DispatchArgsCPU), static_cast<uint64_t>(std::max(1u, materialDispatchCount_)) * sizeof(DispatchArgsCPU)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        materialTileListBuffer_,
        std::max<uint64_t>(sizeof(uint32_t), static_cast<uint64_t>(std::max(1u, materialDispatchCount_)) * std::max<uint64_t>(1u, tileCount_) * sizeof(uint32_t)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(atlasInfoBuffer_, sizeof(VirtualTextureSystem::AtlasInfoGPU), rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        textureEntriesBuffer_,
        std::max<uint64_t>(sizeof(VirtualTextureSystem::TextureEntryGPU), textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        vtPageTableBuffer_,
        std::max<uint64_t>(sizeof(VirtualTextureSystem::PageTableEntryGPU), pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(
        vtPhysicalPagesBuffer_,
        std::max<uint64_t>(sizeof(uint32_t), physicalPages.size() * sizeof(uint32_t)),
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
    initializeFrameOverrideBuffer(feedbackBuffer_, feedbackBufferSize_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc);
    initializeFrameOverrideBuffer(feedbackReadbackBuffer_, feedbackBufferSize_, rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull);

    writePassUniforms();
    initializeFrameLocalVirtualTextureState();
    createCommonResources();
    createMaterialResources();
    initialized_ = true;
  }

  void beginFrame()
  {
    if (!initialized_ || renderGraph_ == nullptr)
      return;

    syncVirtualTextureState(false);

    if (materialTileBitsetWords_ != 0u)
    {
      std::vector<uint32_t> zeroWords(materialTileBitsetWords_, 0u);
      writeBufferCurrentFrame(materialTileBitsetBuffer_, 0, static_cast<uint64_t>(zeroWords.size()) * sizeof(uint32_t), zeroWords.data());
    }

    if (materialDispatchCount_ != 0u)
    {
      std::vector<DispatchArgsCPU> dispatchArgs(materialDispatchCount_);
      for (auto &args : dispatchArgs)
      {
        args.groupCountX = tileWorkgroupCountX_;
        args.groupCountY = tileWorkgroupCountY_;
        args.groupCountZ = 0u;
      }
      writeBufferCurrentFrame(materialDispatchArgsBuffer_, 0, static_cast<uint64_t>(dispatchArgs.size()) * sizeof(DispatchArgsCPU), dispatchArgs.data());
    }

    VirtualTextureSystem::FeedbackHeaderGPU zeroHeader{};
    writeBufferCurrentFrame(feedbackBuffer_, 0, sizeof(zeroHeader), &zeroHeader);
  }

  bool processFeedback(uint32_t uploadBudget = 0u)
  {
    if (!initialized_ || renderGraph_ == nullptr || !feedbackReadbackBuffer_.isValid() || feedbackBufferSize_ == 0u)
      return false;

    VirtualTextureSystem::FeedbackHeaderGPU header{};
    readBufferCurrentFrame(
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
    readBufferCurrentFrame(
        feedbackReadbackBuffer_,
        sizeof(VirtualTextureSystem::FeedbackHeaderGPU),
        static_cast<uint64_t>(requestCount) * sizeof(VirtualTextureSystem::FeedbackRequestGPU),
        [&](const void *data)
        {
          std::memcpy(requests.data(), data, static_cast<size_t>(requestCount) * sizeof(VirtualTextureSystem::FeedbackRequestGPU));
        });

    uint32_t resolvedUploadBudget = uploadBudget;
    if (resolvedUploadBudget == 0u)
    {
      const uint32_t baseBudget = std::max(1u, settings_.maxUploadsPerFrame);
      const uint32_t pressureBudget = baseBudget + requestCount / 256u + header.overflowCount / 64u;
      resolvedUploadBudget = std::min(baseBudget * 4u, std::max(baseBudget, pressureBudget));
    }

    const bool changed = scene_.getVirtualTextureSystem().processFeedbackRequests(requests, resolvedUploadBudget, &lastFeedbackStats_);
    if (changed)
      syncVirtualTextureState(false);

    return changed;
  }

  const VirtualTextureSystem::FeedbackStats &getLastFeedbackStats() const
  {
    return lastFeedbackStats_;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    appendFrameOverride(overrides, passUniformBuffer_, frameSlot);
    appendFrameOverride(overrides, materialTileBitsetBuffer_, frameSlot);
    appendFrameOverride(overrides, materialDispatchArgsBuffer_, frameSlot);
    appendFrameOverride(overrides, materialTileListBuffer_, frameSlot);
    appendFrameOverride(overrides, atlasInfoBuffer_, frameSlot);
    appendFrameOverride(overrides, textureEntriesBuffer_, frameSlot);
    appendFrameOverride(overrides, vtPageTableBuffer_, frameSlot);
    appendFrameOverride(overrides, vtPhysicalPagesBuffer_, frameSlot);
    appendFrameOverride(overrides, feedbackBuffer_, frameSlot);
    appendFrameOverride(overrides, feedbackReadbackBuffer_, frameSlot);
    for (const rendering::Buffer &buffer : materialRuntimeBuffers_)
      appendFrameOverride(overrides, buffer, frameSlot);
  }

  void updateCamera(const math::Mat4f &, const math::Mat4f &, uint32_t, uint32_t)
  {
  }

  void recordSetupCommands(rendering::CommandRecorder &commandBuffer)
  {
    commandBuffer.cmdBindComputePipeline(clearPipeline_);
    commandBuffer.cmdBindBindingGroups(commonBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.viewPortWidth + settings_.tileWorkgroupSize - 1u) / settings_.tileWorkgroupSize,
        (settings_.viewPortHeight + settings_.tileWorkgroupSize - 1u) / settings_.tileWorkgroupSize,
        1u);

    if (materialDispatchCount_ == 0u)
      return;

    commandBuffer.cmdBindComputePipeline(cullPipeline_);
    commandBuffer.cmdBindBindingGroups(commonBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(tilesX_, tilesY_, 1u);
  }

  void recordPrepareCommands(rendering::CommandRecorder &commandBuffer)
  {
    if (materialDispatchCount_ == 0u)
      return;

    commandBuffer.cmdBindComputePipeline(preparePipeline_);
    commandBuffer.cmdBindBindingGroups(commonBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(wordsPerMaterial_, materialDispatchCount_, 1u);
  }

  void recordDispatchCommands(rendering::CommandRecorder &commandBuffer)
  {
    if (materialDispatchCount_ != 0u)
    {
      for (uint32_t materialIndex = 0u; materialIndex < materialDispatchCount_; ++materialIndex)
      {
        commandBuffer.cmdBindComputePipeline(materialPipelines_[materialIndex]);
        commandBuffer.cmdBindBindingGroups(materialBindingGroups_[materialIndex], nullptr, 0);
        commandBuffer.cmdDispatchIndirect(materialDispatchArgsBuffer_, static_cast<uint64_t>(materialIndex) * sizeof(DispatchArgsCPU));
      }
    }

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
  struct MaterialPassUniformsCPU
  {
    uint32_t viewportWidth = 1u;
    uint32_t viewportHeight = 1u;
    uint32_t tileSize = 128u;
    uint32_t tilesX = 1u;
    uint32_t tilesY = 1u;
    uint32_t tileCount = 1u;
    uint32_t wordsPerMaterial = 1u;
    uint32_t tileWorkgroupCountX = 16u;
    uint32_t tileWorkgroupCountY = 16u;
    uint32_t materialCount = 0u;
    uint32_t feedbackStride = 0u;
    uint32_t feedbackRequestCapacity = 0u;
    uint32_t displayViewportWidth = 1u;
    uint32_t displayViewportHeight = 1u;
    float temporalMipBias = 0.0f;
    float temporalRenderScale = 1.0f;
    float _padding0[2]{};
  };
  static_assert(sizeof(MaterialPassUniformsCPU) == 72u, "MaterialPassUniformsCPU must match material.wgsl MaterialPassUniforms");

  struct MaterialRuntimeGPU
  {
    uint32_t materialId = 0u;
    uint32_t tileListBase = 0u;
    uint32_t baseColorTextureId = Material::InvalidTextureId;
    uint32_t normalTextureId = Material::InvalidTextureId;
    uint32_t hasNormalTexture = 0u;
    uint32_t _padding0[3]{};
    float baseColorFactor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float pbrFactors[4]{1.0f, 0.0f, 1.0f, 0.0f};
    float emissiveFactorAndStrength[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(MaterialRuntimeGPU) == 80u, "MaterialRuntimeGPU must match material.wgsl MaterialRuntimeData");
  static_assert(offsetof(MaterialRuntimeGPU, baseColorFactor) == 32u, "MaterialRuntimeGPU baseColorFactor offset must match WGSL");
  static_assert(offsetof(MaterialRuntimeGPU, pbrFactors) == 48u, "MaterialRuntimeGPU pbrFactors offset must match WGSL");
  static_assert(offsetof(MaterialRuntimeGPU, emissiveFactorAndStrength) == 64u, "MaterialRuntimeGPU emissive offset must match WGSL");

  struct DispatchArgsCPU
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 0u;
    uint32_t groupCountZ = 0u;
  };

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph_ != nullptr && renderGraph_->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph_->createBuffer(info);
  }

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

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
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

  static std::string resolveShaderBinaryPath(const std::string &shaderPathStem)
  {
    if (shaderPathStem.size() >= 6u && shaderPathStem.substr(shaderPathStem.size() - 6u) == ".spirv")
      return shaderPathStem;
    return shaderPathStem + ".spirv";
  }

  static uint32_t resolveTextureId(const Material *material, const VirtualTextureSystem::MaterialEntryGPU &materialEntry, bool wantNormal)
  {
    return resolveMaterialTextureId(material, materialEntry, wantNormal);
  }

  void writePassUniforms()
  {
    MaterialPassUniformsCPU uniforms;
    uniforms.viewportWidth = settings_.viewPortWidth;
    uniforms.viewportHeight = settings_.viewPortHeight;
    uniforms.tileSize = settings_.tileSize;
    uniforms.tilesX = tilesX_;
    uniforms.tilesY = tilesY_;
    uniforms.tileCount = tileCount_;
    uniforms.wordsPerMaterial = wordsPerMaterial_;
    uniforms.tileWorkgroupCountX = tileWorkgroupCountX_;
    uniforms.tileWorkgroupCountY = tileWorkgroupCountY_;
    uniforms.materialCount = materialDispatchCount_;
    uniforms.feedbackStride = settings_.feedbackStride;
    uniforms.feedbackRequestCapacity = settings_.feedbackRequestCapacity;
    uniforms.displayViewportWidth = settings_.outputViewPortWidth;
    uniforms.displayViewportHeight = settings_.outputViewPortHeight;
    uniforms.temporalRenderScale = std::clamp(settings_.temporalRenderScale, 0.01f, 1.0f);
    uniforms.temporalMipBias = std::clamp(0.85f * std::log2(uniforms.temporalRenderScale), -2.0f, 0.0f);
    writeBufferAllFrames(passUniformBuffer_, 0, sizeof(uniforms), &uniforms);
  }

  void createCommonResources()
  {
    commonLayout_ = renderGraph_->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName_ + "_CommonLayout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "materialPassUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialTileBitset", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialDispatchArgs", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialTileList", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .textures = {
                {.name = "materialIdTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "sceneDepthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .storageTextures = {
                {.name = "outputTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "baseColorTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "normalTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialPropertiesTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "emissiveTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
          }},
        });

    commonBindingGroups_ = renderGraph_->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = commonLayout_,
          .name = passName_ + "_CommonGroups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                {.binding = 0, .bufferView = {.buffer = passUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(MaterialPassUniformsCPU)}},
                {.binding = 1, .bufferView = {.buffer = materialTileBitsetBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph_->getBufferSize(materialTileBitsetBuffer_)}},
                {.binding = 2, .bufferView = {.buffer = materialDispatchArgsBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph_->getBufferSize(materialDispatchArgsBuffer_)}},
                {.binding = 3, .bufferView = {.buffer = materialTileListBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph_->getBufferSize(materialTileListBuffer_)}},
              },
              .textures = {
                {.binding = 4, .textureView = makeSampledColorView(frameTarget_.materialIdView.texture)},
                {.binding = 5, .textureView = makeSampledDepthView(frameTarget_.depthTexture)},
              },
              .storageTextures = {
                {.binding = 6, .textureView = makeStorageColorView(frameTarget_.colorTexture)},
                {.binding = 7, .textureView = makeStorageColorView(frameTarget_.baseColorTexture)},
                {.binding = 8, .textureView = makeStorageColorView(frameTarget_.normalTexture)},
                {.binding = 9, .textureView = makeStorageColorView(frameTarget_.materialPropertiesTexture)},
                {.binding = 10, .textureView = makeStorageColorView(frameTarget_.emissiveTexture)},
              },
          }},
        });

    materialPassShader_ = renderGraph_->createShader(
        rendering::ShaderInfo{
          .name = passName_ + "_MaterialPassCS",
          .layout = commonLayout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/material-pass-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    clearPipeline_ = renderGraph_->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "clearColor",
          .layout = commonLayout_,
          .name = passName_ + "_Clear.pipeline",
          .shader = materialPassShader_,
        });
    cullPipeline_ = renderGraph_->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "cullMaterials",
          .layout = commonLayout_,
          .name = passName_ + "_Cull.pipeline",
          .shader = materialPassShader_,
        });
    preparePipeline_ = renderGraph_->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "prepareMaterialDispatches",
          .layout = commonLayout_,
          .name = passName_ + "_Prepare.pipeline",
          .shader = materialPassShader_,
        });

    materialLayout_ = renderGraph_->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName_ + "_MaterialLayout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                {.name = "materialPassUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialRuntime", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialTileList", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "atlasInfo", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "textureEntries", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "vtPageTable", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "vtPhysicalPages", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "feedbackBuffer", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "instances", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "scenePageTable", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "scenePagesBuffer", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .textures = {
                {.name = "materialIdTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialUVTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "shadowLightingTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "sceneDepthTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "packedGeometryIdsLoTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "packedGeometryIdsHiTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "geometryNormalTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .storageTextures = {
                {.name = "outputTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "baseColorTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "normalTexture", .binding = 20, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "materialPropertiesTexture", .binding = 21, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "emissiveTexture", .binding = 22, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
          }},
        });
  }

  void createMaterialResources()
  {
    const auto &materialManager = scene_.getMaterialManager();

    materialRuntimeBuffers_.resize(materialDispatchCount_);
    materialBindingGroups_.resize(materialDispatchCount_);
    materialShaders_.resize(materialDispatchCount_);
    materialPipelines_.resize(materialDispatchCount_);

    for (uint32_t materialIndex = 0u; materialIndex < materialDispatchCount_; ++materialIndex)
    {
      MaterialRuntimeGPU runtimeData{};
      runtimeData.materialId = materialIndex;
      runtimeData.tileListBase = materialIndex * tileCount_;
      const auto &materialEntries = scene_.getVirtualTextureSystem().getMaterialEntries();
      const VirtualTextureSystem::MaterialEntryGPU emptyMaterialEntry{};
      const VirtualTextureSystem::MaterialEntryGPU &materialEntry =
          materialIndex < materialEntries.size() ? materialEntries[materialIndex] : emptyMaterialEntry;
      const Material *material = materialManager.getMaterial(materialIndex);
      runtimeData.baseColorTextureId = resolveTextureId(material, materialEntry, false);
      runtimeData.normalTextureId = resolveTextureId(material, materialEntry, true);
      runtimeData.hasNormalTexture = runtimeData.normalTextureId != Material::InvalidTextureId ? 1u : 0u;
      if (material != nullptr)
      {
        const float *baseColorFactor = material->getBaseColorFactor();
        std::copy(baseColorFactor, baseColorFactor + 4, std::begin(runtimeData.baseColorFactor));
        runtimeData.pbrFactors[0] = material->getRoughnessFactor();
        runtimeData.pbrFactors[1] = material->getMetallicFactor();
        runtimeData.pbrFactors[2] = material->getOpacity();
        const float *emissiveFactor = material->getEmissiveFactor();
        runtimeData.emissiveFactorAndStrength[0] = emissiveFactor[0];
        runtimeData.emissiveFactorAndStrength[1] = emissiveFactor[1];
        runtimeData.emissiveFactorAndStrength[2] = emissiveFactor[2];
        runtimeData.emissiveFactorAndStrength[3] = material->getEmissiveStrength();
      }

      materialRuntimeBuffers_[materialIndex] = createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName_ + "_MaterialRuntime_" + std::to_string(materialIndex) + ".buffer",
            .size = sizeof(MaterialRuntimeGPU),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
      initializeFrameOverrideBuffer(materialRuntimeBuffers_[materialIndex], sizeof(MaterialRuntimeGPU), rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push);
      writeBufferAllFrames(materialRuntimeBuffers_[materialIndex], 0, sizeof(runtimeData), &runtimeData);

      materialBindingGroups_[materialIndex] = renderGraph_->createBindingGroups(
          rendering::BindingGroupsInfo{
            .layout = materialLayout_,
            .name = passName_ + "_MaterialBindingGroups_" + std::to_string(materialIndex),
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                  {.binding = 0, .bufferView = {.buffer = passUniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(MaterialPassUniformsCPU)}},
                  {.binding = 1, .bufferView = {.buffer = materialRuntimeBuffers_[materialIndex], .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(MaterialRuntimeGPU)}},
                  {.binding = 2, .bufferView = {.buffer = materialTileListBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph_->getBufferSize(materialTileListBuffer_)}},
                  {.binding = 12, .bufferView = {.buffer = atlasInfoBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph_->getBufferSize(atlasInfoBuffer_)}},
                  {.binding = 13, .bufferView = {.buffer = textureEntriesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph_->getBufferSize(textureEntriesBuffer_)}},
                  {.binding = 14, .bufferView = {.buffer = vtPageTableBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph_->getBufferSize(vtPageTableBuffer_)}},
                  {.binding = 15, .bufferView = {.buffer = vtPhysicalPagesBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph_->getBufferSize(vtPhysicalPagesBuffer_)}},
                  {.binding = 16, .bufferView = {.buffer = feedbackBuffer_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = feedbackBufferSize_}},
                  {.binding = 17, .bufferView = {.buffer = scene_.instanceBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.instanceBufferSize}},
                  {.binding = 18, .bufferView = {.buffer = scene_.pageTableBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesTableBufferSize}},
                  {.binding = 19, .bufferView = {.buffer = scene_.pagesBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesBufferSize}},
                },
                .textures = {
                  {.binding = 3, .textureView = makeSampledColorView(frameTarget_.materialIdView.texture)},
                  {.binding = 4, .textureView = makeSampledColorView(frameTarget_.materialUVView.texture)},
                  {.binding = 5, .textureView = makeSampledColorView(shadowLightingTexture_)},
                  {.binding = 6, .textureView = makeSampledDepthView(frameTarget_.depthTexture)},
                  {.binding = 7, .textureView = makeSampledColorView(frameTarget_.packedGeometryIdsLoView.texture)},
                  {.binding = 8, .textureView = makeSampledColorView(frameTarget_.packedGeometryIdsHiView.texture)},
                  {.binding = 9, .textureView = makeSampledColorView(frameTarget_.geometryNormalTexture)},
                },
                .storageTextures = {
                  {.binding = 10, .textureView = makeStorageColorView(frameTarget_.colorTexture)},
                  {.binding = 11, .textureView = makeStorageColorView(frameTarget_.baseColorTexture)},
                  {.binding = 20, .textureView = makeStorageColorView(frameTarget_.normalTexture)},
                  {.binding = 21, .textureView = makeStorageColorView(frameTarget_.materialPropertiesTexture)},
                  {.binding = 22, .textureView = makeStorageColorView(frameTarget_.emissiveTexture)},
                },
            }},
          });

      const std::string shaderStem = settings_.shaderOverridePath.empty()
                                         ? (material != nullptr ? material->getShaderPath() : Material::defaultShaderPath())
                                         : settings_.shaderOverridePath;
      const std::string shaderPath = resolveShaderBinaryPath(shaderStem);
      const std::string fallbackShaderPath = resolveShaderBinaryPath(Material::defaultShaderPath());
      const std::filesystem::path executableRelativeShader = std::filesystem::path(os::io::getExecutableDirectory()) / shaderPath;
      const std::string chosenShaderPath = std::filesystem::exists(executableRelativeShader) ? shaderPath : fallbackShaderPath;

      materialShaders_[materialIndex] = renderGraph_->createShader(
          rendering::ShaderInfo{
            .name = passName_ + "_MaterialShader_" + std::to_string(materialIndex),
            .layout = materialLayout_,
            .src = os::io::readRelativeFile(chosenShaderPath),
            .type = rendering::ShaderType::SpirV,
          });
      materialPipelines_[materialIndex] = renderGraph_->createComputePipeline(
          rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = materialLayout_,
            .name = passName_ + "_MaterialPipeline_" + std::to_string(materialIndex),
            .shader = materialShaders_[materialIndex],
          });
    }
  }

  void initializeFrameOverrideBuffer(const rendering::Buffer &buffer, uint64_t size, rendering::BufferUsage usage)
  {
    if (renderGraph_ == nullptr)
      return;

    const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
    if (frameCount <= 1u)
      return;

    auto &bufferIds = frameOverrideBufferIds_[buffer.name];
    bufferIds.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      bufferIds[frameSlot] = renderGraph_->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = buffer.name + ".override_frame" + std::to_string(frameSlot),
              .size = size,
              .usage = usage,
          });
    }
  }

  rendering::BufferId getFrameOverrideBufferId(const rendering::Buffer &buffer, uint32_t frameSlot) const
  {
    const auto it = frameOverrideBufferIds_.find(buffer.name);
    if (it == frameOverrideBufferIds_.end() || it->second.empty())
      return rendering::BufferId::Invalid;
    return it->second[frameSlot % it->second.size()];
  }

  void appendFrameOverride(rendering::RenderGraph::Overrides &overrides, const rendering::Buffer &buffer, uint32_t frameSlot) const
  {
    const rendering::BufferId bufferId = getFrameOverrideBufferId(buffer, frameSlot);
    if (bufferId != rendering::BufferId::Invalid)
      overrides.bufferOverrides.emplace(buffer.name, rendering::RenderGraphBufferOverride{.bufferId = bufferId});
  }

  void writeBufferForFrame(const rendering::Buffer &buffer, uint32_t frameSlot, uint64_t offset, uint64_t size, const void *data)
  {
    if (renderGraph_ == nullptr)
      return;

    const rendering::BufferId overrideBufferId = getFrameOverrideBufferId(buffer, frameSlot);
    if (overrideBufferId != rendering::BufferId::Invalid)
    {
      renderGraph_->getRHI()->bufferWrite(overrideBufferId, offset, size, const_cast<void *>(data));
      return;
    }

    const uint32_t previousFrameSlot = renderGraph_->getCurrentFrameIndex();
    renderGraph_->setCurrentFrameIndex(frameSlot);
    renderGraph_->bufferWrite(buffer, offset, size, const_cast<void *>(data));
    renderGraph_->setCurrentFrameIndex(previousFrameSlot);
  }

  void writeBufferCurrentFrame(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, const void *data)
  {
    writeBufferForFrame(buffer, getCurrentFrameSlot(), offset, size, data);
  }

  template <typename Fn> void readBufferCurrentFrame(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, Fn &&fn)
  {
    if (renderGraph_ == nullptr)
      return;

    const rendering::BufferId overrideBufferId = getFrameOverrideBufferId(buffer, getCurrentFrameSlot());
    if (overrideBufferId != rendering::BufferId::Invalid)
    {
      renderGraph_->getRHI()->bufferRead(overrideBufferId, offset, size, std::forward<Fn>(fn));
      return;
    }

    renderGraph_->bufferRead(buffer, offset, size, std::forward<Fn>(fn));
  }

  void writeBufferAllFrames(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, const void *data)
  {
    if (renderGraph_ == nullptr)
      return;

    const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
      writeBufferForFrame(buffer, frameSlot, offset, size, data);
  }

  void syncVirtualTextureState(bool forceFullUpload)
  {
    if (renderGraph_ == nullptr || !atlasInfoBuffer_.isValid())
      return;

    capturePendingVirtualTextureChanges();

    const uint32_t frameSlot = getCurrentFrameSlot();
    const bool needsFullUpload = forceFullUpload || frameSlot >= frameVirtualTextureInitialized_.size() || frameVirtualTextureInitialized_[frameSlot] == 0u;
    const bool hasPendingSync = frameSlot < frameVirtualTextureSyncPending_.size() && frameVirtualTextureSyncPending_[frameSlot] != 0u;
    if (!needsFullUpload && !hasPendingSync)
      return;

    uploadVirtualTextureStateForFrame(frameSlot, needsFullUpload);

    if (frameSlot < frameVirtualTextureInitialized_.size())
      frameVirtualTextureInitialized_[frameSlot] = 1u;
    if (frameSlot < frameVirtualTextureSyncPending_.size())
      frameVirtualTextureSyncPending_[frameSlot] = 0u;

    const bool hasRemainingPendingFrames = std::any_of(
        frameVirtualTextureSyncPending_.begin(),
        frameVirtualTextureSyncPending_.end(),
        [](uint8_t pending)
        {
          return pending != 0u;
        });
    if (!hasRemainingPendingFrames)
      clearPendingVirtualTextureChanges();
  }

  uint32_t getCurrentFrameSlot() const
  {
    if (renderGraph_ == nullptr)
      return 0u;
    const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
    return renderGraph_->getCurrentFrameIndex() % frameCount;
  }

  void initializeFrameLocalVirtualTextureState()
  {
    if (renderGraph_ == nullptr)
      return;

    const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
    frameVirtualTextureInitialized_.assign(frameCount, 0u);
    frameVirtualTextureSyncPending_.assign(frameCount, 0u);
    pendingDirtyPhysicalPageFlags_.assign(std::max(1u, scene_.getVirtualTextureSystem().getAtlasInfo().totalPhysicalPages), 0u);

    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      uploadVirtualTextureStateForFrame(frameSlot, true);
      frameVirtualTextureInitialized_[frameSlot] = 1u;
      frameVirtualTextureSyncPending_[frameSlot] = 0u;
    }
    scene_.getVirtualTextureSystem().clearDirtyTracking();
    clearPendingVirtualTextureChanges();
  }

  void capturePendingVirtualTextureChanges()
  {
    auto &virtualTextures = scene_.getVirtualTextureSystem();
    if (!virtualTextures.hasDirtyResidency())
      return;

    pendingPageTableUpload_ = true;
    const auto &dirtyPhysicalPages = virtualTextures.getDirtyPhysicalPages();
    for (uint32_t dirtyPhysicalPage : dirtyPhysicalPages)
    {
      if (dirtyPhysicalPage >= pendingDirtyPhysicalPageFlags_.size())
        continue;
      if (pendingDirtyPhysicalPageFlags_[dirtyPhysicalPage] != 0u)
        continue;
      pendingDirtyPhysicalPageFlags_[dirtyPhysicalPage] = 1u;
      pendingDirtyPhysicalPages_.push_back(dirtyPhysicalPage);
    }

    std::fill(frameVirtualTextureSyncPending_.begin(), frameVirtualTextureSyncPending_.end(), 1u);
    virtualTextures.clearDirtyTracking();
  }

  void clearPendingVirtualTextureChanges()
  {
    pendingPageTableUpload_ = false;
    pendingDirtyPhysicalPages_.clear();
    std::fill(pendingDirtyPhysicalPageFlags_.begin(), pendingDirtyPhysicalPageFlags_.end(), 0u);
  }

  void uploadVirtualTextureStateForFrame(uint32_t frameSlot, bool fullUpload)
  {
    auto &virtualTextures = scene_.getVirtualTextureSystem();
    const auto &atlasInfo = virtualTextures.getAtlasInfo();
    const auto &textureEntries = virtualTextures.getTextureEntries();
    const auto &pageTableEntries = virtualTextures.getPageTableEntries();
    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();

    writeBufferForFrame(atlasInfoBuffer_, frameSlot, 0, sizeof(VirtualTextureSystem::AtlasInfoGPU), &atlasInfo);
    if (!textureEntries.empty())
      writeBufferForFrame(textureEntriesBuffer_, frameSlot, 0, textureEntries.size() * sizeof(VirtualTextureSystem::TextureEntryGPU), textureEntries.data());

    if (fullUpload || pendingPageTableUpload_)
    {
      if (!pageTableEntries.empty())
        writeBufferForFrame(vtPageTableBuffer_, frameSlot, 0, pageTableEntries.size() * sizeof(VirtualTextureSystem::PageTableEntryGPU), pageTableEntries.data());
    }

    if (fullUpload)
    {
      if (!physicalPages.empty())
        writeBufferForFrame(vtPhysicalPagesBuffer_, frameSlot, 0, physicalPages.size() * sizeof(uint32_t), physicalPages.data());
      return;
    }

    if (pendingDirtyPhysicalPages_.empty())
      return;

    const uint64_t pageTexelCount = static_cast<uint64_t>(virtualTextures.getSettings().pageSize) * virtualTextures.getSettings().pageSize;
    for (uint32_t dirtyPhysicalPage : pendingDirtyPhysicalPages_)
    {
      const uint64_t pixelOffset = static_cast<uint64_t>(dirtyPhysicalPage) * pageTexelCount;
      writeBufferForFrame(vtPhysicalPagesBuffer_, frameSlot, pixelOffset * sizeof(uint32_t), pageTexelCount * sizeof(uint32_t), physicalPages.data() + pixelOffset);
    }
  }

  VirtualGeometryScene &scene_;
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_;
  rendering::Texture shadowLightingTexture_;
  Settings settings_;

  rendering::RenderGraph *renderGraph_ = nullptr;
  std::string passName_;
  uint32_t tilesX_ = 1u;
  uint32_t tilesY_ = 1u;
  uint32_t tileCount_ = 1u;
  uint32_t wordsPerMaterial_ = 1u;
  uint32_t tileWorkgroupCountX_ = 16u;
  uint32_t tileWorkgroupCountY_ = 16u;
  uint32_t materialDispatchCount_ = 0u;
  uint32_t materialTileBitsetWords_ = 0u;
  uint64_t feedbackBufferSize_ = 0u;
  VirtualTextureSystem::FeedbackStats lastFeedbackStats_{};
  bool initialized_ = false;
  std::vector<uint8_t> frameVirtualTextureInitialized_;
  std::vector<uint8_t> frameVirtualTextureSyncPending_;
  std::vector<uint32_t> pendingDirtyPhysicalPages_;
  std::vector<uint8_t> pendingDirtyPhysicalPageFlags_;
  bool pendingPageTableUpload_ = false;
  std::unordered_map<std::string, std::vector<rendering::BufferId>> frameOverrideBufferIds_;

  rendering::BindingsLayout commonLayout_;
  rendering::BindingsLayout materialLayout_;
  rendering::BindingGroups commonBindingGroups_;
  rendering::Shader materialPassShader_;
  rendering::ComputePipeline clearPipeline_;
  rendering::ComputePipeline cullPipeline_;
  rendering::ComputePipeline preparePipeline_;

  rendering::Buffer passUniformBuffer_;
  rendering::Buffer atlasInfoBuffer_;
  rendering::Buffer materialTileBitsetBuffer_;
  rendering::Buffer materialDispatchArgsBuffer_;
  rendering::Buffer materialTileListBuffer_;
  rendering::Buffer textureEntriesBuffer_;
  rendering::Buffer vtPageTableBuffer_;
  rendering::Buffer vtPhysicalPagesBuffer_;
  rendering::Buffer feedbackBuffer_;
  rendering::Buffer feedbackReadbackBuffer_;
  std::vector<rendering::Buffer> materialRuntimeBuffers_;
  std::vector<rendering::BindingGroups> materialBindingGroups_;
  std::vector<rendering::Shader> materialShaders_;
  std::vector<rendering::ComputePipeline> materialPipelines_;
};

class MaterialSetupPass : public rendering::Pass
{
public:
  explicit MaterialSetupPass(MaterialPass &materialPass) : materialPass_(materialPass) {}

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    materialPass_.recordSetupCommands(commandBuffer);
  }

private:
  MaterialPass &materialPass_;
};

class MaterialPreparePass : public rendering::Pass
{
public:
  explicit MaterialPreparePass(MaterialPass &materialPass) : materialPass_(materialPass) {}

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    materialPass_.recordPrepareCommands(commandBuffer);
  }

private:
  MaterialPass &materialPass_;
};

class MaterialDispatchPass : public rendering::Pass
{
public:
  explicit MaterialDispatchPass(MaterialPass &materialPass) : materialPass_(materialPass) {}

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    materialPass_.recordDispatchCommands(commandBuffer);
  }

private:
  MaterialPass &materialPass_;
};

} // namespace gpgpu
} // namespace virtualgeometry
