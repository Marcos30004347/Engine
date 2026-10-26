#include "graphrewrite/runtime/CpuPageCache.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

CpuPageCache::CpuPageCache(DiskStore &diskStore, CpuPageCacheSettings settings) : disk(diskStore), cacheSettings(std::move(settings))
{
}

void CpuPageCache::configure(std::vector<EdgeTypeId> partitionEdgeTypes)
{
  partitionTypes = std::move(partitionEdgeTypes);
}

uint32_t CpuPageCache::capacityInPages() const
{
  const uint64_t bytesPerPage = sizeof(Edge) * cacheSettings.edgesPerPage;

  if (bytesPerPage == 0u)
  {
    return 1u;
  }

  // Never zero: a cache that can hold nothing cannot make progress, and that
  // is a configuration error rather than a state to handle everywhere else.
  return static_cast<uint32_t>(std::max<uint64_t>(1u, cacheSettings.byteBudget / bytesPerPage));
}

void CpuPageCache::touch(const Key &key)
{
  const auto entry = entries.find(key);

  if (entry == entries.end())
  {
    return;
  }

  recency.erase(entry->second.position);
  recency.push_back(key);
  entry->second.position = std::prev(recency.end());
}

CpuPageCache::Key CpuPageCache::chooseVictim() const
{
  if (usageValues == nullptr)
  {
    return recency.front();
  }

  // Belady: the page whose edge type is needed furthest away goes first. A type
  // never needed again is infinitely far.
  Key best = recency.front();
  uint32_t bestDistance = 0u;
  bool found = false;

  for (const Key &key : recency)
  {
    const EdgeTypeId type = key.partition < partitionTypes.size() ? partitionTypes[key.partition] : 0u;

    if (type >= usageValues->edgeTypeCount || currentInstruction >= usageValues->instructionCount)
    {
      return key;
    }

    const uint32_t next = usageValues->at(currentInstruction, type);

    if (next == compiler::UsageValues::never)
    {
      return key; // nothing will ask for this again
    }

    if (!found || next > bestDistance)
    {
      found = true;
      bestDistance = next;
      best = key;
    }
  }

  return best;
}

bool CpuPageCache::evictUntilWithinBudget(DiagnosticSink &sink)
{
  bool ok = true;

  while (entries.size() > capacityInPages() && !recency.empty())
  {
    const Key victim = chooseVictim();
    const auto entry = entries.find(victim);

    if (entry == entries.end())
    {
      recency.remove_if([&victim](const Key &key) { return !(key < victim) && !(victim < key); });

      continue;
    }

    // A clean page costs nothing to drop: disk already holds it. A dirty one
    // must be written first.
    if (entry->second.dirty)
    {
      ok = disk.writePage(victim.partition, victim.id, entry->second.header, entry->second.edges, sink) && ok;

      ++counters.writebacks;
    }

    recency.erase(entry->second.position);
    entries.erase(entry);

    ++counters.evictions;
  }

  return ok;
}

const std::vector<Edge> *CpuPageCache::get(const uint32_t partition, const PageId id, DiskPageHeader &header, DiagnosticSink &sink)
{
  const Key key{partition, id};

  const auto existing = entries.find(key);

  if (existing != entries.end())
  {
    ++counters.hits;

    touch(key);

    header = existing->second.header;

    return &existing->second.edges;
  }

  ++counters.misses;

  Entry entry;

  if (!disk.readPage(partition, id, entry.header, entry.edges, sink))
  {
    return nullptr;
  }

  recency.push_back(key);

  entry.position = std::prev(recency.end());

  const auto inserted = entries.emplace(key, std::move(entry)).first;

  evictUntilWithinBudget(sink);

  // Eviction may have chosen the entry just inserted when the budget is one
  // page and something else was pinned; re-find rather than holding a stale
  // iterator.
  const auto after = entries.find(key);

  if (after == entries.end())
  {
    return nullptr;
  }

  (void)inserted;

  header = after->second.header;

  return &after->second.edges;
}

void CpuPageCache::update(const uint32_t partition, const PageId id, const DiskPageHeader &header, std::vector<Edge> edges)
{
  const Key key{partition, id};

  const auto existing = entries.find(key);

  if (existing != entries.end())
  {
    existing->second.header = header;
    existing->second.header.pageId = id;
    existing->second.header.count = static_cast<uint32_t>(edges.size());
    existing->second.edges = std::move(edges);
    existing->second.dirty = true;

    touch(key);

    return;
  }

  Entry entry;

  entry.header = header;
  entry.header.pageId = id;
  entry.header.count = static_cast<uint32_t>(edges.size());
  entry.edges = std::move(edges);
  entry.dirty = true;

  recency.push_back(key);
  entry.position = std::prev(recency.end());

  entries.emplace(key, std::move(entry));

  DiagnosticSink scratch;

  evictUntilWithinBudget(scratch);
}

bool CpuPageCache::contains(const uint32_t partition, const PageId id) const
{
  return entries.count(Key{partition, id}) != 0u;
}

bool CpuPageCache::isDirty(const uint32_t partition, const PageId id) const
{
  const auto entry = entries.find(Key{partition, id});

  return entry != entries.end() && entry->second.dirty;
}

bool CpuPageCache::flush(DiagnosticSink &sink)
{
  bool ok = true;

  for (auto &entry : entries)
  {
    if (!entry.second.dirty)
    {
      continue;
    }

    ok = disk.writePage(entry.first.partition, entry.first.id, entry.second.header, entry.second.edges, sink) && ok;

    entry.second.dirty = false;

    ++counters.writebacks;
  }

  return ok;
}

void CpuPageCache::setEvictionOrder(const compiler::UsageValues *usage, const uint32_t instruction)
{
  usageValues = usage;
  currentInstruction = instruction;
}

uint32_t CpuPageCache::entryCount() const
{
  return static_cast<uint32_t>(entries.size());
}

const CpuPageCache::Stats &CpuPageCache::stats() const
{
  return counters;
}

void CpuPageCache::resetStats()
{
  counters = Stats();
}

} // namespace graphrewrite::runtime
