#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelDepthPresentPass final : public ::rendering::Pass
{
public:
  SparseVoxelDepthPresentPass(
      const ::rendering::Texture depth,
      const ::rendering::Texture output,
      const uint32_t width,
      const uint32_t height,
      const bool reverseZ)
      : depth_(depth), output_(output), uniforms_{width, height, reverseZ ? 1u : 0u, 0u}
  {
  }

  ~SparseVoxelDepthPresentPass() override
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

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), uniforms_.data());
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (uniforms_[0] + 7u) / 8u),
        std::max(1u, (uniforms_[1] + 7u) / 8u),
        1u);
  }

private:
  static ::rendering::TextureView sampledDepth(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  static ::rendering::TextureView storageColor(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(uniforms_), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {
             {.name = "depthTexture", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .storageTextures = {
             {.name = "outputTexture", .binding = 2u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(uniforms_), .access = ::rendering::AccessPattern::UNIFORM_READ}},
           },
           .textures = {{.textureView = sampledDepth(depth_), .binding = 1u}},
           .storageTextures = {{.textureView = storageColor(output_), .binding = 2u}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-depth-present.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "presentVoxelDepth", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture depth_{};
  ::rendering::Texture output_{};
  std::array<uint32_t, 4u> uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
