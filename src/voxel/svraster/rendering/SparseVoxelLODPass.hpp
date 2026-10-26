#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelLODPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Initialize,
    Traverse,
    FinalizeLevel,
    FinalizeSelection,
    PreserveTemporalSelection,
  };

  struct Buffers
  {
    ::rendering::Buffer counters;
    ::rendering::Buffer visibleInstances;
    ::rendering::Buffer visibleVoxels;
    ::rendering::Buffer instances;
    ::rendering::Buffer nodes;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer queueA;
    ::rendering::Buffer queueB;
    ::rendering::Buffer queueCounts;
    ::rendering::Buffer lodDispatchArgs;
    ::rendering::Buffer rasterIndirectArgs;
    ::rendering::Buffer residentMetadata;
    ::rendering::Buffer pageRequestBits;
    ::rendering::Buffer pageRequestPriorities;
    ::rendering::Buffer pageRequestInstances;
    ::rendering::Buffer lodSeeds;
    ::rendering::Buffer pageBounds;
    ::rendering::Buffer rejectedNodes;
    ::rendering::Buffer recoveryState;
    ::rendering::Buffer temporalVisibleVoxels;
    ::rendering::Buffer preSubmittedFlags;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU raster{};
    SparseVoxelLODPassUniformsGPU lod{};
  };

  struct OcclusionResources
  {
    ::rendering::Texture hzb;
  };

  SparseVoxelLODPass(
      const Kernel kernel,
      const Buffers buffers,
      const Settings settings,
      const OcclusionResources occlusion = {})
      : kernel_(kernel), buffers_(buffers), settings_(settings), occlusion_(occlusion)
  {
  }

  ~SparseVoxelLODPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (rasterUniformBuffer_.isValid())
      renderGraph->deleteBuffer(rasterUniformBuffer_);
    if (lodUniformBuffer_.isValid())
      renderGraph->deleteBuffer(lodUniformBuffer_);
    if (ownsFallbackHZB_ && fallbackHZB_.isValid())
      renderGraph->deleteTexture(fallbackHZB_);
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.raster = uniforms;
    if (rasterUniformBuffer_.isValid())
      renderGraph->bufferWrite(rasterUniformBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void updateHistoryValidity(const bool valid)
  {
    settings_.lod.historyValid = valid ? 1u : 0u;
    if (lodUniformBuffer_.isValid())
      renderGraph->bufferWrite(lodUniformBuffer_, 0u, sizeof(settings_.lod), &settings_.lod);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(rasterUniformBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
    renderGraph->bufferWrite(lodUniformBuffer_, 0u, sizeof(settings_.lod), &settings_.lod);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::Traverse)
      commandBuffer.cmdDispatchIndirect(buffers_.lodDispatchArgs, uint64_t{settings_.lod.depth} * sizeof(IndirectDispatchArgsGPU));
    else if (kernel_ == Kernel::PreserveTemporalSelection)
      commandBuffer.cmdDispatchIndirect(buffers_.rasterIndirectArgs, 0u);
    else
      commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  const char *entry() const
  {
    switch (kernel_)
    {
    case Kernel::Initialize:
      return "initializeLOD";
    case Kernel::Traverse:
      return "traverseLOD";
    case Kernel::FinalizeLevel:
      return "finalizeLODLevel";
    case Kernel::FinalizeSelection:
      return "finalizeLODSelection";
    case Kernel::PreserveTemporalSelection:
      return "preserveTemporalSelection";
    }
    return "";
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    rasterUniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_RasterUniforms", .size = sizeof(settings_.raster), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    lodUniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_LODUniforms", .size = sizeof(settings_.lod), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    std::vector<::rendering::BindingGroupLayoutBufferEntry> entries = {
      {.name = "raster", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name = "lod", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
    };
    for (uint32_t binding = 2u; binding <= 18u; ++binding)
      entries.push_back({.name = "storage" + std::to_string(binding), .binding = binding, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    for (uint32_t binding = 20u; binding <= 23u; ++binding)
      entries.push_back({.name = "storage" + std::to_string(binding), .binding = binding, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {::rendering::BindingGroupLayout{
      .buffers = entries,
      .textures = {{.name = "hzb", .binding = 19u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
    }}});
    const uint64_t nodeCapacity = settings_.lod.nodeCapacity;
    const uint64_t levelCapacity = settings_.lod.levelCapacity == 0u ? 8u : settings_.lod.levelCapacity;
    const auto rw = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    if (!occlusion_.hzb.isValid())
    {
      fallbackHZB_ = renderGraph->createTexture({
        .name = passName + "_FallbackHZB",
        .format = ::rendering::Format::Format_R32Float,
        .width = 1u,
        .height = 1u,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage | ::rendering::ImageUsage::ImageUsage_Sampled,
      });
      occlusion_.hzb = fallbackHZB_;
      ownsFallbackHZB_ = true;
    }
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = view(rasterUniformBuffer_, sizeof(settings_.raster), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(lodUniformBuffer_, sizeof(settings_.lod), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 2u, .bufferView = view(buffers_.counters, sizeof(SparseVoxelFrameCountersGPU), rw)},
             {.binding = 3u, .bufferView = view(buffers_.visibleInstances, std::max<uint64_t>(sizeof(VisibleInstanceGPU), uint64_t{settings_.lod.seedCapacity} * sizeof(VisibleInstanceGPU)), rw)},
             {.binding = 4u, .bufferView = view(buffers_.visibleVoxels, std::max<uint64_t>(sizeof(VisibleVoxelGPU), uint64_t{settings_.raster.visibleVoxelCapacity} * sizeof(VisibleVoxelGPU)), rw)},
             {.binding = 5u, .bufferView = view(buffers_.instances, std::max<uint64_t>(sizeof(SparseVoxelInstanceGPU), uint64_t{settings_.lod.seedCapacity} * sizeof(SparseVoxelInstanceGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 6u, .bufferView = view(buffers_.nodes, nodeCapacity * sizeof(SparseVoxelLODNodeGPU), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 7u, .bufferView = view(buffers_.centersLengths, nodeCapacity * sizeof(SparseVoxelCenterLengthGPU), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 8u, .bufferView = view(buffers_.queueA, nodeCapacity * sizeof(SparseVoxelLODQueueElementGPU), rw)},
             {.binding = 9u, .bufferView = view(buffers_.queueB, nodeCapacity * sizeof(SparseVoxelLODQueueElementGPU), rw)},
             {.binding = 10u, .bufferView = view(buffers_.queueCounts, levelCapacity * sizeof(uint32_t), rw)},
             {.binding = 11u, .bufferView = view(buffers_.lodDispatchArgs, levelCapacity * sizeof(IndirectDispatchArgsGPU), rw)},
             {.binding = 12u, .bufferView = view(buffers_.rasterIndirectArgs, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), rw)},
             {.binding = 13u, .bufferView = view(buffers_.residentMetadata, nodeCapacity * sizeof(SparseVoxelResidentNodeMetadataGPU), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 14u, .bufferView = view(buffers_.pageRequestBits, std::max<uint64_t>(sizeof(uint32_t), uint64_t{settings_.lod.requestWordCount} * sizeof(uint32_t)), rw)},
             {.binding = 15u, .bufferView = view(buffers_.pageRequestPriorities, std::max<uint64_t>(sizeof(uint32_t), uint64_t{settings_.lod.pageCount} * sizeof(uint32_t)), rw)},
             {.binding = 16u, .bufferView = view(buffers_.pageRequestInstances, std::max<uint64_t>(sizeof(uint32_t), uint64_t{settings_.lod.pageCount} * sizeof(uint32_t)), rw)},
             {.binding = 17u, .bufferView = view(buffers_.lodSeeds, std::max<uint64_t>(sizeof(SparseVoxelLODSeedGPU), uint64_t{settings_.lod.seedCapacity} * sizeof(SparseVoxelLODSeedGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 18u, .bufferView = view(buffers_.pageBounds, std::max<uint64_t>(sizeof(SparseVoxelPageBoundsGPU), uint64_t{settings_.lod.pageCount} * sizeof(SparseVoxelPageBoundsGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 20u, .bufferView = view(buffers_.rejectedNodes, std::max<uint64_t>(sizeof(SparseVoxelLODQueueElementGPU), uint64_t{settings_.lod.rejectionCapacity} * sizeof(SparseVoxelLODQueueElementGPU)), rw)},
             {.binding = 21u, .bufferView = view(buffers_.recoveryState, sizeof(SparseVoxelLODRecoveryStateGPU), rw)},
             {.binding = 22u, .bufferView = view(buffers_.temporalVisibleVoxels, std::max<uint64_t>(sizeof(VisibleVoxelGPU), uint64_t{settings_.lod.rejectionCapacity} * sizeof(VisibleVoxelGPU)), rw)},
             {.binding = 23u, .bufferView = view(buffers_.preSubmittedFlags, std::max<uint64_t>(sizeof(uint32_t), uint64_t{settings_.lod.seedCapacity} * sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_READ)},
           },
           .textures = {{.binding = 19u, .textureView = {
             .texture = occlusion_.hzb,
             .access = ::rendering::AccessPattern::SHADER_READ,
             .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
             .index = 0u,
             .flags = ::rendering::ImageAspectFlags::Color,
             .baseArrayLayer = 0u,
             .baseMipLevel = 0u,
             .layerCount = 1u,
             .levelCount = std::max(1u, settings_.lod.hzbMipCount),
           }}},
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-lod.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = entry(), .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_{};
  Buffers buffers_{};
  Settings settings_{};
  OcclusionResources occlusion_{};
  ::rendering::Buffer rasterUniformBuffer_{};
  ::rendering::Buffer lodUniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::Texture fallbackHZB_{};
  bool ownsFallbackHZB_ = false;
};

} // namespace voxel::svraster::rendering
