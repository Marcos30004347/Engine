#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/PlanetCbt.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtAllocatorPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp"

#include <algorithm>
#include <array>
#undef NDEBUG
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

constexpr uint32_t kCanary = 0xd15ea5edu;
constexpr uint64_t kGuardBytes = 256u;
constexpr uint32_t kGuardWords = static_cast<uint32_t>(kGuardBytes / sizeof(uint32_t));

struct GuardedBuffer
{
  Buffer gpu{};
  Buffer readback{};
  uint64_t payloadBytes = 0u;
  std::vector<uint32_t> upload{};
  std::vector<uint32_t> downloaded{};

  uint64_t payloadOffset() const
  {
    return kGuardBytes;
  }
  uint64_t totalBytes() const
  {
    return upload.size() * sizeof(uint32_t);
  }
  uint32_t payloadWords() const
  {
    return static_cast<uint32_t>(payloadBytes / sizeof(uint32_t));
  }
};

GuardedBuffer makeGuardedBuffer(RenderGraph &graph, const std::string &name, const uint64_t payloadBytes, const BufferUsage extraUsage = BufferUsage::BufferUsage_None)
{
  assert((payloadBytes % sizeof(uint32_t)) == 0u);
  GuardedBuffer result{};
  result.payloadBytes = payloadBytes;
  result.upload.assign(kGuardWords + payloadBytes / sizeof(uint32_t) + kGuardWords, kCanary);
  result.downloaded.resize(result.upload.size());
  result.gpu = graph.createBuffer({
    .name = name,
    .size = result.totalBytes(),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc | extraUsage,
  });
  result.readback = graph.createBuffer({
    .name = name + ".Readback",
    .size = result.totalBytes(),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  return result;
}

template <typename T> void setPayload(GuardedBuffer &buffer, const T &value)
{
  assert(sizeof(T) == buffer.payloadBytes);
  std::fill(buffer.upload.begin(), buffer.upload.end(), kCanary);
  std::memcpy(buffer.upload.data() + kGuardWords, &value, sizeof(T));
}

template <typename T> void setPayload(GuardedBuffer &buffer, const std::vector<T> &value)
{
  assert(value.size() * sizeof(T) == buffer.payloadBytes);
  std::fill(buffer.upload.begin(), buffer.upload.end(), kCanary);
  if (!value.empty())
    std::memcpy(buffer.upload.data() + kGuardWords, value.data(), buffer.payloadBytes);
}

void upload(RenderGraph &graph, GuardedBuffer &buffer)
{
  graph.bufferWrite(buffer.gpu, 0u, buffer.totalBytes(), buffer.upload.data());
}

void download(RenderGraph &graph, GuardedBuffer &buffer)
{
  graph.bufferRead(
      buffer.readback,
      0u,
      buffer.totalBytes(),
      [&](const void *data)
      {
        std::memcpy(buffer.downloaded.data(), data, buffer.totalBytes());
      });
}

template <typename T> T downloadedPayload(const GuardedBuffer &buffer)
{
  assert(sizeof(T) == buffer.payloadBytes);
  T result{};
  std::memcpy(&result, buffer.downloaded.data() + kGuardWords, sizeof(T));
  return result;
}

template <typename T> std::vector<T> downloadedPayloadVector(const GuardedBuffer &buffer)
{
  assert((buffer.payloadBytes % sizeof(T)) == 0u);
  std::vector<T> result(buffer.payloadBytes / sizeof(T));
  if (!result.empty())
    std::memcpy(result.data(), buffer.downloaded.data() + kGuardWords, buffer.payloadBytes);
  return result;
}

bool canariesIntact(const GuardedBuffer &buffer)
{
  for (uint32_t word = 0u; word < kGuardWords; ++word)
    if (buffer.downloaded[word] != kCanary)
      return false;
  const uint32_t suffix = kGuardWords + buffer.payloadWords();
  for (uint32_t word = suffix; word < buffer.downloaded.size(); ++word)
    if (buffer.downloaded[word] != kCanary)
      return false;
  return true;
}

void destroyGuardedBuffer(RenderGraph &graph, GuardedBuffer &buffer)
{
  graph.deleteBuffer(buffer.readback);
  graph.deleteBuffer(buffer.gpu);
}

enum class Pattern
{
  Empty,
  Full,
  Alternating,
  Sparse,
  Random,
};

std::vector<uint32_t> makeHeap(const uint32_t treeDepth, const Pattern pattern)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  std::vector<uint32_t> heap(planetCbtHeapEntryCount(treeDepth), 0x7f7f7f7fu);
  heap[0] = 0u;
  std::mt19937 random(0x49e321d7u ^ capacity);
  for (uint32_t slot = 0u; slot < capacity; ++slot)
  {
    bool allocated = false;
    switch (pattern)
    {
    case Pattern::Empty:
      break;
    case Pattern::Full:
      allocated = true;
      break;
    case Pattern::Alternating:
      allocated = (slot & 1u) == 0u;
      break;
    case Pattern::Sparse:
      allocated = slot == 0u || slot == capacity / 3u || slot == capacity / 2u + 1u || slot + 1u == capacity;
      break;
    case Pattern::Random:
      allocated = (random() & 7u) < 3u;
      break;
    }
    heap[capacity + slot] = allocated ? 1u : 0u;
  }
  return heap;
}

const char *patternName(const Pattern pattern)
{
  switch (pattern)
  {
  case Pattern::Empty:
    return "empty";
  case Pattern::Full:
    return "full";
  case Pattern::Alternating:
    return "alternating";
  case Pattern::Sparse:
    return "sparse";
  case Pattern::Random:
    return "random";
  }
  return "unknown";
}

void runCapacityFixture(::rendering::backend::vulkan::VulkanRHI &rhi, const uint32_t treeDepth)
{
  const uint32_t capacity = planetCbtPoolCapacity(treeDepth);
  RenderGraph graph(&rhi);
  GuardedBuffer heap = makeGuardedBuffer(graph, "PlanetCbt.Heap." + std::to_string(capacity), planetCbtHeapEntryCount(treeDepth) * sizeof(uint32_t));
  GuardedBuffer pointers = makeGuardedBuffer(graph, "PlanetCbt.Pointers." + std::to_string(capacity), uint64_t{capacity} * sizeof(PlanetCbtPointerGPU));
  GuardedBuffer counters = makeGuardedBuffer(graph, "PlanetCbt.Counters." + std::to_string(capacity), sizeof(PlanetCbtCountersGPU));
  GuardedBuffer dispatches = makeGuardedBuffer(graph, "PlanetCbt.Dispatches." + std::to_string(capacity), sizeof(PlanetCbtDispatchesGPU), BufferUsage::BufferUsage_Indirect);
  GuardedBuffer draw = makeGuardedBuffer(graph, "PlanetCbt.Draw." + std::to_string(capacity), sizeof(PlanetCbtDrawIndirectGPU), BufferUsage::BufferUsage_Indirect);

  const svrRendering::SparseVoxelPlanetCbtAllocatorPass::Buffers buffers{heap.gpu, pointers.gpu, counters.gpu, dispatches.gpu, draw.gpu};
  const svrRendering::SparseVoxelPlanetCbtAllocatorPass::Offsets offsets{heap.payloadOffset(), pointers.payloadOffset(), counters.payloadOffset(), dispatches.payloadOffset(), draw.payloadOffset()};
  uint32_t passIndex = 0u;
  std::vector<svrRendering::SparseVoxelPlanetCbtAllocatorPass *> allocatorPasses;
  for (uint32_t level = treeDepth; level > 0u; --level)
    allocatorPasses.push_back(graph.registerPass<svrRendering::SparseVoxelPlanetCbtAllocatorPass>(
        "PlanetCbt.Reduce." + std::to_string(capacity) + "." + std::to_string(level), passIndex++, svrRendering::SparseVoxelPlanetCbtAllocatorPass::Stage::Reduce, buffers, treeDepth, level, offsets));
  allocatorPasses.push_back(graph.registerPass<svrRendering::SparseVoxelPlanetCbtAllocatorPass>(
      "PlanetCbt.Reset." + std::to_string(capacity), passIndex++, svrRendering::SparseVoxelPlanetCbtAllocatorPass::Stage::Reset, buffers, treeDepth, 0u, offsets));
  allocatorPasses.push_back(graph.registerPass<svrRendering::SparseVoxelPlanetCbtAllocatorPass>(
      "PlanetCbt.Cache." + std::to_string(capacity), passIndex++, svrRendering::SparseVoxelPlanetCbtAllocatorPass::Stage::CachePointers, buffers, treeDepth, 0u, offsets));

  std::array<GuardedBuffer *, 5> guarded{&heap, &pointers, &counters, &dispatches, &draw};
  std::vector<::rendering::gpgpu::CopyBufferPass *> copies;
  for (GuardedBuffer *buffer : guarded)
    copies.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "PlanetCbt.Copy." + std::to_string(capacity) + "." + std::to_string(copies.size()), passIndex++, buffer->gpu, 0u, buffer->totalBytes(), buffer->readback, 0u, buffer->totalBytes()));
  graph.compile();

  constexpr std::array patterns{Pattern::Empty, Pattern::Full, Pattern::Alternating, Pattern::Sparse, Pattern::Random};
  for (const Pattern pattern : patterns)
  {
    const std::vector<uint32_t> inputHeap = makeHeap(treeDepth, pattern);
    std::vector<uint32_t> expectedHeap = inputHeap;
    assert(planetCbtReduce(expectedHeap, treeDepth));
    const uint32_t activeCount = expectedHeap[1u];
    const uint32_t freeCount = capacity - activeCount;
    const uint32_t pointerCount = std::max(activeCount, freeCount);

    setPayload(heap, inputHeap);
    std::vector<PlanetCbtPointerGPU> initialPointers(capacity);
    std::memset(initialPointers.data(), 0xa5, initialPointers.size() * sizeof(initialPointers[0]));
    setPayload(pointers, initialPointers);
    PlanetCbtCountersGPU initialCounters{};
    std::memset(&initialCounters, 0xa5, sizeof(initialCounters));
    setPayload(counters, initialCounters);
    PlanetCbtDispatchesGPU initialDispatches{};
    std::memset(&initialDispatches, 0xa5, sizeof(initialDispatches));
    setPayload(dispatches, initialDispatches);
    PlanetCbtDrawIndirectGPU initialDraw{};
    std::memset(&initialDraw, 0xa5, sizeof(initialDraw));
    setPayload(draw, initialDraw);
    for (GuardedBuffer *buffer : guarded)
      upload(graph, *buffer);

    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    for (GuardedBuffer *buffer : guarded)
      download(graph, *buffer);

    const auto actualHeap = downloadedPayloadVector<uint32_t>(heap);
    const auto actualPointers = downloadedPayloadVector<PlanetCbtPointerGPU>(pointers);
    const auto actualCounters = downloadedPayload<PlanetCbtCountersGPU>(counters);
    const auto actualDispatches = downloadedPayload<PlanetCbtDispatchesGPU>(dispatches);
    const auto actualDraw = downloadedPayload<PlanetCbtDrawIndirectGPU>(draw);
    if (actualHeap != expectedHeap)
      std::cerr << "Planet CBT heap mismatch at capacity " << capacity << " pattern " << patternName(pattern) << '\n';
    assert(actualHeap == expectedHeap);
    assert(actualCounters.activeCount == activeCount);
    assert(actualCounters.freeCount == freeCount);
    assert(actualCounters.requestedAllocations == 0u);
    assert(actualCounters.overflowCount == 0u);
    assert(actualCounters.splitCount == 0u);
    assert(actualCounters.mergeCount == 0u);
    assert(actualCounters.unchangedCount == 0u);
    assert(actualCounters.maximumDepth == 0u);

    const auto checkDispatch = [](const PlanetCbtDispatchIndirectGPU &dispatch, const uint32_t count)
    {
      assert(dispatch.groupCountX ==
          (count + kPlanetCbtWorkgroupSize - 1u) /
              kPlanetCbtWorkgroupSize);
      assert(dispatch.groupCountY == 1u);
      assert(dispatch.groupCountZ == 1u);
      assert(dispatch.itemCount == count);
    };
    checkDispatch(actualDispatches.activeDispatch, activeCount);
    checkDispatch(actualDispatches.freeDispatch, freeCount);
    checkDispatch(actualDispatches.pointerCacheDispatch, pointerCount);
    assert(actualDraw.vertexCount == activeCount * 3u);
    assert(actualDraw.instanceCount == 1u);
    assert(actualDraw.firstVertex == 0u);
    assert(actualDraw.firstInstance == 0u);

    for (uint32_t index = 0u; index < pointerCount; ++index)
    {
      const uint32_t expectedActive = index < activeCount ? planetCbtOneToBitID(expectedHeap, treeDepth, index) : kPlanetCbtNullSlot;
      const uint32_t expectedFree = index < freeCount ? planetCbtZeroToBitID(expectedHeap, treeDepth, index) : kPlanetCbtNullSlot;
      assert(actualPointers[index].activePoolSlot == expectedActive);
      assert(actualPointers[index].freePoolSlot == expectedFree);
    }
    for (uint32_t index = pointerCount; index < capacity; ++index)
    {
      assert(actualPointers[index].activePoolSlot == 0xa5a5a5a5u);
      assert(actualPointers[index].freePoolSlot == 0xa5a5a5a5u);
    }
    for (const GuardedBuffer *buffer : guarded)
      assert(canariesIntact(*buffer));
  }

  for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
    delete *iterator;
  for (auto iterator = allocatorPasses.rbegin(); iterator != allocatorPasses.rend(); ++iterator)
    delete *iterator;
  for (GuardedBuffer *buffer : guarded)
    destroyGuardedBuffer(graph, *buffer);
  std::cout << "  capacity " << capacity << " passed empty/full/alternating/sparse/random\n";
}

void testOwnedRootChain(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  RenderGraph graph(&rhi);
  uint32_t passIndex = 0u;
  auto *chain = new svrRendering::SparseVoxelPlanetCbtUpdateChain(graph, passIndex, 6u, "PlanetCbt.OwnedRoot");
  assert(chain->capacity() == 64u);
  const uint64_t defaultStorageBytes = uint64_t{kPlanetCbtDefaultCapacity} * sizeof(PlanetCbtBisectorGPU) + planetCbtHeapEntryCount(kPlanetCbtDefaultDepth) * sizeof(uint32_t) +
                                       uint64_t{kPlanetCbtDefaultCapacity} * sizeof(PlanetCbtPointerGPU) + sizeof(PlanetCbtCountersGPU) + sizeof(PlanetCbtDispatchesGPU) + sizeof(PlanetCbtDrawIndirectGPU) +
                                       uint64_t{kPlanetCbtRootBisectorCount} * sizeof(PlanetCbtRootTriangleGPU) + uint64_t{kPlanetCbtMaximumForcedRequests} * sizeof(PlanetCbtRequestGPU) +
                                       uint64_t{kPlanetCbtDefaultCapacity} * sizeof(uint32_t) + sizeof(PlanetCbtUpdateSettingsGPU);
  assert(defaultStorageBytes < 10u * 1024u * 1024u);
  const Buffer readback = graph.createBuffer({
    .name = "PlanetCbt.OwnedRoot.Readback",
    .size = sizeof(PlanetCbtCountersGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer poolReadback = graph.createBuffer({
    .name = "PlanetCbt.OwnedRoot.PoolReadback",
    .size = uint64_t{chain->capacity()} * sizeof(PlanetCbtBisectorGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>("PlanetCbt.OwnedRoot.Copy", passIndex++, chain->counters(), 0u, sizeof(PlanetCbtCountersGPU), readback, 0u, sizeof(PlanetCbtCountersGPU));
  auto *poolCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "PlanetCbt.OwnedRoot.PoolCopy", passIndex++, chain->pool(), 0u, uint64_t{chain->capacity()} * sizeof(PlanetCbtBisectorGPU), poolReadback, 0u, uint64_t{chain->capacity()} * sizeof(PlanetCbtBisectorGPU));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  PlanetCbtCountersGPU counters{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(counters),
      [&](const void *data)
      {
        std::memcpy(&counters, data, sizeof(counters));
      });
  assert(counters.activeCount == kPlanetCbtRootBisectorCount);
  assert(counters.freeCount == 4u);
  std::vector<PlanetCbtBisectorGPU> pool(chain->capacity());
  graph.bufferRead(
      poolReadback,
      0u,
      pool.size() * sizeof(PlanetCbtBisectorGPU),
      [&](const void *data)
      {
        std::memcpy(pool.data(), data, pool.size() * sizeof(PlanetCbtBisectorGPU));
      });
  for (uint32_t root = 0u; root < kPlanetCbtRootBisectorCount; ++root)
  {
    assert(pool[root].rootHalfedge == root);
    assert(pool[root].depth == 0u && pool[root].pathLow == 0u && pool[root].pathHigh == 0u);
    assert(pool[root].next < kPlanetCbtRootBisectorCount);
    assert(pool[root].previous < kPlanetCbtRootBisectorCount);
    assert(pool[root].twin < kPlanetCbtRootBisectorCount);
  }
  for (uint32_t slot = kPlanetCbtRootBisectorCount; slot < chain->capacity(); ++slot)
    assert(pool[slot].rootHalfedge == kPlanetCbtNullSlot);
  delete poolCopy;
  delete copy;
  graph.deleteBuffer(poolReadback);
  graph.deleteBuffer(readback);
  delete chain;
  std::cout << "  owned chain initialized 60 roots without CPU readback\n";
}

} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 16u * 1024u * 1024u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {}, true);
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);

  std::cout << "Planet CBT phase-2 GPU allocator\n";
  testOwnedRootChain(rhi);
  for (uint32_t depth = 6u; depth <= 17u; ++depth)
    runCapacityFixture(rhi, depth);
  std::cout << "Planet CBT phase-2 GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
