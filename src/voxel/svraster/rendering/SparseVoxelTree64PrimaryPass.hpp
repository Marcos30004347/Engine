#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// Surface resolution for the Tree64 primary-ray G-buffer resolver. The trace
// pass generates its own rays from the invocation index, so this pass only
// turns trace results into the ResolvedVoxelSurface contract.
class SparseVoxelTree64PrimaryPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer treeState;
    ::rendering::Buffer results;
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    ::rendering::Buffer materialSampleAddresses;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
  };

  SparseVoxelTree64PrimaryPass(const Buffers buffers, const Settings settings)
      : buffers_(buffers), settings_(settings)
  {
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  ~SparseVoxelTree64PrimaryPass() override
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
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    constexpr uint32_t kWorkgroupSide = 8u;
    commandBuffer.cmdDispatch(
        std::max(1u, (settings_.uniforms.viewportTiles[0] + kWorkgroupSide - 1u) / kWorkgroupSide),
        std::max(1u, (settings_.uniforms.viewportTiles[1] + kWorkgroupSide - 1u) / kWorkgroupSide),
        1u);
  }

private:
  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer",
         .size = sizeof(SparseVoxelRasterUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    const auto uniform = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             uniform(0u, "uniforms"),
             storage(1u, "treeState"),
             storage(2u, "results"),
             storage(3u, "resolvedSurfaces"),
             storage(4u, "coverage"),
             storage(5u, "linearDepth"),
             storage(6u, "materialSampleAddresses"),
           }}}});

    const uint32_t pixelCapacity =
        std::max(1u, settings_.uniforms.viewportTiles[0] * settings_.uniforms.viewportTiles[1]);
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    bindingGroups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = view(uniformBuffer_, sizeof(SparseVoxelRasterUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(buffers_.treeState, sizeof(SparseVoxelTree64StateGPU), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u, .bufferView = view(buffers_.results, capacityBytes(pixelCapacity, sizeof(SparseVoxelOccupancyDDAResultGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 3u, .bufferView = view(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 4u, .bufferView = view(buffers_.coverage, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 5u, .bufferView = view(buffers_.linearDepth, capacityBytes(pixelCapacity, sizeof(float)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 6u, .bufferView = view(
                 buffers_.materialSampleAddresses,
                 capacityBytes(pixelCapacity, sizeof(VoxelMaterialSampleAddressGPU)),
                 ::rendering::AccessPattern::SHADER_WRITE)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-tree64-primary.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "resolveTree64PrimarySurfaces", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
