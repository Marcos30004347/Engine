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

class SurfelRuntimeResetPass : public rendering::Pass
{
public:
  using Settings = SurfelRuntimeSettings;

  SurfelRuntimeResetPass(
      rendering::Buffer surfelStackBuffer,
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelNormalLifecycleBuffer,
      rendering::Buffer surfelAttachmentBuffer,
      rendering::Buffer surfelLifecycleBuffer,
      rendering::Buffer surfelMaterialBuffer,
      rendering::Buffer surfelIrradianceBuffer,
      rendering::Buffer surfelReservoirBuffer,
      Settings settings)
      : surfelStackBuffer_(surfelStackBuffer),
        surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelNormalLifecycleBuffer_(surfelNormalLifecycleBuffer),
        surfelAttachmentBuffer_(surfelAttachmentBuffer),
        surfelLifecycleBuffer_(surfelLifecycleBuffer),
        surfelMaterialBuffer_(surfelMaterialBuffer),
        surfelIrradianceBuffer_(surfelIrradianceBuffer),
        surfelReservoirBuffer_(surfelReservoirBuffer),
        settings_(settings)
  {
  }

  ~SurfelRuntimeResetPass() override
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

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((settings_.surfelCapacity + 63u) / 64u, 1u, 1u);
  }

private:
  struct Uniforms
  {
    uint32_t surfelCapacity = 1u;
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
                    {.name = "surfelStack", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelPosR", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelNorW", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelAttachment", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelLifecycle", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelMaterial", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelIrradiance", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelReservoir", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeStorageBufferView(surfelStackBuffer_, surfelStackBufferSize(settings_))},
                    {.binding = 2, .bufferView = makeStorageBufferView(surfelPositionRadiusBuffer_, surfelPositionRadiusBufferSize(settings_))},
                    {.binding = 3, .bufferView = makeStorageBufferView(surfelNormalLifecycleBuffer_, surfelNormalLifecycleBufferSize(settings_))},
                    {.binding = 4, .bufferView = makeStorageBufferView(surfelAttachmentBuffer_, surfelAttachmentBufferSize(settings_))},
                    {.binding = 5, .bufferView = makeStorageBufferView(surfelLifecycleBuffer_, surfelLifecycleBufferSize(settings_))},
                    {.binding = 6, .bufferView = makeStorageBufferView(surfelMaterialBuffer_, surfelMaterialBufferSize(settings_))},
                    {.binding = 7, .bufferView = makeStorageBufferView(surfelIrradianceBuffer_, surfelIrradianceBufferSize(settings_))},
                    {.binding = 8, .bufferView = makeStorageBufferView(surfelReservoirBuffer_, surfelReservoirBufferSize(settings_))},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-runtime-reset-cs.spirv"),
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
    uniforms.surfelCapacity = settings_.surfelCapacity;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  rendering::Buffer surfelStackBuffer_{};
  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelNormalLifecycleBuffer_{};
  rendering::Buffer surfelAttachmentBuffer_{};
  rendering::Buffer surfelLifecycleBuffer_{};
  rendering::Buffer surfelMaterialBuffer_{};
  rendering::Buffer surfelIrradianceBuffer_{};
  rendering::Buffer surfelReservoirBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
