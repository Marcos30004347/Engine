#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "sdf/SDFSceneManager.hpp"

namespace sdf::rendering
{

class SDFTracePass : public ::rendering::Pass
{
public:
  struct Settings
  {
    uint32_t rayCount = 1u;
    uint32_t rayOffset = 0u;
    uint32_t hitOffset = 0u;
    uint32_t maxSteps = 256u;
    float maxTraceDistance = 10000.0f;
    float hitThresholdScale = 1.0f;
    float stepScale = 0.95f;
    float minStepScale = 0.25f;
    bool enabled = true;
  };

  SDFTracePass(
      SDFSceneManager &sceneManager,
      ::rendering::Buffer inputRayBuffer,
      ::rendering::Buffer outputHitBuffer,
      Settings settings)
      : sceneManager_(sceneManager), inputRayBuffer_(inputRayBuffer), outputHitBuffer_(outputHitBuffer), settings_(settings)
  {
  }

  ~SDFTracePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (distanceAtlasSampler_.isValid())
      renderGraph->deleteSampler(distanceAtlasSampler_);
  }

  static constexpr uint64_t getInputRayBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount) * sizeof(gpusigneddistancefields::gpgpu::TraceRayGPU);
  }

  static constexpr uint64_t getOutputHitBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount) * sizeof(gpusigneddistancefields::gpgpu::TraceHitGPU);
  }

  void setSettings(Settings settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    if (!settings_.enabled)
      return;
    ensureResources();
    writeUniforms();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.rayCount + kWorkgroupSize - 1u) / kWorkgroupSize, 1u, 1u);
  }

private:
  static constexpr uint32_t kWorkgroupSize = 64u;

  struct Uniforms
  {
    uint32_t rayCount = 1u;
    uint32_t rayOffset = 0u;
    uint32_t hitOffset = 0u;
    uint32_t maxSteps = 256u;
    float maxTraceDistance = 10000.0f;
    float hitThresholdScale = 1.0f;
    float stepScale = 0.95f;
    float minStepScale = 0.25f;
  };

  static ::rendering::BufferView makeBufferView(::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
  {
    return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  static ::rendering::TextureView makeSampledColor3DView(::rendering::Texture texture)
  {
    return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    sceneManager_.setRenderGraph(renderGraph);
    if (!sceneManager_.gpuResources().isValid())
      sceneManager_.uploadGPUResources();
    if (!sceneManager_.gpuResources().isValid())
      throw std::runtime_error("SDFTracePass requires uploaded SDFSceneManager GPU resources");

    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!distanceAtlasSampler_.isValid())
    {
      distanceAtlasSampler_ = renderGraph->createSampler(
          ::rendering::SamplerInfo{
              .name = passName + "_DistanceAtlasSampler",
              .minFilter = ::rendering::Filter::Linear,
              .magFilter = ::rendering::Filter::Linear,
              .addressModeU = ::rendering::SamplerAddressMode::ClampToEdge,
              .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
              .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
              .anisotropyEnable = false,
              .maxAnisotropy = 1.0f,
              .maxLod = 1.0f,
          });
    }
    if (!layout_.isValid())
    {
      layout_ = renderGraph->createBindingsLayout(
          ::rendering::BindingsLayoutInfo{
              .name = passName + "_Layout",
              .groups = {::rendering::BindingGroupLayout{
                  .buffers =
                      {
                          {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneInfo", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "rays", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "hits", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "instances", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blases", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blasNodes", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blasBricks", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "distanceWords", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tlasNodes", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tlasInstanceRefs", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blasNodeChildWordPrefixes", .binding = 14u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tracePageTables", .binding = 17u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tracePages", .binding = 18u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tracePageMasks", .binding = 19u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "traceBrickMaps", .binding = 20u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridInfo", .binding = 21u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridCells", .binding = 22u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridRefs", .binding = 23u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridGlobalRefs", .binding = 24u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .samplers = {
                      {.name = "distanceAtlasSampler", .binding = 13u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                  },
                  .textures = {
                      {.name = "distanceAtlasTexture", .binding = 12u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              }},
          });
    }

    const SDFGPUResources &resources = sceneManager_.gpuResources();
    const uint64_t resourceRevision = sceneManager_.gpuResourceRevision();
    if (!bindingGroups_.isValid() || boundGpuResourceRevision_ != resourceRevision)
    {
      if (bindingGroups_.isValid())
        renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                        {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 1u, .bufferView = makeBufferView(resources.sceneInfo, sizeof(SDFSceneInfoGPU), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 2u, .bufferView = makeBufferView(inputRayBuffer_, getInputRayBufferSize(std::max(settings_.rayCount + settings_.rayOffset, 1u)), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 3u, .bufferView = makeBufferView(outputHitBuffer_, getOutputHitBufferSize(std::max(settings_.rayCount + settings_.hitOffset, 1u)), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
                        {.binding = 4u, .bufferView = makeBufferView(resources.instances, renderGraph->getBufferSize(resources.instances), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 5u, .bufferView = makeBufferView(resources.blases, renderGraph->getBufferSize(resources.blases), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 6u, .bufferView = makeBufferView(resources.blasNodes, renderGraph->getBufferSize(resources.blasNodes), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 7u, .bufferView = makeBufferView(resources.blasBricks, renderGraph->getBufferSize(resources.blasBricks), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 8u, .bufferView = makeBufferView(resources.distanceWords, renderGraph->getBufferSize(resources.distanceWords), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 9u, .bufferView = makeBufferView(resources.tlasNodes, renderGraph->getBufferSize(resources.tlasNodes), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 10u, .bufferView = makeBufferView(resources.tlasInstanceRefs, renderGraph->getBufferSize(resources.tlasInstanceRefs), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 14u, .bufferView = makeBufferView(resources.blasNodeChildWordPrefixes, renderGraph->getBufferSize(resources.blasNodeChildWordPrefixes), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 17u, .bufferView = makeBufferView(resources.tracePageTables, renderGraph->getBufferSize(resources.tracePageTables), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 18u, .bufferView = makeBufferView(resources.tracePages, renderGraph->getBufferSize(resources.tracePages), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 19u, .bufferView = makeBufferView(resources.tracePageMasks, renderGraph->getBufferSize(resources.tracePageMasks), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 20u, .bufferView = makeBufferView(resources.traceBrickMaps, renderGraph->getBufferSize(resources.traceBrickMaps), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 21u, .bufferView = makeBufferView(resources.sceneGridInfo, renderGraph->getBufferSize(resources.sceneGridInfo), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 22u, .bufferView = makeBufferView(resources.sceneGridCells, renderGraph->getBufferSize(resources.sceneGridCells), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 23u, .bufferView = makeBufferView(resources.sceneGridRefs, renderGraph->getBufferSize(resources.sceneGridRefs), ::rendering::AccessPattern::SHADER_READ)},
                        {.binding = 24u, .bufferView = makeBufferView(resources.sceneGridGlobalRefs, renderGraph->getBufferSize(resources.sceneGridGlobalRefs), ::rendering::AccessPattern::SHADER_READ)},
                    },
                .samplers = {
                    {.sampler = distanceAtlasSampler_, .view = makeSampledColor3DView(resources.distanceAtlas), .binding = 13u},
                },
                .textures = {
                    {.binding = 12u, .textureView = makeSampledColor3DView(resources.distanceAtlas)},
                },
            }},
        });
      boundGpuResourceRevision_ = resourceRevision;
    }

    if (!shader_.isValid())
    {
      shader_ = renderGraph->createShader(
          ::rendering::ShaderInfo{
              .name = passName + "_Shader",
              .layout = layout_,
              .src = os::io::readRelativeFile("assets/shaders/spirv/sdf-trace.spirv"),
              .type = ::rendering::ShaderType::SpirV,
          });
    }
    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
              .entry = "cs_main",
              .layout = layout_,
              .name = passName + "_Pipeline",
              .shader = shader_,
          });
    }
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.rayCount = settings_.rayCount;
    uniforms.rayOffset = settings_.rayOffset;
    uniforms.hitOffset = settings_.hitOffset;
    uniforms.maxSteps = settings_.maxSteps;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.hitThresholdScale = settings_.hitThresholdScale;
    uniforms.stepScale = settings_.stepScale;
    uniforms.minStepScale = settings_.minStepScale;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  SDFSceneManager &sceneManager_;
  ::rendering::Buffer inputRayBuffer_{};
  ::rendering::Buffer outputHitBuffer_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::Sampler distanceAtlasSampler_{};
  uint64_t boundGpuResourceRevision_ = UINT64_MAX;
};

} // namespace sdf::rendering
