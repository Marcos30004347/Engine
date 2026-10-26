#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{

const char *CommandBuffersAnalysis::name() const
{
  return "analyseCommandBuffers";
}

void CommandBuffersAnalysis::run(RenderGraphCompiler &, RenderGraph &) const
{
  // Command buffers are now created per-node while recording runtime work,
  // so there is no compile-time command-buffer assignment step anymore.
}

} // namespace rendering
