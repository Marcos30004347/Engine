#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>

namespace voxel::svraster::rendering
{

class SparseVoxelSceneRayStatisticsResetPass final : public ::rendering::Pass
{
public:
  explicit SparseVoxelSceneRayStatisticsResetPass(
      const ::rendering::Buffer statistics,
      const uint64_t statisticsBytes = sizeof(SparseVoxelSceneRayStatisticsGPU))
      : statistics_(statistics),
        statisticsBytes_(std::max<uint64_t>(statisticsBytes, sizeof(uint32_t)))
  {
  }

  ~SparseVoxelSceneRayStatisticsResetPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  void ensureResources()
  {
    if (layout_.isValid()) return;
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{.buffers = {
        {.name = "statistics", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      }}},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {{
          .binding = 0u,
          .bufferView = {
            .buffer = statistics_,
            .offset = 0u,
            .size = statisticsBytes_,
            .access = ::rendering::AccessPattern::SHADER_WRITE,
          },
        }},
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-scene-ray-statistics-reset.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "resetSparseVoxelSceneRayStatistics",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  ::rendering::Buffer statistics_{};
  uint64_t statisticsBytes_ = sizeof(SparseVoxelSceneRayStatisticsGPU);
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
