#include "RenderGraphTextureBarrierDispatcher.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"

#include <cassert>
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

void RenderGraphTextureBarrierDispatcher::submitPreBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  (void)runtimeResources;
  const auto &transitions = renderGraph.nodes[nodeIndex].preTextureTransitions;
  auto &currentNode = renderGraph.nodes[nodeIndex];
  for (auto &transition : transitions)
  {
    const bool isQueueTransfer = transition.toQueue != transition.fromQueue;
    ScopedTimingScope transitionTiming(callbacks, isQueueTransfer ? "barriers.texture.queueTransfer" : "barriers.texture.sameQueue");
    const auto &texture = renderGraph.runtimeContext.textures[transition.runtimeId];
    const ResourceLayout overrideLayout = texture.overrideLayout;
    const ResourceLayout fromLayout = (overrideLayout != ResourceLayout::UNDEFINED) ? overrideLayout : transition.fromLayout;
    if (!isQueueTransfer && transition.fromNode != InvalidNodeId && fromLayout == transition.toLayout && isReadOnlyAccess(transition.fromAccess) && isReadOnlyAccess(transition.toAccess))
    {
      renderGraph.lastRunDebugStats.skippedReadOnlyTextureBarrierCount += 1u;
      continue;
    }
    PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
    if (transition.fromNode != InvalidNodeId)
    {
      fromStage = queueStage(renderGraph.nodes[transition.fromNode].queue);
    }
    const PipelineStage toStage = queueStage(transition.toQueue);
    assert(transition.toLayout != ResourceLayout::UNDEFINED && transition.toLayout != ResourceLayout::PREINITIALIZED);
    const ImageAspectFlags aspectFlags = GetImageAspectFlags(transition.format);
    if (isQueueTransfer)
    {
      renderGraph.lastRunDebugStats.textureQueueTransferCount += 1u;
      if (callbacks.logBarrier)
      {
        std::ostringstream message;
        message << "[RenderGraph][Barrier][Image][QueueTransfer] '" << texture.name << "'"
                << " phase=" << (transition.phase == TextureBarrier::Phase::QueueTransferRelease ? "release" : "acquire")
                << " fromNode=" << transition.fromNode
                << " node=" << currentNode.id
                << " layout " << static_cast<uint32_t>(fromLayout)
                << " -> " << static_cast<uint32_t>(transition.toLayout)
                << " access " << static_cast<uint32_t>(transition.fromAccess)
                << " -> " << static_cast<uint32_t>(transition.toAccess)
                << " mips [" << transition.baseMip << ".." << (transition.baseMip + transition.mipCount) << ")"
                << " layers [" << transition.baseLayer << ".." << (transition.baseLayer + transition.layerCount) << ")"
                << " fromQueue=" << queueName(transition.fromQueue)
                << " toQueue=" << queueName(transition.toQueue)
                << " fromStage=" << static_cast<uint32_t>(fromStage)
                << " toStage=" << static_cast<uint32_t>(toStage);
        callbacks.logBarrier(message.str());
      }
      if (transition.fromNode == InvalidNodeId)
      {
        timedRHI(callbacks, "cmdImageBarrier", [&]()
                 {
                   renderGraph.lastRunDebugStats.emittedTextureBarrierCount += 1u;
                   renderGraph.rhi->cmdImageBarrier(commandBuffer, texture.resourceId, fromStage, toStage, transition.fromAccess, transition.toAccess, fromLayout, transition.toLayout, aspectFlags, transition.baseMip, transition.mipCount, transition.baseLayer, transition.layerCount, Queue::None, Queue::None);
                 });
        continue;
      }
      const AccessPattern emittedFromAccess = transition.phase == TextureBarrier::Phase::QueueTransferAcquire ? AccessPattern::NONE : transition.fromAccess;
      const AccessPattern emittedToAccess = transition.phase == TextureBarrier::Phase::QueueTransferRelease ? AccessPattern::NONE : transition.toAccess;
      timedRHI(callbacks, "cmdImageBarrier", [&]()
               {
                 renderGraph.lastRunDebugStats.emittedTextureBarrierCount += 1u;
                 renderGraph.rhi->cmdImageBarrier(commandBuffer, texture.resourceId, fromStage, toStage, emittedFromAccess, emittedToAccess, fromLayout, transition.toLayout, aspectFlags, transition.baseMip, transition.mipCount, transition.baseLayer, transition.layerCount, transition.fromQueue, transition.toQueue);
               });
      continue;
    }
    if (callbacks.logBarrier)
    {
      std::ostringstream message;
      message << "[RenderGraph][Barrier][Image] '" << texture.name << "'"
              << " layout " << static_cast<uint32_t>(fromLayout)
              << " -> " << static_cast<uint32_t>(transition.toLayout)
              << " access " << static_cast<uint32_t>(transition.fromAccess)
              << " -> " << static_cast<uint32_t>(transition.toAccess)
              << " mips [" << transition.baseMip << ".." << (transition.baseMip + transition.mipCount) << ")"
              << " layers [" << transition.baseLayer << ".." << (transition.baseLayer + transition.layerCount) << ")"
              << " queue=" << queueName(transition.fromQueue)
              << " fromNode=" << transition.fromNode
              << " fromStage=" << static_cast<uint32_t>(fromStage)
              << " toStage=" << static_cast<uint32_t>(toStage);
      callbacks.logBarrier(message.str());
    }
    timedRHI(callbacks, "cmdImageBarrier", [&]()
             {
               renderGraph.lastRunDebugStats.emittedTextureBarrierCount += 1u;
               renderGraph.rhi->cmdImageBarrier(commandBuffer, texture.resourceId, fromStage, toStage, transition.fromAccess, transition.toAccess, fromLayout, transition.toLayout, aspectFlags, transition.baseMip, transition.mipCount, transition.baseLayer, transition.layerCount, transition.fromQueue, transition.toQueue);
             });
  }
}

void RenderGraphTextureBarrierDispatcher::submitPostBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  (void)runtimeResources;
  const auto &transitions = renderGraph.nodes[nodeIndex].postTextureTransitions;
  auto &currentNode = renderGraph.nodes[nodeIndex];
  for (auto &transition : transitions)
  {
    const auto &texture = renderGraph.runtimeContext.textures[transition.runtimeId];
    const ResourceLayout overrideLayout = texture.overrideLayout;
    const ResourceLayout fromLayout = (overrideLayout != ResourceLayout::UNDEFINED) ? overrideLayout : transition.fromLayout;
    PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
    if (transition.fromNode != InvalidNodeId)
    {
      fromStage = queueStage(renderGraph.nodes[transition.fromNode].queue);
    }
    const PipelineStage toStage = queueStage(transition.toQueue);
    const ImageAspectFlags aspectFlags = GetImageAspectFlags(transition.format);
    renderGraph.lastRunDebugStats.textureQueueTransferCount += 1u;
    if (callbacks.logBarrier)
    {
      std::ostringstream message;
      message << "[RenderGraph][Barrier][Image][QueueTransfer] '" << texture.name << "'"
              << " phase=release"
              << " fromNode=" << transition.fromNode
              << " node=" << currentNode.id
              << " layout " << static_cast<uint32_t>(fromLayout)
              << " -> " << static_cast<uint32_t>(transition.toLayout)
              << " access " << static_cast<uint32_t>(transition.fromAccess)
              << " -> " << static_cast<uint32_t>(transition.toAccess)
              << " mips [" << transition.baseMip << ".." << (transition.baseMip + transition.mipCount) << ")"
              << " layers [" << transition.baseLayer << ".." << (transition.baseLayer + transition.layerCount) << ")"
              << " fromQueue=" << queueName(transition.fromQueue)
              << " toQueue=" << queueName(transition.toQueue)
              << " fromStage=" << static_cast<uint32_t>(fromStage)
              << " toStage=" << static_cast<uint32_t>(toStage);
      callbacks.logBarrier(message.str());
    }
    timedRHI(callbacks, "cmdImageBarrier", [&]()
             {
               renderGraph.lastRunDebugStats.emittedTextureBarrierCount += 1u;
               renderGraph.rhi->cmdImageBarrier(commandBuffer, texture.resourceId, fromStage, toStage, transition.fromAccess, AccessPattern::NONE, fromLayout, transition.toLayout, aspectFlags, transition.baseMip, transition.mipCount, transition.baseLayer, transition.layerCount, transition.fromQueue, transition.toQueue);
             });
  }
}

} // namespace rendering
