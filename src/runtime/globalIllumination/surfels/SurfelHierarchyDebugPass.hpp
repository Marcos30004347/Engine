#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "globalIllumination/surfels/SurfelHierarchyTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelHierarchyDebugPass : public rendering::Pass
{
public:
  using DebugMode = SurfelHierarchyDebugMode;
  using Settings = SurfelHierarchySettings;

  SurfelHierarchyDebugPass(
      rendering::Texture sceneDepthTexture,
      rendering::Texture outputDebugTexture,
      Settings settings)
      : sceneDepthTexture_(sceneDepthTexture),
        outputDebugTexture_(outputDebugTexture),
        settings_(settings)
  {
  }

  ~SurfelHierarchyDebugPass() override
  {
    destroyFrameOverrideBuffers();
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
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void updateUniforms(
      const math::Mat4f &inverseViewProjection,
      const math::Vec3f &cameraPosition,
      const math::Vec3f &cameraRight,
      const math::Vec3f &cameraUp,
      const math::Vec3f &cameraForward,
      bool reverseZ,
      DebugMode debugMode)
  {
    inverseViewProjection_ = inverseViewProjection;
    cameraPosition_ = cameraPosition;
    cameraRight_ = cameraRight;
    cameraUp_ = cameraUp;
    cameraForward_ = cameraForward;
    reverseZ_ = reverseZ;
    debugMode_ = debugMode;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
    {
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "surfelHierarchyUniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "sceneDepthTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputDebugTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = makeReadOnlyBufferView(uniformBuffer_, sizeof(Uniforms))},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                },
                .storageTextures = {
                    {.binding = 2, .textureView = makeStorageColorView(outputDebugTexture_)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-hierarchy-debug-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch((settings_.viewPortWidth + 7u) / 8u, (settings_.viewPortHeight + 7u) / 8u, 1u);
  }

private:
  struct Uniforms
  {
    math::Mat4f inverseViewProjection = math::Mat4f::identity();
    float cameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float cameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    uint32_t viewport[2]{1u, 1u};
    uint32_t reverseZ = 0u;
    uint32_t debugMode = 0u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    uint32_t gridCapacity = surfelLookupTotalCellCount(16u);
    uint32_t _padding0 = 0u;
  };

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot),
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;
    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  rendering::BufferId getCurrentFrameOverrideUniformBufferId() const
  {
    return getFrameOverrideUniformBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection_;
    uniforms.cameraPosition[0] = cameraPosition_.x();
    uniforms.cameraPosition[1] = cameraPosition_.y();
    uniforms.cameraPosition[2] = cameraPosition_.z();
    uniforms.cameraRight[0] = cameraRight_.x();
    uniforms.cameraRight[1] = cameraRight_.y();
    uniforms.cameraRight[2] = cameraRight_.z();
    uniforms.cameraUp[0] = cameraUp_.x();
    uniforms.cameraUp[1] = cameraUp_.y();
    uniforms.cameraUp[2] = cameraUp_.z();
    uniforms.cameraForward[0] = cameraForward_.x();
    uniforms.cameraForward[1] = cameraForward_.y();
    uniforms.cameraForward[2] = cameraForward_.z();
    uniforms.viewport[0] = settings_.viewPortWidth;
    uniforms.viewport[1] = settings_.viewPortHeight;
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.debugMode = static_cast<uint32_t>(debugMode_);
    uniforms.lookupCellResolution = std::max(1u, settings_.lookupCellResolution);
    uniforms.centralGridHalfExtent = settings_.centralGridHalfExtent;
    uniforms.gridScale = settings_.gridScale;
    uniforms.frustumFarDistance = settings_.frustumFarDistance;
    uniforms.gridCapacity = settings_.gridCapacity;

    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
    }
    else
    {
      renderGraph->getRHI()->bufferWrite(getCurrentFrameOverrideUniformBufferId(), 0u, sizeof(Uniforms), &uniforms);
    }
  }

  rendering::Texture sceneDepthTexture_{};
  rendering::Texture outputDebugTexture_{};
  Settings settings_{};
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f cameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f cameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f cameraForward_{0.0f, 0.0f, -1.0f};
  bool reverseZ_ = false;
  DebugMode debugMode_ = DebugMode::Region;
  rendering::Buffer uniformBuffer_{};
  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
};

} // namespace surfels
} // namespace globalillumination
