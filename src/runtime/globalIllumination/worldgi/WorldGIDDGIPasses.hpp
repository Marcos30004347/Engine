#pragma once

#include "globalIllumination/worldgi/WorldGIDDGI.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace globalillumination::worldgi
{

// Refresh cursor and generation, owned by the GPU.
inline constexpr uint32_t kWorldGIDDGIScheduleWords = 4u;

// Mirrors WorldGIDDGISettings in worldgi_ddgi_common.wgsl.
struct alignas(16) WorldGIDDGISettingsGPU
{
  std::array<uint32_t, 4> cascadeRaysProbes{};
  std::array<float, 4> blendTuning{};
  std::array<float, 4> sunDirectionIntensity{};
  std::array<float, 4> sunColorCosine{};
  std::array<float, 4> skyZenithHorizon{};
  std::array<float, 4> groundSkyIntensity{};
  std::array<uint32_t, 4> frameSeedCursor{};
};
static_assert(sizeof(WorldGIDDGISettingsGPU) == 112u);

// Nine RGB spherical-harmonic coefficients per probe, one per vec4.
struct alignas(16) WorldGIDDGIProbeSHGPU
{
  std::array<std::array<float, 4>, kWorldGIDDGISHCoefficientCount> coefficients{};
};
static_assert(sizeof(WorldGIDDGIProbeSHGPU) == 144u);

// CPU-side cascade bookkeeping. Each frame the volumes are re-snapped to the
// camera and the toroidal scroll advanced, so probes that remain inside the
// volume keep their storage slot and therefore their history. This is what
// makes the cache stable under motion; rebuilding on movement would reintroduce
// exactly the swing it exists to remove.
class WorldGIDDGIVolumes
{
public:
  WorldGIDDGIVolumes() = default;

  void configure(const WorldGIDDGISettings &settings, const float baseSpacing)
  {
    settings_ = worldGIDDGISanitize(settings);
    baseSpacing_ = std::max(baseSpacing, 1.0e-3f);
    volumes_.assign(settings_.cascadeCount, WorldGIDDGIVolumeGPU{});
    uint32_t base = 0u;
    for (uint32_t cascade = 0u; cascade < settings_.cascadeCount; ++cascade)
    {
      WorldGIDDGIVolumeGPU &volume = volumes_[cascade];
      const float spacing = baseSpacing_ *
          static_cast<float>(1u << cascade);
      volume.originSpacing[0] = 0.0f;
      volume.originSpacing[1] = 0.0f;
      volume.originSpacing[2] = 0.0f;
      volume.originSpacing[3] = spacing;
      volume.dimensionsCascade[0] = settings_.resolution;
      volume.dimensionsCascade[1] = settings_.resolution;
      volume.dimensionsCascade[2] = settings_.resolution;
      volume.dimensionsCascade[3] = cascade;
      for (uint32_t axis = 0u; axis < 4u; ++axis)
        volume.scrollOffsetGeneration[axis] = 0;
      volume.probeBaseFlagsCursor[0] = base;
      volume.probeBaseFlagsCursor[1] = 0u;
      volume.probeBaseFlagsCursor[2] = 0u;
      volume.probeBaseFlagsCursor[3] = 0u;
      base += settings_.resolution * settings_.resolution *
          settings_.resolution;
      originValid_.push_back(false);
      origins_.push_back({0.0f, 0.0f, 0.0f});
    }
    probeCount_ = base;
  }

  // Returns true when any cascade scrolled, which the caller uses to decide
  // whether newly exposed probes need their history invalidated.
  bool update(const std::array<float, 3> cameraPosition)
  {
    bool scrolled = false;
    for (uint32_t cascade = 0u; cascade < volumes_.size(); ++cascade)
    {
      WorldGIDDGIVolumeGPU &volume = volumes_[cascade];
      const std::array<uint32_t, 3> dimensions{
        volume.dimensionsCascade[0], volume.dimensionsCascade[1],
        volume.dimensionsCascade[2]};
      const float spacing = volume.originSpacing[3];
      const std::array<float, 3> origin =
          worldGIDDGISnappedOrigin(cameraPosition, spacing, dimensions);
      if (!originValid_[cascade])
      {
        origins_[cascade] = origin;
        originValid_[cascade] = true;
      }
      const std::array<int32_t, 3> delta =
          worldGIDDGIScrollDelta(origins_[cascade], origin, spacing);
      if (delta[0] != 0 || delta[1] != 0 || delta[2] != 0)
      {
        scrolled = true;
        const std::array<int32_t, 3> offset{
          volume.scrollOffsetGeneration[0], volume.scrollOffsetGeneration[1],
          volume.scrollOffsetGeneration[2]};
        const std::array<int32_t, 3> advanced =
            worldGIDDGIAdvanceScrollOffset(offset, delta, dimensions);
        volume.scrollOffsetGeneration[0] = advanced[0];
        volume.scrollOffsetGeneration[1] = advanced[1];
        volume.scrollOffsetGeneration[2] = advanced[2];
        volume.scrollOffsetGeneration[3] += 1;
        origins_[cascade] = origin;
      }
      volume.originSpacing[0] = origin[0];
      volume.originSpacing[1] = origin[1];
      volume.originSpacing[2] = origin[2];
      volume.originSpacing[3] = spacing;
    }
    return scrolled;
  }

  const std::vector<WorldGIDDGIVolumeGPU> &volumes() const { return volumes_; }
  uint32_t probeCount() const { return probeCount_; }
  const WorldGIDDGISettings &settings() const { return settings_; }

private:
  WorldGIDDGISettings settings_{};
  float baseSpacing_ = 1.0f;
  uint32_t probeCount_ = 0u;
  std::vector<WorldGIDDGIVolumeGPU> volumes_;
  std::vector<std::array<float, 3>> origins_;
  std::vector<bool> originValid_;
};

// Writes this frame's probe rays in the Tree64 tracer's ray format. The trace
// itself is the existing SparseVoxelTree64TracePass, pointed at these buffers.
class WorldGIDDGIRayGenPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer volumes;
    ::rendering::Buffer rays;
    ::rendering::Buffer schedule;
  };

  WorldGIDDGIRayGenPass(
      const Buffers buffers, const uint32_t rayCapacity,
      const uint32_t cascadeCapacity)
      : buffers_(buffers), rayCapacity_(std::max(rayCapacity, 1u)),
        cascadeCapacity_(std::max(cascadeCapacity, 1u))
  {
  }

  ~WorldGIDDGIRayGenPass() override { destroy(); }

  void updateSettings(const WorldGIDDGISettingsGPU &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatch((rayCapacity_ + 63u) / 64u, 1u, 1u);
  }

private:
  void destroy()
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
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Settings",
         .size = sizeof(WorldGIDDGISettingsGPU),
         // Frame-local, and written from *both* updateSettings and the
         // recorded command buffer. That pairing is what actually delivers a
         // changing uniform here: with either half missing the kernel kept
         // reading the frame-zero copy, the refresh cursor never moved, and
         // one batch of 864 probes was refreshed forever while the other 6,048
         // stayed unvisited.
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "settings",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_UniformBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "volumes",
              .binding = 1u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_StorageBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "rays",
              .binding = 2u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_StorageBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "schedule",
              .binding = 3u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_StorageBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = {.buffer = uniforms_,
                .offset = 0u,
                .size = sizeof(WorldGIDDGISettingsGPU),
                .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u,
              .bufferView = {.buffer = buffers_.volumes,
                .offset = 0u,
                .size = uint64_t{cascadeCapacity_} *
                    sizeof(WorldGIDDGIVolumeGPU),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 2u,
              .bufferView = {.buffer = buffers_.rays,
                .offset = 0u,
                .size = uint64_t{rayCapacity_} * 32u,
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 3u,
              .bufferView = {.buffer = buffers_.schedule,
                .offset = 0u,
                .size = kWorldGIDDGIScheduleWords * sizeof(uint32_t),
                .access = ::rendering::AccessPattern::SHADER_READ}},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/worldgi/worldgi-ddgi-raygen.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.name = passName + "_Pipeline",
         .shader = shader_,
         .entry = "generateWorldGIDDGIRays",
         .layout = layout_});
  }

  Buffers buffers_{};
  uint32_t rayCapacity_ = 1u;
  uint32_t cascadeCapacity_ = 1u;
  WorldGIDDGISettingsGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Folds the traced results into the cache.
class WorldGIDDGIUpdatePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer volumes;
    ::rendering::Buffer results;
    ::rendering::Buffer probeSH;
    ::rendering::Buffer probeState;
    ::rendering::Buffer schedule;
    ::rendering::Buffer rays;
  };

  // The cursor advance is registered as its own pass rather than a second
  // dispatch inside the refresh. Two dispatches recorded into one pass did not
  // see each other's writes - the advance read the schedule the refresh had
  // just written as zeros, so the cursor stepped by (0 + 0) % 1 forever and one
  // batch of 864 probes was refreshed for the whole run. Separate passes get
  // the graph's ordering.
  enum class Mode
  {
    Refresh,
    AdvanceCursor,
  };

  WorldGIDDGIUpdatePass(
      const Buffers buffers, const uint32_t probeCapacity,
      const uint32_t rayCapacity, const uint32_t cascadeCapacity,
      const Mode mode = Mode::Refresh)
      : buffers_(buffers), probeCapacity_(std::max(probeCapacity, 1u)),
        rayCapacity_(std::max(rayCapacity, 1u)),
        cascadeCapacity_(std::max(cascadeCapacity, 1u)), mode_(mode)
  {
  }

  ~WorldGIDDGIUpdatePass() override { destroy(); }

  void updateSettings(const WorldGIDDGISettingsGPU &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatch(
        mode_ == Mode::Refresh ? (probeCapacity_ + 63u) / 64u : 1u, 1u, 1u);
  }

private:
  void destroy()
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
  }

  static ::rendering::BindingGroupLayoutBufferEntry storageEntry(
      const uint32_t binding, const char *name)
  {
    return {
      .name = name,
      .binding = binding,
      .isDynamic = false,
      .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Settings",
         .size = sizeof(WorldGIDDGISettingsGPU),
         // Frame-local, and written from *both* updateSettings and the
         // recorded command buffer. That pairing is what actually delivers a
         // changing uniform here: with either half missing the kernel kept
         // reading the frame-zero copy, the refresh cursor never moved, and
         // one batch of 864 probes was refreshed forever while the other 6,048
         // stayed unvisited.
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "settings",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_UniformBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
             storageEntry(1u, "volumes"),
             storageEntry(2u, "results"),
             storageEntry(3u, "probeSH"),
             storageEntry(4u, "probeState"),
             storageEntry(5u, "schedule"),
             storageEntry(6u, "rays"),
           }}}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = {.buffer = uniforms_,
                .offset = 0u,
                .size = sizeof(WorldGIDDGISettingsGPU),
                .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u,
              .bufferView = {.buffer = buffers_.volumes,
                .offset = 0u,
                .size = uint64_t{cascadeCapacity_} *
                    sizeof(WorldGIDDGIVolumeGPU),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 2u,
              .bufferView = {.buffer = buffers_.results,
                .offset = 0u,
                .size = uint64_t{rayCapacity_} * 48u,
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 3u,
              .bufferView = {.buffer = buffers_.probeSH,
                .offset = 0u,
                .size = uint64_t{probeCapacity_} *
                    sizeof(WorldGIDDGIProbeSHGPU),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 4u,
              .bufferView = {.buffer = buffers_.probeState,
                .offset = 0u,
                .size = uint64_t{probeCapacity_} *
                    sizeof(WorldGIDDGIProbeStateGPU),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 5u,
              .bufferView = {.buffer = buffers_.schedule,
                .offset = 0u,
                .size = kWorldGIDDGIScheduleWords * sizeof(uint32_t),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 6u,
              .bufferView = {.buffer = buffers_.rays,
                .offset = 0u,
                .size = uint64_t{rayCapacity_} * 32u,
                .access = ::rendering::AccessPattern::SHADER_READ}},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/worldgi/worldgi-ddgi-update.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.name = passName + "_Pipeline",
         .shader = shader_,
         .entry = mode_ == Mode::Refresh
             ? "updateWorldGIDDGIProbes"
             : "advanceWorldGIDDGICursor",
         .layout = layout_});
  }

  Buffers buffers_{};
  uint32_t probeCapacity_ = 1u;
  uint32_t rayCapacity_ = 1u;
  uint32_t cascadeCapacity_ = 1u;
  WorldGIDDGISettingsGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  Mode mode_ = Mode::Refresh;
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace globalillumination::worldgi
