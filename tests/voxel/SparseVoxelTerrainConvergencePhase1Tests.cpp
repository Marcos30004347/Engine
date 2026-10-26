#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUJointPublication.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

uint64_t mix(const uint64_t value, const uint64_t data)
{
  uint64_t hash = value ^ (data + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u));
  hash ^= hash >> 33u;
  hash *= 0xff51afd7ed558ccdull;
  hash ^= hash >> 33u;
  return hash;
}

uint64_t terrainLocalPage(const svr::TerrainGPUPageIdentity &identity)
{
  uint64_t hash = identity.bundleSignature;
  hash = mix(hash, static_cast<uint32_t>(identity.chunkX));
  hash = mix(hash, static_cast<uint32_t>(identity.chunkZ));
  hash = mix(hash, identity.internalLOD);
  hash = mix(hash, identity.generatorRevision);
  return hash;
}

svr::SparseVoxelStreamedInstance makeInstance(const uint32_t instanceIndex, const svr::SparseVoxelPageProducer &producer, const svr::SparseVoxelPageId rootPage, const uint32_t firstRootNode, const uint32_t pageCount)
{
  svr::SparseVoxelStreamedInstance instance{};
  instance.instanceIndex = instanceIndex;
  instance.producer = producer;
  instance.rootPage = rootPage;
  instance.firstRootNode = firstRootNode;
  instance.rootNodeCount = 1u;
  instance.firstPage = 0u;
  instance.pageCount = pageCount;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return instance;
}

svr::SparseVoxelPhysicalPageAllocation makeAllocation(
    const svr::SparseVoxelPageId page,
    const uint64_t revision,
    const svr::SparseVoxelBufferRange hierarchyNodes,
    const svr::SparseVoxelBufferRange childIndices,
    const svr::SparseVoxelBufferRange brickNodes,
    const svr::SparseVoxelBufferRange materials,
    const svr::SparseVoxelBufferRange tree64Nodes)
{
  svr::SparseVoxelPhysicalPageAllocation allocation{};
  allocation.page = page;
  allocation.allocationRevision = revision;
  allocation.hierarchyNodes = hierarchyNodes;
  allocation.childIndices = childIndices;
  allocation.brickNodes = brickNodes;
  allocation.brickOccupancy = {brickNodes.first, brickNodes.count};
  allocation.materials = materials;
  allocation.materialCells = {materials.first, materials.count};
  allocation.tree64Nodes = tree64Nodes;
  allocation.tree64Materials = {tree64Nodes.first, tree64Nodes.count};
  return allocation;
}

svr::SparseVoxelPageInstallRewrite makeInstall(const svr::SparseVoxelPhysicalPageAllocation &allocation, const uint64_t producerRevision)
{
  svr::SparseVoxelPageInstallRewrite install{};
  install.page = allocation.page;
  install.allocation = allocation;
  install.requiredProducerRevision = producerRevision;
  install.rewrite.nodeIndex = allocation.hierarchyNodes.first;
  install.rewrite.firstChild = allocation.childIndices.empty() ? 0u : allocation.childIndices.first;
  install.rewrite.childCount = allocation.childIndices.count;
  install.rewrite.flags = 1u;
  return install;
}

svr::SparseVoxelPageUninstallRewrite makeUninstall(const svr::SparseVoxelPhysicalPageAllocation &allocation)
{
  svr::SparseVoxelPageUninstallRewrite uninstall{};
  uninstall.page = allocation.page;
  uninstall.retiredAllocationRevision = allocation.allocationRevision;
  uninstall.rewrite.nodeIndex = allocation.hierarchyNodes.first;
  uninstall.rewrite.firstChild = 0u;
  uninstall.rewrite.childCount = 0u;
  uninstall.rewrite.flags = 0u;
  return uninstall;
}

bool renderPassCanResolveResidentNode(
    const svr::SparseVoxelStreamedInstance &instance,
    const svr::SparseVoxelPageState state,
    const svr::SparseVoxelPhysicalPageAllocation &allocation,
    const svr::SparseVoxelPageInstallRewrite &install,
    const svr::SparseVoxelPageUninstallRewrite &uninstall)
{
  return instance.valid() && svr::isSparseVoxelPageResident(state) && allocation.hasRenderablePayload() && install.page == allocation.page && uninstall.page == allocation.page &&
         install.rewrite.nodeIndex != svr::kInvalidSparseVoxelIndex && uninstall.rewrite.nodeIndex == install.rewrite.nodeIndex;
}

void testNeutralPageIdsAndStates()
{
  std::cout << "neutral page ids and residency states\n";
  const svr::SparseVoxelPageId invalid{};
  const svr::SparseVoxelPageId rootPageZero{17u, 0u};
  const svr::SparseVoxelPageId childPage{17u, 1u};

  check(!invalid.valid(), "default page id is invalid");
  check(rootPageZero.valid(), "local page zero remains a valid streamed page");
  check(rootPageZero < childPage, "page id ordering is stable inside producer namespace");
  check(svr::isSparseVoxelPageResident(svr::SparseVoxelPageState::Resident), "resident state is render-visible through neutral helper");
  check(!svr::isSparseVoxelPageResident(svr::SparseVoxelPageState::Requested), "requested state is not render-visible through neutral helper");
  check(!svr::isSparseVoxelPageResident(svr::SparseVoxelPageState::EvictPending), "evict-pending state is not a selectable resident page");

  svr::SparseVoxelResidencyManager manager{};
  (void)manager;
  check(true, "SparseVoxelResidencyManager names the shared residency owner");
}

void testSharedDescriptionsForAllProducerKinds()
{
  std::cout << "source-neutral instance/page/allocation descriptions\n";

  const svr::SparseVoxelPageProducer cpuAsset{0x1001u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel, 7u, true, false, "prebuilt-object-vsvo"};
  const svr::SparseVoxelPageProducer generatedTerrain{0x2002u, svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain, 11u, false, true, "world-terrain-gpu"};
  const svr::SparseVoxelPageProducer generatedObject{0x3003u, svr::SparseVoxelPageProducerKind::GPUGeneratedObject, 3u, true, true, "future-dynamic-object"};

  svr::TerrainGPUPageIdentity terrainIdentity{};
  terrainIdentity.bundleSignature = 0x7465727261696eull;
  terrainIdentity.chunkX = -3;
  terrainIdentity.chunkZ = 5;
  terrainIdentity.internalLOD = 1u;
  terrainIdentity.generatorRevision = generatedTerrain.producerRevision;

  const std::vector<svr::SparseVoxelStreamedInstance> instances{
    makeInstance(0u, cpuAsset, {cpuAsset.producerId, 0u}, 0u, 2u),
    makeInstance(1u, generatedTerrain, {generatedTerrain.producerId, terrainLocalPage(terrainIdentity)}, 256u, 1u),
    makeInstance(2u, generatedObject, {generatedObject.producerId, 42u}, 1024u, 4u),
  };

  const std::vector<svr::SparseVoxelPhysicalPageAllocation> allocations{
    makeAllocation(instances[0].rootPage, 10u, {0u, 16u}, {64u, 8u}, {128u, 4u}, {512u, 4u}, {}),
    makeAllocation(instances[1].rootPage, 20u, {256u, 32u}, {512u, 16u}, {2048u, 8u}, {4096u, 8u}, {64u, 12u}),
    makeAllocation(instances[2].rootPage, 30u, {1024u, 48u}, {1536u, 24u}, {3072u, 12u}, {8192u, 12u}, {256u, 16u}),
  };

  check(
      std::all_of(
          instances.begin(),
          instances.end(),
          [](const auto &instance)
          {
            return instance.valid();
          }),
      "CPU asset, generated terrain and generated object instances share one streamed-instance shape");
  check(
      std::all_of(
          allocations.begin(),
          allocations.end(),
          [](const auto &allocation)
          {
            return allocation.valid() && allocation.hasRenderablePayload();
          }),
      "all producer kinds share one physical allocation shape");
  check(allocations[1].tree64Nodes.count != 0u, "terrain Tree64 sidecar fits the shared optional allocation ranges");

  for (size_t index = 0u; index < instances.size(); ++index)
  {
    const auto install = makeInstall(allocations[index], instances[index].producer.producerRevision);
    const auto uninstall = makeUninstall(allocations[index]);
    check(renderPassCanResolveResidentNode(instances[index], svr::SparseVoxelPageState::Resident, allocations[index], install, uninstall), "neutral render view resolves resident page " + std::to_string(index));
    check(!renderPassCanResolveResidentNode(instances[index], svr::SparseVoxelPageState::Requested, allocations[index], install, uninstall), "neutral render view rejects non-resident page " + std::to_string(index));
  }
}

void testInvalidAllocationIsSourceNeutral()
{
  std::cout << "source-neutral invalid allocation handling\n";
  const svr::SparseVoxelPageId page{99u, 1u};
  svr::SparseVoxelPhysicalPageAllocation allocation{};
  allocation.page = page;
  allocation.hierarchyNodes = {svr::kInvalidSparseVoxelIndex, 1u};

  check(!allocation.valid(), "non-empty ranges require a concrete first element regardless of source");
  check(!allocation.hasRenderablePayload(), "invalid allocation cannot be treated as renderable payload");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 1 contracts\n";
  testNeutralPageIdsAndStates();
  testSharedDescriptionsForAllProducerKinds();
  testInvalidAllocationIsSourceNeutral();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
