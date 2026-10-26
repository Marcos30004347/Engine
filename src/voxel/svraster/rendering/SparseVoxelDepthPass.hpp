#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

// Material-free terminal pass for prepasses, HZB sources, and shadow depth.
// The accepted/resolved surface contract is shared with SparseVoxelGBufferPass.
class SparseVoxelDepthPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer instances;
    ::rendering::Buffer resolvedSurfaces;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
    uint32_t instanceCapacity = 1u;
    float clearDepth = 0.0f;
    ::rendering::LoadOp loadOp = ::rendering::LoadOp::LoadOp_Clear;
    ::rendering::StoreOp storeOp = ::rendering::StoreOp::StoreOp_Store;
    ::rendering::ComparisonOp comparison = ::rendering::ComparisonOp::ComparisonOp_Always;
  };

  SparseVoxelDepthPass(const Buffers buffers, const ::rendering::Texture depth, const Settings settings)
      : buffers_(buffers), depth_(depth), settings_(settings)
  {
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  ~SparseVoxelDepthPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(pipeline_);
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
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);

    ::rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = ::rendering::Rect2D(0, 0, settings_.uniforms.viewportTiles[0], settings_.uniforms.viewportTiles[1]);
    renderPass.viewport = ::rendering::Viewport(settings_.uniforms.viewportTiles[0], settings_.uniforms.viewportTiles[1]);
    renderPass.colorAttachments = {};
    renderPass.depthStencilAttachment = {
      .enabled = true,
      .name = passName + "_Depth",
      .view = depthView(),
      .clearDepth = settings_.clearDepth,
      .clearStencil = 0u,
    };

    commandBuffer.cmdBindGraphicsPipeline(pipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDraw(6u, 1u, 0u, 0u);
    commandBuffer.cmdEndRenderPass();
  }

private:
  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  ::rendering::TextureView depthView() const
  {
    return {
      .texture = depth_,
      .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
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
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "instances", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "resolvedSurfaces", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           }}}});

    const uint32_t pixelCapacity = std::max(1u, settings_.uniforms.viewportTiles[0] * settings_.uniforms.viewportTiles[1]);
    const auto bufferView = [](
                                const ::rendering::Buffer buffer,
                                const uint64_t size,
                                const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = bufferView(uniformBuffer_, sizeof(SparseVoxelRasterUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = bufferView(buffers_.instances, capacityBytes(settings_.instanceCapacity, sizeof(SparseVoxelInstanceGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u, .bufferView = bufferView(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU)), ::rendering::AccessPattern::SHADER_READ)},
           }}}});

    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-depth.spirv"), .type = ::rendering::ShaderType::SpirV});

    ::rendering::DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.comparison = settings_.comparison;
    depthAttachment.format = ::rendering::Format::Format_Depth32Float;
    depthAttachment.loadOp = settings_.loadOp;
    depthAttachment.storeOp = settings_.storeOp;

    ::rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = shader_;
    pipelineInfo.vertexStage.shaderEntry = "writeVoxelDepthVertex";
    pipelineInfo.vertexStage.cullType = ::rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = shader_;
    pipelineInfo.fragmentStage.shaderEntry = "writeVoxelDepth";
    pipelineInfo.fragmentStage.colorAttatchments = {};
    pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;
    pipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  Buffers buffers_{};
  ::rendering::Texture depth_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
