#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsTracePass : public rendering::Pass
{
public:
  static constexpr uint32_t kMaxTraceCascades = 8u;

  struct Settings
  {
    uint32_t rayCount = 1u;
    uint32_t rayOffset = 0u;
    uint32_t hitOffset = 0u;
    uint32_t maxSteps = 64u;
    float maxTraceDistance = 128.0f;
    float hitThresholdScale = 0.75f;
    float emptySpaceStepScale = 1.0f;
    float normalBias = 0.01f;
    int32_t cascadeOverride = -1;
    bool traceStatsEnabled = false;
    bool surfaceCacheMaterialsEnabled = true;
  };

  GPUSignedDistanceFieldsTracePass(
      GPUSignedDistanceFieldsManager &manager,
      rendering::Buffer inputRayBuffer,
      rendering::Buffer outputHitBuffer,
      Settings settings,
      rendering::Buffer traceStatsBuffer = {})
      : manager_(manager),
        inputRayBuffer_(inputRayBuffer),
        outputHitBuffer_(outputHitBuffer),
        traceStatsBuffer_(traceStatsBuffer),
        settings_(settings)
  {
    if (manager_.getCascadeCount() > kMaxTraceCascades)
    {
      throw std::runtime_error("GPUSignedDistanceFieldsTracePass supports up to 8 cascades");
    }
  }

  ~GPUSignedDistanceFieldsTracePass() override
  {
    destroyFrameOverrideBuffers(frameOverrideUniformBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideCascadeInfoBufferIds_);
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
    if (cascadeInfoBuffer_.isValid())
      renderGraph->deleteBuffer(cascadeInfoBuffer_);
    if (fallbackBrickMapBuffer_.isValid())
      renderGraph->deleteBuffer(fallbackBrickMapBuffer_);
    if (fallbackAabbTreeBuffer_.isValid())
      renderGraph->deleteBuffer(fallbackAabbTreeBuffer_);
    if (sdfAtlasSampler_.isValid())
      renderGraph->deleteSampler(sdfAtlasSampler_);
    if (fallbackTraceStatsBuffer_.isValid())
      renderGraph->deleteBuffer(fallbackTraceStatsBuffer_);
    if (fallbackSurfaceCacheMaterialReferenceAtlas_.isValid())
      renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceAtlas_);
    if (fallbackSurfaceCacheMaterialReferenceBasisAtlas_.isValid())
      renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceBasisAtlas_);
  }

  static constexpr uint64_t getInputRayBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount) * sizeof(TraceRayGPU);
  }

  static constexpr uint64_t getOutputHitBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount) * sizeof(TraceHitGPU);
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    const rendering::BufferId uniformOverride = getFrameOverrideBufferId(frameOverrideUniformBufferIds_, frameSlot);
    if (uniformOverride != rendering::BufferId::Invalid)
    {
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = uniformOverride});
    }

    const rendering::BufferId cascadeInfoOverride = getFrameOverrideBufferId(frameOverrideCascadeInfoBufferIds_, frameSlot);
    if (cascadeInfoOverride != rendering::BufferId::Invalid)
    {
      overrides.bufferOverrides.emplace(cascadeInfoBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = cascadeInfoOverride});
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();
    writeCascadeInfos();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch((settings_.rayCount + kWorkgroupSize - 1u) / kWorkgroupSize, 1u, 1u);
  }

private:
  static constexpr uint32_t kWorkgroupSize = 64u;

  struct Uniforms
  {
    uint32_t rayCount = 1u;
    uint32_t rayOffset = 0u;
    uint32_t hitOffset = 0u;
    uint32_t traceStatsEnabled = 0u;
    uint32_t maxSteps = 64u;
    uint32_t atlasBricksPerAxis = 0u;
    uint32_t cascadeCount = 0u;
    int32_t cascadeOverride = -1;
    uint32_t surfaceCachePageResolution = 0u;
    uint32_t surfaceCachePagesPerRow = 0u;
    uint32_t _padding0[2]{};
    float hitThresholdScale = 0.75f;
    float emptySpaceStepScale = 1.0f;
    float normalBias = 0.01f;
    float maxTraceDistance = 128.0f;
  };

  static rendering::TextureView makeSampledColor3DView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledColor2DView(rendering::Texture texture)
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
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
      initializeFrameOverrideBuffers(uniformBuffer_, rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push, frameOverrideUniformBufferIds_);
    }
    if (!cascadeInfoBuffer_.isValid())
    {
      cascadeInfoBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_CascadeInfos.buffer",
              .size = sizeof(GPUSignedDistanceFieldsManager::CascadeInfoGPU) * kMaxTraceCascades,
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
      initializeFrameOverrideBuffers(cascadeInfoBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverrideCascadeInfoBufferIds_);
    }
    if (!fallbackBrickMapBuffer_.isValid())
    {
      fallbackBrickMapBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_FallbackBrickMap.buffer",
              .size = sizeof(uint32_t),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
      uint32_t invalidBrick = GPUSignedDistanceFieldsManager::INVALID_ID;
      renderGraph->bufferWrite(fallbackBrickMapBuffer_, 0u, sizeof(uint32_t), &invalidBrick);
    }
    if (!fallbackAabbTreeBuffer_.isValid())
    {
      fallbackAabbTreeBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_FallbackAabbTree.buffer",
              .size = GPUSignedDistanceFieldsManager::BUILD_TREE_FLOAT4_COUNT * sizeof(float) * 4u,
              .usage = rendering::BufferUsage::BufferUsage_Storage,
          });
    }
    if (!sdfAtlasSampler_.isValid())
    {
      sdfAtlasSampler_ = renderGraph->createSampler(
          rendering::SamplerInfo{
              .name = passName + "_SdfAtlasSampler",
              .minFilter = rendering::Filter::Linear,
              .magFilter = rendering::Filter::Linear,
              .addressModeU = rendering::SamplerAddressMode::ClampToEdge,
              .addressModeV = rendering::SamplerAddressMode::ClampToEdge,
              .addressModeW = rendering::SamplerAddressMode::ClampToEdge,
              .anisotropyEnable = false,
              .maxAnisotropy = 1.0f,
              .maxLod = 1.0f,
          });
    }
    if (!fallbackTraceStatsBuffer_.isValid())
    {
      fallbackTraceStatsBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_FallbackTraceStats.buffer",
              .size = getTraceStatsBufferSize(),
              .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!fallbackSurfaceCacheMaterialReferenceAtlas_.isValid())
    {
      fallbackSurfaceCacheMaterialReferenceAtlas_ = renderGraph->createTexture(
          rendering::TextureInfo{
              .name = passName + "_FallbackSurfaceCacheMaterialReferenceAtlas.texture",
              .format = rendering::Format::Format_RGBA32Uint,
              .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
              .usage = rendering::ImageUsage::ImageUsage_Sampled,
              .width = 1u,
              .height = 1u,
              .depth = 1u,
              .mipLevels = 1u,
          });
    }
    if (!fallbackSurfaceCacheMaterialReferenceBasisAtlas_.isValid())
    {
      fallbackSurfaceCacheMaterialReferenceBasisAtlas_ = renderGraph->createTexture(
          rendering::TextureInfo{
              .name = passName + "_FallbackSurfaceCacheMaterialReferenceBasisAtlas.texture",
              .format = rendering::Format::Format_RGBA32Uint,
              .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
              .usage = rendering::ImageUsage::ImageUsage_Sampled,
              .width = 1u,
              .height = 1u,
              .depth = 1u,
              .mipLevels = 1u,
          });
    }

    if (layout_.isValid())
    {
      return;
    }

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {
                rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeInfos", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "traceRays", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "traceHits", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap0", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap1", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap2", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap3", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap4", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap5", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap6", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap7", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickAabb", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree0", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree1", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree2", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree3", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree4", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree5", .binding = 20, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree6", .binding = 21, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree7", .binding = 22, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "traceStats", .binding = 23, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                    .samplers = {
                        {.name = "sdfAtlasSampler", .binding = 13, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                    .textures = {
                        {.name = "sdfAtlasTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "surfaceCacheMaterialReferenceAtlas", .binding = 26, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "surfaceCacheMaterialReferenceBasisAtlas", .binding = 27, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                },
            },
        });

    const auto &shared = manager_.getSharedResources();
    const auto &combined = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Combined);
    auto brickMapView = [&](uint32_t cascadeIndex) -> rendering::BufferView
    {
      if (cascadeIndex < combined.cascades.size())
      {
        const auto &cascade = combined.cascades[cascadeIndex];
        return rendering::BufferView{
            .buffer = cascade.brickMap,
            .offset = cascade.brickMapOffset,
            .size = cascade.brickMapSize,
            .access = rendering::AccessPattern::SHADER_READ,
        };
      }

      return rendering::BufferView{
          .buffer = fallbackBrickMapBuffer_,
          .offset = 0u,
          .size = sizeof(uint32_t),
          .access = rendering::AccessPattern::SHADER_READ,
      };
    };
    auto aabbTreeView = [&](uint32_t cascadeIndex) -> rendering::BufferView
    {
      if (cascadeIndex < combined.cascades.size())
      {
        const auto &cascade = combined.cascades[cascadeIndex];
        return rendering::BufferView{
            .buffer = cascade.aabbTree,
            .offset = cascade.aabbTreeOffset,
            .size = cascade.aabbTreeSize,
            .access = rendering::AccessPattern::SHADER_READ,
        };
      }

      return rendering::BufferView{
          .buffer = fallbackAabbTreeBuffer_,
          .offset = 0u,
          .size = GPUSignedDistanceFieldsManager::BUILD_TREE_FLOAT4_COUNT * sizeof(float) * 4u,
          .access = rendering::AccessPattern::SHADER_READ,
      };
    };
    const rendering::Buffer boundTraceStatsBuffer = traceStatsBuffer_.isValid() ? traceStatsBuffer_ : fallbackTraceStatsBuffer_;
    const rendering::Texture surfaceCacheMaterialReferenceAtlas =
        shared.surfaceCacheMaterialReferenceAtlas.isValid() ? shared.surfaceCacheMaterialReferenceAtlas : fallbackSurfaceCacheMaterialReferenceAtlas_;
    const rendering::Texture surfaceCacheMaterialReferenceBasisAtlas =
        shared.surfaceCacheMaterialReferenceBasisAtlas.isValid() ? shared.surfaceCacheMaterialReferenceBasisAtlas : fallbackSurfaceCacheMaterialReferenceBasisAtlas_;

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = shared.cascadeInfos, .offset = 0u, .size = shared.cascadeInfosSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = inputRayBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(inputRayBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = outputHitBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(outputHitBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 4, .bufferView = brickMapView(0u)},
                        {.binding = 5, .bufferView = brickMapView(1u)},
                        {.binding = 6, .bufferView = brickMapView(2u)},
                        {.binding = 7, .bufferView = brickMapView(3u)},
                        {.binding = 8, .bufferView = brickMapView(4u)},
                        {.binding = 9, .bufferView = brickMapView(5u)},
                        {.binding = 10, .bufferView = brickMapView(6u)},
                        {.binding = 11, .bufferView = brickMapView(7u)},
                        {.binding = 14, .bufferView = {.buffer = shared.brickAABB, .offset = 0u, .size = shared.brickAABBSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 15, .bufferView = aabbTreeView(0u)},
                        {.binding = 16, .bufferView = aabbTreeView(1u)},
                        {.binding = 17, .bufferView = aabbTreeView(2u)},
                        {.binding = 18, .bufferView = aabbTreeView(3u)},
                        {.binding = 19, .bufferView = aabbTreeView(4u)},
                        {.binding = 20, .bufferView = aabbTreeView(5u)},
                        {.binding = 21, .bufferView = aabbTreeView(6u)},
                        {.binding = 22, .bufferView = aabbTreeView(7u)},
                        {.binding = 23, .bufferView = {.buffer = boundTraceStatsBuffer, .offset = 0u, .size = getTraceStatsBufferSize(), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                    },
                    .samplers = {
                        {.sampler = sdfAtlasSampler_, .view = makeSampledColor3DView(shared.sdfAtlas), .binding = 13},
                    },
                    .textures = {
                        {.binding = 12, .textureView = makeSampledColor3DView(shared.sdfAtlas)},
                        {.binding = 26, .textureView = makeSampledColor2DView(surfaceCacheMaterialReferenceAtlas)},
                        {.binding = 27, .textureView = makeSampledColor2DView(surfaceCacheMaterialReferenceBasisAtlas)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-trace.spirv"),
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
    Uniforms uniforms{};
    uniforms.rayCount = settings_.rayCount;
    uniforms.rayOffset = settings_.rayOffset;
    uniforms.hitOffset = settings_.hitOffset;
    uniforms.traceStatsEnabled = settings_.traceStatsEnabled ? 1u : 0u;
    uniforms.maxSteps = settings_.maxSteps;
    uniforms.atlasBricksPerAxis = manager_.getSettings().atlasBrickCapacityPerAxis;
    uniforms.cascadeCount = std::min<uint32_t>(manager_.getCascadeCount(), kMaxTraceCascades);
    uniforms.cascadeOverride = settings_.cascadeOverride;
    const auto &shared = manager_.getSharedResources();
    const bool canSampleSurfaceCache =
        settings_.surfaceCacheMaterialsEnabled && shared.surfaceCacheMaterialReferenceAtlas.isValid();
    uniforms.surfaceCachePageResolution = canSampleSurfaceCache ? manager_.getSettings().surfaceCachePageResolution : 0u;
    uniforms.surfaceCachePagesPerRow = canSampleSurfaceCache ? shared.surfaceCacheLayout.pagesPerRow : 0u;
    uniforms.hitThresholdScale = settings_.hitThresholdScale;
    uniforms.emptySpaceStepScale = settings_.emptySpaceStepScale;
    uniforms.normalBias = settings_.normalBias;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    writeFrameLocalBuffer(frameOverrideUniformBufferIds_, uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);

  }

  void writeCascadeInfos()
  {
    std::array<GPUSignedDistanceFieldsManager::CascadeInfoGPU, kMaxTraceCascades> cascadeInfos{};
    const uint32_t count = std::min<uint32_t>(manager_.getCascadeCount(), kMaxTraceCascades);
    for (uint32_t cascadeIndex = 0u; cascadeIndex < count; ++cascadeIndex)
    {
      cascadeInfos[cascadeIndex] = manager_.getCascadeInfo(cascadeIndex);
    }
    writeFrameLocalBuffer(frameOverrideCascadeInfoBufferIds_, cascadeInfoBuffer_, 0u, sizeof(cascadeInfos), cascadeInfos.data());
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void initializeFrameOverrideBuffers(
      const rendering::Buffer &buffer,
      rendering::BufferUsage usage,
      std::vector<rendering::BufferId> &bufferIds)
  {
    if (renderGraph == nullptr)
    {
      return;
    }

    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    if (frameCount <= 1u || !bufferIds.empty())
    {
      return;
    }

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
    {
      return;
    }

    for (const rendering::BufferId bufferId : bufferIds)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
  }

  rendering::BufferId getFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds, uint32_t frameSlot) const
  {
    if (bufferIds.empty())
    {
      return rendering::BufferId::Invalid;
    }

    return bufferIds[frameSlot % bufferIds.size()];
  }

  rendering::BufferId getCurrentFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds) const
  {
    return getFrameOverrideBufferId(bufferIds, renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

  void writeFrameLocalBuffer(
      const std::vector<rendering::BufferId> &bufferIds,
      const rendering::Buffer &buffer,
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

  GPUSignedDistanceFieldsManager &manager_;
  rendering::Buffer inputRayBuffer_;
  rendering::Buffer outputHitBuffer_;
  rendering::Buffer traceStatsBuffer_;
  Settings settings_{};
  bool enabled_ = true;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
  rendering::Buffer cascadeInfoBuffer_;
  rendering::Buffer fallbackBrickMapBuffer_;
  rendering::Buffer fallbackAabbTreeBuffer_;
  rendering::Buffer fallbackTraceStatsBuffer_;
  rendering::Texture fallbackSurfaceCacheMaterialReferenceAtlas_;
  rendering::Texture fallbackSurfaceCacheMaterialReferenceBasisAtlas_;
  rendering::Sampler sdfAtlasSampler_;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideCascadeInfoBufferIds_{};
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
