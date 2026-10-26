#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Performs race-free four-neighbor spatial reuse after validating receiver
// identity, normal, depth, world position, and light generation.
class SparseVoxelDISpatialReservoirPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer candidates;
    ::rendering::Buffer inputRays;
    ::rendering::Buffer inputReservoirs;
    ::rendering::Buffer outputRays;
    ::rendering::Buffer outputReservoirs;
    ::rendering::Buffer lights;
    ::rendering::Buffer linearDepth;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture instanceId;
    ::rendering::Texture voxelId;
    ::rendering::Buffer lightReachability;
    ::rendering::Texture reservoirReceiver;
    ::rendering::Texture reservoirCore;
    ::rendering::Buffer activeRayIndices;
    ::rendering::Buffer activeRayState;
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t candidatesPerPixel = 1u;
    uint32_t contentGeneration = 0u;
    uint32_t lightCapacity = 1u;
    float receiverRadius = 0.5f;
    float normalThreshold = 0.85f;
    float depthThreshold = 0.05f;
    uint32_t sourceWidth = 1u;
    uint32_t sourceHeight = 1u;
    uint32_t frameIndex = 0u;
    // Upper bound on the RIS weight W; see SparseVoxelDIReservoirPass.
    float maximumWeight = 64.0f;
    uint32_t reachabilityEntryCount =
        kSparseVoxelLightReachabilityEntryCount;
    float reachabilityExplorationFloor = 1.0f / 64.0f;
    uint32_t raysPerPixel = 1u;
  };

  SparseVoxelDISpatialReservoirPass(Buffers buffers, Settings settings) : buffers_(buffers), settings_(sanitize(settings))
  {
  }

  void update(Settings settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid())
      upload();
  }

  ~SparseVoxelDISpatialReservoirPass() override
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
    if (ownsLightReachability_ && buffers_.lightReachability.isValid())
      renderGraph->deleteBuffer(buffers_.lightReachability);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatchIndirect(buffers_.activeRayState, 0u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t width;
    uint32_t height;
    uint32_t candidatesPerPixel;
    uint32_t contentGeneration;
    float receiverRadius;
    float normalThreshold;
    float depthThreshold;
    float maximumWeight;
    uint32_t sourceWidth;
    uint32_t sourceHeight;
    uint32_t frameSeed;
    uint32_t reserved2;
    uint32_t lightCapacity;
    uint32_t reachabilityEntryCount;
    float reachabilityExplorationFloor;
    uint32_t reserved3;
  };
  static_assert(sizeof(Uniforms) == 64u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.raysPerPixel = std::clamp(settings.raysPerPixel, 1u, 8u);
    settings.candidatesPerPixel =
        std::clamp(settings.candidatesPerPixel, settings.raysPerPixel, 16u);
    settings.lightCapacity = std::max(1u, settings.lightCapacity);
    settings.sourceWidth = std::max(1u, settings.sourceWidth);
    settings.sourceHeight = std::max(1u, settings.sourceHeight);
    settings.receiverRadius = std::max(0.0f, settings.receiverRadius);
    settings.normalThreshold = std::clamp(settings.normalThreshold, -1.0f, 1.0f);
    settings.depthThreshold = std::max(0.0f, settings.depthThreshold);
    settings.maximumWeight = std::max(1.0e-4f, settings.maximumWeight);
    settings.reachabilityEntryCount =
        std::max(1u, settings.reachabilityEntryCount);
    settings.reachabilityExplorationFloor =
        std::clamp(
            settings.reachabilityExplorationFloor, 1.0f / 1024.0f, 1.0f);
    return settings;
  }

  static ::rendering::BufferView view(::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  static ::rendering::TextureView sampledView(::rendering::Texture texture)
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

  void upload()
  {
    const Uniforms uniforms{
      settings_.width,
      settings_.height,
      settings_.candidatesPerPixel,
      settings_.contentGeneration,
      settings_.receiverRadius,
      settings_.normalThreshold,
      settings_.depthThreshold,
      settings_.maximumWeight,
      settings_.sourceWidth,
      settings_.sourceHeight,
      settings_.frameIndex,
      settings_.raysPerPixel,
      settings_.lightCapacity,
      settings_.reachabilityEntryCount,
      settings_.reachabilityExplorationFloor,
      0u,
    };
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(uniforms), const_cast<Uniforms *>(&uniforms));
  }

  void ensure()
  {
    if (layout_.isValid())
      return;
    if (!buffers_.lightReachability.isValid())
    {
      buffers_.lightReachability = renderGraph->createBuffer({
        .name = passName + "_EmptyLightReachability",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const uint32_t zero = 0u;
      renderGraph->bufferWrite(
          buffers_.lightReachability, 0u, sizeof(zero),
          const_cast<uint32_t *>(&zero));
      ownsLightReachability_ = true;
    }
    const uint64_t pixels = uint64_t{settings_.width} * settings_.height;
    const uint64_t rays = pixels * settings_.raysPerPixel;
    const uint64_t sourcePixels =
        uint64_t{settings_.sourceWidth} * settings_.sourceHeight;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(Uniforms),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph
                  ->createBindingsLayout(
                      {
                        .name = passName + "_Layout",
                        .groups =
                            {::rendering::BindingGroupLayout{
                              .buffers =
                                  {
                                    {.name = "settings",
                                     .binding = 0u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "candidates",
                                     .binding = 1u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "inputRays",
                                     .binding = 2u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "inputReservoirs",
                                     .binding = 3u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "outputRays",
                                     .binding = 4u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "outputReservoirs",
                                     .binding = 5u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "lights",
                                     .binding = 6u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "linearDepth",
                                     .binding = 7u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "lightReachability",
                                     .binding = 11u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "activeRayIndices",
                                     .binding = 14u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "activeRayState",
                                     .binding = 15u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                              .textures =
                                  {
                                    {.name = "geometryNormal", .binding = 8u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "instanceId", .binding = 9u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "voxelId", .binding = 10u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "reservoirReceiver", .binding = 12u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "reservoirCore", .binding = 13u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                            }},
                      });
    groups_ = renderGraph
                  ->createBindingGroups(
                      {
                        .layout = layout_,
                        .name = passName + "_Groups",
                        .groups =
                            {::rendering::GroupInfo{
                              .name = "group0",
                              .buffers =
                                  {
                                    {.binding = 0u, .bufferView = view(uniforms_, sizeof(Uniforms), ::rendering::AccessPattern::UNIFORM_READ)},
                                    {.binding = 1u, .bufferView = view(buffers_.candidates, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 2u, .bufferView = view(buffers_.inputRays, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 3u, .bufferView = view(buffers_.inputReservoirs, pixels * sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 4u, .bufferView = view(buffers_.outputRays, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_WRITE)},
                                    {.binding = 5u, .bufferView = view(buffers_.outputReservoirs, pixels * sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_WRITE)},
                                    {.binding = 6u, .bufferView = view(buffers_.lights, globalillumination::worldgi::worldGILightBufferSize(settings_.lightCapacity), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 7u, .bufferView = view(buffers_.linearDepth, sourcePixels * sizeof(float), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 11u,
                                     .bufferView = view(
                                         buffers_.lightReachability,
                                         ownsLightReachability_
                                             ? sizeof(uint32_t)
                                             : sparseVoxelLightReachabilityBufferSize(
                                                   settings_.lightCapacity,
                                                   settings_.reachabilityEntryCount),
                                         ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 14u,
                                     .bufferView = view(
                                         buffers_.activeRayIndices,
                                         rays * sizeof(uint32_t),
                                         ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 15u,
                                     .bufferView = view(
                                         buffers_.activeRayState,
                                         sizeof(IndirectDispatchArgsGPU),
                                         ::rendering::AccessPattern::SHADER_READ)},
                                  },
                              .textures =
                                  {
                                    {.binding = 8u, .textureView = sampledView(buffers_.geometryNormal)},
                                    {.binding = 9u, .textureView = sampledView(buffers_.instanceId)},
                                    {.binding = 10u, .textureView = sampledView(buffers_.voxelId)},
                                    {.binding = 12u, .textureView = sampledView(buffers_.reservoirReceiver)},
                                    {.binding = 13u, .textureView = sampledView(buffers_.reservoirCore)},
                                  },
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-di-spatial-reservoir.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "resampleSparseVoxelDISpatially",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool ownsLightReachability_ = false;
};

} // namespace voxel::svraster::rendering
