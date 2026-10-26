#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPURasterBuildPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer config;
    ::rendering::Buffer columns;
    ::rendering::Buffer materialTable;
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
    ::rendering::Buffer microSurfaceData;
    ::rendering::Buffer childIndices;
    ::rendering::Buffer rootIndices;
    ::rendering::Buffer candidates;
    ::rendering::Buffer scratch;
    ::rendering::Buffer result;
    ::rendering::Buffer columnResult;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPURasterBuildPass(const uint32_t passSlot, const uint32_t stage,
      const uint32_t level, const TerrainGPURasterCapacity capacity,
      const uint32_t maximumColumns, const Buffers buffers)
      : passSlot_(passSlot), capacity_(capacity),
        maximumColumns_(maximumColumns), buffers_(buffers)
  {
    stageUniforms_[0] = stage;
    stageUniforms_[1] = level;
    if (maximumColumns == 0u || capacity.nodes == 0u ||
        !buffers.config.isValid() || !buffers.result.isValid() ||
        !buffers.indirectArgs.isValid())
      throw std::invalid_argument("GPU terrain raster pass is invalid");
  }

  ~TerrainGPURasterBuildPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(stageUniforms_), stageUniforms_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatchIndirect(buffers_.indirectArgs,
        uint64_t{passSlot_} * sizeof(IndirectDispatchArgsGPU));
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer,
      const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniformBuffer_ = renderGraph->createBuffer({
      .name = passName + ".Uniforms",
      .size = sizeof(stageUniforms_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false,
    });
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 14u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding), .binding = binding,
        .type = binding == 13u
            ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer
            : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + ".Layout", .groups = {std::move(group)},
    });
    const uint64_t nodeCount = capacity_.nodes;
    const uint64_t materialWords = (uint64_t{capacity_.materials} + 3u) / 4u;
    const uint64_t scratchWords = 16u + nodeCount * 512u;
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .name = passName + ".Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = view(buffers_.config,
              sizeof(TerrainGPURasterConfigGPU), read), .binding = 0u},
          {.bufferView = view(buffers_.columns,
              uint64_t{maximumColumns_} * sizeof(TerrainGPURasterColumnGPU),
              read), .binding = 1u},
          {.bufferView = view(buffers_.materialTable,
              kTerrainGPURasterMaterialTableSize *
                  sizeof(TerrainGPURasterMaterialGPU), read), .binding = 2u},
          {.bufferView = view(buffers_.nodes,
              nodeCount * sizeof(SparseVoxelBrickNodeGPU), readWrite),
              .binding = 3u},
          {.bufferView = view(buffers_.occupancy,
              nodeCount * sizeof(SparseVoxelBrickOccupancyGPU), readWrite),
              .binding = 4u},
          {.bufferView = view(buffers_.materialWords,
              materialWords * sizeof(uint32_t), readWrite), .binding = 5u},
          {.bufferView = view(buffers_.microSurfaceData,
              uint64_t{capacity_.materials} * sizeof(uint64_t), readWrite),
              .binding = 6u},
          {.bufferView = view(buffers_.childIndices,
              uint64_t{capacity_.children} * sizeof(uint32_t), readWrite),
              .binding = 7u},
          {.bufferView = view(buffers_.rootIndices,
              uint64_t{capacity_.roots} * sizeof(uint32_t), readWrite),
              .binding = 8u},
          {.bufferView = view(buffers_.candidates,
              uint64_t{capacity_.denseCandidates} * 2u * sizeof(uint32_t),
              readWrite), .binding = 9u},
          {.bufferView = view(buffers_.scratch,
              scratchWords * sizeof(uint32_t), readWrite), .binding = 10u},
          {.bufferView = view(buffers_.result,
              sizeof(TerrainGPURasterResultGPU), readWrite), .binding = 11u},
          {.bufferView = view(buffers_.columnResult,
              sizeof(TerrainGPUColumnResultGPU), read), .binding = 12u},
          {.bufferView = view(uniformBuffer_, sizeof(stageUniforms_),
              ::rendering::AccessPattern::UNIFORM_READ), .binding = 13u},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/terrain-gpu-raster-build.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline", .shader = shader_,
      .entry = "runTerrainRasterStage", .layout = layout_,
    });
  }

  uint32_t passSlot_ = 0u;
  TerrainGPURasterCapacity capacity_{};
  uint32_t maximumColumns_ = 0u;
  Buffers buffers_{};
  uint32_t stageUniforms_[4]{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
