#pragma once

#include "RenderGraphAnalysis.hpp"

namespace rendering
{

class PassesAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

class DependencyGraphAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

class TaskLevelsAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

class AllocationsAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

class SemaphoresAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

class CommandBuffersAnalysis final : public RenderGraphAnalysis
{
public:
  const char *name() const override;
  void run(RenderGraphCompiler &compiler, RenderGraph &renderGraph) const override;
};

} // namespace rendering
