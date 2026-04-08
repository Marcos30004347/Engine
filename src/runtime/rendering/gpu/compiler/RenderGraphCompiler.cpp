#include "RenderGraphCompiler.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "RenderGraphAnalyses.hpp"
#include "os/Logger.hpp"
#include "time/TimeSpan.hpp"
#include <algorithm>

#ifndef RENDER_GRAPH_ENABLE_DETAILED_STATS
#define RENDER_GRAPH_ENABLE_DETAILED_STATS 0
#endif

namespace rendering
{

RenderGraphCompiler::RenderGraphCompiler(RenderGraph &renderGraph) : renderGraph(renderGraph)
{
}

void RenderGraphCompiler::resetCompileState()
{
  for (auto &node : renderGraph.nodes)
  {
    for (const auto &timerBinding : node.timers)
    {
      renderGraph.deleteTimer(timerBinding.timer);
    }
  }

  renderGraph.nodes.clear();
  renderGraph.edges.clear();
  renderGraph.semaphores.clear();

  for (auto &[name, meta] : renderGraph.resources.bufferMetadatas)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.textureMetadatas)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.samplerMetadatas)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.bindingsLayoutMetadata)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.bindingGroupsMetadata)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.graphicsPipelineMetadata)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.computePipelineMetadata)
  {
    meta.usages.clear();
  }
  for (auto &[name, meta] : renderGraph.resources.scratchBuffers)
  {
    meta.usages.clear();
  }
}

void RenderGraphCompiler::buildRuntimeIds()
{
  auto &rg = renderGraph;
  auto &runtimeContext = rg.runtimeContext;

  runtimeContext.clear();

  for (const auto &[name, meta] : rg.resources.bufferMetadatas)
  {
    const uint32_t runtimeId = static_cast<uint32_t>(runtimeContext.buffers.size());
    runtimeContext.bufferNameToRuntimeId[name] = runtimeId;
    runtimeContext.buffers.push_back(BufferRuntimeMetadata{
        .name = name,
        .bufferInfo = meta.bufferInfo,
        .firstUsedAt = meta.firstUsedAt,
        .lastUsedAt = meta.lastUsedAt,
        .usages = meta.usages,
        .resourceId = meta.resourceId,
    });
  }

  for (const auto &[name, meta] : rg.resources.textureMetadatas)
  {
    const uint32_t runtimeId = static_cast<uint32_t>(runtimeContext.textures.size());
    runtimeContext.textureNameToRuntimeId[name] = runtimeId;
    runtimeContext.textures.push_back(TextureRuntimeMetadata{
        .name = name,
        .textureInfo = meta.textureInfo,
        .usages = meta.usages,
        .resourceId = meta.resourceId,
        .overrideLayout = ResourceLayout::UNDEFINED,
    });
  }

  for (const auto &[bgName, bgMeta] : rg.resources.bindingGroupsMetadata)
  {
    const uint32_t bgRuntimeId = static_cast<uint32_t>(runtimeContext.bindingGroups.size());
    runtimeContext.bindingGroupsNameToRuntimeId[bgName] = bgRuntimeId;
    runtimeContext.bindingGroups.push_back(BindingGroupsRuntimeMetadata{
        .name = bgName,
        .groupsInfo = bgMeta.groupsInfo,
        .resourceId = bgMeta.resourceId,
    });

    for (uint32_t gi = 0; gi < static_cast<uint32_t>(bgMeta.groupsInfo.groups.size()); gi++)
    {
      for (uint32_t bi = 0; bi < static_cast<uint32_t>(bgMeta.groupsInfo.groups[gi].buffers.size()); bi++)
      {
        const auto &bufName = bgMeta.groupsInfo.groups[gi].buffers[bi].bufferView.buffer.name;
        auto runtimeIdIt = runtimeContext.bufferNameToRuntimeId.find(bufName);
        if (runtimeIdIt != runtimeContext.bufferNameToRuntimeId.end())
        {
          runtimeContext.bufferToBindingGroupRefs[runtimeIdIt->second].push_back(BindingGroupBufferRef{
              .bgRuntimeId = bgRuntimeId,
              .groupIndex = gi,
              .bindingIndex = bi,
          });
        }
      }

      for (uint32_t ti = 0; ti < static_cast<uint32_t>(bgMeta.groupsInfo.groups[gi].textures.size()); ++ti)
      {
        const auto &texName = bgMeta.groupsInfo.groups[gi].textures[ti].textureView.texture.name;
        auto runtimeIdIt = runtimeContext.textureNameToRuntimeId.find(texName);
        if (runtimeIdIt != runtimeContext.textureNameToRuntimeId.end())
        {
          runtimeContext.textureToBindingGroupRefs[runtimeIdIt->second].push_back(BindingGroupTextureRef{
              .bgRuntimeId = bgRuntimeId,
              .groupIndex = gi,
              .bindingIndex = ti,
              .kind = BindingGroupTextureRefKind::SampledTexture,
          });
        }
      }

      for (uint32_t ti = 0; ti < static_cast<uint32_t>(bgMeta.groupsInfo.groups[gi].storageTextures.size()); ++ti)
      {
        const auto &texName = bgMeta.groupsInfo.groups[gi].storageTextures[ti].textureView.texture.name;
        auto runtimeIdIt = runtimeContext.textureNameToRuntimeId.find(texName);
        if (runtimeIdIt != runtimeContext.textureNameToRuntimeId.end())
        {
          runtimeContext.textureToBindingGroupRefs[runtimeIdIt->second].push_back(BindingGroupTextureRef{
              .bgRuntimeId = bgRuntimeId,
              .groupIndex = gi,
              .bindingIndex = ti,
              .kind = BindingGroupTextureRefKind::StorageTexture,
          });
        }
      }

      for (uint32_t si = 0; si < static_cast<uint32_t>(bgMeta.groupsInfo.groups[gi].samplers.size()); ++si)
      {
        const auto &texName = bgMeta.groupsInfo.groups[gi].samplers[si].view.texture.name;
        auto runtimeIdIt = runtimeContext.textureNameToRuntimeId.find(texName);
        if (runtimeIdIt != runtimeContext.textureNameToRuntimeId.end())
        {
          runtimeContext.textureToBindingGroupRefs[runtimeIdIt->second].push_back(BindingGroupTextureRef{
              .bgRuntimeId = bgRuntimeId,
              .groupIndex = gi,
              .bindingIndex = si,
              .kind = BindingGroupTextureRefKind::SamplerView,
          });
        }
      }
    }
  }
}

void RenderGraphCompiler::allocateNodeTimers()
{
  uint32_t timerCount = 0u;
  for (auto &node : renderGraph.nodes)
  {
    timerCount += static_cast<uint32_t>(node.timers.size());
  }

  if (timerCount == 0u)
  {
    return;
  }

  renderGraph.getRHI()->reserveTimerCapacity(timerCount);
  for (auto &node : renderGraph.nodes)
  {
    for (auto &timerBinding : node.timers)
    {
      timerBinding.timer = renderGraph.createTimer(timerBinding.info);
    }
  }
}

void RenderGraphCompiler::submitRegisteredPasses()
{
  std::vector<Pass *> registeredPassesRuntime;
  for (auto pass : renderGraph.registeredPasses)
  {
    registeredPassesRuntime.push_back(pass.second);
  }

  std::sort(
      registeredPassesRuntime.begin(),
      registeredPassesRuntime.end(),
      [](const Pass *a, const Pass *b)
      {
        return a->index < b->index;
      });

  for (auto pass : registeredPassesRuntime)
  {
    pass->submit();
  }
}

void RenderGraphCompiler::warnUnusedResources() const
{
  for (const auto &[name, meta] : renderGraph.resources.bufferMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Buffer %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : renderGraph.resources.textureMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Buffer %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : renderGraph.resources.samplerMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Sampler %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : renderGraph.resources.bindingsLayoutMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Binding Layout %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : renderGraph.resources.bindingGroupsMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Binding Groups %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : renderGraph.resources.graphicsPipelineMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Graphics Pipeline %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : renderGraph.resources.computePipelineMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Compute Pipeline %s not used in current graph", name.c_str());
    }
  }
}

void RenderGraphCompiler::compile()
{
  resetCompileState();
  buildRuntimeIds();
  submitRegisteredPasses();

  std::vector<std::unique_ptr<RenderGraphAnalysis>> analyses;
  analyses.push_back(std::make_unique<PassesAnalysis>());
  analyses.push_back(std::make_unique<DependencyGraphAnalysis>());
  analyses.push_back(std::make_unique<TaskLevelsAnalysis>());
  analyses.push_back(std::make_unique<AllocationsAnalysis>());
  analyses.push_back(std::make_unique<SemaphoresAnalysis>());

  for (const auto &analysis : analyses)
  {
    lib::time::TimeSpan start = lib::time::TimeSpan::now();
    analysis->run(*this, renderGraph);
    lib::time::TimeSpan end = lib::time::TimeSpan::now();
#if RENDER_GRAPH_ENABLE_DETAILED_STATS
    os::Logger::logf("[RenderGraph] %s time = %fms", analysis->name(), (end - start).milliseconds());
#endif
  }

  allocateNodeTimers();
  warnUnusedResources();
  renderGraph.compiled = true;
}

} // namespace rendering
