#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/material/VoxelGeometryIR.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGeometryFieldCache.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/SparseVoxelMicroPreparePass.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Culling, LOD-history commit, indirect-argument preparation and counter reset
// for the brick hierarchy resolver. They share one shader and binding layout;
// the kernel selects which entry point runs.
class SparseVoxelBrickCullPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Reset,
    Cull,
    CommitLODHistory,
    PrepareDrawArgs,
  };

  struct Buffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer visibleBricks;
    ::rendering::Buffer counters;
    ::rendering::Buffer drawArgs;
    ::rendering::Buffer previouslyVisible;
    // Shared by both phases; phase zero's reset kernel clears it.
    ::rendering::Buffer microCounters;
    ::rendering::Buffer terrainPageRequestBits;
    ::rendering::Buffer terrainPageRequestPriorities;
    ::rendering::Buffer terrainPageStates;
    ::rendering::Buffer terrainPageRequestGenerations;
    ::rendering::Buffer terrainPageRequestDesiredLODs;
    ::rendering::Buffer lodGroups;
    ::rendering::Buffer nodeLODGroupIndices;
    ::rendering::Buffer representativeLODGroupIndices;
  };

  struct Settings
  {
    SparseVoxelBrickCullUniformsGPU uniforms{};
  };

  SparseVoxelBrickCullPass(const Kernel kernel, const Buffers buffers, const Settings settings, const ::rendering::Texture hzb)
      : kernel_(kernel), buffers_(buffers), settings_(settings), hzb_(hzb)
  {
  }

  ~SparseVoxelBrickCullPass() override
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
  }

  void updateUniforms(const SparseVoxelBrickCullUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::Cull)
      commandBuffer.cmdDispatch(std::max(1u, (settings_.uniforms.nodeCount + 63u) / 64u), 1u, 1u);
    else if (kernel_ == Kernel::CommitLODHistory)
      commandBuffer.cmdDispatch(std::max(1u, (settings_.uniforms.nodeCount + 63u) / 64u), 1u, 1u);
    else
      commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  const char *entryPoint() const
  {
    switch (kernel_)
    {
    case Kernel::Reset:
      return "resetBrickCounters";
    case Kernel::Cull:
      return "cullBricks";
    case Kernel::CommitLODHistory:
      return "commitBrickLODHistory";
    case Kernel::PrepareDrawArgs:
      return "prepareBrickDrawArgs";
    }
    return "";
  }

  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(SparseVoxelBrickCullUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(1u, "nodes"),
             storage(2u, "visibleBricks"),
             storage(3u, "counters"),
             storage(4u, "drawArgs"),
             storage(6u, "previouslyVisible"),
             storage(7u, "microCounters"),
             storage(8u, "terrainPageRequestBits"),
             storage(9u, "terrainPageRequestPriorities"),
             storage(10u, "terrainPageStates"),
             storage(11u, "terrainPageRequestGenerations"),
             storage(12u, "terrainPageRequestDesiredLODs"),
             storage(13u, "lodGroups"),
             storage(14u, "nodeLODGroupIndices"),
             storage(15u, "representativeLODGroupIndices"),
           },
           .textures = {{.name = "hzb", .binding = 5u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
         }}});
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    const ::rendering::AccessPattern readWrite = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = view(uniformBuffer_, sizeof(SparseVoxelBrickCullUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(buffers_.nodes, capacityBytes(settings_.uniforms.nodeCount, sizeof(SparseVoxelBrickNodeGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u, .bufferView = view(buffers_.visibleBricks, capacityBytes(settings_.uniforms.visibleCapacity, sizeof(uint32_t)), readWrite)},
             // SparseVoxelBrickCounters has sixteen atomic words. Keeping this
             // range at the original four shipping counters made the shader's
             // occlusion attribution writes fall outside its declared storage
             // view, which is undefined on Vulkan and can corrupt adjacent
             // resources when diagnostics are enabled.
             {.binding = 3u, .bufferView = view(buffers_.counters, sizeof(uint32_t) * 16u, readWrite)},
             {.binding = 4u, .bufferView = view(buffers_.drawArgs, sizeof(SparseVoxelBrickDrawIndirectGPU), readWrite)},
             {.binding = 6u, .bufferView = view(buffers_.previouslyVisible, capacityBytes(settings_.uniforms.nodeCount, sizeof(uint32_t)), readWrite)},
             {.binding = 7u, .bufferView = view(buffers_.microCounters, sizeof(SparseVoxelMicroCountersGPU), readWrite)},
             {.binding = 8u, .bufferView = view(
                  buffers_.terrainPageRequestBits,
                  capacityBytes(
                      (settings_.uniforms.terrainPageRequestCapacity + 31u) /
                          32u,
                      sizeof(uint32_t)),
                  readWrite)},
             {.binding = 9u, .bufferView = view(
                  buffers_.terrainPageRequestPriorities,
                  capacityBytes(
                      settings_.uniforms.terrainPageRequestCapacity,
                      sizeof(uint32_t)),
                  readWrite)},
             {.binding = 10u, .bufferView = view(
                  buffers_.terrainPageStates,
                  capacityBytes(
                      settings_.uniforms.terrainPageRequestCapacity,
                      sizeof(SparseVoxelTerrainPageStateGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 11u, .bufferView = view(
                  buffers_.terrainPageRequestGenerations,
                  capacityBytes(
                      settings_.uniforms.terrainPageRequestCapacity,
                      sizeof(uint32_t)),
                  readWrite)},
             {.binding = 12u, .bufferView = view(
                  buffers_.terrainPageRequestDesiredLODs,
                  capacityBytes(
                      settings_.uniforms.terrainPageRequestCapacity,
                      sizeof(uint32_t)),
                  readWrite)},
             {.binding = 13u, .bufferView = view(
                  buffers_.lodGroups,
                  capacityBytes(
                      settings_.uniforms.lodGroupCount,
                      sizeof(SparseVoxelBrickLODGroupGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 14u, .bufferView = view(
                  buffers_.nodeLODGroupIndices,
                  capacityBytes(settings_.uniforms.nodeCount, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 15u, .bufferView = view(
                  buffers_.representativeLODGroupIndices,
                  capacityBytes(settings_.uniforms.nodeCount, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
           },
           .textures = {{.textureView = {.texture = hzb_, .access = ::rendering::AccessPattern::SHADER_READ, .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY, .index = 0u, .flags = ::rendering::ImageAspectFlags::Color, .baseArrayLayer = 0u, .baseMipLevel = 0u, .layerCount = 1u, .levelCount = std::max(1u, settings_.uniforms.hzbMipCount)}, .binding = 5u}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-brick-cull.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = entryPoint(), .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_;
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Texture hzb_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

// Rasterizes three faces per visible brick and DDAs each fragment. Depth
// testing resolves which brick owns a pixel, so the attachments it writes are
// already the nearest surface.
class SparseVoxelBrickDrawPass final : public ::rendering::Pass
{
public:
  enum class RasterSource
  {
    FullBrick,
    MicroResidual,
    PreparedMicroParent,
    // Rare boundary-tie and exterior side-entry samples only. Replays
    // incumbent ownership after the R2 direct pass discards the ambiguity.
    PreparedMicroTieReplay,
  };

  // One indirect draw. Every source in a pass shares the render pass, its
  // attachments and their load/store operations; each brings its own shader,
  // binding layout, indirect arguments and per-source input buffer.
  struct Source
  {
    RasterSource rasterSource = RasterSource::FullBrick;
    ::rendering::Buffer drawArgs;
    // MicroResidual: SparseVoxelMicroResidualGPU by visible-list slot.
    // Prepared*: compact SparseVoxelMicroBrickRecordGPU records.
    ::rendering::Buffer microRasterInput;
    uint32_t microRasterInputCount = 1u;
  };

  struct Buffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    // One palette index per occupied voxel, four packed to a word.
    ::rendering::Buffer materials;
    // One normalized context byte plus four-corner patch per occupied voxel.
    ::rendering::Buffer microSurfaceData;
    // `paletteStride` records per instance, selected by the brick's own
    // instance index.
    ::rendering::Buffer palette;
    // Two 16-bit descriptor IDs per word, parallel to palette.
    ::rendering::Buffer paletteDescriptorIds;
    // Global deduplicated SparseVoxelMicroMaterialGPU records.
    ::rendering::Buffer microMaterials;
    // The same descriptors denormalized per palette address, so the hot path
    // avoids the scattered dependent load through a descriptor id.
    ::rendering::Buffer paletteMicroMaterials;
    ::rendering::Buffer geometryPrograms;
    ::rendering::Buffer geometryInstructions;
    ::rendering::Buffer geometryParameters;
    ::rendering::Buffer visibleBricks;
    // Per-pixel fragment invocation counter for the overdraw view. A 1-word
    // stub unless that view is selected.
    ::rendering::Buffer overdraw;
    // Shared frame-wide exact micro-geometry diagnostics.
    ::rendering::Buffer microCounters;
    ::rendering::Buffer materialFieldDefaults;
    ::rendering::Buffer geometryFieldCache;
  };

  struct FrameTarget
  {
    // RGBA32Uint, carrying the primary packed material, hit face/flags, optional
    // secondary index/modes, exact surface identity, and ray distance bit-cast
    // into the fourth channel. Depth is the only value hardware owns separately.
    ::rendering::Texture surface;
    // R32Uint bottom RGB resolved from the material's secondary palette index.
    // Keeping it separate preserves the full per-voxel temporal identity in
    // surface.z.
    ::rendering::Texture bottomAlbedo;
    // R32Uint exact micro-surface payload. Zero identifies a legacy/parent
    // hit; nonzero records descriptor and face-relative microcell identity.
    ::rendering::Texture microPayload;
    // RGBA32Uint canonical material-grid XYZ plus logical region mapping ID.
    ::rendering::Texture materialSampleAddress;
    // R32Uint logical terrain page identity. Kept separate from the material
    // address so ownership diagnostics cannot change material-field sampling.
    ::rendering::Texture terrainOwnershipId;
    ::rendering::Texture depth;
  };

  struct Capacities
  {
    uint32_t nodeCount = 1u;
    uint32_t occupancyCount = 1u;
    // Words in the packed index array, not voxels.
    uint32_t materialCount = 1u;
    uint32_t microSurfaceCount = 1u;
    uint32_t visibleCapacity = 1u;
    // Pixels in the overdraw counter, or 1 when the view is not built.
    uint32_t overdrawCount = 1u;
    uint32_t paletteCount = 1u;
    uint32_t paletteStride = 256u;
    uint32_t paletteDescriptorWordCount = 1u;
    uint32_t microMaterialCount = 1u;
    uint32_t geometryProgramCount = 1u;
    uint32_t geometryInstructionCount = 1u;
    uint32_t geometryParameterCount = 1u;
    uint32_t materialFieldDefaultCount = 1u;
    uint32_t geometryFieldCacheCount = 1u;
  };

  struct Settings
  {
    SparseVoxelBrickDrawUniformsGPU uniforms{};
    // Reverse-Z far value, matching the G-buffer's own depth clear.
    float clearDepth = 0.0f;
    // Phase two draws into phase one's depth and attachments rather than
    // clearing them, so the two halves compose into one image.
    bool loadPrevious = false;
    // Selects a separately compiled shader. False is the legacy/default SPIR-V
    // and contains no distance comparison in its fragment fast path.
    bool exactMicroDistanceCutoff = false;
    // Selects the exact-micro code path at all. When false, the fragment
    // shader contains only the parent-voxel DDA used by 10 cm terrain.
    bool exactMicroGeometry = true;
    // Generated terrain uses the parent-voxel DDA too, but needs a more
    // conservative late-depth proxy near the eye plane than small authored
    // no-micro test scenes. This selects that terrain-specific SPIR-V.
    bool terrainOccupancyOnly = false;
    // Selects the opt-in shader containing fragment-side traversal/overdraw
    // counters. The shipping variants compile those counters out completely.
    bool rasterDiagnostics = false;
    // Drawn in order, all inside a *single* render pass. More than one source
    // therefore costs one attachment load/store cycle rather than one per
    // draw, which on a tile GPU is the whole reason the option exists.
    std::vector<Source> sources{};
  };

  SparseVoxelBrickDrawPass(const Buffers buffers, const FrameTarget target, const Capacities capacities, const Settings settings)
      : buffers_(buffers), target_(target), capacities_(capacities), settings_(settings)
  {
    if (settings_.sources.empty())
      throw std::runtime_error(
          "SparseVoxelBrickDrawPass needs at least one raster source");
  }

  ~SparseVoxelBrickDrawPass() override
  {
    for (Variant &variant : variants_)
    {
      if (variant.pipeline.isValid())
        renderGraph->deleteGraphicsPipeline(variant.pipeline);
      if (variant.shader.isValid())
        renderGraph->deleteShader(variant.shader);
      if (variant.groups.isValid())
        renderGraph->deleteBindingGroups(variant.groups);
      if (variant.layout.isValid())
        renderGraph->deleteBindingsLayout(variant.layout);
    }
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void updateUniforms(const SparseVoxelBrickDrawUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    const uint32_t width = settings_.uniforms.viewport[0];
    const uint32_t height = settings_.uniforms.viewport[1];

    ::rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = ::rendering::Rect2D(0, 0, width, height);
    renderPass.viewport = ::rendering::Viewport(width, height);
    renderPass.colorAttachments = {
      colorAttachment("Surface", target_.surface),
      colorAttachment("BottomAlbedo", target_.bottomAlbedo),
      colorAttachment("MicroPayload", target_.microPayload),
      colorAttachment("MaterialSampleAddress", target_.materialSampleAddress),
      colorAttachment("TerrainOwnershipId", target_.terrainOwnershipId),
    };
    renderPass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(target_.depth),
      .clearDepth = settings_.clearDepth,
      .clearStencil = 0u,
      .enabled = true,
    };
    // cmdBeginRenderPass derives its render pass from the *bound* pipeline, so
    // the first variant has to be bound before the pass opens. Every variant is
    // built with the same attachment load/store operations, which keeps them
    // render-pass compatible and lets the rest bind inside the open pass. The
    // attachments are therefore cleared once and the draws compose through the
    // shared depth test exactly as the separate passes did.
    commandBuffer.cmdBindGraphicsPipeline(variants_.front().pipeline);
    commandBuffer.cmdBeginRenderPass(renderPass);
    for (size_t index = 0u; index < variants_.size(); ++index)
    {
      const Variant &variant = variants_[index];
      if (index != 0u)
        commandBuffer.cmdBindGraphicsPipeline(variant.pipeline);
      commandBuffer.cmdBindBindingGroups(variant.groups, nullptr, 0u);
      commandBuffer.cmdDrawIndirect(
          {.buffer = variant.source.drawArgs, .offset = 0u, .size = sizeof(SparseVoxelBrickDrawIndirectGPU), .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ},
          0u,
          1u,
          sizeof(SparseVoxelBrickDrawIndirectGPU));
    }
    commandBuffer.cmdEndRenderPass();
  }

private:
  // Everything one Source needs. The uniform buffer stays shared because all
  // sources rasterize the same view.
  struct Variant
  {
    Source source{};
    ::rendering::BindingsLayout layout{};
    ::rendering::BindingGroups groups{};
    ::rendering::Shader shader{};
    ::rendering::GraphicsPipeline pipeline{};
  };

  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  ::rendering::TextureView colorView(const ::rendering::Texture texture) const
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
      .levelCount = 1u,
    };
  }

  ::rendering::TextureView depthView(const ::rendering::Texture texture) const
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
      .levelCount = 1u,
    };
  }

  ::rendering::ColorAttachmentInfo colorAttachment(const char *suffix, const ::rendering::Texture texture) const
  {
    return {
      .name = passName + "_" + suffix,
      .view = colorView(texture),
      .clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f),
    };
  }

  ::rendering::ColorAttatchment colorPipelineAttachment(
      const ::rendering::Format format,
      const bool clearOnFirstPhase) const
  {
    return {
      .format = format,
      .loadOp = settings_.loadPrevious
          ? ::rendering::LoadOp::LoadOp_Load
          : clearOnFirstPhase
              ? ::rendering::LoadOp::LoadOp_Clear
              : ::rendering::LoadOp::LoadOp_DontCare,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    };
  }

  void ensureResources()
  {
    if (!variants_.empty())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms",
         .size = sizeof(SparseVoxelBrickDrawUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    variants_.reserve(settings_.sources.size());
    for (size_t index = 0u; index < settings_.sources.size(); ++index)
      variants_.push_back(createVariant(
          settings_.sources[index],
          settings_.sources.size() == 1u
              ? std::string{}
              : "_" + std::to_string(index)));
  }

  Variant createVariant(const Source &source, const std::string &suffix)
  {
    Variant variant{};
    variant.source = source;
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex | ::rendering::BindingVisibility::BindingVisibility_Fragment};
    };
    // The R2 direct module has its own deliberately small interface. Filtering
    // the C++ layout as well as the SPIR-V avoids false RenderGraph lifetimes
    // and read/write hazards on legacy traversal and diagnostic buffers.
    const auto directPreparedUsesBinding = [this](const uint32_t binding)
    {
      switch (binding)
      {
      case 0u:  // uniforms
      case 1u:  // nodes
      case 6u:  // brickPalette
      case 8u:  // microMaterials
      case 11u: // voxelGeometryPrograms
      case 12u: // voxelGeometryInstructions
      case 13u: // voxelGeometryParameters
      case 14u: // voxelMaterialFieldDefaults
      case 16u: // microBrickRecords
      case 17u: // brickPaletteMicroMaterials
        return true;
      case 5u: // overdraw
      case 9u: // microCounters
        return settings_.rasterDiagnostics;
      default:
        return false;
      }
    };
    std::vector<::rendering::BindingGroupLayoutBufferEntry> layoutBuffers{
             {.name = "uniforms",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Vertex | ::rendering::BindingVisibility::BindingVisibility_Fragment},
             storage(1u, "nodes"),
             storage(2u, "occupancy"),
             storage(3u, "brickMaterialIndices"),
             storage(4u, "visibleBricks"),
             storage(5u, "overdraw"),
             storage(6u, "brickPalette"),
             storage(7u, "brickPaletteDescriptorIds"),
             storage(8u, "microMaterials"),
             storage(9u, "microCounters"),
             storage(10u, "brickMicroSurfaceData"),
             storage(11u, "voxelGeometryPrograms"),
             storage(12u, "voxelGeometryInstructions"),
             storage(13u, "voxelGeometryParameters"),
             storage(14u, "voxelMaterialFieldDefaults"),
             storage(15u, "voxelGeometryFieldCache"),
             storage(17u, "brickPaletteMicroMaterials"),
    };
    if (source.rasterSource != RasterSource::FullBrick)
      layoutBuffers.push_back(storage(
          16u,
          source.rasterSource == RasterSource::MicroResidual
              ? "microResiduals"
              : "microBrickRecords"));
    if (source.rasterSource == RasterSource::PreparedMicroParent)
      layoutBuffers.erase(
          std::remove_if(
              layoutBuffers.begin(), layoutBuffers.end(),
              [&](const auto &entry)
              { return !directPreparedUsesBinding(entry.binding); }),
          layoutBuffers.end());
    variant.layout = renderGraph->createBindingsLayout(
        {.name = passName + suffix + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = std::move(layoutBuffers)}}});
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = ::rendering::AccessPattern::SHADER_READ};
    };
    std::vector<::rendering::BindingBuffer> groupBuffers{
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelBrickDrawUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u, .bufferView = view(buffers_.nodes, capacityBytes(capacities_.nodeCount, sizeof(SparseVoxelBrickNodeGPU)))},
             {.binding = 2u, .bufferView = view(buffers_.occupancy, capacityBytes(capacities_.occupancyCount, sizeof(SparseVoxelBrickOccupancyGPU)))},
             {.binding = 3u, .bufferView = view(buffers_.materials, capacityBytes(capacities_.materialCount, sizeof(uint32_t)))},
             {.binding = 4u, .bufferView = view(buffers_.visibleBricks, capacityBytes(capacities_.visibleCapacity, sizeof(uint32_t)))},
             {.binding = 5u,
              .bufferView = {
                .buffer = buffers_.overdraw,
                .offset = 0u,
                .size = capacityBytes(capacities_.overdrawCount, sizeof(uint32_t)),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 6u, .bufferView = view(buffers_.palette, capacityBytes(capacities_.paletteCount, sizeof(SparseVoxelBrickMaterialGPU)))},
             {.binding = 7u, .bufferView = view(buffers_.paletteDescriptorIds, capacityBytes(capacities_.paletteDescriptorWordCount, sizeof(uint32_t)))},
             {.binding = 8u, .bufferView = view(buffers_.microMaterials, capacityBytes(capacities_.microMaterialCount, sizeof(SparseVoxelMicroMaterialGPU)))},
             {.binding = 17u, .bufferView = view(buffers_.paletteMicroMaterials, capacityBytes(capacities_.paletteCount, 4u * sizeof(uint32_t)))},
             {.binding = 9u,
              .bufferView = {
                .buffer = buffers_.microCounters,
                .offset = 0u,
                .size = sizeof(SparseVoxelMicroCountersGPU),
                .access = ::rendering::AccessPattern::SHADER_READ |
                    ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 10u, .bufferView = view(
                 buffers_.microSurfaceData,
                 capacityBytes(
                     capacities_.microSurfaceCount, sizeof(uint64_t)))},
             {.binding = 11u, .bufferView = view(
                 buffers_.geometryPrograms,
                 capacityBytes(capacities_.geometryProgramCount,
                     sizeof(::voxel::material::VoxelGeometryProgramGPU)))},
             {.binding = 12u, .bufferView = view(
                 buffers_.geometryInstructions,
                 capacityBytes(capacities_.geometryInstructionCount,
                     sizeof(::voxel::material::VoxelGeometryInstructionGPU)))},
             {.binding = 13u, .bufferView = view(
                 buffers_.geometryParameters,
                 capacityBytes(capacities_.geometryParameterCount,
                     sizeof(::voxel::material::VoxelGeometryParameterBlockGPU)))},
             {.binding = 14u, .bufferView = view(
                 buffers_.materialFieldDefaults,
                 capacityBytes(capacities_.materialFieldDefaultCount,
                     sizeof(::voxel::material::VoxelMaterialFieldDefaultsGPU)))},
             {.binding = 15u, .bufferView = view(
                 buffers_.geometryFieldCache,
                 capacityBytes(capacities_.geometryFieldCacheCount,
                     sizeof(SparseVoxelGeometryFieldCacheRecordGPU)))},
    };
    if (source.rasterSource != RasterSource::FullBrick)
    {
      const uint64_t stride =
          source.rasterSource == RasterSource::MicroResidual
          ? sizeof(SparseVoxelMicroResidualGPU)
          : sizeof(SparseVoxelMicroBrickRecordGPU);
      groupBuffers.push_back(
          {.binding = 16u,
           .bufferView = view(
               source.microRasterInput,
               capacityBytes(source.microRasterInputCount, stride))});
    }
    if (source.rasterSource == RasterSource::PreparedMicroParent)
      groupBuffers.erase(
          std::remove_if(
              groupBuffers.begin(), groupBuffers.end(),
              [&](const auto &entry)
              { return !directPreparedUsesBinding(entry.binding); }),
          groupBuffers.end());
    variant.groups = renderGraph->createBindingGroups(
        {.layout = variant.layout,
         .name = passName + suffix + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = std::move(groupBuffers)}}});
    const char *shaderPath = nullptr;
    if (source.rasterSource == RasterSource::MicroResidual)
    {
      if (settings_.rasterDiagnostics)
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-residual-micro-distance-diagnostics.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-residual-diagnostics.spirv";
      else
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-residual-micro-distance.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-residual.spirv";
    }
    else if (source.rasterSource == RasterSource::PreparedMicroTieReplay)
    {
      if (settings_.rasterDiagnostics)
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-tie-replay-micro-distance-diagnostics.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-tie-replay-diagnostics.spirv";
      else
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-tie-replay-micro-distance.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-tie-replay.spirv";
    }
    else if (source.rasterSource == RasterSource::PreparedMicroParent)
    {
      if (settings_.rasterDiagnostics)
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-micro-distance-diagnostics.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-diagnostics.spirv";
      else
        shaderPath = settings_.exactMicroDistanceCutoff
            ? "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared-micro-distance.spirv"
            : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw-prepared.spirv";
    }
    else if (!settings_.exactMicroGeometry)
    {
      if (settings_.terrainOccupancyOnly)
        shaderPath = settings_.rasterDiagnostics
            ? "assets/shaders/spirv/svraster/"
              "sparse-voxel-brick-draw-no-micro-terrain-diagnostics.spirv"
            : "assets/shaders/spirv/svraster/"
              "sparse-voxel-brick-draw-no-micro-terrain.spirv";
      else
        shaderPath = settings_.rasterDiagnostics
            ? "assets/shaders/spirv/svraster/"
              "sparse-voxel-brick-draw-no-micro-diagnostics.spirv"
            : "assets/shaders/spirv/svraster/"
              "sparse-voxel-brick-draw-no-micro.spirv";
    }
    else if (settings_.rasterDiagnostics)
      shaderPath = settings_.exactMicroDistanceCutoff
          ? "assets/shaders/spirv/svraster/"
            "sparse-voxel-brick-draw-micro-distance-diagnostics.spirv"
          : "assets/shaders/spirv/svraster/"
            "sparse-voxel-brick-draw-diagnostics.spirv";
    else
      shaderPath = settings_.exactMicroDistanceCutoff
          ? "assets/shaders/spirv/svraster/"
            "sparse-voxel-brick-draw-micro-distance.spirv"
          : "assets/shaders/spirv/svraster/sparse-voxel-brick-draw.spirv";
    variant.shader = renderGraph->createShader(
        {.name = passName + suffix + "_Shader",
         .layout = variant.layout,
         .src = os::io::readRelativeFile(shaderPath),
         .type = ::rendering::ShaderType::SpirV});

    ::rendering::DepthAttatchment depthAttachment{};
    depthAttachment.enabled = true;
    // Nearest brick wins the pixel - this replaces the tile resolver's
    // per-candidate hitPrecedes() ordering. The renderer is reverse-Z (the
    // G-buffer clears depth to 0.0), so "nearest" is the *greater* depth and
    // the buffer clears to the far value. Getting this backwards lets distant
    // coarse bricks overwrite near geometry, which reads as huge black boxes
    // spanning the scene.
    // Equal-depth terrain faces occur at page, brick and LOD boundaries. With
    // GreaterOrEqual both fragments pass and the GPU may commit their five MRT
    // payloads in either order, so identity, material and normals flicker even
    // though depth itself is unchanged. Strict Greater makes the first raster
    // owner win deterministically. Do not rely on conservative early-Z for
    // terrain proxies: their exact DDA hit can differ from the raster envelope
    // at close/grazing angles.
    depthAttachment.comparison = settings_.exactMicroGeometry
        ? ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual
        : ::rendering::ComparisonOp::ComparisonOp_Greater;
    depthAttachment.format = ::rendering::Format::Format_Depth32Float;
    depthAttachment.loadOp =
        settings_.loadPrevious ? ::rendering::LoadOp::LoadOp_Load : ::rendering::LoadOp::LoadOp_Clear;
    depthAttachment.storeOp = ::rendering::StoreOp::StoreOp_Store;

    ::rendering::GraphicsPipelineInfo pipelineInfo{};
    pipelineInfo.name = passName + suffix + "_Pipeline";
    pipelineInfo.layout = variant.layout;
    pipelineInfo.vertexStage.vertexLayoutElements = {};
    pipelineInfo.vertexStage.vertexShader = variant.shader;
    pipelineInfo.vertexStage.shaderEntry = "drawBrickVertex";
    // The vertex shader already selects the three faces on the camera's side,
    // and the camera-inside case emits a full-screen triangle whose winding is
    // whatever it happens to be. Face culling would only remove fragments this
    // pass depends on.
    pipelineInfo.vertexStage.cullType = ::rendering::CullMode::None;
    pipelineInfo.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipelineInfo.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipelineInfo.fragmentStage.fragmentShader = variant.shader;
    pipelineInfo.fragmentStage.shaderEntry = "drawBrickFragment";
    // Keep this order identical to BrickFragmentOutput and FrameTarget:
    // surface, bottomAlbedo, microPayload, materialSampleAddress,
    // terrainOwnershipId.
    pipelineInfo.fragmentStage.colorAttatchments = {
      // Resolve reads the surface hit bit before touching the other targets.
      // Every surviving fragment writes all five outputs, so phase zero only
      // needs a defined miss value for surface. Avoid initializing 28 bytes
      // per pixel of auxiliary MRT data that is either overwritten or ignored.
      colorPipelineAttachment(
          ::rendering::Format::Format_RGBA32Uint, true),
      colorPipelineAttachment(
          ::rendering::Format::Format_R32Uint, false),
      colorPipelineAttachment(
          ::rendering::Format::Format_R32Uint, false),
      colorPipelineAttachment(
          ::rendering::Format::Format_RGBA32Uint, false),
      colorPipelineAttachment(
          ::rendering::Format::Format_R32Uint, false),
    };
    pipelineInfo.fragmentStage.depthAttatchment = depthAttachment;
    variant.pipeline = renderGraph->createGraphicsPipeline(pipelineInfo);
    return variant;
  }

  Buffers buffers_{};
  FrameTarget target_{};
  Capacities capacities_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  std::vector<Variant> variants_;
};

// Copies the rasterized attachments into resolvedSurfaces/coverage/linearDepth.
class SparseVoxelBrickResolvePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    // Per-pixel exact micro-surface payload, zero for every non-micro hit.
    ::rendering::Buffer microPayloads;
    ::rendering::Buffer materialSampleAddresses;
    ::rendering::Buffer terrainOwnershipIds;
    // The draw pass's counter, drained here, and the per-frame total this pass
    // publishes for the debug view. 1-word stubs unless the view is built.
    ::rendering::Buffer overdraw;
    ::rendering::Buffer overdrawResolved;
  };

  struct Capacities
  {
    uint32_t overdrawCount = 1u;
  };

  struct Textures
  {
    ::rendering::Texture surface;
    ::rendering::Texture bottomAlbedo;
    ::rendering::Texture microPayload;
    ::rendering::Texture materialSampleAddress;
    ::rendering::Texture terrainOwnershipId;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
    // Selects the diagnostic SPIR-V and exposes bindings 5/6. False keeps the
    // shipping resolver free of overdraw resources and atomic exchanges.
    bool overdrawDiagnostics = false;
  };

  SparseVoxelBrickResolvePass(
      const Buffers buffers, const Textures textures, const Settings settings,
      const Capacities capacities)
      : buffers_(buffers), textures_(textures), settings_(settings),
        capacities_(capacities)
  {
  }

  ~SparseVoxelBrickResolvePass() override
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
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.uniforms.viewportTiles[0] + 7u) / 8u),
        std::max(1u, (settings_.uniforms.viewportTiles[1] + 7u) / 8u),
        1u);
  }

private:
  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  ::rendering::TextureView readView(const ::rendering::Texture texture) const
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
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
        {.name = passName + "_Uniforms",
         .size = sizeof(SparseVoxelRasterUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    std::vector<::rendering::BindingGroupLayoutBufferEntry> layoutBuffers{
      {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      storage(1u, "resolvedSurfaces"),
      storage(2u, "coverage"),
      storage(3u, "linearDepth"),
    };
    if (settings_.overdrawDiagnostics)
    {
      layoutBuffers.push_back(storage(5u, "overdraw"));
      layoutBuffers.push_back(storage(6u, "overdrawResolved"));
    }
    layoutBuffers.push_back(storage(8u, "microPayloads"));
    layoutBuffers.push_back(storage(10u, "materialSampleAddresses"));
    layoutBuffers.push_back(storage(12u, "terrainOwnershipIds"));
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = std::move(layoutBuffers),
           .textures = {
             {.name = "surfaceTexture", .binding = 4u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "bottomAlbedoTexture", .binding = 7u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "microPayloadTexture", .binding = 9u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialSampleAddressTexture", .binding = 11u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "terrainOwnershipIdTexture", .binding = 13u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});
    const uint32_t pixelCapacity =
        std::max(1u, settings_.uniforms.viewportTiles[0] * settings_.uniforms.viewportTiles[1]);
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = ::rendering::AccessPattern::SHADER_WRITE};
    };
    std::vector<::rendering::BindingBuffer> groupBuffers{
      {.bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelRasterUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}, .binding = 0u},
      {.bufferView = view(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU))), .binding = 1u},
      {.bufferView = view(buffers_.coverage, capacityBytes(pixelCapacity, sizeof(uint32_t))), .binding = 2u},
      {.bufferView = view(buffers_.linearDepth, capacityBytes(pixelCapacity, sizeof(float))), .binding = 3u},
    };
    if (settings_.overdrawDiagnostics)
    {
      groupBuffers.push_back({
        .bufferView = {
          .buffer = buffers_.overdraw,
          .offset = 0u,
          .size = capacityBytes(
              capacities_.overdrawCount, sizeof(uint32_t)),
          .access = ::rendering::AccessPattern::SHADER_READ |
              ::rendering::AccessPattern::SHADER_WRITE},
        .binding = 5u});
      groupBuffers.push_back({
        .bufferView = view(
            buffers_.overdrawResolved,
            capacityBytes(capacities_.overdrawCount, sizeof(uint32_t))),
        .binding = 6u});
    }
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.microPayloads,
          capacityBytes(pixelCapacity, sizeof(uint32_t))),
      .binding = 8u});
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.materialSampleAddresses,
          capacityBytes(
              pixelCapacity, sizeof(VoxelMaterialSampleAddressGPU))),
      .binding = 10u});
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.terrainOwnershipIds,
          capacityBytes(pixelCapacity, sizeof(uint32_t))),
      .binding = 12u});
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = std::move(groupBuffers),
           .textures = {
             {.textureView = readView(textures_.surface), .binding = 4u},
             {.textureView = readView(textures_.bottomAlbedo), .binding = 7u},
             {.textureView = readView(textures_.microPayload), .binding = 9u},
             {.textureView = readView(textures_.materialSampleAddress), .binding = 11u},
             {.textureView = readView(textures_.terrainOwnershipId), .binding = 13u},
           }}}});
    const char *shaderPath = settings_.overdrawDiagnostics
        ? "assets/shaders/spirv/svraster/"
          "sparse-voxel-brick-resolve-diagnostics.spirv"
        : "assets/shaders/spirv/svraster/sparse-voxel-brick-resolve.spirv";
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(shaderPath),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "resolveBrickSurfaces", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Buffers buffers_{};
  Textures textures_{};
  Settings settings_{};
  Capacities capacities_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

class SparseVoxelVirtualGeometryGBufferAdapterPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer microPayloads;
    ::rendering::Buffer materialSampleAddresses;
    ::rendering::Buffer terrainOwnershipIds;
    // Per-pixel software-raster resolve (payload, reverse-Z depth, oct
    // normal, dense instance) from the decoded-VG merge kernel. Pass a
    // one-element dummy to disable the software merge; the shader guards on
    // arrayLength.
    ::rendering::Buffer swResolvedPayloads;
  };

  struct Textures
  {
    ::rendering::Texture packedGeometryIdsLo;
    ::rendering::Texture packedGeometryIdsHi;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture baseColor;
    ::rendering::Texture materialProperties;
    ::rendering::Texture emissive;
    ::rendering::Texture normal;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU raster{};
    uint32_t width = 1u;
    uint32_t height = 1u;
    float depthLinearA = 0.0f;
    float depthLinearB = 1.0f;
  };

  SparseVoxelVirtualGeometryGBufferAdapterPass(
      const ::rendering::Texture depth,
      const Textures textures,
      const Buffers buffers,
      const Settings settings)
      : depth_(depth), textures_(textures), buffers_(buffers), settings_(sanitize(settings))
  {
  }

  ~SparseVoxelVirtualGeometryGBufferAdapterPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (settingsBuffer_.isValid())
      renderGraph->deleteBuffer(settingsBuffer_);
    if (rasterBuffer_.isValid())
      renderGraph->deleteBuffer(rasterBuffer_);
  }

  void update(const SparseVoxelRasterUniformsGPU &raster)
  {
    settings_.raster = raster;
    if (settings_.depthLinearB <= 1.0e-6f)
      settings_.depthLinearB =
          std::max(raster.cameraWorldPositionNear[3], 1.0e-6f);
    upload();
  }

  void updateSettings(const Settings settings)
  {
    settings_ = sanitize(settings);
    upload();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensure();
    upload();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.width + 7u) / 8u),
        std::max(1u, (settings_.height + 7u) / 8u),
        1u);
  }

private:
  struct alignas(16) AdapterSettingsGPU
  {
    std::array<uint32_t, 4> dimensions{};
    std::array<float, 4> depthConversion{};
  };
  static_assert(sizeof(AdapterSettingsGPU) == 32u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.depthLinearB = std::max(settings.depthLinearB, 1.0e-6f);
    return settings;
  }

  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  static ::rendering::BufferView bufferView(
      const ::rendering::Buffer buffer,
      const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, 4u),
      .access = access};
  }

  static ::rendering::TextureView depthView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  static ::rendering::TextureView readColorView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  static ::rendering::TextureView storageColorView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
  }

  void upload()
  {
    if (!settingsBuffer_.isValid() || !rasterBuffer_.isValid())
      return;
    const AdapterSettingsGPU gpu{
      {settings_.width, settings_.height, 0u, 0u},
      {settings_.depthLinearA, settings_.depthLinearB, 0.0f, 0.0f}};
    renderGraph->bufferWrite(settingsBuffer_, 0u, sizeof(gpu), const_cast<AdapterSettingsGPU *>(&gpu));
    renderGraph->bufferWrite(rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid())
      return;
    rasterBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_RasterUniforms",
         .size = sizeof(SparseVoxelRasterUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    settingsBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Settings",
         .size = sizeof(AdapterSettingsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "raster",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "settings",
              .binding = 1u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(2u, "resolvedSurfaces"),
             storage(3u, "coverage"),
             storage(4u, "linearDepth"),
             storage(5u, "microPayloads"),
             storage(6u, "materialSampleAddresses"),
             storage(7u, "terrainOwnershipIds"),
             storage(16u, "swResolvedPayloads")},
           .textures = {
             {.name = "sceneDepthTexture",
              .binding = 8u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "packedGeometryIdsLoTexture",
              .binding = 9u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "packedGeometryIdsHiTexture",
              .binding = 10u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "geometryNormalTexture",
              .binding = 11u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
           .storageTextures = {
             {.name = "baseColorTexture",
              .binding = 12u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialPropertiesTexture",
              .binding = 13u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "emissiveTexture",
              .binding = 14u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "normalTexture",
              .binding = 15u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}}}}});

    const uint32_t pixelCapacity = settings_.width * settings_.height;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = bufferView(rasterBuffer_, sizeof(SparseVoxelRasterUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ), .binding = 0u},
             {.bufferView = bufferView(settingsBuffer_, sizeof(AdapterSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ), .binding = 1u},
             {.bufferView = bufferView(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 2u},
             {.bufferView = bufferView(buffers_.coverage, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 3u},
             {.bufferView = bufferView(buffers_.linearDepth, capacityBytes(pixelCapacity, sizeof(float)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 4u},
             {.bufferView = bufferView(buffers_.microPayloads, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 5u},
             {.bufferView = bufferView(buffers_.materialSampleAddresses, capacityBytes(pixelCapacity, sizeof(VoxelMaterialSampleAddressGPU)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 6u},
             {.bufferView = bufferView(buffers_.terrainOwnershipIds, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_WRITE), .binding = 7u},
             {.bufferView = bufferView(buffers_.swResolvedPayloads, renderGraph->getBufferSize(buffers_.swResolvedPayloads), ::rendering::AccessPattern::SHADER_READ), .binding = 16u}},
           .textures = {
             {.textureView = depthView(depth_), .binding = 8u},
             {.textureView = readColorView(textures_.packedGeometryIdsLo), .binding = 9u},
             {.textureView = readColorView(textures_.packedGeometryIdsHi), .binding = 10u},
             {.textureView = readColorView(textures_.geometryNormal), .binding = 11u}},
           .storageTextures = {
             {.textureView = storageColorView(textures_.baseColor), .binding = 12u},
             {.textureView = storageColorView(textures_.materialProperties), .binding = 13u},
             {.textureView = storageColorView(textures_.emissive), .binding = 14u},
             {.textureView = storageColorView(textures_.normal), .binding = 15u}}}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-vg-gbuffer-adapter.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "adaptVirtualGeometryGBuffer",
         .layout = layout_,
         .name = passName + "_Pipeline",
         .shader = shader_});
  }

  ::rendering::Texture depth_{};
  Textures textures_{};
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer settingsBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
