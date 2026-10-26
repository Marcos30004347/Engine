#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelTerrainFeedbackPackPass final : public ::rendering::Pass
{
public:
  struct alignas(16) SettingsGPU
  {
    uint32_t pageCount = 0u;
    uint32_t requestWordCount = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
  };

  struct Buffers
  {
    ::rendering::Buffer requestBits;
    ::rendering::Buffer requestPriorities;
    ::rendering::Buffer requestGenerations;
    ::rendering::Buffer requestDesiredLODs;
    ::rendering::Buffer packedFeedback;
  };

  SparseVoxelTerrainFeedbackPackPass(
      const Buffers buffers, const SettingsGPU settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  ~SparseVoxelTerrainFeedbackPackPass() override
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

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(settings_), &settings_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.pageCount + 63u) / 64u), 1u, 1u);
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer, const uint64_t size,
      const ::rendering::AccessPattern access)
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
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(SettingsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility =
            ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "uniforms",
           .binding = 0u,
           .isDynamic = false,
           .type =
               ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          storage(1u, "requestBits"),
          storage(2u, "requestPriorities"),
          storage(3u, "requestGenerations"),
          storage(4u, "requestDesiredLODs"),
          storage(5u, "packedFeedback"),
        },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               uniforms_, sizeof(SettingsGPU),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               buffers_.requestBits,
               uint64_t{settings_.requestWordCount} * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.requestPriorities,
               uint64_t{settings_.pageCount} * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.requestGenerations,
               uint64_t{settings_.pageCount} * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.requestDesiredLODs,
               uint64_t{settings_.pageCount} * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.packedFeedback,
               uint64_t{settings_.pageCount} *
                   sizeof(SparseVoxelTerrainFeedbackRecordGPU),
               ::rendering::AccessPattern::SHADER_READ_WRITE)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-terrain-feedback-pack.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "packTerrainFeedback",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  SettingsGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

static_assert(
    sizeof(SparseVoxelTerrainFeedbackPackPass::SettingsGPU) == 16u);

} // namespace voxel::svraster::rendering
