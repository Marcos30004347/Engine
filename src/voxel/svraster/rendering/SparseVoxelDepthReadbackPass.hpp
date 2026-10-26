#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelDepthReadbackPass final : public ::rendering::Pass
{
public:
  SparseVoxelDepthReadbackPass(
      const ::rendering::Texture depth,
      const ::rendering::Buffer output,
      const SparseVoxelRasterUniformsGPU uniforms)
      : depth_(depth), output_(output), uniforms_(uniforms)
  {
  }

  ~SparseVoxelDepthReadbackPass() override
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
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (uniforms_.viewportTiles[0] + 7u) / 8u),
        std::max(1u, (uniforms_.viewportTiles[1] + 7u) / 8u),
        1u);
  }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "outputDepth", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {
             {.name = "depthTexture", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});
    const ::rendering::TextureView sampledDepth{
      .texture = depth_,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
    const uint64_t outputSize = std::max<uint64_t>(1u, uniforms_.viewportTiles[0] * uniforms_.viewportTiles[1]) * sizeof(float);
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelRasterUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 2u, .bufferView = {.buffer = output_, .offset = 0u, .size = outputSize, .access = ::rendering::AccessPattern::SHADER_WRITE}},
           },
           .textures = {{.textureView = sampledDepth, .binding = 1u}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-depth-readback.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "readVoxelDepth", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture depth_{};
  ::rendering::Buffer output_{};
  SparseVoxelRasterUniformsGPU uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
