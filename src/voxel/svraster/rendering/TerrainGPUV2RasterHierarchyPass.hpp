#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2RasterHierarchyPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset,
    ClearMappings,
    ClearMaterials,
    CopyLeaves,
    CopyLeafMaterials,
    Classify,
    Reduce,
    ScanLevel0,
    ScanLevel1,
    ScanLevel2,
    AddLevel1,
    AddLevel0,
    Reserve,
    EmitNodes,
    EmitWords,
    Link,
    EmitRoots,
    Validate,
    Finalize,
  };

  struct Buffers
  {
    ::rendering::Buffer config;
    ::rendering::Buffer leafNodes;
    ::rendering::Buffer leafOccupancy;
    ::rendering::Buffer leafMaterials;
    ::rendering::Buffer rasterResult;
    ::rendering::Buffer scratch;
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
    ::rendering::Buffer childIndices;
    ::rendering::Buffer rootIndices;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
    ::rendering::Buffer heightSamples;
  };

  TerrainGPUV2RasterHierarchyPass(const Stage stage, const uint32_t level, const uint32_t passSlot, const TerrainGPUV2RasterHierarchyConfigGPU capacity, const uint32_t workgroup, const uint32_t lanes, const Buffers buffers)
      : stage_(stage), level_(level), passSlot_(passSlot), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), buffers_(buffers)
  {
    const bool levelStage = stage >= Stage::Classify && stage <= Stage::Link;
    const bool compiled = workgroup == 64u || workgroup == 128u || workgroup == 256u;
    if (!compiled || lanes == 0u || capacity.levelCount < 2u || capacity.levelCount > kTerrainGPUV2RasterHierarchyMaximumLevels || capacity.maximumParentCandidates == 0u || capacity.nodeCapacity == 0u ||
        capacity.materialWordCapacity == 0u || capacity.childCapacity == 0u || capacity.rootCapacity == 0u || (levelStage && (level == 0u || level >= capacity.levelCount)) || !buffers.config.isValid() ||
        !buffers.leafNodes.isValid() || !buffers.leafOccupancy.isValid() || !buffers.leafMaterials.isValid() || !buffers.rasterResult.isValid() || !buffers.scratch.isValid() || !buffers.nodes.isValid() ||
        !buffers.occupancy.isValid() || !buffers.materialWords.isValid() || !buffers.childIndices.isValid() || !buffers.rootIndices.isValid() || !buffers.result.isValid() || !buffers.indirectArgs.isValid() ||
        !buffers.heightSamples.isValid())
      throw std::invalid_argument("terrain GPU V2 raster hierarchy pass is invalid");
  }

  ~TerrainGPUV2RasterHierarchyPass() override
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
        uint64_t{passSlot_} * sizeof(IndirectDispatchArgsGPU), enabled_);
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

private:
  static uint32_t divide(const uint32_t value, const uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  std::string entry() const
  {
    const std::string level = std::to_string(level_);
    switch (stage_)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2RasterHierarchy";
    case Stage::ClearMappings:
      return "clearTerrainGPUV2RasterHierarchyMappings";
    case Stage::ClearMaterials:
      return "clearTerrainGPUV2RasterHierarchyMaterials";
    case Stage::CopyLeaves:
      return "copyTerrainGPUV2RasterHierarchyLeaves";
    case Stage::CopyLeafMaterials:
      return "copyTerrainGPUV2RasterHierarchyLeafMaterials";
    case Stage::Classify:
      return "classifyTerrainGPUV2RasterHierarchyLevel" + level;
    case Stage::Reduce:
      return "reduceTerrainGPUV2RasterHierarchyLevel" + level;
    case Stage::ScanLevel0:
      return "scanTerrainGPUV2RasterHierarchyL" + level + "S0";
    case Stage::ScanLevel1:
      return "scanTerrainGPUV2RasterHierarchyL" + level + "S1";
    case Stage::ScanLevel2:
      return "scanTerrainGPUV2RasterHierarchyL" + level + "S2";
    case Stage::AddLevel1:
      return "addTerrainGPUV2RasterHierarchyL" + level + "S1";
    case Stage::AddLevel0:
      return "addTerrainGPUV2RasterHierarchyL" + level + "S0";
    case Stage::Reserve:
      return "reserveTerrainGPUV2RasterHierarchyLevel" + level;
    case Stage::EmitNodes:
      return "emitTerrainGPUV2RasterHierarchyNodesL" + level;
    case Stage::EmitWords:
      return "emitTerrainGPUV2RasterHierarchyWordsL" + level;
    case Stage::Link:
      return "linkTerrainGPUV2RasterHierarchyL" + level;
    case Stage::EmitRoots:
      return "emitTerrainGPUV2RasterHierarchyRoots";
    case Stage::Validate:
      return "validateTerrainGPUV2RasterHierarchy";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2RasterHierarchy";
    }
    return {};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    TerrainGPUV2RasterHierarchyScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2RasterHierarchyScratchLayout(capacity_.maximumParentCandidates, capacity_.totalDenseCandidates, capacity_.levelCount, scratch, &error))
      throw std::runtime_error(error);
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 13u; ++binding)
      group.buffers.push_back(
          {.name = "buffer" + std::to_string(binding),
           .binding = binding,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false});
    layout_ = renderGraph->createBindingsLayout({.name = passName + ".Layout", .groups = {std::move(group)}});
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups",
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.config, uint64_t{lanes_} * sizeof(TerrainGPUV2RasterHierarchyConfigGPU), read), .binding = 0u},
             {.bufferView = view(buffers_.leafNodes, uint64_t{lanes_} * capacity_.leafNodeCapacity * sizeof(SparseVoxelBrickNodeGPU), read), .binding = 1u},
             {.bufferView = view(buffers_.leafOccupancy, uint64_t{lanes_} * capacity_.leafNodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), read), .binding = 2u},
             {.bufferView = view(buffers_.leafMaterials, uint64_t{lanes_} * capacity_.leafMaterialWordCapacity * sizeof(uint32_t), read), .binding = 3u},
             {.bufferView = view(buffers_.rasterResult, uint64_t{lanes_} * sizeof(TerrainGPUV2FineRasterResultGPU), read), .binding = 4u},
             {.bufferView = view(buffers_.scratch, uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t), rw), .binding = 5u},
             {.bufferView = view(buffers_.nodes, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickNodeGPU), rw), .binding = 6u},
             {.bufferView = view(buffers_.occupancy, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), rw), .binding = 7u},
             {.bufferView = view(buffers_.materialWords, uint64_t{lanes_} * capacity_.materialWordCapacity * sizeof(uint32_t), rw), .binding = 8u},
             {.bufferView = view(buffers_.childIndices, uint64_t{lanes_} * capacity_.childCapacity * sizeof(uint32_t), rw), .binding = 9u},
             {.bufferView = view(buffers_.rootIndices, uint64_t{lanes_} * capacity_.rootCapacity * sizeof(uint32_t), rw), .binding = 10u},
             {.bufferView = view(buffers_.result, uint64_t{lanes_} * sizeof(TerrainGPUV2RasterHierarchyResultGPU), rw), .binding = 11u},
             {.bufferView = view(buffers_.heightSamples,
                  uint64_t{lanes_} *
                      std::max<uint32_t>(1u,
                          ((capacity_.reserved >> 16u) *
                              (capacity_.bricksZ * capacity_.brickSide +
                                  2u * (capacity_.reserved & 0xffffu) + 1u))) *
                      sizeof(TerrainGPUV2HeightSampleGPU),
                  read),
              .binding = 12u},
           }}}});
    const std::string path = "assets/shaders/spirv/svraster/terrain-gpu-v2-raster-hierarchy-" + std::to_string(workgroup_) + ".spirv";
    shader_ = renderGraph->createShader({.name = passName + ".Shader", .src = os::io::readRelativeFile(path), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    const std::string entryPoint = entry();
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entryPoint.c_str(), .layout = layout_});
  }

  Stage stage_{};
  uint32_t level_ = 0u;
  uint32_t passSlot_ = 0u;
  TerrainGPUV2RasterHierarchyConfigGPU capacity_{};
  uint32_t workgroup_ = 64u;
  uint32_t lanes_ = 1u;
  Buffers buffers_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
};

} // namespace voxel::svraster::rendering
