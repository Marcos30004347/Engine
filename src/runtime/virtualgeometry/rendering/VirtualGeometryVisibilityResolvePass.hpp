#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace virtualgeometry::gpgpu
{

// Fullscreen pass that writes the whole G-buffer - depth, geometry ids,
// material id and UV, geometry normal, motion vectors - from the merged
// visibility key of every pixel. See virtualgeometry-visibility-resolve.wgsl.
// Its per-frame uniforms mirror the hardware draw pass's, including the
// per-slot override buffers the sample supplies.
class VirtualGeometryVisibilityResolvePass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    ::rendering::Format motionVectorFormat = ::rendering::Format::Format_RG16Float;
    ::rendering::Format depthFormat = ::rendering::Format::Format_Depth32Float;
  };

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

  struct TemporalUniforms
  {
    float prevView[16];
    float prevProj[16];
    uint32_t hasPreviousFrame;
    uint32_t _padding[3];
  };

  struct ResolveUniforms
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
  };

  VirtualGeometryVisibilityResolvePass(
      VirtualGeometryScene &scene,
      ::rendering::Buffer visibilityKeys,
      VirtualGeometryHardwareDrawPass::FrameTarget frameTarget,
      Settings settings)
      : scene_(scene), visibilityKeys_(visibilityKeys), frameTarget_(frameTarget), settings_(settings)
  {
  }

  ~VirtualGeometryVisibilityResolvePass() override
  {
    destroyFrameOverrideBuffers();
    if (pipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (resolveUniformBuffer_.isValid())
      renderGraph->deleteBuffer(resolveUniformBuffer_);
    if (ownedCityAlbedo_.isValid())
      renderGraph->deleteTexture(ownedCityAlbedo_);
  }

  // Exact city voxel RGBA per pixel, alpha 0 where no city voxel resolved.
  // Valid once the pass has created its resources.
  ::rendering::Texture cityAlbedoTexture() const { return frameTarget_.cityAlbedoView.texture; }

  void updateUniforms(
      const float view[16],
      const float proj[16],
      const float viewPosition[4],
      uint32_t viewportW,
      uint32_t viewportH,
      float nearPlane,
      float farPlane,
      uint32_t hiZLevels,
      float errorThreshold = 0.0f,
      const float *prevView = nullptr,
      const float *prevProj = nullptr,
      bool hasPreviousFrame = false)
  {
    CullingUniforms u{};
    TemporalUniforms temporal{};
    std::memcpy(u.view, view, sizeof(float) * 16);
    std::memcpy(u.proj, proj, sizeof(float) * 16);
    std::memcpy(u.viewPosition, viewPosition, sizeof(float) * 4);
    u.viewport[0] = viewportW;
    u.viewport[1] = viewportH;
    u.error = errorThreshold;
    u.instances_count = static_cast<uint32_t>(scene_.getInstanceCount());
    u.nearPlane = nearPlane;
    u.farPlane = farPlane;
    u.hiZLevels = hiZLevels;
    std::memcpy(temporal.prevView, prevView != nullptr ? prevView : view, sizeof(temporal.prevView));
    std::memcpy(temporal.prevProj, prevProj != nullptr ? prevProj : proj, sizeof(temporal.prevProj));
    temporal.hasPreviousFrame = hasPreviousFrame ? 1u : 0u;
    if (!uniformsBuffer_.isValid())
      return;
    renderGraph->bufferWrite(uniformsBuffer_, 0u, sizeof(u), &u);
    renderGraph->bufferWrite(temporalUniformsBuffer_, 0u, sizeof(temporal), &temporal);
    if (!frameOverrideUniformBufferIds_.empty())
    {
      const uint32_t slot = renderGraph->getCurrentFrameIndex() % frameOverrideUniformBufferIds_.size();
      renderGraph->getRHI()->bufferWrite(frameOverrideUniformBufferIds_[slot], 0u, sizeof(u), &u);
      renderGraph->getRHI()->bufferWrite(frameOverrideTemporalUniformBufferIds_[slot], 0u, sizeof(temporal), &temporal);
    }
  }

  void appendFrameOverrides(::rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return;
    const uint32_t slot = frameSlot % frameOverrideUniformBufferIds_.size();
    overrides.bufferOverrides.emplace(
        uniformsBuffer_.name, ::rendering::RenderGraphBufferOverride{.bufferId = frameOverrideUniformBufferIds_[slot]});
    overrides.bufferOverrides.emplace(
        temporalUniformsBuffer_.name,
        ::rendering::RenderGraphBufferOverride{.bufferId = frameOverrideTemporalUniformBufferIds_[slot]});
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    using namespace ::rendering;
    RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = Rect2D(0, 0, settings_.viewportWidth, settings_.viewportHeight);
    renderPass.viewport = Viewport(settings_.viewportWidth, settings_.viewportHeight);
    renderPass.colorAttachments = {
      {.name = passName + "_PackedGeometryIdsLo", .view = frameTarget_.packedGeometryIdsLoView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_PackedGeometryIdsHi", .view = frameTarget_.packedGeometryIdsHiView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_MaterialId", .view = frameTarget_.materialIdView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_MaterialUV", .view = frameTarget_.materialUVView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_GeometryNormal", .view = frameTarget_.geometryNormalView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_MotionVectors", .view = frameTarget_.motionVectorView, .clearValue = Color::rgba(0, 0, 0, 0)},
      {.name = passName + "_CityAlbedo", .view = frameTarget_.cityAlbedoView, .clearValue = Color::rgba(0, 0, 0, 0)},
    };
    renderPass.depthStencilAttachment = {
      .enabled = true,
      .name = passName + "_Depth",
      .view = frameTarget_.depthView,
      .clearDepth = 0.0f,
      .clearStencil = 0u,
    };
    commandBuffer.cmdBindGraphicsPipeline(pipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDraw(6u, 1u, 0u, 0u);
    commandBuffer.cmdEndRenderPass();
  }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    using namespace ::rendering;

    uniformsBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_Uniforms.buffer",
      .size = sizeof(CullingUniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });
    temporalUniformsBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_TemporalUniforms.buffer",
      .size = sizeof(TemporalUniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });
    resolveUniformBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_ResolveUniforms.buffer",
      .size = sizeof(ResolveUniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
    });
    ResolveUniforms resolveUniforms{};
    resolveUniforms.viewportWidth = settings_.viewportWidth;
    resolveUniforms.viewportHeight = settings_.viewportHeight;
    renderGraph->bufferWrite(resolveUniformBuffer_, 0u, sizeof(resolveUniforms), &resolveUniforms);
    initializeFrameOverrideBuffers();

    const auto storage = [](const char *name, const uint32_t binding)
    {
      return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Fragment,
        .isDynamic = false,
      };
    };
    const auto uniform = [](const char *name, const uint32_t binding)
    {
      return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_UniformBuffer,
        .visibility = BindingVisibility::BindingVisibility_Fragment,
        .isDynamic = false,
      };
    };
    layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups = {BindingGroupLayout{
        .buffers = {
          uniform("uniforms", 0u),
          uniform("temporalUniforms", 1u),
          uniform("resolve", 2u),
          storage("instances", 3u),
          storage("instanceSparseToDense", 4u),
          storage("decodedClusterGeometryInfos", 5u),
          storage("pagesBuffer", 9u),
          storage("decodedVertexBuffer", 6u),
          storage("decodedIndexBuffer", 7u),
          storage("visibilityKeys", 8u),
          storage("cityGeometries", 10u),
          storage("cityBricks", 11u),
          storage("cityAppearances", 12u),
          storage("cityAppearanceWords", 13u),
          storage("cityClusterSources", 14u),
          storage("cityProfileSlots", 15u),
        },
      }},
    });

    const uint64_t instanceBytes = std::max<uint64_t>(scene_.instanceBufferSize, sizeof(VirtualGeometryInstanceGPUData));
    const auto view = [](const Buffer &buffer, const uint64_t size, const AccessPattern access)
    {
      return BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    groups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = view(uniformsBuffer_, sizeof(CullingUniforms), AccessPattern::UNIFORM_READ), .binding = 0u},
          {.bufferView = view(temporalUniformsBuffer_, sizeof(TemporalUniforms), AccessPattern::UNIFORM_READ), .binding = 1u},
          {.bufferView = view(resolveUniformBuffer_, sizeof(ResolveUniforms), AccessPattern::UNIFORM_READ), .binding = 2u},
          {.bufferView = view(scene_.instanceBuffer, instanceBytes, AccessPattern::SHADER_READ), .binding = 3u},
          {.bufferView = view(scene_.instanceSparseToDenseBuffer, scene_.instanceSparseToDenseBufferSize, AccessPattern::SHADER_READ), .binding = 4u},
          {.bufferView = view(scene_.decodedClusterGeometryBuffer, scene_.decodedClusterGeometryBufferSize, AccessPattern::SHADER_READ), .binding = 5u},
          {.bufferView = view(scene_.pagesBuffer, scene_.pagesBufferSize, AccessPattern::SHADER_READ), .binding = 9u},
          {.bufferView = view(scene_.decodedVertexBuffer, scene_.decodedVertexBufferSize, AccessPattern::SHADER_READ), .binding = 6u},
          {.bufferView = view(scene_.decodedIndexBuffer, scene_.decodedIndexBufferSize, AccessPattern::SHADER_READ), .binding = 7u},
          {.bufferView = view(visibilityKeys_, renderGraph->getBufferSize(visibilityKeys_), AccessPattern::SHADER_READ), .binding = 8u},
          {.bufferView = view(scene_.cityGeometryTableBuffer, scene_.cityGeometryTableBytes, AccessPattern::SHADER_READ), .binding = 10u},
          {.bufferView = view(scene_.cityBrickBuffer, scene_.cityBrickBytes, AccessPattern::SHADER_READ), .binding = 11u},
          {.bufferView = view(scene_.cityAppearanceTableBuffer, scene_.cityAppearanceTableBytes, AccessPattern::SHADER_READ), .binding = 12u},
          {.bufferView = view(scene_.cityAppearanceWordsBuffer, scene_.cityAppearanceWordsBytes, AccessPattern::SHADER_READ), .binding = 13u},
          {.bufferView = view(scene_.cityClusterSourceBuffer, scene_.cityClusterSourceBytes, AccessPattern::SHADER_READ), .binding = 14u},
          {.bufferView = view(scene_.cityProfileSlotBuffer, scene_.cityProfileSlotBytes, AccessPattern::SHADER_READ), .binding = 15u},
        },
      }},
    });

    shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-visibility-resolve.spirv"),
      .type = ShaderType::SpirV,
      .layout = layout_,
    });

    DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    depthAttachment.format = settings_.depthFormat;
    depthAttachment.loadOp = LoadOp::LoadOp_Load;
    depthAttachment.storeOp = StoreOp::StoreOp_Store;
    depthAttachment.depthTestEnabled = true;
    depthAttachment.depthWriteEnabled = true;
    depthAttachment.comparison = ComparisonOp::ComparisonOp_Always;

    const auto color = [](const Format format)
    {
      return ColorAttatchment{
        .format = format,
        .loadOp = LoadOp::LoadOp_Load,
        .storeOp = StoreOp::StoreOp_Store,
        .blendMode = ColorBlendMode::Replace,
        .initialLayout = ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ResourceLayout::COLOR_ATTACHMENT,
      };
    };
    // Unlike the G-buffer targets the city colour has no other writer, so it
    // is cleared here rather than loaded.
    ColorAttatchment cityAlbedoColor = color(Format::Format_RGBA8Unorm);
    cityAlbedoColor.loadOp = LoadOp::LoadOp_Clear;
    if (!frameTarget_.cityAlbedoView.texture.isValid())
    {
      ownedCityAlbedo_ = renderGraph->createTexture(TextureInfo{
        .name = passName + "_CityAlbedo",
        .format = Format::Format_RGBA8Unorm,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
        .width = settings_.viewportWidth,
        .height = settings_.viewportHeight,
        .mipLevels = 1u,
      });
      frameTarget_.cityAlbedoView = TextureView{
        .texture = ownedCityAlbedo_,
        .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = ResourceLayout::COLOR_ATTACHMENT,
        .index = 0,
        .flags = ImageAspectFlags::Color,
        .baseArrayLayer = 0,
        .baseMipLevel = 0,
        .layerCount = 1,
        .levelCount = 1,
      };
    }
    GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + "_Pipeline";
    pipelineInfo.layout = layout_;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = shader_;
    pipelineInfo.vertexStage.shaderEntry = "resolveVertex";
    pipelineInfo.vertexStage.cullType = CullMode::None;
    pipelineInfo.vertexStage.winding = WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = shader_;
    pipelineInfo.fragmentStage.shaderEntry = "resolveFragment";
    pipelineInfo.fragmentStage.colorAttatchments = {
      color(Format::Format_R32Uint),
      color(Format::Format_R32Uint),
      color(Format::Format_R32Uint),
      color(Format::Format_RG32Float),
      color(Format::Format_RGBA16Float),
      color(settings_.motionVectorFormat),
      cityAlbedoColor,
    };
    pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;
    pipeline_ = renderGraph->createGraphicsPipeline(pipelineInfo);
  }

  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, ::rendering::BufferId::Invalid);
    frameOverrideTemporalUniformBufferIds_.assign(frameCount, ::rendering::BufferId::Invalid);
    for (uint32_t slot = 0u; slot < frameCount; ++slot)
    {
      frameOverrideUniformBufferIds_[slot] = renderGraph->getRHI()->createBuffer(::rendering::BufferInfo{
        .name = passName + "_Uniforms.override_frame" + std::to_string(slot),
        .size = sizeof(CullingUniforms),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      });
      frameOverrideTemporalUniformBufferIds_[slot] = renderGraph->getRHI()->createBuffer(::rendering::BufferInfo{
        .name = passName + "_TemporalUniforms.override_frame" + std::to_string(slot),
        .size = sizeof(TemporalUniforms),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;
    for (const auto id : frameOverrideUniformBufferIds_)
      if (id != ::rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(id);
    for (const auto id : frameOverrideTemporalUniformBufferIds_)
      if (id != ::rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(id);
    frameOverrideUniformBufferIds_.clear();
    frameOverrideTemporalUniformBufferIds_.clear();
  }

  VirtualGeometryScene &scene_;
  ::rendering::Buffer visibilityKeys_{};
  ::rendering::Texture ownedCityAlbedo_{};
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_{};
  Settings settings_{};
  ::rendering::Buffer uniformsBuffer_{};
  ::rendering::Buffer temporalUniformsBuffer_{};
  ::rendering::Buffer resolveUniformBuffer_{};
  std::vector<::rendering::BufferId> frameOverrideUniformBufferIds_;
  std::vector<::rendering::BufferId> frameOverrideTemporalUniformBufferIds_;
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
};

} // namespace virtualgeometry::gpgpu
