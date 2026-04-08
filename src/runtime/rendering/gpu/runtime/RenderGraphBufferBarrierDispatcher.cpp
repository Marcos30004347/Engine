#include "RenderGraphBufferBarrierDispatcher.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <limits>
#include <sstream>

namespace rendering
{

namespace
{

class ScopedTimingScope
{
public:
  ScopedTimingScope(const RenderGraphRuntimeCallbacks &callbacks, const char *scopeName)
      : callbacks(callbacks), enabled(callbacks.beginScopedTiming && callbacks.endScopedTiming)
  {
    if (enabled)
    {
      callbacks.beginScopedTiming(scopeName);
    }
  }

  ~ScopedTimingScope()
  {
    if (enabled)
    {
      callbacks.endScopedTiming();
    }
  }

private:
  const RenderGraphRuntimeCallbacks &callbacks;
  bool enabled = false;
};

constexpr uint64_t InvalidNodeId = std::numeric_limits<uint64_t>::max();

const char *queueName(Queue q)
{
  switch (q)
  {
  case Queue::None:
    return "None";
  case Queue::Graphics:
    return "Graphics";
  case Queue::Compute:
    return "Compute";
  case Queue::Transfer:
    return "Transfer";
  case Queue::Present:
    return "Present";
  default:
    return "Unknown";
  }
}

PipelineStage queueStage(Queue queue)
{
  switch (queue)
  {
  case Queue::Compute:
    return PipelineStage::COMPUTE_SHADER;
  case Queue::Graphics:
    return PipelineStage::ALL_GRAPHICS;
  case Queue::Transfer:
    return PipelineStage::TRANSFER;
  default:
    return PipelineStage::ALL_COMMANDS;
  }
}

PipelineStage queueStageForAccess(Queue queue, AccessPattern access)
{
  // Indirect dispatch/draw argument reads execute in the indirect-command
  // stage, which is not covered by COMPUTE_SHADER or ALL_GRAPHICS.
  if (access & AccessPattern::INDIRECT_COMMAND_READ)
  {
    return PipelineStage::ALL_COMMANDS;
  }

  return queueStage(queue);
}

bool hasWriteAccess(AccessPattern access)
{
  const uint64_t bits = static_cast<uint64_t>(access);
  constexpr uint64_t writeMask =
      static_cast<uint64_t>(AccessPattern::SHADER_WRITE) |
      static_cast<uint64_t>(AccessPattern::COLOR_ATTACHMENT_WRITE) |
      static_cast<uint64_t>(AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE) |
      static_cast<uint64_t>(AccessPattern::TRANSFER_WRITE) |
      static_cast<uint64_t>(AccessPattern::MEMORY_WRITE);
  return (bits & writeMask) != 0u;
}

bool isReadOnlyAccess(AccessPattern access)
{
  return access != AccessPattern::NONE && !hasWriteAccess(access);
}

template <typename Fn>
void timedRHI(const RenderGraphRuntimeCallbacks &callbacks, const char *opName, Fn &&fn)
{
  if (callbacks.beginRHITiming)
  {
    callbacks.beginRHITiming(opName);
  }

  fn();

  if (callbacks.endRHITiming)
  {
    callbacks.endRHITiming();
  }
}

} // namespace

void RenderGraphBufferBarrierDispatcher::submitPreBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  (void)runtimeResources;
  const auto &transitions = renderGraph.nodes[nodeIndex].preBufferTransitions;
  auto &currentNode = renderGraph.nodes[nodeIndex];
  for (auto &transition : transitions)
  {
    const bool isQueueTransfer = transition.toQueue != transition.fromQueue;
    ScopedTimingScope transitionTiming(callbacks, isQueueTransfer ? "barriers.buffer.queueTransfer" : "barriers.buffer.sameQueue");
    const auto &buffer = renderGraph.runtimeContext.buffers[transition.runtimeId];
    if (!isQueueTransfer && transition.fromNode != InvalidNodeId && isReadOnlyAccess(transition.fromAccess) && isReadOnlyAccess(transition.toAccess))
    {
      renderGraph.lastRunDebugStats.skippedReadOnlyBufferBarrierCount += 1u;
      continue;
    }
    PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
    if (transition.fromNode != InvalidNodeId)
    {
      const auto &fromNode = renderGraph.nodes[transition.fromNode];
      fromStage = queueStageForAccess(fromNode.queue, transition.fromAccess);
    }
    const PipelineStage toStage = queueStageForAccess(transition.toQueue, transition.toAccess);
    if (isQueueTransfer)
    {
      renderGraph.lastRunDebugStats.bufferQueueTransferCount += 1u;
      if (callbacks.logBarrier)
      {
        std::ostringstream message;
        message << "[RenderGraph][Barrier][Buffer][QueueTransfer] '" << buffer.name << "'"
                << " phase=" << (transition.phase == BufferBarrier::Phase::QueueTransferRelease ? "release" : "acquire")
                << " fromNode=" << transition.fromNode
                << " node=" << currentNode.id
                << " offset=" << transition.offset
                << " size=" << transition.size
                << " fromAccess=" << static_cast<uint32_t>(transition.fromAccess)
                << " toAccess=" << static_cast<uint32_t>(transition.toAccess)
                << " fromQueue=" << queueName(transition.fromQueue)
                << " toQueue=" << queueName(transition.toQueue);
        callbacks.logBarrier(message.str());
      }
      if (transition.fromNode == InvalidNodeId)
      {
        timedRHI(callbacks, "cmdBufferBarrier", [&]()
                 {
                   renderGraph.lastRunDebugStats.emittedBufferBarrierCount += 1u;
                   renderGraph.rhi->cmdBufferBarrier(commandBuffer, buffer.resourceId, fromStage, toStage, transition.fromAccess, transition.toAccess, transition.offset, transition.size, Queue::None, Queue::None);
                 });
        continue;
      }
      const AccessPattern emittedFromAccess = transition.phase == BufferBarrier::Phase::QueueTransferAcquire ? AccessPattern::NONE : transition.fromAccess;
      const AccessPattern emittedToAccess = transition.phase == BufferBarrier::Phase::QueueTransferRelease ? AccessPattern::NONE : transition.toAccess;
      timedRHI(callbacks, "cmdBufferBarrier", [&]()
               {
                 renderGraph.lastRunDebugStats.emittedBufferBarrierCount += 1u;
                 renderGraph.rhi->cmdBufferBarrier(commandBuffer, buffer.resourceId, fromStage, toStage, emittedFromAccess, emittedToAccess, transition.offset, transition.size, transition.fromQueue, transition.toQueue);
               });
      continue;
    }
    if (callbacks.logBarrier)
    {
      std::ostringstream message;
      message << "[RenderGraph][Barrier][Buffer] '" << buffer.name << "'"
              << " offset=" << transition.offset
              << " size=" << transition.size
              << " fromAccess=" << static_cast<uint32_t>(transition.fromAccess)
              << " toAccess=" << static_cast<uint32_t>(transition.toAccess)
              << " queue=" << queueName(transition.fromQueue);
      callbacks.logBarrier(message.str());
    }
    timedRHI(callbacks, "cmdBufferBarrier", [&]()
             {
               renderGraph.lastRunDebugStats.emittedBufferBarrierCount += 1u;
               renderGraph.rhi->cmdBufferBarrier(commandBuffer, buffer.resourceId, fromStage, toStage, transition.fromAccess, transition.toAccess, transition.offset, transition.size, transition.fromQueue, transition.toQueue);
             });
  }
}

void RenderGraphBufferBarrierDispatcher::submitPostBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  (void)runtimeResources;
  const auto &transitions = renderGraph.nodes[nodeIndex].postBufferTransitions;
  auto &currentNode = renderGraph.nodes[nodeIndex];
  for (auto &transition : transitions)
  {
    const bool isQueueTransfer = transition.toQueue != transition.fromQueue;
    ScopedTimingScope transitionTiming(callbacks, isQueueTransfer ? "barriers.buffer.queueTransfer" : "barriers.buffer.sameQueue");
    const auto &buffer = renderGraph.runtimeContext.buffers[transition.runtimeId];
    PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
    if (transition.fromNode != InvalidNodeId)
    {
      const auto &fromNode = renderGraph.nodes[transition.fromNode];
      fromStage = queueStageForAccess(fromNode.queue, transition.fromAccess);
    }
    const PipelineStage toStage = queueStageForAccess(transition.toQueue, transition.toAccess);
    renderGraph.lastRunDebugStats.bufferQueueTransferCount += 1u;
    if (callbacks.logBarrier)
    {
      std::ostringstream message;
      message << "[RenderGraph][Barrier][Buffer][QueueTransfer] '" << buffer.name << "'"
              << " phase=release"
              << " fromNode=" << transition.fromNode
              << " node=" << currentNode.id
              << " offset=" << transition.offset
              << " size=" << transition.size
              << " fromAccess=" << static_cast<uint32_t>(transition.fromAccess)
              << " toAccess=" << static_cast<uint32_t>(transition.toAccess)
              << " fromQueue=" << queueName(transition.fromQueue)
              << " toQueue=" << queueName(transition.toQueue);
      callbacks.logBarrier(message.str());
    }
    timedRHI(callbacks, "cmdBufferBarrier", [&]()
             {
               renderGraph.lastRunDebugStats.emittedBufferBarrierCount += 1u;
               renderGraph.rhi->cmdBufferBarrier(commandBuffer, buffer.resourceId, fromStage, toStage, transition.fromAccess, AccessPattern::NONE, transition.offset, transition.size, transition.fromQueue, transition.toQueue);
             });
  }
}

} // namespace rendering
