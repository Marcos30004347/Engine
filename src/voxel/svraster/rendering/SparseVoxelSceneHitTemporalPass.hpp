#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{
class SparseVoxelSceneHitTemporalPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer currentHits;
    ::rendering::Buffer currentRays;
    ::rendering::Buffer previousHistory;
    ::rendering::Buffer history;
    ::rendering::Buffer stableHits;
    ::rendering::Buffer stableVisibility;
    ::rendering::Texture motionVector;
  };
  struct Settings { uint32_t rayCount=1u, maximumSamples=24u, contentGeneration=0u; bool historyValid=false; float distanceThreshold=0.25f; float receiverThreshold=0.05f; uint32_t width=1u; uint32_t height=1u; };
  SparseVoxelSceneHitTemporalPass(Buffers buffers, Settings settings) : buffers_(buffers), settings_(sanitize(settings)) {}
  void update(Settings settings) { settings_=sanitize(settings); if(uniforms_.isValid()) upload(); }
  ~SparseVoxelSceneHitTemporalPass() override { if(pipeline_.isValid())renderGraph->deleteComputePipeline(pipeline_);if(shader_.isValid())renderGraph->deleteShader(shader_);if(groups_.isValid())renderGraph->deleteBindingGroups(groups_);if(layout_.isValid())renderGraph->deleteBindingsLayout(layout_);if(uniforms_.isValid())renderGraph->deleteBuffer(uniforms_); }
  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override { ensure();upload();cmd.cmdBindComputePipeline(pipeline_);cmd.cmdBindBindingGroups(groups_,nullptr,0u);cmd.cmdDispatch((settings_.rayCount+63u)/64u,1u,1u); }
private:
  struct alignas(16) Uniforms { uint32_t rayCount, maximumSamples, historyValid, contentGeneration; float distanceThreshold, receiverThreshold, reserved1, reserved2; uint32_t width, height, reserved3, reserved4; };
  static_assert(sizeof(Uniforms)==48u);
  static Settings sanitize(Settings s){s.rayCount=std::max(1u,s.rayCount);s.maximumSamples=std::max(1u,s.maximumSamples);s.distanceThreshold=std::max(0.0f,s.distanceThreshold);s.receiverThreshold=std::max(0.0f,s.receiverThreshold);s.width=std::max(1u,s.width);s.height=std::max(1u,s.height);return s;}
  static ::rendering::BufferView view(::rendering::Buffer b,uint64_t size,::rendering::AccessPattern access){return {.buffer=b,.offset=0u,.size=std::max<uint64_t>(size,4u),.access=access};}
  static ::rendering::TextureView sampledView(::rendering::Texture texture){return {.texture=texture,.access=::rendering::AccessPattern::SHADER_READ,.layout=::rendering::ResourceLayout::SHADER_READ_ONLY,.index=0u,.flags=::rendering::ImageAspectFlags::Color,.baseArrayLayer=0u,.baseMipLevel=0u,.layerCount=1u,.levelCount=1u};}
  void upload(){const Uniforms u{settings_.rayCount,settings_.maximumSamples,settings_.historyValid?1u:0u,settings_.contentGeneration,settings_.distanceThreshold,settings_.receiverThreshold,0.0f,0.0f,settings_.width,settings_.height,0u,0u};renderGraph->bufferWrite(uniforms_,0u,sizeof(u),const_cast<Uniforms*>(&u));}
  void ensure()
  {
    if (layout_.isValid())
      return;
    const uint64_t count = settings_.rayCount;
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
          {.name = "settings", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentHits", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentRays", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "previousHistory", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "history", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "stableHits", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "stableVisibility", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .textures = {
          {.name = "motionVector", .binding = 7u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = view(uniforms_, sizeof(Uniforms), ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u, .bufferView = view(buffers_.currentHits, count * sizeof(SparseVoxelSceneRayHitGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 2u, .bufferView = view(buffers_.currentRays, count * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u, .bufferView = view(buffers_.previousHistory, count * sizeof(SparseVoxelTemporalSceneRayHitGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u, .bufferView = view(buffers_.history, count * sizeof(SparseVoxelTemporalSceneRayHitGPU), ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 5u, .bufferView = view(buffers_.stableHits, count * sizeof(SparseVoxelSceneRayHitGPU), ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 6u, .bufferView = view(buffers_.stableVisibility, count * sizeof(float), ::rendering::AccessPattern::SHADER_WRITE)},
        },
        .textures = {
          {.binding = 7u, .textureView = sampledView(buffers_.motionVector)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-scene-hit-temporal.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "accumulateSparseVoxelSceneHits",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }
  Buffers buffers_{};Settings settings_{};::rendering::Buffer uniforms_{};::rendering::BindingsLayout layout_{};::rendering::BindingGroups groups_{};::rendering::Shader shader_{};::rendering::ComputePipeline pipeline_{};
};
} // namespace voxel::svraster::rendering
