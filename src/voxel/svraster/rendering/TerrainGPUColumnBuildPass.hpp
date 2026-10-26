#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUColumnBuildPass final : public ::rendering::Pass
{
public:
  struct BufferSizes
  {
    uint64_t metadata = 0u;
    uint64_t pageTable = 0u;
    uint64_t physical = 0u;
    uint32_t maximumSide = 0u;
    uint32_t maximumHalo = 0u;
  };

  struct Buffers
  {
    ::rendering::Buffer metadata;
    ::rendering::Buffer pageTable;
    ::rendering::Buffer physical;
    ::rendering::Buffer config;
    ::rendering::Buffer cornerHeights;
    ::rendering::Buffer materials;
    ::rendering::Buffer tops;
    ::rendering::Buffer columns;
    ::rendering::Buffer walls;
    ::rendering::Buffer rasterColumns;
    ::rendering::Buffer canonicalCorners;
    ::rendering::Buffer boundaryMinimums;
    ::rendering::Buffer boundaryMaximums;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUColumnBuildPass(uint32_t passSlot, uint32_t stage, BufferSizes sizes, Buffers buffers) : passSlot_(passSlot), sizes_(sizes), buffers_(buffers)
  {
    stageUniforms_[0] = stage;
    if (sizes.maximumSide == 0u || sizes.maximumHalo == 0u || sizes.metadata == 0u || sizes.pageTable == 0u || sizes.physical == 0u || !buffers.metadata.isValid() || !buffers.config.isValid() ||
        !buffers.result.isValid())
      throw std::invalid_argument("GPU terrain column pass is invalid");
  }

  ~TerrainGPUColumnBuildPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(stageUniforms_), stageUniforms_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(passSlot_) * sizeof(IndirectDispatchArgsGPU));
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer({
      .name = passName + ".Uniforms",
      .size = sizeof(stageUniforms_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false,
    });
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 15u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding),
        .binding = binding,
        .type = binding == 14u ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + ".Layout",
      .groups = {std::move(group)},
    });
    const uint64_t side = sizes_.maximumSide;
    const uint64_t halo = sizes_.maximumHalo;
    const uint64_t columns = side * side;
    const uint64_t cornerScratch = side + halo * 2u + 1u;
    const uint64_t topScratch = side + halo * 2u;
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph
                  ->createBindingGroups(
                      {
                        .name = passName + ".Groups",
                        .layout = layout_,
                        .groups =
                            {::rendering::GroupInfo{
                              .name = "group0",
                              .buffers =
                                  {
                                    {.bufferView = view(buffers_.metadata, sizes_.metadata, read), .binding = 0u},
                                    {.bufferView = view(buffers_.pageTable, sizes_.pageTable, read), .binding = 1u},
                                    {.bufferView = view(buffers_.physical, sizes_.physical, read), .binding = 2u},
                                    {.bufferView = view(buffers_.config, sizeof(TerrainGPUColumnConfigGPU), read), .binding = 3u},
                                    {.bufferView = view(buffers_.cornerHeights, cornerScratch * cornerScratch * sizeof(float), readWrite), .binding = 4u},
                                    {.bufferView = view(buffers_.materials, columns * sizeof(uint32_t), readWrite), .binding = 5u},
                                    {.bufferView = view(buffers_.tops, topScratch * topScratch * sizeof(uint32_t), readWrite), .binding = 6u},
                                    {.bufferView = view(buffers_.columns, columns * sizeof(WorldMapTerrainColumnGPU), readWrite), .binding = 7u},
                                    {.bufferView = view(buffers_.walls, columns * sizeof(WorldMapTerrainColumnWallGPU), readWrite), .binding = 8u},
                                    {.bufferView = view(buffers_.rasterColumns, columns * sizeof(TerrainGPURasterColumnGPU), readWrite), .binding = 9u},
                                    {.bufferView = view(buffers_.canonicalCorners, (side + 1u) * (side + 1u) * sizeof(TerrainGPUHeight64GPU), readWrite), .binding = 10u},
                                    {.bufferView = view(buffers_.boundaryMinimums, 4u * side * sizeof(TerrainGPUHeight64GPU), readWrite), .binding = 11u},
                                    {.bufferView = view(buffers_.boundaryMaximums, 4u * side * sizeof(TerrainGPUHeight64GPU), readWrite), .binding = 12u},
                                    {.bufferView = view(buffers_.result, sizeof(TerrainGPUColumnResultGPU), readWrite), .binding = 13u},
                                    {.bufferView = view(uniformBuffer_, sizeof(stageUniforms_), ::rendering::AccessPattern::UNIFORM_READ), .binding = 14u},
                                  },
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/terrain-gpu-column-build.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline",
      .shader = shader_,
      .entry = "runTerrainColumnStage",
      .layout = layout_,
    });
  }

  uint32_t passSlot_ = 0u;
  BufferSizes sizes_{};
  Buffers buffers_{};
  uint32_t stageUniforms_[4]{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
