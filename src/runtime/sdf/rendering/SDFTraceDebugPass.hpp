#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "sdf/SDFSceneManager.hpp"

namespace sdf::rendering
{

class SDFTraceDebugPass : public ::rendering::Pass
{
public:
  enum class Mode : uint32_t
  {
    LocalInstanceRaymarch = 6u,
    LocalInstanceNormal = 7u,
    LocalInstanceAlbedo = 8u,
    LocalInstanceCascades = 9u,
    LocalInstanceCascadeInstances = 10u,
    LocalInstanceBlinn = 11u,
    LocalInstanceStepCount = 12u,
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    Mode mode = Mode::LocalInstanceRaymarch;
    uint32_t maxSteps = 256u;
    float maxTraceDistance = 10000.0f;
    float hitThresholdScale = 0.25f;
    float stepScale = 0.95f;
    float minStepScale = 0.1f;
    math::Vec4f traceHeatmapLowColor = math::Vec4f(0.02f, 0.03f, 0.08f, 1.0f);
    math::Vec4f traceHeatmapMidColor = math::Vec4f(0.10f, 0.72f, 0.50f, 1.0f);
    math::Vec4f traceHeatmapHighColor = math::Vec4f(0.92f, 0.26f, 0.12f, 1.0f);
    float traceHeatmapLowCount = 8.0f;
    float traceHeatmapMidCount = 64.0f;
    float traceHeatmapHighCount = 192.0f;
    bool enabled = true;
  };

  SDFTraceDebugPass(SDFSceneManager &sceneManager, ::rendering::Texture outputTexture, Settings settings)
      : sceneManager_(sceneManager), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~SDFTraceDebugPass() override
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

  void setEnabled(bool enabled)
  {
    settings_.enabled = enabled;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void setSettings(Settings settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void setMode(Mode mode)
  {
    settings_.mode = mode;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void updateRuntimeState(const math::Mat4f &inverseViewProjection, const math::Vec3f &cameraPosition, bool reverseZ)
  {
    inverseViewProjection_ = inverseViewProjection;
    cameraPosition_ = cameraPosition;
    reverseZ_ = reverseZ;
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
    commandBuffer.cmdDispatch((settings_.width + kWorkgroupSizeX - 1u) / kWorkgroupSizeX, (settings_.height + kWorkgroupSizeY - 1u) / kWorkgroupSizeY, 1u);
  }

private:
  static constexpr uint32_t kWorkgroupSizeX = 8u;
  static constexpr uint32_t kWorkgroupSizeY = 8u;

  struct Uniforms
  {
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    math::Vec3f cameraPosition = math::Vec3f(0.0f, 0.0f, 0.0f);
    float maxTraceDistance = 10000.0f;
    uint32_t outputWidth = 1u;
    uint32_t outputHeight = 1u;
    uint32_t mode = static_cast<uint32_t>(Mode::LocalInstanceRaymarch);
    uint32_t maxSteps = 256u;
    float hitThresholdScale = 0.25f;
    float stepScale = 0.95f;
    float minStepScale = 0.1f;
    uint32_t reverseZ = 1u;
    uint32_t enabled = 1u;
    uint32_t _padding0[3] = {};
    math::Vec4f traceHeatmapLowColor = math::Vec4f(0.02f, 0.03f, 0.08f, 1.0f);
    math::Vec4f traceHeatmapMidColor = math::Vec4f(0.10f, 0.72f, 0.50f, 1.0f);
    math::Vec4f traceHeatmapHighColor = math::Vec4f(0.92f, 0.26f, 0.12f, 1.0f);
    math::Vec4f traceHeatmapCounts = math::Vec4f(8.0f, 64.0f, 192.0f, 0.0f);
  };
  static_assert(sizeof(Uniforms) == 192u, "SDF trace debug uniforms must match WGSL layout");
  static_assert(offsetof(Uniforms, cameraPosition) == 64u, "SDF trace debug camera offset must match WGSL layout");
  static_assert(offsetof(Uniforms, traceHeatmapLowColor) == 128u, "SDF trace debug heatmap offset must match WGSL layout");
  static_assert(offsetof(Uniforms, traceHeatmapCounts) == 176u, "SDF trace debug heatmap counts offset must match WGSL layout");

  static ::rendering::BufferView makeBufferView(::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
  {
    return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  static ::rendering::TextureView makeStorageColorView(::rendering::Texture texture)
  {
    return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE,
        .layout = ::rendering::ResourceLayout::GENERAL,
        .index = 0,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
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
    if (!outputTexture_.isValid())
      throw std::runtime_error("SDFTraceDebugPass requires a valid output texture");

    sceneManager_.setRenderGraph(renderGraph);
    if (!sceneManager_.gpuResources().isValid())
      sceneManager_.uploadGPUResources();
    if (!sceneManager_.gpuResources().isValid())
      throw std::runtime_error("SDFTraceDebugPass requires uploaded SDFSceneManager GPU resources");

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
                  .storageTextures = {
                      {.name = "outputTexture", .binding = 11u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
                .storageTextures = {
                    {.binding = 11u, .textureView = makeStorageColorView(outputTexture_)},
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
              .src = os::io::readRelativeFile("assets/shaders/spirv/sdf-trace-debug.spirv"),
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
    uniforms.inverseViewProjection = inverseViewProjection_;
    uniforms.cameraPosition = cameraPosition_;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.outputWidth = settings_.width;
    uniforms.outputHeight = settings_.height;
    uniforms.mode = static_cast<uint32_t>(settings_.mode);
    uniforms.maxSteps = settings_.maxSteps;
    uniforms.hitThresholdScale = settings_.hitThresholdScale;
    uniforms.stepScale = settings_.stepScale;
    uniforms.minStepScale = settings_.minStepScale;
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.traceHeatmapLowColor = settings_.traceHeatmapLowColor;
    uniforms.traceHeatmapMidColor = settings_.traceHeatmapMidColor;
    uniforms.traceHeatmapHighColor = settings_.traceHeatmapHighColor;
    uniforms.traceHeatmapCounts = math::Vec4f(
        std::max(settings_.traceHeatmapLowCount, 0.0f),
        std::max(settings_.traceHeatmapMidCount, settings_.traceHeatmapLowCount + 1.0f),
        std::max(settings_.traceHeatmapHighCount, settings_.traceHeatmapMidCount + 1.0f),
        0.0f);
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  SDFSceneManager &sceneManager_;
  ::rendering::Texture outputTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_ = math::Vec3f(0.0f, 0.0f, 0.0f);
  bool reverseZ_ = true;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::Sampler distanceAtlasSampler_{};
  uint64_t boundGpuResourceRevision_ = UINT64_MAX;
};

} // namespace sdf::rendering
