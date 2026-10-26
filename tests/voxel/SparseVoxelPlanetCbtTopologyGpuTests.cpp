#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/PlanetCbtReference.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp"

#include <algorithm>
#include <array>
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;
using Address = procedural::PlanetSurfaceBisectorAddress;

bool sameAddress(const Address &a, const Address &b)
{
  return a.rootHalfedge == b.rootHalfedge && a.depth == b.depth && a.path == b.path;
}

PlanetCbtRequestGPU requestFor(const Address &address, const uint32_t command)
{
  return {
    .rootHalfedge = address.rootHalfedge,
    .depth = address.depth,
    .pathLow = static_cast<uint32_t>(address.path),
    .pathHigh = static_cast<uint32_t>(address.path >> 32u),
    .command = command,
  };
}

uint32_t findSlot(const std::vector<PlanetCbtBisectorGPU> &pool, const std::vector<uint32_t> &heap, const uint32_t capacity, const Address &address)
{
  for (uint32_t slot = 0u; slot < capacity; ++slot)
    if (heap[capacity + slot] != 0u && sameAddress(planetCbtBisectorAddress(pool[slot]), address))
      return slot;
  return kPlanetCbtNullSlot;
}

struct TopologyFixture
{
  TopologyFixture(::rendering::backend::vulkan::VulkanRHI &rhi, const uint32_t depth, const char *name) : graph(&rhi), treeDepth(depth), capacity(planetCbtPoolCapacity(depth))
  {
    procedural::PlanetSurfaceHalfedgeMesh mesh;
    assert(buildPlanetCbtIcosahedron(mesh));
    assert(reference.initialize(mesh, depth));
    chain = new svrRendering::SparseVoxelPlanetCbtUpdateChain(graph, passIndex, depth, name);
    poolReadback = graph.createBuffer({
      .name = std::string(name) + ".PoolReadback",
      .size = uint64_t{capacity} * sizeof(PlanetCbtBisectorGPU),
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    heapReadback = graph.createBuffer({
      .name = std::string(name) + ".HeapReadback",
      .size = planetCbtHeapEntryCount(depth) * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    countersReadback = graph.createBuffer({
      .name = std::string(name) + ".CountersReadback",
      .size = sizeof(PlanetCbtCountersGPU),
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    copies.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        std::string(name) + ".CopyPool", passIndex++, chain->pool(), 0u, uint64_t{capacity} * sizeof(PlanetCbtBisectorGPU), poolReadback, 0u, uint64_t{capacity} * sizeof(PlanetCbtBisectorGPU)));
    copies.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        std::string(name) + ".CopyHeap", passIndex++, chain->heap(), 0u, planetCbtHeapEntryCount(depth) * sizeof(uint32_t), heapReadback, 0u, planetCbtHeapEntryCount(depth) * sizeof(uint32_t)));
    copies.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(std::string(name) + ".CopyCounters", passIndex++, chain->counters(), 0u, sizeof(PlanetCbtCountersGPU), countersReadback, 0u, sizeof(PlanetCbtCountersGPU)));
    graph.compile();
  }

  ~TopologyFixture()
  {
    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
    graph.deleteBuffer(countersReadback);
    graph.deleteBuffer(heapReadback);
    graph.deleteBuffer(poolReadback);
    delete chain;
  }

  void run(const std::vector<PlanetCbtRequestGPU> &requests = {})
  {
    PlanetCbtUpdateSettingsGPU settings{};
    settings.enableProjectedLod = 0u;
    settings.maximumDepth = kPlanetCbtMaximumSubdivisionDepth;
    run(settings, requests);
  }

  void run(const PlanetCbtUpdateSettingsGPU &settings, const std::vector<PlanetCbtRequestGPU> &requests = {})
  {
    assert(chain->update(settings, requests));
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    graph.bufferRead(
        poolReadback,
        0u,
        uint64_t{capacity} * sizeof(PlanetCbtBisectorGPU),
        [&](const void *data)
        {
          pool.resize(capacity);
          std::memcpy(pool.data(), data, pool.size() * sizeof(PlanetCbtBisectorGPU));
        });
    graph.bufferRead(
        heapReadback,
        0u,
        planetCbtHeapEntryCount(treeDepth) * sizeof(uint32_t),
        [&](const void *data)
        {
          heap.resize(planetCbtHeapEntryCount(treeDepth));
          std::memcpy(heap.data(), data, heap.size() * sizeof(uint32_t));
        });
    graph.bufferRead(
        countersReadback,
        0u,
        sizeof(counters),
        [&](const void *data)
        {
          std::memcpy(&counters, data, sizeof(counters));
        });
  }

  void compare() const
  {
    PlanetCbtTopologyError error{};
    assert(reference.validate(&error));
    std::vector<Address> gpuAddresses;
    gpuAddresses.reserve(counters.activeCount);
    for (uint32_t slot = 0u; slot < capacity; ++slot)
    {
      const bool active = heap[capacity + slot] != 0u;
      assert(active == (pool[slot].rootHalfedge != kPlanetCbtNullSlot));
      if (active)
        gpuAddresses.push_back(planetCbtBisectorAddress(pool[slot]));
    }
    const auto compareAddress = [](const Address &a, const Address &b)
    {
      if (a.rootHalfedge != b.rootHalfedge)
        return a.rootHalfedge < b.rootHalfedge;
      if (a.depth != b.depth)
        return a.depth < b.depth;
      return a.path < b.path;
    };
    std::sort(gpuAddresses.begin(), gpuAddresses.end(), compareAddress);
    const auto cpuAddresses = reference.activeAddresses();
    assert(gpuAddresses.size() == cpuAddresses.size());
    for (size_t index = 0u; index < cpuAddresses.size(); ++index)
      assert(sameAddress(gpuAddresses[index], cpuAddresses[index]));
    assert(counters.activeCount == reference.activeCount());
    assert(counters.freeCount == reference.freeCount());
    assert(heap[1u] == reference.activeCount());

    // Compare neighbor relations by stable address, independent of physical
    // allocation order, and require every GPU edge to be reciprocal.
    for (const Address &address : cpuAddresses)
    {
      const uint32_t gpuSlot = findSlot(pool, heap, capacity, address);
      const uint32_t cpuSlot = findSlot(reference.pool(), reference.heap(), capacity, address);
      assert(gpuSlot != kPlanetCbtNullSlot && cpuSlot != kPlanetCbtNullSlot);
      const std::array gpuNeighbors{pool[gpuSlot].next, pool[gpuSlot].previous, pool[gpuSlot].twin};
      const std::array cpuNeighbors{reference.pool()[cpuSlot].next, reference.pool()[cpuSlot].previous, reference.pool()[cpuSlot].twin};
      for (uint32_t edge = 0u; edge < gpuNeighbors.size(); ++edge)
      {
        assert(gpuNeighbors[edge] < capacity && cpuNeighbors[edge] < capacity);
        assert(heap[capacity + gpuNeighbors[edge]] != 0u);
        assert(sameAddress(planetCbtBisectorAddress(pool[gpuNeighbors[edge]]), planetCbtBisectorAddress(reference.pool()[cpuNeighbors[edge]])));
        const auto &neighbor = pool[gpuNeighbors[edge]];
        assert(neighbor.next == gpuSlot || neighbor.previous == gpuSlot || neighbor.twin == gpuSlot);
      }
    }
  }

  RenderGraph graph;
  uint32_t passIndex = 0u;
  uint32_t treeDepth = 0u;
  uint32_t capacity = 0u;
  PlanetCbtReference reference;
  svrRendering::SparseVoxelPlanetCbtUpdateChain *chain = nullptr;
  Buffer poolReadback{};
  Buffer heapReadback{};
  Buffer countersReadback{};
  std::vector<::rendering::gpgpu::CopyBufferPass *> copies;
  std::vector<PlanetCbtBisectorGPU> pool;
  std::vector<uint32_t> heap;
  PlanetCbtCountersGPU counters{};
};

void runRequest(TopologyFixture &fixture, const Address &address, const uint32_t command)
{
  if ((command & PlanetCbtCommand_Split) != 0u)
    fixture.reference.split(address);
  else
    fixture.reference.merge(address);
  fixture.run({requestFor(address, command)});
  fixture.compare();
}

void testScriptedAndRandom(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  TopologyFixture fixture(rhi, 8u, "PlanetCbt.Topology");
  fixture.run();
  fixture.compare();

  Address root = fixture.reference.activeAddresses().front();
  runRequest(fixture, root, PlanetCbtCommand_Split);
  Address child = procedural::planetSurfaceBisectorChild(root, 0u);
  runRequest(fixture, child, PlanetCbtCommand_Split);

  // Concurrent merge and split votes on the same leaf must refine: split wins.
  Address conflict = fixture.reference.activeAddresses().front();
  PlanetCbtReference candidate = fixture.reference;
  if (candidate.split(conflict))
  {
    fixture.reference = std::move(candidate);
    fixture.run({requestFor(conflict, PlanetCbtCommand_Merge), requestFor(conflict, PlanetCbtCommand_Split)});
    fixture.compare();
  }

  std::mt19937 random(0x3cb71a29u);
  for (uint32_t round = 0u; round < 10000u; ++round)
  {
    const auto addresses = fixture.reference.activeAddresses();
    const Address address = addresses[random() % addresses.size()];
    const bool refine = fixture.reference.activeCount() < 190u && address.depth < 12u && (random() % 100u) < 58u;
    const uint32_t command = refine ? PlanetCbtCommand_Split : PlanetCbtCommand_Merge;
    if (refine)
      fixture.reference.split(address);
    else
      fixture.reference.merge(address);
    fixture.run({requestFor(address, command)});
    // GPU neighbor validation runs every round. Stable-address and relation
    // parity are sampled throughout and checked unconditionally at the end.
    if ((round & 127u) == 0u)
      fixture.compare();
  }
  fixture.compare();
  std::cout << "  scripted split/merge, split-wins, and 10k rounds passed\n";
}

void testPoolExhaustion(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  TopologyFixture fixture(rhi, 6u, "PlanetCbt.Exhaustion");
  fixture.run();
  while (fixture.reference.freeCount() >= 2u)
  {
    bool split = false;
    for (const Address &address : fixture.reference.activeAddresses())
    {
      PlanetCbtReference candidate = fixture.reference;
      if (!candidate.split(address))
        continue;
      fixture.reference = std::move(candidate);
      fixture.run({requestFor(address, PlanetCbtCommand_Split)});
      fixture.compare();
      split = true;
      break;
    }
    assert(split);
  }
  assert(fixture.reference.freeCount() == 0u);
  const auto before = fixture.reference.activeAddresses();
  const Address rejected = before.front();
  assert(!fixture.reference.split(rejected));
  fixture.run({requestFor(rejected, PlanetCbtCommand_Split)});
  fixture.compare();
  assert(fixture.counters.overflowCount > 0u);
  std::cout << "  exhausted pool rejected atomically at a valid topology\n";
}

void testProjectedAreaCommands(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  TopologyFixture fixture(rhi, 8u, "PlanetCbt.ProjectedArea");
  const auto roots = fixture.reference.activeAddresses();
  for (const Address &root : roots)
    fixture.reference.split(root);

  PlanetCbtUpdateSettingsGPU refine{};
  refine.viewportFocal = {1920.0f, 1080.0f, 1.0f, 0.001f};
  refine.lodThresholds = {0.000001f, 1.0f, 0.5f, 0.0f};
  refine.enableProjectedLod = 1u;
  refine.maximumDepth = 1u;
  refine.lifecycleFlags =
      PlanetCbtLifecycleFlag_DisableVisibilityDecimation |
      PlanetCbtLifecycleFlag_DisableMinimumVisibleDepth;
  fixture.run(refine);
  fixture.compare();
  assert(fixture.reference.activeCount() == 120u);

  const auto leaves = fixture.reference.activeAddresses();
  for (const Address &leaf : leaves)
    fixture.reference.merge(leaf);
  PlanetCbtUpdateSettingsGPU coarsen = refine;
  coarsen.lodThresholds = {1.0e9f, 1.0f, 0.5f, 0.0f};
  fixture.run(coarsen);
  fixture.compare();
  assert(fixture.reference.activeCount() == kPlanetCbtRootBisectorCount);
  std::cout << "  projected-area refine and hysteretic coarsen passed\n";
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
  std::cout << "Planet CBT phase-3 GPU topology\n";
  testPoolExhaustion(rhi);
  testProjectedAreaCommands(rhi);
  testScriptedAndRandom(rhi);
  std::cout << "Planet CBT phase-3 GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
