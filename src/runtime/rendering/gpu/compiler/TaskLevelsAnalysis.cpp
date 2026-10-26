#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/commands/Commands.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <functional>
#include <stack>
#include <vector>

#define RENDER_GRAPH_FATAL(...)                                                                                                                                                                                            \
  do                                                                                                                                                                                                                       \
  {                                                                                                                                                                                                                        \
    os::Logger::errorf(__VA_ARGS__);                                                                                                                                                                                       \
    exit(1);                                                                                                                                                                                                               \
  } while (0)

namespace rendering
{

const char *TaskLevelsAnalysis::name() const
{
  return "analyseTaskLevels";
}

void TaskLevelsAnalysis::run(RenderGraphCompiler &, RenderGraph &renderGraph) const
{
  std::vector<uint32_t> topologicalOrder;

  std::vector<bool> visited(renderGraph.nodes.size(), false);
  std::vector<bool> recStack(renderGraph.nodes.size(), false);
  std::stack<uint32_t> topologicalSort;

  std::function<void(uint32_t)> topologicalSortDFS = [&](uint32_t id)
  {
    if (recStack[id])
    {
      RENDER_GRAPH_FATAL("Cyclical dependency in Task Graph");
    }

    if (visited[id])
    {
      return;
    }

    visited[id] = true;
    recStack[id] = true;

    for (auto &edge : renderGraph.edges[id])
    {
      topologicalSortDFS(edge.taskId);
    }

    recStack[id] = false;
    topologicalSort.push(id);
  };

  for (uint32_t taskId = 0; taskId < renderGraph.nodes.size(); taskId++)
  {
    if (visited[taskId])
    {
      continue;
    }

    topologicalSortDFS(taskId);
  }

  while (!topologicalSort.empty())
  {
    topologicalOrder.push_back(topologicalSort.top());
    topologicalSort.pop();
  }

  for (auto &task : renderGraph.nodes)

    for (uint32_t id : topologicalOrder)
    {
      uint32_t currentLevel = renderGraph.nodes[id].level;
      for (auto &edge : renderGraph.edges[id])
      {
        uint64_t increment = 1;
        renderGraph.nodes[edge.taskId].level = std::max(renderGraph.nodes[edge.taskId].level, currentLevel + increment);
      }
    }

  for (const auto &node : renderGraph.nodes)
  {
    for (auto &cmd : node.commands)
    {
      if (auto *copyBuffer = dynamic_cast<CopyBufferCommand *>(cmd.get()))
      {
        auto srcMeta = renderGraph.resources.bufferMetadatas.find(copyBuffer->src.buffer.name);
        srcMeta->second.firstUsedAt = std::min(srcMeta->second.firstUsedAt, node.level);
        srcMeta->second.lastUsedAt = std::max(srcMeta->second.lastUsedAt, node.level);
        auto dstMeta = renderGraph.resources.bufferMetadatas.find(copyBuffer->dst.buffer.name);
        dstMeta->second.firstUsedAt = std::min(dstMeta->second.firstUsedAt, node.level);
        dstMeta->second.lastUsedAt = std::max(dstMeta->second.lastUsedAt, node.level);
        continue;
      }
      if (auto *bindGroups = dynamic_cast<BindBindingGroupsCommand *>(cmd.get()))
      {
        const auto &groupsMeta = renderGraph.resources.bindingGroupsMetadata.at(bindGroups->groups.name).groupsInfo.groups;
        for (const auto &group : groupsMeta)
        {
          for (const auto &buffer : group.buffers)
          {
            auto meta = renderGraph.resources.bufferMetadatas.find(buffer.bufferView.buffer.name);
            meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
            meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
          }
        }
        continue;
      }
      if (auto *bindVertex = dynamic_cast<BindVertexBufferCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(bindVertex->buffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
        continue;
      }
      if (auto *bindIndex = dynamic_cast<BindIndexBufferCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(bindIndex->buffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
        continue;
      }
      if (auto *drawIndirect = dynamic_cast<DrawIndirectCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(drawIndirect->buffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
        continue;
      }
      if (auto *drawIndirectCount = dynamic_cast<DrawIndirectCountCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(drawIndirectCount->indirectBuffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);

        meta = renderGraph.resources.bufferMetadatas.find(drawIndirectCount->countBuffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);

        continue;
      }
      if (auto *drawIndexedIndirect = dynamic_cast<DrawIndexedIndirectCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(drawIndexedIndirect->buffer.buffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
        continue;
      }
      if (auto *dispatchIndirect = dynamic_cast<DispatchIndirectCommand *>(cmd.get()))
      {
        auto meta = renderGraph.resources.bufferMetadatas.find(dispatchIndirect->indirectBuffer.name);
        meta->second.firstUsedAt = std::min(meta->second.firstUsedAt, node.level);
        meta->second.lastUsedAt = std::max(meta->second.lastUsedAt, node.level);
        continue;
      }
    }
  }
}

} // namespace rendering
