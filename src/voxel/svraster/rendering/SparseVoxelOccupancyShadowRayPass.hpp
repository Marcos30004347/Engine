#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyShadowRayPass final : public ::rendering::Pass
{
public:
  enum class Mode : uint32_t
  {
    Disabled = 0u,
    SurfaceToLight = 1u,
    CameraPrimary = 2u,
  };

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer rays;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU raster{};
    std::array<float, 3> lightDirection{-0.408248f, 0.816497f, 0.408248f};
    float maximumDistance = 64.0f;
    float originBias = 0.5f;
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t sourceWidth = 1u;
    Mode mode = Mode::Disabled;
  };

  SparseVoxelOccupancyShadowRayPass(
      const Buffers buffers,
      Settings settings)
      : buffers_(buffers), settings_(settings)
  {
    settings_.width = std::max(1u, settings_.width);
    settings_.height = std::max(1u, settings_.height);
    settings_.sourceWidth = std::max(1u, settings_.sourceWidth);
  }

  void update(
      const SparseVoxelRasterUniformsGPU &raster,
      const Mode mode)
  {
    settings_.raster = raster;
    settings_.mode = mode;
    upload();
  }

  ~SparseVoxelOccupancyShadowRayPass() override
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

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    upload();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.width + 7u) / 8u),
        std::max(1u, (settings_.height + 7u) / 8u),
        1u);
  }

private:
  struct alignas(16) ShadowSettingsGPU
  {
    std::array<float, 4> lightDirectionMaximumDistance{};
    std::array<uint32_t, 4> dimensionsEnabled{};
    std::array<float, 4> biasReserved{};
  };

  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer,
      const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void upload()
  {
    if (!rasterBuffer_.isValid())
      return;
    const ShadowSettingsGPU gpu{
      {settings_.lightDirection[0], settings_.lightDirection[1],
       settings_.lightDirection[2], settings_.maximumDistance},
      {settings_.width, settings_.height, settings_.sourceWidth,
       static_cast<uint32_t>(settings_.mode)},
      {settings_.originBias, 0.0f, 0.0f, 0.0f},
    };
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster),
        &settings_.raster);
    renderGraph->bufferWrite(
        settingsBuffer_, 0u, sizeof(gpu),
        const_cast<ShadowSettingsGPU *>(&gpu));
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    rasterBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Raster",
      .size = sizeof(settings_.raster),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    settingsBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Settings",
      .size = sizeof(ShadowSettingsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "raster", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "shadowSettings", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "coverage", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "linearDepth", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "rays", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    const uint64_t sourcePixelCount =
        uint64_t{settings_.sourceWidth} *
        settings_.raster.viewportTiles[1];
    const uint64_t rayCount =
        uint64_t{settings_.width} * settings_.height;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = view(rasterBuffer_, sizeof(settings_.raster), ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u, .bufferView = view(settingsBuffer_, sizeof(ShadowSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 2u, .bufferView = view(buffers_.coverage, sourcePixelCount * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u, .bufferView = view(buffers_.linearDepth, sourcePixelCount * sizeof(float), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u, .bufferView = view(buffers_.rays, rayCount * sizeof(SparseVoxelOccupancyDDARayGPU), ::rendering::AccessPattern::SHADER_WRITE)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-shadow-rays.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "generateOccupancyShadowRays",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

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
