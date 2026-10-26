#pragma once

// The key-ordered directory of a partition, and the search over it.
//
// Two things here are easy to get wrong and both are silent when they are:
//
//   - The search must return a RUN of pages, not one page. Chunking a sorted
//     edge list at a fixed page size leaves a high-degree vertex straddling a
//     boundary, so two adjacent pages share that key. A search that returns a
//     single page drops the other one's matches and nothing reports it.
//
//   - The bounds must be signed. `high = mid - 1` at mid == 0 underflows an
//     unsigned counter and walks off the front of the array.

#include "graphrewrite/runtime/Edge.hpp"

#include <vector>

namespace graphrewrite::runtime {

struct PartitionDirectory
{
  std::vector<PageId> order;    // directory position -> PageId, ordered by key
  std::vector<uint32_t> minKey; // parallel to `order`
  std::vector<uint32_t> maxKey;

  uint32_t size() const
  {
    return static_cast<uint32_t>(order.size());
  }
};

PageRun directoryRange(const PartitionDirectory &directory, uint32_t key);

} // namespace graphrewrite::runtime
