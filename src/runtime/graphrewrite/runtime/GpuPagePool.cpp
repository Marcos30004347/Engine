#include "graphrewrite/runtime/GpuPagePool.hpp"

#include <map>

#include <algorithm>
#include <cstring>

namespace graphrewrite::runtime {

GpuPagePool::GpuPagePool(rendering::RenderGraph &renderGraph, const GpuPagePoolSettings &settings)
    : graph(renderGraph), poolSettings(settings)
{
  const uint64_t pageBytes = static_cast<uint64_t>(poolSettings.edgesPerPage) * sizeof(Edge);

  // Push only: the RHI rejects a buffer that is both MAP_WRITE and MAP_READ, so
  // reading a page back goes through the staging mirror below.
  pages = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.pagePool",
      .size = pageBytes * poolSettings.slotCount,
      .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push |
               rendering::BufferUsage::BufferUsage_CopySrc,
  });

  // A Pull buffer is readback-only: no Storage binding, no GPU writes, just a
  // copy destination the CPU maps afterwards.
  staging = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.pageStaging",
      .size = pageBytes,
      .usage = rendering::BufferUsage::BufferUsage_Pull | rendering::BufferUsage::BufferUsage_CopyDst,
  });

  freeSlots.reserve(poolSettings.slotCount);

  for (uint32_t slot = poolSettings.slotCount; slot-- > 0u;)
  {
    freeSlots.push_back(slot);
  }

  slotToMeta.assign(poolSettings.slotCount, notResident);
}

void GpuPagePool::configure(const EdgePageStore &store)
{
  metaCpu.clear();
  partFirstCpu.clear();

  partFirstCpu.push_back(0u);

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    const Partition &source = store.partitionAt(partition);

    for (uint32_t position = 0u; position < source.pageCount(); ++position)
    {
      PageMeta entry;

      entry.partition = partition;
      entry.minKey = source.minKeyAt(position);
      entry.maxKey = source.maxKeyAt(position);
      entry.count = static_cast<uint32_t>(source.pageAt(position).size());
      entry.slot = notResident;
      entry.flags = PageFlagNone;

      metaCpu.push_back(entry);
    }

    partFirstCpu.push_back(static_cast<uint32_t>(metaCpu.size()));
  }

  // The metadata covers every page whether or not it is resident, so it is
  // sized from the store rather than from the pool - with headroom, because a
  // rewrite splits pages and the array has to grow without reallocating.
  metaCapacity = static_cast<uint32_t>(metaCpu.size()) * 4u + 1024u;

  const uint64_t metaBytes = sizeof(PageMeta) * metaCapacity;

  meta = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.pageMeta",
      .size = metaBytes,
      .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
  });

  partFirst = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.partitionFirst",
      .size = sizeof(uint32_t) * (partFirstCpu.size() + 64u),
      .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
  });

  const uint64_t requestWords = (metaCapacity + 31u) / 32u;

  requests = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.pageRequests",
      .size = std::max<uint64_t>(sizeof(uint32_t), sizeof(uint32_t) * requestWords),
      .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push |
               rendering::BufferUsage::BufferUsage_CopySrc,
  });

  // Requests are written by the GPU and read by the CPU, which the RHI will not
  // do on one buffer either - so the bits live in a Storage buffer and are
  // copied into this mirror before being read.
  requestStaging = graph.createBuffer(rendering::BufferInfo{
      .name = "graphrewrite.pageRequestStaging",
      .size = std::max<uint64_t>(sizeof(uint32_t), sizeof(uint32_t) * requestWords),
      .usage = rendering::BufferUsage::BufferUsage_Pull | rendering::BufferUsage::BufferUsage_CopyDst,
  });

  flushMetadata();
}

bool GpuPagePool::rebuildMetadata(const EdgePageStore &store)
{
  std::vector<PageMeta> rebuilt;
  std::vector<uint32_t> first;

  first.push_back(0u);

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    const Partition &source = store.partitionAt(partition);

    for (uint32_t position = 0u; position < source.pageCount(); ++position)
    {
      PageMeta entry;

      entry.partition = partition;
      entry.minKey = source.minKeyAt(position);
      entry.maxKey = source.maxKeyAt(position);
      entry.count = static_cast<uint32_t>(source.pageAt(position).size());
      entry.slot = notResident;
      entry.flags = PageFlagNone;

      rebuilt.push_back(entry);
    }

    first.push_back(static_cast<uint32_t>(rebuilt.size()));
  }

  if (rebuilt.size() > metaCapacity)
  {
    return false;
  }

  metaCpu.swap(rebuilt);
  partFirstCpu.swap(first);

  // Everything is non-resident again, so every slot goes back to the pool.
  freeSlots.clear();

  for (uint32_t slot = poolSettings.slotCount; slot-- > 0u;)
  {
    freeSlots.push_back(slot);
  }

  slotToMeta.assign(poolSettings.slotCount, notResident);

  flushMetadata();

  return true;
}

std::vector<GpuPagePool::ResidentPage> GpuPagePool::snapshotResidency(const EdgePageStore &store) const
{
  std::vector<ResidentPage> resident;

  for (uint32_t partition = 0u; partition + 1u < static_cast<uint32_t>(partFirstCpu.size()) && partition < store.partitionCount();
       ++partition)
  {
    const Partition &source = store.partitionAt(partition);
    const uint32_t first = partFirstCpu[partition];
    const uint32_t last = partFirstCpu[partition + 1u];

    for (uint32_t index = first; index < last && index - first < source.pageCount(); ++index)
    {
      const PageMeta &entry = metaCpu[index];

      if (entry.slot == notResident)
      {
        continue;
      }

      ResidentPage page;

      page.partition = partition;
      page.page = source.pageIdAt(index - first);
      page.slot = entry.slot;
      page.count = entry.count;
      page.minKey = entry.minKey;
      page.maxKey = entry.maxKey;

      resident.push_back(page);
    }
  }

  return resident;
}

bool GpuPagePool::rebuildMetadataKeeping(const EdgePageStore &store, const std::vector<ResidentPage> &kept)
{
  if (!rebuildMetadata(store))
  {
    return false;
  }

  std::map<std::pair<uint32_t, PageId>, const ResidentPage *> byIdentity;

  for (const ResidentPage &page : kept)
  {
    byIdentity[{page.partition, page.page}] = &page;
  }

  // Slots the kept pages hold come back out of the free list.
  std::vector<bool> taken(poolSettings.slotCount, false);

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    const Partition &source = store.partitionAt(partition);

    for (uint32_t position = 0u; position < source.pageCount(); ++position)
    {
      const auto found = byIdentity.find({partition, source.pageIdAt(position)});

      if (found == byIdentity.end() || found->second->slot >= poolSettings.slotCount || taken[found->second->slot])
      {
        continue;
      }

      const ResidentPage &was = *found->second;
      const uint32_t index = metaIndex(partition, position);

      taken[was.slot] = true;

      const bool unchanged = static_cast<uint32_t>(source.pageAt(position).size()) == was.count &&
                             source.minKeyAt(position) == was.minKey && source.maxKeyAt(position) == was.maxKey;

      if (unchanged)
      {
        metaCpu[index].slot = was.slot;
        slotToMeta[was.slot] = index;
      }
      else
      {
        upload(store, partition, position, was.slot);
      }
    }
  }

  freeSlots.clear();

  for (uint32_t slot = poolSettings.slotCount; slot-- > 0u;)
  {
    if (!taken[slot])
    {
      freeSlots.push_back(slot);
    }
  }

  flushMetadata();

  return true;
}

uint32_t GpuPagePool::metaIndex(const uint32_t partition, const uint32_t position) const
{
  if (partition + 1u >= partFirstCpu.size())
  {
    return notResident;
  }

  const uint32_t base = partFirstCpu[partition];

  if (base + position >= partFirstCpu[partition + 1u])
  {
    return notResident;
  }

  return base + position;
}

uint32_t GpuPagePool::pageCountOf(const uint32_t partition) const
{
  if (partition + 1u >= partFirstCpu.size())
  {
    return 0u;
  }

  return partFirstCpu[partition + 1u] - partFirstCpu[partition];
}

uint32_t GpuPagePool::totalPages() const
{
  return static_cast<uint32_t>(metaCpu.size());
}

bool GpuPagePool::acquireSlot(uint32_t &slot)
{
  if (freeSlots.empty())
  {
    return false;
  }

  slot = freeSlots.back();
  freeSlots.pop_back();

  return true;
}

void GpuPagePool::releaseSlot(const uint32_t slot)
{
  if (slot >= poolSettings.slotCount)
  {
    return;
  }

  slotToMeta[slot] = notResident;
  freeSlots.push_back(slot);
}

uint32_t GpuPagePool::freeSlotCount() const
{
  return static_cast<uint32_t>(freeSlots.size());
}

bool GpuPagePool::stageUpload(const EdgePageStore &store, const uint32_t partition, const uint32_t position, const uint32_t slot)
{
  const uint32_t index = metaIndex(partition, position);

  if (index == notResident || slot >= poolSettings.slotCount)
  {
    return false;
  }

  const Partition &source = store.partitionAt(partition);
  const std::vector<Edge> &edges = source.pageAt(position);

  if (stagedEdges.size() < static_cast<size_t>(poolSettings.slotCount) * poolSettings.edgesPerPage)
  {
    stagedEdges.assign(static_cast<size_t>(poolSettings.slotCount) * poolSettings.edgesPerPage, Edge{});
  }

  Edge *destination = stagedEdges.data() + static_cast<size_t>(slot) * poolSettings.edgesPerPage;

  std::fill(destination, destination + poolSettings.edgesPerPage, Edge{});
  std::copy(edges.begin(), edges.end(), destination);

  metaCpu[index].slot = slot;
  metaCpu[index].count = static_cast<uint32_t>(edges.size());
  metaCpu[index].minKey = source.minKeyAt(position);
  metaCpu[index].maxKey = source.maxKeyAt(position);

  slotToMeta[slot] = index;

  stagedSlots.push_back(slot);

  return true;
}

void GpuPagePool::flushUploads()
{
  if (stagedSlots.empty())
  {
    return;
  }

  std::sort(stagedSlots.begin(), stagedSlots.end());
  stagedSlots.erase(std::unique(stagedSlots.begin(), stagedSlots.end()), stagedSlots.end());

  const uint64_t pageBytesValue = static_cast<uint64_t>(poolSettings.edgesPerPage) * sizeof(Edge);

  // Coalesce consecutive slots into one write each. The free list hands slots
  // back in order, so in practice a batch collapses to a handful of writes.
  size_t runStart = 0u;

  for (size_t i = 1u; i <= stagedSlots.size(); ++i)
  {
    const bool endOfRun = i == stagedSlots.size() || stagedSlots[i] != stagedSlots[i - 1u] + 1u;

    if (!endOfRun)
    {
      continue;
    }

    const uint32_t firstSlot = stagedSlots[runStart];
    const uint32_t slotCount = static_cast<uint32_t>(i - runStart);

    graph.bufferWrite(pages, pageBytesValue * firstSlot, pageBytesValue * slotCount,
                      stagedEdges.data() + static_cast<size_t>(firstSlot) * poolSettings.edgesPerPage);

    ++uploadWrites;

    runStart = i;
  }

  stagedSlots.clear();
}

bool GpuPagePool::upload(const EdgePageStore &store, const uint32_t partition, const uint32_t position, const uint32_t slot)
{
  const uint32_t index = metaIndex(partition, position);

  if (index == notResident || slot >= poolSettings.slotCount)
  {
    return false;
  }

  const Partition &source = store.partitionAt(partition);
  std::vector<Edge> edges = source.pageAt(position);

  const uint64_t pageBytes = static_cast<uint64_t>(poolSettings.edgesPerPage) * sizeof(Edge);

  edges.resize(poolSettings.edgesPerPage, Edge{});

  graph.bufferWrite(pages, pageBytes * slot, pageBytes, edges.data());

  metaCpu[index].slot = slot;
  metaCpu[index].count = static_cast<uint32_t>(source.pageAt(position).size());
  metaCpu[index].minKey = source.minKeyAt(position);
  metaCpu[index].maxKey = source.maxKeyAt(position);

  slotToMeta[slot] = index;

  return true;
}

void GpuPagePool::evict(const uint32_t partition, const uint32_t position)
{
  const uint32_t index = metaIndex(partition, position);

  if (index == notResident)
  {
    return;
  }

  const uint32_t slot = metaCpu[index].slot;

  metaCpu[index].slot = notResident;

  releaseSlot(slot);
}

void GpuPagePool::stagePage(const uint32_t slot)
{
  stagedSlot = slot;
}

uint64_t GpuPagePool::pageBytes() const
{
  return static_cast<uint64_t>(poolSettings.edgesPerPage) * sizeof(Edge);
}

uint64_t GpuPagePool::stagedOffset() const
{
  return stagedSlot == notResident ? 0u : pageBytes() * stagedSlot;
}

const rendering::Buffer &GpuPagePool::stagingBuffer() const
{
  return staging;
}

bool GpuPagePool::readStaged(std::vector<Edge> &out) const
{
  out.assign(poolSettings.edgesPerPage, Edge{});

  bool done = false;

  const_cast<rendering::RenderGraph &>(graph).bufferRead(staging, 0u, pageBytes(), [&out, &done](const void *data) {
    std::memcpy(out.data(), data, out.size() * sizeof(Edge));

    done = true;
  });

  return done;
}

bool GpuPagePool::isResident(const uint32_t partition, const uint32_t position) const
{
  const uint32_t index = metaIndex(partition, position);

  return index != notResident && metaCpu[index].slot != notResident;
}

uint32_t GpuPagePool::slotOf(const uint32_t partition, const uint32_t position) const
{
  const uint32_t index = metaIndex(partition, position);

  return index == notResident ? notResident : metaCpu[index].slot;
}

const std::vector<PageMeta> &GpuPagePool::metadata() const
{
  return metaCpu;
}

const std::vector<uint32_t> &GpuPagePool::partitionFirst() const
{
  return partFirstCpu;
}

const rendering::Buffer &GpuPagePool::pagesBuffer() const
{
  return pages;
}

const rendering::Buffer &GpuPagePool::metadataBuffer() const
{
  return meta;
}

const rendering::Buffer &GpuPagePool::requestBuffer() const
{
  return requests;
}

const rendering::Buffer &GpuPagePool::requestStagingBuffer() const
{
  return requestStaging;
}

uint64_t GpuPagePool::requestBytes() const
{
  const size_t words = (metaCapacity + 31u) / 32u;

  return std::max<uint64_t>(sizeof(uint32_t), sizeof(uint32_t) * words);
}

const rendering::Buffer &GpuPagePool::partitionFirstBuffer() const
{
  return partFirst;
}

void GpuPagePool::flushMetadata()
{
  if (!metaCpu.empty())
  {
    graph.bufferWrite(meta, 0u, sizeof(PageMeta) * metaCpu.size(), metaCpu.data());
  }

  if (!partFirstCpu.empty())
  {
    graph.bufferWrite(partFirst, 0u, sizeof(uint32_t) * partFirstCpu.size(), partFirstCpu.data());
  }
}

std::vector<uint32_t> GpuPagePool::collectRequests()
{
  std::vector<uint32_t> out;

  const size_t words = (metaCpu.size() + 31u) / 32u;

  if (words == 0u)
  {
    return out;
  }

  std::vector<uint32_t> bits(words, 0u);

  graph.bufferRead(requestStaging, 0u, sizeof(uint32_t) * words, [&bits, words](const void *data) {
    std::memcpy(bits.data(), data, words * sizeof(uint32_t));
  });

  for (size_t word = 0u; word < words; ++word)
  {
    uint32_t value = bits[word];

    while (value != 0u)
    {
      const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(value));

      value &= value - 1u;

      const uint32_t page = static_cast<uint32_t>(word) * 32u + bit;

      if (page < metaCpu.size())
      {
        out.push_back(page);
      }
    }
  }

  // Clearing is the caller's half of the contract - a request that stays set
  // would be serviced again every frame - but the request buffer is MAP_READ
  // only, so the clear is a GPU-side job for the plan pass rather than a write
  // from here.
  return out;
}

const GpuPagePoolSettings &GpuPagePool::settings() const
{
  return poolSettings;
}

} // namespace graphrewrite::runtime
