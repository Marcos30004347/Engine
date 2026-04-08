#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include <unordered_set>

namespace rendering
{

struct SemaphoreHash
{
  std::size_t operator()(const Semaphore &s) const noexcept
  {
    std::size_t h = 0;
    auto hc = [](auto seed, auto v)
    {
      seed ^= std::hash<decltype(v)>{}(v) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
      return seed;
    };
    h = hc(h, static_cast<int>(s.signalQueue));
    h = hc(h, static_cast<int>(s.waitQueue));
    h = hc(h, s.signalTask);
    h = hc(h, s.waitTask);
    return h;
  }
};

struct SemaphoreEq
{
  bool operator()(const Semaphore &a, const Semaphore &b) const noexcept
  {
    return a.signalQueue == b.signalQueue && a.waitQueue == b.waitQueue && a.signalTask == b.signalTask && a.waitTask == b.waitTask;
  }
};

const char *SemaphoresAnalysis::name() const
{
  return "analyseSemaphores";
}

void SemaphoresAnalysis::run(RenderGraphCompiler &, RenderGraph &renderGraph) const
{
  uint32_t fromTask = 0;
  std::unordered_set<Semaphore, SemaphoreHash, SemaphoreEq> semaphoresSet;

  for (auto &taskEdges : renderGraph.edges)
  {
    for (auto &edge : taskEdges)
    {
      uint32_t toTask = edge.taskId;

      semaphoresSet.insert(
          Semaphore{
            .signalQueue = renderGraph.nodes[fromTask].queue,
            .waitQueue = renderGraph.nodes[toTask].queue,
            .signalTask = fromTask,
            .waitTask = toTask,
          });
    }

    fromTask += 1;
  }

  uint32_t at = 0;

  for (const auto &semaphore : semaphoresSet)
  {
    renderGraph.semaphores.push_back(semaphore);
    renderGraph.nodes[semaphore.signalTask].signalSemaphores.push_back(at);
    renderGraph.nodes[semaphore.waitTask].waitSemaphores.push_back(at);
    at++;
  }
}

} // namespace rendering
