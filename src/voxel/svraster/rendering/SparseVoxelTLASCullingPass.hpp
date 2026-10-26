#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelTLASCullingPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Initialize,
    Traverse,
    FinalizeLevel,
    ScanVisibility,
    ScanBlockSums,
    ScatterVisibility,
  };

  struct Buffers
  {
    ::rendering::Buffer counters;
    ::rendering::Buffer instances;
    ::rendering::Buffer nodes;
    ::rendering::Buffer instanceIndices;
    ::rendering::Buffer blasRoots;
    ::rendering::Buffer queueA;
    ::rendering::Buffer queueB;
    ::rendering::Buffer queueCounts;
    ::rendering::Buffer dispatchArgs;
    ::rendering::Buffer visibilityFlags;
    ::rendering::Buffer visibilityOffsets;
    ::rendering::Buffer scanBlocks;
    ::rendering::Buffer visibleInstances;
    ::rendering::Buffer lodSeeds;
    ::rendering::Buffer preSubmittedFlags;
    ::rendering::Buffer rejectionReasons;
  };

  struct OcclusionResources
  {
    ::rendering::Texture hzb;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU raster{};
    SparseVoxelTLASCullingUniformsGPU culling{};
  };

  SparseVoxelTLASCullingPass(
      const Kernel kernel,
      const Buffers buffers,
      const Settings settings,
      const OcclusionResources occlusion = {})
      : kernel_(kernel), buffers_(buffers), settings_(settings), occlusion_(occlusion)
  {
  }

  ~SparseVoxelTLASCullingPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (rasterUniformBuffer_.isValid()) renderGraph->deleteBuffer(rasterUniformBuffer_);
    if (cullingUniformBuffer_.isValid()) renderGraph->deleteBuffer(cullingUniformBuffer_);
    if (ownsFallbackPreSubmitted_ && fallbackPreSubmitted_.isValid()) renderGraph->deleteBuffer(fallbackPreSubmitted_);
    if (ownsFallbackReasons_ && fallbackReasons_.isValid()) renderGraph->deleteBuffer(fallbackReasons_);
    if (ownsFallbackHZB_ && fallbackHZB_.isValid()) renderGraph->deleteTexture(fallbackHZB_);
  }

  void updateRasterUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.raster = uniforms;
    if (rasterUniformBuffer_.isValid())
      renderGraph->bufferWrite(rasterUniformBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }

  void updateCullingUniforms(const SparseVoxelTLASCullingUniformsGPU &uniforms)
  {
    settings_.culling = uniforms;
    if (cullingUniformBuffer_.isValid())
      renderGraph->bufferWrite(cullingUniformBuffer_, 0u, sizeof(settings_.culling), &settings_.culling);
  }

  void updateHistoryValidity(const bool valid)
  {
    settings_.culling.historyValid = valid ? 1u : 0u;
    if (cullingUniformBuffer_.isValid())
      renderGraph->bufferWrite(cullingUniformBuffer_, 0u, sizeof(settings_.culling), &settings_.culling);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    const uint32_t requiredBlocks = std::max(1u, (settings_.culling.instanceCount + kSparseVoxelTLASWorkgroupSize - 1u) / kSparseVoxelTLASWorkgroupSize);
    if (settings_.culling.scanBlockCount != requiredBlocks || settings_.culling.scanBlockCount > kSparseVoxelTLASMaxScanBlocks)
      throw std::runtime_error("Sparse voxel TLAS stable compaction exceeds its 65,536-instance scan contract");
    ensureResources();
    renderGraph->bufferWrite(rasterUniformBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
    renderGraph->bufferWrite(cullingUniformBuffer_, 0u, sizeof(settings_.culling), &settings_.culling);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    switch (kernel_)
    {
    case Kernel::Initialize:
    {
      const uint32_t items = std::max({settings_.culling.instanceCount, settings_.culling.visibleInstanceCapacity, settings_.culling.maxDepth, settings_.culling.scanBlockCount, 1u});
      commandBuffer.cmdDispatch((items + kSparseVoxelTLASWorkgroupSize - 1u) / kSparseVoxelTLASWorkgroupSize, 1u, 1u);
      break;
    }
    case Kernel::Traverse:
      commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, uint64_t{settings_.culling.depth} * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::FinalizeLevel:
    case Kernel::ScanBlockSums:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      break;
    case Kernel::ScanVisibility:
      commandBuffer.cmdDispatch(settings_.culling.scanBlockCount, 1u, 1u);
      break;
    case Kernel::ScatterVisibility:
      commandBuffer.cmdDispatch((settings_.culling.instanceCount + kSparseVoxelTLASWorkgroupSize - 1u) / kSparseVoxelTLASWorkgroupSize, 1u, 1u);
      break;
    }
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = std::max<uint64_t>(size, sizeof(uint32_t)), .access = access};
  }

  const char *entry() const
  {
    switch (kernel_)
    {
    case Kernel::Initialize: return "initializeTLASCulling";
    case Kernel::Traverse: return "traverseTLAS";
    case Kernel::FinalizeLevel: return "finalizeTLASLevel";
    case Kernel::ScanVisibility: return "scanTLASVisibility";
    case Kernel::ScanBlockSums: return "scanTLASBlockSums";
    case Kernel::ScatterVisibility: return "scatterTLASVisibility";
    }
    return "";
  }

  // Bindings each entry point statically uses. Vulkan only validates against
  // what a pipeline's entry point actually references, so a kernel that touches
  // nine buffers has no reason to carry a nineteen-entry descriptor set - every
  // extra entry is a descriptor write plus a residency reference the driver has
  // to process for a dispatch that never reads it.
  bool usesBinding(const uint32_t binding) const
  {
    switch (kernel_)
    {
    // counters(2) is reached indirectly through recordOverflow, so any kernel
    // that can report an overflow needs it even where no direct reference is
    // visible at the call site.
    case Kernel::Initialize:
      return binding == 1u || binding == 2u || binding == 7u ||
             binding == 9u || binding == 10u || binding == 11u ||
             binding == 12u || binding == 13u || binding == 18u;
    case Kernel::FinalizeLevel:
      return binding == 1u || binding == 9u || binding == 10u;
    case Kernel::ScanVisibility:
      return binding == 1u || binding == 3u || binding == 6u ||
             binding == 11u || binding == 12u || binding == 13u;
    case Kernel::ScanBlockSums:
      return binding == 1u || binding == 2u || binding == 13u;
    case Kernel::ScatterVisibility:
      return binding == 1u || binding == 2u || binding == 3u ||
             binding == 6u || binding == 11u || binding == 12u ||
             binding == 13u || binding == 14u || binding == 15u;
    case Kernel::Traverse:
      break;
    }
    return true;
  }

  bool usesHZB() const { return kernel_ == Kernel::Traverse; }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    rasterUniformBuffer_ = renderGraph->createBuffer({.name = passName + "_Raster", .size = sizeof(settings_.raster), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    cullingUniformBuffer_ = renderGraph->createBuffer({.name = passName + "_Culling", .size = sizeof(settings_.culling), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    std::vector<::rendering::BindingGroupLayoutBufferEntry> entries;
    if (usesBinding(0u))
      entries.push_back({.name = "raster", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    if (usesBinding(1u))
      entries.push_back({.name = "culling", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    for (uint32_t binding = 2u; binding <= 15u; ++binding)
      if (usesBinding(binding))
        entries.push_back({.name = "storage" + std::to_string(binding), .binding = binding, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    if (usesBinding(17u))
      entries.push_back({.name = "preSubmittedFlags", .binding = 17u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    if (usesBinding(18u))
      entries.push_back({.name = "rejectionReasons", .binding = 18u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    std::vector<::rendering::BindingGroupLayoutTextureEntry> textureEntries;
    if (usesHZB())
      textureEntries.push_back({.name = "hzb", .binding = 16u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {::rendering::BindingGroupLayout{
      .buffers = entries,
      .textures = textureEntries,
    }}});
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = read | ::rendering::AccessPattern::SHADER_WRITE;
    const uint64_t instances = settings_.culling.instanceCount;
    const uint64_t nodes = settings_.culling.nodeCount;
    const uint64_t queue = settings_.culling.queueCapacity;
    const uint64_t depth = settings_.culling.maxDepth;
    const uint64_t visible = settings_.culling.visibleInstanceCapacity;
    const uint64_t blocks = settings_.culling.scanBlockCount;
    const uint64_t blases = settings_.culling.blasCount;
    if (!buffers_.preSubmittedFlags.isValid())
    {
      fallbackPreSubmitted_ = renderGraph->createBuffer({.name = passName + "_FallbackPreSubmitted", .size = std::max<uint64_t>(sizeof(uint32_t), instances * sizeof(uint32_t)), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      buffers_.preSubmittedFlags = fallbackPreSubmitted_;
      ownsFallbackPreSubmitted_ = true;
      const std::vector<uint32_t> zero(std::max<uint64_t>(1u, instances), 0u);
      renderGraph->bufferWrite(fallbackPreSubmitted_, 0u, zero.size() * sizeof(uint32_t), const_cast<uint32_t *>(zero.data()));
    }
    if (!buffers_.rejectionReasons.isValid())
    {
      fallbackReasons_ = renderGraph->createBuffer({.name = passName + "_FallbackReasons", .size = std::max<uint64_t>(sizeof(uint32_t), instances * sizeof(uint32_t)), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      buffers_.rejectionReasons = fallbackReasons_;
      ownsFallbackReasons_ = true;
    }
    if (!occlusion_.hzb.isValid())
    {
      fallbackHZB_ = renderGraph->createTexture({.name = passName + "_FallbackHZB", .format = ::rendering::Format::Format_R32Float, .width = 1u, .height = 1u, .mipLevels = 1u, .usage = ::rendering::ImageUsage::ImageUsage_Storage | ::rendering::ImageUsage::ImageUsage_Sampled});
      occlusion_.hzb = fallbackHZB_;
      ownsFallbackHZB_ = true;
    }
    const std::vector<std::pair<uint32_t, ::rendering::BufferView>> allBuffers{
      {0u, view(rasterUniformBuffer_, sizeof(settings_.raster), ::rendering::AccessPattern::UNIFORM_READ)},
      {1u, view(cullingUniformBuffer_, sizeof(settings_.culling), ::rendering::AccessPattern::UNIFORM_READ)},
      {2u, view(buffers_.counters, sizeof(SparseVoxelFrameCountersGPU), rw)},
      {3u, view(buffers_.instances, instances * sizeof(SparseVoxelInstanceGPU), read)},
      {4u, view(buffers_.nodes, nodes * sizeof(SparseVoxelTLASNodeGPU), read)},
      {5u, view(buffers_.instanceIndices, instances * sizeof(uint32_t), read)},
      {6u, view(buffers_.blasRoots, blases * sizeof(SparseVoxelBLASRootRangeGPU), read)},
      {7u, view(buffers_.queueA, queue * sizeof(TLASQueueElementGPU), rw)},
      {8u, view(buffers_.queueB, queue * sizeof(TLASQueueElementGPU), rw)},
      {9u, view(buffers_.queueCounts, depth * sizeof(uint32_t), rw)},
      {10u, view(buffers_.dispatchArgs, depth * sizeof(IndirectDispatchArgsGPU), rw)},
      {11u, view(buffers_.visibilityFlags, instances * sizeof(uint32_t), rw)},
      {12u, view(buffers_.visibilityOffsets, instances * sizeof(SparseVoxelVisibilityOffsetGPU), rw)},
      {13u, view(buffers_.scanBlocks, blocks * sizeof(SparseVoxelTLASScanBlockGPU), rw)},
      {14u, view(buffers_.visibleInstances, visible * sizeof(VisibleInstanceGPU), rw)},
      {15u, view(buffers_.lodSeeds, visible * sizeof(SparseVoxelLODSeedGPU), rw)},
      {17u, view(buffers_.preSubmittedFlags, instances * sizeof(uint32_t), read)},
      {18u, view(buffers_.rejectionReasons, instances * sizeof(uint32_t), rw)},
    };
    std::vector<::rendering::BindingBuffer> bufferBindings;
    for (const auto &[binding, bufferView] : allBuffers)
      if (usesBinding(binding))
        bufferBindings.push_back({.binding = binding, .bufferView = bufferView});
    std::vector<::rendering::BindingTextureInfo> textureBindings;
    if (usesHZB())
      textureBindings.push_back({.binding = 16u, .textureView = {
        .texture = occlusion_.hzb,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = std::max(1u, settings_.culling.hzbMipCount),
      }});
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_, .name = passName + "_Groups", .groups = {::rendering::GroupInfo{
        .name = "group0", .buffers = bufferBindings, .textures = textureBindings}}});
    shader_ = renderGraph->createShader({.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-tlas-culling.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = entry(), .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_{};
  Buffers buffers_{};
  Settings settings_{};
  OcclusionResources occlusion_{};
  ::rendering::Buffer rasterUniformBuffer_{};
  ::rendering::Buffer cullingUniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::Buffer fallbackPreSubmitted_{};
  ::rendering::Buffer fallbackReasons_{};
  ::rendering::Texture fallbackHZB_{};
  bool ownsFallbackPreSubmitted_ = false;
  bool ownsFallbackReasons_ = false;
  bool ownsFallbackHZB_ = false;
};

} // namespace voxel::svraster::rendering
