#pragma once

// On-disk layout of a page segment.
//
// Fixed-size records, so a PageId resolves to a byte offset by arithmetic and
// there is no index to keep consistent:
//
//   segment = pageId / pagesPerSegment
//   slot    = pageId % pagesPerSegment
//
// Page headers are grouped AHEAD of the edge blocks on purpose. `count` changes
// on every insert, and a count-only update then touches one 16-byte record
// rather than the page's whole 2 KiB of edges.

#include "graphrewrite/runtime/Edge.hpp"

#include <cstdint>

namespace graphrewrite::runtime {

constexpr uint32_t segmentMagic = 0x53455247u; // "GRES"
constexpr uint32_t segmentVersion = 1u;
constexpr PageId freePageSlot = ~PageId(0u);

struct SegmentHeader
{
  uint32_t magic = segmentMagic;
  uint32_t version = segmentVersion;
  uint32_t partition = 0u;
  uint32_t segmentIndex = 0u;
  uint32_t pagesPerSegment = 0u;
  uint32_t pageEdges = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct DiskPageHeader
{
  PageId pageId = freePageSlot;
  uint32_t count = 0u;
  uint32_t minKey = 0u;
  uint32_t maxKey = 0u;
};

constexpr uint32_t segmentAlignment = 64u;

constexpr uint32_t alignUpTo(const uint32_t value, const uint32_t alignment)
{
  return ((value + alignment - 1u) / alignment) * alignment;
}

struct SegmentLayout
{
  uint32_t pagesPerSegment = 0u;
  uint32_t edgesPerPage = 0u;

  uint32_t headersBase() const
  {
    return alignUpTo(static_cast<uint32_t>(sizeof(SegmentHeader)), segmentAlignment);
  }

  uint32_t edgesBase() const
  {
    return alignUpTo(headersBase() + static_cast<uint32_t>(sizeof(DiskPageHeader)) * pagesPerSegment, segmentAlignment);
  }

  uint32_t headerOffset(const uint32_t slot) const
  {
    return headersBase() + static_cast<uint32_t>(sizeof(DiskPageHeader)) * slot;
  }

  uint32_t edgesOffset(const uint32_t slot) const
  {
    return edgesBase() + static_cast<uint32_t>(sizeof(Edge)) * edgesPerPage * slot;
  }

  uint32_t totalSize() const
  {
    return edgesBase() + static_cast<uint32_t>(sizeof(Edge)) * edgesPerPage * pagesPerSegment;
  }
};

} // namespace graphrewrite::runtime
