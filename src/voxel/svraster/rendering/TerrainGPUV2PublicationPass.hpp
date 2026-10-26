#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2PublicationPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Validate,
    Commit,
  };

  struct Buffers
  {
    ::rendering::Buffer batch;
    ::rendering::Buffer requests;
    ::rendering::Buffer members;
    ::rendering::Buffer payloadResults;
    ::rendering::Buffer groups;
    ::rendering::Buffer owners;
    ::rendering::Buffer results;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUV2PublicationPass(const Stage stage, const TerrainGPUV2PublicationCapacities capacities, const Buffers buffers) : stage_(stage), capacities_(capacities), buffers_(buffers)
  {
    if (capacities.requests == 0u || capacities.members == 0u || capacities.payloadResults == 0u || capacities.groups == 0u || capacities.owners == 0u || !buffers.batch.isValid() || !buffers.requests.isValid() ||
        !buffers.members.isValid() || !buffers.payloadResults.isValid() || !buffers.groups.isValid() || !buffers.owners.isValid() || !buffers.results.isValid() || !buffers.indirectArgs.isValid())
      throw std::invalid_argument("terrain GPU V2 publication pass is invalid");
  }

  ~TerrainGPUV2PublicationPass() override
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
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU));
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
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 7u; ++binding)
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
             {.bufferView = view(buffers_.batch, sizeof(TerrainGPUV2PublicationBatchGPU), read), .binding = 0u},
             {.bufferView = view(buffers_.requests, uint64_t{capacities_.requests} * sizeof(TerrainGPUV2PublicationRequestGPU), read), .binding = 1u},
             {.bufferView = view(buffers_.members, uint64_t{capacities_.members} * sizeof(TerrainGPUV2PublicationMemberGPU), read), .binding = 2u},
             {.bufferView = view(buffers_.payloadResults, uint64_t{capacities_.payloadResults} * sizeof(TerrainGPUV2PayloadResultGPU), read), .binding = 3u},
             {.bufferView = view(buffers_.groups, uint64_t{capacities_.groups} * sizeof(TerrainGPUV2OwnershipGroupStateGPU), rw), .binding = 4u},
             {.bufferView = view(buffers_.owners, uint64_t{capacities_.owners} * sizeof(TerrainGPUV2PublishedOwnerGPU), rw), .binding = 5u},
             {.bufferView = view(buffers_.results, uint64_t{capacities_.requests} * sizeof(TerrainGPUV2PublicationResultGPU), rw), .binding = 6u},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + ".Shader", .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/terrain-gpu-v2-publication.spirv"), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    const char *entry = stage_ == Stage::Validate ? "validateTerrainGPUV2Publication" : "commitTerrainGPUV2Publication";
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entry, .layout = layout_});
  }

  Stage stage_{};
  TerrainGPUV2PublicationCapacities capacities_{};
  Buffers buffers_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
