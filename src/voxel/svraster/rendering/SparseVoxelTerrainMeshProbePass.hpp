#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

// One resident terrain page's mesh placement. Mirrors TerrainMeshProbeChunk in
// sparse_voxel_terrain_mesh_probe.wgsl.
struct alignas(16) SparseVoxelTerrainMeshChunkGPU
{
  std::array<float, 4> centreEdgeVoxel{};
  std::array<float, 4> finerBounds{};
  std::array<uint32_t, 4> rasterOffsetLod{};
};
static_assert(sizeof(SparseVoxelTerrainMeshChunkGPU) == 48u);

struct alignas(16) SparseVoxelTerrainMeshProbeUniformsGPU
{
  std::array<float, 16> worldToClip{};
  std::array<uint32_t, 4> viewportCounting{};
  std::array<uint32_t, 4> rasterLayout{};
  std::array<float, 4> heightBiasFar{};
  std::array<uint32_t, 4> sampleLayout{};
  std::array<float, 16> cameraToWorld{};
  std::array<float, 4> cameraWorldPositionNear{};
  std::array<float, 4> focalPrincipal{};
};
static_assert(sizeof(SparseVoxelTerrainMeshProbeUniformsGPU) == 224u);

// T0 instrument: rasterizes the resident terrain height field as a mesh under
// the scene camera into throwaway targets, counting fragment invocations per
// pixel into the same per-pixel counter the brick overdraw view uses. Nothing
// downstream reads its attachments; only the counter leaves the pass.
class SparseVoxelTerrainMeshProbePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    // Per resident chunk placement, rebuilt whenever residency changes.
    ::rendering::Buffer chunks;
    // Concatenated per-chunk height rasters, halo included.
    ::rendering::Buffer heights;
    // Per-pixel fragment counter, drained by the brick resolver.
    ::rendering::Buffer overdraw;
    // Heights the vertex stage actually sampled, for the CPU comparison.
    ::rendering::Buffer samples;
    // Contract mode only: two packed words per interior column, and the
    // conservative mesh envelope parallel to `heights`.
    ::rendering::Buffer columnMaterials;
    ::rendering::Buffer envelope;
  };

  struct Targets
  {
    // Probe mode: throwaway targets, because the measurement is of the mesh
    // alone and sharing the brick depth would let brick geometry early-Z away
    // the very fragments being counted.
    //
    // Contract mode: the brick raster's own attachments, loaded rather than
    // cleared, so the mesh and the brick geometry compose through one depth
    // buffer and the existing resolver publishes both.
    ::rendering::Texture color;
    ::rendering::Texture depth;
    ::rendering::Texture bottomAlbedo;
    ::rendering::Texture microPayload;
    ::rendering::Texture materialSampleAddress;
  };

  struct Capacities
  {
    uint32_t chunkCapacity = 1u;
    uint32_t heightCapacity = 1u;
    uint32_t overdrawCount = 1u;
    uint32_t sampleCount = 1u;
    uint32_t columnMaterialCount = 1u;
  };

  struct Settings
  {
    SparseVoxelTerrainMeshProbeUniformsGPU uniforms{};
    // Six vertices per mesh quad of one chunk. Quads are not indexed so a
    // culled one can collapse without tearing its neighbours.
    uint32_t verticesPerChunk = 0u;
    uint32_t chunkCount = 0u;
    // Publish the brick resolver's contract instead of a fragment counter.
    bool publishContract = false;
    // Which mesh faces survive rasterization. A height field seen from above
    // its surface presents its back faces as the *exits* of the mesh solid, and
    // a fragment that starts inside the solid can only duplicate the march the
    // entry fragment already ran. Culling them is therefore the first place to
    // look for fragments the carve does not need - but only while the camera is
    // above the envelope, since underground the exits are the entries.
    ::rendering::CullMode cullMode = ::rendering::CullMode::None;
    ::rendering::WindingOrder winding = ::rendering::WindingOrder::CCW;
  };

  SparseVoxelTerrainMeshProbePass(
      const Buffers buffers, const Targets targets,
      const Capacities capacities, Settings settings)
      : buffers_(buffers), targets_(targets), capacities_(capacities),
        settings_(std::move(settings))
  {
  }

  ~SparseVoxelTerrainMeshProbePass() override
  {
    if (samplePipeline_.isValid())
      renderGraph->deleteComputePipeline(samplePipeline_);
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

  void updateUniforms(const SparseVoxelTerrainMeshProbeUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    // Command buffers are recorded once at registration, so the per-frame
    // write has to happen here as well: the uniform buffer is frame-local and
    // a slot never written holds zeros, which reads as a zero chunk count and
    // draws nothing at all.
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(
          uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  void updateChunkCount(const uint32_t chunkCount)
  {
    settings_.chunkCount = chunkCount;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    const uint32_t width = std::max(settings_.uniforms.viewportCounting[0], 1u);
    const uint32_t height = std::max(settings_.uniforms.viewportCounting[1], 1u);

    ::rendering::RenderPassInfo pass{};
    pass.name = passName + "_RenderPass";
    pass.scissor = ::rendering::Rect2D(0, 0, width, height);
    pass.viewport = ::rendering::Viewport(width, height);
    pass.colorAttachments = {colorAttachment("Color", targets_.color)};
    if (settings_.publishContract)
      pass.colorAttachments = {
        colorAttachment("Surface", targets_.color),
        colorAttachment("BottomAlbedo", targets_.bottomAlbedo),
        colorAttachment("MicroPayload", targets_.microPayload),
        colorAttachment(
            "MaterialSampleAddress", targets_.materialSampleAddress)};
    pass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = {
        .texture = targets_.depth,
        .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ |
            ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
        .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
      },
      // Reverse-Z: the far value is 0.0, matching every other depth target
      // here, so "nearest wins" is GreaterOrEqual.
      .clearDepth = 0.0f,
      .clearStencil = 0u,
      .enabled = true,
    };
    // Publish the sampled heights before the render pass opens: the gate is
    // about the raster the mesh is built from, and it must be measured whether
    // or not any of that mesh survives to be drawn.
    if (settings_.uniforms.sampleLayout[0] != 0u && settings_.chunkCount != 0u)
    {
      commands.cmdBindComputePipeline(samplePipeline_);
      commands.cmdBindBindingGroups(groups_, nullptr, 0u);
      const uint32_t sampleTotal =
          settings_.uniforms.sampleLayout[0] * settings_.chunkCount;
      commands.cmdDispatch((sampleTotal + 63u) / 64u, 1u, 1u);
    }
    commands.cmdBindGraphicsPipeline(pipeline_);
    commands.cmdBeginRenderPass(pass);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (settings_.chunkCount != 0u && settings_.verticesPerChunk != 0u)
      commands.cmdDraw(
          settings_.verticesPerChunk, settings_.chunkCount, 0u, 0u);
    commands.cmdEndRenderPass();
  }

private:
  ::rendering::ColorAttachmentInfo colorAttachment(
      const char *suffix, const ::rendering::Texture texture) const
  {
    return {
      .name = passName + "_" + suffix,
      .view = {
        .texture = texture,
        .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
      },
      .clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f),
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(SparseVoxelTerrainMeshProbeUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name,
                             const ::rendering::BindingVisibility visibility)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = visibility};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::
                  BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::
                      BindingVisibility_Vertex |
                  ::rendering::BindingVisibility::BindingVisibility_Fragment |
                  ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(1u, "chunks",
                 static_cast<::rendering::BindingVisibility>(
                     ::rendering::BindingVisibility::BindingVisibility_Vertex |
                     ::rendering::BindingVisibility::
                         BindingVisibility_Compute)),
             storage(2u, "heights",
                 static_cast<::rendering::BindingVisibility>(
                     ::rendering::BindingVisibility::BindingVisibility_Vertex |
                     ::rendering::BindingVisibility::
                         BindingVisibility_Compute)),
             storage(3u, "overdraw",
                 ::rendering::BindingVisibility::BindingVisibility_Fragment),
             storage(4u, "probeSamples",
                 ::rendering::BindingVisibility::BindingVisibility_Compute),
             storage(5u, "columnMaterials",
                 ::rendering::BindingVisibility::BindingVisibility_Fragment),
             storage(6u, "envelope",
                 static_cast<::rendering::BindingVisibility>(
                     ::rendering::BindingVisibility::BindingVisibility_Vertex |
                     ::rendering::BindingVisibility::
                         BindingVisibility_Fragment)),
           }}}});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = {.buffer = uniformBuffer_,
                .offset = 0u,
                .size = sizeof(SparseVoxelTerrainMeshProbeUniformsGPU),
                .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u,
              .bufferView = {.buffer = buffers_.chunks,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.chunkCapacity, 1u)} *
                    sizeof(SparseVoxelTerrainMeshChunkGPU),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 2u,
              .bufferView = {.buffer = buffers_.heights,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.heightCapacity, 1u)} *
                    sizeof(float),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 3u,
              .bufferView = {.buffer = buffers_.overdraw,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.overdrawCount, 1u)} *
                    sizeof(uint32_t),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 5u,
              .bufferView = {.buffer = buffers_.columnMaterials,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.columnMaterialCount, 1u)} *
                    sizeof(uint32_t),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 6u,
              .bufferView = {.buffer = buffers_.envelope,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.heightCapacity, 1u)} *
                    sizeof(float),
                .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 4u,
              .bufferView = {.buffer = buffers_.samples,
                .offset = 0u,
                .size = uint64_t{std::max(capacities_.sampleCount, 1u)} *
                    sizeof(float),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             settings_.publishContract
                 ? "assets/shaders/spirv/svraster/"
                   "sparse-voxel-terrain-mesh-contract.spirv"
                 : "assets/shaders/spirv/svraster/"
                   "sparse-voxel-terrain-mesh-probe.spirv"),
         .type = ::rendering::ShaderType::SpirV});

    ::rendering::DepthAttatchment depth{};
    depth.enabled = true;
    depth.comparison = ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual;
    depth.format = ::rendering::Format::Format_Depth32Float;
    // Contract mode draws into the brick raster's depth after its phases have
    // run, so the two compose by depth test rather than one clearing the other.
    depth.loadOp = settings_.publishContract
        ? ::rendering::LoadOp::LoadOp_Load
        : ::rendering::LoadOp::LoadOp_Clear;
    depth.storeOp = ::rendering::StoreOp::StoreOp_Store;

    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawTerrainMeshProbeVertex";
    pipeline.vertexStage.vertexLayoutElements = {};
    pipeline.vertexStage.cullType = settings_.cullMode;
    pipeline.vertexStage.winding = settings_.winding;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = settings_.publishContract
        ? "drawTerrainMeshContractFragment"
        : "drawTerrainMeshProbeFragment";
    const auto attachment = [this](const ::rendering::Format format)
    {
      return ::rendering::ColorAttatchment{
        .format = format,
        .loadOp = settings_.publishContract
            ? ::rendering::LoadOp::LoadOp_Load
            : ::rendering::LoadOp::LoadOp_Clear,
        .storeOp = ::rendering::StoreOp::StoreOp_Store,
        .blendMode = ::rendering::ColorBlendMode::Replace,
        .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
        .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      };
    };
    // Same order and formats as BrickFragmentOutput and the brick draw's frame
    // target, because the resolver reads them by location.
    pipeline.fragmentStage.colorAttatchments = settings_.publishContract
        ? std::vector<::rendering::ColorAttatchment>{
              attachment(::rendering::Format::Format_RGBA32Uint),
              attachment(::rendering::Format::Format_R32Uint),
              attachment(::rendering::Format::Format_R32Uint),
              attachment(::rendering::Format::Format_RGBA32Uint)}
        : std::vector<::rendering::ColorAttatchment>{
              attachment(::rendering::Format::Format_RGBA8Unorm)};
    pipeline.fragmentStage.depthAttatchment = depth;
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
    samplePipeline_ = renderGraph->createComputePipeline(
        {.name = passName + "_SamplePipeline",
         .shader = shader_,
         .entry = "sampleTerrainMeshProbeHeights",
         .layout = layout_});
  }

  Buffers buffers_{};
  Targets targets_{};
  Capacities capacities_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};
  ::rendering::ComputePipeline samplePipeline_{};
};

} // namespace voxel::svraster::rendering
