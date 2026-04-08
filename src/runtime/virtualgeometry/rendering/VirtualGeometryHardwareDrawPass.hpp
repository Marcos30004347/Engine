#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"

using namespace rendering;

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryHardwareDrawPass : public Pass
{
public:
  struct FrameTarget
  {
    TextureView packedGeometryIdsLoView;
    TextureView packedGeometryIdsHiView;
    TextureView materialIdView;
    TextureView materialUVView;
    TextureView depthView;
    Texture depthTexture;
    TextureView colorView;
    Texture colorTexture;
  };

  struct Settings
  {
    uint32_t viewPortWidth = 1920;
    uint32_t viewPortHeight = 1080;
    uint32_t maxDrawnClusters = VirtualGeometryScene::MAX_VISIBLE_CLUSTERS;
    rendering::Format colorFormat = rendering::Format::Format_BGRA8Unorm;
    rendering::Format depthFormat = rendering::Format::Format_Depth32Float;
  };

private:
  rendering::BindingsLayout drawLayout;
  rendering::BindingGroups drawBindingGroups;
  rendering::Shader vertexShader;
  rendering::Shader fragmentShader;
  rendering::GraphicsPipeline graphicsPipeline;

  VirtualGeometryScene &scene;
  rendering::Buffer indirectBuffer;
  rendering::Buffer visibleClustesInfoBuffer;

  rendering::Buffer uniformsBuffer;
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
  rendering::Buffer cullingCountersBuffer;
#ifdef COLLECT_FRAME_STATISTICS
  rendering::Buffer frameStatisticsBuffer;
  std::vector<rendering::BufferId> frameOverrideFrameStatisticsBufferIds_;
  std::vector<uint32_t> frameStatisticsClearValues;
#endif

  FrameTarget frameTarget;

  float clearDepthValue;
  LoadOp depthLoadOp;

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

  static void multiplyMat4ColumnMajor(const float a[16], const float b[16], float out[16])
  {
    for (uint32_t col = 0; col < 4u; ++col)
    {
      for (uint32_t row = 0; row < 4u; ++row)
      {
        float sum = 0.0f;
        for (uint32_t k = 0; k < 4u; ++k)
        {
          // Column-major indexing: m[col*4 + row]
          sum += a[k * 4u + row] * b[col * 4u + k];
        }
        out[col * 4u + row] = sum;
      }
    }
  }

  VirtualGeometryHardwareDrawPass(
      VirtualGeometryScene &scene,
      rendering::Buffer indirectBuffer,
      rendering::Buffer visibleClustesInfoBuffer,
      rendering::Buffer cullingCountersBuffer,
      FrameTarget frameTarget,
      LoadOp depthLoadOp,
      float clearDepth,
      Settings settings)
      : scene(scene), cullingCountersBuffer(cullingCountersBuffer), indirectBuffer(indirectBuffer), visibleClustesInfoBuffer(visibleClustesInfoBuffer), frameTarget(frameTarget), clearDepthValue(clearDepth),
        depthLoadOp(depthLoadOp), settings(settings)
  {
  }

  VirtualGeometryHardwareDrawPass(
      VirtualGeometryScene &scene,
      rendering::Buffer indirectBuffer,
      rendering::Buffer visibleClustesInfoBuffer,
      rendering::Buffer cullingCountersBuffer,
      FrameTarget frameTarget,
      bool shouldClearDepth,
      float clearDepth,
      Settings settings)
      : VirtualGeometryHardwareDrawPass(
            scene,
            indirectBuffer,
            visibleClustesInfoBuffer,
            cullingCountersBuffer,
            frameTarget,
            shouldClearDepth ? LoadOp::LoadOp_Clear : LoadOp::LoadOp_DontCare,
            clearDepth,
            settings)
  {
  }

  void updateUniforms(
      const float view[16],
      const float proj[16],
      const float viewPosition[4],
      uint32_t viewportW,
      uint32_t viewportH,
      float nearPlane,
      float farPlane,
      uint32_t hiZLevels,
      float errorThreshold = 0.0f)
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
#ifdef COLLECT_FRAME_STATISTICS
    const uint32_t pixelCount = std::max(1u, viewportW * viewportH);
    if (frameStatisticsClearValues.size() != pixelCount)
    {
      frameStatisticsClearValues.assign(pixelCount, 0u);
    }
    if (frameOverrideFrameStatisticsBufferIds_.empty())
    {
      renderGraph->bufferWrite(frameStatisticsBuffer, 0, frameStatisticsClearValues.size() * sizeof(uint32_t), frameStatisticsClearValues.data());
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(
          getCurrentFrameOverrideFrameStatisticsBufferId(),
          0,
          frameStatisticsClearValues.size() * sizeof(uint32_t),
          frameStatisticsClearValues.data());
    }
#endif
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
    {
      overrides.bufferOverrides.emplace(uniformsBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
    }
#ifdef COLLECT_FRAME_STATISTICS
    if (!frameOverrideFrameStatisticsBufferIds_.empty())
    {
      overrides.bufferOverrides.emplace(frameStatisticsBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideFrameStatisticsBufferId(frameSlot)});
    }
#endif
  }

  ~VirtualGeometryHardwareDrawPass()
  {
    destroyFrameOverrideBuffers();
    renderGraph->deleteGraphicsPipeline(graphicsPipeline);
    renderGraph->deleteShader(vertexShader);
    renderGraph->deleteShader(fragmentShader);
    renderGraph->deleteBindingGroups(drawBindingGroups);
    renderGraph->deleteBindingsLayout(drawLayout);
#ifdef COLLECT_FRAME_STATISTICS
    renderGraph->deleteBuffer(frameStatisticsBuffer);
#endif
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
#ifdef COLLECT_FRAME_STATISTICS
    const uint32_t frameStatisticsPixelCount = std::max(1u, settings.viewPortWidth * settings.viewPortHeight);
    frameStatisticsBuffer = createFrameLocalBuffer(
        BufferInfo{
          .name = passName + "_FrameStatistics.buffer",
          .size = static_cast<uint64_t>(frameStatisticsPixelCount) * sizeof(uint32_t),
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
        });
    frameStatisticsClearValues.assign(frameStatisticsPixelCount, 0u);
    renderGraph->bufferWrite(frameStatisticsBuffer, 0, frameStatisticsClearValues.size() * sizeof(uint32_t), frameStatisticsClearValues.data());
#endif

    drawLayout = renderGraph->createBindingsLayout(
        BindingsLayoutInfo{
          .name = passName + "_drawLayout.layout",
          .groups =
              {
                BindingGroupLayout{
                  .buffers =
                      {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility =
#ifdef COLLECT_FRAME_STATISTICS
                            BindingVisibility::BindingVisibility_Vertex | BindingVisibility::BindingVisibility_Fragment
#else
                            BindingVisibility::BindingVisibility_Vertex
#endif
                        },
                        {.name = "instances", .binding = 1, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "pageTable", .binding = 2, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "pagesBuffer", .binding = 3, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "visibleClusterInfos", .binding = 4, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "cullingCounters", .binding = 5, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
                        {.name = "meshPartTransforms", .binding = 6, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Vertex},
#ifdef COLLECT_FRAME_STATISTICS
                        {.name = "frameStatistics", .binding = 7, .isDynamic = false, .type = BufferBindingType::BufferBindingType_StorageBuffer, .visibility = BindingVisibility::BindingVisibility_Fragment},
#endif
                      },
                },
              },
        });

    const uint64_t instanceBufferSize = scene.getInstanceCount() * sizeof(VirtualGeometryInstanceGPUData);
    const uint64_t visibleClusterInfosBufferSize = renderGraph->getBufferSize(visibleClustesInfoBuffer);// scene.visibleClusterInfosBufferSize;

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
#ifdef COLLECT_FRAME_STATISTICS
                        {.binding = 7, .bufferView = {.buffer = frameStatisticsBuffer, .access = AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE, .offset = 0, .size = renderGraph->getBufferSize(frameStatisticsBuffer)}},
#endif
                      },
                },
              },
        });

    const bool drawIndirectCountDisabled = !(renderGraph->getRHI()->getFeatures() & DeviceFeatures::DeviceFeatures_DrawIndirectCount);
    const char *vsSpirv = drawIndirectCountDisabled
                              ? "assets/shaders/spirv/virtualgeometry-meshlet-vs-draw_indirect_count_disabled.spirv"
                              : "assets/shaders/spirv/virtualgeometry-meshlet-vs.spirv";
    auto vsSrc = os::io::readRelativeFile(vsSpirv);
    auto fsSrc = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-meshlet-fs.spirv");

    vertexShader = renderGraph->createShader(ShaderInfo{.name = passName + "_meshletVS.shader", .layout = drawLayout, .src = vsSrc, .type = ShaderType::SpirV});
    fragmentShader = renderGraph->createShader(ShaderInfo{.name = passName + "_meshletFS.shader", .layout = drawLayout, .src = fsSrc, .type = ShaderType::SpirV});

    DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.comparison = ComparisonOp::ComparisonOp_GreaterOrEqual;
    depthAttachment.format = settings.depthFormat;
    depthAttachment.loadOp = depthLoadOp;
    depthAttachment.storeOp = StoreOp::StoreOp_Store;

    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_MeshletPipeline";
    pipelineInfo.layout = drawLayout;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = vertexShader;
    pipelineInfo.vertexStage.shaderEntry = "vs_main";
    pipelineInfo.vertexStage.cullType = CullMode::Front;
    pipelineInfo.vertexStage.winding = WindingOrder::CW;
    pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = fragmentShader;
    pipelineInfo.fragmentStage.shaderEntry = "fs_main";
    pipelineInfo.fragmentStage.colorAttatchments = {
      {
        .format = Format::Format_R32Uint,
        .loadOp = LoadOp::LoadOp_Clear,
        .storeOp = StoreOp::StoreOp_Store,
        .blendMode = ColorBlendMode::Replace,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      }, // packed geometry ids lo
      {
        .format = Format::Format_R32Uint,
        .loadOp = LoadOp::LoadOp_Clear,
        .storeOp = StoreOp::StoreOp_Store,
        .blendMode = ColorBlendMode::Replace,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      }, // packed geometry ids hi
      {
        .format = Format::Format_R32Uint,
        .loadOp = LoadOp::LoadOp_Clear,
        .storeOp = StoreOp::StoreOp_Store,
        .blendMode = ColorBlendMode::Replace,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      }, // material id
      {
        .format = Format::Format_RG32Float,
        .loadOp = LoadOp::LoadOp_Clear,
        .storeOp = StoreOp::StoreOp_Store,
        .blendMode = ColorBlendMode::Replace,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      }, // material uv
    };
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
    renderPass.colorAttachments = {
      {
        .name = passName + "_PackedGeometryIdsLo",
        .view = target.packedGeometryIdsLoView,
        .clearValue = Color::rgba(0, 0, 0, 0),
      },
      {
        .name = passName + "_PackedGeometryIdsHi",
        .view = target.packedGeometryIdsHiView,
        .clearValue = Color::rgba(0, 0, 0, 0),
      },
      {
        .name = passName + "_MaterialId",
        .view = target.materialIdView,
        .clearValue = Color::rgba(0, 0, 0, 0),
      },
      {
        .name = passName + "_MaterialUV",
        .view = target.materialUVView,
        .clearValue = Color::rgba(0, 0, 0, 0),
      },
    };

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

#ifdef COLLECT_FRAME_STATISTICS
  const rendering::Buffer &getFrameStatisticsBuffer() const
  {
    return frameStatisticsBuffer;
  }
#endif

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
#ifdef COLLECT_FRAME_STATISTICS
    const uint32_t frameStatisticsPixelCount = std::max(1u, settings.viewPortWidth * settings.viewPortHeight);
    frameOverrideFrameStatisticsBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideFrameStatisticsBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          BufferInfo{
              .name = passName + "_FrameStatistics.override_frame" + std::to_string(frameSlot),
              .size = static_cast<uint64_t>(frameStatisticsPixelCount) * sizeof(uint32_t),
              .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
          });
    }
#endif
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
#ifdef COLLECT_FRAME_STATISTICS
    for (const rendering::BufferId bufferId : frameOverrideFrameStatisticsBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    frameOverrideFrameStatisticsBufferIds_.clear();
#endif
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

#ifdef COLLECT_FRAME_STATISTICS
  rendering::BufferId getFrameOverrideFrameStatisticsBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideFrameStatisticsBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideFrameStatisticsBufferIds_[frameSlot % frameOverrideFrameStatisticsBufferIds_.size()];
  }

  rendering::BufferId getCurrentFrameOverrideFrameStatisticsBufferId() const
  {
    return getFrameOverrideFrameStatisticsBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }
#endif
};

} // namespace gpgpu
} // namespace virtualgeometry
