#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2ColumnPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset,
    SampleHeights,
    SampleMaterials,
    BuildColumns,
    Finalize,
  };

  struct Capacities
  {
    uint32_t columnsX = 0u;
    uint32_t columnsZ = 0u;
    uint32_t halo = 0u;
    uint32_t faults = 0u;
    uint32_t workgroupX = 8u;
    uint32_t workgroupZ = 8u;
    uint32_t lanes = 1u;
    uint64_t metadataBytes = 0u;
    uint64_t pageTableBytes = 0u;
    uint64_t physicalBytes = 0u;
    uint64_t cityGroundBytes = 0u; // CityGroundOverlay::packGPU words.
  };

  struct Buffers
  {
    ::rendering::Buffer metadata;
    ::rendering::Buffer pageTable;
    ::rendering::Buffer physical;
    ::rendering::Buffer config;
    ::rendering::Buffer heights;
    ::rendering::Buffer materials;
    ::rendering::Buffer columns;
    ::rendering::Buffer faults;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
    ::rendering::Buffer cityGround;
  };

  TerrainGPUV2ColumnPass(const Stage stage, const Capacities capacities, const Buffers buffers) : stage_(stage), capacities_(capacities), buffers_(buffers)
  {
    const bool workgroup = (capacities.workgroupX == 8u && capacities.workgroupZ == 8u) || (capacities.workgroupX == 16u && capacities.workgroupZ == 8u) || (capacities.workgroupX == 8u && capacities.workgroupZ == 16u);
    if (!workgroup || capacities.columnsX == 0u || capacities.columnsZ == 0u || capacities.halo == 0u || capacities.faults == 0u || capacities.lanes == 0u || capacities.metadataBytes == 0u || capacities.pageTableBytes == 0u ||
        capacities.physicalBytes == 0u || !buffers.metadata.isValid() || !buffers.pageTable.isValid() || !buffers.physical.isValid() || !buffers.config.isValid() || !buffers.heights.isValid() ||
        !buffers.materials.isValid() || !buffers.columns.isValid() || !buffers.faults.isValid() || !buffers.result.isValid() || !buffers.indirectArgs.isValid() ||
        !buffers.cityGround.isValid() || capacities.cityGroundBytes == 0u)
      throw std::invalid_argument("terrain GPU V2 column pass is invalid");
  }

  ~TerrainGPUV2ColumnPass() override
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
    commands.cmdDispatchIndirect(
        buffers_.indirectArgs,
        static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU),
        enabled_);
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2Columns";
    case Stage::SampleHeights:
      return "sampleTerrainGPUV2Heights";
    case Stage::SampleMaterials:
      return "sampleTerrainGPUV2Materials";
    case Stage::BuildColumns:
      return "buildTerrainGPUV2Columns";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2Columns";
    }
    return "";
  }

  std::string shaderPath() const
  {
    return "assets/shaders/spirv/svraster/terrain-gpu-v2-columns-" + std::to_string(capacities_.workgroupX) + "x" + std::to_string(capacities_.workgroupZ) + ".spirv";
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 10u; ++binding)
      group.buffers.push_back(
          {.name = "buffer" + std::to_string(binding),
           .binding = binding,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false});
    layout_ = renderGraph->createBindingsLayout({.name = passName + ".Layout", .groups = {std::move(group)}});
    const uint64_t sampleX = capacities_.columnsX + capacities_.halo * 2u + 1u;
    const uint64_t sampleZ = capacities_.columnsZ + capacities_.halo * 2u + 1u;
    const uint64_t columnX = sampleX - 1u;
    const uint64_t columnZ = sampleZ - 1u;
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups",
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.metadata, capacities_.metadataBytes, read), .binding = 0u},
             {.bufferView = view(buffers_.pageTable, capacities_.pageTableBytes, read), .binding = 1u},
             {.bufferView = view(buffers_.physical, capacities_.physicalBytes, read), .binding = 2u},
             {.bufferView = view(buffers_.config, uint64_t{capacities_.lanes} * sizeof(TerrainGPUV2ColumnConfigGPU), read), .binding = 3u},
             {.bufferView = view(buffers_.heights, uint64_t{capacities_.lanes} * sampleX * sampleZ * sizeof(TerrainGPUV2HeightSampleGPU), rw), .binding = 4u},
             {.bufferView = view(buffers_.materials, uint64_t{capacities_.lanes} * columnX * columnZ * sizeof(uint32_t), rw), .binding = 5u},
             {.bufferView = view(buffers_.columns, uint64_t{capacities_.lanes} * columnX * columnZ * sizeof(TerrainGPUV2ColumnGPU), rw), .binding = 6u},
             {.bufferView = view(buffers_.faults, uint64_t{capacities_.lanes} * capacities_.faults * sizeof(TerrainGPUV2MapFaultGPU), rw), .binding = 7u},
             {.bufferView = view(buffers_.result, uint64_t{capacities_.lanes} * sizeof(TerrainGPUV2ColumnResultGPU), rw), .binding = 8u},
             {.bufferView = view(buffers_.cityGround, capacities_.cityGroundBytes, read), .binding = 9u},
           }}}});
    shader_ = renderGraph->createShader({.name = passName + ".Shader", .src = os::io::readRelativeFile(shaderPath()), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entry(), .layout = layout_});
  }

  Stage stage_{};
  Capacities capacities_{};
  Buffers buffers_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
};

} // namespace voxel::svraster::rendering
