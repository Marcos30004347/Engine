#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "datastructure/BoundedTaggedRectTreap.hpp"
#include "datastructure/TaggedInternvalTree.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>

#ifndef RENDER_GRAPH_ENABLE_DETAILED_STATS
#define RENDER_GRAPH_ENABLE_DETAILED_STATS 0
#endif

namespace rendering
{

namespace
{

constexpr uint64_t InvalidNodeId = static_cast<uint64_t>(-1);

} // namespace

struct AccessConsumerPair
{
  AccessPattern access;
  uint64_t consumer;
  Queue queue;
  bool operator==(const AccessConsumerPair &o) const
  {
    return access == o.access && consumer == o.consumer && o.queue == queue;
  }
  bool operator!=(const AccessConsumerPair &o) const
  {
    return access != o.access || consumer != o.consumer || o.queue != queue;
  }
};

struct AccessLayoutConsumerTriple
{
  AccessPattern access;
  ResourceLayout layout;
  uint64_t consumer;
  Queue queue;

  bool operator==(const AccessLayoutConsumerTriple &o) const
  {
    return access == o.access && layout == o.layout && queue == o.queue;
  }

  bool operator!=(const AccessLayoutConsumerTriple &o) const
  {
    return access != o.access || layout != o.layout || queue != o.queue;
  }
};

const char *DependencyGraphAnalysis::name() const
{
  return "analyseDependencyGraph";
}

void DependencyGraphAnalysis::run(RenderGraphCompiler &, RenderGraph &renderGraph) const
{
  auto &nodes = renderGraph.nodes;
  auto &edges = renderGraph.edges;
  auto &resources = renderGraph.resources;
  auto &runtimeContext = renderGraph.runtimeContext;

  edges.clear();
  edges.resize(nodes.size());

  std::vector<std::unordered_set<uint64_t>> uniqueEdges(nodes.size());

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [&](BufferResourceUsage taskA, BufferResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t>::Interval> intervals;
    intervals.reserve(4 * meta.usages.size());

    lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t> bufferIntevals(meta.usages.size() * 4);

    bufferIntevals.insert(
        0,
        meta.bufferInfo.size - 1,
        AccessConsumerPair{
          .access = AccessPattern::NONE,
          .consumer = (uint64_t)-1,
          .queue = Queue::None,
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      bufferIntevals.queryAll(usage.view.offset, usage.view.offset + usage.view.size - 1, intervals);

      for (const auto &interval : intervals)
      {
        const BufferBarrier transition = {
          .runtimeId = runtimeContext.bufferNameToRuntimeId.at(meta.bufferInfo.name),
          .offset = interval.start,
          .size = interval.end - interval.start + 1,
          .fromAccess = interval.tag.access,
          .toAccess = usage.view.access,
          .toLevel = nodes[usage.consumer].level,
          .fromQueue = interval.tag.queue,
          .toQueue = nodes[usage.consumer].queue,
          .fromNode = interval.tag.consumer,
          .phase = BufferBarrier::Phase::Normal,
        };

        if (transition.toQueue != transition.fromQueue && transition.fromNode != InvalidNodeId)
        {
          auto releaseTransition = transition;
          releaseTransition.phase = BufferBarrier::Phase::QueueTransferRelease;
          nodes[transition.fromNode].postBufferTransitions.emplace_back(releaseTransition);

          auto acquireTransition = transition;
          acquireTransition.phase = BufferBarrier::Phase::QueueTransferAcquire;
          nodes[usage.consumer].preBufferTransitions.emplace_back(acquireTransition);
        }
        else
        {
          nodes[usage.consumer].preBufferTransitions.emplace_back(transition);
        }

        if (interval.tag.consumer == usage.consumer)
        {
          continue;
        }

        if (interval.tag.consumer != -1)
        {
          if (uniqueEdges[interval.tag.consumer].insert(usage.consumer).second)
          {
            EdgeType edgeType = (interval.tag.access != usage.view.access || interval.tag.queue != usage.queue) ? EdgeType::ResourceDependency : EdgeType::ResourceShare;

            edges[interval.tag.consumer].emplace_back(
                RenderGraphEdge{
                  .taskId = usage.consumer,
                  .type = edgeType,
                });

#if RENDER_GRAPH_ENABLE_DETAILED_STATS
            os::Logger::logf("[RenderGraph] Adding edge from %s to %s", nodes[interval.tag.consumer].name.c_str(), nodes[usage.consumer].name.c_str());
#endif
          }
        }

        bufferIntevals.remove(interval.start, interval.end, interval.tag);
        bufferIntevals.insert(
            interval.start,
            interval.end,
            AccessConsumerPair{
              .access = usage.view.access,
              .consumer = usage.consumer,
              .queue = usage.queue,
            });
      }
    }
  }

  for (auto [name, meta] : resources.textureMetadatas)
  {
    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [&](TextureResourceUsage taskA, TextureResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t>::Rect> intervals;
    intervals.reserve(resources.textureMetadatas.size() * 4);
    lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t> textureState(meta.usages.size() * 4);

    textureState.insert(
        0,
        0,
        meta.textureInfo.mipLevels,
        std::max(meta.textureInfo.arrayLayers, meta.textureInfo.depth),
        AccessLayoutConsumerTriple{
          .access = AccessPattern::NONE,
          .layout = ResourceLayout::UNDEFINED,
          .consumer = (uint64_t)-1,
          .queue = Queue::None,
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      textureState.queryAll(usage.view.baseMipLevel, usage.view.baseArrayLayer, usage.view.baseMipLevel + usage.view.levelCount - 1, usage.view.baseArrayLayer + usage.view.layerCount - 1, intervals);

      auto currentTag = AccessLayoutConsumerTriple{
        .access = usage.view.access,
        .layout = usage.view.layout,
        .consumer = usage.consumer,
        .queue = nodes[usage.consumer].queue,
      };

      for (const auto &interval : intervals)
      {
        const TextureBarrier transition = {
          .runtimeId = runtimeContext.textureNameToRuntimeId.at(meta.textureInfo.name),
          .format = meta.textureInfo.format,
          .toLevel = nodes[usage.consumer].level,
          .baseMip = interval.x1,
          .mipCount = interval.x2 - interval.x1 + 1,
          .baseLayer = interval.y1,
          .layerCount = interval.y2 - interval.y1 + 1,
          .fromAccess = interval.tag.access,
          .toAccess = usage.view.access,
          .fromLayout = interval.tag.layout,
          .toLayout = usage.view.layout,
          .fromQueue = interval.tag.queue,
          .toQueue = nodes[usage.consumer].queue,
          .fromNode = interval.tag.consumer,
          .phase = TextureBarrier::Phase::Normal,
        };

        if (transition.toQueue != transition.fromQueue && transition.fromNode != InvalidNodeId)
        {
          auto releaseTransition = transition;
          releaseTransition.phase = TextureBarrier::Phase::QueueTransferRelease;
          nodes[transition.fromNode].postTextureTransitions.emplace_back(releaseTransition);

          auto acquireTransition = transition;
          acquireTransition.phase = TextureBarrier::Phase::QueueTransferAcquire;
          nodes[usage.consumer].preTextureTransitions.emplace_back(acquireTransition);
        }
        else
        {
          nodes[usage.consumer].preTextureTransitions.emplace_back(transition);
        }

        if (interval.tag.consumer == usage.consumer)
        {
          continue;
        }

        if (interval.tag.consumer != -1)
        {
          if (uniqueEdges[interval.tag.consumer].insert(usage.consumer).second)
          {
            EdgeType edgeType = (interval.tag.access != currentTag.access || interval.tag.layout != currentTag.layout || interval.tag.queue != currentTag.queue) ? EdgeType::ResourceDependency : EdgeType::ResourceShare;

            edges[interval.tag.consumer].emplace_back(
                RenderGraphEdge{
                  .taskId = usage.consumer,
                  .type = edgeType,
                });
          }
        }

        textureState.remove(interval.x1, interval.y1, interval.x2, interval.y2, interval.tag);
        textureState.insert(interval.x1, interval.y1, interval.x2, interval.y2, currentTag);
      }
    }
  }
}

} // namespace rendering
