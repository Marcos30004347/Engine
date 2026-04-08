#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"

using namespace rendering;

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
  }

  void bufferRead(BufferId bufferId, const uint64_t offset, const uint64_t size, std::function<void(const void *)> callback) override
  {
    auto &storage = buffers_[bufferId];
    if (storage.size() < offset + size)
    {
      storage.resize(static_cast<size_t>(offset + size), 0u);
    }
    callback(storage.data() + offset);
  }

  void bufferWrite(BufferId bufferId, const uint64_t offset, const uint64_t size, void *data) override
  {
    auto &storage = buffers_[bufferId];
    if (storage.size() < offset + size)
    {
      storage.resize(static_cast<size_t>(offset + size), 0u);
    }
    std::memcpy(storage.data() + offset, data, static_cast<size_t>(size));
  }

  const SwapChain createSwapChain(uint32_t, uint32_t, uint32_t) override
  {
    return SwapChain::Invalid;
  }

  void destroySwapChain(SwapChain) override {}

  Format getSwapChainFormat(SwapChain) override
  {
    return Format::Format_None;
  }

  const uint32_t getSwapChainImagesCount(SwapChain) override
  {
    return 0u;
  }

  const uint32_t getSwapChainImagesWidth(SwapChain) override
  {
    return 0u;
  }

  const uint32_t getSwapChainImagesHeight(SwapChain) override
  {
    return 0u;
  }

  const TextureView getSwapChainTextureView(SwapChain, uint32_t) override
  {
    return {};
  }

  const TextureView getCurrentSwapChainTextureView(SwapChain) override
  {
    return {};
  }

  std::vector<CommandBuffer> allocateCommandBuffers(Queue, uint32_t count) override
  {
    std::vector<CommandBuffer> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
      result.push_back(nextCommandBuffer());
    }
    return result;
  }

  void releaseCommandBuffer(std::vector<CommandBuffer> &buffers) override
  {
    buffers.clear();
  }

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

  bool isCompleted(GPUFuture &future) override
  {
    return !future.valid() || future.checkStatus() == FenceStatus::FINISHED;
  }

  const Timer createTimer(const TimerInfo info) override
  {
    return Timer{
      .name = info.name,
      .sampleCount = info.sampleCount,
    };
  }

  void deleteTimer(const Timer &) override {}

  double readTimer(const Timer &) override
  {
    return 0.0;
  }

  double getTimestampPeriodNs() const override
  {
    return 1.0;
  }

  BufferId createBuffer(const BufferInfo &info) override
  {
    const BufferId id = nextBufferId();
    buffers_[id].resize(static_cast<size_t>(info.size), 0u);
    return id;
  }

  TextureId createTexture(const TextureInfo &) override
  {
    return nextTextureId();
  }

  SamplerId createSampler(const SamplerInfo &) override
  {
    return nextSamplerId();
  }

  BindingsLayoutId createBindingsLayout(const BindingsLayoutInfo &) override
  {
    return nextBindingsLayoutId();
  }

  BindingGroupsId createBindingGroups(const BindingGroupsInfo &) override
  {
    return nextBindingGroupsId();
  }

  GraphicsPipelineId createGraphicsPipeline(const GraphicsPipelineInfo &) override
  {
    return nextGraphicsPipelineId();
  }

  ComputePipelineId createComputePipeline(const ComputePipelineInfo &) override
  {
    return nextComputePipelineId();
  }

  ShaderId createShader(const ShaderInfo) override
  {
    return nextShaderId();
  }

  void deleteShader(ShaderId) override {}

  void deleteBuffer(BufferId resourceId) override
  {
    buffers_.erase(resourceId);
  }

  void deleteTexture(TextureId) override {}
  void deleteSampler(SamplerId) override {}
  void deleteBindingsLayout(BindingsLayoutId) override {}
  void deleteBindingGroups(BindingGroupsId) override {}
  void deleteGraphicsPipeline(GraphicsPipelineId) override {}
  void deleteComputePipeline(ComputePipelineId) override {}

private:
  template <typename Id> static Id nextId(uintptr_t &counter)
  {
    return static_cast<Id>(counter++);
  }

  BufferId nextBufferId()
  {
    return nextId<BufferId>(nextBufferId_);
  }

  TextureId nextTextureId()
  {
    return nextId<TextureId>(nextTextureId_);
  }

  SamplerId nextSamplerId()
  {
    return nextId<SamplerId>(nextSamplerId_);
  }

  ShaderId nextShaderId()
  {
    return nextId<ShaderId>(nextShaderId_);
  }

  BindingsLayoutId nextBindingsLayoutId()
  {
    return nextId<BindingsLayoutId>(nextBindingsLayoutId_);
  }

  BindingGroupsId nextBindingGroupsId()
  {
    return nextId<BindingGroupsId>(nextBindingGroupsId_);
  }

  GraphicsPipelineId nextGraphicsPipelineId()
  {
    return nextId<GraphicsPipelineId>(nextGraphicsPipelineId_);
  }

  ComputePipelineId nextComputePipelineId()
  {
    return nextId<ComputePipelineId>(nextComputePipelineId_);
  }

  CommandBuffer nextCommandBuffer()
  {
    return nextId<CommandBuffer>(nextCommandBufferId_);
  }

  uintptr_t nextBufferId_ = 1u;
  uintptr_t nextTextureId_ = 1u;
  uintptr_t nextSamplerId_ = 1u;
  uintptr_t nextShaderId_ = 1u;
  uintptr_t nextBindingsLayoutId_ = 1u;
  uintptr_t nextBindingGroupsId_ = 1u;
  uintptr_t nextGraphicsPipelineId_ = 1u;
  uintptr_t nextComputePipelineId_ = 1u;
  uintptr_t nextCommandBufferId_ = 1u;
  std::unordered_map<BufferId, std::vector<uint8_t>> buffers_;
};

BufferInfo makeBufferInfo(const std::string &name)
{
  return BufferInfo{
    .name = name,
    .size = 1024u,
    .usage = BufferUsage_CopySrc | BufferUsage_CopyDst,
  };
}

uint32_t submittedNodeCount(const RenderGraph::Frame &frame)
{
  uint32_t count = 0u;
  for (const auto &submission : frame.submissionFutures)
  {
    count += static_cast<uint32_t>(submission.nodes.size());
  }
  return count;
}

} // namespace

int main()
{
  os::Logger::start();

  FakeRHI rhi;
  RenderGraph renderGraph(&rhi);

  const Buffer bufferA = renderGraph.createBuffer(makeBufferInfo("BufferA"));
  const Buffer bufferB = renderGraph.createBuffer(makeBufferInfo("BufferB"));
  const Buffer bufferC = renderGraph.createBuffer(makeBufferInfo("BufferC"));
  const Buffer bufferD = renderGraph.createBuffer(makeBufferInfo("BufferD"));

  renderGraph.registerPass<gpgpu::CopyBufferPass>("copyAtoB", 0u, bufferA, 0u, 256u, bufferB, 0u, 256u);
  renderGraph.registerPass<gpgpu::CopyBufferPass>("copyBtoC", 1u, bufferB, 0u, 256u, bufferC, 0u, 256u);
  renderGraph.registerPass<gpgpu::CopyBufferPass>("copyCtoD", 2u, bufferC, 0u, 256u, bufferD, 0u, 256u);

  renderGraph.compile();

  RenderGraph::Frame frame;
  renderGraph.run(frame, RenderGraph::Overrides{});
  const uint32_t totalSubmittedNodes = submittedNodeCount(frame);
  const RenderGraph::RunDebugStats stats = renderGraph.getLastRunDebugStats();

  renderGraph.waitFrame(frame);

  assert(totalSubmittedNodes == 3u);
  assert(stats.submittedCommandBufferCount == totalSubmittedNodes);
  assert(stats.sameQueueWaitDependencyCount >= 2u);

  os::Logger::shutdown();
  return 0;
}
