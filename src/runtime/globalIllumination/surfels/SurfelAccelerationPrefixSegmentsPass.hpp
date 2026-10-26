#pragma once

#include <cstdint>
#include <string>

#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelAccelerationPrefixSegmentsPass : public rendering::Pass
{
public:
  using Settings = SurfelRuntimeSettings;

  SurfelAccelerationPrefixSegmentsPass(
      rendering::Buffer surfelPrefixSegmentsBuffer,
      Settings settings)
      : surfelPrefixSegmentsBuffer_(surfelPrefixSegmentsBuffer),
        settings_(settings)
  {
  }

  ~SurfelAccelerationPrefixSegmentsPass() override
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

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  struct Uniforms
  {
    uint32_t segmentCount = 1u;
    uint32_t _padding0[3]{};
  };

  static rendering::BufferView makeStorageBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
    };
  }

  static rendering::BufferView makeReadOnlyBufferView(rendering::Buffer buffer, uint64_t size)
  {
    return rendering::BufferView{
        .buffer = buffer,
        .offset = 0u,
        .size = size,
        .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }

    if (layout_.isValid())
      return;

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "segments", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeStorageBufferView(surfelPrefixSegmentsBuffer_, surfelPrefixSegmentsBufferSize(settings_))},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-acceleration-prefix-segments-cs.spirv"),
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
    uniforms.segmentCount = surfelPrefixSegmentCount(settings_);
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  rendering::Buffer surfelPrefixSegmentsBuffer_{};
  Settings settings_{};
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
