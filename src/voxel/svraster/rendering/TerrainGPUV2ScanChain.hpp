#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ScanPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUV2ScanChain final
{
public:
  static constexpr uint32_t kPassCount = 9u;

  TerrainGPUV2ScanChain(::rendering::RenderGraph &graph, uint32_t &passIndex, const uint32_t maximumElements, const uint32_t maximumOutputElements, std::string name = "TerrainGPUV2.Scan")
      : graph_(&graph), maximumElements_(maximumElements), maximumOutputElements_(maximumOutputElements), name_(std::move(name))
  {
    if (maximumElements == 0u || maximumElements > kTerrainGPUV2ScanMaximumElements || maximumOutputElements == 0u)
      throw std::invalid_argument("terrain GPU V2 scan chain capacity is invalid");
    const uint32_t level0 = std::max(1u, (maximumElements + 255u) / 256u);
    const uint32_t level1 = std::max(1u, (level0 + 255u) / 256u);
    input_ = storage(".Input", uint64_t{maximumElements} * 4u, ::rendering::BufferUsage::BufferUsage_Push);
    output_ = storage(".Output", uint64_t{maximumElements} * 4u, ::rendering::BufferUsage::BufferUsage_CopySrc);
    level0Sums_ = storage(".Level0Sums", uint64_t{level0} * 4u);
    level0Offsets_ = storage(".Level0Offsets", uint64_t{level0} * 4u);
    level1Sums_ = storage(".Level1Sums", uint64_t{level1} * 4u);
    level1Offsets_ = storage(".Level1Offsets", uint64_t{level1} * 4u);
    status_ = storage(".Status", sizeof(TerrainGPUV2ScanStatusGPU), ::rendering::BufferUsage::BufferUsage_CopySrc);
    compacted_ = storage(".Compacted", uint64_t{maximumOutputElements} * 4u, ::rendering::BufferUsage::BufferUsage_CopySrc | ::rendering::BufferUsage::BufferUsage_Push);
    indirect_ = storage(".Indirect", sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_CopySrc | ::rendering::BufferUsage::BufferUsage_Indirect);
    stageIndirect_ = storage(".StageIndirect", kPassCount * sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push);
    const TerrainGPUV2ScanPass::Buffers buffers{input_, output_, level0Sums_, level0Offsets_, level1Sums_, level1Offsets_, status_, compacted_, indirect_, stageIndirect_};
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
      passes_[stage] =
          graph_->registerPass<TerrainGPUV2ScanPass>(name_ + ".Stage" + std::to_string(stage), passIndex++, static_cast<TerrainGPUV2ScanPass::Stage>(stage), maximumElements_, maximumOutputElements_, buffers);
    update({}, false, maximumOutputElements_);
  }

  ~TerrainGPUV2ScanChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(indirect_);
    graph_->deleteBuffer(stageIndirect_);
    graph_->deleteBuffer(compacted_);
    graph_->deleteBuffer(status_);
    graph_->deleteBuffer(level1Offsets_);
    graph_->deleteBuffer(level1Sums_);
    graph_->deleteBuffer(level0Offsets_);
    graph_->deleteBuffer(level0Sums_);
    graph_->deleteBuffer(output_);
    graph_->deleteBuffer(input_);
  }

  bool update(const std::vector<uint32_t> &input, const bool compact, const uint32_t outputCapacity)
  {
    if (input.size() > maximumElements_ || outputCapacity > maximumOutputElements_)
      return false;
    TerrainGPUV2ScanParametersGPU parameters{};
    parameters.count = static_cast<uint32_t>(input.size());
    parameters.level0Blocks = (parameters.count + 255u) / 256u;
    parameters.level1Blocks = (parameters.level0Blocks + 255u) / 256u;
    parameters.outputCapacity = outputCapacity;
    parameters.compactEnabled = compact ? 1u : 0u;
    if (!input.empty())
      graph_->bufferWrite(input_, 0u, input.size() * sizeof(uint32_t), const_cast<uint32_t *>(input.data()));
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
    {
      auto stageParameters = parameters;
      if (stage >= 1u && stage <= 5u)
        stageParameters.stage = stage - 1u;
      passes_[stage]->configure(stageParameters);
    }
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    for (auto &args : indirect)
      args = {1u, 1u, 1u, 0u};
    indirect[1].x = std::max(1u, parameters.level0Blocks);
    indirect[2].x = std::max(1u, parameters.level1Blocks);
    indirect[4].x = std::max(1u, parameters.level1Blocks);
    indirect[5].x = std::max(1u, parameters.level0Blocks);
    indirect[6].x = std::max(1u, (parameters.count + 63u) / 64u);
    indirect[8].x = indirect[6].x;
    stageIndirectCPU_ = indirect;
    graph_->bufferWrite(stageIndirect_, 0u, sizeof(indirect), indirect.data());
    count_ = parameters.count;
    compactEnabled_ = compact;
    return true;
  }

  ::rendering::Buffer output() const
  {
    return output_;
  }
  ::rendering::Buffer compactedIndices() const
  {
    return compacted_;
  }
  ::rendering::Buffer status() const
  {
    return status_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirect_;
  }
  uint32_t count() const
  {
    return count_;
  }
  bool compactEnabled() const
  {
    return compactEnabled_;
  }

  void setEnabled(const bool enabled)
  {
    const auto indirect = enabled ? stageIndirectCPU_ : std::array<IndirectDispatchArgsGPU, kPassCount>{};
    if (enabled)
      graph_->bufferWrite(stageIndirect_, 0u, sizeof(indirect), const_cast<IndirectDispatchArgsGPU *>(indirect.data()));
    else
    {
      auto idle = indirect;
      for (auto &args : idle)
        args = {0u, 1u, 1u, 0u};
      graph_->bufferWrite(stageIndirect_, 0u, sizeof(idle), idle.data());
    }
  }

  ::rendering::Buffer stageIndirectArgs() const
  {
    return stageIndirect_;
  }

private:
  ::rendering::Buffer storage(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage extra = ::rendering::BufferUsage::BufferUsage_None)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra});
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  uint32_t maximumElements_ = 0u;
  uint32_t maximumOutputElements_ = 0u;
  uint32_t count_ = 0u;
  bool compactEnabled_ = false;
  std::string name_{};
  ::rendering::Buffer input_{};
  ::rendering::Buffer output_{};
  ::rendering::Buffer level0Sums_{};
  ::rendering::Buffer level0Offsets_{};
  ::rendering::Buffer level1Sums_{};
  ::rendering::Buffer level1Offsets_{};
  ::rendering::Buffer status_{};
  ::rendering::Buffer compacted_{};
  ::rendering::Buffer indirect_{};
  ::rendering::Buffer stageIndirect_{};
  std::array<IndirectDispatchArgsGPU, kPassCount> stageIndirectCPU_{};
  std::array<TerrainGPUV2ScanPass *, kPassCount> passes_{};
};

} // namespace voxel::svraster::rendering
