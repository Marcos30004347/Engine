#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace voxel::svraster::rendering
{

// Performs candidate RIS plus motion-reprojected temporal reuse. Reservoirs
// retain receiver attributes so history can be rejected on geometry changes.
class SparseVoxelDIReservoirPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Dense,
    Reset,
    Compact,
  };

  struct Buffers
  {
    ::rendering::Buffer candidates;
    ::rendering::Buffer previousSelectedRays;
    ::rendering::Buffer previousReservoirs;
    ::rendering::Buffer selectedRays;
    ::rendering::Buffer reservoirs;
    ::rendering::Buffer lights;
    ::rendering::Buffer linearDepth;
    ::rendering::Texture geometryNormal;
    ::rendering::Texture motionVector;
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
    uint32_t pixelCount = 1u;
    uint32_t candidatesPerPixel = 1u;
    uint32_t contentGeneration = 0u;
    bool historyValid = false;
    uint32_t lightCapacity = 1u;
    uint32_t width = 1u;
    uint32_t height = 1u;
    float normalThreshold = 0.85f;
    float depthThreshold = 0.05f;
    uint32_t sourceWidth = 1u;
    uint32_t sourceHeight = 1u;
    uint32_t maximumHistorySamples = 32u;
    uint32_t frameIndex = 0u;
    // Upper bound on the RIS weight W. Applied in ray space, before the
    // shading pass upsamples one reservoir across a divisor-sized pixel block
    // and turns a single outlier into a block the denoiser cannot reject.
    float maximumWeight = 64.0f;
    uint32_t reachabilityEntryCount =
        kSparseVoxelLightReachabilityEntryCount;
    float reachabilityExplorationFloor = 1.0f / 64.0f;
    // Number of independently selected reservoirs (and therefore traced
    // visibility rays) produced for each pixel. Candidate generation is
    // controlled separately by candidatesPerPixel.
    uint32_t raysPerPixel = 1u;
  };

  SparseVoxelDIReservoirPass(
      Buffers buffers, Settings settings,
      const Kernel kernel = Kernel::Dense)
      : buffers_(buffers), settings_(sanitize(settings)), kernel_(kernel)
  {
  }

  void update(Settings settings)
  {
    settings_ = sanitize(settings);
    if (uniforms_.isValid())
      upload();
  }

  ~SparseVoxelDIReservoirPass() override
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
    for (const ::rendering::Buffer buffer : ownedFallbackBuffers_)
      if (buffer.isValid())
        renderGraph->deleteBuffer(buffer);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure();
    upload();
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::Compact)
      cmd.cmdDispatchIndirect(buffers_.activeRayState, 0u);
    else
      cmd.cmdDispatch((settings_.pixelCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t pixelCount;
    uint32_t candidatesPerPixel;
    uint32_t contentGeneration;
    uint32_t historyValid;
    uint32_t width;
    uint32_t height;
    uint32_t reserved0;
    uint32_t reserved1;
    float normalThreshold;
    float depthThreshold;
    uint32_t maximumHistorySamples;
    uint32_t frameSeed;
    float maximumWeight;
    uint32_t reachabilityEntryCount;
    float reachabilityExplorationFloor;
    uint32_t lightCapacity;
    uint32_t raysPerPixel;
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t reserved4;
  };
  static_assert(sizeof(Uniforms) == 80u);

  static Settings sanitize(Settings settings)
  {
    settings.width = std::max(1u, settings.width);
    settings.height = std::max(1u, settings.height);
    settings.pixelCount = static_cast<uint32_t>(std::min<uint64_t>(
        uint64_t{settings.width} * settings.height,
        std::numeric_limits<uint32_t>::max()));
    settings.raysPerPixel = std::clamp(settings.raysPerPixel, 1u, 8u);
    settings.candidatesPerPixel =
        std::clamp(settings.candidatesPerPixel, settings.raysPerPixel, 16u);
    settings.lightCapacity = std::max(1u, settings.lightCapacity);
    settings.sourceWidth = std::max(1u, settings.sourceWidth);
    settings.sourceHeight = std::max(1u, settings.sourceHeight);
    settings.maximumHistorySamples =
        std::clamp(settings.maximumHistorySamples, 1u, 4096u);
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

  static ::rendering::TextureView storageView(::rendering::Texture texture)
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
      .levelCount = 1u,
    };
  }

  void upload()
  {
    const Uniforms uniforms{
      settings_.pixelCount,
      settings_.candidatesPerPixel,
      settings_.contentGeneration,
      settings_.historyValid ? 1u : 0u,
      settings_.width,
      settings_.height,
      settings_.sourceWidth,
      settings_.sourceHeight,
      settings_.normalThreshold,
      settings_.depthThreshold,
      settings_.maximumHistorySamples,
      settings_.frameIndex,
      settings_.maximumWeight,
      settings_.reachabilityEntryCount,
      settings_.reachabilityExplorationFloor,
      settings_.lightCapacity,
      settings_.raysPerPixel,
      0u,
      0u,
      0u,
    };
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(uniforms), const_cast<Uniforms *>(&uniforms));
  }

  void ensure()
  {
    if (layout_.isValid())
      return;
    const auto fallbackBuffer = [&](const std::string &suffix,
                                    const uint64_t size)
    {
      const ::rendering::Buffer buffer = renderGraph->createBuffer({
        .name = passName + suffix,
        .size = std::max<uint64_t>(size, sizeof(uint32_t)),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage,
        .frameLocal = true,
      });
      ownedFallbackBuffers_.push_back(buffer);
      return buffer;
    };
    const uint64_t fallbackRayCount =
        uint64_t{settings_.pixelCount} * settings_.raysPerPixel;
    if (!buffers_.activeRayIndices.isValid())
      buffers_.activeRayIndices = fallbackBuffer(
          "_FallbackActiveRayIndices",
          fallbackRayCount * sizeof(uint32_t));
    if (!buffers_.activeRayState.isValid())
      buffers_.activeRayState =
          fallbackBuffer("_FallbackActiveRayState", 16u);
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
    const uint64_t pixels = settings_.pixelCount;
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
                                    {.name = "previousSelectedRays",
                                     .binding = 2u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "previousReservoirs",
                                     .binding = 3u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "selectedRays",
                                     .binding = 4u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "reservoirs",
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
                                     .binding = 12u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "activeRayIndices",
                                     .binding = 15u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "activeRayState",
                                     .binding = 16u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                              .textures =
                                  {
                                    {.name = "geometryNormal", .binding = 8u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "motionVector", .binding = 9u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "instanceId", .binding = 10u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "voxelId", .binding = 11u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                              .storageTextures =
                                  {
                                    {.name = "reservoirReceiver", .binding = 13u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "reservoirCore", .binding = 14u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
                                    {.binding = 2u, .bufferView = view(buffers_.previousSelectedRays, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 3u, .bufferView = view(buffers_.previousReservoirs, pixels * sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 4u, .bufferView = view(buffers_.selectedRays, rays * sizeof(SparseVoxelSceneRayGPU), ::rendering::AccessPattern::SHADER_WRITE)},
                                    {.binding = 5u, .bufferView = view(buffers_.reservoirs, pixels * sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_WRITE)},
                                    {.binding = 6u, .bufferView = view(buffers_.lights, globalillumination::worldgi::worldGILightBufferSize(settings_.lightCapacity), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 7u, .bufferView = view(buffers_.linearDepth, sourcePixels * sizeof(float), ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 12u,
                                     .bufferView = view(
                                         buffers_.lightReachability,
                                         ownsLightReachability_
                                             ? sizeof(uint32_t)
                                             : sparseVoxelLightReachabilityBufferSize(
                                                   settings_.lightCapacity,
                                                   settings_.reachabilityEntryCount),
                                         ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 15u,
                                     .bufferView = view(
                                         buffers_.activeRayIndices,
                                         rays * sizeof(uint32_t),
                                         ::rendering::AccessPattern::SHADER_READ)},
                                    {.binding = 16u,
                                     .bufferView = view(
                                         buffers_.activeRayState, 16u,
                                         ::rendering::AccessPattern::SHADER_READ |
                                             ::rendering::AccessPattern::INDIRECT_COMMAND_READ)},
                                  },
                              .textures =
                                  {
                                    {.binding = 8u, .textureView = sampledView(buffers_.geometryNormal)},
                                    {.binding = 9u, .textureView = sampledView(buffers_.motionVector)},
                                    {.binding = 10u, .textureView = sampledView(buffers_.instanceId)},
                                    {.binding = 11u, .textureView = sampledView(buffers_.voxelId)},
                                  },
                              .storageTextures =
                                  {
                                    {.binding = 13u, .textureView = storageView(buffers_.reservoirReceiver)},
                                    {.binding = 14u, .textureView = storageView(buffers_.reservoirCore)},
                                  },
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-di-reservoir.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry =
        kernel_ == Kernel::Reset
            ? "resetSparseVoxelDIOutputs"
        : kernel_ == Kernel::Compact
            ? "resampleSparseVoxelDICompact"
            : "resampleSparseVoxelDICandidates";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  Settings settings_{};
  Kernel kernel_ = Kernel::Dense;
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool ownsLightReachability_ = false;
  std::vector<::rendering::Buffer> ownedFallbackBuffers_{};
};

} // namespace voxel::svraster::rendering
