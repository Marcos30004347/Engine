#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace virtualgeometry::gpgpu
{

// Folds the hardware draw's depth and payload attachments and the software
// rasterizer's per-phase buffers into one visibility key per pixel. See
// virtualgeometry-visibility-merge-cs.wgsl.
class VirtualGeometryVisibilityMergePass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    // Software phases to merge, 0..2. Buffers for phases beyond this count
    // may be invalid.
    uint32_t softwarePhases = 0u;
  };

  struct SoftwareInputs
  {
    ::rendering::Buffer depthA{};
    ::rendering::Buffer payloadA{};
    ::rendering::Buffer depthB{};
    ::rendering::Buffer payloadB{};
  };

  struct Uniforms
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t softwarePhases = 0u;
    uint32_t padding = 0u;
  };
  static_assert(sizeof(Uniforms) == 16u);

  VirtualGeometryVisibilityMergePass(
      ::rendering::Texture hardwareDepth,
      ::rendering::Texture hardwarePayload,
      SoftwareInputs software,
      Settings settings)
      : hardwareDepth_(hardwareDepth), hardwarePayload_(hardwarePayload), software_(software), settings_(settings)
  {
  }

  ~VirtualGeometryVisibilityMergePass() override
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
    if (keysBuffer_.isValid())
      renderGraph->deleteBuffer(keysBuffer_);
    if (dummyBuffer_.isValid())
      renderGraph->deleteBuffer(dummyBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.viewportWidth + 7u) / 8u),
        std::max(1u, (settings_.viewportHeight + 7u) / 8u),
        1u);
    commandBuffer.cmdMemoryBarrier(
        ::rendering::PipelineStage::ALL_COMMANDS,
        ::rendering::PipelineStage::ALL_COMMANDS,
        ::rendering::AccessPattern::SHADER_WRITE | ::rendering::AccessPattern::MEMORY_WRITE,
        ::rendering::AccessPattern::MEMORY_READ | ::rendering::AccessPattern::SHADER_READ);
  }

  // Two u32 per pixel: depth bits, payload.
  const ::rendering::Buffer &getVisibilityKeysBuffer() const { return keysBuffer_; }
  uint64_t visibilityKeysBytes() const { return keysBytes_; }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    using namespace ::rendering;

    const uint64_t pixelCount = std::max<uint64_t>(1u, uint64_t{settings_.viewportWidth} * settings_.viewportHeight);
    keysBytes_ = pixelCount * sizeof(uint32_t) * 2u;
    keysBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_VisibilityKeys.buffer",
      .size = keysBytes_,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    dummyBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Dummy.buffer",
      .size = 16u,
      .usage = BufferUsage::BufferUsage_Storage,
    });
    uniformBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Uniforms.buffer",
      .size = sizeof(Uniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });
    Uniforms uniforms{};
    uniforms.viewportWidth = settings_.viewportWidth;
    uniforms.viewportHeight = settings_.viewportHeight;
    uniforms.softwarePhases = std::min(settings_.softwarePhases, 2u);
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);

    const auto storage = [](const char *name, const uint32_t binding)
    {
      return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      };
    };
    layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups = {BindingGroupLayout{
        .buffers = {
          {.name = "uniforms", .binding = 0u, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
          storage("softwareDepthA", 3u),
          storage("softwarePayloadA", 4u),
          storage("softwareDepthB", 5u),
          storage("softwarePayloadB", 6u),
          storage("visibilityKeys", 7u),
        },
        .textures = {
          {.name = "hardwareDepthTexture", .binding = 1u, .visibility = BindingVisibility::BindingVisibility_Compute},
          {.name = "hardwarePayloadTexture", .binding = 2u, .visibility = BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });

    const auto softwareView = [&](const Buffer &buffer)
    {
      const Buffer &bound = buffer.isValid() ? buffer : dummyBuffer_;
      return BufferView{
        .buffer = bound, .offset = 0u, .size = renderGraph->getBufferSize(bound), .access = AccessPattern::SHADER_READ};
    };
    groups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = AccessPattern::UNIFORM_READ}, .binding = 0u},
          {.bufferView = softwareView(settings_.softwarePhases >= 1u ? software_.depthA : Buffer{}), .binding = 3u},
          {.bufferView = softwareView(settings_.softwarePhases >= 1u ? software_.payloadA : Buffer{}), .binding = 4u},
          {.bufferView = softwareView(settings_.softwarePhases >= 2u ? software_.depthB : Buffer{}), .binding = 5u},
          {.bufferView = softwareView(settings_.softwarePhases >= 2u ? software_.payloadB : Buffer{}), .binding = 6u},
          {.bufferView = {.buffer = keysBuffer_, .offset = 0u, .size = keysBytes_, .access = AccessPattern::SHADER_WRITE}, .binding = 7u},
        },
        .textures = {
          {.textureView = {
             .texture = hardwareDepth_,
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
          {.textureView = {
             .texture = hardwarePayload_,
             .index = 0u,
             .flags = ImageAspectFlags::Color,
             .baseMipLevel = 0u,
             .levelCount = 1u,
             .baseArrayLayer = 0u,
             .layerCount = 1u,
             .access = AccessPattern::SHADER_READ,
             .layout = ResourceLayout::SHADER_READ_ONLY,
           },
           .binding = 2u},
        },
      }},
    });

    shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-visibility-merge-cs.spirv"),
      .type = ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{.name = passName + "_Pipeline", .shader = shader_, .entry = "mergeVisibility", .layout = layout_});
  }

  ::rendering::Texture hardwareDepth_{};
  ::rendering::Texture hardwarePayload_{};
  SoftwareInputs software_{};
  Settings settings_{};
  uint64_t keysBytes_ = 0u;
  ::rendering::Buffer keysBuffer_{};
  ::rendering::Buffer dummyBuffer_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace virtualgeometry::gpgpu
