#pragma once

// GPU pool <- CPU cache <- disk.
//
// The GPU emits the requests: a kernel that finds a page non-resident sets a
// bit. This services them between frames. Two rules here are load-bearing:
//
//   - Pages the frame just CONSUMED are not protected. Every row's cursor has
//     already moved past them, so pinning them starves the rows that requested
//     something else. Doing the opposite deadlocked the model on its first case.
//
//   - Forward progress: every admitted page was requested by a row whose cursor
//     points directly at it, so admitting any requested page advances at least
//     one row. If a service call can admit nothing while requests are
//     outstanding, the configuration is broken and it says so.

#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/runtime/CpuPageCache.hpp"
#include "graphrewrite/runtime/GpuPagePool.hpp"

#include <vector>

namespace graphrewrite::runtime {

struct StreamingStats
{
  uint64_t requested = 0u;
  uint64_t admitted = 0u;
  uint64_t poolEvictions = 0u;
  uint64_t alreadyResident = 0u;
  uint64_t starvedCalls = 0u;
};

class StreamingManager
{
public:
  StreamingManager(GpuPagePool &pool, EdgePageStore &store, const compiler::UsageValues &usage);

  // Which edge type each partition indexes, for the eviction ranking.
  void configure(std::vector<EdgeTypeId> partitionEdgeTypes);

  // Records a request from the CPU side (the GPU path uses the request bits).
  void request(uint32_t partition, uint32_t position);

  void requestByMetaIndex(uint32_t metaIndex);

  // Admits as many requested pages as the pool can hold. Returns how many.
  uint32_t service(uint32_t instruction);

  bool lastCallStarved() const;

  const StreamingStats &stats() const;
  void resetStats();

  uint32_t pendingRequests() const;

private:
  // Eviction order for one service call, cheapest victim first. Built once per
  // call rather than rescanning the resident set per admission: doing the latter
  // is O(admissions x resident x pending), which at a few thousand of each is
  // billions of operations and looks exactly like a slow GPU.
  void buildEvictionOrder(uint32_t instruction);

  uint32_t takeVictim();
  uint32_t nextUseOf(uint32_t metaIndex, uint32_t instruction) const;

  GpuPagePool &pool;
  EdgePageStore &store;
  const compiler::UsageValues &usage;

  std::vector<EdgeTypeId> partitionTypes;
  std::vector<uint32_t> pending;  // metadata indices
  std::vector<uint32_t> resident; // metadata indices currently in the pool

  // Flat, metadata-indexed bookkeeping. A pool sized for real work holds on the
  // order of a million slots, so every per-page question here has to be O(1) and
  // hash-free: `residentAt` doubles as the membership test and as the index that
  // makes removal from `resident` a swap rather than a scan.
  std::vector<uint32_t> residentAt;    // metaIndex -> position in `resident`
  std::vector<uint8_t> requestedFlag;  // metaIndex -> is in `pending` this call
  std::vector<uint32_t> evictionOrder; // best victim last, popped from the back
  std::vector<uint32_t> evictionBuckets;

  void resizeBookkeeping();

  StreamingStats counters;
  bool starved = false;
};

} // namespace graphrewrite::runtime
