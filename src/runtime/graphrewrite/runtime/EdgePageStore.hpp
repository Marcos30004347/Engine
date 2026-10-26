#pragma once

// Pages, and the directory that orders them.
//
// Page IDENTITY and page ORDER are deliberately separate. A PageId is stable for
// the life of a page and (once the disk layer arrives) names a fixed byte offset
// in a fixed file; the key ordering lives in a directory array beside it. A
// split therefore allocates a new PageId, writes it wherever there is room, and
// inserts one entry into the directory - no page's id moves and no file is
// rewritten. Had position been the identity, every split would have shifted the
// tail of a partition.

#include "graphrewrite/runtime/Edge.hpp"
#include "graphrewrite/runtime/PartitionDirectory.hpp"

#include <string>
#include <vector>

namespace graphrewrite::runtime {

// One edge type in one key order.
class Partition
{
public:
  Partition(EdgeTypeId type, KeyOrder order, uint32_t partitionId);

  EdgeTypeId edgeType() const;
  KeyOrder order() const;
  uint32_t id() const;

  // Sorts, chunks into pages and fills the directory.
  void build(std::vector<Edge> edges);

  uint32_t pageCount() const;

  // Addressed by DIRECTORY POSITION, which is what a key search returns.
  PageId pageIdAt(uint32_t position) const;
  const std::vector<Edge> &pageAt(uint32_t position) const;
  uint32_t minKeyAt(uint32_t position) const;
  uint32_t maxKeyAt(uint32_t position) const;

  // The pages that may hold `key`, as a contiguous run of directory positions.
  PageRun rangeFor(uint32_t key) const;

  const PartitionDirectory &directoryView() const;

  // Addressed by PageId, which is what the disk and residency layers use.
  const std::vector<Edge> &pageById(PageId id) const;
  uint32_t positionOf(PageId id) const;

  // Inserts an edge into the page whose range covers it, splitting on overflow.
  // Returns the directory position touched.
  uint32_t insert(const Edge &edge);

  // Removes one copy of an edge. Returns false when it was not there.
  bool remove(const Edge &edge);

  // Is a copy of this edge present? Asked before a probabilistic rewrite fires,
  // to tell a match that is still valid from one whose edges an earlier rewrite
  // already took.
  bool contains(const Edge &edge) const;

  // Recomputes a page's key range from its contents. Removing a page's lowest
  // edge moves its minKey, and a directory that still claims the old range stops
  // returning that page for keys it still holds.
  void refreshRange(uint32_t position);

  // Replaces a page's contents wholesale, as after a GPU compaction, and brings
  // its key range with it. A range left stale here stops the page being found
  // for keys it still holds, and nothing reports that.
  void replacePage(uint32_t position, std::vector<Edge> contents);

  std::vector<Edge> snapshot() const;

  // Rebuilds a partition from what a manifest and its segment files held.
  void restore(std::vector<std::vector<Edge>> pagesById, PartitionDirectory directory);

  const std::vector<std::vector<Edge>> &allPages() const;

  bool checkInvariants(std::string &problem) const;

private:
  uint32_t splitPage(uint32_t position);
  PageId allocatePageId();

  EdgeTypeId type;
  KeyOrder keyOrder;
  uint32_t partitionId = 0u;

  std::vector<std::vector<Edge>> pages; // indexed by PageId
  std::vector<PageId> freeList;

  // The directory: position -> PageId, ordered by key.
  PartitionDirectory directory;
};

// All partitions of a graph, plus the vertex allocator.
class EdgePageStore
{
public:
  // Builds exactly the partitions the programme traverses - one per (edge type,
  // direction actually used).
  void configure(const compiler::EdgeTypeTable &types);

  uint32_t partitionCount() const;

  bool has(EdgeTypeId type, KeyOrder order) const;
  Partition &partition(EdgeTypeId type, KeyOrder order);
  const Partition &partition(EdgeTypeId type, KeyOrder order) const;

  Partition &partitionAt(uint32_t index);
  const Partition &partitionAt(uint32_t index) const;

  // Adds an edge to every partition that indexes its type.
  void addEdge(EdgeTypeId type, const Edge &edge);
  bool removeEdge(EdgeTypeId type, const Edge &edge);

  // True when either ordering of this type still holds the edge.
  bool containsEdge(EdgeTypeId type, const Edge &edge) const;

  // How many edges of a type the graph holds. This is the statistic a plan is
  // ordered by, so it has to be cheap and available at runtime.
  uint32_t edgeCount(EdgeTypeId type) const;

  std::vector<uint32_t> edgeCounts(uint32_t typeCount) const;

  uint32_t allocateVertex();
  uint32_t vertexCount() const;
  void reserveVertices(uint32_t count);

  // Ids at or above the ceiling are VALUES - a day, a place in a table - not
  // vertices this store hands out. Adding an edge to one must not push the
  // allocator past it, or every node created afterwards is born in the value
  // range and named like one.
  void setVertexCeiling(uint32_t ceiling);

private:
  int32_t indexOf(EdgeTypeId type, KeyOrder order) const;

  std::vector<Partition> partitions;
  std::vector<int32_t> forwardIndex;  // edge type -> partition index, or -1
  std::vector<int32_t> backwardIndex;

  uint32_t nextVertex = 0u;

  uint32_t vertexCeiling = ~uint32_t(0);
};

} // namespace graphrewrite::runtime
