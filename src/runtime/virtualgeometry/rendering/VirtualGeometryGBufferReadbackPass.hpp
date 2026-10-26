#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace virtualgeometry::gpgpu
{

// Copies the virtual geometry G-buffer attachments into a buffer for tests:
// per pixel four vec4<u32> - [depthBits, idsLo, idsHi, materialId],
// [normal.xyz, uv.x], [uv.y, baseColor.rgb] and [motion.xy, 0, 0] as f32 bit patterns. See
// virtualgeometry-gbuffer-readback-cs.wgsl.
class VirtualGeometryGBufferReadbackPass final : public ::rendering::Pass
{
public:
  static constexpr uint32_t WORDS_PER_PIXEL = 16u;

  struct Inputs
  {
    ::rendering::Texture depth{};
    ::rendering::Texture packedGeometryIdsLo{};
    ::rendering::Texture packedGeometryIdsHi{};
    ::rendering::Texture materialId{};
    ::rendering::Texture geometryNormal{};
    ::rendering::Texture materialUV{};
    ::rendering::Texture baseColor{};
    ::rendering::Texture motion{};
    // Optional resolve city albedo; read into word 14 as packed RGBA8.
    ::rendering::Texture cityAlbedo{};
  };

  struct Uniforms
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
  };

  static uint64_t outputBytes(const uint32_t width, const uint32_t height)
  {
    return std::max<uint64_t>(1u, width) * std::max<uint64_t>(1u, height) * WORDS_PER_PIXEL * sizeof(uint32_t);
  }

  VirtualGeometryGBufferReadbackPass(Inputs inputs, ::rendering::Buffer output, uint32_t width, uint32_t height)
      : inputs_(inputs), output_(output)
  {
    uniforms_.width = width;
    uniforms_.height = height;
  }

  ~VirtualGeometryGBufferReadbackPass() override
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
    if (placeholder_.isValid())
      renderGraph->deleteTexture(placeholder_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (uniforms_.width + 7u) / 8u), std::max(1u, (uniforms_.height + 7u) / 8u), 1u);
  }

private:
  static ::rendering::TextureView colorView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseMipLevel = 0u,
      .levelCount = 1u,
      .baseArrayLayer = 0u,
      .layerCount = 1u,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    using namespace ::rendering;
    uniformBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Uniforms.buffer",
      .size = sizeof(Uniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });
    const auto texture = [](const char *name, const uint32_t binding)
    {
      return BindingGroupLayoutTextureEntry{
        .name = name, .binding = binding, .visibility = BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups = {BindingGroupLayout{
        .buffers = {
          {.name = "uniforms", .binding = 0u, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
          {.name = "output", .binding = 7u, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
        },
        .textures = {
          texture("depthTexture", 1u),
          texture("packedGeometryIdsLo", 2u),
          texture("packedGeometryIdsHi", 3u),
          texture("materialIdTexture", 4u),
          texture("geometryNormalTexture", 5u),
          texture("materialUVTexture", 6u),
          texture("baseColorTexture", 8u),
          texture("motionTexture", 9u),
          texture("cityAlbedoTexture", 10u),
        },
      }},
    });
    if (!inputs_.cityAlbedo.isValid())
    {
      placeholder_ = renderGraph->createTexture(TextureInfo{
        .name = passName + "_CityAlbedoPlaceholder",
        .format = Format::Format_RGBA8Unorm,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_Sampled,
        .width = 1u,
        .height = 1u,
        .mipLevels = 1u,
      });
      inputs_.cityAlbedo = placeholder_;
    }
    groups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = AccessPattern::UNIFORM_READ}, .binding = 0u},
          {.bufferView = {.buffer = output_, .offset = 0u, .size = outputBytes(uniforms_.width, uniforms_.height), .access = AccessPattern::SHADER_WRITE}, .binding = 7u},
        },
        .textures = {
          {.textureView = {
             .texture = inputs_.depth,
             .index = 0u,
             .flags = ImageAspectFlags::Depth,
             .baseMipLevel = 0u,
             .levelCount = 1u,
             .baseArrayLayer = 0u,
             .layerCount = 1u,
             .access = AccessPattern::SHADER_READ,
             .layout = ResourceLayout::DEPTH_STENCIL_READ_ONLY,
           },
           .binding = 1u},
          {.textureView = colorView(inputs_.packedGeometryIdsLo), .binding = 2u},
          {.textureView = colorView(inputs_.packedGeometryIdsHi), .binding = 3u},
          {.textureView = colorView(inputs_.materialId), .binding = 4u},
          {.textureView = colorView(inputs_.geometryNormal), .binding = 5u},
          {.textureView = colorView(inputs_.materialUV), .binding = 6u},
          {.textureView = colorView(inputs_.baseColor), .binding = 8u},
          {.textureView = colorView(inputs_.motion), .binding = 9u},
          {.textureView = colorView(inputs_.cityAlbedo), .binding = 10u},
        },
      }},
    });
    shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-gbuffer-readback-cs.spirv"),
      .type = ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{.name = passName + "_Pipeline", .shader = shader_, .entry = "readVirtualGeometryGBuffer", .layout = layout_});
  }

  Inputs inputs_{};
  ::rendering::Texture placeholder_{};
  ::rendering::Buffer output_{};
  Uniforms uniforms_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace virtualgeometry::gpgpu
