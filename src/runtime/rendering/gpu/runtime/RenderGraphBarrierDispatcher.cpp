#include "RenderGraphBarrierDispatcher.hpp"

namespace rendering
{

namespace
{

class ScopedTimingScope
{
public:
  ScopedTimingScope(const RenderGraphRuntimeCallbacks &callbacks, const char *scopeName) : callbacks(callbacks), enabled(callbacks.beginScopedTiming && callbacks.endScopedTiming)
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

} // namespace

void RenderGraphBarrierDispatcher::submitPreBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  {
    ScopedTimingScope timing(callbacks, "barriers.dispatch.buffer.pre");
    bufferDispatcher.submitPreBarriers(renderGraph, nodeIndex, commandBuffer, runtimeResources, callbacks);
  }

  {
    ScopedTimingScope timing(callbacks, "barriers.dispatch.texture.pre");
    textureDispatcher.submitPreBarriers(renderGraph, nodeIndex, commandBuffer, runtimeResources, callbacks);
  }
}

void RenderGraphBarrierDispatcher::submitPostBarriers(
    RenderGraph &renderGraph,
    uint32_t nodeIndex,
    CommandBuffer commandBuffer,
    RenderGraphRuntimeResourcesManager &runtimeResources,
    const RenderGraphRuntimeCallbacks &callbacks) const
{
  {
    ScopedTimingScope timing(callbacks, "barriers.dispatch.buffer.post");
    bufferDispatcher.submitPostBarriers(renderGraph, nodeIndex, commandBuffer, runtimeResources, callbacks);
  }

  {
    ScopedTimingScope timing(callbacks, "barriers.dispatch.texture.post");
    textureDispatcher.submitPostBarriers(renderGraph, nodeIndex, commandBuffer, runtimeResources, callbacks);
  }
}

} // namespace rendering
