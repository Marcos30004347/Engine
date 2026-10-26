#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Test-only readback support for the actual GPU overview target. Non-zero
// words encode coverage plus quantized RGB so tests can distinguish shaded,
// wireframe, and depth diagnostics. The regular
// sparse-voxel coverage buffer is produced before planet navigation is
// composited and therefore cannot prove that the globe is on screen.
class SparseVoxelPlanetOverviewCoveragePass final : public ::rendering::Pass
{
public:
  SparseVoxelPlanetOverviewCoveragePass(const ::rendering::Texture color, const ::rendering::Buffer coverage, const uint32_t width, const uint32_t height)
      : color_(color), coverage_(coverage), width_(width), height_(height)
  {
  }

  ~SparseVoxelPlanetOverviewCoveragePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    const std::array<uint32_t, 4> dimensions{width_, height_, 0u, 0u};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(dimensions), const_cast<std::array<uint32_t, 4> *>(&dimensions));
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    commands.cmdDispatch((width_ + 7u) / 8u, (height_ + 7u) / 8u, 1u);
  }

private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms", .size = sizeof(std::array<uint32_t, 4>), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = false});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers =
               {{.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                {.name = "coverage", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
           .textures = {{.name = "overviewColor", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}}}}});
    const ::rendering::TextureView colorView{
      .texture = color_,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u};
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers =
               {{.binding = 0u, .bufferView = {.buffer = uniforms_, .offset = 0u, .size = sizeof(std::array<uint32_t, 4>), .access = ::rendering::AccessPattern::UNIFORM_READ}},
                {.binding = 2u,
                 .bufferView = {.buffer = coverage_, .offset = 0u, .size = uint64_t{width_} * height_ * sizeof(uint32_t), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE}}},
           .textures = {{.binding = 1u, .textureView = colorView}}}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-planet-overview-coverage.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = "measurePlanetOverviewCoverage", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  ::rendering::Texture color_{};
  ::rendering::Buffer coverage_{};
  uint32_t width_ = 1u;
  uint32_t height_ = 1u;
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
