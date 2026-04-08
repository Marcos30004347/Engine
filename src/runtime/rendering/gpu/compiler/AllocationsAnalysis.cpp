#include "RenderGraphAnalyses.hpp"

#include "rendering/gpu/RenderGraph.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RENDER_GRAPH_ENABLE_DETAILED_STATS
#define RENDER_GRAPH_ENABLE_DETAILED_STATS 0
#endif

namespace rendering
{

struct Request
{
  std::string id;
  uint64_t size;
  uint64_t start;
  uint64_t end;
};

static std::string bufferUsageToString(int usage)
{
  if (usage == BufferUsage_None)
    return "None";

  std::ostringstream oss;
  bool first = true;

  auto addFlag = [&](int flag, const char *name)
  {
    if (usage & flag)
    {
      if (!first)
        oss << " | ";
      oss << name;
      first = false;
    }
  };

  addFlag(BufferUsage_Uniform, "Uniform");
  addFlag(BufferUsage_Storage, "Storage");
  addFlag(BufferUsage_Push, "Push");
  addFlag(BufferUsage_Pull, "Pull");
  addFlag(BufferUsage_Vertex, "Vertex");
  addFlag(BufferUsage_Indirect, "Indirect");
  addFlag(BufferUsage_Timestamp, "Timestamp");
  addFlag(BufferUsage_Index, "Index");

  return oss.str();
}

struct BufferSlice
{
  std::string bufferId;
  size_t offset;
  size_t size;
};

inline size_t alignUp(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

std::pair<std::map<std::string, BufferSlice>, size_t> allocateBuffersGraphColoring(std::vector<Request> &requests, size_t alignment)
{
  std::sort(
      requests.begin(),
      requests.end(),
      [](const Request &a, const Request &b)
      {
        return a.start < b.start;
      });

  struct ColorSlot
  {
    uint64_t offsetBase;
    uint64_t currentSize;
    int lastEnd;
  };

  std::vector<ColorSlot> colors;
  std::map<std::string, BufferSlice> allocations;

  for (size_t i = 0; i < requests.size(); ++i)
  {
    const Request &req = requests[i];

    int chosenColor = -1;

    for (size_t c = 0; c < colors.size(); ++c)
    {
      if (req.start > colors[c].lastEnd)
      {
        chosenColor = static_cast<int>(c);
        break;
      }
    }

    if (chosenColor == -1)
    {
      size_t offsetBase = colors.empty() ? 0 : alignUp(colors.back().offsetBase + colors.back().currentSize, alignment);
      colors.push_back(ColorSlot{offsetBase, 0, -1});
      chosenColor = static_cast<int>(colors.size() - 1);
    }

    ColorSlot &slot = colors[chosenColor];
    slot.currentSize = std::max(slot.currentSize, req.size);

    slot.lastEnd = req.end;

    allocations[req.id] = BufferSlice{
      requests[i].id,
      slot.offsetBase,
      req.size,
    };
  }

  size_t totalSize = 0;
  for (size_t i = 0; i < colors.size(); ++i)
  {
    totalSize = alignUp(totalSize, alignment);
    totalSize += colors[i].currentSize;
  }

  return std::make_pair(allocations, totalSize);
}

const char *AllocationsAnalysis::name() const
{
  return "analyseAllocations";
}

void AllocationsAnalysis::run(RenderGraphCompiler &, RenderGraph &renderGraph) const
{
  auto &resources = renderGraph.resources;
  resources.scratchBuffers.clear();

  std::unordered_map<BufferUsage, std::vector<Request>> memoryRequests;

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    if (meta.bufferInfo.scratch && !meta.usages.empty())
    {
      const BufferInfo &info = meta.bufferInfo;

      memoryRequests[info.usage].push_back(
          Request{
            .id = meta.bufferInfo.name,
            .start = meta.firstUsedAt,
            .end = meta.lastUsedAt,
            .size = info.size,
          });
    }
  }

  resources.scratchMap.clear();

  for (auto &[usage, requests] : memoryRequests)
  {
    auto [allocations, totalSize] = allocateBuffersGraphColoring(requests, 16);

    BufferInfo info;
    info.name = bufferUsageToString(usage) + ".buffer";
    info.size = totalSize;
    info.usage = usage;

    auto metadata = BufferResourceMetadata{
      .bufferInfo = info,
    };

    resources.scratchBuffers[usage] = metadata;

#if RENDER_GRAPH_ENABLE_DETAILED_STATS
    os::Logger::logf("[RenderGraph] Reserving %u bytes for %s", info.size, info.name.c_str());
#endif

    for (auto &allocation : allocations)
    {
      auto &scratch = resources.scratchMap[allocation.second.bufferId];

      scratch.usage = usage;
      scratch.offset = allocation.second.offset;
      scratch.size = allocation.second.size;

#if RENDER_GRAPH_ENABLE_DETAILED_STATS
      os::Logger::logf(
          "[RenderGraph] Reserving slice of %s, offset = %u, size = %u, for %s",
          info.name.c_str(),
          allocation.second.offset,
          allocation.second.size,
          resources.bufferMetadatas.at(allocation.second.bufferId).bufferInfo.name.c_str());
#endif
    }
  }
}

} // namespace rendering
