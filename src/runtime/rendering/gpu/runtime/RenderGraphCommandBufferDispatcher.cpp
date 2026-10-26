#include "RenderGraphCommandBufferDispatcher.hpp"

#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{

namespace
{

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

CommandBuffer RenderGraphCommandBufferDispatcher::allocate(RenderGraph &renderGraph, Queue queue, const RenderGraphRuntimeCallbacks &callbacks) const
{
  CommandBuffer commandBuffer = CommandBuffer::Invalid;

  timedRHI(
      callbacks,
      "allocateCommandBuffers",
      [&]()
      {
        auto allocated = renderGraph.rhi->allocateCommandBuffers(queue, 1u);
        if (!allocated.empty())
        {
          commandBuffer = allocated.front();
        }
      });

  return commandBuffer;
}

void RenderGraphCommandBufferDispatcher::begin(RenderGraph &renderGraph, CommandBuffer commandBuffer, const RenderGraphRuntimeCallbacks &callbacks, bool oneTimeSubmit) const
{
  timedRHI(
      callbacks,
      "beginCommandBuffer",
      [&]()
      {
        renderGraph.rhi->beginCommandBuffer(commandBuffer, oneTimeSubmit);
      });
}

void RenderGraphCommandBufferDispatcher::end(RenderGraph &renderGraph, CommandBuffer commandBuffer, const RenderGraphRuntimeCallbacks &callbacks) const
{
  timedRHI(
      callbacks,
      "endCommandBuffer",
      [&]()
      {
        renderGraph.rhi->endCommandBuffer(commandBuffer);
      });
}

} // namespace rendering
