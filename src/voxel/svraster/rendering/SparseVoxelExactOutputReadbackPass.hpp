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

// Diagnostic-only full exactness capture. The pass is registered only for a
// bounded correctness run and is absent from benchmark/interactive graphs.
class SparseVoxelExactOutputReadbackPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer microPayloads;
    ::rendering::Buffer materialSampleAddresses;
    ::rendering::Buffer coverage;
    ::rendering::Buffer output;
  };

  SparseVoxelExactOutputReadbackPass(
      const SparseVoxelGBufferPass::FrameTarget target,
      const Buffers buffers,
      const SparseVoxelRasterUniformsGPU uniforms)
      : target_(target), buffers_(buffers), uniforms_(uniforms)
  {
  }

  ~SparseVoxelExactOutputReadbackPass() override
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
        std::max(1u, (uniforms_.viewportTiles[0] + 7u) / 8u),
        std::max(1u, (uniforms_.viewportTiles[1] + 7u) / 8u), 1u);
  }

private:
  static ::rendering::TextureView sampledView(
      const ::rendering::Texture texture,
      const ::rendering::ImageAspectFlags aspect)
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

  static uint64_t pixelBytes(
      const SparseVoxelRasterUniformsGPU &uniforms, const uint64_t stride)
  {
    return std::max<uint64_t>(
               1u, uint64_t{uniforms.viewportTiles[0]} *
                       uniforms.viewportTiles[1]) *
        stride;
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer",
         .size = sizeof(SparseVoxelRasterUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push});

    std::vector<::rendering::BindingGroupLayoutTextureEntry> textureLayout;
    for (uint32_t binding = 1u; binding <= 12u; ++binding)
      textureLayout.push_back(
          {.name = "texture" + std::to_string(binding),
           .binding = binding,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(13u, "resolvedSurfaces"),
             storage(14u, "microPayloads"),
             storage(15u, "materialSampleAddresses"),
             storage(16u, "coverage"),
             storage(17u, "outputPixels"),
           },
           .textures = std::move(textureLayout),
         }}});

    const std::array<::rendering::Texture, 10> colorTextures = {
      target_.instanceId, target_.voxelId, target_.materialId,
      target_.materialUv, target_.baseColor, target_.materialProperties,
      target_.emissive, target_.geometryNormal, target_.normal,
      target_.motionVector,
    };
    std::vector<::rendering::BindingTextureInfo> textures;
    for (uint32_t index = 0u; index < colorTextures.size(); ++index)
      textures.push_back(
          {.textureView = sampledView(
               colorTextures[index], ::rendering::ImageAspectFlags::Color),
           .binding = index + 1u});
    textures.push_back(
        {.textureView = sampledView(
             target_.depth, ::rendering::ImageAspectFlags::Depth),
         .binding = 11u});
    textures.push_back(
        {.textureView = sampledView(
             target_.temporalId, ::rendering::ImageAspectFlags::Color),
         .binding = 12u});

    const uint64_t surfaceBytes = pixelBytes(
        uniforms_, sizeof(ResolvedVoxelSurfaceGPU));
    const uint64_t microBytes = pixelBytes(uniforms_, sizeof(uint32_t));
    const uint64_t addressBytes = pixelBytes(
        uniforms_, sizeof(VoxelMaterialSampleAddressGPU));
    const uint64_t outputBytes = pixelBytes(
        uniforms_, sizeof(SparseVoxelExactOutputPixelGPU));
    const ::rendering::AccessPattern read =
        ::rendering::AccessPattern::SHADER_READ;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {
                  .buffer = uniformBuffer_, .offset = 0u,
                  .size = sizeof(SparseVoxelRasterUniformsGPU),
                  .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 13u, .bufferView = {
                  .buffer = buffers_.resolvedSurfaces, .offset = 0u,
                  .size = surfaceBytes, .access = read}},
             {.binding = 14u, .bufferView = {
                  .buffer = buffers_.microPayloads, .offset = 0u,
                  .size = microBytes, .access = read}},
             {.binding = 15u, .bufferView = {
                  .buffer = buffers_.materialSampleAddresses, .offset = 0u,
                  .size = addressBytes, .access = read}},
             {.binding = 16u, .bufferView = {
                  .buffer = buffers_.coverage, .offset = 0u,
                  .size = microBytes, .access = read}},
             {.binding = 17u, .bufferView = {
                  .buffer = buffers_.output, .offset = 0u,
                  .size = outputBytes,
                  .access = ::rendering::AccessPattern::SHADER_WRITE}},
           },
           .textures = std::move(textures),
         }}});

    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/"
             "sparse-voxel-exact-output-readback.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "readSparseVoxelExactOutput", .layout = layout_,
         .name = passName + "_Pipeline", .shader = shader_});
  }

  SparseVoxelGBufferPass::FrameTarget target_{};
  Buffers buffers_{};
  SparseVoxelRasterUniformsGPU uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
