#pragma once

#include <array>
#include <limits>
#include <string>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{
namespace detail
{

struct GPUSignedDistanceFieldsBuildPathSettings
{
  bool performContextBegin = false;
  bool performSkinning = false;
};

enum class GPUSignedDistanceFieldsBuildKernel : uint32_t
{
  ClearCounters = 0u,
  CollectClearBricks,
  PrepareClearBricks,
  ClearBrickEntry,
  ClearBuildCounters,
  ResetCascade,
  InitializeCascade,
  MarkCascadeUninitialized,
  FreeCascade,
  ScrollCascade,
  ClearRefCounters,
  ClearJobCounter,
  InvalidateJobAreas,
  CoarseCulling,
  ScanJobs,
  FinalizeJobScan,
  Voxelize,
  ScanReferences,
  FinalizeReferenceScan,
  CompactReferences,
  ClearBrickStorage,
  EmitSdf,
  CompressBricks,
};

inline const char *buildKernelEntryName(GPUSignedDistanceFieldsBuildKernel kernel)
{
  switch (kernel)
  {
  case GPUSignedDistanceFieldsBuildKernel::ClearCounters:
    return "ClearCountersCS";
  case GPUSignedDistanceFieldsBuildKernel::CollectClearBricks:
    return "CollectClearBricksCS";
  case GPUSignedDistanceFieldsBuildKernel::PrepareClearBricks:
    return "PrepareClearBricksCS";
  case GPUSignedDistanceFieldsBuildKernel::ClearBrickEntry:
    return "ClearBrickEntryCS";
  case GPUSignedDistanceFieldsBuildKernel::ClearBuildCounters:
    return "ClearBuildCountersCS";
  case GPUSignedDistanceFieldsBuildKernel::ResetCascade:
    return "ResetCascadeCS";
  case GPUSignedDistanceFieldsBuildKernel::InitializeCascade:
    return "InitializeCascadeCS";
  case GPUSignedDistanceFieldsBuildKernel::MarkCascadeUninitialized:
    return "MarkCascadeUninitializedCS";
  case GPUSignedDistanceFieldsBuildKernel::FreeCascade:
    return "FreeCascadeCS";
  case GPUSignedDistanceFieldsBuildKernel::ScrollCascade:
    return "ScrollCascadeCS";
  case GPUSignedDistanceFieldsBuildKernel::ClearRefCounters:
    return "ClearRefCountersCS";
  case GPUSignedDistanceFieldsBuildKernel::ClearJobCounter:
    return "ClearJobCounterCS";
  case GPUSignedDistanceFieldsBuildKernel::InvalidateJobAreas:
    return "InvalidateJobAreasCS";
  case GPUSignedDistanceFieldsBuildKernel::CoarseCulling:
    return "CoarseCullingCS";
  case GPUSignedDistanceFieldsBuildKernel::ScanJobs:
    return "ScanJobsCS";
  case GPUSignedDistanceFieldsBuildKernel::FinalizeJobScan:
    return "FinalizeJobScanCS";
  case GPUSignedDistanceFieldsBuildKernel::Voxelize:
    return "VoxelizeCS";
  case GPUSignedDistanceFieldsBuildKernel::ScanReferences:
    return "ScanReferencesCS";
  case GPUSignedDistanceFieldsBuildKernel::FinalizeReferenceScan:
    return "FinalizeReferenceScanCS";
  case GPUSignedDistanceFieldsBuildKernel::CompactReferences:
    return "CompactReferencesCS";
  case GPUSignedDistanceFieldsBuildKernel::ClearBrickStorage:
    return "ClearBrickStorageCS";
  case GPUSignedDistanceFieldsBuildKernel::EmitSdf:
    return "EmitSdfCS";
  case GPUSignedDistanceFieldsBuildKernel::CompressBricks:
    return "CompressBricksCS";
  default:
    return "UnknownCS";
  }
}

class GPUSignedDistanceFieldsBuildStepPassBase : public rendering::Pass
{
public:
  static constexpr uint32_t GlobalCascadeIndex = std::numeric_limits<uint32_t>::max();

  struct DispatchUniforms
  {
    GPUSignedDistanceFieldsManager::CascadeInfoGPU cascade;
    uint32_t pathIndex = 0u;
    uint32_t cascadeIndex = 0u;
    uint32_t cascadeCount = 0u;
    uint32_t dispatchAllCascades = 0u;
    uint32_t numJobs = 0u;
    uint32_t performSkinning = 0u;
    uint32_t doInitialization = 0u;
    uint32_t totalBricks = 0u;
    uint32_t maxReferences = 0u;
    uint32_t maxBricksPerBake = 0u;
    uint32_t atlasBricksPerAxis = 0u;
    uint32_t numJobVoxels = 0u;
    uint32_t maxTriangleSwapWords = 0u;
    uint32_t maxJobsPerCascade = 0u;
    uint32_t maxJobScanGroups = 0u;
    uint32_t maxVoxelScanGroups = 0u;
    uint32_t surfaceCachePageResolution = 0u;
    uint32_t surfaceCachePagesPerRow = 0u;
    uint32_t _padding3 = 0u;
    uint32_t _padding4 = 0u;
  };
  static_assert(sizeof(DispatchUniforms) % 16u == 0u, "DispatchUniforms must satisfy uniform-buffer alignment");

  GPUSignedDistanceFieldsBuildStepPassBase(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      GPUSignedDistanceFieldsBuildPathSettings settings,
      GPUSignedDistanceFieldsBuildKernel kernel,
      bool dispatchAllCascades = false)
      : manager_(manager), path_(path), cascadeIndex_(cascadeIndex), settings_(settings), kernel_(kernel), dispatchAllCascades_(dispatchAllCascades)
  {
  }

  ~GPUSignedDistanceFieldsBuildStepPassBase() override
  {
    for (const rendering::BufferId bufferId : frameOverrideBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(bufferId);
    }
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

protected:
  static uint32_t divCeil(uint32_t value, uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  GPUSignedDistanceFieldsManager &manager()
  {
    return manager_;
  }

  const GPUSignedDistanceFieldsManager &manager() const
  {
    return manager_;
  }

  GPUSignedDistanceFieldsManager::Path path() const
  {
    return path_;
  }

  uint32_t cascadeIndex() const
  {
    return cascadeIndex_;
  }

  bool usesGlobalBindingGroup() const
  {
    return cascadeIndex_ == GlobalCascadeIndex;
  }

  bool dispatchAllCascades() const
  {
    return dispatchAllCascades_;
  }

  const GPUSignedDistanceFieldsBuildPathSettings &settings() const
  {
    return settings_;
  }

  const auto &sharedResources() const
  {
    return manager_.getSharedResources();
  }

  const auto &pathResources() const
  {
    return manager_.getPathResources(path_);
  }

  const auto &cascadeResources() const
  {
    return pathResources().cascades[bindingCascadeIndex()];
  }

  GPUSignedDistanceFieldsManager::CascadeInfoGPU cascadeInfo() const
  {
    return manager_.getCascadeInfo(bindingCascadeIndex());
  }

  bool isPathBuildable() const
  {
    return manager_.isPathBuildable(path_);
  }

  void initializeResources(uint32_t maxUniformRecords = 1u)
  {
    if (uniformBuffer_.isValid())
      return;

    const uint32_t alignedUniformSize = alignedUniformStride();
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = static_cast<uint64_t>(alignedUniformSize) * maxUniformRecords,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    layout_ = createLayout();
    bindingGroups_ = createBindingGroups(bindingCascadeIndex());

    const char *entry = buildKernelEntryName(kernel_);
    const std::string shaderPath = std::string("assets/shaders/spirv/gpusigneddistancefields-build-") + entry + ".spirv";
    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_" + entry + ".shader",
            .layout = layout_,
            .src = os::io::readRelativeFile(shaderPath),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = entry,
            .layout = layout_,
            .name = passName + "_" + entry + ".pipeline",
            .shader = shader_,
        });
  }

  DispatchUniforms makeUniforms(uint32_t uniformCascadeIndex) const
  {
    const auto &managerSettings = manager_.getSettings();
    DispatchUniforms uniforms{};
    uniforms.cascade = manager_.getCascadeInfo(uniformCascadeIndex);
    uniforms.pathIndex = static_cast<uint32_t>(path_);
    uniforms.cascadeIndex = uniformCascadeIndex;
    uniforms.cascadeCount = managerSettings.cascadeCount;
    uniforms.dispatchAllCascades = dispatchAllCascades_ ? 1u : 0u;
    uniforms.performSkinning = settings_.performSkinning ? 1u : 0u;
    uniforms.doInitialization = usesGlobalBindingGroup() && manager_.needsContextInitialization() ? 1u : 0u;
    uniforms.totalBricks = managerSettings.maxBricks;
    uniforms.maxReferences = managerSettings.maxReferences;
    uniforms.maxBricksPerBake = managerSettings.maxBricksPerBake;
    uniforms.atlasBricksPerAxis = managerSettings.atlasBrickCapacityPerAxis;
    uniforms.maxTriangleSwapWords = managerSettings.maxTriangleSwapWords != 0u ? managerSettings.maxTriangleSwapWords : (managerSettings.maxReferences * 9u);
    uniforms.maxJobsPerCascade = managerSettings.maxJobsPerCascade;
    uniforms.maxJobScanGroups = std::max<uint32_t>(1u, divCeil(managerSettings.maxJobsPerCascade, GPUSignedDistanceFieldsManager::SCAN_GROUP_SIZE));
    uniforms.maxVoxelScanGroups = std::max<uint32_t>(1u, divCeil(GPUSignedDistanceFieldsManager::CASCADE_VOXEL_COUNT, GPUSignedDistanceFieldsManager::SCAN_GROUP_SIZE));
    uniforms.surfaceCachePageResolution = managerSettings.surfaceCachePageResolution;
    uniforms.surfaceCachePagesPerRow = manager_.getSharedResources().surfaceCacheLayout.pagesPerRow;
    return uniforms;
  }

  DispatchUniforms makeCurrentUniforms() const
  {
    DispatchUniforms uniforms = makeUniforms(bindingCascadeIndex());
    if (!usesGlobalBindingGroup())
    {
      if (dispatchAllCascades_)
      {
        uniforms.numJobs = maxJobCountAcrossCascades();
        uniforms.numJobVoxels = maxJobVoxelCountAcrossCascades();
      }
      else
      {
        uniforms.numJobs = cascadeResources().jobCount;
        uniforms.numJobVoxels = cascadeResources().jobVoxelCount;
      }
    }
    return uniforms;
  }

public:
  // Re-writes the per-pass uniform buffer with current manager state.
  // Call this each frame before GPU submission when job counts or cascade info change.
  void writeCurrentUniforms()
  {
    if (!uniformBuffer_.isValid() || usesGlobalBindingGroup())
      return;
    DispatchUniforms u = makeCurrentUniforms();
    writeUniformBufferCurrentFrame(0u, sizeof(DispatchUniforms), &u);
  }

  // Re-writes uniforms for context-begin (global) passes, updating doInitialization
  // from the manager's current initialization state. Must be called each frame via preFrame().
  void writeContextBeginUniforms()
  {
    if (!uniformBuffer_.isValid() || !usesGlobalBindingGroup())
      return;
    DispatchUniforms u = makeUniforms(0u);
    u.doInitialization = manager_.needsContextInitialization() ? 1u : 0u;
    writeUniformBufferCurrentFrame(0u, sizeof(DispatchUniforms), &u);
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    const rendering::BufferId bufferId = getFrameOverrideBufferId(frameSlot);
    if (bufferId != rendering::BufferId::Invalid)
    {
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = bufferId});
    }
  }

protected:
  void dispatchDirect(rendering::CommandRecorder &commandBuffer, const DispatchUniforms &uniforms, uint32_t x, uint32_t y, uint32_t z, uint32_t uniformIndex = 0u)
  {
    const uint32_t dynamicOffset = uniformIndex * alignedUniformStride();
    DispatchUniforms upload = uniforms;
    // Record-time dispatch uniforms are frame-specific. Writing them into every
    // override slot lets a later queued frame overwrite the still-in-flight
    // frame's clipmap/remap state before the GPU consumes it.
    writeUniformBufferCurrentFrame(dynamicOffset, sizeof(DispatchUniforms), &upload);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, const_cast<uint32_t *>(&dynamicOffset), 1u);
    commandBuffer.cmdDispatch(x, y, z);
  }

  void dispatchIndirect(
      rendering::CommandRecorder &commandBuffer,
      const DispatchUniforms &uniforms,
      const rendering::Buffer &indirectBuffer,
      uint64_t indirectOffset,
      uint32_t uniformIndex = 0u)
  {
    const uint32_t dynamicOffset = uniformIndex * alignedUniformStride();
    DispatchUniforms upload = uniforms;
    // See dispatchDirect(): indirect dispatches still consume per-frame
    // uniforms, so only update the current frame slot.
    writeUniformBufferCurrentFrame(dynamicOffset, sizeof(DispatchUniforms), &upload);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, const_cast<uint32_t *>(&dynamicOffset), 1u);
    commandBuffer.cmdDispatchIndirect(indirectBuffer, indirectOffset);
  }

  uint32_t dispatchCascadeCount() const
  {
    return dispatchAllCascades_ ? manager_.getCascadeCount() : 1u;
  }

  uint32_t maxJobCountAcrossCascades() const
  {
    uint32_t result = 0u;
    for (const auto &cascade : pathResources().cascades)
      result = std::max(result, cascade.jobCount);
    return result;
  }

  uint32_t maxJobVoxelCountAcrossCascades() const
  {
    uint32_t result = 0u;
    for (const auto &cascade : pathResources().cascades)
      result = std::max(result, cascade.jobVoxelCount);
    return result;
  }

  uint32_t currentJobCount() const
  {
    return dispatchAllCascades_ ? maxJobCountAcrossCascades() : cascadeResources().jobCount;
  }

  uint32_t currentJobVoxelCount() const
  {
    return dispatchAllCascades_ ? maxJobVoxelCountAcrossCascades() : cascadeResources().jobVoxelCount;
  }

  bool cascadeWasScrolled(uint32_t cascadeIndex) const
  {
    const auto &cascade = manager_.getCascadeInfo(cascadeIndex);
    return cascade.clipmapInvalidationOffset[0] != 0 ||
           cascade.clipmapInvalidationOffset[1] != 0 ||
           cascade.clipmapInvalidationOffset[2] != 0;
  }

  bool anyCascadeWasScrolled() const
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
    {
      if (cascadeWasScrolled(cascadeIndex))
        return true;
    }
    return false;
  }

  bool anyCascadeNeedsInitialization() const
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < manager_.getCascadeCount(); ++cascadeIndex)
    {
      const auto &cascade = manager_.getCascadeInfo(cascadeIndex);
      if (cascade.isInitialized == 0u)
        return true;
    }
    return false;
  }

private:
  void initializeFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;

    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    if (frameCount <= 1u || !frameOverrideBufferIds_.empty())
      return;

    frameOverrideBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    const uint64_t bufferSize = renderGraph->getBufferSize(uniformBuffer_);
    const auto usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push;
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = uniformBuffer_.name + ".override_frame" + std::to_string(frameSlot),
              .size = bufferSize,
              .usage = usage,
          });
    }
  }

  rendering::BufferId getFrameOverrideBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideBufferIds_[frameSlot % frameOverrideBufferIds_.size()];
  }

  void writeUniformBufferForFrame(uint32_t frameSlot, uint64_t offset, uint64_t size, const void *data) const
  {
    const rendering::BufferId bufferId = getFrameOverrideBufferId(frameSlot);
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->bufferWrite(bufferId, offset, size, const_cast<void *>(data));
      return;
    }

    renderGraph->bufferWrite(uniformBuffer_, offset, size, const_cast<void *>(data));
  }

  void writeUniformBufferCurrentFrame(uint64_t offset, uint64_t size, const void *data) const
  {
    writeUniformBufferForFrame(getCurrentFrameIndex(), offset, size, data);
  }

  uint32_t bindingCascadeIndex() const { return usesGlobalBindingGroup() ? 0u : cascadeIndex_; }

  uint32_t alignedUniformStride() const
  {
    const uint32_t alignment = static_cast<uint32_t>(renderGraph->getRHI()->GetProperties().uniformBufferAlignment);
    return ((static_cast<uint32_t>(sizeof(DispatchUniforms)) + alignment - 1u) / alignment) * alignment;
  }

  rendering::BindingsLayout createLayout()
  {
    return renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeInfos", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeBuildInfos", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "meshTable", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vertices", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "indices", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "boneWeights", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "boneMatrices", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "instances", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "jobs", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "jobSubvoxelOffsets", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchCounters", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchJobCounters", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchIndexSwap", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchRawReferences", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchCompactedReferences", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchReferenceCounters", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchVoxelAllocationFailCounters", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchJobCountersScan", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchJobGlobalCountersScan", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchReferenceCounterScan", .binding = 20, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchReferenceGlobalScan", .binding = 21, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchStampScan", .binding = 22, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchStampGlobalScan", .binding = 23, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchBrickStorage", .binding = 24, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchBrickStorageOffsets", .binding = 25, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchBrickCompressionList", .binding = 26, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "scratchBrickClearList", .binding = 27, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "indirectArgs", .binding = 28, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextIndirectArgs", .binding = 29, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeBrickMap", .binding = 30, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickVoxelMap", .binding = 31, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickAABB", .binding = 32, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickFreeList", .binding = 33, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickClearList", .binding = 34, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickDirtyList", .binding = 35, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickMergeList", .binding = 36, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickEikonalCounters", .binding = 37, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextCounters", .binding = 38, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "surfaceMaterials", .binding = 40, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "surfaceCacheMaterialReferenceScores", .binding = 45, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .storageTextures = {
                    {.name = "sdfAtlas", .binding = 39, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfaceCacheMaterialReferenceAtlas", .binding = 44, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "surfaceCacheMaterialReferenceBasisAtlas", .binding = 46, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });
  }

  static rendering::TextureView makeAtlasStorageView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
    };
  }

  rendering::BindingGroups createBindingGroups(uint32_t)
  {
    const auto &shared = sharedResources();
    const auto &pathResourcesRef = pathResources();

    return renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(DispatchUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = shared.cascadeInfos, .offset = 0u, .size = shared.cascadeInfosSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = pathResourcesRef.cascadeBuildInfos, .offset = 0u, .size = pathResourcesRef.cascadeBuildInfosSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 3, .bufferView = {.buffer = shared.meshTable, .offset = 0u, .size = shared.meshTableSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = shared.vertices, .offset = 0u, .size = shared.verticesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 5, .bufferView = {.buffer = shared.indices, .offset = 0u, .size = shared.indicesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = shared.boneWeights, .offset = 0u, .size = shared.boneWeightsSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 7, .bufferView = {.buffer = shared.boneMatrices, .offset = 0u, .size = shared.boneMatricesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 8, .bufferView = {.buffer = pathResourcesRef.instances, .offset = 0u, .size = pathResourcesRef.instancesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 9, .bufferView = {.buffer = pathResourcesRef.jobBuffers, .offset = 0u, .size = pathResourcesRef.jobBuffersSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 10, .bufferView = {.buffer = pathResourcesRef.jobSubvoxelOffsetBuffers, .offset = 0u, .size = pathResourcesRef.jobSubvoxelOffsetBuffersSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 11, .bufferView = {.buffer = pathResourcesRef.scratch.counters, .offset = 0u, .size = pathResourcesRef.scratch.countersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 12, .bufferView = {.buffer = pathResourcesRef.scratch.jobCounters, .offset = 0u, .size = pathResourcesRef.scratch.jobCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 13, .bufferView = {.buffer = pathResourcesRef.scratch.indexSwap, .offset = 0u, .size = pathResourcesRef.scratch.indexSwapSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 14, .bufferView = {.buffer = pathResourcesRef.scratch.rawReferences, .offset = 0u, .size = pathResourcesRef.scratch.rawReferencesSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 15, .bufferView = {.buffer = pathResourcesRef.scratch.compactedReferences, .offset = 0u, .size = pathResourcesRef.scratch.compactedReferencesSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 16, .bufferView = {.buffer = pathResourcesRef.scratch.referenceCounters, .offset = 0u, .size = pathResourcesRef.scratch.referenceCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 17, .bufferView = {.buffer = pathResourcesRef.scratch.voxelAllocationFailCounters, .offset = 0u, .size = pathResourcesRef.scratch.voxelAllocationFailCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 18, .bufferView = {.buffer = pathResourcesRef.scratch.jobCountersScan, .offset = 0u, .size = pathResourcesRef.scratch.jobCountersScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 19, .bufferView = {.buffer = pathResourcesRef.scratch.jobGlobalCountersScan, .offset = 0u, .size = pathResourcesRef.scratch.jobGlobalCountersScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 20, .bufferView = {.buffer = pathResourcesRef.scratch.referenceCounterScan, .offset = 0u, .size = pathResourcesRef.scratch.referenceCounterScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 21, .bufferView = {.buffer = pathResourcesRef.scratch.referenceGlobalScan, .offset = 0u, .size = pathResourcesRef.scratch.referenceGlobalScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 22, .bufferView = {.buffer = pathResourcesRef.scratch.stampScan, .offset = 0u, .size = pathResourcesRef.scratch.stampScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 23, .bufferView = {.buffer = pathResourcesRef.scratch.stampGlobalScan, .offset = 0u, .size = pathResourcesRef.scratch.stampGlobalScanSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 24, .bufferView = {.buffer = pathResourcesRef.scratch.brickStorage, .offset = 0u, .size = pathResourcesRef.scratch.brickStorageSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 25, .bufferView = {.buffer = pathResourcesRef.scratch.brickStorageOffsets, .offset = 0u, .size = pathResourcesRef.scratch.brickStorageOffsetsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 26, .bufferView = {.buffer = pathResourcesRef.scratch.brickCompressionList, .offset = 0u, .size = pathResourcesRef.scratch.brickCompressionListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 27, .bufferView = {.buffer = pathResourcesRef.scratch.brickClearList, .offset = 0u, .size = pathResourcesRef.scratch.brickClearListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 28, .bufferView = {.buffer = pathResourcesRef.stageIndirectArgs, .offset = 0u, .size = pathResourcesRef.stageIndirectArgsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 29, .bufferView = {.buffer = shared.contextIndirectArgs, .offset = 0u, .size = shared.contextIndirectArgsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 30, .bufferView = {.buffer = pathResourcesRef.brickMaps, .offset = 0u, .size = pathResourcesRef.brickMapsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 31, .bufferView = {.buffer = shared.brickVoxelMap, .offset = 0u, .size = shared.brickVoxelMapSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 32, .bufferView = {.buffer = shared.brickAABB, .offset = 0u, .size = shared.brickAABBSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 33, .bufferView = {.buffer = shared.brickFreeList, .offset = 0u, .size = shared.brickFreeListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 34, .bufferView = {.buffer = shared.brickClearList, .offset = 0u, .size = shared.brickClearListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 35, .bufferView = {.buffer = shared.brickDirtyList, .offset = 0u, .size = shared.brickDirtyListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 36, .bufferView = {.buffer = shared.brickMergeList, .offset = 0u, .size = shared.brickMergeListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 37, .bufferView = {.buffer = shared.brickEikonalCounters, .offset = 0u, .size = shared.brickEikonalCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 38, .bufferView = {.buffer = shared.contextCounters, .offset = 0u, .size = shared.contextCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 40, .bufferView = {.buffer = shared.surfaceMaterials, .offset = 0u, .size = shared.surfaceMaterialsSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 45, .bufferView = {.buffer = shared.surfaceCacheMaterialReferenceScores, .offset = 0u, .size = shared.surfaceCacheMaterialReferenceScoresSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                    },
                .storageTextures = {
                    {.binding = 39, .textureView = makeAtlasStorageView(shared.sdfAtlas)},
                    {.binding = 44, .textureView = makeAtlasStorageView(shared.surfaceCacheMaterialReferenceAtlas)},
                    {.binding = 46, .textureView = makeAtlasStorageView(shared.surfaceCacheMaterialReferenceBasisAtlas)},
                },
            }},
        });
  }

  GPUSignedDistanceFieldsManager &manager_;
  GPUSignedDistanceFieldsManager::Path path_;
  uint32_t cascadeIndex_;
  GPUSignedDistanceFieldsBuildPathSettings settings_;
  GPUSignedDistanceFieldsBuildKernel kernel_;
  bool dispatchAllCascades_ = false;
  rendering::Buffer uniformBuffer_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  std::vector<rendering::BufferId> frameOverrideBufferIds_{};
};

} // namespace detail
} // namespace gpgpu
} // namespace gpusigneddistancefields
