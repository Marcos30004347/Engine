#pragma once

#include "os/File.hpp"
#include "rendering/gpgpu/RadixSort2Pass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelBinningPass : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    PreprocessVisibleVoxels,
    ScanDuplicateBlocks,
    ScanDuplicateBlockSums,
    AddDuplicateBlockOffsets,
    EmitTileCandidates,
    ClearTileRanges,
    BuildTileRanges,
    ClearTileBuckets,
    CountTileCandidates,
    PreprocessAndCountVisibleVoxels,
    ScanTileBucketBlocks,
    ScanTileBucketBlockSums,
    AddTileBucketBlockOffsets,
    ScatterTileCandidates,
  };

  struct Buffers
  {
    ::rendering::Buffer counters;
    ::rendering::Buffer visibleInstances;
    ::rendering::Buffer instances;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer pathsLevels;
    ::rendering::Buffer projectedVoxels;
    ::rendering::Buffer duplicateOffsets;
    ::rendering::Buffer scanBlockSums;
    ::rendering::Buffer scanBlockOffsets;
    ::rendering::Buffer candidates;
    ::rendering::Buffer tileRanges;
    ::rendering::Buffer indirectArgs;
    ::rendering::Buffer candidateCount;
    ::rendering::Buffer visibleVoxels;
    ::rendering::Buffer tileCandidateCounts;
    ::rendering::Buffer tileCandidateCursors;
    ::rendering::Buffer tileCandidateIndices;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
    uint32_t visibleInstanceCapacity = 1u;
    uint32_t instanceCapacity = 1u;
    // Zero preserves the original contract where hierarchy and visible
    // scratch capacities are identical.
    uint32_t hierarchyCapacity = 0u;
  };

  SparseVoxelBinningPass(const Kernel kernel, const Buffers buffers, const Settings settings) : kernel_(kernel), buffers_(buffers), settings_(settings)
  {
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  ~SparseVoxelBinningPass() override
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

    switch (kernel_)
    {
    case Kernel::PreprocessVisibleVoxels:
    case Kernel::ScanDuplicateBlocks:
      commandBuffer.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(SparseVoxelIndirectSlot::Preprocess) * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::ScanDuplicateBlockSums:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      break;
    case Kernel::AddDuplicateBlockOffsets:
      commandBuffer.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(SparseVoxelIndirectSlot::Preprocess) * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::EmitTileCandidates:
      commandBuffer.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(SparseVoxelIndirectSlot::CandidateEmission) * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::ClearTileRanges:
      commandBuffer.cmdDispatch(std::max(1u, (settings_.uniforms.tileRangeCapacity + 255u) / 256u), 1u, 1u);
      break;
    case Kernel::BuildTileRanges:
      commandBuffer.cmdDispatchIndirect(buffers_.indirectArgs, static_cast<uint64_t>(SparseVoxelIndirectSlot::TileRanges) * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::ClearTileBuckets:
    case Kernel::ScanTileBucketBlocks:
    case Kernel::AddTileBucketBlockOffsets:
      commandBuffer.cmdDispatch(tileBucketBlockCount(), 1u, 1u);
      break;
    case Kernel::CountTileCandidates:
    case Kernel::PreprocessAndCountVisibleVoxels:
    case Kernel::ScatterTileCandidates:
      commandBuffer.cmdDispatchIndirect(
          buffers_.indirectArgs,
          static_cast<uint64_t>(SparseVoxelIndirectSlot::Preprocess) *
              sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::ScanTileBucketBlockSums:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      break;
    }
  }

private:
  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  uint32_t tileBucketBlockCount() const
  {
    return std::max(1u, (settings_.uniforms.tileRangeCapacity + 255u) / 256u);
  }

  static ::rendering::BufferView storageView(const ::rendering::Buffer buffer, const uint64_t size)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE};
  }

  const char *entryPoint() const
  {
    switch (kernel_)
    {
    case Kernel::PreprocessVisibleVoxels:
      return "preprocessVisibleVoxels";
    case Kernel::ScanDuplicateBlocks:
      return "scanDuplicateBlocks";
    case Kernel::ScanDuplicateBlockSums:
      return "scanDuplicateBlockSums";
    case Kernel::AddDuplicateBlockOffsets:
      return "addDuplicateBlockOffsets";
    case Kernel::EmitTileCandidates:
      return "emitTileCandidates";
    case Kernel::ClearTileRanges:
      return "clearTileRanges";
    case Kernel::BuildTileRanges:
      return "buildTileRanges";
    case Kernel::ClearTileBuckets:
      return "clearTileBuckets";
    case Kernel::CountTileCandidates:
      return "countTileCandidates";
    case Kernel::PreprocessAndCountVisibleVoxels:
      return "preprocessAndCountVisibleVoxels";
    case Kernel::ScanTileBucketBlocks:
      return "scanTileBucketBlocks";
    case Kernel::ScanTileBucketBlockSums:
      return "scanTileBucketBlockSums";
    case Kernel::AddTileBucketBlockOffsets:
      return "addTileBucketBlockOffsets";
    case Kernel::ScatterTileCandidates:
      return "scatterTileCandidates";
    }
    return "";
  }

  bool usesTileBuckets() const
  {
    return kernel_ == Kernel::ClearTileBuckets ||
        kernel_ == Kernel::CountTileCandidates ||
        kernel_ == Kernel::PreprocessAndCountVisibleVoxels ||
        kernel_ == Kernel::ScanTileBucketBlocks ||
        kernel_ == Kernel::ScanTileBucketBlockSums ||
        kernel_ == Kernel::AddTileBucketBlockOffsets ||
        kernel_ == Kernel::ScatterTileCandidates;
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    }
    if (layout_.isValid())
      return;

    std::vector<::rendering::BindingGroupLayoutBufferEntry> bindings;
    bindings.push_back(
        {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    const uint32_t lastBinding = usesTileBuckets() ? 17u : 14u;
    for (uint32_t binding = 1u; binding <= lastBinding; ++binding)
    {
      bindings.push_back(
          {.name = "storage" + std::to_string(binding),
           .binding = binding,
           .isDynamic = false,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    }
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {::rendering::BindingGroupLayout{.buffers = bindings}}});

    const uint32_t visibleCapacity = settings_.uniforms.visibleVoxelCapacity;
    const uint32_t projectedCapacity = settings_.uniforms.projectedVoxelCapacity;
    const uint32_t candidateCapacity = settings_.uniforms.candidateCapacity;
    const uint32_t rangeCapacity = settings_.uniforms.tileRangeCapacity;
    // The scan block scratch is shared by the duplicate-offset scan, which has
    // one entry per 256 visible voxels, and the tile bucket scan, which has one
    // per 256 screen tiles. Neither dominates the other for every viewport and
    // scene, so the view must cover both.
    const uint32_t scanBlockCapacity =
        std::max(std::max(1u, (visibleCapacity + 255u) / 256u), tileBucketBlockCount());
    std::vector<::rendering::BindingBuffer> groupBuffers{
      {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelRasterUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
      {.binding = 1u, .bufferView = storageView(buffers_.counters, sizeof(SparseVoxelFrameCountersGPU))},
      {.binding = 2u, .bufferView = storageView(buffers_.visibleInstances, capacityBytes(settings_.visibleInstanceCapacity, sizeof(VisibleInstanceGPU)))},
      {.binding = 3u, .bufferView = storageView(buffers_.instances, capacityBytes(settings_.instanceCapacity, sizeof(SparseVoxelInstanceGPU)))},
      {.binding = 4u, .bufferView = storageView(buffers_.centersLengths, capacityBytes(settings_.hierarchyCapacity == 0u ? visibleCapacity : settings_.hierarchyCapacity, sizeof(SparseVoxelCenterLengthGPU)))},
      {.binding = 5u, .bufferView = storageView(buffers_.pathsLevels, capacityBytes(settings_.hierarchyCapacity == 0u ? visibleCapacity : settings_.hierarchyCapacity, sizeof(SparseVoxelPathLevelGPU)))},
      {.binding = 6u, .bufferView = storageView(buffers_.projectedVoxels, capacityBytes(projectedCapacity, sizeof(ProjectedVoxelGPU)))},
      {.binding = 7u, .bufferView = storageView(buffers_.duplicateOffsets, capacityBytes(visibleCapacity, sizeof(uint32_t)))},
      {.binding = 8u, .bufferView = storageView(buffers_.scanBlockSums, capacityBytes(scanBlockCapacity, sizeof(uint32_t)))},
      {.binding = 9u, .bufferView = storageView(buffers_.scanBlockOffsets, capacityBytes(scanBlockCapacity, sizeof(uint32_t)))},
      {.binding = 10u, .bufferView = storageView(buffers_.candidates, capacityBytes(candidateCapacity, sizeof(TileCandidateGPU)))},
      {.binding = 11u, .bufferView = storageView(buffers_.tileRanges, capacityBytes(rangeCapacity, sizeof(SparseVoxelTileRangeGPU)))},
      {.binding = 12u, .bufferView = storageView(buffers_.indirectArgs, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU))},
      {.binding = 13u, .bufferView = storageView(buffers_.candidateCount, sizeof(uint32_t))},
      {.binding = 14u, .bufferView = storageView(buffers_.visibleVoxels, capacityBytes(visibleCapacity, sizeof(VisibleVoxelGPU)))},
    };
    if (usesTileBuckets())
    {
      groupBuffers.push_back(
          {.binding = 15u,
           .bufferView = storageView(
               buffers_.tileCandidateCounts,
               capacityBytes(rangeCapacity, sizeof(uint32_t)))});
      groupBuffers.push_back(
          {.binding = 16u,
           .bufferView = storageView(
               buffers_.tileCandidateCursors,
               capacityBytes(rangeCapacity, sizeof(uint32_t)))});
      groupBuffers.push_back(
          {.binding = 17u,
           .bufferView = storageView(
               buffers_.tileCandidateIndices,
               capacityBytes(candidateCapacity, sizeof(uint32_t)))});
    }
    bindingGroups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = groupBuffers}}});

    shader_ =
        renderGraph->createShader({.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-binning.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = entryPoint(), .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_;
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

class SparseVoxelRadixDispatchPass : public ::rendering::Pass
{
public:
  SparseVoxelRadixDispatchPass(
      const ::rendering::ComputePipeline pipeline,
      const ::rendering::BindingGroups groups,
      const uint32_t uniformOffset,
      const ::rendering::Buffer indirectArgs,
      const uint64_t indirectOffset,
      const bool direct)
      : pipeline_(pipeline), groups_(groups), uniformOffset_(uniformOffset), indirectArgs_(indirectArgs), indirectOffset_(indirectOffset), direct_(direct)
  {
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, &uniformOffset_, 1u);
    if (direct_)
      commandBuffer.cmdDispatch(1u, 1u, 1u);
    else
      commandBuffer.cmdDispatchIndirect(indirectArgs_, indirectOffset_);
  }

private:
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::BindingGroups groups_{};
  uint32_t uniformOffset_ = 0u;
  ::rendering::Buffer indirectArgs_{};
  uint64_t indirectOffset_ = 0u;
  bool direct_ = false;
};

class SparseVoxelRadixSortPass : public ::rendering::Pass
{
public:
  static constexpr uint32_t kChildPassCount = 6u;

  enum class KeyOrdering : uint32_t
  {
    // Order by the complete 64-bit key: a low-word sort followed by a stable
    // high-word sort.
    Full,
    // Order by the high word alone, skipping the low-word sort entirely. Only
    // valid when the consumer needs the grouping the high word encodes and is
    // independent of the relative order of entries sharing a high word.
    HighWordOnly,
  };

  SparseVoxelRadixSortPass(
      const uint32_t candidateCapacity,
      const ::rendering::Buffer candidates,
      const ::rendering::Buffer scratchCandidates,
      const ::rendering::Buffer candidateCount,
      const uint32_t sortingBits = 64u,
      const KeyOrdering ordering = KeyOrdering::Full)
      : candidateCapacity_(candidateCapacity), candidates_(candidates), scratchCandidates_(scratchCandidates), candidateCount_(candidateCount), sortingBits_(sortingBits <= 32u ? 32u : 64u), ordering_(ordering)
  {
  }

  ~SparseVoxelRadixSortPass() override
  {
    destroyChildPasses();
    for (::rendering::ComputePipeline pipeline : helperPipelines_)
    {
      if (pipeline.isValid())
        renderGraph->deleteComputePipeline(pipeline);
    }
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (helperGroups_.isValid())
      renderGraph->deleteBindingGroups(helperGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (keys_.isValid())
      renderGraph->deleteBuffer(keys_);
    if (indices_.isValid())
      renderGraph->deleteBuffer(indices_);
    if (dispatchArgs_.isValid())
      renderGraph->deleteBuffer(dispatchArgs_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    (void)commandBuffer;
    destroyChildPasses();
    ensureResources();

    uint32_t childIndex = getIndex() + 1u;
    registerHelper("prepare", childIndex++, helperPipelines_[Prepare], DispatchSlot::Items, true);
    if (ordering_ == KeyOrdering::HighWordOnly)
    {
      // One sort instead of two. Each 32-bit sort expands into dozens of
      // dispatches, so dropping the low-word sort removes about half of this
      // pass group's GPU cost.
      registerHelper("extractHigh", childIndex++, helperPipelines_[ExtractHighSeeded], DispatchSlot::Items, false);
      registerSort("sortHigh", childIndex++, true);
    }
    else
    {
      registerHelper("extractLow", childIndex++, helperPipelines_[ExtractLow], DispatchSlot::Items, false);
      registerSort("sortLow", childIndex++);
      if (sortingBits_ == 64u)
      {
        registerHelper("extractHigh", childIndex++, helperPipelines_[ExtractHigh], DispatchSlot::Items, false);
        registerSort("sortHigh", childIndex++);
      }
    }
    registerHelper("gather", childIndex++, helperPipelines_[Gather], DispatchSlot::Items, false);
    assert(childIndex <= getIndex() + 1u + kChildPassCount);
  }

private:
  enum PipelineIndex : uint32_t
  {
    Prepare = 0u,
    ExtractLow = 1u,
    ExtractHigh = 2u,
    Gather = 3u,
    ExtractHighSeeded = 4u,
  };

  enum class DispatchSlot : uint32_t
  {
    Items = 6u,
  };

  void registerHelper(const std::string &suffix, const uint32_t index, const ::rendering::ComputePipeline pipeline, const DispatchSlot slot, const bool direct)
  {
    childPasses_.push_back(
        renderGraph->registerPass<SparseVoxelRadixDispatchPass>(passName + "." + suffix, index, pipeline, helperGroups_, 0u, dispatchArgs_, static_cast<uint64_t>(slot) * sizeof(SparseVoxelRadixDispatchArgsGPU), direct));
  }

  void registerSort(const std::string &suffix, const uint32_t index, const bool tileGroupingOnly = false)
  {
    auto *sort = renderGraph->registerPass<::rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t>>(passName + "." + suffix, index, candidateCapacity_, keys_, indices_, candidateCount_, dispatchArgs_, false);
    if (tileGroupingOnly)
    {
      // The high word is (tileId << 16) | rank, and only the tile grouping is
      // consumed. Sorting the low 16 bits would order by a rank nothing reads,
      // so restrict the sort to the tile digits and halve the digit rounds.
      sort->setSortingBitRange(16u, 16u);
    }
    childPasses_.push_back(sort);
  }

  void destroyChildPasses()
  {
    for (::rendering::Pass *pass : childPasses_)
      delete pass;
    childPasses_.clear();
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRadixUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    SparseVoxelRadixUniformsGPU uniforms{0u, candidateCapacity_, 0u, 0u};
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);

    const uint64_t itemCapacity = std::max<uint64_t>(1u, candidateCapacity_);
    const auto storageBuffer = [&](const std::string &name, const uint64_t size, const ::rendering::BufferUsage extraUsage = ::rendering::BufferUsage::BufferUsage_None)
    {
      return renderGraph->createBuffer({.name = passName + name, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extraUsage, .frameLocal = true});
    };
    keys_ = storageBuffer("_Keys.buffer", itemCapacity * sizeof(uint32_t));
    indices_ = storageBuffer("_Indices.buffer", itemCapacity * sizeof(uint32_t));
    dispatchArgs_ = storageBuffer("_DispatchArgs.buffer", 7u * sizeof(SparseVoxelRadixDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect);

    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = true, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "candidateCount",
              .binding = 1u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "keys", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "indices", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "unsortedCandidates",
              .binding = 4u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "sortedCandidates",
              .binding = 5u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "dispatchArgs", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});
    const uint64_t candidateBytes = std::max<uint64_t>(1u, candidateCapacity_) * sizeof(TileCandidateGPU);
    const uint64_t keyBytes = itemCapacity * sizeof(uint32_t);
    const uint64_t indexBytes = itemCapacity * sizeof(uint32_t);
    helperGroups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(SparseVoxelRadixUniformsGPU), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 1u, .bufferView = {.buffer = candidateCount_, .offset = 0u, .size = sizeof(uint32_t), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 2u, .bufferView = {.buffer = keys_, .offset = 0u, .size = keyBytes, .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 3u, .bufferView = {.buffer = indices_, .offset = 0u, .size = indexBytes, .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 4u, .bufferView = {.buffer = candidates_, .offset = 0u, .size = candidateBytes, .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 5u, .bufferView = {.buffer = scratchCandidates_, .offset = 0u, .size = candidateBytes, .access = ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 6u,
              .bufferView = {.buffer = dispatchArgs_, .offset = 0u, .size = 7u * sizeof(SparseVoxelRadixDispatchArgsGPU), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE}},
           }}}});
    shader_ =
        renderGraph->createShader({.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-radix.spirv"), .type = ::rendering::ShaderType::SpirV});
    const std::array<const char *, 5> entries = {"prepareRadixDispatch", "extractLowKeys", "extractHighKeys", "gatherSortedCandidates", "extractHighKeysSeeded"};
    for (uint32_t index = 0u; index < entries.size(); ++index)
      helperPipelines_[index] = renderGraph->createComputePipeline({.entry = entries[index], .layout = layout_, .name = passName + "_" + entries[index] + ".pipeline", .shader = shader_});
  }

  uint32_t candidateCapacity_ = 0u;
  ::rendering::Buffer candidates_{};
  ::rendering::Buffer scratchCandidates_{};
  ::rendering::Buffer candidateCount_{};
  uint32_t sortingBits_ = 64u;
  KeyOrdering ordering_ = KeyOrdering::Full;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer keys_{};
  ::rendering::Buffer indices_{};
  ::rendering::Buffer dispatchArgs_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups helperGroups_{};
  ::rendering::Shader shader_{};
  std::array<::rendering::ComputePipeline, 5> helperPipelines_{};
  std::vector<::rendering::Pass *> childPasses_{};
};

} // namespace voxel::svraster::rendering
