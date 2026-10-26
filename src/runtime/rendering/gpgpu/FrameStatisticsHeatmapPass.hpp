#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace passes
{

class FrameStatisticsHeatmapPass : public Pass
{
public:
  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t blueThreshold = 10u;
    uint32_t greenThreshold = 20u;
    uint32_t redThreshold = 30u;
  };

  FrameStatisticsHeatmapPass(rendering::Buffer inputBuffer, rendering::Texture outputTexture, Settings settings)
      : inputBuffer(inputBuffer), outputTexture(outputTexture), settings(settings)
  {
  }

  ~FrameStatisticsHeatmapPass() override
  {
    renderGraph->deleteComputePipeline(pipeline);
    renderGraph->deleteShader(shader);
    renderGraph->deleteBindingGroups(bindingGroups);
    renderGraph->deleteBindingsLayout(layout);
    renderGraph->deleteBuffer(uniformBuffer);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = sizeof(Uniforms),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    const Uniforms uniforms = {
      .width = settings.width,
      .height = settings.height,
      .blueThreshold = settings.blueThreshold,
      .greenThreshold = settings.greenThreshold,
      .redThreshold = settings.redThreshold,
    };
    renderGraph->bufferWrite(uniformBuffer, 0, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms));

    layout = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {rendering::BindingGroupLayout{
              .buffers =
                  {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "statistics", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
              .storageTextures =
                  {
                    {.name = "outputTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
          }},
        });

    bindingGroups = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
          .layout = layout,
          .name = passName + "_Groups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers =
                  {
                    {.binding = 0, .bufferView = {.buffer = uniformBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                    {.binding = 1, .bufferView = {.buffer = inputBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = renderGraph->getBufferSize(inputBuffer)}},
                  },
              .storageTextures =
                  {
                    {.binding = 2,
                     .textureView =
                         {
                           .texture = outputTexture,
                           .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
                           .layout = rendering::ResourceLayout::GENERAL,
                           .index = 0,
                           .flags = rendering::ImageAspectFlags::Color,
                           .baseArrayLayer = 0,
                           .baseMipLevel = 0,
                           .layerCount = 1,
                           .levelCount = 1,
                         }},
                  },
          }},
        });

    shader = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout,
          .src = os::io::readRelativeFile("assets/shaders/spirv/frameStatisticsHeatmap-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    pipeline = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "cs_main",
          .layout = layout,
          .name = passName + "_Pipeline",
          .shader = shader,
        });

    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(bindingGroups, nullptr, 0);
    commandBuffer.cmdDispatch((settings.width + 7u) / 8u, (settings.height + 7u) / 8u, 1u);
  }

private:
  struct Uniforms
  {
    uint32_t width;
    uint32_t height;
    uint32_t blueThreshold;
    uint32_t greenThreshold;
    uint32_t redThreshold;
  };

  rendering::BindingsLayout layout;
  rendering::BindingGroups bindingGroups;
  rendering::Shader shader;
  rendering::ComputePipeline pipeline;
  rendering::Buffer uniformBuffer;

  rendering::Buffer inputBuffer;
  rendering::Texture outputTexture;
  Settings settings;
};

} // namespace passes
} // namespace rendering
