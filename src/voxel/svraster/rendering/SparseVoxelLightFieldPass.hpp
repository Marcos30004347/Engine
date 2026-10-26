#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelSkyGuide.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

// Resolves the reduced-resolution ray results into a full-resolution lighting
// field with its own per-pixel temporal accumulation.
//
// Accumulating in ray-slot space capped the achievable detail at the ray grid:
// the shading pass could filter it however it liked, but the signal it filtered
// had already been stored and reprojected at the lower resolution. Moving the
// accumulator here makes the field full resolution at any ray count, so ray
// count is purely a quality/cost control.
class SparseVoxelLightFieldPass final : public ::rendering::Pass
{
public:
  static constexpr uint32_t ReconstructionStatisticCount = 28u;

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer sceneRays;
    ::rendering::Buffer sceneDIReservoirs;
    ::rendering::Buffer sceneLights;
    ::rendering::Buffer currentHits;
    ::rendering::Buffer lightReachability;
    ::rendering::Buffer skyGuideFeedback;
    ::rendering::Buffer regionStability;
    // Adaptive tile-budget records. This pass only accumulates the resolved-ray
    // and visibility-change counters the scheduler cannot observe itself.
    ::rendering::Buffer rayTileBudget;
    // Optional reconstruction, history-loss, and localized-pop diagnostics.
    // Omitted leaves the shader's counters disabled; nothing else depends on
    // them.
    ::rendering::Buffer reconstructionStatistics;
    // Nonzero for generated-terrain pixels. Physical page instances may change
    // across an equivalent cut, so those pixels use depth-validated history.
    ::rendering::Buffer terrainOwnershipIds;
  };

  struct Textures
  {
    ::rendering::Texture geometryNormal;
    ::rendering::Texture shadingNormal;
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture motionVector;
    ::rendering::Texture historyField;
    ::rendering::Texture historyId;
    ::rendering::Texture outputField;
    ::rendering::Texture outputId;
    ::rendering::Texture reactiveOutput;
    ::rendering::Texture historyMoments;
    ::rendering::Texture outputMoments;
    ::rendering::Texture historyLightHash;
    ::rendering::Texture outputLightHash;
    ::rendering::Texture historyViewDepth;
    ::rendering::Texture outputViewDepth;
    // Octahedral geometry normal, previous and current frame. History
    // validation compares orientation continuously; the three-bit face class it
    // replaced was a hard reject on a quantized normal.
    ::rendering::Texture historyNormal;
    ::rendering::Texture outputNormal;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t rayWidth = 1u;
    uint32_t rayHeight = 1u;
    bool historyValid = false;
    uint32_t maximumSamples = 32u;
    uint32_t frameIndex = 0u;
    uint32_t lightCapacity = 1u;
    float normalThreshold = 0.85f;
    float depthThreshold = 0.05f;
    float spatialFillRadius = 2.0f;
    uint32_t generation = 0u;
    uint32_t reachabilityEntryCount =
        kSparseVoxelLightReachabilityEntryCount;
    float reachabilityRegionSize = 1.0f;
    uint32_t raysPerPixel = 1u;
    bool independentRays = false;
    // A surface identity can remain valid while camera motion changes its
    // visible lighting. Once motion and a bright-to-dark change both exceed
    // these thresholds, shorten and clamp history before SVGF sees it.
    float historyMotionThresholdPixels = 0.5f;
    float historyChangeThreshold = 0.25f;
    float historyChangeMaximumSamples = 8.0f;
    float historyClampStrength = 0.9f;
    uint32_t regionTileSize = kSparseVoxelRegionStabilityDefaultTileSize;
    uint32_t regionWakeFrames = 4u;
    SparseVoxelRasterUniformsGPU raster{};
    // Soft area/sky visibility is a distribution, not a deterministic bit.
    // Compare 16-observation windows, require repeated deviations, and use a
    // sparse subset of active rays to establish the baseline before sleeping.
    float regionSoftVisibilityThreshold = 0.375f;
    uint32_t regionSoftVisibilityStreak = 2u;
    uint32_t regionSoftPrefillStride = 16u;
    // Legacy post-trace reconstruction controls retained for settings/uniform
    // ABI compatibility. They are intentionally inert: visibility belongs to
    // the receiver that traced it, while spatial ReSTIR reuse occurs upstream
    // and reconnects the proposal before visibility is evaluated.
    float spatialReuseDirectWeight = 1.0f;
    float spatialReuseNeighbourWeight = 0.0f;
    float spatialReuseUntilSamples = 0.0f;
    // Fraction of unshadowed sky and sun published for a pixel with no
    // measurement or valid history. A dim guess is
    // the lowest-regret one: wrong in shadow it is barely visible, wrong in
    // light it reads as unconverged rather than as a hole.
    float uninformedAmbientFraction = 0.25f;
    // Floor of the variance-driven sample window. The configured maximum is
    // kept for converged receivers; noisy ones collapse toward this.
    float adaptiveMinimumSamples = 16.0f;
    // One-sided firefly clamp in standard deviations of the receiver's own
    // history. Zero disables it.
    float fireflyClampSigma = 0.0f;
    // Legacy reconstruction stability credit, retained for ABI compatibility.
    float stabilityReconstructedCredit = 0.5f;
    // Stability lost per observation that fails the error test. A hard reset
    // turns the streak into a random-restart process that a binary visibility
    // signal may never complete.
    float stabilityDecayPerMiss = 1.0f;
    // Relative previous-view depth tolerance and normal cosine threshold.
    // These are the complete binary temporal surface-validity rule; identity,
    // face class, page ownership, and local-light hashes do not participate.
    float terrainHistoryDepthThreshold = 0.02f;
    float historyNormalThreshold = 0.8f;
    // Diagnostic marker for frames affected by terrain publication. It does
    // not relax surface validation; incompatible depth or normal must start a
    // new lighting record.
    bool terrainCommitted = false;
    // Legacy receiver-sharing width retained for settings ABI compatibility.
    // The light field no longer gathers visibility across a tile.
    uint32_t receiverTileWidth = 1u;
    // The adaptive sky guide is an optional estimator. When disabled the
    // light-field pass skips its feedback atomics and no resolve pass is
    // registered by the renderer.
    bool skyGuideEnabled = false;
  };

  SparseVoxelLightFieldPass(Buffers buffers, Textures textures, Settings settings)
      : buffers_(buffers), textures_(textures), settings_(sanitize(settings))
  {
  }

  void update(Settings settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid())
      upload();
  }

  ~SparseVoxelLightFieldPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid())
      renderGraph->deleteBuffer(rasterBuffer_);
    for (const ::rendering::Texture texture : ownedFallbackTextures_)
      if (texture.isValid())
        renderGraph->deleteTexture(texture);
    if (ownsLightReachability_ && buffers_.lightReachability.isValid())
      renderGraph->deleteBuffer(buffers_.lightReachability);
    if (ownsSkyGuideFeedback_ && buffers_.skyGuideFeedback.isValid())
      renderGraph->deleteBuffer(buffers_.skyGuideFeedback);
    if (ownsRegionStability_ && buffers_.regionStability.isValid())
      renderGraph->deleteBuffer(buffers_.regionStability);
    if (ownsRayTileBudget_ && buffers_.rayTileBudget.isValid())
      renderGraph->deleteBuffer(buffers_.rayTileBudget);
    if (ownsReconstructionStatistics_ &&
        buffers_.reconstructionStatistics.isValid())
      renderGraph->deleteBuffer(buffers_.reconstructionStatistics);
    if (ownsTerrainOwnershipIds_ && buffers_.terrainOwnershipIds.isValid())
      renderGraph->deleteBuffer(buffers_.terrainOwnershipIds);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch((settings_.width + 7u) / 8u, (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t dimensions[4] = {1u, 1u, 1u, 1u};
    uint32_t controls[4] = {0u, 1u, 0u, 0u};
    float thresholds[4] = {0.85f, 0.05f, 2.0f, 0.0f};
    uint32_t reachability[4] = {0u, 0u, 0u, 0u};
    float antiLag[4] = {0.5f, 0.25f, 8.0f, 0.9f};
    uint32_t regionStability[4] = {0u, 0u, 0u, 0u};
    float regionSoftProbe[4] = {0.375f, 2.0f, 16.0f, 0.0f};
    float spatialReuse[4] = {1.0f, 1.0f, 64.0f, 16.0f};
    float spatialReuse2[4] = {2.5f, 0.0f, 0.0f, 0.0f};
    // x: terrain-publication diagnostic marker. y: visible-voxel receiver tile
    // width. zw reserved.
    uint32_t commit[4] = {0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 160u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.rayWidth = std::max(1u, settings.rayWidth);
    settings.rayHeight = std::max(1u, settings.rayHeight);
    settings.maximumSamples = std::clamp(settings.maximumSamples, 1u, 4096u);
    settings.lightCapacity = std::max(1u, settings.lightCapacity);
    settings.normalThreshold = std::clamp(settings.normalThreshold, -1.0f, 1.0f);
    settings.depthThreshold = std::max(0.0f, settings.depthThreshold);
    settings.spatialFillRadius = std::clamp(settings.spatialFillRadius, 1.0f, 8.0f);
    settings.historyMotionThresholdPixels =
        std::clamp(settings.historyMotionThresholdPixels, 0.0f, 64.0f);
    settings.historyChangeThreshold =
        std::clamp(settings.historyChangeThreshold, 0.0f, 0.99f);
    settings.historyChangeMaximumSamples =
        std::clamp(settings.historyChangeMaximumSamples, 1.0f, 64.0f);
    settings.historyClampStrength =
        std::clamp(settings.historyClampStrength, 0.0f, 1.0f);
    settings.reachabilityEntryCount =
        std::max(1u, settings.reachabilityEntryCount);
    settings.reachabilityRegionSize =
        std::max(1.0e-4f, settings.reachabilityRegionSize);
    settings.raysPerPixel =
        std::clamp(settings.raysPerPixel, 1u, 8u);
    settings.regionTileSize = std::clamp(settings.regionTileSize, 4u, 64u);
    settings.regionWakeFrames = std::clamp(settings.regionWakeFrames, 1u, 64u);
    settings.regionSoftVisibilityThreshold =
        std::clamp(settings.regionSoftVisibilityThreshold, 0.125f, 0.75f);
    settings.regionSoftVisibilityStreak =
        std::clamp(settings.regionSoftVisibilityStreak, 1u, 7u);
    settings.regionSoftPrefillStride =
        std::clamp(settings.regionSoftPrefillStride, 1u, 256u);
    settings.spatialReuseDirectWeight =
        std::clamp(settings.spatialReuseDirectWeight, 0.0f, 16.0f);
    settings.spatialReuseNeighbourWeight =
        std::clamp(settings.spatialReuseNeighbourWeight, 0.0f, 16.0f);
    settings.spatialReuseUntilSamples =
        std::clamp(settings.spatialReuseUntilSamples, 0.0f, 4096.0f);
    settings.adaptiveMinimumSamples = std::clamp(
        settings.adaptiveMinimumSamples, 1.0f,
        static_cast<float>(settings.maximumSamples));
    settings.fireflyClampSigma =
        std::clamp(settings.fireflyClampSigma, 0.0f, 64.0f);
    settings.stabilityReconstructedCredit =
        std::clamp(settings.stabilityReconstructedCredit, 0.0f, 1.0f);
    settings.stabilityDecayPerMiss =
        std::clamp(settings.stabilityDecayPerMiss, 0.0f, 255.0f);
    settings.terrainHistoryDepthThreshold =
        std::clamp(settings.terrainHistoryDepthThreshold, 1.0e-5f, 0.5f);
    settings.historyNormalThreshold =
        std::clamp(settings.historyNormalThreshold, -0.99f, 0.99f);
    return settings;
  }

  static ::rendering::BufferView view(
      ::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  static ::rendering::TextureView sampledView(::rendering::Texture texture)
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

  static ::rendering::TextureView storageView(::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
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
    Uniforms uniforms{};
    uniforms.dimensions[0] = settings_.width;
    uniforms.dimensions[1] = settings_.height;
    uniforms.dimensions[2] = settings_.rayWidth;
    uniforms.dimensions[3] = settings_.rayHeight;
    uniforms.controls[0] = settings_.historyValid ? 1u : 0u;
    uniforms.controls[1] = settings_.maximumSamples;
    uniforms.controls[2] = settings_.frameIndex;
    uniforms.controls[3] = settings_.lightCapacity;
    uniforms.thresholds[0] = settings_.normalThreshold;
    uniforms.thresholds[1] = settings_.depthThreshold;
    uniforms.thresholds[2] = settings_.spatialFillRadius;
    // thresholds.w is the history normal floor. It used to receive
    // reachabilityRegionSize, which the shader never read there - the
    // reachability stage takes its parameters from the `reachability` vector -
    // and the shader's own comment already called this slot unused.
    uniforms.thresholds[3] = settings_.historyNormalThreshold;
    uniforms.commit[1] = std::clamp(settings_.receiverTileWidth, 1u, 4u);
    uniforms.reachability[0] = settings_.generation;
    uniforms.reachability[1] = settings_.reachabilityEntryCount;
    uniforms.reachability[2] = settings_.raysPerPixel;
    uniforms.reachability[3] = settings_.independentRays ? 1u : 0u;
    uniforms.antiLag[0] = settings_.historyMotionThresholdPixels;
    uniforms.antiLag[1] = settings_.historyChangeThreshold;
    uniforms.antiLag[2] = settings_.historyChangeMaximumSamples;
    uniforms.antiLag[3] = settings_.historyClampStrength;
    uniforms.regionStability[0] = settings_.regionTileSize;
    uniforms.regionStability[1] = settings_.regionWakeFrames;
    uniforms.regionStability[2] =
        buffers_.regionStability.isValid() && !ownsRegionStability_ ? 1u : 0u;
    uniforms.regionStability[3] =
        buffers_.rayTileBudget.isValid() && !ownsRayTileBudget_ ? 1u : 0u;
    uniforms.regionSoftProbe[0] =
        settings_.regionSoftVisibilityThreshold;
    uniforms.regionSoftProbe[1] =
        static_cast<float>(settings_.regionSoftVisibilityStreak);
    uniforms.regionSoftProbe[2] =
        static_cast<float>(settings_.regionSoftPrefillStride);
    uniforms.regionSoftProbe[3] = settings_.terrainHistoryDepthThreshold;
    uniforms.spatialReuse[0] = settings_.spatialReuseDirectWeight;
    uniforms.spatialReuse[1] = settings_.spatialReuseNeighbourWeight;
    uniforms.spatialReuse[2] = settings_.spatialReuseUntilSamples;
    uniforms.spatialReuse[3] = settings_.adaptiveMinimumSamples;
    uniforms.spatialReuse2[0] = settings_.fireflyClampSigma;
    uniforms.spatialReuse2[1] = settings_.stabilityReconstructedCredit;
    uniforms.spatialReuse2[2] = settings_.stabilityDecayPerMiss;
    uniforms.commit[0] = settings_.terrainCommitted ? 1u : 0u;
    uniforms.commit[2] = settings_.skyGuideEnabled ? 1u : 0u;
    uniforms.spatialReuse2[3] =
        std::clamp(settings_.uninformedAmbientFraction, 0.0f, 1.0f);
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(uniforms), &uniforms);
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid())
      return;
    const auto fallbackMoments = [&](const char *suffix)
    {
      const ::rendering::Texture texture = renderGraph->createTexture({
        .name = passName + suffix,
        .format = ::rendering::Format::Format_RG16Float,
        .width = settings_.width,
        .height = settings_.height,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage |
                 ::rendering::ImageUsage::ImageUsage_Sampled,
      });
      ownedFallbackTextures_.push_back(texture);
      return texture;
    };
    if (!textures_.historyMoments.isValid())
      textures_.historyMoments = fallbackMoments("_FallbackHistoryMoments");
    if (!textures_.outputMoments.isValid())
      textures_.outputMoments = fallbackMoments("_FallbackOutputMoments");
    const auto fallbackLightHash = [&](const char *suffix)
    {
      const ::rendering::Texture texture = renderGraph->createTexture({
        .name = passName + suffix,
        .format = ::rendering::Format::Format_R32Uint,
        .width = settings_.width,
        .height = settings_.height,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage |
                 ::rendering::ImageUsage::ImageUsage_Sampled,
      });
      ownedFallbackTextures_.push_back(texture);
      return texture;
    };
    if (!textures_.historyLightHash.isValid())
      textures_.historyLightHash =
          fallbackLightHash("_FallbackHistoryLightHash");
    if (!textures_.outputLightHash.isValid())
      textures_.outputLightHash =
          fallbackLightHash("_FallbackOutputLightHash");
    const auto fallbackViewDepth = [&](const char *suffix)
    {
      const ::rendering::Texture texture = renderGraph->createTexture({
        .name = passName + suffix,
        .format = ::rendering::Format::Format_R32Float,
        .width = settings_.width,
        .height = settings_.height,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage |
                 ::rendering::ImageUsage::ImageUsage_Sampled,
      });
      ownedFallbackTextures_.push_back(texture);
      return texture;
    };
    if (!textures_.historyViewDepth.isValid())
      textures_.historyViewDepth =
          fallbackViewDepth("_FallbackHistoryViewDepth");
    if (!textures_.outputViewDepth.isValid())
      textures_.outputViewDepth =
          fallbackViewDepth("_FallbackOutputViewDepth");
    const auto fallbackNormal = [&](const char *suffix)
    {
      const ::rendering::Texture texture = renderGraph->createTexture({
        .name = passName + suffix,
        .format = ::rendering::Format::Format_RGBA16Float,
        .width = settings_.width,
        .height = settings_.height,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage |
                 ::rendering::ImageUsage::ImageUsage_Sampled,
      });
      ownedFallbackTextures_.push_back(texture);
      return texture;
    };
    if (!textures_.historyNormal.isValid())
      textures_.historyNormal = fallbackNormal("_FallbackHistoryNormal");
    if (!textures_.outputNormal.isValid())
      textures_.outputNormal = fallbackNormal("_FallbackOutputNormal");
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
    if (!buffers_.skyGuideFeedback.isValid())
    {
      buffers_.skyGuideFeedback = renderGraph->createBuffer({
        .name = passName + "_EmptySkyGuideFeedback",
        .size = sparseVoxelSkyGuideFeedbackBufferSize(
            settings_.width, settings_.height),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const std::vector<uint32_t> zeros(
          sparseVoxelSkyGuideFeedbackBufferSize(
              settings_.width, settings_.height) /
              sizeof(uint32_t),
          0u);
      renderGraph->bufferWrite(
          buffers_.skyGuideFeedback, 0u,
          uint64_t{zeros.size()} * sizeof(uint32_t),
          const_cast<uint32_t *>(zeros.data()));
      ownsSkyGuideFeedback_ = true;
    }
    if (!buffers_.regionStability.isValid())
    {
      buffers_.regionStability = renderGraph->createBuffer({
        .name = passName + "_EmptyRegionStability",
        .size = uint64_t{kSparseVoxelRegionStabilityWordsPerTile} *
            sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const std::array<uint32_t, kSparseVoxelRegionStabilityWordsPerTile>
          zeros{};
      renderGraph->bufferWrite(
          buffers_.regionStability, 0u, sizeof(zeros),
          const_cast<uint32_t *>(zeros.data()));
      ownsRegionStability_ = true;
    }
    if (!buffers_.rayTileBudget.isValid())
    {
      buffers_.rayTileBudget = renderGraph->createBuffer({
        .name = passName + "_EmptyRayTileBudget",
        .size = uint64_t{kSparseVoxelRayBudgetHeaderWords} * sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const std::array<uint32_t, kSparseVoxelRayBudgetHeaderWords> zeros{};
      renderGraph->bufferWrite(
          buffers_.rayTileBudget, 0u, sizeof(zeros),
          const_cast<uint32_t *>(zeros.data()));
      ownsRayTileBudget_ = true;
    }
    // The counters are diagnostic and normally absent. The shader always
    // declares binding 29, so the layout must too, which means an absent buffer
    // has to become a real one rather than an empty view - otherwise the
    // shipping path (no --test-frame-signatures) fails to register the pass at
    // all. Two words is below the shader's length check, so the counters stay
    // disabled on this fallback instead of silently accumulating.
    if (!buffers_.reconstructionStatistics.isValid())
    {
      buffers_.reconstructionStatistics = renderGraph->createBuffer({
        .name = passName + "_EmptyReconstructionStatistics",
        .size = 2u * sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      std::array<uint32_t, 2u> zeros{};
      renderGraph->bufferWrite(
          buffers_.reconstructionStatistics, 0u, sizeof(zeros), zeros.data());
      ownsReconstructionStatistics_ = true;
    }
    if (!buffers_.terrainOwnershipIds.isValid())
    {
      buffers_.terrainOwnershipIds = renderGraph->createBuffer({
        .name = passName + "_EmptyTerrainOwnershipIds",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const uint32_t zero = 0u;
      renderGraph->bufferWrite(
          buffers_.terrainOwnershipIds, 0u, sizeof(zero),
          const_cast<uint32_t *>(&zero));
      ownsTerrainOwnershipIds_ = true;
    }
    const uint64_t pixels = uint64_t{settings_.width} * settings_.height;
    const uint64_t rays =
        uint64_t{settings_.rayWidth} * settings_.rayHeight *
        settings_.raysPerPixel;
    const uint64_t reservoirPixels =
        uint64_t{settings_.rayWidth} * settings_.rayHeight;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });

    const auto storageBuffer = [](const char *name, uint32_t binding)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      };
    };
    const auto sampledTexture = [](const char *name, uint32_t binding)
    {
      return ::rendering::BindingGroupLayoutTextureEntry{
        .name = name,
        .binding = binding,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      };
    };

    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "settings",
               .binding = 0u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "raster",
               .binding = 24u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              storageBuffer("coverage", 1u),
              storageBuffer("linearDepth", 2u),
              storageBuffer("sceneRays", 8u),
              storageBuffer("sceneDIReservoirs", 9u),
              storageBuffer("sceneLights", 13u),
              storageBuffer("currentHits", 18u),
              storageBuffer("lightReachability", 20u),
              storageBuffer("skyGuideFeedback", 21u),
              storageBuffer("regionStability", 27u),
              storageBuffer("rayTileBudget", 28u),
              storageBuffer("reconstructionStatistics", 29u),
              storageBuffer("terrainOwnershipIds", 32u),
            },
        .textures =
            {
              sampledTexture("geometryNormal", 3u),
              sampledTexture("shadingNormal", 4u),
              sampledTexture("instanceId", 5u),
              sampledTexture("voxelId", 6u),
              sampledTexture("motionVector", 7u),
              sampledTexture("historyField", 14u),
              sampledTexture("historyId", 15u),
              sampledTexture("historyMoments", 22u),
              sampledTexture("historyLightHash", 25u),
              sampledTexture("historyViewDepth", 30u),
              sampledTexture("historyNormal", 33u),
            },
        .storageTextures =
            {
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputField",
                .binding = 16u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputId",
                .binding = 17u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "reactiveOutput",
                .binding = 19u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputMoments",
                .binding = 23u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputLightHash",
                .binding = 26u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputViewDepth",
                .binding = 31u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              ::rendering::BindingGroupLayoutStorageTextureEntry{
                .name = "outputNormal",
                .binding = 34u,
                .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });

    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(uniforms_, sizeof(Uniforms), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 24u, .bufferView = view(rasterBuffer_, sizeof(settings_.raster), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 1u, .bufferView = view(buffers_.coverage, pixels * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 2u, .bufferView = view(buffers_.linearDepth, pixels * sizeof(float), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 8u, .bufferView = view(buffers_.sceneRays, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 9u, .bufferView = view(buffers_.sceneDIReservoirs, reservoirPixels * sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 13u,
               .bufferView = view(
                   buffers_.sceneLights,
                   globalillumination::worldgi::worldGILightBufferSize(settings_.lightCapacity),
                   ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 18u, .bufferView = view(buffers_.currentHits, rays * sizeof(SparseVoxelSceneRayHitGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 20u,
               .bufferView = view(
                   buffers_.lightReachability,
                   ownsLightReachability_
                       ? sizeof(uint32_t)
                       : sparseVoxelLightReachabilityBufferSize(
                             settings_.lightCapacity,
                             settings_.reachabilityEntryCount),
                   ::rendering::AccessPattern::SHADER_READ |
                       ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 21u,
               .bufferView = view(
                   buffers_.skyGuideFeedback,
                   sparseVoxelSkyGuideFeedbackBufferSize(
                       settings_.width, settings_.height),
                   ::rendering::AccessPattern::SHADER_READ |
                       ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 27u,
               .bufferView = view(
                   buffers_.regionStability,
                   ownsRegionStability_
                       ? uint64_t{kSparseVoxelRegionStabilityWordsPerTile} *
                             sizeof(uint32_t)
                       : sparseVoxelRegionStabilityBufferSize(
                             settings_.width, settings_.height,
                             settings_.regionTileSize),
                   ::rendering::AccessPattern::SHADER_READ |
                       ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 28u,
               .bufferView = view(
                   buffers_.rayTileBudget,
                   ownsRayTileBudget_
                       ? uint64_t{kSparseVoxelRayBudgetHeaderWords} *
                             sizeof(uint32_t)
                       : sparseVoxelRayBudgetBufferSize(
                             settings_.width, settings_.height,
                             settings_.regionTileSize),
                   ::rendering::AccessPattern::SHADER_READ |
                       ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 29u,
               .bufferView = view(
                   buffers_.reconstructionStatistics,
                   ownsReconstructionStatistics_ ? uint64_t{2u} * sizeof(uint32_t)
                                                 : uint64_t{ReconstructionStatisticCount} * sizeof(uint32_t),
                   ::rendering::AccessPattern::SHADER_READ |
                       ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 32u,
               .bufferView = view(
                   buffers_.terrainOwnershipIds,
                   ownsTerrainOwnershipIds_
                       ? sizeof(uint32_t)
                       : pixels * sizeof(uint32_t),
                   ::rendering::AccessPattern::SHADER_READ)},
            },
        .textures =
            {
              {.binding = 3u, .textureView = sampledView(textures_.geometryNormal)},
              {.binding = 4u, .textureView = sampledView(textures_.shadingNormal)},
              {.binding = 5u, .textureView = sampledView(textures_.instanceId)},
              {.binding = 6u, .textureView = sampledView(textures_.voxelId)},
              {.binding = 7u, .textureView = sampledView(textures_.motionVector)},
              {.binding = 14u, .textureView = sampledView(textures_.historyField)},
              {.binding = 15u, .textureView = sampledView(textures_.historyId)},
              {.binding = 22u, .textureView = sampledView(textures_.historyMoments)},
              {.binding = 25u, .textureView = sampledView(textures_.historyLightHash)},
              {.binding = 30u, .textureView = sampledView(textures_.historyViewDepth)},
              {.binding = 33u, .textureView = sampledView(textures_.historyNormal)},
            },
        .storageTextures =
            {
              {.binding = 16u, .textureView = storageView(textures_.outputField)},
              {.binding = 17u, .textureView = storageView(textures_.outputId)},
              {.binding = 19u, .textureView = storageView(textures_.reactiveOutput)},
              {.binding = 23u, .textureView = storageView(textures_.outputMoments)},
              {.binding = 26u, .textureView = storageView(textures_.outputLightHash)},
              {.binding = 31u, .textureView = storageView(textures_.outputViewDepth)},
              {.binding = 34u, .textureView = storageView(textures_.outputNormal)},
            },
      }},
    });

    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-light-field.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "resolveSparseVoxelLightField",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  Textures textures_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool ownsLightReachability_ = false;
  bool ownsSkyGuideFeedback_ = false;
  bool ownsRegionStability_ = false;
  bool ownsRayTileBudget_ = false;
  bool ownsReconstructionStatistics_ = false;
  bool ownsTerrainOwnershipIds_ = false;
  std::vector<::rendering::Texture> ownedFallbackTextures_{};
};

} // namespace voxel::svraster::rendering
