#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace virtualgeometry::raytracing
{
// A lifetime identity, deliberately independent of recycled scene/page slots.
inline uint64_t allocateBlasOwner()
{
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}
struct PageBlasKey
{
  uint64_t owner = 0;
  uint32_t page = 0;
  uint64_t generation = 0, revision = 0;
};
class PageBlasGate
{
public:
  enum class State { Missing, Pending, Ready, Failed };
  struct ClusterGeometry
  {
    void *vertices = nullptr, *indices = nullptr;
    uint64_t vertexOffset = 0, indexOffset = 0;
    uint32_t vertexStride = 48, vertexCount = 0, triangleCount = 0;
  };
  virtual ~PageBlasGate() = default;
  // GPU-generated geometry uses a producer event, never a CPU geometry readback.
  virtual State prepareDecoded(PageBlasKey, const std::vector<ClusterGeometry> &,
      void *readyEvent, uint64_t readyValue, bool producerCompleted = false) { return State::Failed; }
  // Copies encoded input on first request. Repeated immutable keys are cache hits.
  virtual State prepareEncoded(PageBlasKey, const uint32_t *words, uint64_t wordCount,
      uint32_t headerWords, float unitScale) = 0;
  virtual State state(PageBlasKey, uint32_t clusterCount) = 0;
  virtual std::vector<uint32_t> handles(PageBlasKey) = 0;
  // Nonblocking, called once per scene frame. Never reads geometry/counts back.
  virtual void pump() = 0;
  virtual void releaseOwner(uint64_t owner) = 0;
  virtual void retireOtherVersions(PageBlasKey) {}
  // Cancels queued work, but refuses retirement while GPU input reads remain.
  virtual bool releasePage(PageBlasKey) = 0;
  // Teardown only: wait for submitted input reads, never called by frame updates.
  virtual void drainInputReads() = 0;
};
}
