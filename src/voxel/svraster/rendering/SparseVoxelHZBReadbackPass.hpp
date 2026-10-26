#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelHZBReadbackPass final : public ::rendering::Pass
{
public:
  struct alignas(16) Uniforms
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t outputOffset = 0u;
    uint32_t reserved = 0u;
  };

  SparseVoxelHZBReadbackPass(
      const ::rendering::Texture hzb,
      const uint32_t mip,
      const ::rendering::Buffer output,
      const uint64_t outputSize,
      const Uniforms uniforms)
      : hzb_(hzb), mip_(mip), output_(output), outputSize_(outputSize), uniforms_(uniforms)
  {
  }

  ~SparseVoxelHZBReadbackPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (uniforms_.width + 7u) / 8u),
        std::max(1u, (uniforms_.height + 7u) / 8u),
        1u);
  }

private:
  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniformBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(uniforms_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "output", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .textures = {{.name = "sourceMip", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(uniforms_), .access = ::rendering::AccessPattern::UNIFORM_READ}},
          {.binding = 2u, .bufferView = {.buffer = output_, .offset = 0u, .size = outputSize_, .access = ::rendering::AccessPattern::SHADER_WRITE}},
        },
        .textures = {{.binding = 1u, .textureView = {
          .texture = hzb_,
          .access = ::rendering::AccessPattern::SHADER_READ,
          .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
          .index = 0u,
          .flags = ::rendering::ImageAspectFlags::Color,
          .baseArrayLayer = 0u,
          .baseMipLevel = mip_,
          .layerCount = 1u,
          .levelCount = 1u,
        }}},
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-hzb-readback.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({.entry = "readHZBMip", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture hzb_{};
  uint32_t mip_ = 0u;
  ::rendering::Buffer output_{};
  uint64_t outputSize_ = 0u;
  Uniforms uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
