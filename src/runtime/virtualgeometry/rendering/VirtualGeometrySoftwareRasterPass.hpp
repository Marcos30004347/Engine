#pragma once

#include "os/File.hpp"
#include "os/Logger.hpp"
#if defined(__APPLE__)
#include "voxel/svraster/rendering/TerrainMetalComputeRasterPass.hpp"
#endif
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace virtualgeometry::gpgpu
{

// Software rasterizer for the clusters the culler routed away from the
// hardware draw. Runs in the render graph, in WGSL, and writes the same
// visibility key the hardware path produces - see
// virtualgeometry-software-raster-cs.wgsl for the two-pass nearest-wins.
//
// One instance per occlusion phase: each owns a depth and a payload buffer
// for its phase, clears them at the start of its record, and leaves them for
// the merge pass. Phase one's buffers are not touched again by phase two, so
// the final merge can fold both phases with the hardware attachments.
class VirtualGeometrySoftwareRasterPass final : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
  };

  struct Uniforms
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t padding0 = 0u;
    uint32_t padding1 = 0u;
  };
  static_assert(sizeof(Uniforms) == 16u);

  static constexpr uint32_t CLEAR_WORKGROUP_SIZE = 256u;

  VirtualGeometrySoftwareRasterPass(
      VirtualGeometryScene &scene,
      ::rendering::Buffer cullingUniforms,
      ::rendering::Buffer cullingCounters,
      ::rendering::Buffer swVisibleClusterInfos,
      ::rendering::Buffer swDispatchArgs,
      Settings settings)
      : scene_(scene),
        cullingUniforms_(cullingUniforms),
        cullingCounters_(cullingCounters),
        swVisibleClusterInfos_(swVisibleClusterInfos),
        swDispatchArgs_(swDispatchArgs),
        settings_(settings)
  {
  }

  ~VirtualGeometrySoftwareRasterPass() override
  {
    if (clearPipeline_.isValid())
      renderGraph->deleteComputePipeline(clearPipeline_);
    if (depthPipeline_.isValid())
      renderGraph->deleteComputePipeline(depthPipeline_);
    if (payloadPipeline_.isValid())
      renderGraph->deleteComputePipeline(payloadPipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (keyBuffer_.isValid())
      renderGraph->deleteBuffer(keyBuffer_);
    if (depthBuffer_.isValid())
      renderGraph->deleteBuffer(depthBuffer_);
    if (payloadBuffer_.isValid())
      renderGraph->deleteBuffer(payloadBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    const auto barrier = [&]()
    {
      commandBuffer.cmdMemoryBarrier(
          ::rendering::PipelineStage::ALL_COMMANDS,
          ::rendering::PipelineStage::ALL_COMMANDS,
          ::rendering::AccessPattern::SHADER_WRITE | ::rendering::AccessPattern::MEMORY_WRITE,
          ::rendering::AccessPattern::MEMORY_READ | ::rendering::AccessPattern::SHADER_READ_WRITE |
              ::rendering::AccessPattern::INDIRECT_COMMAND_READ);
    };
    const uint32_t pixelCount = std::max(1u, settings_.viewportWidth * settings_.viewportHeight);

    commandBuffer.cmdBindComputePipeline(clearPipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
#if defined(__APPLE__)
    if (native_ != nullptr)
    {
      commandBuffer.recorded.back().commands.push_back(std::make_shared<NativeVisibilityCommand>([this]() {
        NativeRaster::VisibilityFrame frame{};
        const ::rendering::Buffer inputs[] = {cullingUniforms_, uniformBuffer_,
            scene_.instanceBuffer, scene_.pageTableBuffer, swVisibleClusterInfos_,
            cullingCounters_, scene_.decodedClusterGeometryBuffer, scene_.decodedVertexBuffer,
            scene_.decodedIndexBuffer, keyBuffer_, depthBuffer_, payloadBuffer_, swDispatchArgs_, scene_.pagesBuffer};
        for (size_t i=0; i<frame.buffers.size(); ++i)
          frame.buffers[i] = renderGraph->resolveBufferId(inputs[i]);
        frame.width=settings_.viewportWidth; frame.height=settings_.viewportHeight;
        return [native=native_, frame](void *event, uint64_t ready, uint64_t done) {
          native->encodeVisibility(frame,event,ready,done);
        };
      }));
      barrier();
      return;
    }
#endif
    commandBuffer.cmdDispatch((pixelCount + CLEAR_WORKGROUP_SIZE - 1u) / CLEAR_WORKGROUP_SIZE, 1u, 1u);
    barrier();

    commandBuffer.cmdBindComputePipeline(depthPipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(swDispatchArgs_, 0u);
    barrier();

    commandBuffer.cmdBindComputePipeline(payloadPipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(swDispatchArgs_, 0u);
    barrier();
  }

  const ::rendering::Buffer &getDepthBuffer() const { return depthBuffer_; }
  const ::rendering::Buffer &getPayloadBuffer() const { return payloadBuffer_; }
  uint64_t pixelBufferBytes() const { return pixelBufferBytes_; }
  const Settings &settings() const { return settings_; }
  bool usesNativeAtomic64() const
  {
#if defined(__APPLE__)
    return native_ != nullptr;
#else
    return false;
#endif
  }


private:
  void ensureResources()
  {
    if (layout_.isValid())
      return;
    using namespace ::rendering;

#if defined(__APPLE__)
    if (scene_.settings.metalExportableBuffers)
    {
      auto candidate = std::make_shared<NativeRaster>(*renderGraph->getRHI(),
          NativeRaster::Configuration{.visibilityOnly = true});
      if (candidate->visibilityAvailable())
      {
        native_ = std::move(candidate);
        os::Logger::log("VG compute raster: native atomic64, GPU timeline ordered, indirect cluster dispatch");
      }
    }
#endif
    pixelBufferBytes_ =
        static_cast<uint64_t>(std::max(1u, settings_.viewportWidth * settings_.viewportHeight)) * sizeof(uint32_t);
    depthBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_VisibilityDepth.buffer",
      .size = pixelBufferBytes_,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      .metalExportable = scene_.settings.metalExportableBuffers,
    });
    payloadBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_VisibilityPayload.buffer",
      .size = pixelBufferBytes_,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      .metalExportable = scene_.settings.metalExportableBuffers,
    });
    uniformBuffer_ = renderGraph->createBuffer(BufferInfo{
      .name = passName + "_Uniforms.buffer",
      .size = sizeof(Uniforms),
      .usage = BufferUsage::BufferUsage_Uniform | BufferUsage::BufferUsage_Push,
      .metalExportable = scene_.settings.metalExportableBuffers,
    });
    keyBuffer_ = createFrameLocalBuffer(BufferInfo{
      .name = passName + "_VisibilityAtomic64.buffer",
      .size = pixelBufferBytes_ * 2u,
      .usage = BufferUsage::BufferUsage_Storage,
      .metalExportable = scene_.settings.metalExportableBuffers,
    });
    Uniforms uniforms{};
    uniforms.viewportWidth = settings_.viewportWidth;
    uniforms.viewportHeight = settings_.viewportHeight;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);

    const auto storage = [](const char *name, const uint32_t binding)
    {
      return BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      };
    };
    layout_ = renderGraph->createBindingsLayout(BindingsLayoutInfo{
      .name = passName + "_Layout",
      .groups = {BindingGroupLayout{
        .buffers = {
          {.name = "cullingUniforms", .binding = 0u, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
          {.name = "raster", .binding = 1u, .type = BufferBindingType::BufferBindingType_UniformBuffer, .visibility = BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
          storage("instances", 2u),
          storage("pageTable", 3u),
          storage("swVisibleClusterInfos", 4u),
          storage("cullingCounters", 5u),
          storage("decodedClusterGeometryInfos", 6u),
          storage("pagesBuffer", 13u),
          storage("decodedVertexBuffer", 7u),
          storage("decodedIndexBuffer", 8u),
          storage("visibilityDepth", 9u),
          storage("visibilityPayload", 10u),
          storage("visibilityKeys", 11u),
          storage("dispatchArgs", 12u),
        },
      }},
    });

    const uint64_t instanceBytes = std::max<uint64_t>(scene_.instanceBufferSize, sizeof(VirtualGeometryInstanceGPUData));
    const auto view = [](const Buffer &buffer, const uint64_t size, const AccessPattern access)
    {
      return BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    groups_ = renderGraph->createBindingGroups(BindingGroupsInfo{
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {GroupInfo{
        .name = "group0",
        .buffers = {
          {.bufferView = view(cullingUniforms_, renderGraph->getBufferSize(cullingUniforms_), AccessPattern::UNIFORM_READ), .binding = 0u},
          {.bufferView = view(uniformBuffer_, sizeof(Uniforms), AccessPattern::UNIFORM_READ), .binding = 1u},
          {.bufferView = view(scene_.instanceBuffer, instanceBytes, AccessPattern::SHADER_READ), .binding = 2u},
          {.bufferView = view(scene_.pageTableBuffer, scene_.pagesTableBufferSize, AccessPattern::SHADER_READ), .binding = 3u},
          {.bufferView = view(swVisibleClusterInfos_, renderGraph->getBufferSize(swVisibleClusterInfos_), AccessPattern::SHADER_READ), .binding = 4u},
          {.bufferView = view(cullingCounters_, renderGraph->getBufferSize(cullingCounters_), AccessPattern::SHADER_READ), .binding = 5u},
          {.bufferView = view(scene_.decodedClusterGeometryBuffer, scene_.decodedClusterGeometryBufferSize, AccessPattern::SHADER_READ), .binding = 6u},
          {.bufferView = view(scene_.pagesBuffer, scene_.pagesBufferSize, AccessPattern::SHADER_READ), .binding = 13u},
          {.bufferView = view(scene_.decodedVertexBuffer, scene_.decodedVertexBufferSize, AccessPattern::SHADER_READ), .binding = 7u},
          {.bufferView = view(scene_.decodedIndexBuffer, scene_.decodedIndexBufferSize, AccessPattern::SHADER_READ), .binding = 8u},
          {.bufferView = view(depthBuffer_, pixelBufferBytes_, AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE), .binding = 9u},
          {.bufferView = view(payloadBuffer_, pixelBufferBytes_, AccessPattern::SHADER_READ | AccessPattern::SHADER_WRITE), .binding = 10u},
          {.bufferView = view(keyBuffer_, pixelBufferBytes_ * 2u, AccessPattern::SHADER_READ_WRITE), .binding = 11u},
          {.bufferView = view(swDispatchArgs_, renderGraph->getBufferSize(swDispatchArgs_), AccessPattern::SHADER_READ | AccessPattern::INDIRECT_COMMAND_READ), .binding = 12u},
        },
      }},
    });

    shader_ = renderGraph->createShader(ShaderInfo{
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-software-raster-cs.spirv"),
      .type = ShaderType::SpirV,
      .layout = layout_,
    });
    clearPipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{.name = passName + "_Clear.pipeline", .shader = shader_, .entry = "clearVisibility", .layout = layout_});
    depthPipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{.name = passName + "_Depth.pipeline", .shader = shader_, .entry = "rasterizeDepth", .layout = layout_});
    payloadPipeline_ = renderGraph->createComputePipeline(ComputePipelineInfo{.name = passName + "_Payload.pipeline", .shader = shader_, .entry = "rasterizePayload", .layout = layout_});
  }

#if defined(__APPLE__)
  using NativeRaster = ::voxel::svraster::rendering::TerrainMetalComputeRasterPass;
  std::shared_ptr<NativeRaster> native_;
  class NativeVisibilityCommand final : public ::rendering::Command
  {
  public:
    std::function<::rendering::RHI::MetalComputeSubmission()> prepareSubmission;
    explicit NativeVisibilityCommand(decltype(prepareSubmission) callback)
        : prepareSubmission(std::move(callback)) {}
    std::shared_ptr<::rendering::Command> clone() const override
    { return std::make_shared<NativeVisibilityCommand>(*this); }
    ::rendering::Queue queueHint() const override { return ::rendering::Queue::Compute; }
    bool isDispatchCommand() const override { return true; }
    bool triggersNodeSplit() const override { return true; }
    void run(::rendering::CommandRunContext &context) const override
    { context.rhi->cmdMetalCompute(context.commandBuffer, prepareSubmission()); }
  };
#endif
  ::rendering::Buffer keyBuffer_{};
  VirtualGeometryScene &scene_;
  ::rendering::Buffer cullingUniforms_{};
  ::rendering::Buffer cullingCounters_{};
  ::rendering::Buffer swVisibleClusterInfos_{};
  ::rendering::Buffer swDispatchArgs_{};
  Settings settings_{};
  uint64_t pixelBufferBytes_ = 0u;
  ::rendering::Buffer depthBuffer_{};
  ::rendering::Buffer payloadBuffer_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline clearPipeline_{};
  ::rendering::ComputePipeline depthPipeline_{};
  ::rendering::ComputePipeline payloadPipeline_{};
};

} // namespace virtualgeometry::gpgpu
