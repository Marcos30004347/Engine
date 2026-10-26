#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace voxel::svraster::rendering
{

class SparseVoxelBootstrapPass : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    ClearFrame,
    EmitBootstrapVisibleInstance,
  };

  struct Settings
  {
    uint32_t voxelCount = kExteriorShellVoxelCount;
    uint32_t preprocessWorkgroupSize = kPreprocessWorkgroupSize;
    uint32_t visibleInstanceCapacity = 1u;
    uint32_t visibleVoxelCapacity = kExteriorShellVoxelCount;
  };

  SparseVoxelBootstrapPass(const Kernel kernel, const ::rendering::Buffer counters, const ::rendering::Buffer visibleInstances, const ::rendering::Buffer indirectArgs, const Settings settings)
      : kernel_(kernel), counters_(counters), visibleInstances_(visibleInstances), indirectArgs_(indirectArgs), settings_(settings)
  {
  }

  ~SparseVoxelBootstrapPass() override
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

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    if (kernel_ == Kernel::ClearFrame)
    {
      const uint32_t itemCount = std::max(settings_.visibleInstanceCapacity, kSparseVoxelIndirectSlotCount);
      commandBuffer.cmdDispatch(std::max(1u, (itemCount + 63u) / 64u), 1u, 1u);
    }
    else
    {
      commandBuffer.cmdDispatch(1u, 1u, 1u);
    }
  }

private:
  static ::rendering::BufferView storageView(const ::rendering::Buffer buffer, const uint64_t size)
  {
    return ::rendering::BufferView{
      .buffer = buffer,
      .offset = 0u,
      .size = size,
      .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE,
    };
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(SparseVoxelBootstrapUniformsGPU),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (layout_.isValid())
      return;

    layout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {::rendering::BindingGroupLayout{
            .buffers =
                {
                  {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "counters", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "visibleInstances",
                   .binding = 2u,
                   .isDynamic = false,
                   .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                   .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "indirectArgs",
                   .binding = 3u,
                   .isDynamic = false,
                   .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                   .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
          }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
          .layout = layout_,
          .name = passName + "_Groups",
          .groups = {::rendering::GroupInfo{
            .name = "group0",
            .buffers =
                {
                  {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelBootstrapUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
                  {.binding = 1u, .bufferView = storageView(counters_, sizeof(SparseVoxelFrameCountersGPU))},
                  {.binding = 2u, .bufferView = storageView(visibleInstances_, std::max<uint64_t>(1u, settings_.visibleInstanceCapacity) * sizeof(VisibleInstanceGPU))},
                  {.binding = 3u, .bufferView = storageView(indirectArgs_, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU))},
                },
          }},
        });

    shader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-bootstrap.spirv"),
          .type = ::rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
          .entry = kernel_ == Kernel::ClearFrame ? "clearFrame" : "emitBootstrapVisibleInstance",
          .layout = layout_,
          .name = passName + "_Pipeline",
          .shader = shader_,
        });
  }

  void writeUniforms()
  {
    SparseVoxelBootstrapUniformsGPU uniforms{};
    uniforms.voxelCount = settings_.voxelCount;
    uniforms.preprocessWorkgroupSize = settings_.preprocessWorkgroupSize;
    uniforms.visibleInstanceCapacity = settings_.visibleInstanceCapacity;
    uniforms.visibleVoxelCapacity = settings_.visibleVoxelCapacity;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  Kernel kernel_;
  ::rendering::Buffer counters_{};
  ::rendering::Buffer visibleInstances_{};
  ::rendering::Buffer indirectArgs_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
