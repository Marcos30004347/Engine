#pragma once

namespace rendering
{

class RenderGraph;
class RenderGraphCompiler;

class RenderGraphAnalysis
{
public:
  virtual ~RenderGraphAnalysis() = default;
  virtual const char *name() const = 0;
  virtual void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const = 0;
};

} // namespace rendering
