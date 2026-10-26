#include "graphrewrite/runtime/StreamingManager.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

StreamingManager::StreamingManager(GpuPagePool &pagePool, EdgePageStore &pageStore, const compiler::UsageValues &usageValues)
    : pool(pagePool), store(pageStore), usage(usageValues)
{
}

void StreamingManager::configure(std::vector<EdgeTypeId> partitionEdgeTypes)
{
  partitionTypes = std::move(partitionEdgeTypes);

  // Called again after the pool rebuilds its metadata, which drops residency
  // wholesale: a split renumbers positions, so every metadata index we were
  // holding now means something else. Forget the lot rather than remap it.
  resident.clear();
  pending.clear();
  residentAt.assign(residentAt.size(), notResident);
  requestedFlag.assign(requestedFlag.size(), 0u);
}

void StreamingManager::request(const uint32_t partition, const uint32_t position)
{
  requestByMetaIndex(pool.metaIndex(partition, position));
}

void StreamingManager::requestByMetaIndex(const uint32_t metaIndex)
{
  if (metaIndex == notResident || metaIndex >= pool.totalPages())
  {
    return;
  }

  if (std::find(pending.begin(), pending.end(), metaIndex) == pending.end())
  {
    pending.push_back(metaIndex);
  }
}

uint32_t StreamingManager::nextUseOf(const uint32_t metaIndex, const uint32_t instruction) const
{
  const uint32_t partition = pool.metadata()[metaIndex].partition;

  if (partition >= partitionTypes.size())
  {
    return compiler::UsageValues::never;
  }

  const EdgeTypeId type = partitionTypes[partition];

  if (type >= usage.edgeTypeCount || instruction >= usage.instructionCount)
  {
    return compiler::UsageValues::never;
  }

  return usage.at(instruction, type);
}

void StreamingManager::resizeBookkeeping()
{
  const uint32_t capacity = pool.metadataCapacity();

  if (residentAt.size() == capacity)
  {
    return;
  }

  residentAt.assign(capacity, notResident);
  requestedFlag.assign(capacity, 0u);

  for (uint32_t index = 0u; index < resident.size(); ++index)
  {
    residentAt[resident[index]] = index;
  }
}

void StreamingManager::buildEvictionOrder(const uint32_t instruction)
{
  // Belady, by counting sort. The key is the next-use distance, which is an
  // instruction index and therefore already a small dense integer - so ordering
  // a million resident pages is two linear passes over buckets, not an
  // O(n log n) comparison sort run once per tick.
  //
  // Key 0 is "a row is asking for this page right now", which must be evicted
  // last. Above that, a larger key means the page's edge type is needed further
  // away, and `never` - nothing reads this type again - is the largest of all,
  // so pages the program is finished with go first.
  const uint32_t horizon = usage.instructionCount + 1u;
  const uint32_t keyCount = horizon + 2u;

  evictionBuckets.assign(keyCount + 1u, 0u);

  const auto keyOf = [this, instruction, horizon](const uint32_t metaIndex) {
    if (requestedFlag[metaIndex] != 0u)
    {
      return 0u;
    }

    const uint32_t distance = nextUseOf(metaIndex, instruction);

    return 1u + (distance >= horizon ? horizon : distance);
  };

  for (const uint32_t metaIndex : resident)
  {
    ++evictionBuckets[keyOf(metaIndex)];
  }

  uint32_t running = 0u;

  for (uint32_t key = 0u; key < keyCount; ++key)
  {
    const uint32_t count = evictionBuckets[key];

    evictionBuckets[key] = running;
    running += count;
  }

  evictionOrder.assign(resident.size(), notResident);

  for (const uint32_t metaIndex : resident)
  {
    evictionOrder[evictionBuckets[keyOf(metaIndex)]++] = metaIndex;
  }
}

uint32_t StreamingManager::takeVictim()
{
  while (!evictionOrder.empty())
  {
    const uint32_t candidate = evictionOrder.back();

    evictionOrder.pop_back();

    const uint32_t position = residentAt[candidate];

    if (position == notResident)
    {
      continue; // already evicted this call
    }

    const PageMeta &entry = pool.metadata()[candidate];
    const uint32_t partition = entry.partition;

    pool.evict(partition, candidate - pool.partitionFirst()[partition]);

    const uint32_t moved = resident.back();

    resident[position] = moved;
    residentAt[moved] = position;
    resident.pop_back();
    residentAt[candidate] = notResident;

    ++counters.poolEvictions;

    return candidate;
  }

  return notResident;
}

uint32_t StreamingManager::service(const uint32_t instruction)
{
  starved = false;

  if (pending.empty())
  {
    return 0u;
  }

  counters.requested += pending.size();

  // Deterministic order, so a run is reproducible.
  std::sort(pending.begin(), pending.end());

  resizeBookkeeping();

  for (const uint32_t metaIndex : pending)
  {
    requestedFlag[metaIndex] = 1u;
  }

  buildEvictionOrder(instruction);

  uint32_t admitted = 0u;

  std::vector<uint32_t> deferred;

  for (const uint32_t metaIndex : pending)
  {
    const PageMeta &entry = pool.metadata()[metaIndex];
    const uint32_t partition = entry.partition;
    const uint32_t position = metaIndex - pool.partitionFirst()[partition];

    if (pool.isResident(partition, position))
    {
      ++counters.alreadyResident;

      continue;
    }

    uint32_t slot = notResident;

    if (!pool.acquireSlot(slot))
    {
      // Nothing free: make room. Pages the frame just consumed are fair game -
      // their cursors have already passed them.
      if (takeVictim() == notResident)
      {
        deferred.push_back(metaIndex);

        continue;
      }

      if (!pool.acquireSlot(slot))
      {
        deferred.push_back(metaIndex);

        continue;
      }
    }

    if (!pool.stageUpload(store, partition, position, slot))
    {
      pool.releaseSlot(slot);
      deferred.push_back(metaIndex);

      continue;
    }

    residentAt[metaIndex] = static_cast<uint32_t>(resident.size());
    resident.push_back(metaIndex);

    ++admitted;
    ++counters.admitted;
  }

  // One batched write for the whole admission, not one per page.
  pool.flushUploads();

  // `pending` is exactly the set the flags were set on, so clearing costs the
  // size of the request, not the size of the pool.
  for (const uint32_t metaIndex : pending)
  {
    requestedFlag[metaIndex] = 0u;
  }

  pending.swap(deferred);

  if (admitted == 0u && !pending.empty())
  {
    starved = true;

    ++counters.starvedCalls;
  }

  pool.flushMetadata();

  return admitted;
}

bool StreamingManager::lastCallStarved() const
{
  return starved;
}

const StreamingStats &StreamingManager::stats() const
{
  return counters;
}

void StreamingManager::resetStats()
{
  counters = StreamingStats();
}

uint32_t StreamingManager::pendingRequests() const
{
  return static_cast<uint32_t>(pending.size());
}

} // namespace graphrewrite::runtime
