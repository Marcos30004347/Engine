#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/PlanetCbt.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetSurfaceAtlas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelPlanetOverviewPass final : public ::rendering::Pass
{
public:
  enum class Geometry : uint32_t
  {
    Static = 0u,
    AdaptiveCbt = 1u,
  };

  enum class Diagnostic : uint32_t
  {
    Shaded = 0u,
    Wireframe = 1u,
    SubdivisionDepth = 2u,
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool enabled = false;
    float verticalFovRadians = 1.04719755f;
    std::array<float, 4> cameraDirectionDistance{0.0f, 0.0f, 1.0f, 2.0f};
    std::array<float, 4> cameraRightRelief{1.0f, 0.0f, 0.0f, 8.0f};
    std::array<float, 4> cameraUpSourceRadius{0.0f, 1.0f, 0.0f, 6371000.0f};
    std::array<float, 4> sunDirectionIntensity{-0.4f, -0.7f, -0.5f, 1.0f};
    std::array<float, 4> sunColorAtmosphere{1.0f, 0.96f, 0.88f, 0.35f};
    // Forest, mountain, river, and ocean strengths.
    std::array<float, 4> featureStrengths{0.65f, 0.7f, 0.85f, 1.0f};
    // Normalized destination direction and enabled flag.
    std::array<float, 4> markerDirectionEnabled{0.0f, 0.0f, 1.0f, 0.0f};
    // RGB marker colour and chord-space radius on the normalized sphere.
    std::array<float, 4> markerColorRadius{1.0f, 0.25f, 0.04f, 0.032f};
    Geometry geometry = Geometry::Static;
    Diagnostic diagnostic = Diagnostic::Shaded;
    uint32_t diagnosticMaximumDepth = kPlanetCbtMaximumSubdivisionDepth;
    uint32_t renderSubdivisionDepth = 0u;
  };

  struct CbtBuffers
  {
    ::rendering::Buffer pool;
    ::rendering::Buffer pointers;
    ::rendering::Buffer roots;
    ::rendering::Buffer drawIndirect;
    uint32_t capacity = 0u;
  };

  SparseVoxelPlanetOverviewPass(
      const ::rendering::Texture color,
      const ::rendering::Texture depth,
      std::vector<PlanetOverviewVertex> vertices,
      std::vector<uint32_t> indices,
      const SparseVoxelPlanetSurfaceAtlas::Resources surfaceAtlas,
      const CbtBuffers cbt,
      const Settings settings)
      : color_(color), depth_(depth), vertices_(std::move(vertices)), indices_(std::move(indices)), surfaceAtlas_(surfaceAtlas), surfaceMapsAvailable_(surfaceAtlas_.valid()), cbt_(cbt), settings_(settings)
  {
    if (!surfaceAtlas_.albedoRoughness.isValid() ||
        !surfaceAtlas_.terrainFeatures.isValid() ||
        !surfaceAtlas_.height.isValid() ||
        !cbt_.pool.isValid() || !cbt_.pointers.isValid() ||
        !cbt_.roots.isValid() || !cbt_.drawIndirect.isValid() ||
        cbt_.capacity < kPlanetCbtRootBisectorCount)
      throw std::invalid_argument("Planet overview CBT buffers are invalid");
  }

  ~SparseVoxelPlanetOverviewPass() override
  {
    if (adaptivePipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(adaptivePipeline_);
    if (pipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (surfaceSampler_.isValid())
      renderGraph->deleteSampler(surfaceSampler_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (vertexBuffer_.isValid())
      renderGraph->deleteBuffer(vertexBuffer_);
    if (indexBuffer_.isValid())
      renderGraph->deleteBuffer(indexBuffer_);
    if (staticDrawIndirect_.isValid())
      renderGraph->deleteBuffer(staticDrawIndirect_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void update(const Settings &settings)
  {
    settings_ = settings;
    settings_.width = std::max(settings_.width, 1u);
    settings_.height = std::max(settings_.height, 1u);
    if (uniformBuffer_.isValid())
      upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    ::rendering::ColorAttachmentInfo color{};
    color.name = passName + "_Color";
    color.view = colorView(color_);
    color.clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(0, 0, settings_.width, settings_.height);
    pass.viewport = ::rendering::Viewport(settings_.width, settings_.height);
    pass.colorAttachments = {color};
    pass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(depth_),
      .clearDepth = 0.0f,
      .clearStencil = 0u,
      .enabled = true,
    };
    commands.cmdBindGraphicsPipeline(pipeline_);
    commands.cmdBeginRenderPass(pass);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    // Render-graph commands are recorded when the pass is registered. Keep the
    // indirect draw in the graph so cached command buffers remain valid while
    // update() switches overview mode; upload() gates the current frame slot by
    // writing a zero index count while the overview is hidden.
    if (!vertices_.empty() && !indices_.empty())
    {
      commands.cmdBindVertexBuffer(0u, {.buffer = vertexBuffer_, .offset = 0u, .size = vertices_.size() * sizeof(PlanetOverviewVertex), .access = ::rendering::AccessPattern::VERTEX_ATTRIBUTE_READ});
      commands.cmdBindIndexBuffer({.buffer = indexBuffer_, .offset = 0u, .size = indices_.size() * sizeof(uint32_t), .access = ::rendering::AccessPattern::INDEX_READ}, ::rendering::Type_Uint32);
      commands.cmdDrawIndexedIndirect(
          {.buffer = staticDrawIndirect_,
           .offset = 0u,
           .size = sizeof(StaticDrawIndexedIndirectGPU),
           .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ},
          0u, 1u, sizeof(StaticDrawIndexedIndirectGPU));
    }
    commands.cmdBindGraphicsPipeline(adaptivePipeline_);
    commands.cmdDrawIndirect({.buffer = cbt_.drawIndirect, .offset = 0u, .size = sizeof(PlanetCbtDrawIndirectGPU), .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ}, 0u, 1u, sizeof(PlanetCbtDrawIndirectGPU));
    commands.cmdEndRenderPass();
  }

private:
  struct StaticDrawIndexedIndirectGPU
  {
    uint32_t indexCount = 0u;
    uint32_t instanceCount = 1u;
    uint32_t firstIndex = 0u;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0u;
  };
  static_assert(sizeof(StaticDrawIndexedIndirectGPU) == 20u);

  struct alignas(16) UniformsGPU
  {
    std::array<float, 4> viewportFocal{};
    std::array<float, 4> cameraDirectionDistance{};
    std::array<float, 4> cameraRightRelief{};
    std::array<float, 4> cameraUpSourceRadius{};
    std::array<float, 4> sunDirectionIntensity{};
    std::array<float, 4> sunColorAtmosphere{};
    std::array<float, 4> featureStrengths{};
    std::array<float, 4> markerDirectionEnabled{};
    std::array<float, 4> markerColorRadius{};
    std::array<float, 4> surfaceMapInfo{};
    std::array<uint32_t, 4> renderControls{};
  };

  static ::rendering::TextureView colorView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  static ::rendering::TextureView depthView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
      .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  void upload()
  {
    UniformsGPU uniforms{};
    uniforms.viewportFocal = {static_cast<float>(settings_.width), static_cast<float>(settings_.height), 1.0f / std::tan(std::max(settings_.verticalFovRadians, 0.01f) * 0.5f), 0.001f};
    uniforms.cameraDirectionDistance = settings_.cameraDirectionDistance;
    uniforms.cameraRightRelief = settings_.cameraRightRelief;
    uniforms.cameraUpSourceRadius = settings_.cameraUpSourceRadius;
    uniforms.sunDirectionIntensity = settings_.sunDirectionIntensity;
    uniforms.sunColorAtmosphere = settings_.sunColorAtmosphere;
    uniforms.featureStrengths = settings_.featureStrengths;
    uniforms.markerDirectionEnabled = settings_.markerDirectionEnabled;
    uniforms.markerColorRadius = settings_.markerColorRadius;
    const double sourceRadius = std::max(1.0f, settings_.cameraUpSourceRadius[3]);
    uniforms.surfaceMapInfo = {
      surfaceMapsAvailable_ ? static_cast<float>(surfaceAtlas_.faceResolution) : 0.0f,
      surfaceMapsAvailable_ ? static_cast<float>(surfaceAtlas_.mipCount) : 1.0f,
      static_cast<float>(surfaceAtlas_.heightMinimumMetres / sourceRadius),
      static_cast<float>(surfaceAtlas_.heightRangeMetres / sourceRadius),
    };
    uniforms.renderControls = {
      static_cast<uint32_t>(settings_.geometry),
      static_cast<uint32_t>(settings_.diagnostic),
      std::max(settings_.diagnosticMaximumDepth, 1u),
      std::min(settings_.renderSubdivisionDepth,
          kPlanetCbtMaximumRenderSubdivisionDepth),
    };
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    StaticDrawIndexedIndirectGPU staticDraw{};
    staticDraw.indexCount = settings_.enabled &&
            settings_.geometry == Geometry::Static
        ? static_cast<uint32_t>(indices_.size()) : 0u;
    renderGraph->bufferWrite(
        staticDrawIndirect_, 0u, sizeof(staticDraw), &staticDraw);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(UniformsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      // The overview can transition while older frame slots remain in flight.
      // Keep its uniforms paired with the frame-local indirect draw gate.
      .frameLocal = true,
    });
    staticDrawIndirect_ = renderGraph->createBuffer({
      .name = passName + "_StaticDrawIndirect",
      .size = sizeof(StaticDrawIndexedIndirectGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
      // update() writes the draw gate before submitting the selected frame
      // slot. Do not overwrite arguments that an earlier in-flight slot can
      // still be consuming.
      .frameLocal = true,
    });
    vertexBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Vertices",
         .size = std::max<uint64_t>(sizeof(PlanetOverviewVertex), vertices_.size() * sizeof(PlanetOverviewVertex)),
         .usage = ::rendering::BufferUsage::BufferUsage_Vertex | ::rendering::BufferUsage::BufferUsage_Push});
    if (!vertices_.empty())
      renderGraph->bufferWrite(vertexBuffer_, 0u, vertices_.size() * sizeof(PlanetOverviewVertex), vertices_.data());
    indexBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Indices",
         .size = std::max<uint64_t>(sizeof(uint32_t), indices_.size() * sizeof(uint32_t)),
         .usage = ::rendering::BufferUsage::BufferUsage_Index | ::rendering::BufferUsage::BufferUsage_Push});
    if (!indices_.empty())
      renderGraph->bufferWrite(indexBuffer_, 0u, indices_.size() * sizeof(uint32_t), indices_.data());
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers =
               {
                 {.name = "uniforms",
                  .binding = 0u,
                  .isDynamic = false,
                  .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                  .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex | ::rendering::BindingVisibility::BindingVisibility_Fragment},
                 {.name = "cbtBisectors",
                  .binding = 3u,
                  .isDynamic = false,
                  .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                  .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex},
                 {.name = "cbtPointers",
                  .binding = 4u,
                  .isDynamic = false,
                  .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                  .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex},
                 {.name = "cbtRoots", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex},
               },
           .samplers = {
             {.name = "surfaceLinearSampler",
              .binding = 7u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex |
                  ::rendering::BindingVisibility::BindingVisibility_Fragment},
           },
           .textures = {
             {.name = "surfaceAlbedoRoughness", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "surfaceTerrainFeatures", .binding = 2u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "surfaceHeight", .binding = 6u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex | ::rendering::BindingVisibility::BindingVisibility_Fragment},
           }}}});
    surfaceSampler_ = renderGraph->createSampler({
      .name = passName + "_SurfaceLinearSampler",
      .minFilter = ::rendering::Filter::Linear,
      .magFilter = ::rendering::Filter::Linear,
      .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
      .anisotropyEnable = false,
      .maxAnisotropy = 1.0f,
      .maxLod = static_cast<float>(
          std::max(surfaceAtlas_.mipCount, 1u) - 1u),
    });
    const auto sampled = [this](::rendering::Texture texture)
    {
      return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = surfaceAtlas_.mipCount};
    };
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers =
               {
                 {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(UniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
                 {.binding = 3u, .bufferView = {.buffer = cbt_.pool, .offset = 0u, .size = uint64_t{cbt_.capacity} * sizeof(PlanetCbtBisectorGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
                 {.binding = 4u, .bufferView = {.buffer = cbt_.pointers, .offset = 0u, .size = uint64_t{cbt_.capacity} * sizeof(PlanetCbtPointerGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
                 {.binding = 5u, .bufferView = {.buffer = cbt_.roots, .offset = 0u, .size = uint64_t{kPlanetCbtRootBisectorCount} * sizeof(PlanetCbtRootTriangleGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
               },
           .samplers = {
             {.binding = 7u,
              .sampler = surfaceSampler_,
              .view = sampled(surfaceAtlas_.albedoRoughness)},
           },
           .textures = {
             {.binding = 1u, .textureView = sampled(surfaceAtlas_.albedoRoughness)},
             {.binding = 2u, .textureView = sampled(surfaceAtlas_.terrainFeatures)},
             {.binding = 6u, .textureView = sampled(surfaceAtlas_.height)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-planet-overview.spirv"), .type = ::rendering::ShaderType::SpirV});
    ::rendering::DepthAttatchment depth{};
    depth.enabled = true;
    depth.comparison = ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual;
    depth.format = ::rendering::Format::Format_Depth32Float;
    depth.loadOp = ::rendering::LoadOp::LoadOp_Clear;
    depth.storeOp = ::rendering::StoreOp::StoreOp_Store;
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawOverviewVertex";
    pipeline.vertexStage.vertexLayoutElements = {
      {.name = "directionElevation", .type = ::rendering::Type_Float32x4, .binding = 0u, .offset = 0u, .location = 0u},
      {.name = "baseColorForest", .type = ::rendering::Type_Float32x4, .binding = 0u, .offset = 16u, .location = 1u},
      {.name = "soilColorMountain", .type = ::rendering::Type_Float32x4, .binding = 0u, .offset = 32u, .location = 2u},
      {.name = "climateRiverWater", .type = ::rendering::Type_Float32x4, .binding = 0u, .offset = 48u, .location = 3u},
    };
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "drawOverviewFragment";
    pipeline.fragmentStage.colorAttatchments = {{
      .format = ::rendering::Format::Format_RGBA16Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }};
    pipeline.fragmentStage.depthAttatchment = depth;
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
    pipeline.name = passName + "_AdaptivePipeline";
    pipeline.vertexStage.shaderEntry = "drawAdaptiveOverviewVertex";
    pipeline.vertexStage.vertexLayoutElements.clear();
    adaptivePipeline_ = renderGraph->createGraphicsPipeline(pipeline);
  }

  ::rendering::Texture color_{};
  ::rendering::Texture depth_{};
  std::vector<PlanetOverviewVertex> vertices_;
  std::vector<uint32_t> indices_;
  SparseVoxelPlanetSurfaceAtlas::Resources surfaceAtlas_{};
  bool surfaceMapsAvailable_ = false;
  CbtBuffers cbt_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer staticDrawIndirect_{};
  ::rendering::Buffer vertexBuffer_{};
  ::rendering::Buffer indexBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Sampler surfaceSampler_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
  ::rendering::GraphicsPipeline adaptivePipeline_{};
};

} // namespace voxel::svraster::rendering
