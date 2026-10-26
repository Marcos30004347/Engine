#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPass.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"

using namespace rendering;
using namespace gpusigneddistancefields;
using namespace gpusigneddistancefields::gpgpu;

namespace
{

struct ReadyFuture
{
  bool isValid() const
  {
    return true;
  }

  FenceStatus checkStatus() const
  {
    return FenceStatus::FINISHED;
  }
};

class FakeRHI final : public RHI
{
public:
  FakeRHI()
  {
    features = DeviceFeatures::DeviceFeatures_Compute;
    properties.uniformBufferAlignment = 256u;
    properties.maxComputeSharedMemorySize = 64u * 1024u;
    properties.maxComputeWorkGroupInvocations = 1024u;
  }

  void bufferRead(BufferId bufferId, const uint64_t offset, const uint64_t size, std::function<void(const void *)> callback) override
  {
    auto &storage = buffers_[bufferId];
    if (storage.size() < offset + size)
      storage.resize(static_cast<size_t>(offset + size), 0u);
    callback(storage.data() + offset);
  }

  void bufferWrite(BufferId bufferId, const uint64_t offset, const uint64_t size, void *data) override
  {
    auto &storage = buffers_[bufferId];
    if (storage.size() < offset + size)
      storage.resize(static_cast<size_t>(offset + size), 0u);
    std::memcpy(storage.data() + offset, data, static_cast<size_t>(size));
  }

  const SwapChain createSwapChain(uint32_t, uint32_t, uint32_t) override { return SwapChain::Invalid; }
  void destroySwapChain(SwapChain) override {}
  Format getSwapChainFormat(SwapChain) override { return Format::Format_None; }
  const uint32_t getSwapChainImagesCount(SwapChain) override { return 0u; }
  const uint32_t getSwapChainImagesWidth(SwapChain) override { return 0u; }
  const uint32_t getSwapChainImagesHeight(SwapChain) override { return 0u; }
  const TextureView getSwapChainTextureView(SwapChain, uint32_t) override { return {}; }
  const TextureView getCurrentSwapChainTextureView(SwapChain) override { return {}; }

  std::vector<CommandBuffer> allocateCommandBuffers(Queue, uint32_t count) override
  {
    std::vector<CommandBuffer> result;
    result.reserve(count);
    for (uint32_t i = 0u; i < count; ++i)
      result.push_back(nextCommandBuffer());
    return result;
  }

  void releaseCommandBuffer(std::vector<CommandBuffer> &buffers) override { buffers.clear(); }
  void beginCommandBuffer(CommandBuffer, bool) override {}
  void endCommandBuffer(CommandBuffer) override {}
  void cmdCopyBuffer(CommandBuffer, const BufferView &, const BufferView &) override {}
  void cmdCopyImage(CommandBuffer, const TextureView &, const TextureView &) override {}
  void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) override {}
  void cmdEndRenderPass(CommandBuffer) override {}
  void cmdBindBindingGroups(CommandBuffer, BindingGroupsId, uint32_t *, uint32_t) override {}
  void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) override {}
  void cmdBindComputePipeline(CommandBuffer, ComputePipeline) override {}
  void cmdBindVertexBuffer(CommandBuffer, uint32_t, const BufferView &) override {}
  void cmdBindIndexBuffer(CommandBuffer, const BufferView &, Type) override {}
  void cmdDraw(CommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void cmdDrawIndirect(CommandBuffer, const BufferView &, uint32_t, uint32_t) override {}
  void cmdDrawIndirectCount(CommandBuffer, const BufferView &, const BufferView &, uint32_t, uint32_t) override {}
  void cmdDrawIndexed(CommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
  void cmdDrawIndexedIndirect(CommandBuffer, const BufferView &, uint32_t, uint32_t) override {}
  void cmdResetTimer(CommandBuffer, const Timer &) override {}
  void cmdStartTimer(CommandBuffer, const Timer &, uint32_t, PipelineStage) override {}
  void cmdStopTimer(CommandBuffer, const Timer &, uint32_t, PipelineStage) override {}
  void cmdResolveTimersToBuffer(CommandBuffer, const Timer *, uint32_t, BufferId, uint64_t) override {}
  void cmdDispatchIndirect(CommandBuffer, const BufferView &) override {}
  void cmdDispatch(CommandBuffer, uint32_t, uint32_t, uint32_t) override {}
  void cmdImageBarrier(CommandBuffer, TextureId, PipelineStage, PipelineStage, AccessPattern, AccessPattern, ResourceLayout, ResourceLayout, ImageAspectFlags, uint32_t, uint32_t, uint32_t, uint32_t, Queue, Queue) override {}
  void cmdBufferBarrier(CommandBuffer, BufferId, PipelineStage, PipelineStage, AccessPattern, AccessPattern, uint32_t, uint32_t, Queue, Queue) override {}
  void cmdMemoryBarrier(CommandBuffer, PipelineStage, PipelineStage, AccessPattern, AccessPattern) override {}
  void cmdPipelineBarrier(CommandBuffer, PipelineStage, PipelineStage, AccessPattern, AccessPattern) override {}

  GPUFuture submit(Queue, CommandBuffer *, uint32_t, GPUFuture *, uint32_t, bool) override
  {
    return GPUFuture(ReadyFuture{});
  }

  void present(SwapChain &, TextureId, ResourceLayout) override {}
  void waitIdle() override {}
  void blockUntil(GPUFuture &) override {}
  bool isCompleted(GPUFuture &future) override { return !future.valid() || future.checkStatus() == FenceStatus::FINISHED; }

  const Timer createTimer(const TimerInfo info) override
  {
    return Timer{.name = info.name, .sampleCount = info.sampleCount};
  }

  void deleteTimer(const Timer &) override {}
  double readTimer(const Timer &) override { return 0.0; }
  double getTimestampPeriodNs() const override { return 1.0; }

  BufferId createBuffer(const BufferInfo &info) override
  {
    const BufferId id = nextBufferId();
    buffers_[id].resize(static_cast<size_t>(info.size), 0u);
    return id;
  }

  TextureId createTexture(const TextureInfo &) override { return nextTextureId(); }
  void textureWrite(TextureId, const uint64_t size, void *, ResourceLayout) override
  {
    ++textureWriteCount_;
    textureWriteBytes_ += size;
  }
  SamplerId createSampler(const SamplerInfo &) override { return nextSamplerId(); }
  BindingsLayoutId createBindingsLayout(const BindingsLayoutInfo &) override { return nextBindingsLayoutId(); }
  BindingGroupsId createBindingGroups(const BindingGroupsInfo &) override { return nextBindingGroupsId(); }
  GraphicsPipelineId createGraphicsPipeline(const GraphicsPipelineInfo &) override { return nextGraphicsPipelineId(); }
  ComputePipelineId createComputePipeline(const ComputePipelineInfo &) override { return nextComputePipelineId(); }
  ShaderId createShader(const ShaderInfo) override { return nextShaderId(); }
  void deleteShader(ShaderId) override {}
  void deleteBuffer(BufferId resourceId) override { buffers_.erase(resourceId); }
  void deleteTexture(TextureId) override {}
  void deleteSampler(SamplerId) override {}
  void deleteBindingsLayout(BindingsLayoutId) override {}
  void deleteBindingGroups(BindingGroupsId) override {}
  void deleteGraphicsPipeline(GraphicsPipelineId) override {}
  void deleteComputePipeline(ComputePipelineId) override {}

  uint32_t textureWriteCount() const { return textureWriteCount_; }
  uint64_t textureWriteBytes() const { return textureWriteBytes_; }

private:
  template <typename Id> static Id nextId(uintptr_t &counter)
  {
    return static_cast<Id>(counter++);
  }

  BufferId nextBufferId() { return nextId<BufferId>(nextBufferId_); }
  TextureId nextTextureId() { return nextId<TextureId>(nextTextureId_); }
  SamplerId nextSamplerId() { return nextId<SamplerId>(nextSamplerId_); }
  ShaderId nextShaderId() { return nextId<ShaderId>(nextShaderId_); }
  BindingsLayoutId nextBindingsLayoutId() { return nextId<BindingsLayoutId>(nextBindingsLayoutId_); }
  BindingGroupsId nextBindingGroupsId() { return nextId<BindingGroupsId>(nextBindingGroupsId_); }
  GraphicsPipelineId nextGraphicsPipelineId() { return nextId<GraphicsPipelineId>(nextGraphicsPipelineId_); }
  ComputePipelineId nextComputePipelineId() { return nextId<ComputePipelineId>(nextComputePipelineId_); }
  CommandBuffer nextCommandBuffer() { return nextId<CommandBuffer>(nextCommandBufferId_); }

  uintptr_t nextBufferId_ = 1u;
  uintptr_t nextTextureId_ = 1u;
  uintptr_t nextSamplerId_ = 1u;
  uintptr_t nextShaderId_ = 1u;
  uintptr_t nextBindingsLayoutId_ = 1u;
  uintptr_t nextBindingGroupsId_ = 1u;
  uintptr_t nextGraphicsPipelineId_ = 1u;
  uintptr_t nextComputePipelineId_ = 1u;
  uintptr_t nextCommandBufferId_ = 1u;
  uint32_t textureWriteCount_ = 0u;
  uint64_t textureWriteBytes_ = 0u;
  std::unordered_map<BufferId, std::vector<uint8_t>> buffers_;
};

uint32_t passIndex(RenderGraph &renderGraph, const std::string &passName)
{
  Pass *pass = renderGraph.get<Pass>(passName);
  if (pass == nullptr)
    throw std::runtime_error("Pass not registered: " + passName);
  return pass->getIndex();
}

void assertBefore(RenderGraph &renderGraph, const std::string &first, const std::string &second)
{
  const uint32_t firstIndex = passIndex(renderGraph, first);
  const uint32_t secondIndex = passIndex(renderGraph, second);
  assert(firstIndex < secondIndex);
  if (firstIndex >= secondIndex)
    throw std::runtime_error(first + " should be scheduled before " + second);
  std::cout << "[ASSERT] " << first << " index " << firstIndex << " < " << second << " index " << secondIndex << "\n";
}

void runGraph(RenderGraph &renderGraph)
{
  std::cout << "[RUN] compiling GPUSignedDistanceFieldsPassScheduleTests render graph\n";
  renderGraph.compile();

  std::cout << "[RUN] executing GPUSignedDistanceFieldsPassScheduleTests render graph\n";
  RenderGraph::Frame frame;
  RenderGraph::Overrides overrides;
  renderGraph.run(frame, overrides);

  std::cout << "[RUN] waiting GPUSignedDistanceFieldsPassScheduleTests frame\n";
  renderGraph.waitFrame(frame);
  std::cout << "[PASS] GPUSignedDistanceFieldsPassScheduleTests graph execution finished\n";
}

GPUSignedDistanceFieldsManager::Settings tinyManagerSettings()
{
  GPUSignedDistanceFieldsManager::Settings managerSettings{};
  managerSettings.cascadeCount = 1u;
  managerSettings.atlasBrickCapacityPerAxis = 4u;
  managerSettings.maxBricks = 32u;
  managerSettings.maxMeshes = 1u;
  managerSettings.maxVertices = 3u;
  managerSettings.maxIndices = 3u;
  managerSettings.maxBoneWeights = 4u;
  managerSettings.maxBoneMatrices = 4u;
  managerSettings.maxInstancesPerPath = 1u;
  managerSettings.maxJobsPerCascade = 1u;
  managerSettings.maxReferences = 64u;
  managerSettings.maxBricksPerBake = 16u;
  return managerSettings;
}

void testSurfaceCacheResidentResources()
{
  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);
    GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();
    managerSettings.surfaceCachePageResolution = 0u;
    GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
    const GPUSignedDistanceFieldsManager::SharedResources &shared = manager.getSharedResources();
    assert(shared.surfaceCacheMaterialReferenceAtlas.isValid());
    assert(shared.surfaceCacheMaterialReferenceBasisAtlas.isValid());
    assert(shared.surfaceCacheLayout.texelCount == 0u);
    assert(shared.surfaceCacheMaterialReferenceAtlasSize == 0u);
    assert(shared.surfaceCacheMaterialReferenceBasisAtlasSize == 0u);
    assert(rhi.textureWriteCount() == 0u);
    assert(rhi.textureWriteBytes() == 0u);
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);
    GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();
    managerSettings.surfaceCachePageResolution = 4u;
    GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
    const GPUSignedDistanceFieldsManager::SharedResources &shared = manager.getSharedResources();
    assert(shared.surfaceCacheMaterialReferenceAtlas.isValid());
    assert(shared.surfaceCacheLayout.pageCount == 192u);
    assert(shared.surfaceCacheLayout.pagesPerRow == 14u);
    assert(shared.surfaceCacheLayout.atlasWidth == 56u);
    assert(shared.surfaceCacheLayout.atlasHeight == 56u);
    assert(shared.surfaceCacheLayout.texelCount == 3136u);
    assert(shared.surfaceCacheMaterialReferenceAtlasSize == 50176u);
    assert(shared.surfaceCacheMaterialReferenceBasisAtlas.isValid());
    assert(shared.surfaceCacheMaterialReferenceBasisAtlasSize == 50176u);
    assert(rhi.textureWriteCount() == 2u);
    assert(rhi.textureWriteBytes() == 100352u);
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);
    GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();
    managerSettings.surfaceCachePageResolution = 4u;
    managerSettings.surfaceCacheEnableEmissive = true;
    GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
    const GPUSignedDistanceFieldsManager::SharedResources &shared = manager.getSharedResources();
    assert(shared.surfaceCacheMaterialReferenceAtlas.isValid());
    assert(shared.surfaceCacheMaterialReferenceAtlasSize == 50176u);
    assert(shared.surfaceCacheMaterialReferenceBasisAtlas.isValid());
    assert(shared.surfaceCacheMaterialReferenceBasisAtlasSize == 50176u);
    assert(rhi.textureWriteCount() == 2u);
    assert(rhi.textureWriteBytes() == 100352u);
  }

  std::cout << "[PASS] Surface cache resident resource allocation\n";
}

void testSurfaceMaterialResources()
{
  FakeRHI rhi;
  RenderGraph renderGraph(&rhi);
  GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();
  managerSettings.maxMeshes = 2u;
  GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
  const GPUSignedDistanceFieldsManager::SharedResources &shared = manager.getSharedResources();
  assert(shared.surfaceMaterials.isValid());
  assert(shared.surfaceMaterialsSize == managerSettings.maxMeshes * sizeof(GPUSignedDistanceFieldsManager::SurfaceMaterialGPU));
  assert(manager.getUploadedSurfaceMaterials().size() == 1u);
  assert(manager.getUploadedSurfaceMaterials().front().baseColorOpacity[0] == 0.85f);
  assert(manager.getUploadedSurfaceMaterials().front().normalRoughness[3] == 1.0f);

  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU red{};
  red.baseColorOpacity = {1.0f, 0.0f, 0.0f, 1.0f};
  red.normalRoughness[3] = 0.35f;
  red.emissiveMetallic[3] = 0.2f;
  GPUSignedDistanceFieldsManager::SurfaceMaterialGPU blue{};
  blue.baseColorOpacity = {0.0f, 0.0f, 1.0f, 1.0f};
  manager.uploadSurfaceMaterials({red, blue});
  assert(manager.getUploadedSurfaceMaterials().size() == 2u);
  assert(manager.getUploadedSurfaceMaterials()[0].normalRoughness[3] == 0.35f);
  assert(manager.getUploadedSurfaceMaterials()[1].baseColorOpacity[2] == 1.0f);

  GPUSignedDistanceFieldsManager::MeshTableEntryGPU mesh{};
  mesh.surfaceMaterialIndex = 1u;
  manager.uploadMeshTable({mesh});
  assert(manager.getUploadedMeshTable().front().surfaceMaterialIndex == 1u);

  std::cout << "[PASS] Surface material resource upload\n";
}

void testSurfaceCacheResolvePassScheduling()
{
  FakeRHI rhi;
  RenderGraph renderGraph(&rhi);

  GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();
  managerSettings.surfaceCachePageResolution = 4u;
  GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
  manager.updateClipmaps(math::Vec3f(0.0f, 0.0f, 0.0f));

  GPUSignedDistanceFieldsManager::JobGPU job{};
  job.aabbMin[0] = 0u;
  job.aabbMin[1] = 0u;
  job.aabbMin[2] = 0u;
  job.aabbMax[0] = 1u;
  job.aabbMax[1] = 1u;
  job.aabbMax[2] = 1u;
  manager.uploadJobs(GPUSignedDistanceFieldsManager::Path::Static, 0u, std::vector<GPUSignedDistanceFieldsManager::JobGPU>{job});

  GPUSignedDistanceFieldsPass *sdfPass =
      renderGraph.registerPass<GPUSignedDistanceFieldsPass>("gpuSignedDistanceFieldsPass", 34u, manager);

  assert(renderGraph.get<Pass>("gpuSignedDistanceFieldsPass.staticBuild.resolveSurfaceCache") == nullptr);
  assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.staticBuild.emitSdf", "gpuSignedDistanceFieldsPass.staticBuild.compressBricks");

  runGraph(renderGraph);

  delete sdfPass;
  std::cout << "[PASS] Surface cache material reference scheduling\n";
}

} // namespace

int main()
{
  os::Logger::start();

  testSurfaceCacheResidentResources();
  testSurfaceMaterialResources();
  testSurfaceCacheResolvePassScheduling();

  GPUSignedDistanceFieldsManager::Settings managerSettings = tinyManagerSettings();

  GPUSignedDistanceFieldsManager::JobGPU job{};
  job.aabbMin[0] = 0u;
  job.aabbMin[1] = 0u;
  job.aabbMin[2] = 0u;
  job.aabbMax[0] = 1u;
  job.aabbMax[1] = 1u;
  job.aabbMax[2] = 1u;
  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);

    GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
    manager.updateClipmaps(math::Vec3f(0.0f, 0.0f, 0.0f));
    manager.uploadJobs(GPUSignedDistanceFieldsManager::Path::Static, 0u, std::vector<GPUSignedDistanceFieldsManager::JobGPU>{job});
    manager.uploadJobs(GPUSignedDistanceFieldsManager::Path::Skinned, 0u, std::vector<GPUSignedDistanceFieldsManager::JobGPU>{job});

    GPUSignedDistanceFieldsPass *sdfPass =
        renderGraph.registerPass<GPUSignedDistanceFieldsPass>("gpuSignedDistanceFieldsPass", 34u, manager);

    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.staticBuild", "gpuSignedDistanceFieldsPass.staticBuild.clearCounters");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.staticBuild.clearCounters", "gpuSignedDistanceFieldsPass.staticBuild.compressBricks");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.staticBuild.compressBricks", "gpuSignedDistanceFieldsPass.skinnedBuild");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.skinnedBuild", "gpuSignedDistanceFieldsPass.skinnedBuild.compressBricks");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.skinnedBuild.compressBricks", "gpuSignedDistanceFieldsPass.fillBricks");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.fillBricks", "gpuSignedDistanceFieldsPass.fillBricks.collectDirtyBricks");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.fillBricks.collectDirtyBricks", "gpuSignedDistanceFieldsPass.fillBricks.prepareEikonalArgs");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.fillBricks.prepareEikonalArgs", "gpuSignedDistanceFieldsPass.fillBricks.eikonal");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.fillBricks.eikonal", "gpuSignedDistanceFieldsPass.combine");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.combine", "gpuSignedDistanceFieldsPass.combine.buildTreeAabb.bottom");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.combine.buildTreeAabb.bottom", "gpuSignedDistanceFieldsPass.combine.buildTreeAabb.mid");
    assertBefore(renderGraph, "gpuSignedDistanceFieldsPass.combine.buildTreeAabb.mid", "gpuSignedDistanceFieldsPass.combine.buildTreeAabb");

    const uint32_t firstDebugIndex = 34u + GPUSignedDistanceFieldsPass::maxChildPassCount(manager.getCascadeCount()) + 1u;
    const uint32_t buildTreeAabbIndex = passIndex(renderGraph, "gpuSignedDistanceFieldsPass.combine.buildTreeAabb");
    assert(buildTreeAabbIndex < firstDebugIndex);
    if (buildTreeAabbIndex >= firstDebugIndex)
      throw std::runtime_error("SDF debug pass range should start after the combine/build tree pass range");
    std::cout << "[ASSERT] gpuSignedDistanceFieldsPass.combine.buildTreeAabb index "
              << buildTreeAabbIndex << " < first debug index " << firstDebugIndex << "\n";

    runGraph(renderGraph);

    delete sdfPass;
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);

    GPUSignedDistanceFieldsManager manager(&renderGraph, managerSettings);
    manager.updateClipmaps(math::Vec3f(0.0f, 0.0f, 0.0f));
    manager.uploadJobs(GPUSignedDistanceFieldsManager::Path::Static, 0u, std::vector<GPUSignedDistanceFieldsManager::JobGPU>{job});
    manager.uploadJobs(GPUSignedDistanceFieldsManager::Path::Skinned, 0u, {});

    GPUSignedDistanceFieldsPass *sdfPass =
        renderGraph.registerPass<GPUSignedDistanceFieldsPass>("gpuSignedDistanceFieldsPass", 34u, manager);

    assert(renderGraph.get<Pass>("gpuSignedDistanceFieldsPass.skinnedBuild") != nullptr);
    assert(renderGraph.get<Pass>("gpuSignedDistanceFieldsPass.skinnedBuild.clearBuildCounters") == nullptr);
    if (renderGraph.get<Pass>("gpuSignedDistanceFieldsPass.skinnedBuild.clearBuildCounters") != nullptr)
      throw std::runtime_error("Skinned build child passes should not be registered without active skinned instances or skinned jobs");
    std::cout << "[ASSERT] skinned build children stay unregistered when skinned path has no work\n";

    delete sdfPass;
  }

  os::Logger::shutdown();
  std::cout << "GPUSignedDistanceFieldsPassScheduleTests passed\n";
  return 0;
}
