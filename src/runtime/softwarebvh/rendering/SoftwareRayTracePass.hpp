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
    RandomTriangleColor = 17u,
    BLASCandidateCount = 18u,
    BLASSortedCandidateDistance = 19u,
    BLASTestsPerRay = 20u,
    TLASCandidateOverflow = 21u,
    PrimaryRayHeatmap = 22u,
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
    struct TLASCandidatePassSettings
    {
      uint32_t maxLocalBLASStackSize = 32u;
    };

    struct BLASTracePassSettings
    {
      uint32_t maxBLASTests = 32u;
    };

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
    bool enableTraversalEarlyOut = true;
    float traversalEarlyOutDistanceBias = 0.0f;
    bool enableDynamicRayFetch = false;
    bool occlusionOnly = false;
    bool enableSubgroupTraversal = true;
    bool enableRaySorting = true;
    bool enableCameraRaySorting = false;
    bool enableTwoStageBLASTracing = false;
    bool enableFusedSmallCandidateTrace = false;
    bool enableRayStreamTraversal = false;
    bool enableRayStreamScheduler = false;
    bool enableTLASDOBB = false;
    bool enableBLASDOBB = false;
    bool enableQuantizedTLASNodes = true;
    bool generateCameraRays = true;
    uint32_t rayCount = 0u;
    uint32_t fusedSmallCandidateCount = 8u;
    uint32_t fusedSmallCandidateTraceMaxCandidates = 8u;
    uint32_t streamMaxDispatchPasses = 8u;
    uint32_t streamTaskBudgetMultiplier = 8u;
    uint32_t streamMaxLocalNodeIterations = 8u;
    TLASCandidatePassSettings tlasCandidatePass{};
    BLASTracePassSettings blasTracePass{};
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
    uint32_t twoStageBLASCandidatesEmitted = 0u;
    uint32_t twoStageBLASCandidateOverflows = 0u;
    uint32_t twoStageBLASCandidateSorts = 0u;
    uint32_t twoStageBLASCandidatesTested = 0u;
    uint32_t twoStageBLASCandidateDistanceEarlyStops = 0u;
    uint32_t twoStageMaxBLASCandidatesPerRay = 0u;
    uint32_t streamHashSlotsFlattened = 0u;
    uint32_t streamHashBlocksFlattened = 0u;
    uint32_t streamHashPacketsEmitted = 0u;
    uint32_t streamHashInsertFailures = 0u;
    uint32_t streamQueueOverflows = 0u;
    uint32_t streamFinalTaskCountA = 0u;
    uint32_t streamFinalTaskCountB = 0u;
    uint32_t streamFinalHashOccupiedCount = 0u;
    uint32_t streamFinalHashBlockCount = 0u;
  };
  static_assert(sizeof(StatsGPU) == 180u, "SoftwareRayTracePass::StatsGPU must stay tightly packed");

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

  struct BLASCandidateGPU
  {
    uint32_t instanceIndex = UINT32_MAX;
    uint32_t blasHandle = UINT32_MAX;
    float nearT = 0.0f;
    uint32_t flags = 0u;
  };
  static_assert(sizeof(BLASCandidateGPU) == 16u, "SoftwareRayTracePass::BLASCandidateGPU must stay tightly packed");

  struct StreamTaskGPU
  {
    uint32_t kind = 0u;
    uint32_t baseIndex = 0u;
    uint32_t mask = 0u;
    uint32_t instanceIndex = UINT32_MAX;
    uint32_t blasHandle = UINT32_MAX;
    uint32_t rayCount = 0u;
    uint32_t flags = 0u;
    uint32_t _padding0 = 0u;
    uint32_t rayRefs[32] = {};
  };
  static_assert(sizeof(StreamTaskGPU) == 160u, "SoftwareRayTracePass::StreamTaskGPU must stay tightly packed");

  struct StreamLocalRayGPU
  {
    uint32_t rayId = UINT32_MAX;
    uint32_t instanceIndex = UINT32_MAX;
    uint32_t blasHandle = UINT32_MAX;
    uint32_t _padding0 = 0u;
    float localOriginTMin[4] = {0.0f, 0.0f, 0.0f, 0.001f};
    float localDirectionTMax[4] = {0.0f, 0.0f, -1.0f, 3000.0f};
  };
  static_assert(sizeof(StreamLocalRayGPU) == 48u, "SoftwareRayTracePass::StreamLocalRayGPU must stay tightly packed");

  struct StreamGlobalHashSlotGPU
  {
    uint32_t keyHash = 0u;
    uint32_t generation = 0u;
    uint32_t headBlock = UINT32_MAX;
    uint32_t tailBlock = UINT32_MAX;
    uint32_t kind = 0u;
    uint32_t baseIndex = 0u;
    uint32_t mask = 0u;
    uint32_t instanceIndex = UINT32_MAX;
    uint32_t blasHandle = UINT32_MAX;
  };
  static_assert(sizeof(StreamGlobalHashSlotGPU) == 36u, "SoftwareRayTracePass::StreamGlobalHashSlotGPU must stay tightly packed");

  struct StreamPacketBlockGPU
  {
    uint32_t nextBlock = UINT32_MAX;
    uint32_t rayCount = 0u;
    uint32_t rayRefs[32] = {};
  };
  static_assert(sizeof(StreamPacketBlockGPU) == 136u, "SoftwareRayTracePass::StreamPacketBlockGPU must stay tightly packed");

  struct DispatchIndirectArgsGPU
  {
    uint32_t x = 0u;
    uint32_t y = 1u;
    uint32_t z = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgsGPU) == 12u, "SoftwareRayTracePass::DispatchIndirectArgsGPU must stay tightly packed");

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
    const bool requiresResourceRefresh = settings_.width != settings.width || settings_.height != settings.height ||
                                         maxBLASCandidateCount(settings_) != maxBLASCandidateCount(settings) ||
                                         streamTaskCapacity(settings_) != streamTaskCapacity(settings) ||
                                         streamLocalRayCapacity(settings_) != streamLocalRayCapacity(settings);
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

  const ::rendering::Timer &getTraceDispatchTimer() const
  {
    return traceDispatchTimer_;
  }

  void requestTraceDispatchTimer()
  {
    traceDispatchTimerRequested_ = true;
    if (!traceDispatchTimer_.isValid())
    {
      traceDispatchTimer_ = ::rendering::Timer{
        .name = passName + "_trace_dispatch.timer",
        .sampleCount = 1u,
      };
    }
  }

  void ensureTraceDispatchTimerAllocated()
  {
    requestTraceDispatchTimer();
    renderGraph->deleteTimer(traceDispatchTimer_);
    traceDispatchTimer_ = renderGraph->createTimer(
        ::rendering::TimerInfo{
          .name = traceDispatchTimer_.name,
          .unit = ::rendering::TimerUnit::Miliseconds,
          .sampleCount = traceDispatchTimer_.sampleCount,
        });
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

  void setExternalRayBuffers(
      ::rendering::Buffer rayInputBuffer,
      uint64_t rayInputBufferSize,
      ::rendering::Buffer hitOutputBuffer,
      uint64_t hitOutputBufferSize)
  {
    const bool rayChanged = rayInputBuffer_ != rayInputBuffer || rayInputBufferSize_ != rayInputBufferSize || ownsRayInputBuffer_;
    const bool hitChanged = hitOutputBuffer_ != hitOutputBuffer || hitOutputBufferSize_ != hitOutputBufferSize || ownsHitOutputBuffer_;
    if (rayChanged && ownsRayInputBuffer_ && rayInputBuffer_.isValid() && renderGraph != nullptr)
      renderGraph->deleteBuffer(rayInputBuffer_);
    if (hitChanged && ownsHitOutputBuffer_ && hitOutputBuffer_.isValid() && renderGraph != nullptr)
      renderGraph->deleteBuffer(hitOutputBuffer_);

    rayInputBuffer_ = rayInputBuffer;
    rayInputBufferSize_ = rayInputBufferSize;
    hitOutputBuffer_ = hitOutputBuffer;
    hitOutputBufferSize_ = hitOutputBufferSize;
    ownsRayInputBuffer_ = !rayInputBuffer.isValid();
    ownsHitOutputBuffer_ = !hitOutputBuffer.isValid();
    resourcesDirty_ = resourcesDirty_ || rayChanged || hitChanged;
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
    if (!settings_.enabled && !settings_.overlayTLASBounds && !settings_.overlayTLASCwbvhBounds)
    {
      return;
    }

    if (traceDispatchTimerRequested_ && !traceDispatchTimer_.isValid())
    {
      traceDispatchTimer_ = ::rendering::Timer{
        .name = passName + "_trace_dispatch.timer",
        .sampleCount = 1u,
      };
    }

    ensureResources();
    const bool streamTrace = shouldUseRayStreamScheduler();
    const bool fusedTrace = shouldUseFusedSmallCandidateTrace() && !streamTrace;
    const bool twoStageTrace = shouldUseTwoStageBLASTracing() && !fusedTrace && !streamTrace;
    if (!pipeline_.isValid() || !rayGenPipeline_.isValid() || !resolvePipeline_.isValid() || !clearStatsPipeline_.isValid() || !bindingGroups_.isValid() || !rayInputBuffer_.isValid() || settings_.width == 0u ||
        settings_.height == 0u || (streamTrace && (!streamSeedPipeline_.isValid() || !streamTraceAtoBPipeline_.isValid() ||
                                                    !streamTraceBtoAPipeline_.isValid() || !streamFinalizePipeline_.isValid() ||
                                                    !streamPrepareTraceAIndirectPipeline_.isValid() || !streamPrepareTraceBIndirectPipeline_.isValid() ||
                                                    !streamIndirectArgsBuffer_.isValid())) ||
        (fusedTrace && !fusedTLASBLASTracePipeline_.isValid()) || (twoStageTrace && (!tlasCandidatePipeline_.isValid() || !sortBLASCandidatesPipeline_.isValid() || !blasCandidateTracePipeline_.isValid())))
    {
      os::Logger::warningf(
          "[SWRT] skipped command recording pipeline=%s streamTrace=%s fusedTrace=%s tlasCandidates=%s sortCandidates=%s blasTrace=%s raygen=%s resolve=%s clear=%s bindings=%s rayInput=%s size=%ux%u shader=%s",
          pipeline_.isValid() ? "yes" : "no",
          streamSeedPipeline_.isValid() && streamTraceAtoBPipeline_.isValid() && streamTraceBtoAPipeline_.isValid() && streamFinalizePipeline_.isValid()
              ? "yes"
              : "no",
          fusedTLASBLASTracePipeline_.isValid() ? "yes" : "no",
          tlasCandidatePipeline_.isValid() ? "yes" : "no",
          sortBLASCandidatesPipeline_.isValid() ? "yes" : "no",
          blasCandidateTracePipeline_.isValid() ? "yes" : "no",
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
    if (traceDispatchTimerRequested_ && traceDispatchTimer_.isValid())
    {
      commandBuffer.cmdStartTimer(traceDispatchTimer_, ::rendering::PipelineStage::COMPUTE_SHADER);
    }
    if (streamTrace)
    {
      commandBuffer.cmdBindComputePipeline(streamSeedPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(streamSeedTraceWorkgroupCount(), 1u, 1u);

      const uint32_t maxPasses = streamMaxDispatchPasses(settings_);

      for (uint32_t pass = 0u; pass + 1u < maxPasses; ++pass)
      {
        const bool traceBToA = (pass & 1u) == 0u;
        commandBuffer.cmdBindComputePipeline(traceBToA ? streamPrepareTraceBIndirectPipeline_ : streamPrepareTraceAIndirectPipeline_);
        commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
        commandBuffer.cmdDispatch(1u, 1u, 1u);

        commandBuffer.cmdBindComputePipeline(traceBToA ? streamTraceBtoAPipeline_ : streamTraceAtoBPipeline_);
        commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
        commandBuffer.cmdDispatchIndirect(streamIndirectArgsBuffer_, 0u);

      }

      if (shouldRecordStreamFinalize())
      {
        commandBuffer.cmdBindComputePipeline(streamFinalizePipeline_);
        commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
        commandBuffer.cmdDispatch(std::max(1u, (rayCount() + 63u) / 64u), 1u, 1u);
      }
    }
    else if (fusedTrace)
    {
      commandBuffer.cmdBindComputePipeline(fusedTLASBLASTracePipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (traceDispatchWidth() + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (traceDispatchHeight() + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);
    }
    else if (twoStageTrace)
    {
      commandBuffer.cmdBindComputePipeline(tlasCandidatePipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (traceDispatchWidth() + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (traceDispatchHeight() + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);

      commandBuffer.cmdBindComputePipeline(sortBLASCandidatesPipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (rayCount() + 63u) / 64u), 1u, 1u);

      commandBuffer.cmdBindComputePipeline(blasCandidateTracePipeline_);
      commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
      commandBuffer.cmdDispatch(std::max(1u, (traceDispatchWidth() + kWorkgroupSizeX - 1u) / kWorkgroupSizeX), std::max(1u, (traceDispatchHeight() + kWorkgroupSizeY - 1u) / kWorkgroupSizeY), 1u);
    }
    else
    {
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
    }
    if (traceDispatchTimerRequested_ && traceDispatchTimer_.isValid())
    {
      commandBuffer.cmdStopTimer(traceDispatchTimer_, ::rendering::PipelineStage::COMPUTE_SHADER);
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
  static constexpr uint32_t kStreamPacketsPerWorkgroup = 4u;

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
    uint32_t traceFlags[4] = {0u, 0u, 0u, 0u};
    uint32_t twoStageControl[4] = {32u, 32u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 256u, "SoftwareRayTracePass::Uniforms must stay tightly packed");

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

  static ::rendering::BindingGroupLayoutBufferEntry makeLayoutBuffer(const char *name, uint32_t binding, ::rendering::BufferBindingType type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer)
  {
    return ::rendering::BindingGroupLayoutBufferEntry{
      .name = name,
      .binding = binding,
      .type = type,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
    };
  }

  static ::rendering::BindingBuffer makeBufferBinding(uint32_t binding, const ::rendering::Buffer &buffer, ::rendering::AccessPattern access, uint64_t size)
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
      if (!worldCWBVHMode)
      {
        buffers.push_back(makeLayoutBuffer("blasQuantizedTrianglePayloads", 29u));
      }
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
    buffers.push_back(makeLayoutBuffer("blasCandidates", 25u));
    buffers.push_back(makeLayoutBuffer("blasCandidateCounts", 26u));
    buffers.push_back(makeLayoutBuffer("tlasDobbNodes", 27u));
    buffers.push_back(makeLayoutBuffer("tlasNodeMetadata", 28u));
    buffers.push_back(makeLayoutBuffer("blasDobbNodes", 30u));
    buffers.push_back(makeLayoutBuffer("blasNodeMetadata", 31u));
    buffers.push_back(makeLayoutBuffer("streamTasksA", 32u));
    buffers.push_back(makeLayoutBuffer("streamTasksB", 33u));
    buffers.push_back(makeLayoutBuffer("streamCounters", 34u));
    buffers.push_back(makeLayoutBuffer("streamLocalRays", 35u));
    buffers.push_back(makeLayoutBuffer("streamHitT", 36u));
    buffers.push_back(makeLayoutBuffer("streamHashSlots", 37u));
    buffers.push_back(makeLayoutBuffer("streamPacketBlocks", 38u));
    buffers.push_back(makeLayoutBuffer("streamHashOccupiedSlots", 39u));
    buffers.push_back(makeLayoutBuffer("streamIndirectArgs", 40u));
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
      buffers.push_back(makeBufferBinding(
          6u, worldCWBVHMode ? worldTriangleTransformsBuffer_ : blasManager_.getTriangleTransformsBuffer(), read, worldCWBVHMode ? worldTriangleTransformsBufferSize_ : blasManager_.getTriangleTransformsBufferSize()));
      if (!worldCWBVHMode)
      {
        buffers.push_back(makeBufferBinding(29u, blasManager_.getQuantizedTrianglePayloadsBuffer(), read, blasManager_.getQuantizedTrianglePayloadsBufferSize()));
      }
    }

    buffers.push_back(
        makeBufferBinding(9u, worldCWBVHMode ? worldSceneCWBVHNodesBuffer_ : blasManager_.getGPUCWBVHNodesBuffer(), read, worldCWBVHMode ? worldSceneCWBVHNodesBufferSize_ : blasManager_.getGPUCWBVHNodesBufferSize()));
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
    buffers.push_back(makeBufferBinding(25u, blasCandidateBuffer_, readWrite, blasCandidateBufferSize_));
    buffers.push_back(makeBufferBinding(26u, blasCandidateCountBuffer_, readWrite, blasCandidateCountBufferSize_));
    buffers.push_back(makeBufferBinding(27u, tlasManager_.getDobbCwbvhNodesBuffer(), read, tlasManager_.getDobbCwbvhNodesBufferSize()));
    buffers.push_back(makeBufferBinding(28u, tlasManager_.getCwbvhNodeMetadataBuffer(), read, tlasManager_.getCwbvhNodeMetadataBufferSize()));
    buffers.push_back(makeBufferBinding(30u, blasManager_.getGPUDobbCWBVHNodesBuffer(), read, blasManager_.getGPUDobbCWBVHNodesBufferSize()));
    buffers.push_back(makeBufferBinding(31u, blasManager_.getGPUCWBVHNodeMetadataBuffer(), read, blasManager_.getGPUCWBVHNodeMetadataBufferSize()));
    buffers.push_back(makeBufferBinding(32u, streamTaskBufferA_, readWrite, streamTaskBufferSize_));
    buffers.push_back(makeBufferBinding(33u, streamTaskBufferB_, readWrite, streamTaskBufferSize_));
    buffers.push_back(makeBufferBinding(34u, streamCounterBuffer_, readWrite, streamCounterBufferSize()));
    buffers.push_back(makeBufferBinding(35u, streamLocalRayBuffer_, readWrite, streamLocalRayBufferSize_));
    buffers.push_back(makeBufferBinding(36u, streamHitTBuffer_, readWrite, streamHitTBufferSize_));
    buffers.push_back(makeBufferBinding(37u, streamHashSlotBuffer_, readWrite, streamHashSlotBufferSize_));
    buffers.push_back(makeBufferBinding(38u, streamPacketBlockBuffer_, readWrite, streamPacketBlockBufferSize_));
    buffers.push_back(makeBufferBinding(39u, streamHashOccupiedSlotBuffer_, readWrite, streamHashOccupiedSlotBufferSize_));
    buffers.push_back(makeBufferBinding(40u, streamIndirectArgsBuffer_, readWrite, streamIndirectArgsBufferSize()));
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
    return worldSceneCWBVHNodesBuffer_.isValid() && worldSceneCWBVHNodesBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU) && worldTriangleTransformsBuffer_.isValid() &&
           worldTriangleTransformsBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU) && worldLeafRemapBuffer_.isValid() &&
           worldLeafRemapBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::WorldLeafEntryGPU) && worldInstanceRangesBuffer_.isValid() &&
           worldInstanceRangesBufferSize_ >= sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU);
  }

  static const char *shaderPathForSettings(const Settings &settings)
  {
    if (settings.traversalMode == TraversalMode::WorldCWBVH)
    {
      if (settings.enableSubgroupTraversal)
      {
        return needsDebugShaderVariant(settings) ? "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-debug.spirv"
                                                 : "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-release.spirv";
      }
      return needsDebugShaderVariant(settings) ? "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-debug.spirv" : "assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-release.spirv";
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
    if (fusedTLASBLASTracePipeline_.isValid())
      renderGraph->deleteComputePipeline(fusedTLASBLASTracePipeline_);
    if (tlasCandidatePipeline_.isValid())
      renderGraph->deleteComputePipeline(tlasCandidatePipeline_);
    if (sortBLASCandidatesPipeline_.isValid())
      renderGraph->deleteComputePipeline(sortBLASCandidatesPipeline_);
    if (blasCandidateTracePipeline_.isValid())
      renderGraph->deleteComputePipeline(blasCandidateTracePipeline_);
    if (streamSeedPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamSeedPipeline_);
    if (streamClearTaskAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearTaskAPipeline_);
    if (streamClearTaskBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearTaskBPipeline_);
    if (streamClearHashSlotsPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearHashSlotsPipeline_);
    if (streamPrepareTraceAIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareTraceAIndirectPipeline_);
    if (streamPrepareTraceBIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareTraceBIndirectPipeline_);
    if (streamPrepareHashIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareHashIndirectPipeline_);
    if (streamTraceAtoBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamTraceAtoBPipeline_);
    if (streamTraceBtoAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamTraceBtoAPipeline_);
    if (streamFlattenHashToBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFlattenHashToBPipeline_);
    if (streamFlattenHashToAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFlattenHashToAPipeline_);
    if (streamFinalizePipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFinalizePipeline_);
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
    if (rayInputBuffer_.isValid() && ownsRayInputBuffer_)
      renderGraph->deleteBuffer(rayInputBuffer_);
    if (hitOutputBuffer_.isValid() && ownsHitOutputBuffer_)
      renderGraph->deleteBuffer(hitOutputBuffer_);
    if (raySortBinCountsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortBinCountsBuffer_);
    if (raySortBinOffsetsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortBinOffsetsBuffer_);
    if (raySortIdsBuffer_.isValid())
      renderGraph->deleteBuffer(raySortIdsBuffer_);
    if (blasCandidateBuffer_.isValid())
      renderGraph->deleteBuffer(blasCandidateBuffer_);
    if (blasCandidateCountBuffer_.isValid())
      renderGraph->deleteBuffer(blasCandidateCountBuffer_);
    if (streamTaskBufferA_.isValid())
      renderGraph->deleteBuffer(streamTaskBufferA_);
    if (streamTaskBufferB_.isValid())
      renderGraph->deleteBuffer(streamTaskBufferB_);
    if (streamCounterBuffer_.isValid())
      renderGraph->deleteBuffer(streamCounterBuffer_);
    if (streamLocalRayBuffer_.isValid())
      renderGraph->deleteBuffer(streamLocalRayBuffer_);
    if (streamHitTBuffer_.isValid())
      renderGraph->deleteBuffer(streamHitTBuffer_);
    if (streamHashSlotBuffer_.isValid())
      renderGraph->deleteBuffer(streamHashSlotBuffer_);
    if (streamPacketBlockBuffer_.isValid())
      renderGraph->deleteBuffer(streamPacketBlockBuffer_);
    if (streamHashOccupiedSlotBuffer_.isValid())
      renderGraph->deleteBuffer(streamHashOccupiedSlotBuffer_);
    if (streamIndirectArgsBuffer_.isValid())
      renderGraph->deleteBuffer(streamIndirectArgsBuffer_);
    if (outputTexture_.isValid() && ownsOutputTexture_)
      renderGraph->deleteTexture(outputTexture_);
    if (traceDispatchTimer_.isValid())
      renderGraph->deleteTimer(traceDispatchTimer_);

    pipeline_ = {};
    externalRayPipeline_ = {};
    rayGenPipeline_ = {};
    resolvePipeline_ = {};
    clearStatsPipeline_ = {};
    clearRaySortPipeline_ = {};
    countRaySortPipeline_ = {};
    prefixRaySortPipeline_ = {};
    scatterRaySortPipeline_ = {};
    fusedTLASBLASTracePipeline_ = {};
    tlasCandidatePipeline_ = {};
    sortBLASCandidatesPipeline_ = {};
    blasCandidateTracePipeline_ = {};
    streamSeedPipeline_ = {};
    streamClearTaskAPipeline_ = {};
    streamClearTaskBPipeline_ = {};
    streamClearHashSlotsPipeline_ = {};
    streamPrepareTraceAIndirectPipeline_ = {};
    streamPrepareTraceBIndirectPipeline_ = {};
    streamPrepareHashIndirectPipeline_ = {};
    streamTraceAtoBPipeline_ = {};
    streamTraceBtoAPipeline_ = {};
    streamFlattenHashToBPipeline_ = {};
    streamFlattenHashToAPipeline_ = {};
    streamFinalizePipeline_ = {};
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
    blasCandidateBuffer_ = {};
    blasCandidateCountBuffer_ = {};
    streamTaskBufferA_ = {};
    streamTaskBufferB_ = {};
    streamCounterBuffer_ = {};
    streamLocalRayBuffer_ = {};
    streamHitTBuffer_ = {};
    streamHashSlotBuffer_ = {};
    streamPacketBlockBuffer_ = {};
    streamHashOccupiedSlotBuffer_ = {};
    streamIndirectArgsBuffer_ = {};
    outputTexture_ = {};
    outputWidth_ = 0u;
    outputHeight_ = 0u;
    hitOutputBufferSize_ = 0u;
    rayInputBufferSize_ = 0u;
    raySortIdsBufferSize_ = 0u;
    blasCandidateBufferSize_ = 0u;
    blasCandidateCountBufferSize_ = 0u;
    streamTaskBufferSize_ = 0u;
    streamLocalRayBufferSize_ = 0u;
    streamHitTBufferSize_ = 0u;
    streamHashSlotBufferSize_ = 0u;
    streamPacketBlockBufferSize_ = 0u;
    streamHashOccupiedSlotBufferSize_ = 0u;
    traceDispatchTimer_ = {};
    traceDispatchTimerRequested_ = false;
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
    if (fusedTLASBLASTracePipeline_.isValid())
      renderGraph->deleteComputePipeline(fusedTLASBLASTracePipeline_);
    if (tlasCandidatePipeline_.isValid())
      renderGraph->deleteComputePipeline(tlasCandidatePipeline_);
    if (sortBLASCandidatesPipeline_.isValid())
      renderGraph->deleteComputePipeline(sortBLASCandidatesPipeline_);
    if (blasCandidateTracePipeline_.isValid())
      renderGraph->deleteComputePipeline(blasCandidateTracePipeline_);
    if (streamSeedPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamSeedPipeline_);
    if (streamClearTaskAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearTaskAPipeline_);
    if (streamClearTaskBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearTaskBPipeline_);
    if (streamClearHashSlotsPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamClearHashSlotsPipeline_);
    if (streamPrepareTraceAIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareTraceAIndirectPipeline_);
    if (streamPrepareTraceBIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareTraceBIndirectPipeline_);
    if (streamPrepareHashIndirectPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamPrepareHashIndirectPipeline_);
    if (streamTraceAtoBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamTraceAtoBPipeline_);
    if (streamTraceBtoAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamTraceBtoAPipeline_);
    if (streamFlattenHashToBPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFlattenHashToBPipeline_);
    if (streamFlattenHashToAPipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFlattenHashToAPipeline_);
    if (streamFinalizePipeline_.isValid())
      renderGraph->deleteComputePipeline(streamFinalizePipeline_);
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
    fusedTLASBLASTracePipeline_ = {};
    tlasCandidatePipeline_ = {};
    sortBLASCandidatesPipeline_ = {};
    blasCandidateTracePipeline_ = {};
    streamSeedPipeline_ = {};
    streamClearTaskAPipeline_ = {};
    streamClearTaskBPipeline_ = {};
    streamClearHashSlotsPipeline_ = {};
    streamPrepareTraceAIndirectPipeline_ = {};
    streamPrepareTraceBIndirectPipeline_ = {};
    streamPrepareHashIndirectPipeline_ = {};
    streamTraceAtoBPipeline_ = {};
    streamTraceBtoAPipeline_ = {};
    streamFlattenHashToBPipeline_ = {};
    streamFlattenHashToAPipeline_ = {};
    streamFinalizePipeline_ = {};
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
    ensureBLASCandidateBuffers();
    ensureStreamBuffers();

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

    if (shouldUseFusedSmallCandidateTrace())
    {
      if (!fusedTLASBLASTracePipeline_.isValid())
      {
        fusedTLASBLASTracePipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "main_trace_tlas_blas_fused",
              .layout = layout_,
              .name = passName + "_FusedTLASBLASTracePipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
    }

    if (shouldUseTwoStageBLASTracing() && !shouldUseFusedSmallCandidateTrace())
    {
      if (!tlasCandidatePipeline_.isValid())
      {
        tlasCandidatePipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "main_tlas_blas_candidates",
              .layout = layout_,
              .name = passName + "_TLASBLASCandidatesPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }

      if (!sortBLASCandidatesPipeline_.isValid())
      {
        sortBLASCandidatesPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "sort_blas_candidates_by_distance",
              .layout = layout_,
              .name = passName + "_SortBLASCandidatesPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }

      if (!blasCandidateTracePipeline_.isValid())
      {
        blasCandidateTracePipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "main_trace_blas_candidates",
              .layout = layout_,
              .name = passName + "_TraceBLASCandidatesPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
    }

    if (shouldUseRayStreamScheduler())
    {
      if (!streamSeedPipeline_.isValid())
      {
        streamSeedPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_seed_trace_tlas_root_to_b",
              .layout = layout_,
              .name = passName + "_StreamSeedPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
      if (!streamPrepareTraceAIndirectPipeline_.isValid())
      {
        streamPrepareTraceAIndirectPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_prepare_trace_a_indirect",
              .layout = layout_,
              .name = passName + "_StreamPrepareTraceAIndirectPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
      if (!streamPrepareTraceBIndirectPipeline_.isValid())
      {
        streamPrepareTraceBIndirectPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_prepare_trace_b_indirect",
              .layout = layout_,
              .name = passName + "_StreamPrepareTraceBIndirectPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
      if (!streamTraceAtoBPipeline_.isValid())
      {
        streamTraceAtoBPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_trace_a_to_b",
              .layout = layout_,
              .name = passName + "_StreamTraceAtoBPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
      if (!streamTraceBtoAPipeline_.isValid())
      {
        streamTraceBtoAPipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_trace_b_to_a",
              .layout = layout_,
              .name = passName + "_StreamTraceBtoAPipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
      if (!streamFinalizePipeline_.isValid())
      {
        streamFinalizePipeline_ = renderGraph->createComputePipeline(
            ::rendering::ComputePipelineInfo{
              .entry = "stream_finalize_hits",
              .layout = layout_,
              .name = passName + "_StreamFinalizePipeline",
              .shader = shader_,
            });
        resourcesDirty_ = true;
      }
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
    uniforms.traceFlags[0] = settings_.occlusionOnly ? 1u : 0u;
    uniforms.traceFlags[1] = (shouldUseBLASDOBB() ? 1u : 0u) | (streamMaxLocalNodeIterations(settings_) << 8u);
    uniforms.traceFlags[2] = streamTaskCapacity();
    uniforms.traceFlags[3] = nextStreamHashGeneration();
    uniforms.twoStageControl[0] = shouldUseFusedSmallCandidateTrace() ? fusedBLASCandidateCount(settings_) : maxBLASCandidateCount(settings_);
    uniforms.twoStageControl[1] = shouldUseFusedSmallCandidateTrace() ? fusedBLASCandidateCount(settings_) : maxBLASTraceTests(settings_);
    uniforms.twoStageControl[2] = shouldUseTwoStageBLASTracing() ? 1u : 0u;
    uniforms.twoStageControl[3] = shouldUseTLASDOBB() ? 1u : 0u;
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
    if (!ownsRayInputBuffer_)
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
    if (!ownsHitOutputBuffer_)
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
           (!settings_.generateCameraRays || settings_.enableCameraRaySorting) &&
           isCWBVHTraceMode() &&
           !traceDispatchIndirectBuffer_.isValid() &&
           rayCount() > 1u;
  }

  bool shouldUseTwoStageBLASTracing() const
  {
    return !shouldUseRayStreamScheduler() &&
           (settings_.enableTwoStageBLASTracing ||
            settings_.enableFusedSmallCandidateTrace ||
            settings_.enableRayStreamTraversal ||
            isTwoStageCandidateDebugMode(settings_.debugMode)) &&
           settings_.primitiveMode == PrimitiveMode::Triangle &&
           settings_.traversalMode == TraversalMode::LocalInstance &&
           settings_.tlasMode == TLASMode::CWBVH &&
           !settings_.occlusionOnly &&
           !traceDispatchIndirectBuffer_.isValid();
  }

  static bool isTwoStageCandidateDebugMode(const DebugMode debugMode)
  {
    return debugMode == DebugMode::BLASCandidateCount ||
           debugMode == DebugMode::BLASSortedCandidateDistance ||
           debugMode == DebugMode::BLASTestsPerRay ||
           debugMode == DebugMode::TLASCandidateOverflow;
  }

  bool shouldUseFusedSmallCandidateTrace() const
  {
    const uint32_t fusedCandidateCount = fusedBLASCandidateCount(settings_);
    const uint32_t fusedCandidateLimit = std::clamp(settings_.fusedSmallCandidateTraceMaxCandidates, 1u, maxBLASCandidateCount(settings_));
    return (settings_.enableFusedSmallCandidateTrace || settings_.enableRayStreamTraversal) &&
           fusedCandidateCount <= fusedCandidateLimit &&
           shouldUseTwoStageBLASTracing() &&
           !isTwoStageCandidateDebugMode(settings_.debugMode);
  }

  bool shouldUseRayStreamScheduler() const
  {
    return settings_.enableRayStreamTraversal &&
           settings_.enableRayStreamScheduler &&
           settings_.primitiveMode == PrimitiveMode::Triangle &&
           settings_.traversalMode == TraversalMode::LocalInstance &&
           settings_.tlasMode == TLASMode::CWBVH &&
           !settings_.occlusionOnly &&
           !isTwoStageCandidateDebugMode(settings_.debugMode) &&
           !traceDispatchIndirectBuffer_.isValid();
  }

  static uint32_t maxBLASCandidateCount(const Settings &settings)
  {
    return std::clamp(settings.tlasCandidatePass.maxLocalBLASStackSize, 1u, 32u);
  }

  static uint32_t maxBLASTraceTests(const Settings &settings)
  {
    return std::clamp(settings.blasTracePass.maxBLASTests, 1u, maxBLASCandidateCount(settings));
  }

  static uint32_t fusedBLASCandidateCount(const Settings &settings)
  {
    return std::clamp(settings.fusedSmallCandidateCount, 1u, maxBLASCandidateCount(settings));
  }

  bool shouldUseTLASDOBB() const
  {
    return settings_.enableTLASDOBB &&
           settings_.enableQuantizedTLASNodes &&
           settings_.tlasMode == TLASMode::CWBVH &&
           !isWorldCWBVHMode() &&
           tlasManager_.getDobbCwbvhNodeCount() >= tlasManager_.getCwbvhNodeCount() &&
           tlasManager_.getCwbvhNodeCount() > 0u;
  }

  bool shouldUseBLASDOBB() const
  {
    return settings_.enableBLASDOBB &&
           settings_.primitiveMode == PrimitiveMode::Triangle &&
           settings_.traversalMode == TraversalMode::LocalInstance &&
           !isWorldCWBVHMode() &&
           blasManager_.getGPUDobbCWBVHNodeCount() >= blasManager_.getGPUCWBVHNodeCount() &&
           blasManager_.getGPUCWBVHNodeCount() > 0u;
  }

  bool isCWBVHTraceMode() const
  {
    return settings_.traversalMode == TraversalMode::WorldCWBVH || settings_.tlasMode == TLASMode::CWBVH;
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

  uint64_t blasCandidateBufferSize() const
  {
    return static_cast<uint64_t>(std::max(1u, rayCount())) * static_cast<uint64_t>(maxBLASCandidateCount(settings_)) * sizeof(BLASCandidateGPU);
  }

  uint64_t blasCandidateCountBufferSize() const
  {
    return static_cast<uint64_t>(std::max(1u, rayCount())) * sizeof(uint32_t);
  }

  static uint32_t streamMaxDispatchPasses(const Settings &settings)
  {
    return std::clamp(settings.streamMaxDispatchPasses, 1u, 8u);
  }

  static uint32_t streamTaskBudgetMultiplier(const Settings &settings)
  {
    return std::clamp(settings.streamTaskBudgetMultiplier, 1u, 256u);
  }

  static uint32_t streamMaxLocalNodeIterations(const Settings &settings)
  {
    return std::clamp(settings.streamMaxLocalNodeIterations, 1u, 8u);
  }

  static uint32_t streamPacketCountForRayCount(uint32_t count)
  {
    return std::max(1u, (std::max(1u, count) + 31u) / 32u);
  }

  uint32_t streamSeedTraceWorkgroupCount() const
  {
    const uint32_t packetCount = streamPacketCountForRayCount(rayCount());
    return std::max(1u, (packetCount + kStreamPacketsPerWorkgroup - 1u) / kStreamPacketsPerWorkgroup);
  }

  bool shouldRecordStreamFinalize() const
  {
    return needsDebugShaderVariant(settings_) || settings_.debugMode == DebugMode::Heatmap || settings_.debugMode == DebugMode::PrimaryRayHeatmap;
  }

  static uint32_t streamTaskCapacity(const Settings &settings, uint32_t count)
  {
    if (!settings.enableRayStreamTraversal || !settings.enableRayStreamScheduler)
    {
      return 1u;
    }
    const uint64_t packetCount = streamPacketCountForRayCount(count);
    const uint64_t capacity = packetCount * static_cast<uint64_t>(streamTaskBudgetMultiplier(settings));
    return static_cast<uint32_t>(std::min<uint64_t>(capacity, std::numeric_limits<uint32_t>::max()));
  }

  static uint32_t streamTaskCapacity(const Settings &settings)
  {
    const uint32_t count = settings.generateCameraRays || settings.rayCount == 0u ? settings.width * settings.height : settings.rayCount;
    return streamTaskCapacity(settings, count);
  }

  uint32_t streamTaskCapacity() const
  {
    return streamTaskCapacity(settings_, rayCount());
  }

  static uint32_t streamLocalRayCapacity(const Settings &settings)
  {
    (void)settings;
    return 1u;
  }

  uint32_t streamLocalRayCapacity() const
  {
    return streamLocalRayCapacity(settings_);
  }

  uint32_t nextStreamHashGeneration()
  {
    ++streamHashGeneration_;
    if (streamHashGeneration_ == 0u || streamHashGeneration_ == std::numeric_limits<uint32_t>::max())
    {
      streamHashGeneration_ = 1u;
    }
    return streamHashGeneration_;
  }

  uint64_t streamTaskBufferSize() const
  {
    return static_cast<uint64_t>(std::max(1u, streamTaskCapacity())) * sizeof(StreamTaskGPU);
  }

  static uint64_t streamCounterBufferSize()
  {
    return 8ull * sizeof(uint32_t);
  }

  uint64_t streamLocalRayBufferSize() const
  {
    return static_cast<uint64_t>(std::max(1u, streamLocalRayCapacity())) * sizeof(StreamLocalRayGPU);
  }

  uint64_t streamHitTBufferSize() const
  {
    const uint32_t count = settings_.enableRayStreamTraversal && settings_.enableRayStreamScheduler ? rayCount() : 1u;
    return static_cast<uint64_t>(std::max(1u, count)) * sizeof(uint32_t);
  }

  uint32_t streamHashSlotCapacity() const
  {
    return 1u;
  }

  uint32_t streamPacketBlockCapacity() const
  {
    return 1u;
  }

  uint64_t streamHashSlotBufferSize() const
  {
    return static_cast<uint64_t>(streamHashSlotCapacity()) * sizeof(StreamGlobalHashSlotGPU);
  }

  uint64_t streamPacketBlockBufferSize() const
  {
    return static_cast<uint64_t>(streamPacketBlockCapacity()) * sizeof(StreamPacketBlockGPU);
  }

  uint64_t streamHashOccupiedSlotBufferSize() const
  {
    return static_cast<uint64_t>(streamHashSlotCapacity()) * sizeof(uint32_t);
  }

  static uint64_t streamIndirectArgsBufferSize()
  {
    return sizeof(DispatchIndirectArgsGPU);
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

  void ensureBLASCandidateBuffers()
  {
    if (renderGraph == nullptr || settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    const uint64_t requiredCandidateSize = blasCandidateBufferSize();
    if (!blasCandidateBuffer_.isValid() || blasCandidateBufferSize_ != requiredCandidateSize)
    {
      if (blasCandidateBuffer_.isValid())
      {
        renderGraph->deleteBuffer(blasCandidateBuffer_);
        blasCandidateBuffer_ = {};
        blasCandidateBufferSize_ = 0u;
      }
      blasCandidateBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_BLASCandidates.buffer",
            .size = requiredCandidateSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      blasCandidateBufferSize_ = requiredCandidateSize;
      resourcesDirty_ = true;
    }

    const uint64_t requiredCandidateCountSize = blasCandidateCountBufferSize();
    if (!blasCandidateCountBuffer_.isValid() || blasCandidateCountBufferSize_ != requiredCandidateCountSize)
    {
      if (blasCandidateCountBuffer_.isValid())
      {
        renderGraph->deleteBuffer(blasCandidateCountBuffer_);
        blasCandidateCountBuffer_ = {};
        blasCandidateCountBufferSize_ = 0u;
      }
      blasCandidateCountBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_BLASCandidateCounts.buffer",
            .size = requiredCandidateCountSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      blasCandidateCountBufferSize_ = requiredCandidateCountSize;
      resourcesDirty_ = true;
    }
  }

  void ensureStreamBuffers()
  {
    if (renderGraph == nullptr || settings_.width == 0u || settings_.height == 0u)
    {
      return;
    }

    const uint64_t requiredTaskSize = streamTaskBufferSize();
    if (!streamTaskBufferA_.isValid() || !streamTaskBufferB_.isValid() || streamTaskBufferSize_ != requiredTaskSize)
    {
      if (streamTaskBufferA_.isValid())
      {
        renderGraph->deleteBuffer(streamTaskBufferA_);
        streamTaskBufferA_ = {};
      }
      if (streamTaskBufferB_.isValid())
      {
        renderGraph->deleteBuffer(streamTaskBufferB_);
        streamTaskBufferB_ = {};
      }

      streamTaskBufferA_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamTasksA.buffer",
            .size = requiredTaskSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamTaskBufferB_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamTasksB.buffer",
            .size = requiredTaskSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamTaskBufferSize_ = requiredTaskSize;
      resourcesDirty_ = true;
    }

    if (!streamCounterBuffer_.isValid())
    {
      streamCounterBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamCounters.buffer",
            .size = streamCounterBufferSize(),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      resourcesDirty_ = true;
    }

    const uint64_t requiredLocalRaySize = streamLocalRayBufferSize();
    if (!streamLocalRayBuffer_.isValid() || streamLocalRayBufferSize_ != requiredLocalRaySize)
    {
      if (streamLocalRayBuffer_.isValid())
      {
        renderGraph->deleteBuffer(streamLocalRayBuffer_);
        streamLocalRayBuffer_ = {};
      }
      streamLocalRayBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamLocalRays.buffer",
            .size = requiredLocalRaySize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamLocalRayBufferSize_ = requiredLocalRaySize;
      resourcesDirty_ = true;
    }

    const uint64_t requiredHitTSize = streamHitTBufferSize();
    if (!streamHitTBuffer_.isValid() || streamHitTBufferSize_ != requiredHitTSize)
    {
      if (streamHitTBuffer_.isValid())
      {
        renderGraph->deleteBuffer(streamHitTBuffer_);
        streamHitTBuffer_ = {};
      }
      streamHitTBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamHitT.buffer",
            .size = requiredHitTSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamHitTBufferSize_ = requiredHitTSize;
      resourcesDirty_ = true;
    }

    const uint64_t requiredHashSlotSize = streamHashSlotBufferSize();
    if (!streamHashSlotBuffer_.isValid() || streamHashSlotBufferSize_ != requiredHashSlotSize)
    {
      if (streamHashSlotBuffer_.isValid())
      {
        renderGraph->deleteBuffer(streamHashSlotBuffer_);
        streamHashSlotBuffer_ = {};
      }
      streamHashSlotBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamHashSlots.buffer",
            .size = requiredHashSlotSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamHashSlotBufferSize_ = requiredHashSlotSize;
      resourcesDirty_ = true;
    }

    const uint64_t requiredPacketBlockSize = streamPacketBlockBufferSize();
    if (!streamPacketBlockBuffer_.isValid() || streamPacketBlockBufferSize_ != requiredPacketBlockSize)
    {
      if (streamPacketBlockBuffer_.isValid())
      {
        renderGraph->deleteBuffer(streamPacketBlockBuffer_);
        streamPacketBlockBuffer_ = {};
      }
      streamPacketBlockBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamPacketBlocks.buffer",
            .size = requiredPacketBlockSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamPacketBlockBufferSize_ = requiredPacketBlockSize;
      resourcesDirty_ = true;
    }

    const uint64_t requiredOccupiedSlotSize = streamHashOccupiedSlotBufferSize();
    if (!streamHashOccupiedSlotBuffer_.isValid() || streamHashOccupiedSlotBufferSize_ != requiredOccupiedSlotSize)
    {
      if (streamHashOccupiedSlotBuffer_.isValid())
      {
        renderGraph->deleteBuffer(streamHashOccupiedSlotBuffer_);
        streamHashOccupiedSlotBuffer_ = {};
      }
      streamHashOccupiedSlotBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamHashOccupiedSlots.buffer",
            .size = requiredOccupiedSlotSize,
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
          });
      streamHashOccupiedSlotBufferSize_ = requiredOccupiedSlotSize;
      resourcesDirty_ = true;
    }

    if (!streamIndirectArgsBuffer_.isValid())
    {
      streamIndirectArgsBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_StreamIndirectArgs.buffer",
            .size = streamIndirectArgsBufferSize(),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Indirect,
          });
      resourcesDirty_ = true;
    }
  }

  softwarebvh::SoftwareTLASManager &tlasManager_;
  softwarebvh::SoftwareBLASManager &blasManager_;
  Settings settings_{};
  bool resourcesDirty_ = true;
  bool ownsOutputTexture_ = false;
  bool ownsRayInputBuffer_ = true;
  bool ownsHitOutputBuffer_ = true;
  uint32_t outputWidth_ = 0u;
  uint32_t outputHeight_ = 0u;
  uint32_t lastBlasBuildRevision_ = std::numeric_limits<uint32_t>::max();
  uint64_t hitOutputBufferSize_ = 0u;
  uint64_t rayInputBufferSize_ = 0u;
  uint64_t raySortIdsBufferSize_ = 0u;
  uint64_t blasCandidateBufferSize_ = 0u;
  uint64_t blasCandidateCountBufferSize_ = 0u;
  uint64_t streamTaskBufferSize_ = 0u;
  uint64_t streamLocalRayBufferSize_ = 0u;
  uint64_t streamHitTBufferSize_ = 0u;
  uint64_t streamHashSlotBufferSize_ = 0u;
  uint64_t streamPacketBlockBufferSize_ = 0u;
  uint64_t streamHashOccupiedSlotBufferSize_ = 0u;
  std::string shaderPath_;

  ::rendering::Texture outputTexture_;
  ::rendering::Buffer uniformBuffer_;
  ::rendering::Buffer statsBuffer_;
  ::rendering::Buffer rayInputBuffer_;
  ::rendering::Buffer hitOutputBuffer_;
  ::rendering::Buffer raySortBinCountsBuffer_;
  ::rendering::Buffer raySortBinOffsetsBuffer_;
  ::rendering::Buffer raySortIdsBuffer_;
  ::rendering::Buffer blasCandidateBuffer_;
  ::rendering::Buffer blasCandidateCountBuffer_;
  ::rendering::Buffer streamTaskBufferA_;
  ::rendering::Buffer streamTaskBufferB_;
  ::rendering::Buffer streamCounterBuffer_;
  ::rendering::Buffer streamLocalRayBuffer_;
  ::rendering::Buffer streamHitTBuffer_;
  ::rendering::Buffer streamHashSlotBuffer_;
  ::rendering::Buffer streamPacketBlockBuffer_;
  ::rendering::Buffer streamHashOccupiedSlotBuffer_;
  ::rendering::Buffer streamIndirectArgsBuffer_;
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
  ::rendering::ComputePipeline fusedTLASBLASTracePipeline_;
  ::rendering::ComputePipeline tlasCandidatePipeline_;
  ::rendering::ComputePipeline sortBLASCandidatesPipeline_;
  ::rendering::ComputePipeline blasCandidateTracePipeline_;
  ::rendering::ComputePipeline streamSeedPipeline_;
  ::rendering::ComputePipeline streamClearTaskAPipeline_;
  ::rendering::ComputePipeline streamClearTaskBPipeline_;
  ::rendering::ComputePipeline streamClearHashSlotsPipeline_;
  ::rendering::ComputePipeline streamPrepareTraceAIndirectPipeline_;
  ::rendering::ComputePipeline streamPrepareTraceBIndirectPipeline_;
  ::rendering::ComputePipeline streamPrepareHashIndirectPipeline_;
  ::rendering::ComputePipeline streamTraceAtoBPipeline_;
  ::rendering::ComputePipeline streamTraceBtoAPipeline_;
  ::rendering::ComputePipeline streamFlattenHashToBPipeline_;
  ::rendering::ComputePipeline streamFlattenHashToAPipeline_;
  ::rendering::ComputePipeline streamFinalizePipeline_;
  ::rendering::Timer traceDispatchTimer_;
  uint32_t streamHashGeneration_ = 1u;
  bool traceDispatchTimerRequested_ = false;
};

} // namespace rendering
} // namespace softwarebvh
