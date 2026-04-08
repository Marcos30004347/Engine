#pragma once

#include "RenderGraphRuntimeCallbacks.hpp"

namespace rendering
{

class RenderGraph;

class RenderGraphCommandBufferDispatcher
{
public:
  CommandBuffer allocate(RenderGraph &renderGraph, Queue queue, const RenderGraphRuntimeCallbacks &callbacks) const;
  void begin(RenderGraph &renderGraph, CommandBuffer commandBuffer, const RenderGraphRuntimeCallbacks &callbacks, bool oneTimeSubmit) const;
  void end(RenderGraph &renderGraph, CommandBuffer commandBuffer, const RenderGraphRuntimeCallbacks &callbacks) const;
};

} // namespace rendering
