#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace voxel::svraster::rendering
{

// Draws the finite emitters into the composed image so a light being authored
// is visible where it actually is.
//
// The emitters are intersected analytically against the camera ray rather than
// rasterised: at a handful of spheres and quads that costs a few dozen
// instructions per pixel and needs no vertex buffers or draw calls, which this
// renderer has no other use for. Occlusion reuses the linear depth buffer the
// lighting passes already read, so no depth attachment is involved either.
//
// The pass writes the composed colour in place. It runs after compose and
// before the temporal upscaler so emitters are anti-aliased and motion-matched
// with everything else.
class SparseVoxelLightGizmoPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t lightCapacity = 1u;
    // Stable handle of the light the editor has selected, or 0xffffffff. The
    // selected emitter is outlined so the list and the viewport agree.
    uint32_t selectedHandle = 0xffffffffu;
    bool enabled = true;
    // Outline width, as a fraction of the emitter's half extent.
    float outlineWidth = 0.06f;
    SparseVoxelRasterUniformsGPU raster{};
  };

  struct Buffers
  {
    ::rendering::Buffer lights;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Texture output;
  };

  SparseVoxelLightGizmoPass(Buffers buffers, Settings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const Settings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelLightGizmoPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch(
        (settings_.width + 7u) / 8u, (settings_.height + 7u) / 8u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> dimensionsLights{};
    std::array<float, 4> options{};
  };
  static_assert(sizeof(Uniforms) == 32u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height, settings_.lightCapacity,
         settings_.selectedHandle},
        {settings_.enabled ? 1.0f : 0.0f,
         std::clamp(settings_.outlineWidth, 0.0f, 0.5f), 0.0f, 0.0f}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    const uint64_t pixels = uint64_t{settings_.width} * settings_.height;
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size,
                         const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = std::max<uint64_t>(size, sizeof(uint32_t)),
        .access = access};
    };
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    const auto storageBuffer = [](const char *name, const uint32_t binding)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "raster",
           .binding = 0u,
           .isDynamic = false,
           .type =
               ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "settings",
           .binding = 1u,
           .isDynamic = false,
           .type =
               ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          storageBuffer("lights", 2u), storageBuffer("coverage", 3u),
          storageBuffer("linearDepth", 4u)},
        .storageTextures = {
          {.name = "output",
           .binding = 5u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute}}}}});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u,
           .bufferView = view(
               rasterBuffer_, sizeof(settings_.raster),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               uniforms_, sizeof(Uniforms),
               ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u,
           .bufferView = view(
               buffers_.lights,
               globalillumination::worldgi::worldGILightBufferSize(
                   settings_.lightCapacity),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u,
           .bufferView = view(
               buffers_.coverage, pixels * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u,
           .bufferView = view(
               buffers_.linearDepth, pixels * sizeof(float),
               ::rendering::AccessPattern::SHADER_READ)}},
        .storageTextures = {
          {.binding = 5u,
           .textureView = {
             .texture = buffers_.output,
             // Read-write in place: the pass composites the emitter over
             // whatever compose left in the pixel, so it needs both.
             .access = ::rendering::AccessPattern::SHADER_READ |
                 ::rendering::AccessPattern::SHADER_WRITE,
             .layout = ::rendering::ResourceLayout::GENERAL,
             .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Color,
             .baseArrayLayer = 0u,
             .baseMipLevel = 0u,
             .layerCount = 1u,
             .levelCount = 1u}}}}}});
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-light-gizmo.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "drawSparseVoxelLightGizmos",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
