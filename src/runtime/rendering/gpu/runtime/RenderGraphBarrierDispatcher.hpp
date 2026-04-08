#pragma once

#include "RenderGraphBufferBarrierDispatcher.hpp"
#include "RenderGraphTextureBarrierDispatcher.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{

class RenderGraphRuntimeResourcesManager;

class RenderGraphBarrierDispatcher
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

private:
  RenderGraphBufferBarrierDispatcher bufferDispatcher;
  RenderGraphTextureBarrierDispatcher textureDispatcher;
};

} // namespace rendering
