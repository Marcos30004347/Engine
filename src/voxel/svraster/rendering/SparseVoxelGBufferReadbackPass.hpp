#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/SparseVoxelGBufferPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelGBufferReadbackPass final : public ::rendering::Pass
{
public:
  SparseVoxelGBufferReadbackPass(
      const SparseVoxelGBufferPass::FrameTarget target,
      const ::rendering::Buffer output,
      const SparseVoxelRasterUniformsGPU uniforms)
      : target_(target), output_(output), uniforms_(uniforms)
  {
  }

  ~SparseVoxelGBufferReadbackPass() override
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
  static ::rendering::TextureView sampledView(const ::rendering::Texture texture, const ::rendering::ImageAspectFlags aspect)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = aspect,
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
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});

    std::vector<::rendering::BindingGroupLayoutTextureEntry> textureLayout;
    for (uint32_t binding = 1u; binding <= 11u; ++binding)
      textureLayout.push_back({.name = "texture" + std::to_string(binding), .binding = binding, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "outputPixels", .binding = 12u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = std::move(textureLayout),
         }}});

    const std::array<::rendering::Texture, 10> colorTextures = {
      target_.instanceId,
      target_.voxelId,
      target_.materialId,
      target_.materialUv,
      target_.baseColor,
      target_.materialProperties,
      target_.emissive,
      target_.geometryNormal,
      target_.normal,
      target_.motionVector,
    };
    std::vector<::rendering::BindingTextureInfo> textures;
    for (uint32_t index = 0u; index < colorTextures.size(); ++index)
      textures.push_back({.textureView = sampledView(colorTextures[index], ::rendering::ImageAspectFlags::Color), .binding = index + 1u});
    textures.push_back({.textureView = sampledView(target_.depth, ::rendering::ImageAspectFlags::Depth), .binding = 11u});

    const uint64_t outputSize = std::max<uint64_t>(1u, uniforms_.viewportTiles[0] * uniforms_.viewportTiles[1]) * sizeof(SparseVoxelGBufferPixelGPU);
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelRasterUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 12u, .bufferView = {.buffer = output_, .offset = 0u, .size = outputSize, .access = ::rendering::AccessPattern::SHADER_WRITE}},
           },
           .textures = std::move(textures),
         }}});

    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-gbuffer-readback.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "readVoxelGBuffer", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  SparseVoxelGBufferPass::FrameTarget target_{};
  ::rendering::Buffer output_{};
  SparseVoxelRasterUniformsGPU uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
