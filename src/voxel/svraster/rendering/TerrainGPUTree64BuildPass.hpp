#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUTree64Build.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUTree64BuildPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer config;
    ::rendering::Buffer columns;
    ::rendering::Buffer masks;
    ::rendering::Buffer nodes;
    ::rendering::Buffer maskNodeIndices;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUTree64BuildPass(const uint32_t passSlot, const uint32_t stage,
      const uint32_t level, const TerrainGPUTree64Capacity capacity,
      const uint32_t maximumColumns, const Buffers buffers)
      : passSlot_(passSlot), capacity_(capacity),
        maximumColumns_(maximumColumns), buffers_(buffers)
  {
    stageUniforms_[0] = stage;
    stageUniforms_[1] = level;
    if (capacity.masks == 0u || capacity.nodes == 0u ||
        maximumColumns == 0u || !buffers.config.isValid() ||
        !buffers.columns.isValid() || !buffers.masks.isValid() ||
        !buffers.nodes.isValid() || !buffers.maskNodeIndices.isValid() ||
        !buffers.result.isValid() || !buffers.indirectArgs.isValid())
      throw std::invalid_argument("GPU terrain Tree64 pass is invalid");
  }

  ~TerrainGPUTree64BuildPass() override
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
    for (uint32_t binding = 0u; binding < 7u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding), .binding = binding,
        .type = binding == 6u
            ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer
            : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + ".Layout", .groups = {std::move(group)},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .name = passName + ".Groups", .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = view(buffers_.config,
              sizeof(TerrainGPUTree64ConfigGPU), read), .binding = 0u},
          {.bufferView = view(buffers_.columns,
              uint64_t{maximumColumns_} * sizeof(WorldMapTerrainColumnGPU),
              read), .binding = 1u},
          {.bufferView = view(buffers_.masks,
              uint64_t{capacity_.masks} * sizeof(SparseVoxelTree64NodeGPU),
              readWrite), .binding = 2u},
          {.bufferView = view(buffers_.nodes,
              uint64_t{capacity_.nodes} * sizeof(SparseVoxelTree64NodeGPU),
              readWrite), .binding = 3u},
          {.bufferView = view(buffers_.maskNodeIndices,
              uint64_t{capacity_.masks} * sizeof(uint32_t), readWrite),
              .binding = 4u},
          {.bufferView = view(buffers_.result,
              sizeof(TerrainGPUTree64ResultGPU), readWrite), .binding = 5u},
          {.bufferView = view(uniformBuffer_, sizeof(stageUniforms_),
              ::rendering::AccessPattern::UNIFORM_READ), .binding = 6u},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile(
          shaderPath()),
      .type = ::rendering::ShaderType::SpirV, .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline", .shader = shader_,
      .entry = entry(), .layout = layout_,
    });
  }

  const char *entry() const
  {
    switch (stageUniforms_[0])
    {
    case 0u: return "resetTerrainTree64";
    case 1u:
    case 2u: return "buildTerrainTree64Masks";
    case 3u: return "compactTerrainTree64";
    case 4u: return "validateTerrainTree64";
    default: return "";
    }
  }

  const char *shaderPath() const
  {
    switch (stageUniforms_[0])
    {
    case 0u:
      return "assets/shaders/spirv/svraster/terrain-gpu-tree64-reset.spirv";
    case 1u:
    case 2u:
      return "assets/shaders/spirv/svraster/terrain-gpu-tree64-masks.spirv";
    case 3u:
      return "assets/shaders/spirv/svraster/terrain-gpu-tree64-compact.spirv";
    case 4u:
      return "assets/shaders/spirv/svraster/terrain-gpu-tree64-validate.spirv";
    default: return "";
    }
  }

  uint32_t passSlot_ = 0u;
  TerrainGPUTree64Capacity capacity_{};
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
