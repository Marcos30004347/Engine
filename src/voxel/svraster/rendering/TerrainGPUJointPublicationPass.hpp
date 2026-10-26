#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUJointPublication.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

// Phase 5 is deliberately split into validation, relocation, and record
// finalization. A rejected validation lane never writes destination payloads,
// and the final pass only exposes records for lanes whose relocation ran.
class TerrainGPUJointPublicationPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Validate = 0u,
    Relocate = 1u,
    Finalize = 2u,
  };

  struct Buffers
  {
    ::rendering::Buffer requests;
    ::rendering::Buffer rasterResults;
    ::rendering::Buffer rasterNodes;
    ::rendering::Buffer rasterOccupancy;
    ::rendering::Buffer rasterMaterialWords;
    ::rendering::Buffer rasterMicroSurfaces;
    ::rendering::Buffer rasterChildren;
    ::rendering::Buffer rasterRoots;
    ::rendering::Buffer tree64Results;
    ::rendering::Buffer tree64Nodes;
    ::rendering::Buffer results;
    ::rendering::Buffer records;
    ::rendering::Buffer outputNodes;
    ::rendering::Buffer outputOccupancy;
    ::rendering::Buffer outputMaterialWords;
    ::rendering::Buffer outputMicroSurfaces;
    ::rendering::Buffer outputChildren;
    ::rendering::Buffer outputRoots;
    ::rendering::Buffer outputTree64Nodes;
    ::rendering::Buffer outputTree64NodesPadded;
    ::rendering::Buffer outputTree64Instances;
    ::rendering::Buffer outputLODGroups;
    ::rendering::Buffer outputNodeLODGroupIndices;
    ::rendering::Buffer outputRepresentativeLODGroupIndices;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUJointPublicationPass(const Stage stage, const TerrainGPUJointPublicationCapacities capacities, const Buffers buffers) : stage_(stage), capacities_(capacities), buffers_(buffers)
  {
    const std::array<::rendering::Buffer, 25u> all{
      buffers.requests,       buffers.rasterResults, buffers.rasterNodes,       buffers.rasterOccupancy, buffers.rasterMaterialWords, buffers.rasterMicroSurfaces, buffers.rasterChildren,      buffers.rasterRoots,
      buffers.tree64Results,  buffers.tree64Nodes,   buffers.results,           buffers.records,         buffers.outputNodes,         buffers.outputOccupancy,     buffers.outputMaterialWords, buffers.outputMicroSurfaces,
      buffers.outputChildren, buffers.outputRoots,   buffers.outputTree64Nodes,
      buffers.outputTree64NodesPadded, buffers.outputTree64Instances,
      buffers.outputLODGroups, buffers.outputNodeLODGroupIndices,
      buffers.outputRepresentativeLODGroupIndices,
      buffers.indirectArgs,
    };
    uint32_t invalidBufferMask = 0u;
    for (uint32_t index = 0u; index < static_cast<uint32_t>(all.size());
         ++index)
      if (!all[index].isValid())
        invalidBufferMask |= 1u << index;
    const bool invalidBuffer = invalidBufferMask != 0u;
    if (invalidBuffer ||
        capacities.requests == 0u || capacities.requests > 65535u ||
        capacities.rasterResults == 0u || capacities.rasterNodes == 0u ||
        capacities.rasterChildren == 0u || capacities.rasterMaterials == 0u ||
        capacities.rasterRoots == 0u || capacities.tree64Results == 0u ||
        capacities.tree64Nodes == 0u || capacities.destinationNodes == 0u ||
        capacities.destinationChildren == 0u ||
        capacities.destinationMaterials == 0u ||
        capacities.destinationRoots == 0u ||
        capacities.destinationTree64Nodes == 0u ||
        capacities.destinationTree64Instances == 0u)
    {
      throw std::invalid_argument(
          "GPU terrain joint publication pass is invalid: invalidBuffer=" +
          std::to_string(invalidBuffer ? 1u : 0u) +
          " invalidBufferMask=0x" + std::to_string(invalidBufferMask) +
          " requests=" + std::to_string(capacities.requests) +
          " rasterResults=" + std::to_string(capacities.rasterResults) +
          " rasterNodes=" + std::to_string(capacities.rasterNodes) +
          " rasterChildren=" + std::to_string(capacities.rasterChildren) +
          " rasterMaterials=" + std::to_string(capacities.rasterMaterials) +
          " rasterRoots=" + std::to_string(capacities.rasterRoots) +
          " tree64Results=" + std::to_string(capacities.tree64Results) +
          " tree64Nodes=" + std::to_string(capacities.tree64Nodes) +
          " destinationNodes=" + std::to_string(capacities.destinationNodes) +
          " destinationChildren=" +
          std::to_string(capacities.destinationChildren) +
          " destinationMaterials=" +
          std::to_string(capacities.destinationMaterials) +
          " destinationRoots=" + std::to_string(capacities.destinationRoots) +
          " destinationTree64Nodes=" +
          std::to_string(capacities.destinationTree64Nodes) +
          " destinationTree64Instances=" +
          std::to_string(capacities.destinationTree64Instances));
    }
  }

  ~TerrainGPUJointPublicationPass() override
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

  bool update(const uint32_t requestCount)
  {
    if (requestCount > capacities_.requests)
      return false;
    requestCount_ = requestCount;
    *enabled_ = requestCount != 0u;
    // RenderGraph records this pass when it is registered. Upload the mutable
    // count here; writing it only from recordCommandBuffer permanently baked
    // the constructor's zero and made every later publication a no-op.
    if (uniforms_.isValid())
    {
      std::array<uint32_t, 4u> uniforms{
        requestCount_, static_cast<uint32_t>(stage_), 0u, 0u};
      renderGraph->bufferWrite(
          uniforms_, 0u, sizeof(uniforms), uniforms.data());
    }
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    std::array<uint32_t, 4u> uniforms{requestCount_, static_cast<uint32_t>(stage_), 0u, 0u};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(uniforms), uniforms.data());
    commands.cmdBindComputePipeline(pipeline_, enabled_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u, enabled_);
    commands.cmdDispatchIndirect(
        buffers_.indirectArgs,
        static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU),
        enabled_);
    // The finalize shader intentionally does not touch every relocated output.
    // Keep the publication record behind an explicit full compute barrier so
    // it can never race ahead of payload writes based only on resource usage.
    commands.cmdMemoryBarrier(enabled_);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  const char *shaderPath() const
  {
    switch (stage_)
    {
    case Stage::Validate:
      return "assets/shaders/spirv/svraster/terrain-gpu-joint-publication-validate.spirv";
    case Stage::Relocate:
      return "assets/shaders/spirv/svraster/terrain-gpu-joint-publication-relocate.spirv";
    case Stage::Finalize:
      return "assets/shaders/spirv/svraster/terrain-gpu-joint-publication-finalize.spirv";
    }
    return "";
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::Validate:
      return "validateTerrainGPUJointPublication";
    case Stage::Relocate:
      return "relocateTerrainGPUJointPublication";
    case Stage::Finalize:
      return "finalizeTerrainGPUJointPublication";
    }
    return "";
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + ".Uniforms",
      .size = sizeof(uint32_t) * 4u,
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 25u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding),
        .binding = binding,
        .type = binding == 24u ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + ".Layout",
      .groups = {std::move(group)},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = ::rendering::AccessPattern::SHADER_READ_WRITE;
    const auto materialWords = [](const uint32_t materials)
    {
      return uint64_t{(materials + 3u) / 4u} * sizeof(uint32_t);
    };
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
                                    {.bufferView = view(buffers_.requests, uint64_t{capacities_.requests} * sizeof(TerrainGPUJointPublicationRequestGPU), read), .binding = 0u},
                                    {.bufferView = view(buffers_.rasterResults, uint64_t{capacities_.rasterResults} * sizeof(TerrainGPURasterResultGPU), read), .binding = 1u},
                                    {.bufferView = view(buffers_.rasterNodes, uint64_t{capacities_.rasterNodes} * sizeof(SparseVoxelBrickNodeGPU), read), .binding = 2u},
                                    {.bufferView = view(buffers_.rasterOccupancy, uint64_t{capacities_.rasterNodes} * sizeof(SparseVoxelBrickOccupancyGPU), read), .binding = 3u},
                                    {.bufferView = view(buffers_.rasterMaterialWords, materialWords(capacities_.rasterMaterials), read), .binding = 4u},
                                    {.bufferView = view(buffers_.rasterMicroSurfaces, uint64_t{capacities_.rasterMaterials} * sizeof(uint64_t), read), .binding = 5u},
                                    {.bufferView = view(buffers_.rasterChildren, uint64_t{capacities_.rasterChildren} * sizeof(uint32_t), read), .binding = 6u},
                                    {.bufferView = view(buffers_.rasterRoots, uint64_t{capacities_.rasterRoots} * sizeof(uint32_t), read), .binding = 7u},
                                    {.bufferView = view(buffers_.tree64Results, uint64_t{capacities_.tree64Results} * sizeof(TerrainGPUTree64ResultGPU), read), .binding = 8u},
                                    {.bufferView = view(buffers_.tree64Nodes, uint64_t{capacities_.tree64Nodes} * sizeof(SparseVoxelTree64NodeGPU), read), .binding = 9u},
                                    {.bufferView = view(buffers_.results, uint64_t{capacities_.requests} * sizeof(TerrainGPUJointPublicationResultGPU), readWrite), .binding = 10u},
                                    {.bufferView = view(buffers_.records, uint64_t{capacities_.requests} * sizeof(SparseVoxelTerrainPublicationRecordGPU), readWrite), .binding = 11u},
                                    {.bufferView = view(buffers_.outputNodes, uint64_t{capacities_.destinationNodes} * sizeof(SparseVoxelBrickNodeGPU), readWrite), .binding = 12u},
                                    {.bufferView = view(buffers_.outputOccupancy, uint64_t{capacities_.destinationNodes} * sizeof(SparseVoxelBrickOccupancyGPU), readWrite), .binding = 13u},
                                    {.bufferView = view(buffers_.outputMaterialWords, materialWords(capacities_.destinationMaterials), readWrite), .binding = 14u},
                                    {.bufferView = view(buffers_.outputMicroSurfaces, uint64_t{capacities_.destinationMaterials} * sizeof(uint64_t), readWrite), .binding = 15u},
                                    {.bufferView = view(buffers_.outputChildren, uint64_t{capacities_.destinationChildren} * sizeof(uint32_t), readWrite), .binding = 16u},
                                    {.bufferView = view(buffers_.outputRoots, uint64_t{capacities_.destinationRoots} * sizeof(uint32_t), readWrite), .binding = 17u},
                                    {.bufferView = view(buffers_.outputTree64Nodes, uint64_t{capacities_.destinationTree64Nodes} * sizeof(SparseVoxelTree64NodeGPU), readWrite), .binding = 18u},
                                    {.bufferView = view(buffers_.outputTree64NodesPadded, uint64_t{capacities_.destinationTree64Nodes} * sizeof(SparseVoxelTree64NodePaddedGPU), readWrite), .binding = 19u},
                                    {.bufferView = view(buffers_.outputTree64Instances, uint64_t{capacities_.destinationTree64Instances} * sizeof(SparseVoxelTree64DynamicInstanceGPU), readWrite), .binding = 20u},
                                    {.bufferView = view(buffers_.outputLODGroups, uint64_t{capacities_.destinationNodes} * sizeof(SparseVoxelBrickLODGroupGPU), readWrite), .binding = 21u},
                                    {.bufferView = view(buffers_.outputNodeLODGroupIndices, uint64_t{capacities_.destinationNodes} * sizeof(uint32_t), readWrite), .binding = 22u},
                                    {.bufferView = view(buffers_.outputRepresentativeLODGroupIndices, uint64_t{capacities_.destinationNodes} * sizeof(uint32_t), readWrite), .binding = 23u},
                                    {.bufferView = view(uniforms_, sizeof(uint32_t) * 4u, ::rendering::AccessPattern::UNIFORM_READ), .binding = 24u},
                                  },
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + ".Shader",
      .src = os::io::readRelativeFile(shaderPath()),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + ".Pipeline",
      .shader = shader_,
      .entry = entry(),
      .layout = layout_,
    });
  }

  Stage stage_ = Stage::Validate;
  TerrainGPUJointPublicationCapacities capacities_{};
  Buffers buffers_{};
  uint32_t requestCount_ = 0u;
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
