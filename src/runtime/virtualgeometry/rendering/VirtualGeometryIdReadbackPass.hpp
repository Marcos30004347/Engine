#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace virtualgeometry::gpgpu
{

// Copies the virtual geometry visibility ids out of the G-buffer into a buffer
// so a test can look at the rendered picture.
//
// The packed id targets are colour attachments, and the render graph has no
// generic texture-to-buffer copy, so this one dispatch is what stands between a
// rendered frame and an image a test can assert on. Each pixel becomes
// (packedGeometryIdsLo, packedGeometryIdsHi); the Hi word carries page, group,
// cluster and triangle, which is a unique identity per rendered surface.
class VirtualGeometryIdReadbackPass final : public ::rendering::Pass
{
public:
  struct Uniforms
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
  };

  VirtualGeometryIdReadbackPass(
      const ::rendering::Texture packedGeometryIdsLo,
      const ::rendering::Texture packedGeometryIdsHi,
      const ::rendering::Buffer output,
      const uint32_t width,
      const uint32_t height)
      : packedGeometryIdsLo_(packedGeometryIdsLo),
        packedGeometryIdsHi_(packedGeometryIdsHi),
        output_(output)
  {
    uniforms_.width = width;
    uniforms_.height = height;
  }

  ~VirtualGeometryIdReadbackPass() override
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
        std::max(1u, (uniforms_.width + 7u) / 8u),
        std::max(1u, (uniforms_.height + 7u) / 8u),
        1u);
  }

private:
  static ::rendering::TextureView sampledView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
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
        {.name = passName + "_Uniforms.buffer",
         .size = sizeof(Uniforms),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push});

    const uint64_t outputSize =
        std::max<uint64_t>(1u, uniforms_.width) *
        std::max<uint64_t>(1u, uniforms_.height) * sizeof(uint32_t) * 2u;

    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "outputIds",
              .binding = 3u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = {
             {.name = "packedGeometryIdsLo",
              .binding = 1u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "packedGeometryIdsHi",
              .binding = 2u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
         }}});

    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = {.buffer = uniformBuffer_,
                             .offset = 0u,
                             .size = sizeof(Uniforms),
                             .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 3u,
              .bufferView = {.buffer = output_,
                             .offset = 0u,
                             .size = outputSize,
                             .access = ::rendering::AccessPattern::SHADER_WRITE}},
           },
           .textures = {
             {.textureView = sampledView(packedGeometryIdsLo_), .binding = 1u},
             {.textureView = sampledView(packedGeometryIdsHi_), .binding = 2u},
           },
         }}});

    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/virtualgeometry-id-readback-cs.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "readVirtualGeometryIds",
         .layout = layout_,
         .name = passName + "_Pipeline",
         .shader = shader_});
  }

  ::rendering::Texture packedGeometryIdsLo_{};
  ::rendering::Texture packedGeometryIdsHi_{};
  ::rendering::Buffer output_{};
  Uniforms uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace virtualgeometry::gpgpu
