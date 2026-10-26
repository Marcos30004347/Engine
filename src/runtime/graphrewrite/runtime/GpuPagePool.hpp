#pragma once

// The hot tier: a pool of fixed page slots in GPU memory, plus the metadata that
// lets a shader decide for itself which pages it is missing.
//
// The metadata buffer covers EVERY page, resident or not. That is what makes the
// streaming decision a GPU one: a kernel reads `slot`, sees `not_resident`, and
// sets a request bit, rather than the CPU having to predict what will be needed.

#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <vector>

namespace graphrewrite::runtime {

struct GpuPagePoolSettings
{
  uint32_t slotCount = 4096u;
  uint32_t edgesPerPage = pageEdges;
};

class GpuPagePool
{
public:
  GpuPagePool(rendering::RenderGraph &graph, const GpuPagePoolSettings &settings);

  // Builds the metadata array from the store: one PageMeta per page of every
  // partition, laid out so a partition's pages are contiguous and in key order.
  void configure(const EdgePageStore &store);

  // Recomputes the metadata after the store changed - a rewrite splits pages and
  // shifts directory positions - WITHOUT reallocating the buffers.
  //
  // Residency is dropped wholesale rather than remapped: a split renumbers
  // positions, so a slot's old metadata index no longer means what it did, and
  // guessing is how a page ends up serving another page's edges. The streaming
  // manager re-admits what the next instruction asks for.
  bool rebuildMetadata(const EdgePageStore &store);

  // What was resident before the store changed, keyed by page identity, so a
  // rebuild after a split can keep every unchanged page where it is.
  struct ResidentPage
  {
    uint32_t partition = 0u;
    PageId page = 0u;
    uint32_t slot = 0u;
    uint32_t count = 0u;
    uint32_t minKey = 0u;
    uint32_t maxKey = 0u;
  };

  std::vector<ResidentPage> snapshotResidency(const EdgePageStore &store) const;

  // rebuildMetadata, but a page in `kept` whose count and key range are
  // unchanged keeps its slot, and one that changed is re-uploaded into the
  // slot it had. Dropping residency wholesale on every split made the next
  // frames stream every page of the store back in.
  bool rebuildMetadataKeeping(const EdgePageStore &store, const std::vector<ResidentPage> &kept);

  uint32_t metadataCapacity() const
  {
    return metaCapacity;
  }

  // Uploads a page into a slot and marks it resident. Both sides of the
  // residency record move together, which is the invariant the streaming
  // manager relies on.
  bool upload(const EdgePageStore &store, uint32_t partition, uint32_t position, uint32_t slot);

  // Stages a page for upload without issuing a write. `flushUploads` then sends
  // runs of consecutive slots as single writes.
  //
  // This matters more than it sounds: admitting two thousand pages one
  // bufferWrite at a time is two thousand driver round trips per tick, and that
  // - not the kernels - is what makes a pool too small to hold the working set
  // cost two orders of magnitude.
  bool stageUpload(const EdgePageStore &store, uint32_t partition, uint32_t position, uint32_t slot);
  void flushUploads();

  uint32_t pendingUploadCount() const
  {
    return static_cast<uint32_t>(stagedSlots.size());
  }

  uint64_t uploadWriteCount() const
  {
    return uploadWrites;
  }

  // Frees a slot. The caller is responsible for having saved a dirty page first
  // - see StreamingManager.
  void evict(uint32_t partition, uint32_t position);

  // A buffer cannot be both MAP_WRITE and MAP_READ, so readback goes through a
  // dedicated staging mirror that a copy pass fills. `stagePage` records which
  // slot the next graph run should copy; `readStaged` reads it afterwards.
  void stagePage(uint32_t slot);
  bool readStaged(std::vector<Edge> &out) const;

  const rendering::Buffer &stagingBuffer() const;
  uint64_t stagedOffset() const;
  uint64_t pageBytes() const;

  bool isResident(uint32_t partition, uint32_t position) const;
  uint32_t slotOf(uint32_t partition, uint32_t position) const;

  // Global metadata index of a partition's page, which is what the shader's
  // directory search indexes by.
  uint32_t metaIndex(uint32_t partition, uint32_t position) const;
  uint32_t pageCountOf(uint32_t partition) const;
  uint32_t totalPages() const;

  const std::vector<PageMeta> &metadata() const;
  const std::vector<uint32_t> &partitionFirst() const;

  const rendering::Buffer &pagesBuffer() const;
  const rendering::Buffer &metadataBuffer() const;
  const rendering::Buffer &requestBuffer() const;
  const rendering::Buffer &requestStagingBuffer() const;
  uint64_t requestBytes() const;
  const rendering::Buffer &partitionFirstBuffer() const;

  // Pushes the CPU-side metadata and partition table to the GPU.
  void flushMetadata();

  // Reads the request bits the GPU set, and clears them.
  std::vector<uint32_t> collectRequests();

  uint32_t freeSlotCount() const;
  bool acquireSlot(uint32_t &slot);
  void releaseSlot(uint32_t slot);

  const GpuPagePoolSettings &settings() const;

private:
  rendering::RenderGraph &graph;
  GpuPagePoolSettings poolSettings;

  rendering::Buffer pages;
  rendering::Buffer meta;
  rendering::Buffer requests;
  rendering::Buffer partFirst;
  rendering::Buffer staging;
  rendering::Buffer requestStaging;
  uint32_t stagedSlot = notResident;

  std::vector<PageMeta> metaCpu;
  std::vector<uint32_t> partFirstCpu; // partitionCount + 1 entries
  std::vector<uint32_t> slotToMeta;   // slot -> metadata index, or notResident
  std::vector<uint32_t> freeSlots;
  uint32_t metaCapacity = 0u;

  std::vector<uint32_t> stagedSlots;
  std::vector<Edge> stagedEdges; // slotCount-indexed scratch, filled per batch
  uint64_t uploadWrites = 0u;
};

} // namespace graphrewrite::runtime
