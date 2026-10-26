#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPUV2Scan.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

struct TerrainGPUV2ScanParametersGPU
{
  uint32_t count = 0u;
  uint32_t level0Blocks = 0u;
  uint32_t level1Blocks = 0u;
  uint32_t outputCapacity = 0u;
  uint32_t compactEnabled = 0u;
  uint32_t indirectWorkgroupSize = kTerrainGPUV2CompactWorkgroupSize;
  uint32_t stage = 0u;
  uint32_t reserved = 0u;
};

static_assert(sizeof(TerrainGPUV2ScanParametersGPU) == 32u);

class TerrainGPUV2ScanPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset,
    ScanLevel0,
    ScanLevel1,
    ScanLevel2,
    AddLevel1,
    AddLevel0,
    ValidateFlags,
    Finalize,
    Emit,
  };

  struct Buffers
  {
    ::rendering::Buffer input;
    ::rendering::Buffer output;
    ::rendering::Buffer level0Sums;
    ::rendering::Buffer level0Offsets;
    ::rendering::Buffer level1Sums;
    ::rendering::Buffer level1Offsets;
    ::rendering::Buffer status;
    ::rendering::Buffer compactedIndices;
    ::rendering::Buffer indirectArgs;
    ::rendering::Buffer stageIndirectArgs;
  };

  TerrainGPUV2ScanPass(const Stage stage, const uint32_t maximumElements, const uint32_t maximumOutputElements, const Buffers buffers)
      : stage_(stage), maximumElements_(maximumElements), maximumOutputElements_(maximumOutputElements), buffers_(buffers)
  {
    if (maximumElements == 0u || maximumElements > kTerrainGPUV2ScanMaximumElements || maximumOutputElements == 0u || !buffers.input.isValid() || !buffers.output.isValid() || !buffers.level0Sums.isValid() ||
        !buffers.level0Offsets.isValid() || !buffers.level1Sums.isValid() || !buffers.level1Offsets.isValid() || !buffers.status.isValid() || !buffers.compactedIndices.isValid() || !buffers.indirectArgs.isValid() ||
        !buffers.stageIndirectArgs.isValid())
      throw std::invalid_argument("terrain GPU V2 scan pass is invalid");
  }

  ~TerrainGPUV2ScanPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (parametersBuffer_.isValid())
      renderGraph->deleteBuffer(parametersBuffer_);
  }

  void configure(const TerrainGPUV2ScanParametersGPU &parameters)
  {
    parameters_ = parameters;
    // RenderGraph command streams are reusable. Once the immutable bindings
    // exist, update only this pass's private parameter lane rather than
    // relying on command-buffer re-recording.
    if (parametersBuffer_.isValid())
      renderGraph->bufferWrite(parametersBuffer_, 0u, sizeof(parameters_), &parameters_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(parametersBuffer_, 0u, sizeof(parameters_), &parameters_);
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatchIndirect(buffers_.stageIndirectArgs, static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU));
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
    for (uint32_t binding = 0u; binding < 10u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding),
        .binding = binding,
        .type = binding == 9u ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({.name = passName + ".Layout", .groups = {std::move(group)}});
    parametersBuffer_ = renderGraph->createBuffer({
      .name = passName + ".Parameters",
      .size = sizeof(TerrainGPUV2ScanParametersGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false,
    });
    const uint32_t level0Capacity = std::max(1u, (maximumElements_ + 255u) / 256u);
    const uint32_t level1Capacity = std::max(1u, (level0Capacity + 255u) / 256u);
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups",
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.input, uint64_t{maximumElements_} * sizeof(uint32_t), read), .binding = 0u},
             {.bufferView = view(buffers_.output, uint64_t{maximumElements_} * sizeof(uint32_t), rw), .binding = 1u},
             {.bufferView = view(buffers_.level0Sums, uint64_t{level0Capacity} * sizeof(uint32_t), rw), .binding = 2u},
             {.bufferView = view(buffers_.level0Offsets, uint64_t{level0Capacity} * sizeof(uint32_t), rw), .binding = 3u},
             {.bufferView = view(buffers_.level1Sums, uint64_t{level1Capacity} * sizeof(uint32_t), rw), .binding = 4u},
             {.bufferView = view(buffers_.level1Offsets, uint64_t{level1Capacity} * sizeof(uint32_t), rw), .binding = 5u},
             {.bufferView = view(buffers_.status, sizeof(TerrainGPUV2ScanStatusGPU), rw), .binding = 6u},
             {.bufferView = view(buffers_.compactedIndices, uint64_t{maximumOutputElements_} * sizeof(uint32_t), rw), .binding = 7u},
             {.bufferView = view(buffers_.indirectArgs, sizeof(IndirectDispatchArgsGPU), rw), .binding = 8u},
             {.bufferView = view(parametersBuffer_, sizeof(TerrainGPUV2ScanParametersGPU), ::rendering::AccessPattern::UNIFORM_READ), .binding = 9u},
           }}}});
    shader_ =
        renderGraph->createShader({.name = passName + ".Shader", .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/terrain-gpu-v2-scan.spirv"), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entry(), .layout = layout_});
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2Scan";
    case Stage::ScanLevel0:
    case Stage::ScanLevel1:
    case Stage::ScanLevel2:
      return "scanTerrainGPUV2Blocks";
    case Stage::AddLevel1:
    case Stage::AddLevel0:
      return "addTerrainGPUV2ScanBlocks";
    case Stage::ValidateFlags:
      return "validateTerrainGPUV2Compaction";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2Scan";
    case Stage::Emit:
      return "emitTerrainGPUV2Compaction";
    }
    return "";
  }

  Stage stage_{};
  uint32_t maximumElements_ = 0u;
  uint32_t maximumOutputElements_ = 0u;
  Buffers buffers_{};
  TerrainGPUV2ScanParametersGPU parameters_{};
  ::rendering::Buffer parametersBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
