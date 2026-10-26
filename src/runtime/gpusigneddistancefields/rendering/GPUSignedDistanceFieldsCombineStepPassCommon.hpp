#pragma once

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

enum class GPUSignedDistanceFieldsCombineKernel : uint32_t
{
  CollectDirtyBricks = 0u,
  PrepareEikonalArgs,
  Eikonal,
  MergeCascades,
  PrepareMergeBricksArgs,
  MergeBricks,
  BuildTreeAabb,
};

inline const char *combineKernelEntryName(GPUSignedDistanceFieldsCombineKernel kernel)
{
  switch (kernel)
  {
  case GPUSignedDistanceFieldsCombineKernel::CollectDirtyBricks:
    return "CollectDirtyBricksCS";
  case GPUSignedDistanceFieldsCombineKernel::PrepareEikonalArgs:
    return "PrepareEikonalArgsCS";
  case GPUSignedDistanceFieldsCombineKernel::Eikonal:
    return "EikonalCS";
  case GPUSignedDistanceFieldsCombineKernel::MergeCascades:
    return "MergeCascadesCS";
  case GPUSignedDistanceFieldsCombineKernel::PrepareMergeBricksArgs:
    return "PrepareMergeBricksArgsCS";
  case GPUSignedDistanceFieldsCombineKernel::MergeBricks:
    return "MergeBricksCS";
  case GPUSignedDistanceFieldsCombineKernel::BuildTreeAabb:
    return "BuildTreeAabbCS";
  default:
    return "UnknownCS";
  }
}

class GPUSignedDistanceFieldsCombineStepPassBase : public rendering::Pass
{
public:
  static constexpr uint32_t GlobalCascadeIndex = std::numeric_limits<uint32_t>::max();

  struct DispatchUniforms
  {
    GPUSignedDistanceFieldsManager::CascadeInfoGPU cascade;
    uint32_t cascadeIndex = 0u;
    uint32_t treeIteration = 0u;
    uint32_t cascadeCount = 0u;
    uint32_t dispatchAllCascades = 0u;
    uint32_t totalBricks = 0u;
    uint32_t atlasBricksPerAxis = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };
  static_assert(sizeof(DispatchUniforms) % 16u == 0u, "DispatchUniforms must satisfy uniform-buffer alignment");

  GPUSignedDistanceFieldsCombineStepPassBase(
      GPUSignedDistanceFieldsManager &manager,
      uint32_t cascadeIndex,
      GPUSignedDistanceFieldsCombineKernel kernel,
      bool dispatchAllCascades = false)
      : manager_(manager), cascadeIndex_(cascadeIndex), kernel_(kernel), dispatchAllCascades_(dispatchAllCascades)
  {
  }

  ~GPUSignedDistanceFieldsCombineStepPassBase() override
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

  const auto &sharedResources() const
  {
    return manager_.getSharedResources();
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

    const char *entry = combineKernelEntryName(kernel_);
    const std::string shaderPath = std::string("assets/shaders/spirv/gpusigneddistancefields-combine-") + entry + ".spirv";
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
    DispatchUniforms uniforms{};
    uniforms.cascade = manager_.getCascadeInfo(uniformCascadeIndex);
    uniforms.cascadeIndex = uniformCascadeIndex;
    uniforms.cascadeCount = manager_.getCascadeCount();
    uniforms.dispatchAllCascades = dispatchAllCascades_ ? 1u : 0u;
    uniforms.totalBricks = manager_.getSettings().maxBricks;
    uniforms.atlasBricksPerAxis = manager_.getSettings().atlasBrickCapacityPerAxis;
    return uniforms;
  }

  virtual DispatchUniforms makeCurrentUniforms() const
  {
    return makeUniforms(bindingCascadeIndex());
  }

public:
  // Re-writes the per-pass uniform buffer with current manager state.
  // Call this each frame before GPU submission when cascade info changes.
  void writeCurrentUniforms()
  {
    if (!uniformBuffer_.isValid() || usesGlobalBindingGroup())
      return;
    DispatchUniforms u = makeCurrentUniforms();
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

  uint32_t bindingCascadeIndex() const
  {
    return usesGlobalBindingGroup() ? 0u : cascadeIndex_;
  }

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
                        {.name = "staticBrickMap", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "skinnedBrickMap", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickVoxelMap", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickAabb", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickFreeList", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickDirtyList", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickMergeList", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickEikonalCounters", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextCounters", .binding = 12, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextIndirectArgs", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .storageTextures = {
                    {.name = "sdfAtlas", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
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
    const auto &staticPath = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Static);
    const auto &skinnedPath = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Skinned);
    const auto &combinedPath = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Combined);

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
                        {.binding = 2, .bufferView = {.buffer = staticPath.brickMaps, .offset = 0u, .size = staticPath.brickMapsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 3, .bufferView = {.buffer = skinnedPath.brickMaps, .offset = 0u, .size = skinnedPath.brickMapsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 4, .bufferView = {.buffer = combinedPath.brickMaps, .offset = 0u, .size = combinedPath.brickMapsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 5, .bufferView = {.buffer = combinedPath.aabbTrees, .offset = 0u, .size = combinedPath.aabbTreesSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 6, .bufferView = {.buffer = shared.brickVoxelMap, .offset = 0u, .size = shared.brickVoxelMapSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 7, .bufferView = {.buffer = shared.brickAABB, .offset = 0u, .size = shared.brickAABBSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 8, .bufferView = {.buffer = shared.brickFreeList, .offset = 0u, .size = shared.brickFreeListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 9, .bufferView = {.buffer = shared.brickDirtyList, .offset = 0u, .size = shared.brickDirtyListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 10, .bufferView = {.buffer = shared.brickMergeList, .offset = 0u, .size = shared.brickMergeListSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 11, .bufferView = {.buffer = shared.brickEikonalCounters, .offset = 0u, .size = shared.brickEikonalCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 12, .bufferView = {.buffer = shared.contextCounters, .offset = 0u, .size = shared.contextCountersSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 13, .bufferView = {.buffer = shared.contextIndirectArgs, .offset = 0u, .size = shared.contextIndirectArgsSize, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                    },
                .storageTextures = {
                    {.binding = 14, .textureView = makeAtlasStorageView(shared.sdfAtlas)},
                },
            }},
        });
  }

  GPUSignedDistanceFieldsManager &manager_;
  uint32_t cascadeIndex_;
  GPUSignedDistanceFieldsCombineKernel kernel_;
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
