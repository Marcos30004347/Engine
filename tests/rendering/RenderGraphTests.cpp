#include <cassert>
#include <algorithm>
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
  struct TextureCopyRecord
  {
    TextureId src = TextureId::Invalid;
    TextureId dst = TextureId::Invalid;
  };

  struct ImageBarrierRecord
  {
    TextureId texture = TextureId::Invalid;
    ResourceLayout oldLayout = ResourceLayout::UNDEFINED;
    ResourceLayout newLayout = ResourceLayout::UNDEFINED;
    AccessPattern srcAccess = AccessPattern::NONE;
    AccessPattern dstAccess = AccessPattern::NONE;
  };

  explicit FakeRHI(const bool unifyQueues = false)
      : unifyQueues_(unifyQueues)
  {
    features = DeviceFeatures::DeviceFeatures_Compute;
  }

  Queue canonicalQueue(const Queue queue) const override
  {
    if (unifyQueues_ && queue != Queue::None && queue != Queue::Present)
      return Queue::Graphics;
    return queue;
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

  std::vector<CommandBuffer> allocateCommandBuffers(Queue queue, uint32_t count) override
  {
    allocatedQueues_.push_back(queue);
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
  void cmdCopyImage(CommandBuffer, const TextureView &src, const TextureView &dst) override
  {
    textureCopies_.push_back(TextureCopyRecord{.src = src.resourceId, .dst = dst.resourceId});
  }
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
  void cmdResetTimer(CommandBuffer, const Timer &, uint32_t) override {}
  void cmdStartTimer(CommandBuffer, const Timer &, uint32_t, PipelineStage) override {}
  void cmdStopTimer(CommandBuffer, const Timer &, uint32_t, PipelineStage) override {}
  void cmdResolveTimersToBuffer(CommandBuffer, const Timer *, uint32_t, uint32_t, BufferId, uint64_t) override {}
  void cmdDispatchIndirect(CommandBuffer, const BufferView &) override {}
  void cmdDispatch(CommandBuffer, uint32_t, uint32_t, uint32_t) override {}
  void cmdImageBarrier(CommandBuffer, TextureId texture, PipelineStage, PipelineStage, AccessPattern srcAccess, AccessPattern dstAccess, ResourceLayout oldLayout, ResourceLayout newLayout, ImageAspectFlags, uint32_t, uint32_t, uint32_t, uint32_t, Queue, Queue) override
  {
    imageBarriers_.push_back(ImageBarrierRecord{
        .texture = texture,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcAccess = srcAccess,
        .dstAccess = dstAccess,
    });
  }
  void cmdBufferBarrier(CommandBuffer, BufferId, PipelineStage, PipelineStage, AccessPattern, AccessPattern, uint32_t, uint32_t, Queue, Queue) override {}
  void cmdMemoryBarrier(CommandBuffer, PipelineStage, PipelineStage, AccessPattern, AccessPattern) override {}
  void cmdPipelineBarrier(CommandBuffer, PipelineStage, PipelineStage, AccessPattern, AccessPattern) override {}

  GPUFuture submit(Queue queue, CommandBuffer *, uint32_t, GPUFuture *, uint32_t, bool) override
  {
    submittedQueues_.push_back(queue);
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

  BindingGroupsId createBindingGroups(const BindingGroupsInfo &info) override
  {
    for (const GroupInfo &group : info.groups)
      for (const BindingBuffer &binding : group.buffers)
        assert(binding.bufferView.resourceId != BufferId::Invalid);
    bindingGroups_.push_back(info);
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

  void clearRecords()
  {
    textureCopies_.clear();
    imageBarriers_.clear();
  }

  const std::vector<TextureCopyRecord> &textureCopies() const
  {
    return textureCopies_;
  }

  const std::vector<ImageBarrierRecord> &imageBarriers() const
  {
    return imageBarriers_;
  }

  const std::vector<BindingGroupsInfo> &bindingGroups() const
  {
    return bindingGroups_;
  }

  const std::vector<Queue> &allocatedQueues() const
  {
    return allocatedQueues_;
  }

  const std::vector<Queue> &submittedQueues() const
  {
    return submittedQueues_;
  }

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
  bool unifyQueues_ = false;
  std::unordered_map<BufferId, std::vector<uint8_t>> buffers_;
  std::vector<TextureCopyRecord> textureCopies_;
  std::vector<ImageBarrierRecord> imageBarriers_;
  std::vector<BindingGroupsInfo> bindingGroups_;
  std::vector<Queue> allocatedQueues_;
  std::vector<Queue> submittedQueues_;
};

BufferInfo makeBufferInfo(const std::string &name)
{
  return BufferInfo{
    .name = name,
    .size = 1024u,
    .usage = BufferUsage_CopySrc | BufferUsage_CopyDst,
  };
}

TextureInfo makeCopyTextureInfo(const std::string &name, bool frameLocal)
{
  return TextureInfo{
    .name = name,
    .format = Format::Format_RGBA8Uint,
    .memoryProperties = BufferUsage::BufferUsage_None,
    .usage = ImageUsage::ImageUsage_TransferSrc | ImageUsage::ImageUsage_TransferDst,
    .width = 4u,
    .height = 4u,
    .depth = 1u,
    .mipLevels = 1u,
    .frameLocal = frameLocal,
  };
}

TextureView makeTransferSrcView(Texture texture)
{
  return TextureView{
    .texture = texture,
    .flags = ImageAspectFlags::Color,
    .baseMipLevel = 0u,
    .levelCount = 1u,
    .baseArrayLayer = 0u,
    .layerCount = 1u,
    .access = AccessPattern::TRANSFER_READ,
    .layout = ResourceLayout::TRANSFER_SRC,
  };
}

TextureView makeTransferDstView(Texture texture)
{
  return TextureView{
    .texture = texture,
    .flags = ImageAspectFlags::Color,
    .baseMipLevel = 0u,
    .levelCount = 1u,
    .baseArrayLayer = 0u,
    .layerCount = 1u,
    .access = AccessPattern::TRANSFER_WRITE,
    .layout = ResourceLayout::TRANSFER_DST,
  };
}

class CopyTexturePass final : public Pass
{
public:
  CopyTexturePass(Texture src, Texture dst) : src_(std::move(src)), dst_(std::move(dst)) {}

private:
  void recordCommandBuffer(CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdCopyImage(makeTransferSrcView(src_), makeTransferDstView(dst_));
  }

  Texture src_;
  Texture dst_;
};

class PingPongTexturePass final : public Pass
{
public:
  PingPongTexturePass(Texture a, Texture b) : a_(std::move(a)), b_(std::move(b)) {}

private:
  void recordCommandBuffer(CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdCopyImage(makeTransferSrcView(a_), makeTransferDstView(b_));
    commandBuffer.cmdCopyImage(makeTransferSrcView(b_), makeTransferDstView(a_));
  }

  Texture a_;
  Texture b_;
};

class BindGroupsPass final : public Pass
{
public:
  BindGroupsPass(BindingGroups groups, Buffer source, Buffer destination)
      : groups_(std::move(groups)),
        source_(std::move(source)),
        destination_(std::move(destination))
  {
  }

private:
  void recordCommandBuffer(CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdCopyBuffer(
        {.buffer = source_, .offset = 0u, .size = 256u, .access = AccessPattern::TRANSFER_READ},
        {.buffer = destination_, .offset = 0u, .size = 256u, .access = AccessPattern::TRANSFER_WRITE});
  }

  BindingGroups groups_;
  Buffer source_;
  Buffer destination_;
};

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

  {
    FakeRHI rhi(true);
    RenderGraph renderGraph(
        &rhi,
        RenderGraph::Settings{
            .mergeSingleQueueCommandBuffers = true,
        });

    const Buffer bufferA = renderGraph.createBuffer(makeBufferInfo("UnifiedA"));
    const Buffer bufferB = renderGraph.createBuffer(makeBufferInfo("UnifiedB"));
    renderGraph.registerPass<gpgpu::CopyBufferPass>(
        "unifiedCopy", 0u, bufferA, 0u, 256u, bufferB, 0u, 256u);
    renderGraph.compile();

    RenderGraph::Frame frame;
    renderGraph.run(frame, RenderGraph::Overrides{});
    renderGraph.waitFrame(frame);

    assert(!rhi.allocatedQueues().empty());
    assert(!rhi.submittedQueues().empty());
    assert(std::all_of(
        rhi.allocatedQueues().begin(),
        rhi.allocatedQueues().end(),
        [](const Queue queue) { return queue == Queue::Graphics; }));
    assert(std::all_of(
        rhi.submittedQueues().begin(),
        rhi.submittedQueues().end(),
        [](const Queue queue) { return queue == Queue::Graphics; }));
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(
        &rhi,
        RenderGraph::Settings{
            .mergeSingleQueueCommandBuffers = true,
        });

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
    assert(stats.submissionCount == 1u);
    assert(stats.submittedCommandBufferCount == 1u);
    assert(stats.sameQueueWaitDependencyCount == 0u);
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi, RenderGraph::Settings{.maxFramesInFlight = 2u});

    const Texture frameLocalTexture = renderGraph.createTexture(makeCopyTextureInfo("FrameLocalTexture", true));
    const Texture persistentTexture = renderGraph.createTexture(makeCopyTextureInfo("PersistentTexture", false));

    renderGraph.registerPass<CopyTexturePass>("copyFrameLocalTexture", 0u, frameLocalTexture, persistentTexture);
    renderGraph.compile();

    RenderGraph::Overrides overrides{};
    const RenderGraph::CommandBufferHandle frame0Commands = renderGraph.createCommandBuffer(overrides, 0u, false);
    const RenderGraph::CommandBufferHandle frame1Commands = renderGraph.createCommandBuffer(overrides, 1u, false);

    RenderGraph::Frame frame0{};
    renderGraph.run(frame0, frame0Commands);
    renderGraph.waitFrame(frame0);

    RenderGraph::Frame frame1{};
    renderGraph.run(frame1, frame1Commands);
    renderGraph.waitFrame(frame1);

    const std::vector<FakeRHI::TextureCopyRecord> &copies = rhi.textureCopies();
    assert(copies.size() == 2u);
    assert(copies[0].src != TextureId::Invalid);
    assert(copies[1].src != TextureId::Invalid);
    assert(copies[0].src != copies[1].src);
    assert(copies[0].dst == copies[1].dst);
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi, RenderGraph::Settings{.maxFramesInFlight = 2u});

    const Texture frameLocalTexture = renderGraph.createTexture(makeCopyTextureInfo("FrameLocalPingPong", true));
    const Texture persistentTexture = renderGraph.createTexture(makeCopyTextureInfo("PersistentPingPong", false));

    renderGraph.registerPass<PingPongTexturePass>("pingPongFrameLocalTexture", 0u, frameLocalTexture, persistentTexture);
    renderGraph.compile();

    RenderGraph::Overrides overrides{};
    const RenderGraph::CommandBufferHandle commands = renderGraph.createCommandBuffer(overrides, 0u, true);

    RenderGraph::Frame frame{};
    renderGraph.run(frame, commands);
    renderGraph.waitFrame(frame);

    const std::vector<FakeRHI::TextureCopyRecord> &copies = rhi.textureCopies();
    assert(copies.size() == 2u);
    const TextureId frameLocalTextureId = copies[0].src;
    const bool hasSlotReuseBarrier = std::any_of(
        rhi.imageBarriers().begin(),
        rhi.imageBarriers().end(),
        [&](const FakeRHI::ImageBarrierRecord &barrier)
        {
          return barrier.texture == frameLocalTextureId &&
                 barrier.oldLayout == ResourceLayout::TRANSFER_DST &&
                 barrier.newLayout == ResourceLayout::TRANSFER_SRC &&
                 barrier.srcAccess == AccessPattern::TRANSFER_WRITE &&
                 barrier.dstAccess == AccessPattern::TRANSFER_READ;
        });
    assert(hasSlotReuseBarrier);
  }

  {
    FakeRHI rhi;
    RenderGraph renderGraph(&rhi);

    const Buffer virtualBuffer = renderGraph.createBuffer({
      .name = "VirtualHistoryBuffer",
      .size = 256u,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
      .isVirtual = true,
    });
    const BindingsLayout layout = renderGraph.createBindingsLayout({
      .name = "VirtualHistoryLayout",
      .groups = {BindingGroupLayout{.buffers = {{
        .name = "history",
        .binding = 0u,
        .isDynamic = false,
        .type = BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = BindingVisibility::BindingVisibility_Compute,
      }}}},
    });
    const BindingGroups groups = renderGraph.createBindingGroups({
      .layout = layout,
      .name = "VirtualHistoryGroups",
      .groups = {GroupInfo{
        .name = "group0",
        .buffers = {{
          .binding = 0u,
          .bufferView = {
            .buffer = virtualBuffer,
            .offset = 0u,
            .size = 256u,
            .access = AccessPattern::SHADER_READ,
          },
        }},
      }},
    });
    const Buffer destination = renderGraph.createBuffer({
      .name = "VirtualHistoryDestination",
      .size = 256u,
      .usage = BufferUsage::BufferUsage_CopyDst,
    });
    renderGraph.registerPass<BindGroupsPass>(
        "bindVirtualHistory", 0u, groups, virtualBuffer, destination);
    renderGraph.compile();

    const BufferId physicalBuffer = rhi.createBuffer({
      .name = "VirtualHistoryBuffer.physical",
      .size = 256u,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    RenderGraph::Overrides overrides{};
    overrides.bufferOverrides.emplace(
        virtualBuffer.name,
        RenderGraph::BufferOverride{.bufferId = physicalBuffer});
    const RenderGraph::CommandBufferHandle commands =
        renderGraph.createCommandBuffer(overrides, 0u, false);

    RenderGraph::Frame frame{};
    renderGraph.run(frame, commands);
    renderGraph.waitFrame(frame);

    assert(!rhi.bindingGroups().empty());
    assert(rhi.bindingGroups().back().groups[0].buffers[0].bufferView.resourceId ==
           physicalBuffer);
    rhi.deleteBuffer(physicalBuffer);
  }

  os::Logger::shutdown();
  return 0;
}
