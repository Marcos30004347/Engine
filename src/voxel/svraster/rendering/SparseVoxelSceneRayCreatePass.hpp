#pragma once

#include "os/File.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelSkyGuide.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

// Streams initial ReSTIR proposals into one immutable selected ray per
// G-buffer pixel/ray lane. Temporal and spatial passes then reuse those compact
// initial reservoirs without retaining the full proposal pool.
class SparseVoxelSceneRayCreatePass final : public ::rendering::Pass
{
public:
  enum class Kernel : uint32_t
  {
    Monolithic,
    Schedule,
    FinalizeSchedule,
    ParallelCandidates,
  };

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer rays;
    ::rendering::Buffer lights;
    ::rendering::Buffer lightBVHNodes;
    ::rendering::Buffer lightBVHInfo;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture lightFieldHistory;
    ::rendering::Buffer lightReachability;
    // Optional inputs for the legacy adaptive-demand mode. The simple ReSTIR
    // baseline leaves that mode disabled and follows only the fixed sparse
    // schedule; temporal acceptance is handled downstream with depth/normal.
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture motionVector;
    ::rendering::Texture lightFieldHistoryId;
    ::rendering::Buffer activeRayIndices;
    ::rendering::Buffer activeRayState;
    ::rendering::Buffer finalRays;
    ::rendering::Buffer finalReservoirs;
    ::rendering::Buffer reservoirRayIndices;
    ::rendering::Buffer reservoirRayState;
    // Previous frame's per-tile sky-direction CDF. Optional: an omitted guide
    // is replaced by a uniform distribution and strength zero is exact legacy
    // cosine sampling.
    ::rendering::Buffer skyGuideCDF;
    // Previous frame's luminance second moment and stable-frame count. This
    // compact screen-space map lets the cheap scheduler omit all proposal and
    // visibility work for converged, stationary receivers.
    ::rendering::Texture lightFieldHistoryMoments;
    // Reprojected hash of the at-most-eight lights that can affect the
    // receiver. A mismatch wakes only regions touched by a changed light.
    ::rendering::Texture lightFieldHistoryLightHash;
    ::rendering::Buffer visibleReceiverMap;
    // Four words per screen tile: valid-light mask, visible-light mask,
    // wake-until frame, and reserved. Sentinel rays update this after tracing;
    // the following frame's scheduler reads it before deciding to sleep.
    ::rendering::Buffer regionStability;
    // Two frame-parity statistics banks followed by one adaptive-budget record
    // per screen tile. Optional: without it the scheduler still runs, it just
    // admits every eligible receiver and records nothing.
    ::rendering::Buffer rayTileBudget;
    // Generated terrain publishes one stable region id per visible pixel and
    // an open-addressed region-to-page table. Together they let ray creation
    // use the receiver page's raster cell instead of one global terrain LOD.
    ::rendering::Buffer terrainOwnershipIds;
    ::rendering::Buffer terrainOwnershipTable;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU raster{};
    std::array<float, 3> lightDirection{-0.408248f, 0.816497f, 0.408248f};
    float maximumDistance = 64.0f;
    float originBias = 0.0f;
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t candidatesPerPixel = 1u;
    uint32_t lightCapacity = 1u;
    uint32_t generation = 0u;
    uint32_t sampleIndex = 0u;
    uint32_t sourceWidth = 1u;
    uint32_t sourceHeight = 1u;
    // Nonzero makes every emitted candidate a fresh cosine-sampled sky ray.
    // Retained for focused tests and ABI compatibility. Production uses zero;
    // when raysPerPixel > 1 the shader instead reserves exactly one sky lane
    // and leaves the remaining candidates to non-sky ReSTIR selection.
    uint32_t skyMode = 0u;
    // Size of one shadow-volume trace cell. Ray origins remain on the receiver;
    // traversal uses this scale to reject the receiver cell explicitly.
    float traceCellSize = 0.0f;
    // Sample count at which a pixel counts as converged for ray budgeting.
    float convergedSamples = 32.0f;
    // Average number of frames between refresh rays for a converged pixel.
    // Zero sleeps until invalidation, one traces the normal sparse schedule,
    // and larger values perform a low-rate safety refresh.
    float refreshInterval = 1.0f;
    // Retained for ABI compatibility with older callers. Full-resolution ray
    // slots no longer move between receiver pixels.
    uint32_t rotationPeriod = 1u;
    // Side length of the sparse sampling window. One procedurally scheduled
    // receiver is traced in each NxN window (1 is the full-rate reference).
    // The ray domain itself is always full raster resolution.
    uint32_t samplingDivisor = 1u;
    uint32_t reachabilityEntryCount =
        kSparseVoxelLightReachabilityEntryCount;
    float reachabilityRegionSize = 1.0f;
    float reachabilityExplorationFloor = 1.0f / 64.0f;
    // Number of pre-resampled ray lanes emitted per pixel. Initial candidates
    // are streamed into these lanes instead of being retained individually.
    uint32_t raysPerPixel = 1u;
    float skyGuidingStrength = 0.0f;
    bool visibleVoxelTracing = false;
    uint32_t visibleVoxelTileWidth = 2u;
    uint32_t visibleReceiverMapCapacity = 0u;
    uint32_t minimumConvergedSamples = 64u;
    float convergenceRelativeError = 0.20f;
    uint32_t stableObservationCount = 8u;
    // Receivers beyond this camera distance are lit analytically: their rays
    // keep their direction and light selection but carry no span, so nothing
    // traces them and the merge reports the miss that means unshadowed sky and
    // sun. Zero traces everything.
    float analyticLightingDistance = 0.0f;
    uint32_t regionTileSize = kSparseVoxelRegionStabilityDefaultTileSize;
    // Adaptive tile budget. Zero keeps the legacy schedule, admitting every
    // eligible receiver while still producing the tile records and statistics
    // so the budget a scene would use is measurable before it is enforced.
    // Otherwise this is the fraction of a tile's eligible receivers that a tile
    // of average demand may trace.
    float tileBudgetFraction = 0.0f;
    // How far above its equal share a tile with above-average demand may be
    // boosted. The frame total stays bounded because the boost is paid for by
    // tiles below the average.
    float tileBudgetBoost = 4.0f;
    // Slots each covered tile keeps for sentinel validation even when its
    // demand quota is zero, so a settled region never stops checking itself.
    uint32_t tileBudgetHeartbeat = 1u;
    // Finest generated-terrain raster cell. Zero disables the per-page lookup
    // and retains the ordinary global trace-cell behavior.
    float terrainFinestCellSize = 0.0f;
    // Optional legacy policy. False is the fixed-rate ReSTIR baseline.
    bool adaptiveDemand = false;
    // Optional viewport border traced every frame. Motion vectors expand it to
    // pixels whose history falls outside the previous viewport. Zero disables
    // both overrides and keeps the sparse-window budget exact.
    uint32_t fullRateEdgeWidth = 0u;
  };

  SparseVoxelSceneRayCreatePass(
      const Buffers buffers, Settings settings,
      const Kernel kernel = Kernel::Monolithic)
      : buffers_(buffers), settings_(settings), kernel_(kernel)
  {
    demandHistoryAvailable_ =
        buffers.instanceId.isValid() && buffers.voxelId.isValid() &&
        buffers.motionVector.isValid() &&
        buffers.lightFieldHistoryId.isValid() &&
        buffers.lightFieldHistoryMoments.isValid() &&
        buffers.lightFieldHistoryLightHash.isValid();
    reprojectDemand_ = settings_.adaptiveDemand && demandHistoryAvailable_;
    regionStabilityAvailable_ = buffers.regionStability.isValid();
    rayTileBudgetAvailable_ = buffers.rayTileBudget.isValid();
    settings_.width = std::max(1u, settings_.width);
    settings_.height = std::max(1u, settings_.height);
    settings_.candidatesPerPixel =
        std::clamp(settings_.candidatesPerPixel, 1u, 16u);
    settings_.raysPerPixel = std::clamp(settings_.raysPerPixel, 1u, 8u);
    settings_.candidatesPerPixel =
        std::max(settings_.candidatesPerPixel, settings_.raysPerPixel);
    settings_.lightCapacity = std::max(1u, settings_.lightCapacity);
    settings_.sourceWidth = std::max(1u, settings_.sourceWidth);
    settings_.sourceHeight = std::max(1u, settings_.sourceHeight);
    settings_.samplingDivisor =
        settings_.samplingDivisor == 2u ? 2u :
        settings_.samplingDivisor == 4u ? 4u :
        settings_.samplingDivisor == 8u ? 8u :
        settings_.samplingDivisor >= 16u ? 16u : 1u;
    settings_.reachabilityEntryCount =
        std::max(1u, settings_.reachabilityEntryCount);
    settings_.reachabilityRegionSize =
        std::max(1.0e-4f, settings_.reachabilityRegionSize);
    settings_.reachabilityExplorationFloor =
        std::clamp(settings_.reachabilityExplorationFloor, 0.01f, 1.0f);
    settings_.skyGuidingStrength =
        std::clamp(settings_.skyGuidingStrength, 0.0f, 0.95f);
    settings_.terrainFinestCellSize =
        std::max(settings_.terrainFinestCellSize, 0.0f);
    settings_.visibleVoxelTileWidth =
        std::clamp(settings_.visibleVoxelTileWidth, 1u, 2u);
    settings_.minimumConvergedSamples =
        std::clamp(settings_.minimumConvergedSamples, 4u, 4096u);
    settings_.convergenceRelativeError =
        std::clamp(settings_.convergenceRelativeError, 0.01f, 1.0f);
    settings_.stableObservationCount =
        std::clamp(settings_.stableObservationCount, 1u, 255u);
    settings_.regionTileSize =
        std::clamp(settings_.regionTileSize, 4u, 64u);
    settings_.tileBudgetFraction =
        std::clamp(settings_.tileBudgetFraction, 0.0f, 1.0f);
    settings_.tileBudgetBoost =
        std::clamp(settings_.tileBudgetBoost, 1.0f, 64.0f);
    settings_.tileBudgetHeartbeat =
        std::min(settings_.tileBudgetHeartbeat, 64u);
    settings_.fullRateEdgeWidth =
        std::min(settings_.fullRateEdgeWidth, 256u);
  }

  void update(
      const SparseVoxelRasterUniformsGPU &raster,
      const uint32_t generation,
      const uint32_t sampleIndex,
      const bool historyValid = true,
      const float skyGuidingStrength = -1.0f)
  {
    settings_.raster = raster;
    settings_.generation = generation;
    settings_.sampleIndex = sampleIndex;
    historyValid_ = historyValid;
    if (skyGuidingStrength >= 0.0f)
      settings_.skyGuidingStrength =
          std::clamp(skyGuidingStrength, 0.0f, 0.95f);
    upload();
  }

  // Ray span update. originBias is retained in the ABI for old callers but is
  // deliberately forced to zero: light rays start at the exact receiver.
  void updateTrace(const float maximumDistance, const float originBias)
  {
    settings_.maximumDistance = std::max(0.0f, maximumDistance);
    (void)originBias;
    settings_.originBias = 0.0f;
    upload();
  }

  void updateBudget(
      const float traceCellSize, const float convergedSamples,
      const float refreshInterval, const uint32_t rotationPeriod)
  {
    settings_.traceCellSize = std::max(0.0f, traceCellSize);
    settings_.convergedSamples = std::max(1.0f, convergedSamples);
    settings_.refreshInterval = std::max(0.0f, refreshInterval);
    settings_.rotationPeriod = std::max(1u, rotationPeriod);
    upload();
  }

  // Changes only the receiver election rate; the ray buffers always retain
  // one canonical slot per raster pixel. This is intentionally live so
  // diagnostics can compare 1x/2x/4x/8x/16x sparse windows without a graph
  // rebuild.
  void updateSamplingDivisor(const uint32_t divisor)
  {
    settings_.samplingDivisor =
        divisor == 2u ? 2u :
        divisor == 4u ? 4u :
        divisor == 8u ? 8u :
        divisor >= 16u ? 16u : 1u;
    upload();
  }

  void updateAdaptiveDemand(const bool enabled)
  {
    settings_.adaptiveDemand = enabled;
    reprojectDemand_ = enabled && demandHistoryAvailable_;
    upload();
  }

  void updateFullRateEdgeWidth(const uint32_t width)
  {
    settings_.fullRateEdgeWidth = std::min(width, 256u);
    upload();
  }

  void updateReachability(
      const float regionSize,
      const float explorationFloor = 1.0f / 64.0f)
  {
    settings_.reachabilityRegionSize = std::max(1.0e-4f, regionSize);
    settings_.reachabilityExplorationFloor =
        std::clamp(explorationFloor, 0.01f, 1.0f);
    upload();
  }

  void updateSkyGuiding(const float strength)
  {
    settings_.skyGuidingStrength = std::clamp(strength, 0.0f, 0.95f);
    upload();
  }

  void updateVisibleVoxelTracing(
      const bool enabled, const uint32_t tileWidth)
  {
    settings_.visibleVoxelTracing = enabled;
    // N=4 means four pixels, i.e. a 2x2 receiver tile. Wider tiles need a
    // correspondingly wider reconstruction footprint and would otherwise
    // leave receivers outside the current 3x3 light-field gather.
    settings_.visibleVoxelTileWidth = std::clamp(tileWidth, 1u, 2u);
    upload();
  }

  void updateStabilization(
      const uint32_t minimumConvergedSamples,
      const float convergenceRelativeError,
      const uint32_t stableObservationCount,
      const uint32_t regionTileSize,
      const float analyticLightingDistance = -1.0f)
  {
    if (analyticLightingDistance >= 0.0f)
      settings_.analyticLightingDistance = analyticLightingDistance;
    settings_.minimumConvergedSamples =
        std::clamp(minimumConvergedSamples, 4u, 4096u);
    settings_.convergenceRelativeError =
        std::clamp(convergenceRelativeError, 0.01f, 1.0f);
    settings_.stableObservationCount =
        std::clamp(stableObservationCount, 1u, 255u);
    settings_.regionTileSize = std::clamp(regionTileSize, 4u, 64u);
    upload();
  }

  void updateTileBudget(
      const float fraction, const float boost, const uint32_t heartbeat)
  {
    settings_.tileBudgetFraction = std::clamp(fraction, 0.0f, 1.0f);
    settings_.tileBudgetBoost = std::clamp(boost, 1.0f, 64.0f);
    settings_.tileBudgetHeartbeat = std::min(heartbeat, 64u);
    upload();
  }

  ~SparseVoxelSceneRayCreatePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (settingsBuffer_.isValid()) renderGraph->deleteBuffer(settingsBuffer_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
    if (ownsLightReachability_ && buffers_.lightReachability.isValid())
      renderGraph->deleteBuffer(buffers_.lightReachability);
    for (const ::rendering::Texture texture : ownedFallbackTextures_)
      if (texture.isValid())
        renderGraph->deleteTexture(texture);
    for (const ::rendering::Buffer buffer : ownedFallbackBuffers_)
      if (buffer.isValid())
        renderGraph->deleteBuffer(buffer);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    upload();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::ParallelCandidates)
      commandBuffer.cmdDispatchIndirect(buffers_.reservoirRayState, 0u);
    else if (kernel_ == Kernel::Schedule)
      // One workgroup per screen tile: the scheduler reduces the tile's demand
      // and elects its receivers inside a single group, so the budget needs no
      // separate reduction pass and no global sort.
      commandBuffer.cmdDispatch(
          sparseVoxelRegionStabilityTileCount(
              std::max(settings_.sourceWidth, settings_.width),
              std::max(settings_.sourceHeight, settings_.height),
              settings_.regionTileSize),
          1u, 1u);
    else if (kernel_ == Kernel::FinalizeSchedule)
      commandBuffer.cmdDispatch(1u, 1u, 1u);
    else
      commandBuffer.cmdDispatch(
          (settings_.width + 7u) / 8u,
          (settings_.height + 7u) / 8u,
          1u);
  }

private:
  struct alignas(16) CreateSettingsGPU
  {
    std::array<float, 4> lightDirectionMaximumDistance{};
    std::array<uint32_t, 4> dimensionsGeneration{};
    std::array<float, 4> originBiasReserved{};
    std::array<uint32_t, 4> sampling{};
    std::array<uint32_t, 4> rotation{};
    std::array<float, 4> reachability{};
    std::array<uint32_t, 4> reachabilityGeneration{};
    std::array<uint32_t, 4> visibleReceiverMap{};
    std::array<float, 4> stabilization{};
    std::array<float, 4> tileBudget{};
    std::array<uint32_t, 4> tileGrid{};
  };
  static_assert(sizeof(CreateSettingsGPU) == 176u);

  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = std::max<uint64_t>(size, sizeof(uint32_t)), .access = access};
  }

  static ::rendering::TextureView sampledView(const ::rendering::Texture texture)
  {
    return {
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

  void upload()
  {
    if (!rasterBuffer_.isValid()) return;
    uint32_t visibleReceiverGeneration = settings_.sampleIndex + 1u;
    if (visibleReceiverGeneration == 0u ||
        visibleReceiverGeneration == 0xfffffffeu)
      visibleReceiverGeneration = 1u;
    const CreateSettingsGPU settings{
        {settings_.lightDirection[0], settings_.lightDirection[1], settings_.lightDirection[2], settings_.maximumDistance},
        {settings_.width, settings_.height, settings_.candidatesPerPixel, settings_.generation},
        {settings_.originBias, settings_.traceCellSize, settings_.convergedSamples, settings_.refreshInterval},
        {settings_.sampleIndex,
         // sampling.y is a mode bitfield shared with the shader.
         static_cast<uint32_t>(
             (settings_.skyMode != 0u ? 1u : 0u) |
             (reprojectDemand_ ? 2u : 0u) |
             (historyValid_ ? 4u : 0u)),
         settings_.sourceWidth,
         settings_.sourceHeight},
        {settings_.rotationPeriod, settings_.samplingDivisor,
         settings_.reachabilityEntryCount, settings_.raysPerPixel},
        {settings_.reachabilityRegionSize,
         settings_.reachabilityExplorationFloor,
         settings_.skyGuidingStrength, settings_.terrainFinestCellSize},
        {settings_.generation, settings_.lightCapacity,
         settings_.visibleVoxelTracing ? 1u : 0u,
         settings_.visibleVoxelTileWidth},
        {settings_.visibleReceiverMapCapacity, 14u,
         visibleReceiverGeneration,
         regionStabilityAvailable_ ? settings_.regionTileSize : 0u},
        {static_cast<float>(settings_.minimumConvergedSamples),
         settings_.convergenceRelativeError,
         static_cast<float>(settings_.stableObservationCount),
         settings_.analyticLightingDistance},
        {settings_.tileBudgetFraction, settings_.tileBudgetBoost,
         static_cast<float>(settings_.tileBudgetHeartbeat),
         rayTileBudgetAvailable_ ? 1.0f : 0.0f},
        {settings_.regionTileSize,
         sparseVoxelRegionStabilityTileCount(
             settings_.sourceWidth, settings_.sourceHeight,
             settings_.regionTileSize),
         settings_.fullRateEdgeWidth, 0u}};
    renderGraph->bufferWrite(rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
    renderGraph->bufferWrite(settingsBuffer_, 0u, sizeof(settings), const_cast<CreateSettingsGPU *>(&settings));
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    if (!buffers_.lightReachability.isValid())
    {
      buffers_.lightReachability = renderGraph->createBuffer({
        .name = passName + "_EmptyLightReachability",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const uint32_t zero = 0u;
      renderGraph->bufferWrite(
          buffers_.lightReachability, 0u, sizeof(zero),
          const_cast<uint32_t *>(&zero));
      ownsLightReachability_ = true;
    }
    const auto fallbackTexture =
        [&](const char *suffix, const ::rendering::Format format)
        {
          const ::rendering::Texture texture = renderGraph->createTexture({
            .name = passName + suffix,
            .format = format,
            .width = 1u,
            .height = 1u,
            .mipLevels = 1u,
            .usage = ::rendering::ImageUsage::ImageUsage_Storage |
                     ::rendering::ImageUsage::ImageUsage_Sampled,
          });
          ownedFallbackTextures_.push_back(texture);
          return texture;
        };
    if (!buffers_.instanceId.isValid())
      buffers_.instanceId = fallbackTexture(
          "_FallbackInstanceId", ::rendering::Format::Format_R32Uint);
    if (!buffers_.voxelId.isValid())
      buffers_.voxelId = fallbackTexture(
          "_FallbackVoxelId", ::rendering::Format::Format_R32Uint);
    if (!buffers_.motionVector.isValid())
      buffers_.motionVector = fallbackTexture(
          "_FallbackMotionVector", ::rendering::Format::Format_RG16Float);
    if (!buffers_.lightFieldHistory.isValid())
      buffers_.lightFieldHistory = fallbackTexture(
          "_FallbackLightingHistory", ::rendering::Format::Format_RGBA16Float);
    if (!buffers_.lightFieldHistoryId.isValid())
      buffers_.lightFieldHistoryId = fallbackTexture(
          "_FallbackLightFieldId", ::rendering::Format::Format_RG32Uint);
    if (!buffers_.lightFieldHistoryMoments.isValid())
      buffers_.lightFieldHistoryMoments = fallbackTexture(
          "_FallbackLightFieldMoments", ::rendering::Format::Format_RG16Float);
    if (!buffers_.lightFieldHistoryLightHash.isValid())
      buffers_.lightFieldHistoryLightHash = fallbackTexture(
          "_FallbackLightFieldLightHash", ::rendering::Format::Format_R32Uint);
    const uint64_t pixelCount = uint64_t{settings_.width} * settings_.height;
    const uint64_t sourcePixelCount =
        uint64_t{settings_.sourceWidth} * settings_.sourceHeight;
    const uint64_t rayCount = pixelCount * settings_.raysPerPixel;
    const auto fallbackBuffer =
        [&](const std::string &suffix, const uint64_t size)
        {
          const ::rendering::Buffer buffer = renderGraph->createBuffer({
            .name = passName + suffix,
            .size = std::max<uint64_t>(size, sizeof(uint32_t)),
            .usage = ::rendering::BufferUsage::BufferUsage_Storage,
            .frameLocal = true,
          });
          ownedFallbackBuffers_.push_back(buffer);
          return buffer;
        };
    if (!buffers_.activeRayIndices.isValid())
      buffers_.activeRayIndices = fallbackBuffer(
          "_FallbackActiveRayIndices", rayCount * sizeof(uint32_t));
    if (!buffers_.activeRayState.isValid())
      buffers_.activeRayState =
          fallbackBuffer("_FallbackActiveRayState", 16u);
    if (!buffers_.finalRays.isValid())
      buffers_.finalRays = buffers_.rays;
    if (!buffers_.finalReservoirs.isValid())
      buffers_.finalReservoirs = fallbackBuffer(
          "_FallbackFinalReservoirs",
          pixelCount * sizeof(SparseVoxelDIReservoirGPU));
    if (!buffers_.reservoirRayIndices.isValid())
      buffers_.reservoirRayIndices = fallbackBuffer(
          "_FallbackReservoirRayIndices",
          pixelCount * sizeof(uint32_t));
    if (!buffers_.reservoirRayState.isValid())
      buffers_.reservoirRayState =
          fallbackBuffer("_FallbackReservoirRayState", 16u);
    if (!buffers_.skyGuideCDF.isValid())
    {
      const uint32_t guideEntries =
          sparseVoxelSkyGuideTileCount(
              settings_.sourceWidth, settings_.sourceHeight) *
          kSparseVoxelSkyGuideBinCount;
      buffers_.skyGuideCDF = fallbackBuffer(
          "_FallbackSkyGuideCDF",
          uint64_t{guideEntries} * sizeof(float));
      std::vector<float> uniformCDF(guideEntries);
      for (uint32_t entry = 0u; entry < guideEntries; ++entry)
        uniformCDF[entry] = static_cast<float>(
            entry % kSparseVoxelSkyGuideBinCount + 1u) /
            static_cast<float>(kSparseVoxelSkyGuideBinCount);
      renderGraph->bufferWrite(
          buffers_.skyGuideCDF, 0u,
          uint64_t{uniformCDF.size()} * sizeof(float), uniformCDF.data());
    }
    if (!buffers_.visibleReceiverMap.isValid())
    {
      buffers_.visibleReceiverMap = fallbackBuffer(
          "_FallbackVisibleReceiverMap", sizeof(uint32_t));
      settings_.visibleReceiverMapCapacity = 0u;
    }
    if (!buffers_.regionStability.isValid())
      buffers_.regionStability = fallbackBuffer(
          "_FallbackRegionStability",
          kSparseVoxelRegionStabilityWordsPerTile * sizeof(uint32_t));
    if (!buffers_.rayTileBudget.isValid())
      buffers_.rayTileBudget = fallbackBuffer(
          "_FallbackRayTileBudget",
          uint64_t{kSparseVoxelRayBudgetHeaderWords} * sizeof(uint32_t));
    if (!buffers_.terrainOwnershipIds.isValid())
      buffers_.terrainOwnershipIds = fallbackBuffer(
          "_FallbackTerrainOwnershipIds",
          sourcePixelCount * sizeof(uint32_t));
    if (!buffers_.terrainOwnershipTable.isValid())
      buffers_.terrainOwnershipTable = fallbackBuffer(
          "_FallbackTerrainOwnershipTable",
          sizeof(SparseVoxelTerrainOwnershipGPU));
    rasterBuffer_ = renderGraph->createBuffer({.name = passName + "_Raster", .size = sizeof(settings_.raster), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    settingsBuffer_ = renderGraph->createBuffer({.name = passName + "_Settings", .size = sizeof(CreateSettingsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {::rendering::BindingGroupLayout{.buffers = {
        {.name = "raster", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "settings", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "coverage", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "linearDepth", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "rays", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lights", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightBVHNodes", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightBVHInfo", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightReachability", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "activeRayIndices", .binding = 15u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "activeRayState", .binding = 16u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "finalRays", .binding = 17u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "finalReservoirs", .binding = 18u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "reservoirRayIndices", .binding = 19u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "reservoirRayState", .binding = 20u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "skyGuideCDF", .binding = 21u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "visibleReceiverMap", .binding = 24u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "regionStability", .binding = 25u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "rayTileBudget", .binding = 26u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "terrainOwnershipIds", .binding = 27u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "terrainOwnershipTable", .binding = 28u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
    }, .textures = {
        {.name = "geometryNormal", .binding = 8u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightFieldHistory", .binding = 9u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "instanceId", .binding = 11u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "voxelId", .binding = 12u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "motionVector", .binding = 13u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightFieldHistoryId", .binding = 14u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightFieldHistoryMoments", .binding = 22u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lightFieldHistoryLightHash", .binding = 23u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
    }}}});
    groups_ = renderGraph->createBindingGroups({.layout = layout_, .name = passName + "_Groups", .groups = {::rendering::GroupInfo{.name = "group0", .buffers = {
        {.binding = 0u, .bufferView = view(rasterBuffer_, sizeof(settings_.raster), ::rendering::AccessPattern::UNIFORM_READ)},
        {.binding = 1u, .bufferView = view(settingsBuffer_, sizeof(CreateSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
        {.binding = 2u, .bufferView = view(buffers_.coverage, sourcePixelCount * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 3u, .bufferView = view(buffers_.linearDepth, sourcePixelCount * sizeof(float), ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 4u, .bufferView = view(buffers_.rays, rayCount * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 5u, .bufferView = view(buffers_.lights, globalillumination::worldgi::worldGILightBufferSize(settings_.lightCapacity), ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 6u, .bufferView = view(buffers_.lightBVHNodes, globalillumination::worldgi::worldGILightBVHNodeBufferSize(settings_.lightCapacity), ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 7u, .bufferView = view(buffers_.lightBVHInfo, globalillumination::worldgi::worldGILightBVHInfoBufferSize(), ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 10u,
         .bufferView = view(
             buffers_.lightReachability,
             ownsLightReachability_
                 ? sizeof(uint32_t)
                 : sparseVoxelLightReachabilityBufferSize(
                       settings_.lightCapacity,
                       settings_.reachabilityEntryCount),
             ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 15u, .bufferView = view(
             buffers_.activeRayIndices, rayCount * sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 16u, .bufferView = view(
             buffers_.activeRayState, 16u,
             ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 17u, .bufferView = view(
             buffers_.finalRays,
             rayCount * sizeof(SparseVoxelSceneRayGPU),
             ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 18u, .bufferView = view(
             buffers_.finalReservoirs,
             pixelCount * sizeof(SparseVoxelDIReservoirGPU),
             ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 19u, .bufferView = view(
             buffers_.reservoirRayIndices, pixelCount * sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 20u, .bufferView = view(
             buffers_.reservoirRayState, 16u,
             ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 21u, .bufferView = view(
             buffers_.skyGuideCDF,
             sparseVoxelSkyGuideCDFBufferSize(
                 settings_.sourceWidth, settings_.sourceHeight),
             ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 24u, .bufferView = view(
             buffers_.visibleReceiverMap,
             uint64_t{std::max(settings_.visibleReceiverMapCapacity, 1u)} *
                 14u * sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 25u, .bufferView = view(
             buffers_.regionStability,
             regionStabilityAvailable_
                 ? sparseVoxelRegionStabilityBufferSize(
                       settings_.sourceWidth, settings_.sourceHeight,
                       settings_.regionTileSize)
                 : uint64_t{kSparseVoxelRegionStabilityWordsPerTile} *
                       sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 26u, .bufferView = view(
             buffers_.rayTileBudget,
             rayTileBudgetAvailable_
                 ? sparseVoxelRayBudgetBufferSize(
                       settings_.sourceWidth, settings_.sourceHeight,
                       settings_.regionTileSize)
                 : uint64_t{kSparseVoxelRayBudgetHeaderWords} *
                       sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE)},
        {.binding = 27u, .bufferView = view(
             buffers_.terrainOwnershipIds,
             sourcePixelCount * sizeof(uint32_t),
             ::rendering::AccessPattern::SHADER_READ)},
        {.binding = 28u, .bufferView = view(
             buffers_.terrainOwnershipTable,
             renderGraph->getBufferSize(buffers_.terrainOwnershipTable),
             ::rendering::AccessPattern::SHADER_READ)},
    }, .textures = {
        {.binding = 8u, .textureView = sampledView(buffers_.geometryNormal)},
        {.binding = 9u, .textureView = sampledView(buffers_.lightFieldHistory)},
        {.binding = 11u, .textureView = sampledView(buffers_.instanceId)},
        {.binding = 12u, .textureView = sampledView(buffers_.voxelId)},
        {.binding = 13u, .textureView = sampledView(buffers_.motionVector)},
        {.binding = 14u, .textureView = sampledView(buffers_.lightFieldHistoryId)},
        {.binding = 22u, .textureView = sampledView(buffers_.lightFieldHistoryMoments)},
        {.binding = 23u, .textureView = sampledView(buffers_.lightFieldHistoryLightHash)},
    }}}});
    shader_ = renderGraph->createShader({.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-scene-ray-create.spirv"), .type = ::rendering::ShaderType::SpirV});
    const char *entry =
        kernel_ == Kernel::Schedule
            ? "scheduleSparseVoxelSceneRayPixels"
        : kernel_ == Kernel::FinalizeSchedule
            ? "finalizeSparseVoxelSceneRayPixels"
        : kernel_ == Kernel::ParallelCandidates
            ? "createSparseVoxelSceneRaysParallel"
            : "createSparseVoxelSceneRays";
    pipeline_ = renderGraph->createComputePipeline({.entry = entry, .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Buffers buffers_{};
  Settings settings_{};
  Kernel kernel_ = Kernel::Monolithic;
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer settingsBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool ownsLightReachability_ = false;
  bool regionStabilityAvailable_ = false;
  bool rayTileBudgetAvailable_ = false;
  bool demandHistoryAvailable_ = false;
  bool reprojectDemand_ = false;
  bool historyValid_ = false;
  std::vector<::rendering::Texture> ownedFallbackTextures_{};
  std::vector<::rendering::Buffer> ownedFallbackBuffers_{};
};

} // namespace voxel::svraster::rendering
