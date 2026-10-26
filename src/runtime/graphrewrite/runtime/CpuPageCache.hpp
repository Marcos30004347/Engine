#pragma once

// The warm tier: a bounded cache of pages over the disk store.
//
// The behaviour that matters most here is the dirty path. A page changed in
// memory and then evicted must be written back BEFORE its slot is reused, or
// the next fetch quietly serves the stale on-disk copy and the graph loses
// edits with no error anywhere.

#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/runtime/DiskStore.hpp"

#include <cstdint>
#include <list>
#include <map>
#include <vector>

namespace graphrewrite::runtime {

struct CpuPageCacheSettings
{
  // Stated in bytes rather than entries, the way VRAM budgets are. Pages are
  // uniform, so it converts, but the units people reason about are bytes.
  uint64_t byteBudget = 256ull * 1024ull * 1024ull;
  uint32_t edgesPerPage = pageEdges;
};

class CpuPageCache
{
public:
  struct Stats
  {
    uint64_t hits = 0u;
    uint64_t misses = 0u;
    uint64_t evictions = 0u;
    uint64_t writebacks = 0u;
  };

  CpuPageCache(DiskStore &disk, CpuPageCacheSettings settings);

  // Which edge type each partition indexes, so eviction can consult the
  // compiler's usage values.
  void configure(std::vector<EdgeTypeId> partitionEdgeTypes);

  // Fetches on miss. Returns nullptr and reports if the page cannot be read.
  const std::vector<Edge> *get(uint32_t partition, PageId id, DiskPageHeader &header, DiagnosticSink &sink);

  // Inserts or replaces a page's contents and marks it dirty.
  void update(uint32_t partition, PageId id, const DiskPageHeader &header, std::vector<Edge> edges);

  bool contains(uint32_t partition, PageId id) const;
  bool isDirty(uint32_t partition, PageId id) const;

  // Writes every dirty page back. This is what a step commit calls.
  bool flush(DiagnosticSink &sink);

  // When set, eviction ranks by how far away each page's edge type is next
  // needed - Belady, for a straight-line programme.
  void setEvictionOrder(const compiler::UsageValues *usage, uint32_t instruction);

  uint32_t entryCount() const;
  uint32_t capacityInPages() const;

  const Stats &stats() const;
  void resetStats();

private:
  struct Key
  {
    uint32_t partition;
    PageId id;

    bool operator<(const Key &other) const
    {
      return partition != other.partition ? partition < other.partition : id < other.id;
    }
  };

  struct Entry
  {
    DiskPageHeader header;
    std::vector<Edge> edges;
    bool dirty = false;
    std::list<Key>::iterator position;
  };

  void touch(const Key &key);
  bool evictUntilWithinBudget(DiagnosticSink &sink);
  Key chooseVictim() const;

  DiskStore &disk;
  CpuPageCacheSettings cacheSettings;

  std::map<Key, Entry> entries;
  std::list<Key> recency; // front = least recently used

  std::vector<EdgeTypeId> partitionTypes;

  const compiler::UsageValues *usageValues = nullptr;
  uint32_t currentInstruction = 0u;

  Stats counters;
};

} // namespace graphrewrite::runtime
