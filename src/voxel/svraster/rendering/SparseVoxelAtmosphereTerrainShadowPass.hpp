#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetBackgroundPass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

// Persistent, low-resolution directional depth map of the decimated local
// planet cap. Indirect draw counts allow the map to retain its contents between
// 0.25-degree sun updates without rebuilding the render graph.
class SparseVoxelAtmosphereTerrainShadowPass final : public ::rendering::Pass
{
public:
  using Vertex = SparseVoxelPlanetBackgroundPass::Vertex;

  struct Settings
  {
    uint32_t resolution = 256u;
    bool coverageAvailable = false;
    bool rebuild = true;
    std::array<float, 4> tangentX{};
    std::array<float, 4> tangentUp{0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> tangentZ{};
    std::array<float, 4> tangentCamera{};
    // x source radius, y half map extent, z half depth range, in metres.
    std::array<float, 4> sourceRadiusExtent{};
    std::array<float, 4> directionTowardSun{0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> localUp{0.0f, 1.0f, 0.0f, 0.0f};
  };

  SparseVoxelAtmosphereTerrainShadowPass(::rendering::Texture depth,
      std::vector<Vertex> vertices, std::vector<uint32_t> indices,
      Settings settings)
      : depth_(depth), vertices_(std::move(vertices)),
        indices_(std::move(indices)), settings_(settings)
  {
  }

  ~SparseVoxelAtmosphereTerrainShadowPass() override
  {
    if (geometryPipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(geometryPipeline_);
    if (clearPipeline_.isValid())
      renderGraph->deleteGraphicsPipeline(clearPipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (indexBuffer_.isValid()) renderGraph->deleteBuffer(indexBuffer_);
    if (vertexBuffer_.isValid()) renderGraph->deleteBuffer(vertexBuffer_);
    if (geometryIndirect_.isValid()) renderGraph->deleteBuffer(geometryIndirect_);
    if (clearIndirect_.isValid()) renderGraph->deleteBuffer(clearIndirect_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void update(Settings settings)
  {
    settings_ = settings;
    settings_.resolution = std::max(settings_.resolution, 1u);
    if (uniforms_.isValid()) upload();
  }

  bool updateMesh(std::vector<Vertex> vertices,
      std::vector<uint32_t> indices)
  {
    if (vertices.size() != vertices_.size() ||
        indices.size() != indices_.size())
      return false;
    vertices_ = std::move(vertices);
    indices_ = std::move(indices);
    if (vertexBuffer_.isValid() && !vertices_.empty())
      renderGraph->bufferWrite(vertexBuffer_, 0u,
          vertices_.size() * sizeof(Vertex), vertices_.data());
    if (indexBuffer_.isValid() && !indices_.empty())
      renderGraph->bufferWrite(indexBuffer_, 0u,
          indices_.size() * sizeof(uint32_t), indices_.data());
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    upload();
    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(
        0, 0, settings_.resolution, settings_.resolution);
    pass.viewport = ::rendering::Viewport(
        settings_.resolution, settings_.resolution);
    pass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(depth_),
      .clearDepth = 0.0f,
      .clearStencil = 0u,
      .enabled = true,
    };
    commands.cmdBindGraphicsPipeline(clearPipeline_);
    commands.cmdBeginRenderPass(pass);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDrawIndirect(
        {.buffer = clearIndirect_, .offset = 0u,
         .size = sizeof(DrawIndirectGPU),
         .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ},
        0u, 1u, sizeof(DrawIndirectGPU));
    commands.cmdBindGraphicsPipeline(geometryPipeline_);
    if (!vertices_.empty() && !indices_.empty())
    {
      commands.cmdBindVertexBuffer(0u,
          {.buffer = vertexBuffer_, .offset = 0u,
           .size = vertices_.size() * sizeof(Vertex),
           .access = ::rendering::AccessPattern::VERTEX_ATTRIBUTE_READ});
      commands.cmdBindIndexBuffer(
          {.buffer = indexBuffer_, .offset = 0u,
           .size = indices_.size() * sizeof(uint32_t),
           .access = ::rendering::AccessPattern::INDEX_READ},
          ::rendering::Type_Uint32);
      commands.cmdDrawIndexedIndirect(
          {.buffer = geometryIndirect_, .offset = 0u,
           .size = sizeof(DrawIndexedIndirectGPU),
           .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ},
          0u, 1u, sizeof(DrawIndexedIndirectGPU));
    }
    commands.cmdEndRenderPass();
  }

private:
  struct DrawIndirectGPU
  {
    uint32_t vertexCount = 0u;
    uint32_t instanceCount = 1u;
    uint32_t firstVertex = 0u;
    uint32_t firstInstance = 0u;
  };
  static_assert(sizeof(DrawIndirectGPU) == 16u);

  struct DrawIndexedIndirectGPU
  {
    uint32_t indexCount = 0u;
    uint32_t instanceCount = 1u;
    uint32_t firstIndex = 0u;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0u;
  };
  static_assert(sizeof(DrawIndexedIndirectGPU) == 20u);

  struct alignas(16) UniformsGPU
  {
    std::array<float, 4> tangentX{};
    std::array<float, 4> tangentUp{};
    std::array<float, 4> tangentZ{};
    std::array<float, 4> tangentCamera{};
    std::array<float, 4> sourceRadiusExtent{};
    std::array<float, 4> directionTowardSun{};
    std::array<float, 4> localUp{};
  };
  static_assert(sizeof(UniformsGPU) == 112u);

  static ::rendering::TextureView depthView(
      const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ |
                ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
            .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
            .index = 0u,
            .flags = ::rendering::ImageAspectFlags::Depth,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  void upload()
  {
    const UniformsGPU values{settings_.tangentX, settings_.tangentUp,
        settings_.tangentZ, settings_.tangentCamera,
        settings_.sourceRadiusExtent, settings_.directionTowardSun,
        settings_.localUp};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(values),
        const_cast<UniformsGPU *>(&values));
    const bool draw = settings_.coverageAvailable && settings_.rebuild &&
        !vertices_.empty() && !indices_.empty();
    const DrawIndirectGPU clear{draw ? 3u : 0u, 1u, 0u, 0u};
    const DrawIndexedIndirectGPU geometry{
      draw ? static_cast<uint32_t>(indices_.size()) : 0u, 1u, 0u, 0, 0u};
    renderGraph->bufferWrite(clearIndirect_, 0u, sizeof(clear),
        const_cast<DrawIndirectGPU *>(&clear));
    renderGraph->bufferWrite(geometryIndirect_, 0u, sizeof(geometry),
        const_cast<DrawIndexedIndirectGPU *>(&geometry));
  }

  ::rendering::GraphicsPipeline makePipeline(const std::string &name,
      const std::string &vertexEntry,
      const ::rendering::ComparisonOp comparison,
      std::vector<::rendering::VertexLayoutElement> vertexLayout)
  {
    ::rendering::DepthAttatchment depth{};
    depth.enabled = true;
    depth.comparison = comparison;
    depth.format = ::rendering::Format::Format_Depth32Float;
    depth.loadOp = ::rendering::LoadOp::LoadOp_Load;
    depth.storeOp = ::rendering::StoreOp::StoreOp_Store;
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = name;
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = vertexEntry;
    pipeline.vertexStage.vertexLayoutElements = std::move(vertexLayout);
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "writeAtmosphereTerrainShadow";
    pipeline.fragmentStage.depthAttatchment = depth;
    return renderGraph->createGraphicsPipeline(pipeline);
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms", .size = sizeof(UniformsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    clearIndirect_ = renderGraph->createBuffer({
      .name = passName + "_ClearIndirect", .size = sizeof(DrawIndirectGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    geometryIndirect_ = renderGraph->createBuffer({
      .name = passName + "_GeometryIndirect",
      .size = sizeof(DrawIndexedIndirectGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    vertexBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Vertices",
      .size = std::max<uint64_t>(sizeof(Vertex),
          vertices_.size() * sizeof(Vertex)),
      .usage = ::rendering::BufferUsage::BufferUsage_Vertex |
          ::rendering::BufferUsage::BufferUsage_Push});
    indexBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Indices",
      .size = std::max<uint64_t>(sizeof(uint32_t),
          indices_.size() * sizeof(uint32_t)),
      .usage = ::rendering::BufferUsage::BufferUsage_Index |
          ::rendering::BufferUsage::BufferUsage_Push});
    if (!vertices_.empty())
      renderGraph->bufferWrite(vertexBuffer_, 0u,
          vertices_.size() * sizeof(Vertex), vertices_.data());
    if (!indices_.empty())
      renderGraph->bufferWrite(indexBuffer_, 0u,
          indices_.size() * sizeof(uint32_t), indices_.data());
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {{.name = "uniforms", .binding = 0u,
          .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex,
          .isDynamic = false}},
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups", .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {{.binding = 0u,
          .bufferView = {.buffer = uniforms_, .offset = 0u,
            .size = sizeof(UniformsGPU),
            .access = ::rendering::AccessPattern::UNIFORM_READ}}},
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader", .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-atmosphere-terrain-shadow.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    clearPipeline_ = makePipeline(passName + "_ClearPipeline",
        "clearAtmosphereTerrainShadow",
        ::rendering::ComparisonOp::ComparisonOp_Always, {});
    geometryPipeline_ = makePipeline(passName + "_GeometryPipeline",
        "drawAtmosphereTerrainShadow",
        ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual,
        {{.name = "directionElevation",
           .type = ::rendering::Type_Float32x4, .binding = 0u,
           .offset = 0u, .location = 0u},
         {.name = "surfaceCoordinate",
           .type = ::rendering::Type_Float32x4, .binding = 0u,
           .offset = 16u, .location = 1u}});
  }

  ::rendering::Texture depth_{};
  std::vector<Vertex> vertices_;
  std::vector<uint32_t> indices_;
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer clearIndirect_{};
  ::rendering::Buffer geometryIndirect_{};
  ::rendering::Buffer vertexBuffer_{};
  ::rendering::Buffer indexBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline clearPipeline_{};
  ::rendering::GraphicsPipeline geometryPipeline_{};
};

} // namespace voxel::svraster::rendering
