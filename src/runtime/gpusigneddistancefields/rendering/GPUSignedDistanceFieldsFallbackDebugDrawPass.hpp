#pragma once

#include <cstdint>
#include <string>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFallbackDebugPreparePass.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsFallbackDebugDrawPass : public rendering::Pass
{
public:
  enum class Mode : uint32_t
  {
    TriangleId = 0u,
    VertexNormal,
    VertexAlbedo,
    VertexRoughness,
    VertexMetallic,
    VertexEmissive,
    VertexOpacity,
    VertexSurfaceFlags,
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool reverseZ = false;
    Mode mode = Mode::TriangleId;
  };

  GPUSignedDistanceFieldsFallbackDebugDrawPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsFallbackDebugPreparePass &preparePass,
      rendering::Texture outputTexture,
      rendering::Texture depthTexture,
      rendering::Format outputFormat,
      Settings settings)
      : manager_(manager),
        preparePass_(preparePass),
        outputTexture_(outputTexture),
        depthTexture_(depthTexture),
        outputFormat_(outputFormat),
        settings_(settings),
        reverseZ_(settings.reverseZ)
  {
  }

  ~GPUSignedDistanceFieldsFallbackDebugDrawPass() override
  {
    destroyResources();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
  }

  void setMode(Mode mode)
  {
    settings_.mode = mode;
    writeUniforms();
  }

  void updateRuntimeState(const math::Mat4f &viewProjection, bool reverseZ)
  {
    viewProjection_ = viewProjection;
    reverseZ_ = reverseZ;
    writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    writeUniforms();

    rendering::ColorAttachmentInfo colorAttachment{};
    colorAttachment.name = passName + "_Color";
    colorAttachment.view = makeColorView(outputTexture_);
    colorAttachment.clearValue = rendering::Color::rgba(0, 0, 0, 1);

    rendering::DepthStencilAttachmentInfo depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.name = passName + "_Depth";
    depthAttachment.view = makeDepthView(depthTexture_);
    depthAttachment.clearDepth = reverseZ_ ? 0.0f : 1.0f;
    depthAttachment.clearStencil = 0u;

    rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = rendering::Rect2D(0, 0, settings_.width, settings_.height);
    renderPass.viewport = rendering::Viewport(settings_.width, settings_.height);
    renderPass.colorAttachments = {colorAttachment};
    renderPass.depthStencilAttachment = depthAttachment;

    commandBuffer.cmdBindGraphicsPipeline(graphicsPipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    drawFallbackScene(commandBuffer);
    commandBuffer.cmdEndRenderPass();
  }

private:
  struct Uniforms
  {
    math::Mat4f viewProjection = math::Mat4f::identity();
    uint32_t pathCapacity = 0u;
    uint32_t mode = 0u;
    uint32_t _padding1 = 0u;
    uint32_t _padding2 = 0u;
  };
  static_assert(sizeof(Uniforms) % 16u == 0u, "Fallback draw uniforms must be 16-byte aligned");

  static rendering::TextureView makeColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = rendering::ResourceLayout::COLOR_ATTACHMENT,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
        .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  rendering::BindingsLayout createLayout()
  {
    return renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {
                rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "meshTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "vertices", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "indices", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "boneWeights", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "boneMatrices", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "staticInstances", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                        {.name = "skinnedInstances", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Vertex},
                    },
                },
            },
        });
  }

  rendering::BindingGroups createBindingGroups()
  {
    const auto &shared = manager_.getSharedResources();
    const auto &staticResources = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Static);
    const auto &skinnedResources = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Skinned);
    return renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = shared.meshTable, .offset = 0u, .size = shared.meshTableSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = shared.vertices, .offset = 0u, .size = shared.verticesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = shared.indices, .offset = 0u, .size = shared.indicesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = shared.boneWeights, .offset = 0u, .size = shared.boneWeightsSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = shared.boneMatrices, .offset = 0u, .size = shared.boneMatricesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = staticResources.instances, .offset = 0u, .size = staticResources.instancesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 7, .bufferView = {.buffer = skinnedResources.instances, .offset = 0u, .size = skinnedResources.instancesSize, .access = rendering::AccessPattern::SHADER_READ}},
                    },
                },
            },
        });
  }

  void initializeResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    layout_ = createLayout();
    bindingGroups_ = createBindingGroups();

    vertexShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_VertexShader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-fallback-debug-vertex.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    fragmentShader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_FragmentShader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-fallback-debug-fragment.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = vertexShader_;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = fragmentShader_;
    pipelineInfo.fragmentStage.shaderEntry = "fs_main";
    pipelineInfo.fragmentStage.colorAttatchments = {
        {
            .format = outputFormat_,
            .loadOp = rendering::LoadOp::LoadOp_Clear,
            .storeOp = rendering::StoreOp::StoreOp_Store,
            .blendMode = rendering::ColorBlendMode::Replace,
            .initialLayout = rendering::ResourceLayout::COLOR_ATTACHMENT,
            .finalLayout = rendering::ResourceLayout::COLOR_ATTACHMENT,
        },
    };
    pipelineInfo.fragmentStage.depthAttatchment = rendering::DepthAttatchment{
        .enabled = true,
        .comparison = reverseZ_ ? rendering::ComparisonOp::ComparisonOp_GreaterOrEqual : rendering::ComparisonOp::ComparisonOp_LessOrEqual,
        .format = rendering::Format::Format_Depth32Float,
        .loadOp = rendering::LoadOp::LoadOp_Clear,
        .storeOp = rendering::StoreOp::StoreOp_Store,
    };
    graphicsPipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  void destroyResources()
  {
    if (graphicsPipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(graphicsPipeline_);
    graphicsPipeline_ = {};
    if (vertexShader_.isValid())
      renderGraph->deleteShader(vertexShader_);
    vertexShader_ = {};
    if (fragmentShader_.isValid())
      renderGraph->deleteShader(fragmentShader_);
    fragmentShader_ = {};
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    bindingGroups_ = {};
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    layout_ = {};
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    uniformBuffer_ = {};
  }

  void writeUniforms()
  {
    if (!uniformBuffer_.isValid())
      return;

    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection_;
    uniforms.pathCapacity = preparePass_.getPathDrawCapacity();
    uniforms.mode = static_cast<uint32_t>(settings_.mode);
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  void drawFallbackScene(rendering::CommandRecorder &commandBuffer)
  {
    const uint32_t pathCapacity = preparePass_.getPathDrawCapacity();
    if (pathCapacity == 0u)
    {
      return;
    }

    const rendering::Buffer indirectBuffer = preparePass_.getIndirectBuffer();
    const uint32_t stride = sizeof(GPUSignedDistanceFieldsFallbackDebugPreparePass::DrawIndirectCommandGPU);
    const rendering::BufferView indirectView{
        .buffer = indirectBuffer,
        .offset = 0u,
        .size = renderGraph->getBufferSize(indirectBuffer),
        .access = rendering::AccessPattern::INDIRECT_COMMAND_READ,
    };

    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);

    const bool multiDrawIndirect =
        (renderGraph->getRHI()->getFeatures() & rendering::DeviceFeatures::DeviceFeatures_MultiDrawIndirect) != 0u;
    if (multiDrawIndirect)
    {
      commandBuffer.cmdDrawIndirect(indirectView, 0u, pathCapacity, stride);
      commandBuffer.cmdDrawIndirect(indirectView, pathCapacity * stride, pathCapacity, stride);
      return;
    }

    for (uint32_t commandIndex = 0u; commandIndex < pathCapacity; ++commandIndex)
    {
      commandBuffer.cmdDrawIndirect(indirectView, commandIndex * stride, 1u, stride);
    }
    for (uint32_t commandIndex = 0u; commandIndex < pathCapacity; ++commandIndex)
    {
      commandBuffer.cmdDrawIndirect(indirectView, (pathCapacity + commandIndex) * stride, 1u, stride);
    }
  }

  GPUSignedDistanceFieldsManager &manager_;
  GPUSignedDistanceFieldsFallbackDebugPreparePass &preparePass_;
  rendering::Texture outputTexture_;
  rendering::Texture depthTexture_;
  rendering::Format outputFormat_;
  Settings settings_{};
  bool enabled_ = false;
  bool reverseZ_ = false;
  math::Mat4f viewProjection_ = math::Mat4f::identity();

  rendering::Buffer uniformBuffer_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader vertexShader_;
  rendering::Shader fragmentShader_;
  rendering::GraphicsPipeline graphicsPipeline_;
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
