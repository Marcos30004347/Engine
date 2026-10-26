#pragma once

#include <cstdint>
#include <string>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsAtlasDebugPass : public rendering::Pass
{
public:
  enum class Axis : uint32_t
  {
    X = 0u,
    Y = 1u,
    Z = 2u,
  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    Axis axis = Axis::Z;
    uint32_t sliceIndex = 0u;
    bool showBrickGrid = true;
  };

  GPUSignedDistanceFieldsAtlasDebugPass(
      GPUSignedDistanceFieldsManager &manager,
      rendering::Texture outputTexture,
      Settings settings)
      : manager_(manager), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~GPUSignedDistanceFieldsAtlasDebugPass() override
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
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + kWorkgroupSizeX - 1u) / kWorkgroupSizeX,
        (settings_.height + kWorkgroupSizeY - 1u) / kWorkgroupSizeY,
        1u);
  }

private:
  static constexpr uint32_t kWorkgroupSizeX = 8u;
  static constexpr uint32_t kWorkgroupSizeY = 8u;

  struct Uniforms
  {
    uint32_t outputWidth = 1u;
    uint32_t outputHeight = 1u;
    uint32_t atlasResolution = 1u;
    uint32_t atlasBricksPerAxis = 1u;
    uint32_t axis = 2u;
    uint32_t sliceIndex = 0u;
    uint32_t showBrickGrid = 1u;
    uint32_t enabled = 1u;
  };
  static_assert(sizeof(Uniforms) % 16u == 0u, "Atlas debug uniforms must be 16-byte aligned");

  static rendering::TextureView makeSampledColor3DView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
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

  void ensureResources()
  {
    if (uniformBuffer_.isValid())
    {
      return;
    }

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {
                rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                    .textures = {
                        {.name = "sdfAtlasTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                    .storageTextures = {
                        {.name = "outputTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                },
            },
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                    .textures = {
                        {.binding = 1, .textureView = makeSampledColor3DView(manager_.getSharedResources().sdfAtlas)},
                    },
                    .storageTextures = {
                        {.binding = 2, .textureView = makeStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-debug-atlas.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.outputWidth = settings_.width;
    uniforms.outputHeight = settings_.height;
    uniforms.atlasBricksPerAxis = manager_.getSettings().atlasBrickCapacityPerAxis;
    uniforms.atlasResolution = manager_.getSettings().atlasBrickCapacityPerAxis * GPUSignedDistanceFieldsManager::BRICK_RESOLUTION;
    uniforms.axis = static_cast<uint32_t>(settings_.axis);
    uniforms.sliceIndex = settings_.sliceIndex;
    uniforms.showBrickGrid = settings_.showBrickGrid ? 1u : 0u;
    uniforms.enabled = enabled_ ? 1u : 0u;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  GPUSignedDistanceFieldsManager &manager_;
  rendering::Texture outputTexture_;
  Settings settings_{};
  bool enabled_ = false;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
