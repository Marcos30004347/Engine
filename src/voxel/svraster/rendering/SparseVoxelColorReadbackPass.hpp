#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelColorReadbackPass final : public ::rendering::Pass
{
public:
  struct alignas(16) UniformsGPU
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
  };
  static_assert(sizeof(UniformsGPU) == 16u);

  SparseVoxelColorReadbackPass(
      const ::rendering::Texture input,
      const ::rendering::Buffer output,
      const uint32_t width,
      const uint32_t height)
      : input_(input), output_(output), uniforms_{width, height, 0u, 0u}
  {
  }

  ~SparseVoxelColorReadbackPass() override
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

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (uniforms_.width + 7u) / 8u),
        std::max(1u, (uniforms_.height + 7u) / 8u), 1u);
  }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(UniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "outputPixels", .binding = 2u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {{.name = "inputTexture", .binding = 1u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
         }}});
    const uint64_t outputBytes = std::max<uint64_t>(
        sizeof(float) * 4u,
        uint64_t{uniforms_.width} * uniforms_.height * sizeof(float) * 4u);
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {
                  .buffer = uniformBuffer_, .offset = 0u,
                  .size = sizeof(UniformsGPU),
                  .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 2u, .bufferView = {
                  .buffer = output_, .offset = 0u, .size = outputBytes,
                  .access = ::rendering::AccessPattern::SHADER_WRITE}},
           },
           .textures = {{.textureView = {
                .texture = input_,
                .access = ::rendering::AccessPattern::SHADER_READ,
                .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
                .index = 0u,
                .flags = ::rendering::ImageAspectFlags::Color,
                .baseArrayLayer = 0u,
                .baseMipLevel = 0u,
                .layerCount = 1u,
                .levelCount = 1u},
              .binding = 1u}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/"
             "sparse-voxel-color-readback.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "readSparseVoxelColor", .layout = layout_,
         .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture input_{};
  ::rendering::Buffer output_{};
  UniformsGPU uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
