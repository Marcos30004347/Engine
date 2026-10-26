#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

// Reduces one globally anchored occupancy level. Register one pass per
// hierarchy level in bottom-up order and retain the packed mip buffer between
// frames. Each pass rewrites only the parent word masks supplied by update().
class SparseVoxelOccupancyMipPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    FromBase,
    FromMip,
    FullFromBase,
    FullFromMip,
  };

  struct Buffers
  {
    ::rendering::Buffer baseWords;
    ::rendering::Buffer mipWords;
    ::rendering::Buffer dispatchArgs;
  };

  SparseVoxelOccupancyMipPass(const Kernel kernel, const Buffers buffers, const uint64_t baseWordCount, const uint64_t mipWordCount, const uint32_t dirtyWordCapacity, const uint32_t mipIndex)
      : kernel_(kernel), buffers_(buffers), baseWordCount_(baseWordCount), mipWordCount_(mipWordCount), dirtyWordCapacity_(std::max(1u, dirtyWordCapacity)), mipIndex_(mipIndex)
  {
  }

  ~SparseVoxelOccupancyMipPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (dirtyWords_.isValid())
      renderGraph->deleteBuffer(dirtyWords_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  bool update(SparseVoxelOccupancyMipBuildGPU build, const std::vector<SparseVoxelOccupancyDirtyWordGPU> &dirtyWords)
  {
    if (dirtyWords.size() > dirtyWordCapacity_)
      return false;
    build.dirtyWordCount = static_cast<uint32_t>(dirtyWords.size());
    build_ = build;
    pendingDirtyWords_ = dirtyWords;
    if (uniforms_.isValid())
    {
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(build_), &build_);
      if (!pendingDirtyWords_.empty())
        renderGraph->bufferWrite(dirtyWords_, 0u, pendingDirtyWords_.size() * sizeof(SparseVoxelOccupancyDirtyWordGPU), pendingDirtyWords_.data());
    }
    return true;
  }

  bool updateFull(
      SparseVoxelOccupancyMipBuildGPU build,
      const bool enabled)
  {
    build.dirtyWordCount =
        enabled ? build.parentLevel.wordCount : 0u;
    if (build.dirtyWordCount > dirtyWordCapacity_)
      return false;
    build_ = build;
    pendingDirtyWords_.clear();
    if (uniforms_.isValid())
      renderGraph->bufferWrite(
          uniforms_, 0u, sizeof(build_), &build_);
    return true;
  }

  uint32_t dirtyWordCapacity() const
  {
    return dirtyWordCapacity_;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(build_), &build_);
    if (!pendingDirtyWords_.empty())
      renderGraph->bufferWrite(dirtyWords_, 0u, pendingDirtyWords_.size() * sizeof(SparseVoxelOccupancyDirtyWordGPU), pendingDirtyWords_.data());
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::FullFromBase ||
        kernel_ == Kernel::FullFromMip)
      commandBuffer.cmdDispatchIndirect(
          buffers_.dispatchArgs,
          uint64_t{5u + mipIndex_} *
              sizeof(IndirectDispatchArgsGPU));
    else
      commandBuffer.cmdDispatch(
          std::max(1u, (dirtyWordCapacity_ + 63u) / 64u),
          1u,
          1u);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    const uint32_t dirtyStorageCapacity =
        kernel_ == Kernel::FullFromBase ||
            kernel_ == Kernel::FullFromMip
        ? 1u
        : dirtyWordCapacity_;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(build_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    dirtyWords_ = renderGraph->createBuffer({
      .name = passName + "_DirtyWords",
      .size = uint64_t{dirtyStorageCapacity} *
          sizeof(SparseVoxelOccupancyDirtyWordGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });

    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "mipBuild", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyWords", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "baseWords", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "mipWords", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });

    const auto readWrite = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(uniforms_, sizeof(build_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 1u, .bufferView = view(dirtyWords_, uint64_t{dirtyStorageCapacity} * sizeof(SparseVoxelOccupancyDirtyWordGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 2u, .bufferView = view(buffers_.baseWords, baseWordCount_ * sizeof(uint32_t), readWrite)},
              {.binding = 3u, .bufferView = view(buffers_.mipWords, mipWordCount_ * sizeof(uint32_t), readWrite)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-mips.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = "buildMipFromMip";
    if (kernel_ == Kernel::FromBase)
      entry = "buildMipFromBase";
    else if (kernel_ == Kernel::FullFromBase)
      entry = "buildFullMipFromBase";
    else if (kernel_ == Kernel::FullFromMip)
      entry = "buildFullMipFromMip";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Kernel kernel_{};
  Buffers buffers_{};
  uint64_t baseWordCount_ = 0u;
  uint64_t mipWordCount_ = 0u;
  uint32_t dirtyWordCapacity_ = 1u;
  uint32_t mipIndex_ = 0u;
  SparseVoxelOccupancyMipBuildGPU build_{};
  std::vector<SparseVoxelOccupancyDirtyWordGPU> pendingDirtyWords_;
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer dirtyWords_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
