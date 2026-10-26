#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"
#include "voxel/svraster/TerrainGPUTree64Build.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"
#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"
#include "voxel/svraster/TerrainGPUV2Tree64.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2RendererAdapterPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer fineResult;
    ::rendering::Buffer hierarchyResult;
    ::rendering::Buffer treeResult;
    ::rendering::Buffer rasterCompatibilityResult;
    ::rendering::Buffer treeCompatibilityResult;
    ::rendering::Buffer indirectArgs;
  };

  explicit TerrainGPUV2RendererAdapterPass(
      const Buffers buffers, const bool leafOnlyRaster = false,
      const uint32_t lanes = 1u)
      : buffers_(buffers), leafOnlyRaster_(leafOnlyRaster), lanes_(lanes)
  {
    const std::array<::rendering::Buffer, 6u> all{
      buffers.fineResult, buffers.hierarchyResult, buffers.treeResult,
      buffers.rasterCompatibilityResult, buffers.treeCompatibilityResult,
      buffers.indirectArgs};
    constexpr std::array<const char *, 6u> names{
      "fine result", "hierarchy result", "Tree64 result",
      "raster compatibility result", "Tree64 compatibility result",
      "indirect arguments"};
    if (lanes_ == 0u)
      throw std::invalid_argument(
          "terrain GPU V2 renderer adapter has zero lanes");
    for (size_t index = 0u; index < all.size(); ++index)
      if (!all[index].isValid())
        throw std::invalid_argument(
            std::string{"terrain GPU V2 renderer adapter has invalid "} +
            names[index] + " buffer");
  }

  ~TerrainGPUV2RendererAdapterPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    commands.cmdBindComputePipeline(pipeline_, enabled_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u, enabled_);
    commands.cmdDispatchIndirect(buffers_.indirectArgs, 0u, enabled_);
    commands.cmdMemoryBarrier(enabled_);
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer, const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 5u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding),
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + ".Layout", .groups = {std::move(group)}});
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .name = passName + ".Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = view(buffers_.fineResult, uint64_t{lanes_} * sizeof(TerrainGPUV2FineRasterResultGPU), read), .binding = 0u},
          {.bufferView = view(buffers_.hierarchyResult, uint64_t{lanes_} * sizeof(TerrainGPUV2RasterHierarchyResultGPU), read), .binding = 1u},
          {.bufferView = view(buffers_.treeResult, uint64_t{lanes_} * sizeof(TerrainGPUV2Tree64ResultGPU), read), .binding = 2u},
          {.bufferView = view(buffers_.rasterCompatibilityResult, uint64_t{lanes_} * sizeof(TerrainGPURasterResultGPU), rw), .binding = 3u},
          {.bufferView = view(buffers_.treeCompatibilityResult, uint64_t{lanes_} * sizeof(TerrainGPUTree64ResultGPU), rw), .binding = 4u},
        }}}});
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/terrain-gpu-v2-renderer-adapter.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_});
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline",
      .shader = shader_,
      .entry = leafOnlyRaster_
          ? "adaptTerrainGPUV2RendererLeafPayload"
          : "adaptTerrainGPUV2RendererPayload",
      .layout = layout_});
  }

  Buffers buffers_{};
  bool leafOnlyRaster_ = false;
  uint32_t lanes_ = 1u;
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
};

} // namespace voxel::svraster::rendering
