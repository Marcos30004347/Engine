// Phase 3.6: the GPU page pool and its metadata.
//
// Needs a real device. The interesting assertion is that a page uploaded and
// read back is byte-identical, and that the metadata the GPU reads agrees with
// the CPU store after arbitrary upload/evict sequences.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/GpuPagePool.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <memory>
#include <random>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

compiler::EdgeTypeTable twoTypes()
{
  compiler::EdgeTypeTable types;

  const compiler::EdgeTypeId a = types.intern("? a ?", 2u);
  const compiler::EdgeTypeId b = types.intern("? b ?", 2u);

  types.mutableType(a).usedForward = true;
  types.mutableType(b).usedForward = true;
  types.mutableType(b).usedBackward = true;

  return types;
}

void runAll(rendering::RHI *rhi)
{
  auto graphStorage = std::make_unique<rendering::RenderGraph>(rhi);
  rendering::RenderGraph &graph = *graphStorage;

  const compiler::EdgeTypeTable types = twoTypes();

  EdgePageStore store;

  store.configure(types);

  std::mt19937 rng(1234u);

  for (uint32_t i = 0u; i < 900u; ++i)
  {
    store.addEdge(0u, Edge{rng() % 60u, rng() % 60u});
    store.addEdge(1u, Edge{rng() % 60u, rng() % 60u});
  }

  GpuPagePoolSettings settings;

  settings.slotCount = 16u;

  GpuPagePool pool(graph, settings);

  pool.configure(store);

  // --- metadata mirrors the store ---------------------------------------
  uint32_t expectedPages = 0u;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    expectedPages += store.partitionAt(partition).pageCount();
  }

  checkEqual(pool.totalPages(), expectedPages, "metadata covers every page of every partition");
  checkEqual(pool.partitionFirst().size(), size_t(store.partitionCount() + 1u), "partition table has one entry per partition plus a sentinel");

  bool rangesMatch = true;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    const Partition &source = store.partitionAt(partition);

    checkEqual(pool.pageCountOf(partition), source.pageCount(), "partition " + std::to_string(partition) + " page count");

    for (uint32_t position = 0u; position < source.pageCount(); ++position)
    {
      const PageMeta &entry = pool.metadata()[pool.metaIndex(partition, position)];

      rangesMatch = rangesMatch && entry.minKey == source.minKeyAt(position) && entry.maxKey == source.maxKeyAt(position) &&
                    entry.count == source.pageAt(position).size() && entry.partition == partition;
    }
  }

  check(rangesMatch, "every metadata entry agrees with its page");

  // --- everything starts non-resident -----------------------------------
  bool anyResident = false;

  for (uint32_t partition = 0u; partition < store.partitionCount(); ++partition)
  {
    for (uint32_t position = 0u; position < store.partitionAt(partition).pageCount(); ++position)
    {
      anyResident = anyResident || pool.isResident(partition, position);
    }
  }

  check(!anyResident, "a fresh pool has nothing resident");
  checkEqual(pool.freeSlotCount(), settings.slotCount, "and every slot is free");

  // --- upload and read back ---------------------------------------------
  uint32_t slot = notResident;

  check(pool.acquireSlot(slot), "a slot is available");
  check(pool.upload(store, 0u, 0u, slot), "page 0 of partition 0 uploads");
  check(pool.isResident(0u, 0u), "and is marked resident");
  checkEqual(pool.slotOf(0u, 0u), slot, "in the slot it was given");

  pool.flushMetadata();
  pool.stagePage(slot);

  // A buffer cannot be both MAP_WRITE and MAP_READ, so the page is copied into
  // a staging mirror by a pass and read from there.
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("graphrewrite.stageCopy", 0u, pool.pagesBuffer(), pool.stagedOffset(),
                                                       pool.pageBytes(), pool.stagingBuffer(), 0u, pool.pageBytes());

  rendering::RenderGraph::Frame frame;
  rendering::RenderGraph::Overrides overrides;

  graph.setCurrentFrameIndex(0u);
  graph.compile();
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  std::vector<Edge> readBack;

  check(pool.readStaged(readBack), "the slot reads back through staging");

  const std::vector<Edge> &expected = store.partitionAt(0u).pageAt(0u);

  bool identical = readBack.size() >= expected.size();

  for (size_t i = 0u; identical && i < expected.size(); ++i)
  {
    identical = readBack[i] == expected[i];
  }

  check(identical, "an uploaded page reads back byte-identically");

  // --- evict returns the slot -------------------------------------------
  pool.evict(0u, 0u);

  check(!pool.isResident(0u, 0u), "eviction clears residency");
  checkEqual(pool.freeSlotCount(), settings.slotCount, "and returns the slot to the pool");

  // --- arbitrary upload/evict keeps metadata consistent -----------------
  std::vector<std::pair<uint32_t, uint32_t>> resident;

  for (uint32_t step = 0u; step < 200u; ++step)
  {
    const bool doUpload = resident.empty() || ((rng() % 3u) != 0u && pool.freeSlotCount() > 0u);

    if (doUpload && pool.freeSlotCount() > 0u)
    {
      const uint32_t partition = rng() % store.partitionCount();
      const uint32_t pageCount = store.partitionAt(partition).pageCount();

      if (pageCount == 0u)
      {
        continue;
      }

      const uint32_t position = rng() % pageCount;

      if (pool.isResident(partition, position))
      {
        continue;
      }

      uint32_t next = notResident;

      if (!pool.acquireSlot(next))
      {
        continue;
      }

      pool.upload(store, partition, position, next);
      resident.emplace_back(partition, position);
    }
    else if (!resident.empty())
    {
      const size_t index = rng() % resident.size();

      pool.evict(resident[index].first, resident[index].second);
      resident.erase(resident.begin() + static_cast<long>(index));
    }
  }

  bool consistent = true;

  for (const auto &entry : resident)
  {
    consistent = consistent && pool.isResident(entry.first, entry.second);
  }

  checkEqual(pool.freeSlotCount() + static_cast<uint32_t>(resident.size()), settings.slotCount,
             "free slots plus resident pages always account for the whole pool");
  check(consistent, "every page recorded resident really is");

  // A slot is never handed out twice.
  std::vector<uint32_t> usedSlots;

  for (const auto &entry : resident)
  {
    usedSlots.push_back(pool.slotOf(entry.first, entry.second));
  }

  std::sort(usedSlots.begin(), usedSlots.end());

  check(std::adjacent_find(usedSlots.begin(), usedSlots.end()) == usedSlots.end(), "no slot is occupied by two pages");
}

} // namespace

int main()
{
  rendering::DeviceRequiredLimits limits = {
      .minimumMemory = 0,
      .minimumComputeSharedMemory = 0,
      .minimumComputeWorkGroupInvocations = 0,
  };

  const rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute;

  rendering::backend::vulkan::VulkanRHI *rhi =
      new rendering::backend::vulkan::VulkanRHI(rendering::backend::vulkan::Vulkan_1_2, limits, features, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  rhi->init(surfaces);

  runAll(rhi);

  delete rhi;

  return report("GraphRewritePagePoolTests");
}
