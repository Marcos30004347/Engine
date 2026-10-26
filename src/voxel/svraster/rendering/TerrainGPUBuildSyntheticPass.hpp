#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace voxel::svraster::rendering
{

struct TerrainGPUSyntheticWorkGPU
{
  uint32_t jobIdLow = 0u;
  uint32_t jobIdHigh = 0u;
  uint32_t stage = 0u;
  uint32_t cursor = 0u;
  uint32_t workUnits = 0u;
  uint32_t keyHash = 0u;
  uint32_t dispatchIdLow = 0u;
  uint32_t dispatchIdHigh = 0u;
};

struct TerrainGPUSyntheticResultGPU
{
  uint32_t dispatchIdLow = 0u;
  uint32_t dispatchIdHigh = 0u;
  uint32_t checksum = 0u;
  uint32_t completed = 0u;
};

static_assert(sizeof(TerrainGPUSyntheticWorkGPU) == 32u);
static_assert(sizeof(TerrainGPUSyntheticResultGPU) == 16u);

class TerrainGPUBuildSyntheticPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer work;
    ::rendering::Buffer results;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUBuildSyntheticPass(const uint32_t stageIndex,
      const uint32_t stageCapacity, const uint32_t stageCount,
      const Buffers buffers)
      : stageIndex_(stageIndex), stageCapacity_(stageCapacity),
        stageCount_(stageCount), buffers_(buffers)
  {
    if (stageCapacity == 0u || stageIndex >= stageCount ||
        !buffers.work.isValid() || !buffers.results.isValid() ||
        !buffers.indirectArgs.isValid())
      throw std::invalid_argument(
          "GPU terrain synthetic pass capacity is invalid");
    uniforms_[0] = stageIndex;
    uniforms_[1] = stageCapacity;
  }

  ~TerrainGPUBuildSyntheticPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(uniforms_), uniforms_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatchIndirect(buffers_.indirectArgs,
        uint64_t{stageIndex_} * sizeof(IndirectDispatchArgsGPU));
  }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer({
      .name = passName + ".Uniforms",
      .size = sizeof(uniforms_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + ".Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "work", .binding = 0u,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
          {.name = "results", .binding = 1u,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
          {.name = "uniforms", .binding = 2u,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false},
        },
      }},
    });
    const uint64_t recordCount = uint64_t{stageCapacity_} * stageCount_;
    groups_ = renderGraph->createBindingGroups({
      .name = passName + ".Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = {
             .buffer = buffers_.work, .offset = 0u,
             .size = recordCount * sizeof(TerrainGPUSyntheticWorkGPU),
             .access = ::rendering::AccessPattern::SHADER_READ},
           .binding = 0u},
          {.bufferView = {
             .buffer = buffers_.results, .offset = 0u,
             .size = recordCount * sizeof(TerrainGPUSyntheticResultGPU),
             .access = ::rendering::AccessPattern::SHADER_WRITE},
           .binding = 1u},
          {.bufferView = {
             .buffer = uniformBuffer_, .offset = 0u,
             .size = sizeof(uniforms_),
             .access = ::rendering::AccessPattern::UNIFORM_READ},
           .binding = 2u},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "terrain-gpu-build-synthetic.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline", .shader = shader_,
      .entry = "runTerrainSyntheticWork", .layout = layout_,
    });
  }

  uint32_t stageIndex_ = 0u;
  uint32_t stageCapacity_ = 0u;
  uint32_t stageCount_ = 0u;
  Buffers buffers_{};
  uint32_t uniforms_[4]{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
