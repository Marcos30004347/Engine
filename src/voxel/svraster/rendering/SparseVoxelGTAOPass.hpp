#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace voxel::svraster::rendering
{

// Ground-truth ambient occlusion over the light buffer.
//
// The pass runs between the denoiser and material composition. That position is
// deliberate: at that point the buffer is still a light multiplier, so occlusion
// attenuates lighting alone. After composition it would darken albedo and
// emission with it.
//
// It reads one texture and writes another rather than occluding in place. The
// light buffer feeding it is ringed into next frame's history, so an in-place
// multiply applies AO to an already occluded value every frame - a geometric
// decay that reads as the image darkening and growing noisier the longer the
// camera holds still.
//
// The probe path cannot produce this itself at any reasonable cost - one probe
// per screen tile has no way to express contact darkening a few centimetres
// wide - so this is complementary rather than redundant.
class SparseVoxelGTAOPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    // Directions searched per pixel, and horizon steps along each.
    uint32_t sliceCount = 3u;
    uint32_t stepCount = 6u;
    // World-space search radius.
    float radius = 1.0f;
    // Response exponent. One is the raw visibility integral; above one darkens
    // the mid range without moving either end.
    float intensity = 1.0f;
    // Fraction of the radius beyond which an occluder is progressively released
    // because the depth buffer cannot know how thick it is.
    float thickness = 0.25f;
    // Projection coefficients for viewDepth = B / (deviceDepth + A).
    float depthLinearA = 0.0f;
    float depthLinearB = 1.0f;
    uint32_t frameIndex = 0u;
    SparseVoxelRasterUniformsGPU raster{};
  };

  struct Buffers
  {
    ::rendering::Texture geometryNormal;
    ::rendering::Texture depth;
    // Read, never written. This is part of a history ring, so occluding it in
    // place would re-apply AO to an already occluded value every frame.
    ::rendering::Texture lightingSource;
    // The occluded copy the rest of the frame consumes.
    ::rendering::Texture lightingTarget;
  };

  SparseVoxelGTAOPass(Buffers buffers, Settings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void update(const Settings &settings)
  {
    settings_ = settings;
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelGTAOPass() override
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
    std::array<uint32_t, 4> dimensionsSlices{};
    std::array<float, 4> parameters{};
    std::array<float, 4> depthConversion{};
  };
  static_assert(sizeof(Uniforms) == 48u);

  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms values{
        {settings_.width, settings_.height,
         std::clamp(settings_.sliceCount, 1u, 16u),
         std::clamp(settings_.stepCount, 1u, 32u)},
        {std::max(settings_.radius, 1.0e-3f),
         std::clamp(settings_.intensity, 0.01f, 8.0f),
         std::clamp(settings_.thickness, 0.0f, 0.95f),
         static_cast<float>(settings_.frameIndex)},
        {settings_.depthLinearA, settings_.depthLinearB, 0.0f, 0.0f}};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(values), const_cast<Uniforms *>(&values));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size,
                         const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = std::max<uint64_t>(size, sizeof(uint32_t)),
        .access = access};
    };
    const auto uniformEntry = [](const char *name, const uint32_t binding)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
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
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          uniformEntry("raster", 0u), uniformEntry("settings", 1u)},
        .textures = {
          {.name = "geometryNormalTexture",
           .binding = 4u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "lightingSource",
           .binding = 5u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "depthTexture",
           .binding = 7u,
           .visibility =
               ::rendering::BindingVisibility::BindingVisibility_Compute}},
        .storageTextures = {
          {.name = "lightingTarget",
           .binding = 6u,
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
               ::rendering::AccessPattern::UNIFORM_READ)}},
        .textures = {
          {.binding = 4u,
           .textureView = {
             .texture = buffers_.geometryNormal,
             .access = ::rendering::AccessPattern::SHADER_READ,
             .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
             .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Color,
             .baseArrayLayer = 0u,
             .baseMipLevel = 0u,
             .layerCount = 1u,
             .levelCount = 1u}},
          {.binding = 5u,
           .textureView = {
             .texture = buffers_.lightingSource,
             .access = ::rendering::AccessPattern::SHADER_READ,
             .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
             .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Color,
             .baseArrayLayer = 0u,
             .baseMipLevel = 0u,
             .layerCount = 1u,
             .levelCount = 1u}},
          {.binding = 7u,
           .textureView = {
             .texture = buffers_.depth,
             .access = ::rendering::AccessPattern::SHADER_READ,
             .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
             .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Depth,
             .baseArrayLayer = 0u,
             .baseMipLevel = 0u,
             .layerCount = 1u,
             .levelCount = 1u}}},
        .storageTextures = {
          {.binding = 6u,
           .textureView = {
             .texture = buffers_.lightingTarget,
             .access = ::rendering::AccessPattern::SHADER_WRITE,
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
          "assets/shaders/spirv/svraster/sparse-voxel-gtao.spirv"),
      .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "computeSparseVoxelGTAO",
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
