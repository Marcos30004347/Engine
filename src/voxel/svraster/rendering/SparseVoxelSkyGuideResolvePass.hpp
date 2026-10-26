#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelSkyGuide.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Converts the sky observations scattered by SparseVoxelLightFieldPass into a
// compact per-tile CDF consumed by next frame's ray creation. There is one
// invocation per 16x16 screen tile, so this stage stays tiny compared with the
// full-resolution ray creation and trace passes.
class SparseVoxelSkyGuideResolvePass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float learningRate = 0.2f;
    float uniformPrior = 0.02f;
    float fullLearningObservations = 64.0f;
  };

  SparseVoxelSkyGuideResolvePass(
      const ::rendering::Buffer feedback,
      const ::rendering::Buffer guideCDF,
      Settings settings)
      : feedback_(feedback), guideCDF_(guideCDF), settings_(sanitize(settings))
  {
  }

  void update(Settings settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid()) upload();
  }

  ~SparseVoxelSkyGuideResolvePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    const uint32_t tileCount =
        sparseVoxelSkyGuideTileCount(settings_.width, settings_.height);
    cmd.cmdDispatch((tileCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t dimensions[4] = {1u, 1u, 1u, 1u};
    float controls[4] = {0.2f, 0.02f, 64.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 32u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(settings.width, 1u);
    settings.height = std::max(settings.height, 1u);
    settings.learningRate = std::clamp(settings.learningRate, 0.0f, 1.0f);
    settings.uniformPrior = std::clamp(settings.uniformPrior, 1.0e-5f, 1.0f);
    settings.fullLearningObservations =
        std::max(settings.fullLearningObservations, 1.0f);
    return settings;
  }

  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer, const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u,
            .size = std::max<uint64_t>(size, sizeof(uint32_t)),
            .access = access};
  }

  void upload()
  {
    Uniforms uniforms{};
    uniforms.dimensions[0] = settings_.width;
    uniforms.dimensions[1] = settings_.height;
    uniforms.dimensions[2] =
        sparseVoxelSkyGuideTileCount(settings_.width, settings_.height);
    uniforms.dimensions[3] = 1u;
    uniforms.controls[0] = settings_.learningRate;
    uniforms.controls[1] = settings_.uniformPrior;
    uniforms.controls[2] = settings_.fullLearningObservations;
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(uniforms), &uniforms);
  }

  void ensure()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = {
          {.name = "settings", .binding = 0u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "feedback", .binding = 1u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "guideCDF", .binding = 2u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = view(
              uniforms_, sizeof(Uniforms),
              ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u, .bufferView = view(
              feedback_, sparseVoxelSkyGuideFeedbackBufferSize(
                  settings_.width, settings_.height),
              ::rendering::AccessPattern::SHADER_READ |
                  ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 2u, .bufferView = view(
              guideCDF_, sparseVoxelSkyGuideCDFBufferSize(
                  settings_.width, settings_.height),
              ::rendering::AccessPattern::SHADER_READ |
                  ::rendering::AccessPattern::SHADER_WRITE)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-sky-guide.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "resolveSparseVoxelSkyGuide",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  ::rendering::Buffer feedback_{};
  ::rendering::Buffer guideCDF_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
