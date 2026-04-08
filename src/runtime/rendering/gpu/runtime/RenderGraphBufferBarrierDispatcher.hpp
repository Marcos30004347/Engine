#pragma once

#include "RenderGraphRuntimeCallbacks.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{

class RenderGraphRuntimeResourcesManager;

class RenderGraphBufferBarrierDispatcher
{
public:
  void submitPreBarriers(
      RenderGraph &renderGraph,
      uint32_t nodeIndex,
      CommandBuffer commandBuffer,
      RenderGraphRuntimeResourcesManager &runtimeResources,
      const RenderGraphRuntimeCallbacks &callbacks) const;
  void submitPostBarriers(
      RenderGraph &renderGraph,
      uint32_t nodeIndex,
      CommandBuffer commandBuffer,
      RenderGraphRuntimeResourcesManager &runtimeResources,
      const RenderGraphRuntimeCallbacks &callbacks) const;
};

} // namespace rendering
