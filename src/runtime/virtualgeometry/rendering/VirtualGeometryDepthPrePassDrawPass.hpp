#pragma once

#include <cstring>

#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

using namespace rendering;

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryDepthPrePassDrawPass : public Pass
{
public:
  struct FrameTarget
  {
    TextureView depthView;
    Texture depthTexture;
  };

  struct Settings
  {
    uint32_t viewPortWidth = 1920;
    uint32_t viewPortHeight = 1080;
    uint32_t maxDrawnClusters = VirtualGeometryScene::MAX_VISIBLE_CLUSTERS;
    rendering::Format depthFormat = rendering::Format::Format_Depth32Float;
  };

private:
  rendering::BindingsLayout drawLayout;
  rendering::BindingGroups drawBindingGroups;
  rendering::Shader vertexShader;
  rendering::GraphicsPipeline graphicsPipeline;

  VirtualGeometryScene &scene;
  rendering::Buffer indirectBuffer;
  rendering::Buffer visibleClustesInfoBuffer;
  rendering::Buffer cullingCountersBuffer;

  rendering::Buffer uniformsBuffer;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;

  FrameTarget frameTarget;
  LoadOp depthLoadOp;
  float clearDepthValue;

  struct CullingUniforms
  {
    float view[16];
    float proj[16];
    float viewPosition[4];
    uint32_t viewport[2];
    float error;
    uint32_t instances_count;
    uint32_t clusters_count;
    float nearPlane;
    float farPlane;
    uint32_t hiZLevels;
    uint32_t cullingFlags;
  };

  static constexpr uint32_t VISIBLE_CLUSTER_COUNT_INDEX = 2u;

public:
  Settings settings;

  VirtualGeometryDepthPrePassDrawPass(
      VirtualGeometryScene &scene,
      rendering::Buffer indirectBuffer,
      rendering::Buffer visibleClustesInfoBuffer,
      rendering::Buffer cullingCountersBuffer,
      FrameTarget frameTarget,
      LoadOp depthLoadOp,
      float clearDepth,
      Settings settings)
      : scene(scene), indirectBuffer(indirectBuffer), visibleClustesInfoBuffer(visibleClustesInfoBuffer), cullingCountersBuffer(cullingCountersBuffer), frameTarget(frameTarget), depthLoadOp(depthLoadOp),
        clearDepthValue(clearDepth), settings(settings)
  {
  }

  VirtualGeometryDepthPrePassDrawPass(
      VirtualGeometryScene &scene,
      rendering::Buffer indirectBuffer,
      rendering::Buffer visibleClustesInfoBuffer,
      rendering::Buffer cullingCountersBuffer,
      FrameTarget frameTarget,
      bool shouldClearDepth,
      float clearDepth,
      Settings settings)
      : VirtualGeometryDepthPrePassDrawPass(scene, indirectBuffer, visibleClustesInfoBuffer, cullingCountersBuffer, frameTarget, shouldClearDepth ? LoadOp::LoadOp_Clear : LoadOp::LoadOp_DontCare, clearDepth, settings)
  {
  }

  void updateUniforms(const float view[16], const float proj[16], const float viewPosition[4], uint32_t viewportW, uint32_t viewportH, float nearPlane, float farPlane, uint32_t hiZLevels, float errorThreshold = 0.0f)
  {
    CullingUniforms u{};
    std::memcpy(u.view, view, sizeof(float) * 16);
    std::memcpy(u.proj, proj, sizeof(float) * 16);
    std::memcpy(u.viewPosition, viewPosition, sizeof(float) * 4);
    u.viewport[0] = viewportW;
    u.viewport[1] = viewportH;
    u.error = errorThreshold;
    u.instances_count = static_cast<uint32_t>(scene.getInstanceCount());
    u.clusters_count = 0;
    u.nearPlane = nearPlane;
    u.farPlane = farPlane;
    u.hiZLevels = hiZLevels;
    u.cullingFlags = 0u;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformsBuffer, 0, sizeof(CullingUniforms), &u);
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(getCurrentFrameOverrideUniformBufferId(), 0, sizeof(CullingUniforms), &u);
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return;

    overrides.bufferOverrides.emplace(uniformsBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  ~VirtualGeometryDepthPrePassDrawPass() override
  {
    destroyFrameOverrideBuffers();
    renderGraph->deleteGraphicsPipeline(graphicsPipeline);
    renderGraph->deleteShader(vertexShader);
    renderGraph->deleteBindingGroups(drawBindingGroups);
    renderGraph->deleteBindingsLayout(drawLayout);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(CullingUniforms),
          .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    drawLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_drawLayout.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "instances", .binding = 1, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "pageTable", .binding = 2, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "pagesBuffer", .binding = 3, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "visibleClusterInfos", .binding = 4, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "cullingCounters", .binding = 5, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "meshPartTransforms", .binding = 6, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                      },
                },
              },
        });

    const uint64_t instanceBufferSize = scene.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData);
    const uint64_t visibleClusterInfosBufferSize = renderGraph->getBufferSize(visibleClustesInfoBuffer);

    drawBindingGroups = renderGraph->createBindingGroups(
        BindingGroupsInfo{
          .layout = drawLayout,
          .name = passName + "_drawBindingGroups",
          .groups =
              {
                GroupInfo{
                  .name = "Group0",
                  .buffers =
                      {
                        {.binding = 0, .bufferView = {.buffer = uniformsBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(CullingUniforms)}},
                        {.binding = 1, .bufferView = {.buffer = scene.instanceBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = instanceBufferSize}},
                        {.binding = 2, .bufferView = {.buffer = scene.pageTableBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.pagesTableBufferSize}},
                        {.binding = 3, .bufferView = {.buffer = scene.pagesBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.pagesBufferSize}},
                        {.binding = 4, .bufferView = {.buffer = visibleClustesInfoBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = visibleClusterInfosBufferSize}},
                        {.binding = 5, .bufferView = {.buffer = cullingCountersBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = 16 * sizeof(uint32_t)}},
                        {.binding = 6, .bufferView = {.buffer = scene.meshPartTransformsBuffer, .access = AccessPattern::SHADER_READ, .offset = 0, .size = scene.meshPartTransformsBufferSize}},
                      },
                },
              },
        });

    const bool drawIndirectCountDisabled = !(renderGraph->getRHI()->getFeatures() & DeviceFeatures::DeviceFeatures_DrawIndirectCount);
    const char *vsSpirv = drawIndirectCountDisabled
                              ? "assets/shaders/spirv/virtualgeometry-meshlet-vs-draw_indirect_count_disabled.spirv"
                              : "assets/shaders/spirv/virtualgeometry-meshlet-vs.spirv";
    auto vsSrc = os::io::readRelativeFile(vsSpirv);
    vertexShader = renderGraph->createShader(ShaderInfo{.name = passName + "_meshletVS.shader", .layout = drawLayout, .src = vsSrc, .type = ShaderType::SpirV});

    DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.comparison = ComparisonOp::ComparisonOp_GreaterOrEqual;
    depthAttachment.format = settings.depthFormat;
    depthAttachment.loadOp = depthLoadOp;
    depthAttachment.storeOp = StoreOp::StoreOp_Store;

    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_DepthPrePassPipeline";
    pipelineInfo.layout = drawLayout;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = vertexShader;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = CullMode::Front;
    pipelineInfo.vertexStage.winding = WindingOrder::CW;
    pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.shaderEntry = "";
    pipelineInfo.fragmentStage.colorAttatchments = {};
    pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;

    graphicsPipeline = renderGraph->createGraphicsPipeline(pipelineInfo);

    const FrameTarget &target = frameTarget;
    rendering::CommandRecorder &cmd = commandBuffer;

    DepthStencilAttachmentInfo depthAttachmentInfo{};
    depthAttachmentInfo.enabled = true;
    depthAttachmentInfo.name = passName + "_DepthAttachment_Frame";
    depthAttachmentInfo.clearDepth = clearDepthValue;
    depthAttachmentInfo.clearStencil = 0;
    depthAttachmentInfo.view = target.depthView;

    RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass_Frame";
    renderPass.scissor = Rect2D(0, 0, settings.viewPortWidth, settings.viewPortHeight);
    renderPass.viewport = Viewport(settings.viewPortWidth, settings.viewPortHeight);
    renderPass.depthStencilAttachment = depthAttachmentInfo;

    const uint32_t countOffset = VISIBLE_CLUSTER_COUNT_INDEX * sizeof(uint32_t);
    const uint32_t stride = 4u * sizeof(uint32_t);

    cmd.cmdBindGraphicsPipeline(graphicsPipeline);
    cmd.cmdBeginRenderPass(renderPass);
    cmd.cmdBindBindingGroups(drawBindingGroups, nullptr, 0);

    if (!drawIndirectCountDisabled)
    {
      cmd.cmdDrawIndirectCount(
          BufferView{
            .buffer = indirectBuffer,
            .offset = 0,
            .size = renderGraph->getBufferSize(indirectBuffer),
            .access = AccessPattern::INDIRECT_COMMAND_READ,
          },
          0,
          BufferView{
            .buffer = cullingCountersBuffer,
            .offset = countOffset,
            .access = AccessPattern::INDIRECT_COMMAND_READ,
            .size = sizeof(uint32_t),
          },
          countOffset,
          settings.maxDrawnClusters,
          stride);
    }
    else
    {
      // Flat-vertex-stream variant: culling wrote a single VkDrawIndirectCommand
      // at offset 0 of indirectBuffer covering all visible clusters.
      cmd.cmdDrawIndirect(
          BufferView{
            .buffer = indirectBuffer,
            .offset = 0,
            .size = renderGraph->getBufferSize(indirectBuffer),
            .access = AccessPattern::INDIRECT_COMMAND_READ,
          },
          0,
          1,
          stride);
    }

    cmd.cmdEndRenderPass();
  }

private:
  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          BufferInfo{
              .name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot),
              .size = sizeof(CullingUniforms),
              .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
          });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;

    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  rendering::BufferId getCurrentFrameOverrideUniformBufferId() const
  {
    return getFrameOverrideUniformBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

};

} // namespace gpgpu
} // namespace virtualgeometry
