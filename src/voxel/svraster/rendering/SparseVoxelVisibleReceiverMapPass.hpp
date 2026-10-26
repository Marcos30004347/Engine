#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Builds a contiguous hash map from exact visible voxel-face identities to
// their projected screen bounds, then elects center and four edge receivers.
class SparseVoxelVisibleReceiverMapPass final : public ::rendering::Pass
{
public:
  enum class Stage
  {
    Bounds,
    Samples,
  };

  static constexpr uint32_t kEntryWords = 14u;
  static constexpr uint32_t kEntryBytes = kEntryWords * sizeof(uint32_t);

  struct Buffers
  {
    ::rendering::Buffer coverage;
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Texture geometryNormal;
    ::rendering::Buffer entries;
  };

  SparseVoxelVisibleReceiverMapPass(
      const Stage stage, const Buffers buffers,
      const uint32_t width, const uint32_t height,
      const uint32_t capacity)
      : stage_(stage), buffers_(buffers), width_(std::max(width, 1u)),
        height_(std::max(height, 1u)), capacity_(std::max(capacity, 1u))
  {
  }

  void updateGeneration(const uint32_t generation)
  {
    generation_ = generation == 0u || generation == 0xfffffffeu
        ? 1u
        : generation;
  }

  ~SparseVoxelVisibleReceiverMapPass() override
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
    const std::array<uint32_t, 4> settings{
        width_, height_, capacity_, generation_};
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(settings),
        const_cast<std::array<uint32_t, 4> *>(&settings));
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    const uint64_t invocationCount = uint64_t{width_} * height_;
    cmd.cmdDispatch(
        static_cast<uint32_t>((invocationCount + 63u) / 64u), 1u, 1u);
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer, const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u,
            .size = std::max<uint64_t>(size, sizeof(uint32_t)),
            .access = access};
  }

  static ::rendering::TextureView sampledView(
      const ::rendering::Texture texture)
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

  void ensure()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(uint32_t) * 4u,
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
          {.name = "coverage", .binding = 1u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "entries", .binding = 5u, .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .textures = {
          {.name = "instanceId", .binding = 2u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "voxelId", .binding = 3u,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "geometryNormal", .binding = 4u,
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
          {.binding = 0u,
           .bufferView = view(uniforms_, sizeof(uint32_t) * 4u,
                              ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u,
           .bufferView = view(
               buffers_.coverage, uint64_t{width_} * height_ * sizeof(uint32_t),
               ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u,
           .bufferView = view(
               buffers_.entries, uint64_t{capacity_} * kEntryBytes,
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)},
        },
        .textures = {
          {.binding = 2u, .textureView = sampledView(buffers_.instanceId)},
          {.binding = 3u, .textureView = sampledView(buffers_.voxelId)},
          {.binding = 4u, .textureView = sampledView(buffers_.geometryNormal)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-visible-receiver-map.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = stage_ == Stage::Bounds
            ? "buildSparseVoxelVisibleReceiverBounds"
            : "selectSparseVoxelVisibleReceiverSamples";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Stage stage_;
  Buffers buffers_;
  uint32_t width_;
  uint32_t height_;
  uint32_t capacity_;
  uint32_t generation_ = 1u;
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
