#pragma once

#include <memory>
#include <vector>

namespace rendering
{

class RenderGraph;

class RenderGraphCompiler
{
public:
  explicit RenderGraphCompiler(RenderGraph &renderGraph);

  void compile();

private:
  RenderGraph &renderGraph;

  void resetCompileState();
  void buildRuntimeIds();
  void submitRegisteredPasses();
  void allocateNodeTimers();
  void warnUnusedResources() const;
};

} // namespace rendering
