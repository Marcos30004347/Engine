#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination
{
namespace surfels
{

class SurfelAccelerationInsertPass : public rendering::Pass
{
public:
  using Settings = SurfelRuntimeSettings;

  SurfelAccelerationInsertPass(
      rendering::Buffer surfelPositionRadiusBuffer,
      rendering::Buffer surfelOwnerCellBuffer,
      rendering::Buffer surfelGridBuffer,
      rendering::Buffer surfelGridWriteCursorBuffer,
      rendering::Buffer surfelListBuffer,
      Settings settings)
      : surfelPositionRadiusBuffer_(surfelPositionRadiusBuffer),
        surfelOwnerCellBuffer_(surfelOwnerCellBuffer),
        surfelGridBuffer_(surfelGridBuffer),
        surfelGridWriteCursorBuffer_(surfelGridWriteCursorBuffer),
        surfelListBuffer_(surfelListBuffer),
        settings_(settings)
  {
  }

  ~SurfelAccelerationInsertPass() override
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

  void updateUniforms(const math::Vec3f &cameraPosition, const math::Vec3f &cameraRight, const math::Vec3f &cameraUp, const math::Vec3f &cameraForward)
  {
    cameraPosition_ = cameraPosition;
    cameraRight_ = cameraRight;
    cameraUp_ = cameraUp;
    cameraForward_ = cameraForward;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!enabled_)
      return;
    ensureResources();
    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch((std::max(1u, settings_.surfelCapacity) + 127u) / 128u, 1u, 1u);
  }

private:
  struct Uniforms
  {
    float cameraPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float cameraRight[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float cameraUp[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float cameraForward[4]{0.0f, 0.0f, -1.0f, 0.0f};
    uint32_t surfelCapacity = 1u;
    uint32_t gridCapacity = 1u;
    uint32_t listCapacity = 1u;
    uint32_t lookupCellResolution = 16u;
    float centralGridHalfExtent = 0.2f;
    float gridScale = 10.0f;
    float frustumFarDistance = 4096.0f;
    float lookupRadiusScale = 1.0f;
    float spawnMinRadius = 0.05f;
    float spawnDepthRadiusScale = 0.01f;
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
                    {.name = "surfelPosR", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelOwnerCell", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelGrid", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelGridWriteCursor", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfelList", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
                    {.binding = 1, .bufferView = makeReadOnlyBufferView(surfelPositionRadiusBuffer_, surfelPositionRadiusBufferSize(settings_))},
                    {.binding = 2, .bufferView = makeStorageBufferView(surfelOwnerCellBuffer_, surfelOwnerCellBufferSize(settings_))},
                    {.binding = 3, .bufferView = makeStorageBufferView(surfelGridBuffer_, surfelGridBufferSize(settings_))},
                    {.binding = 4, .bufferView = makeStorageBufferView(surfelGridWriteCursorBuffer_, surfelGridWriteCursorBufferSize(settings_))},
                    {.binding = 5, .bufferView = makeStorageBufferView(surfelListBuffer_, surfelListBufferSize(settings_))},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/surfels/surfel-acceleration-insert-cs.spirv"),
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
    uniforms.surfelCapacity = std::max(1u, settings_.surfelCapacity);
    uniforms.gridCapacity = std::max(1u, settings_.gridCapacity);
    uniforms.listCapacity = surfelListCapacity(settings_);
    uniforms.lookupCellResolution = std::max(1u, settings_.lookupCellResolution);
    uniforms.centralGridHalfExtent = settings_.centralGridHalfExtent;
    uniforms.gridScale = settings_.gridScale;
    uniforms.frustumFarDistance = settings_.frustumFarDistance;
    uniforms.lookupRadiusScale = std::max(settings_.lookupRadiusScale, 0.1f);
    uniforms.spawnMinRadius = settings_.spawnMinRadius;
    uniforms.spawnDepthRadiusScale = settings_.spawnDepthRadiusScale;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
  }

  rendering::Buffer surfelPositionRadiusBuffer_{};
  rendering::Buffer surfelOwnerCellBuffer_{};
  rendering::Buffer surfelGridBuffer_{};
  rendering::Buffer surfelGridWriteCursorBuffer_{};
  rendering::Buffer surfelListBuffer_{};
  Settings settings_{};
  math::Vec3f cameraPosition_{0.0f, 0.0f, 0.0f};
  math::Vec3f cameraRight_{1.0f, 0.0f, 0.0f};
  math::Vec3f cameraUp_{0.0f, 1.0f, 0.0f};
  math::Vec3f cameraForward_{0.0f, 0.0f, -1.0f};
  bool enabled_ = true;

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
};

} // namespace surfels
} // namespace globalillumination
