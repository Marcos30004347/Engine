#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelProbe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

// Screen-space probe direct illumination, following Brixelizer GI's structure:
// spawn probes stochastically on the depth buffer, trace a full octahedral
// direction set per probe, project to second-order spherical harmonics, and
// interpolate those onto the G-buffer.
//
// The four passes below replace the entire per-pixel chain between the ray
// buffer and the lighting field texture. They deliberately share the existing
// Tree64 trace and hit-merge passes rather than introducing a second tracing
// backend: probe rays are ordinary scene rays with a dense, fully active
// compaction list.
namespace probe_detail
{
inline ::rendering::BufferView view(
    const ::rendering::Buffer buffer, const uint64_t size,
    const ::rendering::AccessPattern access)
{
  return {
    .buffer = buffer,
    .offset = 0u,
    .size = std::max<uint64_t>(size, sizeof(uint32_t)),
    .access = access};
}

inline ::rendering::TextureView sampledView(const ::rendering::Texture texture)
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

inline ::rendering::TextureView storageView(const ::rendering::Texture texture)
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

inline ::rendering::BindingGroupLayoutBufferEntry storageBuffer(
    const char *name, const uint32_t binding)
{
  return {
    .name = name,
    .binding = binding,
    .isDynamic = false,
    .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
    .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
}

inline ::rendering::BindingGroupLayoutBufferEntry uniformBuffer(
    const char *name, const uint32_t binding)
{
  return {
    .name = name,
    .binding = binding,
    .isDynamic = false,
    .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
    .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
}

inline ::rendering::BindingGroupLayoutTextureEntry sampledTexture(
    const char *name, const uint32_t binding)
{
  return {
    .name = name,
    .binding = binding,
    .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
}

inline ::rendering::BindingGroupLayoutStorageTextureEntry storageTexture(
    const char *name, const uint32_t binding)
{
  return {
    .name = name,
    .binding = binding,
    .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
}
} // namespace probe_detail

// Settings shared by every probe pass so the sample configures the path once.
struct SparseVoxelProbeSettings
{
  SparseVoxelRasterUniformsGPU raster{};
  uint32_t width = 1u;
  uint32_t height = 1u;
  uint32_t tileSize = kSparseVoxelProbeDefaultTileSize;
  uint32_t frameIndex = 0u;
  uint32_t generation = 0u;
  uint32_t lightCapacity = 1u;
  bool historyValid = false;
  // Surface-compatibility thresholds, matching the light field's so probe and
  // pixel history are invalidated by the same events.
  float normalThreshold = 0.85f;
  float depthThreshold = 0.05f;
  // How far beyond a probe's own tile footprint it may still serve pixels.
  float radiusScale = 1.0f;
  // Distance-to-plane falloff, in units of the probe's world radius.
  float planeScale = 1.0f;
  // Scale applied when no probe accepted a pixel and the nearest one is used.
  // A fallback is already lower confidence because it uses one probe instead
  // of the weighted compatible gather. Scaling its energy as well made the
  // stochastic fallback footprint visibly pulse dark on a static camera.
  float fallbackScale = 1.0f;
  // Refinement budget, as a percentage of the uniform probe count. Zero builds
  // the uniform grid alone.
  uint32_t adaptivePercent = kSparseVoxelProbeDefaultAdaptivePercent;
  // Deterministically reserve the two refinement lanes for distinct surface
  // normal clusters before the generic depth/plane-error descent uses any
  // remaining lane. At 90 degrees, perpendicular voxel faces split while
  // ordinary variation on a smooth surface stays in one probe.
  bool faceRefinement = true;
  float faceSplitAngleDegrees = 90.0f;
  // Ray sharing. A neighbouring probe's samples are accepted in proportion to
  // how closely its surface matches: the orientation term is raised to this
  // power, and the separation is measured against the probe's world radius
  // scaled by shareRadiusScale.
  float shareNormalPower = 16.0f;
  float shareRadiusScale = 2.0f;
  float shareMinimumWeight = 0.05f;
  // Responsiveness floor for the per-cell temporal blend. The blend weight is
  // 1/(n+1) over the probe's run of frames on one surface and never drops below
  // this, so a converged probe averages over roughly 1/cellBlend frames while a
  // changed one still reacts within that many.
  float cellBlend = 0.1f;
  // How many times the probe's own mean a single sample may reach before it is
  // treated as a firefly and scaled back. Lower is calmer and flatter; below
  // about four it starts erasing genuine small bright sources.
  float fireflyRatio = 8.0f;
  // Spatial probe filter acceptance. Deliberately stricter than the sharing
  // pass: sharing reprojects through a hit point and so knows a sample belongs
  // here, while the filter only knows two probes look alike.
  float filterNormalPower = 8.0f;
  float filterRadiusScale = 1.0f;
  float filterMinimumWeight = 0.05f;
  float maximumDistance = 64.0f;
  float originBias = 0.0f;
  float traceCellSize = 0.0f;
  std::array<float, 3> lightDirection{-0.408248f, 0.816497f, 0.408248f};
  // Direction slots per probe reserved for explicit emitter sampling. An
  // enabled directional light reserves at least one slot in the shader even
  // when this is zero; finite emitters use only the configured budget.
  uint32_t nextEventSlots = 16u;
  // Retained in the uniform layout for compatibility. Directional lights are
  // now sampled explicitly rather than spread into a stochastic sky cone.
  float directionalCosine = 0.9995f;
  // Probe update scheduling. These are how many frames apart a probe's turn
  // comes up in each tier; the ray-count settings below choose how much of the
  // direction set that update measures. Setting all intervals to one updates
  // every probe every frame.
  uint32_t intervalAwake = kSparseVoxelProbeIntervalAwake;
  uint32_t intervalConverged = kSparseVoxelProbeIntervalConverged;
  uint32_t intervalAsleep = kSparseVoxelProbeIntervalAsleep;
  // Directions traced per update in each tier.
  uint32_t raysAwake = kSparseVoxelProbeRaysAwake;
  // Validation budget used when a probe has a directional prior: either its
  // radiance map was prefilled from compatible on-screen neighbours or its
  // own same-surface predecessor was reprojected while the camera moved.
  uint32_t raysSeeded = kSparseVoxelProbeRaysSeeded;
  uint32_t raysConverged = kSparseVoxelProbeRaysConverged;
  uint32_t raysAsleep = kSparseVoxelProbeRaysAsleep;
  uint32_t wakeFrames = kSparseVoxelProbeWakeFrames;
  uint32_t minimumUpdates = kSparseVoxelProbeMinimumUpdates;
  float maximumError = kSparseVoxelProbeMaximumError;
  uint32_t convergedRuns = kSparseVoxelProbeConvergedRuns;
  uint32_t asleepRuns = kSparseVoxelProbeAsleepRuns;
  float motionRadiusFraction = kSparseVoxelProbeMotionRadiusFraction;
  // Distance-adaptive probe density and lighting. Non-positive distances use
  // the scale defaults from SparseVoxelProbe.hpp against maximumDistance.
  // Density tiers retain 1/4 and 1/16 of the uniform probes. Over the last
  // interval finite/local emitters fade out while traced sky/sun visibility
  // fades into unshadowed analytical sky and directional sunlight. At
  // skyOnlyDistance traversal stops entirely.
  float densityHalfDistance = 0.0f;
  float densityQuarterDistance = 0.0f;
  float localLightCullStartDistance = 0.0f;
  float skyOnlyDistance = 0.0f;
  uint32_t maximumDensityStride = kSparseVoxelProbeMaximumDensityStride;
  // Let a settled probe gather only its own predecessor instead of the
  // whole 3x3 neighbourhood. Saves most of the sharing pass's scatter, but
  // the neighbourhood gather is also what makes adjacent probes agree, so
  // skipping it trades spatial smoothness for that saving.
  bool shareSkipSettled = true;
  // Count per-pixel gather outcomes in the interpolate pass. Diagnostic only:
  // it costs two atomics per covered pixel, and the counters it fills are the
  // only way to observe a per-pixel hole in the probe gather, which every
  // per-probe metric is structurally blind to.
  bool collectDiagnostics = false;
  // Offer the previous frame's seed pixel as one more placement candidate, so a
  // tile only moves to a new surface when the old one is genuinely gone rather
  // than merely unsampled by this frame's jittered attempts.
  //
  // Without it a tile whose attempts all miss last frame's surface is forced
  // onto a perpendicular voxel face, its probe normal flips 90 degrees, and the
  // representability test - which hard-rejects on normal - reports the entire
  // region unrepresented at once. That is a region changing brightness in a
  // single frame, and it is driven entirely by the temporal jitter, which is
  // why it disappears with TAA off. Default on; here to be switched off for
  // diagnosis.
  bool seedContinuity = true;
  // Damp the directional SH bands where neighbouring geometry normals
  // disagree, i.e. where voxel faces are finer than a pixel and the resolved
  // normal flips between perpendicular axes from frame to frame. Costs two
  // texture loads. Generic scenes default off; terrain enables it because
  // sub-pixel top/riser alternation is the intended use case.
  bool normalCoherenceDamping = false;
  float normalCoherenceDampingStrength = 1.0f;
  float normalCoherenceThreshold = 0.75f;
  // Strength and screen-tile radius of the orientation-independent ambient
  // blend used only when the strict probe gather found no compatible probe.
  float fallbackAmbientStrength = 1.0f;
  uint32_t fallbackAmbientRadius = 2u;

  std::array<uint32_t, 4> scheduleIntervals() const
  {
    return {
      std::max(intervalAwake, 1u), std::max(intervalConverged, 1u),
      std::max(intervalAsleep, 1u), wakeFrames};
  }
  std::array<uint32_t, 4> scheduleRayCounts() const
  {
    const auto clampRays = [](const uint32_t rays)
    {
      return std::clamp(rays, 1u, kSparseVoxelProbeDirectionCount);
    };
    return {
      clampRays(raysAwake), clampRays(raysConverged), clampRays(raysAsleep),
      clampRays(raysSeeded)};
  }
  std::array<float, 4> scheduleThresholds() const
  {
    return {
      static_cast<float>(std::max(minimumUpdates, 1u)),
      std::clamp(maximumError, 0.01f, 1.0f),
      static_cast<float>(std::max(convergedRuns, 1u)),
      static_cast<float>(std::max(asleepRuns, convergedRuns + 1u))};
  }
  std::array<float, 4> distanceThresholds() const
  {
    return sparseVoxelProbeDistanceThresholds(maximumDistance,
        densityHalfDistance, densityQuarterDistance,
        localLightCullStartDistance, skyOnlyDistance);
  }

  uint32_t probesAcross() const
  {
    return sparseVoxelProbeCountAxis(width, tileSize);
  }
  uint32_t probesDown() const
  {
    return sparseVoxelProbeCountAxis(height, tileSize);
  }
  uint32_t probeCount() const { return probesAcross() * probesDown(); }
  uint32_t adaptiveCapacity() const
  {
    return sparseVoxelProbeAdaptiveCapacity(
        width, height, tileSize, adaptivePercent);
  }
  // Everything indexed by probe is sized for this: the uniform grid followed by
  // the refinement overflow.
  uint32_t totalProbeCount() const
  {
    return probeCount() + adaptiveCapacity();
  }
  uint32_t rayCount() const
  {
    return totalProbeCount() * kSparseVoxelProbeDirectionCount;
  }
  uint64_t probeBytes() const
  {
    return uint64_t{totalProbeCount()} * sizeof(SparseVoxelProbeGPU);
  }
  uint64_t radianceBytes() const
  {
    return uint64_t{totalProbeCount()} * kSparseVoxelProbeDirectionCount * 4u *
           sizeof(float);
  }
  uint64_t shBytes() const
  {
    return uint64_t{totalProbeCount()} * kSparseVoxelProbeSHCount * 4u *
           sizeof(float);
  }
  uint64_t adaptiveTileBytes() const
  {
    return uint64_t{probeCount()} *
           (kSparseVoxelProbeAdaptivePerTile + 1u) * sizeof(uint32_t);
  }
  uint64_t scheduleBytes() const
  {
    return uint64_t{totalProbeCount()} * sizeof(SparseVoxelProbeScheduleGPU);
  }
};

// Reprojects the previous frame's probe neighbourhood into this probe's frame,
// turning nine probes' worth of traced rays into shared directional samples.
class SparseVoxelProbeSharePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer probes;
    ::rendering::Buffer previousProbes;
    ::rendering::Buffer previousRadiance;
    ::rendering::Buffer radiance;
    // Last frame's scheduling state, so a settled probe can skip the
    // neighbourhood gather it no longer needs.
    ::rendering::Buffer previousSchedule;
  };

  SparseVoxelProbeSharePass(Buffers buffers, SparseVoxelProbeSettings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbeSharePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    // One workgroup per probe, refinement overflow included: the scatter into
    // direction cells is a workgroup reduction and wants the whole cell set
    // resident.
    cmd.cmdDispatch(settings_.totalProbeCount(), 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> weights{};
    std::array<uint32_t, 4> counts{};
    std::array<float, 4> cameraPositionCutoff{};
  };
  static_assert(sizeof(Uniforms) == 64u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.historyValid ? 1u : 0u, settings_.frameIndex},
        {std::max(settings_.shareNormalPower, 1.0f),
         std::max(settings_.shareRadiusScale, 1.0e-3f),
         std::max(settings_.shareMinimumWeight, 0.0f), 0.0f},
        {settings_.totalProbeCount(), std::max(settings_.tileSize, 1u),
         settings_.shareSkipSettled ? 1u : 0u, 0u},
        {settings_.raster.cameraWorldPositionNear[0],
         settings_.raster.cameraWorldPositionNear[1],
         settings_.raster.cameraWorldPositionNear[2],
         settings_.distanceThresholds()[3]}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t radianceBytes = settings_.radianceBytes();
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("settings", 0u), storageBuffer("probes", 1u),
          storageBuffer("previousProbes", 2u),
          storageBuffer("previousRadiance", 3u),
          storageBuffer("radiance", 4u),
          storageBuffer("previousSchedule", 5u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.previousProbes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.previousRadiance, radianceBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.radiance, radianceBytes,
               ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.previousSchedule, settings_.scheduleBytes(),
               ::rendering::AccessPattern::SHADER_READ)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-share.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "shareSparseVoxelProbeRays",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Spawns one probe per screen tile, reprojects it against last frame, and
// refines tiles that one probe cannot describe.
class SparseVoxelProbePlacePass final : public ::rendering::Pass
{
public:
  // The three stages share one binding layout and differ only in entry point,
  // so the refinement's allocation counter is reset, consumed and swept in a
  // single frame without any extra resources.
  enum class Stage : uint32_t
  {
    // Zeroes the refinement allocation counter.
    Reset = 0u,
    // Elects the uniform probe per tile and spawns refined probes.
    Place = 1u,
    // Invalidates the overflow slots refinement did not claim.
    ClearOverflow = 2u,
    // Zeroes the placement diagnostics. Diagnostic builds only.
    ResetDiagnostics = 3u,
  };

  // Per screen region on a 4x4 grid: tiles, covered pixels, pixels the first
  // refinement level found unrepresented, refined probes placed, and tiles that
  // wanted a refined probe and could not have one. Must match the constants in
  // `sparse_voxel_probe_place.wgsl`.
  static constexpr uint32_t kPlaceDiagnosticRegionCount = 16u;
  static constexpr uint32_t kPlaceDiagnosticWordsPerRegion = 5u;
  static constexpr uint32_t kPlaceDiagnosticWords =
      kPlaceDiagnosticRegionCount * kPlaceDiagnosticWordsPerRegion;

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer previousProbes;
    ::rendering::Buffer probes;
    ::rendering::Buffer allocation;
    ::rendering::Buffer adaptiveTiles;
    ::rendering::Buffer previousAdaptiveTiles;
    ::rendering::Buffer diagnostics;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture motionVector;
  };

  SparseVoxelProbePlacePass(
      Stage stage, Buffers buffers, SparseVoxelProbeSettings settings)
      : stage_(stage), buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbePlacePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    switch (stage_)
    {
      case Stage::ResetDiagnostics:
        cmd.cmdDispatch((kPlaceDiagnosticWords + 63u) / 64u, 1u, 1u);
        break;
      case Stage::Reset:
        cmd.cmdDispatch(1u, 1u, 1u);
        break;
      case Stage::Place:
        // One workgroup per tile: the spawn election, the representability
        // scan and the refinement rounds all want the tile resident in one
        // group.
        cmd.cmdDispatch(settings_.probeCount(), 1u, 1u);
        break;
      case Stage::ClearOverflow:
        cmd.cmdDispatch(
            std::max((settings_.adaptiveCapacity() + 63u) / 64u, 1u), 1u, 1u);
        break;
    }
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensionsTile{};
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> thresholds{};
    std::array<uint32_t, 4> flags{};
    std::array<float, 4> distances{};
    std::array<float, 4> faceSettings{};
  };
  static_assert(sizeof(Uniforms) == 96u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height, settings_.tileSize,
         settings_.frameIndex},
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.historyValid ? 1u : 0u, settings_.adaptiveCapacity()},
        {settings_.normalThreshold, settings_.depthThreshold,
         settings_.radiusScale, settings_.planeScale},
        {settings_.collectDiagnostics ? 1u : 0u,
         settings_.seedContinuity ? 1u : 0u,
         std::clamp(settings_.maximumDensityStride, 1u, 4u), 0u},
        settings_.distanceThresholds(),
        {std::cos(
             std::clamp(settings_.faceSplitAngleDegrees, 0.0f, 180.0f) *
             3.14159265358979323846f / 180.0f),
         settings_.faceRefinement ? 1.0f : 0.0f, 0.0f, 0.0f}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t pixels = uint64_t{settings_.width} * settings_.height;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t adaptiveTileBytes = settings_.adaptiveTileBytes();
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("raster", 0u), uniformBuffer("settings", 1u),
          storageBuffer("coverage", 2u), storageBuffer("linearDepth", 3u),
          storageBuffer("previousProbes", 8u), storageBuffer("probes", 9u),
          storageBuffer("allocation", 10u),
          storageBuffer("adaptiveTiles", 11u),
          storageBuffer("previousAdaptiveTiles", 12u),
          storageBuffer("diagnostics", 13u)},
        .textures = {
          sampledTexture("geometryNormal", 4u),
          sampledTexture("instanceId", 5u),
          sampledTexture("voxelId", 6u),
          sampledTexture("motionVector", 7u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               rasterBuffer_, sizeof(settings_.raster),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.coverage, pixels * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.linearDepth, pixels * sizeof(float),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 8u,
           .bufferView = view(
               buffers_.previousProbes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 9u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 10u,
           .bufferView = view(
               buffers_.allocation,
               kSparseVoxelProbeAllocationWords * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 11u,
           .bufferView = view(
               buffers_.adaptiveTiles, adaptiveTileBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 12u,
           .bufferView = view(
               buffers_.previousAdaptiveTiles, adaptiveTileBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 13u,
           .bufferView = view(
               buffers_.diagnostics,
               kPlaceDiagnosticWords * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)}},
        .textures = {
          {.binding = 4u, .textureView = sampledView(buffers_.geometryNormal)},
          {.binding = 5u, .textureView = sampledView(buffers_.instanceId)},
          {.binding = 6u, .textureView = sampledView(buffers_.voxelId)},
          {.binding = 7u,
           .textureView = sampledView(buffers_.motionVector)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-place.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    const char *entry = "placeSparseVoxelProbes";
    if (stage_ == Stage::ResetDiagnostics)
      entry = "resetSparseVoxelProbePlaceDiagnostics";
    else if (stage_ == Stage::Reset) entry = "resetSparseVoxelProbeAllocation";
    if (stage_ == Stage::ClearOverflow)
      entry = "clearSparseVoxelProbeOverflow";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Stage stage_{Stage::Place};
  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Emits each probe's scheduled direction subset into the shared scene ray
// buffer, using the shared radiance prior to reduce neighbour-seeded starts.
class SparseVoxelProbeRayPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer probes;
    ::rendering::Buffer lights;
    ::rendering::Buffer lightBVHInfo;
    ::rendering::Buffer rays;
    ::rendering::Buffer activeRayIndices;
    ::rendering::Buffer activeRayState;
    // Last frame's scheduling state, read through each probe's history index,
    // and this frame's, which the resolve pass finishes filling in.
    ::rendering::Buffer previousSchedule;
    ::rendering::Buffer schedule;
    // Last frame's probe records, for the motion gate: how far a probe slid
    // from the one it reprojected from is only answerable against them.
    ::rendering::Buffer previousProbes;
    // Directional prior produced by the sharing pass. New probes use its
    // coverage to select the cheaper neighbour-seeded initialization budget.
    ::rendering::Buffer radiance;
  };

  SparseVoxelProbeRayPass(Buffers buffers, SparseVoxelProbeSettings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbeRayPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch((settings_.rayCount() + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensionsTile{};
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> trace{};
    std::array<uint32_t, 4> counts{};
    std::array<float, 4> lightDirection{};
    std::array<uint32_t, 4> intervals{};
    std::array<uint32_t, 4> rayCounts{};
    std::array<float, 4> thresholds{};
    std::array<float, 4> distances{};
  };
  static_assert(sizeof(Uniforms) == 144u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height, settings_.tileSize,
         settings_.frameIndex},
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.lightCapacity,
         std::min(settings_.nextEventSlots,
                  kSparseVoxelProbeDirectionCount / 2u)},
        {settings_.maximumDistance, settings_.originBias,
         settings_.traceCellSize,
         std::max(settings_.motionRadiusFraction, 0.0f)},
        {settings_.totalProbeCount(), 0u, 0u, 0u},
        {settings_.lightDirection[0], settings_.lightDirection[1],
         settings_.lightDirection[2], 0.0f},
        settings_.scheduleIntervals(), settings_.scheduleRayCounts(),
        settings_.scheduleThresholds(), settings_.distanceThresholds()};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t rayBytes =
        uint64_t{settings_.rayCount()} * sizeof(SparseVoxelSceneRayGPU);
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("raster", 0u), uniformBuffer("settings", 1u),
          storageBuffer("probes", 2u), storageBuffer("lights", 3u),
          storageBuffer("lightBVHInfo", 4u), storageBuffer("rays", 5u),
          storageBuffer("activeRayIndices", 6u),
          storageBuffer("activeRayState", 7u),
          storageBuffer("previousSchedule", 8u),
          storageBuffer("schedule", 9u),
          storageBuffer("previousProbes", 10u),
          storageBuffer("radiance", 11u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               rasterBuffer_, sizeof(settings_.raster),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.lights,
               globalillumination::worldgi::worldGILightBufferSize(
                   settings_.lightCapacity),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.lightBVHInfo,
               globalillumination::worldgi::worldGILightBVHInfoBufferSize(),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.rays, rayBytes,
               ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 6u,
           .bufferView = view(
               buffers_.activeRayIndices,
               uint64_t{settings_.rayCount()} * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 7u,
           .bufferView = view(
               buffers_.activeRayState, 16u,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 8u,
           .bufferView = view(
               buffers_.previousSchedule, settings_.scheduleBytes(),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 9u,
           .bufferView = view(
               buffers_.schedule, settings_.scheduleBytes(),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 10u,
           .bufferView = view(
               buffers_.previousProbes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 11u,
           .bufferView = view(
               buffers_.radiance, settings_.radianceBytes(),
               ::rendering::AccessPattern::SHADER_READ)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-rays.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "createSparseVoxelProbeRays",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Projects each probe's traced directions into spherical harmonics and blends
// them with the reprojected history.
class SparseVoxelProbeResolvePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer probes;
    ::rendering::Buffer rays;
    ::rendering::Buffer hits;
    ::rendering::Buffer lights;
    ::rendering::Buffer radiance;
    ::rendering::Buffer probeSH;
    // The scheduling state the ray pass opened this frame. Only this pass knows
    // whether re-tracing changed the probe's answer, so it closes the loop.
    ::rendering::Buffer schedule;
    // Finite-light SH is accumulated separately from the directional map: a
    // next-event sample is an estimator contribution, not a radiance cell.
    // Keeping its own history stabilizes point/area lights without feeding a
    // pdf-weighted contribution back through ray sharing.
    ::rendering::Buffer previousEventSH;
    ::rendering::Buffer eventSH;
  };

  SparseVoxelProbeResolvePass(
      Buffers buffers, SparseVoxelProbeSettings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbeResolvePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch(settings_.totalProbeCount(), 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensionsTile{};
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> blend{};
    std::array<uint32_t, 4> counts{};
    std::array<float, 4> thresholds{};
    std::array<uint32_t, 4> intervals{};
    std::array<uint32_t, 4> rayCounts{};
    std::array<float, 4> distances{};
  };
  static_assert(sizeof(Uniforms) == 128u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height, settings_.tileSize,
         settings_.frameIndex},
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.lightCapacity,
         std::min(settings_.nextEventSlots,
                  kSparseVoxelProbeDirectionCount / 2u)},
        {std::clamp(settings_.cellBlend, 0.01f, 1.0f),
         std::clamp(settings_.directionalCosine, -1.0f, 0.999999f),
         std::max(settings_.fireflyRatio, 1.0f), 0.0f},
        {settings_.totalProbeCount(), 0u, 0u, 0u},
        settings_.scheduleThresholds(), settings_.scheduleIntervals(),
        settings_.scheduleRayCounts(), settings_.distanceThresholds()};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t rayBytes =
        uint64_t{settings_.rayCount()} * sizeof(SparseVoxelSceneRayGPU);
    const uint64_t hitBytes =
        uint64_t{settings_.rayCount()} * sizeof(SparseVoxelSceneRayHitGPU);
    const uint64_t shBytes = settings_.shBytes();
    const uint64_t radianceBytes = settings_.radianceBytes();
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("raster", 0u), uniformBuffer("settings", 1u),
          storageBuffer("probes", 2u), storageBuffer("sceneRays", 3u),
          storageBuffer("sceneHits", 4u), storageBuffer("lights", 5u),
          storageBuffer("radiance", 6u), storageBuffer("probeSH", 7u),
          storageBuffer("schedule", 8u),
          storageBuffer("previousEventSH", 9u),
          storageBuffer("eventSH", 10u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               rasterBuffer_, sizeof(settings_.raster),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.rays, rayBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.hits, hitBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.lights,
               globalillumination::worldgi::worldGILightBufferSize(
                   settings_.lightCapacity),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 6u,
           .bufferView = view(
               buffers_.radiance, radianceBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 7u,
           .bufferView = view(
               buffers_.probeSH, shBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 8u,
           .bufferView = view(
               buffers_.schedule, settings_.scheduleBytes(),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 9u,
           .bufferView = view(
               buffers_.previousEventSH, shBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 10u,
           .bufferView = view(
               buffers_.eventSH, shBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-resolve.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "resolveSparseVoxelProbes",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Spatially filters the probe SH against the surrounding probes. A 3x3 kernel
// here spans 24x24 pixels of already-integrated estimates, which is why it buys
// what a far larger screen-space kernel would.
class SparseVoxelProbeFilterPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer probes;
    ::rendering::Buffer sourceSH;
    ::rendering::Buffer filteredSH;
  };

  SparseVoxelProbeFilterPass(Buffers buffers, SparseVoxelProbeSettings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbeFilterPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    // One lane per coefficient, so seven probes per 63-lane group.
    const uint32_t lanes =
        settings_.totalProbeCount() * kSparseVoxelProbeSHCount;
    cmd.cmdDispatch(std::max((lanes + 62u) / 63u, 1u), 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> weights{};
  };
  static_assert(sizeof(Uniforms) == 32u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.totalProbeCount(), settings_.tileSize},
        {std::max(settings_.filterNormalPower, 1.0f),
         std::max(settings_.filterRadiusScale, 1.0e-3f),
         std::max(settings_.filterMinimumWeight, 0.0f), 0.0f}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t shBytes = settings_.shBytes();
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("settings", 0u), storageBuffer("probes", 1u),
          storageBuffer("sourceSH", 2u), storageBuffer("filteredSH", 3u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.sourceSH, shBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.filteredSH, shBytes,
               ::rendering::AccessPattern::SHADER_WRITE)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-filter.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "filterSparseVoxelProbes",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Gathers the surrounding SH probes into the full-resolution lighting field.
class SparseVoxelProbeInterpolatePass final : public ::rendering::Pass
{
public:
  // The interpolate dispatch, or the tiny reset that zeroes its diagnostic
  // counters. Both run the same shader over the same binding layout, so the
  // reset costs a pipeline and nothing else - the same shape
  // `TemporalMetricsPass` uses for the same reason.
  enum class Stage : uint32_t
  {
    Interpolate = 0u,
    ResetDiagnostics = 1u,
  };

  // Words in the diagnostics buffer: covered, near-black result, fallback and
  // widened-search pixel counts, then the same fields and rejection reasons for
  // each cell of a 4x4 screen grid. Must match the constants in
  // `sparse_voxel_probe_interpolate.wgsl`.
  static constexpr uint32_t kDiagnosticRegionAxis = 4u;
  static constexpr uint32_t kDiagnosticRegionCount =
      kDiagnosticRegionAxis * kDiagnosticRegionAxis;
  static constexpr uint32_t kDiagnosticRegionWords = 14u;
  static constexpr uint32_t kDiagnosticRegionOffset = 14u;
  static constexpr uint32_t kDiagnosticWords = kDiagnosticRegionOffset +
      kDiagnosticRegionCount * kDiagnosticRegionWords;
  static constexpr uint32_t kDiagnosticCovered = 0u;
  static constexpr uint32_t kDiagnosticBlack = 1u;
  static constexpr uint32_t kDiagnosticFallback = 2u;
  static constexpr uint32_t kDiagnosticWidened = 3u;

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer probes;
    ::rendering::Buffer probeSH;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture shadingNormal;
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture outputField;
    ::rendering::Texture outputId;
    ::rendering::Texture reactiveOutput;
    ::rendering::Texture outputMoments;
    ::rendering::Texture outputLightHash;
    ::rendering::Buffer adaptiveTiles;
    ::rendering::Buffer radiance;
    ::rendering::Buffer diagnostics;
    // World irradiance cache. Both may be invalid, in which case the pass
    // binds the probe buffers as harmless stand-ins and the shader's cascade
    // count of zero keeps it from reading them.
    ::rendering::Buffer worldCacheVolumes;
    ::rendering::Buffer worldCacheProbeSH;
    uint32_t worldCacheCascades = 0u;
    // Replace the screen-probe gather with the cache query outright.
    bool worldCachePrimary = false;
  };

  SparseVoxelProbeInterpolatePass(
      Stage stage, Buffers buffers, SparseVoxelProbeSettings settings)
      : stage_(stage), buffers_(buffers), settings_(settings)
  {
  }

  void update(const SparseVoxelProbeSettings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelProbeInterpolatePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (stage_ == Stage::ResetDiagnostics)
      cmd.cmdDispatch((kDiagnosticWords + 63u) / 64u, 1u, 1u);
    else
      cmd.cmdDispatch(
          (settings_.width + 7u) / 8u, (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensionsTile{};
    std::array<uint32_t, 4> probeCounts{};
    std::array<float, 4> weights{};
    std::array<float, 4> geometry{};
    std::array<float, 4> distances{};
    // x world irradiance cache cascade count; zero disables the query.
    std::array<uint32_t, 4> worldCache{};
  };
  static_assert(sizeof(Uniforms) == 96u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height, settings_.tileSize,
         settings_.frameIndex},
        {settings_.probesAcross(), settings_.probesDown(),
         settings_.collectDiagnostics ? 1u : 0u,
         settings_.normalCoherenceDamping ? 1u : 0u},
        {settings_.normalThreshold, settings_.depthThreshold,
         settings_.planeScale, settings_.fallbackScale},
        {std::clamp(settings_.normalCoherenceDampingStrength, 0.0f, 1.0f),
         std::clamp(settings_.normalCoherenceThreshold, 0.01f, 1.0f),
         std::clamp(settings_.fallbackAmbientStrength, 0.0f, 1.0f),
         static_cast<float>(std::min(settings_.fallbackAmbientRadius, 4u))},
        settings_.distanceThresholds(),
        {buffers_.worldCacheProbeSH.isValid()
             ? buffers_.worldCacheCascades
             : 0u,
         buffers_.worldCachePrimary ? 1u : 0u, 0u, 0u}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    using namespace probe_detail;
    const uint64_t pixels = uint64_t{settings_.width} * settings_.height;
    const uint64_t probeBytes = settings_.probeBytes();
    const uint64_t shBytes = settings_.shBytes();
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformBuffer("raster", 0u), uniformBuffer("settings", 1u),
          storageBuffer("coverage", 2u), storageBuffer("linearDepth", 3u),
          storageBuffer("probes", 4u), storageBuffer("probeSH", 5u),
          storageBuffer("adaptiveTiles", 15u),
          storageBuffer("probeRadiance", 16u),
          storageBuffer("diagnostics", 17u),
          storageBuffer("ddgiVolumes", 18u),
          storageBuffer("ddgiProbeSH", 19u)},
        .textures = {
          sampledTexture("geometryNormal", 6u),
          sampledTexture("shadingNormal", 7u),
          sampledTexture("instanceId", 8u),
          sampledTexture("voxelId", 9u)},
        .storageTextures = {
          storageTexture("outputField", 10u),
          storageTexture("outputId", 11u),
          storageTexture("reactiveOutput", 12u),
          storageTexture("outputMoments", 13u),
          storageTexture("outputLightHash", 14u)}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               rasterBuffer_, sizeof(settings_.raster),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.coverage, pixels * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.linearDepth, pixels * sizeof(float),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.probes, probeBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.probeSH, shBytes,
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 15u,
           .bufferView = view(
               buffers_.adaptiveTiles, settings_.adaptiveTileBytes(),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 16u,
           .bufferView = view(
               buffers_.radiance, settings_.radianceBytes(),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 17u,
           .bufferView = view(
               buffers_.diagnostics, kDiagnosticWords * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 18u,
           .bufferView = view(
               buffers_.worldCacheVolumes.isValid()
                   ? buffers_.worldCacheVolumes
                   : buffers_.probes,
               buffers_.worldCacheVolumes.isValid()
                   ? uint64_t{8u} * 64u
                   : settings_.probeBytes(),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 19u,
           .bufferView = view(
               buffers_.worldCacheProbeSH.isValid()
                   ? buffers_.worldCacheProbeSH
                   : buffers_.probeSH,
               buffers_.worldCacheProbeSH.isValid()
                   ? uint64_t{buffers_.worldCacheCascades} * 12u * 12u * 12u *
                       144u
                   : shBytes,
               ::rendering::AccessPattern::SHADER_READ)}},
        .textures = {
          {.binding = 6u, .textureView = sampledView(buffers_.geometryNormal)},
          {.binding = 7u, .textureView = sampledView(buffers_.shadingNormal)},
          {.binding = 8u, .textureView = sampledView(buffers_.instanceId)},
          {.binding = 9u, .textureView = sampledView(buffers_.voxelId)}},
        .storageTextures = {
          {.binding = 10u, .textureView = storageView(buffers_.outputField)},
          {.binding = 11u, .textureView = storageView(buffers_.outputId)},
          {.binding = 12u,
           .textureView = storageView(buffers_.reactiveOutput)},
          {.binding = 13u, .textureView = storageView(buffers_.outputMoments)},
          {.binding = 14u,
           .textureView = storageView(buffers_.outputLightHash)}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-probe-interpolate.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = stage_ == Stage::ResetDiagnostics
          ? "resetSparseVoxelProbeDiagnostics"
          : "interpolateSparseVoxelProbes",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Stage stage_{Stage::Interpolate};
  Buffers buffers_{};
  SparseVoxelProbeSettings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
