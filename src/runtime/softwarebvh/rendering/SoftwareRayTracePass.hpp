#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareRayTraceScene.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"

namespace softwarebvh
{
namespace rendering
{

class SoftwareRayTracePass : public ::rendering::Pass
{
public:
  enum class DebugMode : uint32_t
  {
    TriangleId = 0u,
    Heatmap = 1u,
    WorldNormal = 2u,
    ReachedValidBLAS = 3u,
    VisitedBLASNode = 4u,
    ReadTriangle = 5u,
    RejectedTriangle = 6u,
    AcceptedTriangle = 7u,
    RejectedWorldT = 8u,
    AcceptedCandidate = 9u,
    StageSummary = 10u,
    ReachedInvalidBLAS = 11u,
    BLASLeafGroup = 12u,
    InvalidLeafRemap = 13u,
    InvalidLocalRay = 14u,
    ClosestTLASBLASAABB = 15u,
    RBVHCardAABB = 16u,
  };

  enum class PrimitiveMode : uint32_t
  {
    Triangle = 0u,
    RBVHCard = 1u,
  };

  enum class ShaderVariant : uint32_t
  {
    Auto = 0u,
    Release = 1u,
    Debug = 2u,
  };

  enum class TLASMode : uint32_t
  {
    CWBVH = 0u,
    BVH4 = 1u,
  };

  enum class TraversalMode : uint32_t
  {
    LocalInstance = 0u,
    WorldCWBVH = 1u,
  };

  struct Settings
  {
    bool enabled = true;
    uint32_t width = 512u;
    uint32_t height = 512u;
    math::Vec3f cameraPosition = {0.0f, 0.0f, 0.0f};
    math::Vec3f cameraForward = {0.0f, 0.0f, -1.0f};
    math::Vec3f cameraRight = {1.0f, 0.0f, 0.0f};
    math::Vec3f cameraUp = {0.0f, 1.0f, 0.0f};
    float verticalFovRadians = 1.0471976f;
    bool reverseZ = true;
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    bool useExplicitInverseViewProjection = false;
    float tMin = 0.001f;
    float tMax = 3000.0f;
    uint32_t heatmapMaxIterations = 128u;
    float backgroundColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool overlayTLASBounds = false;
    bool overlayTLASCwbvhBounds = false;
    uint32_t overlayTLASLevel = 0u;
    float overlayLineThickness = 1.5f;
    float overlayColor[3] = {0.95f, 0.2f, 0.1f};
    bool enableTraversalEarlyOut = false;
    float traversalEarlyOutDistanceBias = 0.0f;
    bool enableDynamicRayFetch = false;
    bool enableSubgroupTraversal = true;
    bool enableRaySorting = true;
    bool generateCameraRays = true;
    uint32_t rayCount = 0u;
    PrimitiveMode primitiveMode = PrimitiveMode::Triangle;
    TraversalMode traversalMode = TraversalMode::LocalInstance;
    TLASMode tlasMode = TLASMode::CWBVH;
    uint32_t rbvhLod = 0u;
    ShaderVariant shaderVariant = ShaderVariant::Auto;
    DebugMode debugMode = DebugMode::TriangleId;
  };

  struct StatsGPU
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
  static_assert(sizeof(StatsGPU) == 120u, "SoftwareRayTracePass::StatsGPU must stay tightly packed");

  struct HitOutputGPU
  {
    uint32_t instanceId = UINT32_MAX;
    uint32_t triangleId = UINT32_MAX;
    float hitT = 0.0f;
    float baryU = 0.0f;
    float baryV = 0.0f;
    uint32_t tlasInstanceIndex = UINT32_MAX;
    uint32_t flags = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(HitOutputGPU) == 32u, "SoftwareRayTracePass::HitOutputGPU must stay tightly packed");

  struct TraceRayGPU
  {
    float originTMin[4] = {0.0f, 0.0f, 0.0f, 0.001f};
    float directionTMax[4] = {0.0f, 0.0f, -1.0f, 3000.0f};
  };
  static_assert(sizeof(TraceRayGPU) == 32u, "SoftwareRayTracePass::TraceRayGPU must stay tightly packed");

  struct WorldCWBVHBufferSet
  {
    ::rendering::Buffer sceneCWBVHNodes;
    uint64_t sceneCWBVHNodesSize = 0u;
    ::rendering::Buffer worldTriangleTransforms;
    uint64_t worldTriangleTransformsSize = 0u;
    ::rendering::Buffer worldLeafRemap;
    uint64_t worldLeafRemapSize = 0u;
    ::rendering::Buffer worldInstanceRanges;
    uint64_t worldInstanceRangesSize = 0u;
  };

  SoftwareRayTracePass(softwarebvh::SoftwareTLASManager &tlasManager, softwarebvh::SoftwareBLASManager &blasManager) : SoftwareRayTracePass(tlasManager, blasManager, ::rendering::Texture{}, Settings{})
  {
  }

  SoftwareRayTracePass(softwarebvh::SoftwareTLASManager &tlasManager, softwarebvh::SoftwareBLASManager &blasManager, Settings settings) : SoftwareRayTracePass(tlasManager, blasManager, ::rendering::Texture{}, settings)
  {
  }

  SoftwareRayTracePass(softwarebvh::SoftwareTLASManager &tlasManager, softwarebvh::SoftwareBLASManager &blasManager, Settings settings, WorldCWBVHBufferSet worldBuffers)
      : SoftwareRayTracePass(tlasManager, blasManager, ::rendering::Texture{}, settings, worldBuffers)
  {
  }

  SoftwareRayTracePass(softwarebvh::SoftwareTLASManager &tlasManager, softwarebvh::SoftwareBLASManager &blasManager, ::rendering::Texture outputTexture, Settings settings)
      : tlasManager_(tlasManager), blasManager_(blasManager), settings_(settings), outputTexture_(outputTexture), ownsOutputTexture_(!outputTexture.isValid())
  {
  }

  SoftwareRayTracePass(softwarebvh::SoftwareTLASManager &tlasManager, softwarebvh::SoftwareBLASManager &blasManager, ::rendering::Texture outputTexture, Settings settings, WorldCWBVHBufferSet worldBuffers)
      : SoftwareRayTracePass(tlasManager, blasManager, outputTexture, settings)
  {
    applyWorldCWBVHBuffers(worldBuffers);
  }

  ~SoftwareRayTracePass() override
  {
    destroyResources();
  }

  void setSettings(const Settings &settings)
  {
    const bool requiresResourceRefresh = settings_.width != settings.width || settings_.height != settings.height;
    settings_ = settings;
    resourcesDirty_ = resourcesDirty_ || requiresResourceRefresh;
    if (uniformBuffer_.isValid() && !requiresResourceRefresh)
    {
      writeUniforms();
    }
  }

  const Settings &getSettings() const
  {
    return settings_;
  }

  const ::rendering::Texture &getOutputTexture() const
  {
    return outputTexture_;
  }

  const ::rendering::Buffer &getStatsBuffer()
  {
    ensureStatsBuffer();
    return statsBuffer_;
  }

  const ::rendering::Buffer &getHitOutputBuffer()
  {
    ensureHitOutputBuffer();
    return hitOutputBuffer_;
  }

  const ::rendering::Buffer &getRayInputBuffer()
  {
    ensureRayInputBuffer();
    return rayInputBuffer_;
  }

  void setTraceDispatchIndirectBuffer(::rendering::Buffer dispatchIndirectBuffer)
  {
    traceDispatchIndirectBuffer_ = dispatchIndirectBuffer;
  }

  void setWorldCWBVHBuffers(
      ::rendering::Buffer sceneCWBVHNodes,
      uint64_t sceneCWBVHNodesSize,
      ::rendering::Buffer worldTriangleTransforms,
      uint64_t worldTriangleTransformsSize,
      ::rendering::Buffer worldLeafRemap,
      uint64_t worldLeafRemapSize,
      ::rendering::Buffer worldInstanceRanges,
      uint64_t worldInstanceRangesSize)
  {
    applyWorldCWBVHBuffers(
        WorldCWBVHBufferSet{
            .sceneCWBVHNodes = sceneCWBVHNodes,
            .sceneCWBVHNodesSize = sceneCWBVHNodesSize,
            .worldTriangleTransforms = worldTriangleTransforms,
            .worldTriangleTransformsSize = worldTriangleTransformsSize,
            .worldLeafRemap = worldLeafRemap,
            .worldLeafRemapSize = worldLeafRemapSize,
            .worldInstanceRanges = worldInstanceRanges,
            .worldInstanceRangesSize = worldInstanceRangesSize,
        });
    resourcesDirty_ = true;
  }

  void setEnabled(bool enabled)
  {
    settings_.enabled = enabled;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void appendFrameOverrides(::rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    tlasManager_.appendFrameOverrides(overrides, frameSlot);
    blasManager_.appendFrameOverrides(overrides, frameSlot);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    if (!pipeline_.isValid() || !rayGenPipeline_.isValid() || !resolvePipeline_.isValid() || !clearStatsPipeline_.isValid() || !bindingGroups_.isValid() || !rayInputBuffer_.isValid() ||
        settings_.width == 0u || settings_.height == 0u)
    {
      os::Logger::warningf(
          "[SWRT] skipped command recording pipeline=%s raygen=%s resolve=%s clear=%s bindings=%s rayInput=%s size=%ux%u shader=%s",
          pipeline_.isValid() ? "yes" : "no",
          rayGenPipeline_.isValid() ? "yes" : "no",
          resolvePipeline_.isValid() ? "yes" : "no",
          clearStatsPipeline_.isValid() ? "yes" : "no",
          bindingGroups_.isValid() ? "yes" : "no",
          rayInputBuffer_.isValid() ? "yes" : "no",
          settings_.width,
          settings_.height,
          shaderPath_.c_str());
      return;
    }

    os::Logger::logf(
        "[SWRT] recording commands enabled=%s rays=%u output=%ux%u dispatch=%ux%u primitive=%u debugMode=%u shader=%s",
        settings_.enabled ? "yes" : "no",
        rayCount(),
        settings_.width,
        settings_.height,
        traceDispatchWidth(),
        traceDispatchHeight(),
        static_cast<uint32_t>(settings_.primitiveMode),
        static_cast<uint32_t>(settings_.debugMode),
        shaderPath_.c_str());
    writeUniforms();
    if (settings_.generateCameraRays)
    {
      commandBuffer.cmdBindComputePipeline(rayGenPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (settings_.width + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (settings_.height + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);
    }
    commandBuffer.cmdBindComputePipeline(clearStatsPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
    if (shouldSortExternalRays())
    {
      commandBuffer.cmdBindComputePipeline(clearRaySortPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch((kRaySortBinCount + 63u) / 64u, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(countRaySortPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (rayCount() + 63u) / 64u), 1u, 1u);
      commandBuffer.cmdBindComputePipeline(prefixRaySortPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      commandBuffer.cmdBindComputePipeline(scatterRaySortPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (rayCount() + 63u) / 64u), 1u, 1u);
    }
    const bool useExternalIndirectTrace = !settings_.generateCameraRays && traceDispatchIndirectBuffer_.isValid() && externalRayPipeline_.isValid();
    commandBuffer.cmdBindComputePipeline(useExternalIndirectTrace ? externalRayPipeline_ : pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    if (useExternalIndirectTrace)
    {
      commandBuffer.cmdDispatchIndirect(traceDispatchIndirectBuffer_, 0u);
    }
    else
    {
      commandBuffer.cmdDispatch(std::max(1u, (traceDispatchWidth() + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (traceDispatchHeight() + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);
    }
    if (settings_.generateCameraRays && outputTexture_.isValid())
    {
      commandBuffer.cmdBindComputePipeline(resolvePipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (settings_.width + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (settings_.height + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);
    }
  }

private:
  static constexpr uint32_t kWorkgroupSizeX = 8u;
  static constexpr uint32_t kWorkgroupSizeY = 8u;
  static constexpr uint32_t kRaySortBinCount = 256u;

  void applyWorldCWBVHBuffers(const WorldCWBVHBufferSet &worldBuffers)
  {
    worldSceneCWBVHNodesBuffer_ = worldBuffers.sceneCWBVHNodes;
    worldSceneCWBVHNodesBufferSize_ = worldBuffers.sceneCWBVHNodesSize;
    worldTriangleTransformsBuffer_ = worldBuffers.worldTriangleTransforms;
    worldTriangleTransformsBufferSize_ = worldBuffers.worldTriangleTransformsSize;
    worldLeafRemapBuffer_ = worldBuffers.worldLeafRemap;
    worldLeafRemapBufferSize_ = worldBuffers.worldLeafRemapSize;
    worldInstanceRangesBuffer_ = worldBuffers.worldInstanceRanges;
    worldInstanceRangesBufferSize_ = worldBuffers.worldInstanceRangesSize;
  }

  struct Uniforms
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t tlasNodeCount = 0u;
    uint32_t debugMode = 0u;
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float tanHalfFovY = 0.0f;
    float cameraForward[3] = {0.0f, 0.0f, -1.0f};
    float aspectRatio = 1.0f;
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float tMin = 0.001f;
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float tMax = 3000.0f;
    float backgroundColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    uint32_t heatmapMaxIterations = 128u;
    uint32_t rayCount = 0u;
    uint32_t overlayFlags = 0u;
    uint32_t rayDispatchWidth = 1u;
    float overlayColorAndThickness[4] = {0.95f, 0.2f, 0.1f, 1.5f};
    float traceControl[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t primitiveMode = 0u;
    uint32_t rbvhLodLevel = 0u;
    uint32_t rbvhDepthAtlasTextureWidth = 1u;
    uint32_t rbvhDepthAtlasTextureHeight = 1u;
  };
  static_assert(sizeof(Uniforms) == 224u, "SoftwareRayTracePass::Uniforms must stay tightly packed");

  static ::rendering::TextureView makeSampledColorView(const ::rendering::Texture &texture)
  {
    return ::rendering::TextureView{
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  static ::rendering::TextureView makeStorageColorView(const ::rendering::Texture &texture)
  {
    return ::rendering::TextureView{
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  static ::rendering::BindingGroupLayoutBufferEntry makeLayoutBuffer(
      const char *name,
      uint32_t binding,
      ::rendering::BufferBindingType type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer)
  {
    return ::rendering::BindingGroupLayoutBufferEntry{
      .name = name,
      .binding = binding,
      .type = type,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
    };
  }

  static ::rendering::BindingBuffer makeBufferBinding(
      uint32_t binding,
      const ::rendering::Buffer &buffer,
      ::rendering::AccessPattern access,
      uint64_t size)
  {
    return ::rendering::BindingBuffer{
      .bufferView =
          {
            .buffer = buffer,
            .offset = 0u,
            .size = size,
            .access = access,
          },
      .binding = binding,
    };
  }

  uint32_t rayCount() const
  {
    if (settings_.generateCameraRays || settings_.rayCount == 0u)
    {
      return settings_.width * settings_.height;
    }
    return settings_.rayCount;
  }

  uint32_t traceDispatchWidth() const
  {
    return settings_.generateCameraRays ? settings_.width : std::max(1u, rayCount());
  }

  uint32_t traceDispatchHeight() const
  {
    return settings_.generateCameraRays ? settings_.height : 1u;
  }

  std::vector<::rendering::BindingGroupLayoutBufferEntry> buildTraceLayoutBuffers() const
  {
    const bool worldCWBVHMode = isWorldCWBVHMode();
    const bool rbvhMode = !worldCWBVHMode && settings_.primitiveMode == PrimitiveMode::RBVHCard;
    std::vector<::rendering::BindingGroupLayoutBufferEntry> buffers = {
      makeLayoutBuffer("uniforms", 0u, ::rendering::BufferBindingType::BufferBindingType_UniformBuffer),
      makeLayoutBuffer("tlasInstances", 1u),
      makeLayoutBuffer("tlasNodes", 2u),
      makeLayoutBuffer("tlasLeafInstances", 3u),
      makeLayoutBuffer("blasMetadata", 4u),
    };

    if (rbvhMode)
    {
      buffers.push_back(makeLayoutBuffer("blasPrimitiveRemap", 5u));
      buffers.push_back(makeLayoutBuffer("rbvhCards", 6u));
      buffers.push_back(makeLayoutBuffer("rbvhDepthMipAtlas", 8u));
    }
    else
    {
      buffers.push_back(makeLayoutBuffer("blasPrimitiveRemap", 5u));
      buffers.push_back(makeLayoutBuffer("blasTriangleTransforms", 6u));
    }

    buffers.push_back(makeLayoutBuffer("blasNodes", 9u));
    buffers.push_back(makeLayoutBuffer("blasLeafRemap", 10u));
    buffers.push_back(makeLayoutBuffer("traceStats", 11u));
    buffers.push_back(makeLayoutBuffer("rayInput", 13u));
    buffers.push_back(makeLayoutBuffer("hitOutput", 14u));
    buffers.push_back(makeLayoutBuffer("tlasInstanceAabbs", 19u));
    buffers.push_back(makeLayoutBuffer("tlasBvh4Nodes", 20u));
    if (worldCWBVHMode)
    {
      buffers.push_back(makeLayoutBuffer("worldInstanceRanges", 21u));
    }
    buffers.push_back(makeLayoutBuffer("raySortBinCounts", 22u));
    buffers.push_back(makeLayoutBuffer("raySortBinOffsets", 23u));
    buffers.push_back(makeLayoutBuffer("raySortIds", 24u));
    return buffers;
  }

  std::vector<::rendering::BindingBuffer> buildTraceBufferBindings() const
  {
    const bool worldCWBVHMode = isWorldCWBVHMode();
    const bool rbvhMode = !worldCWBVHMode && settings_.primitiveMode == PrimitiveMode::RBVHCard;
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    std::vector<::rendering::BindingBuffer> buffers = {
      makeBufferBinding(0u, uniformBuffer_, read, sizeof(Uniforms)),
      makeBufferBinding(1u, tlasManager_.getInstancesBuffer(), read, tlasManager_.getInstancesBufferSize()),
      makeBufferBinding(2u, worldCWBVHMode ? worldSceneCWBVHNodesBuffer_ : tlasManager_.getCwbvhNodesBuffer(), read, worldCWBVHMode ? worldSceneCWBVHNodesBufferSize_ : tlasManager_.getCwbvhNodesBufferSize()),
      makeBufferBinding(3u, tlasManager_.getLeafInstanceIndicesBuffer(), read, tlasManager_.getLeafInstanceIndicesBufferSize()),
    };

    if (rbvhMode)
    {
      buffers.push_back(makeBufferBinding(4u, blasManager_.getRbvhCardMetadataBuffer(), read, blasManager_.getRbvhCardMetadataBufferSize()));
      buffers.push_back(makeBufferBinding(5u, blasManager_.getPrimitiveRemapBuffer(), read, blasManager_.getPrimitiveRemapBufferSize()));
      buffers.push_back(makeBufferBinding(6u, blasManager_.getRbvhCardsBuffer(), read, blasManager_.getRbvhCardsBufferSize()));
      buffers.push_back(makeBufferBinding(8u, blasManager_.getRbvhDepthMipAtlasBuffer(), read, blasManager_.getRbvhDepthMipAtlasBufferSize()));
    }
    else
    {
      buffers.push_back(makeBufferBinding(4u, blasManager_.getTriangleMetadataBuffer(), read, blasManager_.getTriangleMetadataBufferSize()));
      buffers.push_back(makeBufferBinding(5u, blasManager_.getPrimitiveRemapBuffer(), read, blasManager_.getPrimitiveRemapBufferSize()));
      buffers.push_back(makeBufferBinding(6u, worldCWBVHMode ? worldTriangleTransformsBuffer_ : blasManager_.getTriangleTransformsBuffer(), read, worldCWBVHMode ? worldTriangleTransformsBufferSize_ : blasManager_.getTriangleTransformsBufferSize()));
    }

    buffers.push_back(makeBufferBinding(9u, worldCWBVHMode ? worldSceneCWBVHNodesBuffer_ : blasManager_.getGPUCWBVHNodesBuffer(), read, worldCWBVHMode ? worldSceneCWBVHNodesBufferSize_ : blasManager_.getGPUCWBVHNodesBufferSize()));
    buffers.push_back(makeBufferBinding(10u, worldCWBVHMode ? worldLeafRemapBuffer_ : blasManager_.getGPULeafRemapBuffer(), read, worldCWBVHMode ? worldLeafRemapBufferSize_ : blasManager_.getGPULeafRemapBufferSize()));
    buffers.push_back(makeBufferBinding(11u, statsBuffer_, readWrite, sizeof(StatsGPU)));
    buffers.push_back(makeBufferBinding(13u, rayInputBuffer_, readWrite, rayInputBufferSize_));
    buffers.push_back(makeBufferBinding(14u, hitOutputBuffer_, readWrite, hitOutputBufferSize_));
    buffers.push_back(makeBufferBinding(19u, tlasManager_.getInstanceAabbsBuffer(), read, tlasManager_.getInstanceAabbsBufferSize()));
    buffers.push_back(makeBufferBinding(20u, tlasManager_.getBvh4NodesBuffer(), read, tlasManager_.getBvh4NodesBufferSize()));
    if (worldCWBVHMode)
    {
      buffers.push_back(makeBufferBinding(21u, worldInstanceRangesBuffer_, read, worldInstanceRangesBufferSize_));
    }
    buffers.push_back(makeBufferBinding(22u, raySortBinCountsBuffer_, readWrite, raySortBinCountsBufferSize()));
    buffers.push_back(makeBufferBinding(23u, raySortBinOffsetsBuffer_, readWrite, raySortBinOffsetsBufferSize()));
    buffers.push_back(makeBufferBinding(24u, raySortIdsBuffer_, readWrite, raySortIdsBufferSize()));
    return buffers;
  }

  static bool needsDebugShaderVariant(const Settings &settings)
  {
    if (settings.shaderVariant == ShaderVariant::Debug)
    {
      return true;
    }
    if (settings.shaderVariant == ShaderVariant::Release)
    {
      return false;
    }
    return settings.overlayTLASBounds || settings.overlayTLASCwbvhBounds;
  }

  bool isWorldCWBVHMode() const
  {
    return settings_.traversalMode == TraversalMode::WorldCWBVH;
  }

  bool hasWorldCWBVHBuffers() const
  {
    return worldSceneCWBVHNodesBuffer_.isValid() &&
           worldSceneCWBVHNodesBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU) &&
           worldTriangleTransformsBuffer_.isValid() &&
           worldTriangleTransformsBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU) &&
           worldLeafRemapBuffer_.isValid() &&
           worldLeafRemapBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::WorldLeafEntryGPU) &&
           worldInstanceRangesBuffer_.isValid() &&
           worldInstanceRangesBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU);
  }

  static const char *shaderPathForSettings(const Settings &settings)
  {
    if (settings.traversalMode == TraversalMode::WorldCWBVH)
    {
      if (settings.enableSubgroupTraversal)
      {
        return needsDebugShaderVariant(settings)
            ? "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-debug.spirv"
            : "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-release.spirv";
      }
      return needsDebugShaderVariant(settings)
          ? "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-debug.spirv"
          : "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-release.spirv";
    }
    const bool rbvhMode = settings.primitiveMode == PrimitiveMode::RBVHCard;
    const bool debugVariant = needsDebugShaderVariant(settings);
    const bool bvh4Tlas = settings.tlasMode == TLASMode::BVH4;
    if (rbvhMode)
    {
      if (bvh4Tlas)
      {
        return debugVariant ? "assets/shaders/spirv/softwarebvh-raytrace-rbvh-bvh4-debug.spirv" : "assets/shaders/spirv/softwarebvh-raytrace-rbvh-bvh4-release.spirv";
      }
      return debugVariant ? "assets/shaders/spirv/softwarebvh-raytrace-rbvh-debug.spirv" : "assets/shaders/spirv/softwarebvh-raytrace-rbvh-release.spirv";
    }
    if (bvh4Tlas)
    {
      return debugVariant ? "assets/shaders/spirv/softwarebvh-raytrace-triangle-bvh4-debug.spirv" : "assets/shaders/spirv/softwarebvh-raytrace-triangle-bvh4-release.spirv";
    }
    return debugVariant ? "assets/shaders/spirv/softwarebvh-raytrace-triangle-debug.spirv" : "assets/shaders/spirv/softwarebvh-raytrace-triangle-release.spirv";
  }

  void destroyResources()
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (externalRayPipeline_.isValid())
      renderGraph->deleteComputePipeline(externalRayPipeline_);
    if (rayGenPipeline_.isValid())
      renderGraph->deleteComputePipeline(rayGenPipeline_);
    if (resolvePipeline_.isValid())
      renderGraph->deleteComputePipeline(resolvePipeline_);
    if (clearStatsPipeline_.isValid())
      renderGraph->deleteComputePipeline(clearStatsPipeline_);
    if (clearRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(clearRaySortPipeline_);
    if (countRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(countRaySortPipeline_);
    if (prefixRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(prefixRaySortPipeline_);
    if (scatterRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(scatterRaySortPipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (rbvhDepthAtlasSampler_.isValid())
      renderGraph->deleteSampler(rbvhDepthAtlasSampler_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (statsBuffer_.isValid())
      renderGraph->deleteBuffer(statsBuffer_);
    if (rayInputBuffer_.isValid())
      renderGraph->deleteBuffer(rayInputBuffer_);
    if (hitOutputBuffer_.isValid())
      renderGraph->deleteBuffer(hitOutputBuffer_);
    if (raySortBinCountsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortBinCountsBuffer_);
    if (raySortBinOffsetsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortBinOffsetsBuffer_);
    if (raySortIdsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortIdsBuffer_);
    if (outputTexture_.isValid() && ownsOutputTexture_)
      renderGraph->deleteTexture(outputTexture_);

    pipeline_ = {};
    externalRayPipeline_ = {};
    rayGenPipeline_ = {};
    resolvePipeline_ = {};
    clearStatsPipeline_ = {};
    clearRaySortPipeline_ = {};
    countRaySortPipeline_ = {};
    prefixRaySortPipeline_ = {};
    scatterRaySortPipeline_ = {};
    shader_ = {};
    bindingGroups_ = {};
    layout_ = {};
    rbvhDepthAtlasSampler_ = {};
    uniformBuffer_ = {};
    statsBuffer_ = {};
    rayInputBuffer_ = {};
    hitOutputBuffer_ = {};
    raySortBinCountsBuffer_ = {};
    raySortBinOffsetsBuffer_ = {};
    raySortIdsBuffer_ = {};
    outputTexture_ = {};
    outputWidth_ = 0u;
    outputHeight_ = 0u;
    hitOutputBufferSize_ = 0u;
    rayInputBufferSize_ = 0u;
    raySortIdsBufferSize_ = 0u;
    ownsOutputTexture_ = false;
  }

  void destroyShaderResources()
  {
    if (renderGraph == nullptr)
    {
      return;
    }
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (externalRayPipeline_.isValid())
      renderGraph->deleteComputePipeline(externalRayPipeline_);
    if (rayGenPipeline_.isValid())
      renderGraph->deleteComputePipeline(rayGenPipeline_);
    if (resolvePipeline_.isValid())
      renderGraph->deleteComputePipeline(resolvePipeline_);
    if (clearStatsPipeline_.isValid())
      renderGraph->deleteComputePipeline(clearStatsPipeline_);
    if (clearRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(clearRaySortPipeline_);
    if (countRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(countRaySortPipeline_);
    if (prefixRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(prefixRaySortPipeline_);
    if (scatterRaySortPipeline_.isValid())
      renderGraph->deleteComputePipeline(scatterRaySortPipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);

    pipeline_ = {};
    externalRayPipeline_ = {};
    rayGenPipeline_ = {};
    resolvePipeline_ = {};
    clearStatsPipeline_ = {};
    clearRaySortPipeline_ = {};
    countRaySortPipeline_ = {};
    prefixRaySortPipeline_ = {};
    scatterRaySortPipeline_ = {};
    shader_ = {};
    bindingGroups_ = {};
    layout_ = {};
    resourcesDirty_ = true;
  }

  void ensureResources()
  {
    if (settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    const uint32_t blasBuildRevision = blasManager_.getLastBuildSummary().buildRevision;
    if (lastBlasBuildRevision_ != blasBuildRevision)
    {
      if (bindingGroups_.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups_);
        bindingGroups_ = {};
      }
      lastBlasBuildRevision_ = blasBuildRevision;
      resourcesDirty_ = true;
    }

    const std::string desiredShaderPath = shaderPathForSettings(settings_);
    if (shader_.isValid() && shaderPath_ != desiredShaderPath)
    {
      destroyShaderResources();
    }
    shaderPath_ = desiredShaderPath;

    if (ownsOutputTexture_)
    {
      const bool outputChanged = !outputTexture_.isValid() || outputWidth_ != settings_.width || outputHeight_ != settings_.height;
      if (outputChanged)
      {
        if (bindingGroups_.isValid())
        {
          renderGraph->deleteBindingGroups(bindingGroups_);
          bindingGroups_ = {};
        }
        if (outputTexture_.isValid())
        {
          renderGraph->deleteTexture(outputTexture_);
        }

        outputTexture_ = renderGraph->createTexture(
            ::rendering::TextureInfo{
              .name = passName + "_Output.texture",
              .format = ::rendering::Format::Format_RGBA16Float,
              .memoryProperties = ::rendering::BufferUsage::BufferUsage_Storage,
              .usage = ::rendering::ImageUsage::ImageUsage_Storage | ::rendering::ImageUsage::ImageUsage_Sampled,
              .width = settings_.width,
              .height = settings_.height,
              .depth = 1u,
              .mipLevels = 1u,
            });
        outputWidth_ = settings_.width;
        outputHeight_ = settings_.height;
        resourcesDirty_ = true;
      }
    }
    else if (outputTexture_.isValid())
    {
      outputWidth_ = settings_.width;
      outputHeight_ = settings_.height;
    }

    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
          });
      resourcesDirty_ = true;
    }

    ensureStatsBuffer();
    ensureRayInputBuffer();
    ensureHitOutputBuffer();
    ensureRaySortBuffers();

    if (isWorldCWBVHMode() && !hasWorldCWBVHBuffers())
    {
      os::Logger::warningf(
          "[SWRT] World CWBVH traversal requested without valid world buffers scene=%s triangles=%s leafRemap=%s ranges=%s",
          worldSceneCWBVHNodesBuffer_.isValid() ? "yes" : "no",
          worldTriangleTransformsBuffer_.isValid() ? "yes" : "no",
          worldLeafRemapBuffer_.isValid() ? "yes" : "no",
          worldInstanceRangesBuffer_.isValid() ? "yes" : "no");
      return;
    }

    const bool rbvhMode = !isWorldCWBVHMode() && settings_.primitiveMode == PrimitiveMode::RBVHCard;
    if (rbvhMode && !blasManager_.getRbvhDepthAtlasTexture().isValid())
    {
      os::Logger::warningf(
          "[SWRT] RBVH atlas texture is not valid while recording shader=%s cards=%llu atlasTexels=%u",
          shaderPath_.c_str(),
          static_cast<unsigned long long>(blasManager_.getLastBuildSummary().rbvhCardCount),
          blasManager_.getRbvhDepthAtlasTextureTexelCount());
      return;
    }
    if (rbvhMode && !rbvhDepthAtlasSampler_.isValid())
    {
      rbvhDepthAtlasSampler_ = renderGraph->createSampler(
          ::rendering::SamplerInfo{
            .name = passName + "_RBVHDepthAtlasSampler",
            .minFilter = ::rendering::Filter::Linear,
            .magFilter = ::rendering::Filter::Linear,
            .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
            .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
            .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
            .anisotropyEnable = false,
            .maxAnisotropy = 1.0f,
            .maxLod = 1.0f,
          });
      resourcesDirty_ = true;
    }

    if (!layout_.isValid())
    {
      ::rendering::BindingGroupLayout layoutGroup{
        .buffers = buildTraceLayoutBuffers(),
        .storageTextures =
            {
              {.name = "outputTexture", .binding = 12u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      };
      if (rbvhMode)
      {
        layoutGroup.samplers.push_back(
            ::rendering::BindingGroupLayoutSamplerEntry{
              .name = "rbvhDepthAtlasSampler",
              .binding = 15u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
            });
        layoutGroup.textures.push_back(
            ::rendering::BindingGroupLayoutTextureEntry{
              .name = "rbvhDepthAtlas",
              .binding = 7u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
            });
      }

      layout_ = renderGraph->createBindingsLayout(
          ::rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {layoutGroup},
          });
      resourcesDirty_ = true;
    }

    if (!shader_.isValid())
    {
      const bool rbvhMode = !isWorldCWBVHMode() && settings_.primitiveMode == PrimitiveMode::RBVHCard;
      os::Logger::logf(
          "[SWRT] creating shader path=%s primitive=%s traversal=%s variant=%s debugMode=%u",
          shaderPath_.c_str(),
          rbvhMode ? "RBVHCard" : "Triangle",
          isWorldCWBVHMode() ? (settings_.enableSubgroupTraversal ? "WorldCWBVHSubgroup" : "WorldCWBVH") : "LocalInstance",
          needsDebugShaderVariant(settings_) ? "Debug" : "Release",
          static_cast<uint32_t>(settings_.debugMode));
      shader_ = renderGraph->createShader(
          ::rendering::ShaderInfo{
            .name = passName + (isWorldCWBVHMode() ? "_WorldCWBVH_Shader" : (rbvhMode ? "_SWRBVH_Shader" : "_Triangle_Shader")) + (needsDebugShaderVariant(settings_) ? "_Debug" : "_Release"),
            .layout = layout_,
            .src = os::io::readRelativeFile(shaderPath_),
            .type = ::rendering::ShaderType::SpirV,
          });
      resourcesDirty_ = true;
    }

    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "main",
            .layout = layout_,
            .name = passName + (isWorldCWBVHMode() ? "_WorldCWBVH_Pipeline" : (settings_.primitiveMode == PrimitiveMode::RBVHCard ? "_SWRBVH_Pipeline" : "_Triangle_Pipeline")),
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!externalRayPipeline_.isValid())
    {
      externalRayPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "main_external",
            .layout = layout_,
            .name = passName + (isWorldCWBVHMode() ? "_WorldCWBVH_ExternalRayPipeline" : (settings_.primitiveMode == PrimitiveMode::RBVHCard ? "_SWRBVH_ExternalRayPipeline" : "_Triangle_ExternalRayPipeline")),
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!rayGenPipeline_.isValid())
    {
      rayGenPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "generate_camera_rays",
            .layout = layout_,
            .name = passName + "_GenerateCameraRaysPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!resolvePipeline_.isValid())
    {
      resolvePipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "resolve_camera_hit_output",
            .layout = layout_,
            .name = passName + "_ResolveCameraHitOutputPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!clearStatsPipeline_.isValid())
    {
      clearStatsPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "clear_stats",
            .layout = layout_,
            .name = passName + "_ClearStatsPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!clearRaySortPipeline_.isValid())
    {
      clearRaySortPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "clear_ray_sort_bins",
            .layout = layout_,
            .name = passName + "_ClearRaySortBinsPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!countRaySortPipeline_.isValid())
    {
      countRaySortPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "count_ray_sort_bins",
            .layout = layout_,
            .name = passName + "_CountRaySortBinsPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!prefixRaySortPipeline_.isValid())
    {
      prefixRaySortPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "prefix_ray_sort_bins",
            .layout = layout_,
            .name = passName + "_PrefixRaySortBinsPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (!scatterRaySortPipeline_.isValid())
    {
      scatterRaySortPipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
            .entry = "scatter_ray_sort_ids",
            .layout = layout_,
            .name = passName + "_ScatterRaySortIdsPipeline",
            .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (resourcesDirty_)
    {
      if (bindingGroups_.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups_);
      }

      ::rendering::GroupInfo groupInfo{
        .name = "group0",
        .buffers = buildTraceBufferBindings(),
        .storageTextures =
            {
              {.binding = 12u, .textureView = makeStorageColorView(outputTexture_)},
            },
      };
      if (rbvhMode)
      {
        groupInfo.samplers.push_back(
            ::rendering::BindingSampler{
              .sampler = rbvhDepthAtlasSampler_,
              .view = makeSampledColorView(blasManager_.getRbvhDepthAtlasTexture()),
              .binding = 15u,
            });
        groupInfo.textures.push_back(
            ::rendering::BindingTextureInfo{
              .textureView = makeSampledColorView(blasManager_.getRbvhDepthAtlasTexture()),
              .binding = 7u,
            });
      }

      bindingGroups_ = renderGraph->createBindingGroups(
          ::rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Bindings",
            .groups = {groupInfo},
          });

      resourcesDirty_ = false;
    }
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.width = settings_.width;
    uniforms.height = settings_.height;
    uniforms.tlasNodeCount = isWorldCWBVHMode() || settings_.tlasMode != TLASMode::BVH4 ? tlasManager_.getCwbvhNodeCount() : tlasManager_.getBvh4NodeCount();
    uniforms.debugMode = static_cast<uint32_t>(settings_.debugMode);
    if (settings_.useExplicitInverseViewProjection)
    {
      uniforms.inverseViewProjection = settings_.inverseViewProjection;
    }
    else
    {
      const math::Mat4f view = math::Mat4f::lookAt(settings_.cameraPosition, settings_.cameraPosition + settings_.cameraForward, settings_.cameraUp);
      const math::Mat4f projection = math::Mat4f::perspective(
          settings_.verticalFovRadians,
          settings_.height == 0u ? 1.0f : static_cast<float>(settings_.width) / static_cast<float>(settings_.height),
          std::max(settings_.tMin, 1.0e-4f),
          std::max(settings_.tMax, settings_.tMin + 1.0e-3f),
          settings_.reverseZ);
      uniforms.inverseViewProjection = (projection * view).inverse();
    }
    uniforms.cameraPosition[0] = settings_.cameraPosition[0];
    uniforms.cameraPosition[1] = settings_.cameraPosition[1];
    uniforms.cameraPosition[2] = settings_.cameraPosition[2];
    uniforms.tanHalfFovY = std::tan(settings_.verticalFovRadians * 0.5f);
    uniforms.cameraForward[0] = settings_.cameraForward[0];
    uniforms.cameraForward[1] = settings_.cameraForward[1];
    uniforms.cameraForward[2] = settings_.cameraForward[2];
    uniforms.aspectRatio = settings_.height == 0u ? 1.0f : static_cast<float>(settings_.width) / static_cast<float>(settings_.height);
    uniforms.cameraRight[0] = settings_.cameraRight[0];
    uniforms.cameraRight[1] = settings_.cameraRight[1];
    uniforms.cameraRight[2] = settings_.cameraRight[2];
    uniforms.tMin = settings_.tMin;
    uniforms.cameraUp[0] = settings_.cameraUp[0];
    uniforms.cameraUp[1] = settings_.cameraUp[1];
    uniforms.cameraUp[2] = settings_.cameraUp[2];
    uniforms.tMax = settings_.tMax;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
      uniforms.backgroundColor[i] = settings_.backgroundColor[i];
    }
    uniforms.heatmapMaxIterations = std::max(settings_.heatmapMaxIterations, 1u);
    uniforms.rayCount = rayCount();
    uniforms.overlayFlags = (settings_.overlayTLASBounds ? 1u : 0u) | (settings_.enabled ? 2u : 0u) | (settings_.overlayTLASCwbvhBounds ? 4u : 0u);
    uniforms.rayDispatchWidth = traceDispatchWidth();
    uniforms.overlayColorAndThickness[0] = settings_.overlayColor[0];
    uniforms.overlayColorAndThickness[1] = settings_.overlayColor[1];
    uniforms.overlayColorAndThickness[2] = settings_.overlayColor[2];
    uniforms.overlayColorAndThickness[3] = settings_.overlayLineThickness;
    uniforms.traceControl[0] = settings_.enableTraversalEarlyOut ? 1.0f : 0.0f;
    uniforms.traceControl[1] = std::max(0.0f, settings_.traversalEarlyOutDistanceBias);
    uniforms.traceControl[2] = settings_.enableDynamicRayFetch ? 1.0f : 0.0f;
    uniforms.traceControl[3] = shouldSortExternalRays() ? 1.0f : 0.0f;
    uniforms.primitiveMode = isWorldCWBVHMode() ? static_cast<uint32_t>(PrimitiveMode::Triangle) : static_cast<uint32_t>(settings_.primitiveMode);
    uniforms.rbvhLodLevel = settings_.rbvhLod;
    uniforms.rbvhDepthAtlasTextureWidth = blasManager_.getRbvhDepthAtlasTextureWidth();
    uniforms.rbvhDepthAtlasTextureHeight = blasManager_.getRbvhDepthAtlasTextureHeight();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  void ensureStatsBuffer()
  {
    if (statsBuffer_.isValid() || renderGraph == nullptr)
    {
      return;
    }

    statsBuffer_ = renderGraph->createBuffer(
        ::rendering::BufferInfo{
          .name = passName + "_Stats.buffer",
          .size = sizeof(StatsGPU),
          .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc | ::rendering::BufferUsage::BufferUsage_Push,
        });
    resourcesDirty_ = true;
  }

  void ensureRayInputBuffer()
  {
    if (renderGraph == nullptr || settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    const uint64_t requiredSize = static_cast<uint64_t>(std::max(1u, rayCount())) * sizeof(TraceRayGPU);
    if (rayInputBuffer_.isValid() && rayInputBufferSize_ == requiredSize)
    {
      return;
    }

    if (rayInputBuffer_.isValid())
    {
      renderGraph->deleteBuffer(rayInputBuffer_);
      rayInputBuffer_ = {};
      rayInputBufferSize_ = 0u;
      resourcesDirty_ = true;
    }

    rayInputBuffer_ = renderGraph->createBuffer(
        ::rendering::BufferInfo{
          .name = passName + "_RayInput.buffer",
          .size = requiredSize,
          .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopyDst | ::rendering::BufferUsage::BufferUsage_CopySrc,
        });
    rayInputBufferSize_ = requiredSize;
    resourcesDirty_ = true;
  }

  void ensureHitOutputBuffer()
  {
    if (renderGraph == nullptr || settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    const uint64_t requiredSize = static_cast<uint64_t>(std::max(1u, rayCount())) * sizeof(HitOutputGPU);
    if (hitOutputBuffer_.isValid() && hitOutputBufferSize_ == requiredSize)
    {
      return;
    }

    if (hitOutputBuffer_.isValid())
    {
      renderGraph->deleteBuffer(hitOutputBuffer_);
      hitOutputBuffer_ = {};
      hitOutputBufferSize_ = 0u;
      resourcesDirty_ = true;
    }

    hitOutputBuffer_ = renderGraph->createBuffer(
        ::rendering::BufferInfo{
          .name = passName + "_HitOutput.buffer",
          .size = requiredSize,
          .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc,
        });
    hitOutputBufferSize_ = requiredSize;
    resourcesDirty_ = true;
  }

  bool shouldSortExternalRays() const
  {
    return settings_.enableRaySorting &&
           isCWBVHTraceMode() &&
           !settings_.generateCameraRays &&
           !traceDispatchIndirectBuffer_.isValid() &&
           rayCount() > 1u;
  }

  bool isCWBVHTraceMode() const
  {
    return settings_.traversalMode == TraversalMode::WorldCWBVH ||
           settings_.tlasMode == TLASMode::CWBVH;
  }

  uint64_t raySortBinCountsBufferSize() const
  {
    return static_cast<uint64_t>(kRaySortBinCount) * sizeof(uint32_t);
  }

  uint64_t raySortBinOffsetsBufferSize() const
  {
    return static_cast<uint64_t>(kRaySortBinCount) * sizeof(uint32_t);
  }

  uint64_t raySortIdsBufferSize() const
  {
    return static_cast<uint64_t>(std::max(1u, rayCount())) * sizeof(uint32_t);
  }

  void ensureRaySortBuffers()
  {
    if (renderGraph == nullptr || settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    if (!raySortBinCountsBuffer_.isValid())
    {
      raySortBinCountsBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_RaySortBinCounts.buffer",
            .size = raySortBinCountsBufferSize(),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      resourcesDirty_ = true;
    }

    if (!raySortBinOffsetsBuffer_.isValid())
    {
      raySortBinOffsetsBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_RaySortBinOffsets.buffer",
            .size = raySortBinOffsetsBufferSize(),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      resourcesDirty_ = true;
    }

    const uint64_t requiredSortedIdsSize = raySortIdsBufferSize();
    if (raySortIdsBuffer_.isValid() && raySortIdsBufferSize_ == requiredSortedIdsSize)
    {
      return;
    }

    if (raySortIdsBuffer_.isValid())
    {
      renderGraph->deleteBuffer(raySortIdsBuffer_);
      raySortIdsBuffer_ = {};
      raySortIdsBufferSize_ = 0u;
      resourcesDirty_ = true;
    }

    raySortIdsBuffer_ = renderGraph->createBuffer(
        ::rendering::BufferInfo{
          .name = passName + "_RaySortIds.buffer",
          .size = requiredSortedIdsSize,
          .usage = ::rendering::BufferUsage::BufferUsage_Storage,
        });
    raySortIdsBufferSize_ = requiredSortedIdsSize;
    resourcesDirty_ = true;
  }

  softwarebvh::SoftwareTLASManager &tlasManager_;
  softwarebvh::SoftwareBLASManager &blasManager_;
  Settings settings_{};
  bool resourcesDirty_ = true;
  bool ownsOutputTexture_ = false;
  uint32_t outputWidth_ = 0u;
  uint32_t outputHeight_ = 0u;
  uint32_t lastBlasBuildRevision_ = std::numeric_limits<uint32_t>::max();
  uint64_t hitOutputBufferSize_ = 0u;
  uint64_t rayInputBufferSize_ = 0u;
  uint64_t raySortIdsBufferSize_ = 0u;
  std::string shaderPath_;

  ::rendering::Texture outputTexture_;
  ::rendering::Buffer uniformBuffer_;
  ::rendering::Buffer statsBuffer_;
  ::rendering::Buffer rayInputBuffer_;
  ::rendering::Buffer hitOutputBuffer_;
  ::rendering::Buffer raySortBinCountsBuffer_;
  ::rendering::Buffer raySortBinOffsetsBuffer_;
  ::rendering::Buffer raySortIdsBuffer_;
  ::rendering::Buffer traceDispatchIndirectBuffer_;
  ::rendering::Buffer worldSceneCWBVHNodesBuffer_;
  ::rendering::Buffer worldTriangleTransformsBuffer_;
  ::rendering::Buffer worldLeafRemapBuffer_;
  ::rendering::Buffer worldInstanceRangesBuffer_;
  uint64_t worldSceneCWBVHNodesBufferSize_ = 0u;
  uint64_t worldTriangleTransformsBufferSize_ = 0u;
  uint64_t worldLeafRemapBufferSize_ = 0u;
  uint64_t worldInstanceRangesBufferSize_ = 0u;
  ::rendering::Sampler rbvhDepthAtlasSampler_;
  ::rendering::BindingsLayout layout_;
  ::rendering::BindingGroups bindingGroups_;
  ::rendering::Shader shader_;
  ::rendering::ComputePipeline pipeline_;
  ::rendering::ComputePipeline externalRayPipeline_;
  ::rendering::ComputePipeline rayGenPipeline_;
  ::rendering::ComputePipeline resolvePipeline_;
  ::rendering::ComputePipeline clearStatsPipeline_;
  ::rendering::ComputePipeline clearRaySortPipeline_;
  ::rendering::ComputePipeline countRaySortPipeline_;
  ::rendering::ComputePipeline prefixRaySortPipeline_;
  ::rendering::ComputePipeline scatterRaySortPipeline_;
};

} // namespace rendering
} // namespace softwarebvh
