#pragma once

// Edge storage primitives.
//
// A page holds NOTHING but the two node ids. The label is implicit in which
// partition the page belongs to, so it is not stored per edge - at 256 edges a
// page that is 2 KiB instead of 3 KiB is a third less traffic on every upload,
// eviction and writeback.

#include "graphrewrite/compiler/EdgeTypeTable.hpp"

#include <cstdint>

namespace graphrewrite::runtime {

using compiler::EdgeTypeId;

struct Edge
{
  uint32_t origin = 0u;
  uint32_t target = 0u;

  bool operator==(const Edge &other) const
  {
    return origin == other.origin && target == other.target;
  }

  bool operator<(const Edge &other) const
  {
    return origin != other.origin ? origin < other.origin : target < other.target;
  }
};

constexpr uint32_t pageEdges = 256u;

using PageId = uint32_t;

constexpr PageId invalidPage = ~PageId(0u);
constexpr uint32_t notResident = ~uint32_t(0u);

// Which key a partition is ordered by. A forward match probes by origin, a
// backward one by target; the compiler says which directions a type is actually
// traversed in, so a type only ever walked forward costs one partition.
enum class KeyOrder
{
  ByOrigin,
  ByTarget,
};

inline uint32_t keyOf(const Edge &edge, const KeyOrder order)
{
  return order == KeyOrder::ByOrigin ? edge.origin : edge.target;
}

inline uint32_t otherOf(const Edge &edge, const KeyOrder order)
{
  return order == KeyOrder::ByOrigin ? edge.target : edge.origin;
}

// Per-page state. The GPU reads this for every page, resident or not - that is
// what lets a shader decide for itself that a page is missing and ask for it.
struct PageMeta
{
  uint32_t partition = 0u;
  uint32_t minKey = 0u;
  uint32_t maxKey = 0u;
  uint32_t count = 0u;
  uint32_t slot = notResident;
  uint32_t flags = 0u;
};

enum PageFlags : uint32_t
{
  PageFlagNone = 0u,
  PageFlagDirty = 1u << 0u,
  PageFlagPendingSort = 1u << 1u,
};

// A contiguous run of directory positions. The search returns a RUN rather than
// a single page because fixed-size chunking of a sorted edge list leaves a
// high-degree vertex straddling a page boundary, so two adjacent pages share
// that key.
struct PageRun
{
  uint32_t first = 1u;
  uint32_t last = 0u;

  bool empty() const
  {
    return last < first;
  }

  uint32_t size() const
  {
    return empty() ? 0u : last - first + 1u;
  }
};

} // namespace graphrewrite::runtime
