#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Converts the full-resolution canonical ray domain into a compact list once,
// then all expensive ray passes indirect-dispatch over that shared list.
class SparseVoxelSceneRayWorklistPass final : public ::rendering::Pass
{
public:
  enum class Stage
  {
    Reset,
    Build,
    Finalize,
  };

  struct Buffers
  {
    ::rendering::Buffer candidates;
    ::rendering::Buffer activeIndices;
    ::rendering::Buffer workState;
    ::rendering::Buffer finalRays;
    ::rendering::Buffer finalReservoirs;
    ::rendering::Buffer secondaryWorkState;
  };

  SparseVoxelSceneRayWorklistPass(
      Stage stage, Buffers buffers, uint32_t rayCapacity,
      uint32_t reservoirCapacity = 0u)
      : stage_(stage), buffers_(buffers),
        rayCapacity_(std::max(1u, rayCapacity)),
        reservoirCapacity_(
            std::max(1u, reservoirCapacity == 0u
                ? rayCapacity
                : reservoirCapacity))
  {
  }

  ~SparseVoxelSceneRayWorklistPass() override
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
    const bool resetSecondary = stage_ == Stage::Reset &&
        buffers_.secondaryWorkState.isValid();
    const std::array<uint32_t, 4> settings{
      rayCapacity_, resetSecondary ? 1u : 0u, 0u, 0u};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(settings),
        const_cast<uint32_t *>(settings.data()));
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch(
        stage_ == Stage::Build ? (rayCapacity_ + 63u) / 64u : 1u,
        1u, 1u);
  }

private:
  static ::rendering::BufferView view(
      ::rendering::Buffer buffer, uint64_t size,
      ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    const uint64_t rays = rayCapacity_;
    // Reset touches only workState. Probe mode uses this entry point to clear
    // the active-ray indirect record before ProbeRays fills it, and has no
    // ReSTIR candidate array at all. Keep one valid storage element bound for
    // the statically declared but unreachable candidate binding instead of
    // forcing a full canonical-ray allocation.
    const uint64_t candidateRays = stage_ == Stage::Reset ? 1u : rays;
    const uint64_t reservoirCount = stage_ == Stage::Reset
        ? 1u
        : uint64_t{reservoirCapacity_};
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Settings",
      .size = 16u,
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "settings", .binding = 0u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "candidates", .binding = 1u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "activeIndices", .binding = 2u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "workState", .binding = 3u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "finalRays", .binding = 4u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "finalReservoirs", .binding = 5u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "secondaryWorkState", .binding = 6u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite =
        ::rendering::AccessPattern::SHADER_READ |
        ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = view(
              uniforms_, 16u, ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u, .bufferView = view(
              buffers_.candidates,
              candidateRays * sizeof(SparseVoxelSceneRayGPU), read)},
          {.binding = 2u, .bufferView = view(
              buffers_.activeIndices, rays * sizeof(uint32_t), readWrite)},
          {.binding = 3u, .bufferView = view(
              buffers_.workState, 16u, readWrite)},
          {.binding = 4u, .bufferView = view(
              buffers_.finalRays,
              rays * sizeof(SparseVoxelSceneRayGPU), readWrite)},
          {.binding = 5u, .bufferView = view(
              buffers_.finalReservoirs,
              reservoirCount * sizeof(SparseVoxelDIReservoirGPU),
              readWrite)},
          {.binding = 6u, .bufferView = view(
              buffers_.secondaryWorkState.isValid()
                  ? buffers_.secondaryWorkState
                  : buffers_.workState,
              16u, readWrite)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-scene-ray-worklist.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry =
          stage_ == Stage::Reset
              ? "resetSparseVoxelSceneRayWorklist"
              : stage_ == Stage::Finalize
                  ? "finalizeSparseVoxelSceneRayWorklist"
                  : "buildSparseVoxelSceneRayWorklist",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Stage stage_;
  Buffers buffers_{};
  uint32_t rayCapacity_ = 1u;
  uint32_t reservoirCapacity_ = 1u;
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
