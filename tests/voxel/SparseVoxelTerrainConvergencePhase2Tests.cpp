#include "voxel/svraster/SparseVoxelStreaming.hpp"

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

svr::SparseVoxelPageProducer producer(const uint64_t id, const svr::SparseVoxelPageProducerKind kind)
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = id;
  result.kind = kind;
  result.producerRevision = 4u;
  result.producesOnCPU = kind != svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.producesOnGPU = kind == svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.debugName = "phase2-producer";
  return result;
}

svr::SparseVoxelPhysicalPageAllocation requirements(
    const uint32_t hierarchyNodes,
    const uint32_t childIndices,
    const uint32_t tree64Nodes = 0u,
    const uint32_t renderPayloadItems = 0u,
    const uint32_t traceNodes = 0u,
    const uint32_t traceErrorWords = 0u,
    const uint32_t traceFormat = svr::kVoxelTraceFormatInvalid,
    const uint64_t geometryRevision = 0u)
{
  svr::SparseVoxelPhysicalPageAllocation result{};
  result.geometryRevision = geometryRevision;
  result.traceFormat = traceFormat;
  result.hierarchyNodes.count = hierarchyNodes;
  result.childIndices.count = childIndices;
  result.brickNodes.count = renderPayloadItems;
  result.brickOccupancy.count = renderPayloadItems;
  result.materials.count = renderPayloadItems;
  result.materialCells.count = renderPayloadItems;
  result.tree64Nodes.count = tree64Nodes;
  result.tree64Materials.count = tree64Nodes;
  result.traceNodes.count = traceNodes;
  result.traceErrorWords.count = traceErrorWords;
  return result;
}

svr::SparseVoxelPageInstallRewrite
installRewrite(const svr::SparseVoxelPageId page, const svr::SparseVoxelPhysicalPageAllocation &allocation, const uint32_t nodeIndex, const uint32_t childCount, const uint64_t producerRevision = 4u)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = page;
  rewrite.allocation = allocation;
  rewrite.requiredProducerRevision = producerRevision;
  rewrite.rewrite.nodeIndex = nodeIndex;
  rewrite.rewrite.firstChild = childCount == 0u ? 0u : allocation.childIndices.first;
  rewrite.rewrite.childCount = childCount;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

svr::SparseVoxelPageUninstallRewrite uninstallRewrite(const svr::SparseVoxelPageId page, const svr::SparseVoxelPhysicalPageAllocation &allocation, const uint32_t parentNodeIndex)
{
  svr::SparseVoxelPageUninstallRewrite rewrite{};
  rewrite.page = page;
  rewrite.retiredAllocationRevision = allocation.allocationRevision;
  rewrite.rewrite.nodeIndex = parentNodeIndex;
  rewrite.rewrite.firstChild = 0u;
  rewrite.rewrite.childCount = 0u;
  rewrite.rewrite.flags = 0u;
  return rewrite;
}

bool installPage(svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelPageId page, const svr::SparseVoxelPhysicalPageAllocation &allocation, const uint32_t generation, std::string &error)
{
  const auto rewrite = installRewrite(page, allocation, allocation.hierarchyNodes.first, allocation.childIndices.count);
  return table.installPage(page, allocation, {rewrite}, generation, &error);
}

void testAllocatorLifecycleAndSafeEviction()
{
  std::cout << "shared page table allocator lifecycle and safe eviction\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(
      table.configure(
          {
            6u,
            6u,
            6u,
            6u,
            6u,
            6u,
            4u,
            4u,
            2u,
            1u,
          },
          &error),
      "shared table configures per-buffer allocators: " + error);

  const auto cpu = producer(10u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel);
  const auto terrain = producer(20u, svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain);
  const svr::SparseVoxelPageId root{cpu.producerId, 0u};
  const svr::SparseVoxelPageId parent{cpu.producerId, 1u};
  const svr::SparseVoxelPageId child{cpu.producerId, 2u};
  const svr::SparseVoxelPageId replacement{terrain.producerId, 0u};

  check(table.addPage(root, cpu, {}, true, &error), "pinned root page is added: " + error);
  check(table.addPage(parent, cpu, {root}, false, &error), "parent page depends on root: " + error);
  check(table.addPage(child, cpu, {parent}, false, &error), "child page depends on parent: " + error);
  check(table.addPage(replacement, terrain, {parent}, false, &error), "terrain replacement can share the same dependency contract: " + error);

  svr::SparseVoxelPhysicalPageAllocation rootAllocation{};
  svr::SparseVoxelPhysicalPageAllocation parentAllocation{};
  svr::SparseVoxelPhysicalPageAllocation childAllocation{};
  check(table.allocatePage(root, requirements(1u, 1u, 0u, 1u), rootAllocation, &error), "root physical ranges allocate: " + error);
  check(table.allocatePage(parent, requirements(2u, 1u, 0u, 2u), parentAllocation, &error), "parent physical ranges allocate: " + error);
  check(
      table.allocatePage(
          child,
          requirements(
              2u,
              1u,
              2u,
              2u,
              2u,
              1u,
              svr::kVoxelTraceFormatCubic64,
              0xfeed'0001ull),
          childAllocation,
          &error),
      "child physical ranges allocate with Tree64 sidecar and direct trace view: " + error);
  svr::SparseVoxelPhysicalPageAllocation duplicateChildAllocation{};
  check(!table.allocatePage(child, requirements(1u, 1u), duplicateChildAllocation, &error), "pending page allocation cannot be duplicated before install");

  check(installPage(table, root, rootAllocation, 100u, error), "root installs through shared rewrites: " + error);
  check(installPage(table, parent, parentAllocation, 101u, error), "parent installs after root dependency: " + error);
  check(installPage(table, child, childAllocation, 102u, error), "child installs after parent dependency: " + error);

  const auto *childEntry = table.entry(child);
  check(
      childEntry != nullptr &&
          childEntry->traceable() &&
          childEntry->generation == 102u &&
          childEntry->geometryRevision == 0xfeed'0001ull &&
          childEntry->traceFormat == svr::kVoxelTraceFormatCubic64 &&
          childEntry->allocation.traceNodes.count == 2u &&
          childEntry->allocation.traceErrorWords.count == 1u,
      "direct trace view publishes with the same logical page generation and geometry revision");

  const auto *parentEntry = table.entry(parent);
  check(parentEntry != nullptr && parentEntry->residentDependentCount == 1u, "parent tracks one resident child dependency");

  const auto unsafeParentUninstall = uninstallRewrite(parent, parentAllocation, rootAllocation.hierarchyNodes.first);
  check(!table.uninstallPage(parent, {unsafeParentUninstall}, 7u, &error), "parent eviction is rejected while child is resident");

  const auto childUninstall = uninstallRewrite(child, childAllocation, parentAllocation.hierarchyNodes.first);
  check(table.uninstallPage(child, {childUninstall}, 10u, &error), "child uninstall rewrites parent fallback before retirement: " + error);
  check(table.entry(child) != nullptr && table.entry(child)->state == svr::SparseVoxelPageState::EvictPending, "child remains evict-pending until fence completes");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).retiring == childAllocation.hierarchyNodes.count, "child hierarchy range is retiring, not free");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::BrickNodes).retiring == childAllocation.brickNodes.count, "child brick range is retiring behind the same fence");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Materials).retiring == childAllocation.materials.count, "child material range is retiring behind the same fence");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Tree64Nodes).retiring == childAllocation.tree64Nodes.count, "child Tree64 sidecar range is retiring behind the same fence");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TraceNodes).retiring == childAllocation.traceNodes.count, "child trace-node range is retiring behind the same fence");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TraceErrorWords).retiring == childAllocation.traceErrorWords.count, "child trace-error sidecar range is retiring behind the same fence");
  check(table.entry(child) != nullptr && !table.entry(child)->traceable(), "evict-pending direct trace view is not selectable for rays");

  svr::SparseVoxelPhysicalPageAllocation earlyReplacement{};
  check(
      !table.allocatePage(
          replacement,
          requirements(
              2u,
              1u,
              0u,
              2u,
              2u,
              1u,
              svr::kVoxelTraceFormatCubic64,
              0xfeed'0002ull),
          earlyReplacement,
          &error),
      "retiring child trace/raster ranges cannot be reused before fence");

  check(table.reclaim(9u) == 0u, "fence-before-completion reclaims no ranges");
  check(table.reclaim(10u) >= 1u, "completion fence reclaims child ranges");
  check(table.entry(child) != nullptr && table.entry(child)->state == svr::SparseVoxelPageState::Unloaded, "child returns to unloaded after fence retirement");

  svr::SparseVoxelPhysicalPageAllocation replacementAllocation{};
  check(
      table.allocatePage(
          replacement,
          requirements(
              2u,
              1u,
              0u,
              2u,
              2u,
              1u,
              svr::kVoxelTraceFormatCubic64,
              0xfeed'0002ull),
          replacementAllocation,
          &error),
      "replacement page reuses reclaimed physical space: " + error);
  check(replacementAllocation.hierarchyNodes.first == childAllocation.hierarchyNodes.first, "hierarchy range is reused after fence completion");
  check(replacementAllocation.childIndices.first == childAllocation.childIndices.first, "child-index range is reused after fence completion");
  check(replacementAllocation.traceNodes.first == childAllocation.traceNodes.first, "trace-node range is reused only after fence completion");
  check(replacementAllocation.traceErrorWords.first == childAllocation.traceErrorWords.first, "trace-error range is reused only after fence completion");

  auto badRetiredRewrite = installRewrite(replacement, replacementAllocation, 5u, 0u);
  check(!table.installPage(replacement, replacementAllocation, {badRetiredRewrite}, 103u, &error), "install rewrite cannot target an unallocated hierarchy range");

  const auto validReplacementRewrite = installRewrite(replacement, replacementAllocation, replacementAllocation.hierarchyNodes.first, replacementAllocation.childIndices.count);
  check(table.installPage(replacement, replacementAllocation, {validReplacementRewrite}, 104u, &error), "replacement installs with active rewrite ranges: " + error);
  const auto *replacementEntry = table.entry(replacement);
  check(
      replacementEntry != nullptr &&
          replacementEntry->traceable() &&
          replacementEntry->generation == 104u &&
          replacementEntry->geometryRevision == 0xfeed'0002ull &&
          replacementEntry->traceFormat == svr::kVoxelTraceFormatCubic64,
      "replacement direct trace view installs atomically with render payload identity");

  const auto debug = table.debugReadback();
  check(debug.size() == 4u, "debug readback exposes every logical page");
  check(
      std::any_of(
          debug.begin(),
          debug.end(),
          [root](const auto &entry)
          {
            return entry.page == root && entry.pinned && entry.resident();
          }),
      "debug readback marks pinned root/coarse fallback as resident");
  check(table.installRewrites().size() == 4u, "install rewrite command storage records accepted installs");
  check(table.uninstallRewrites().size() == 1u, "uninstall rewrite command storage records accepted uninstalls");
}

void testBadDependencyAndRewriteRejection()
{
  std::cout << "shared page table dependency and rewrite validation\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({4u, 4u, 0u, 0u, 0u, 0u, 0u, 0u}, &error), "validation table configures: " + error);

  const auto cpu = producer(30u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel);
  const svr::SparseVoxelPageId root{cpu.producerId, 0u};
  const svr::SparseVoxelPageId child{cpu.producerId, 1u};
  check(table.addPage(root, cpu, {}, true, &error), "validation root page is added: " + error);
  check(table.addPage(child, cpu, {root}, false, &error), "validation child page is added: " + error);

  svr::SparseVoxelPhysicalPageAllocation badTraceAllocation{};
  check(
      !table.allocatePage(
          child,
          requirements(
              1u,
              1u,
              0u,
              0u,
              1u,
              0u,
              svr::kVoxelTraceFormatInvalid,
              99u),
          badTraceAllocation,
          &error),
      "trace allocation with invalid format is rejected before reserving ranges");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == 0u, "failed trace allocation leaves hierarchy allocator untouched");

  svr::SparseVoxelPhysicalPageAllocation childAllocation{};
  check(table.allocatePage(child, requirements(1u, 1u), childAllocation, &error), "child allocation can be reserved before dependency is resident: " + error);
  const auto childInstall = installRewrite(child, childAllocation, childAllocation.hierarchyNodes.first, childAllocation.childIndices.count);
  check(!table.installPage(child, childAllocation, {childInstall}, 1u, &error), "child install is rejected until root dependency is resident");

  svr::SparseVoxelPhysicalPageAllocation rootAllocation{};
  check(table.allocatePage(root, requirements(1u, 1u), rootAllocation, &error), "root allocation reserves after failed child install: " + error);
  auto badRewrite = installRewrite(root, rootAllocation, 3u, 0u);
  check(!table.installPage(root, rootAllocation, {badRewrite}, 2u, &error), "install rewrite cannot point at unallocated hierarchy nodes");

  const auto rootInstall = installRewrite(root, rootAllocation, rootAllocation.hierarchyNodes.first, rootAllocation.childIndices.count);
  check(table.installPage(root, rootAllocation, {rootInstall}, 3u, &error), "root installs with valid active rewrite: " + error);
}

void testProducedTracePayloadIdentity()
{
  std::cout << "produced direct trace payload identity validation\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(
      table.configure(
          {
            2u,
            2u,
            2u,
            2u,
            2u,
            2u,
            0u,
            0u,
            2u,
            1u,
          },
          &error),
      "produced-trace table configures trace allocators: " + error);

  const auto cpu = producer(35u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel);
  const svr::SparseVoxelPageId root{cpu.producerId, 0u};
  check(table.addPage(root, cpu, {}, true, &error), "produced-trace root page is added: " + error);

  svr::SparseVoxelProducedPagePayload staleGeometry{};
  staleGeometry.page = root;
  staleGeometry.expectedPageGeneration = 0u;
  staleGeometry.requiredProducerRevision = cpu.producerRevision;
  staleGeometry.payloadRevision = 10u;
  staleGeometry.geometryRevision = 11u;
  staleGeometry.requirements.page = root;
  staleGeometry.requirements.geometryRevision = 10u;
  staleGeometry.requirements.traceFormat = svr::kVoxelTraceFormatCubic64;
  staleGeometry.requirements.traceNodes.count = 1u;
  staleGeometry.installRewriteCount = 1u;
  staleGeometry.payloadComplete = true;
  svr::SparseVoxelReservedPagePayload reservation{};
  check(
      !table.reserveProducedPagePayload(staleGeometry, reservation, &error),
      "produced trace payload with mismatched geometry revision is rejected");

  svr::SparseVoxelProducedPagePayload produced = staleGeometry;
  produced.geometryRevision = 10u;
  produced.requirements.geometryRevision = 10u;
  produced.requirements.hierarchyNodes.count = 1u;
  produced.requirements.childIndices.count = 1u;
  produced.requirements.brickNodes.count = 1u;
  produced.requirements.brickOccupancy.count = 1u;
  produced.requirements.materials.count = 1u;
  produced.requirements.materialCells.count = 1u;
  check(table.reserveProducedPagePayload(produced, reservation, &error), "produced trace payload reserves all page views together: " + error);
  check(reservation.allocation.traceNodes.count == 1u && reservation.allocation.traceFormat == svr::kVoxelTraceFormatCubic64, "reservation carries trace format and node range");
  check(table.markPayloadCopied(reservation, 44u, &error), "produced trace payload marks copied after active range validation: " + error);
  const auto install = installRewrite(root, reservation.allocation, reservation.allocation.hierarchyNodes.first, reservation.allocation.childIndices.count, cpu.producerRevision);
  check(table.installCopiedPagePayload(reservation, {install}, &error), "produced trace payload installs through shared publication boundary: " + error);

  const auto *entry = table.entry(root);
  check(
      entry != nullptr &&
          entry->resident() &&
          entry->traceable() &&
          entry->generation == produced.payloadRevision &&
          entry->geometryRevision == produced.geometryRevision &&
          entry->traceFormat == svr::kVoxelTraceFormatCubic64,
      "installed produced payload exposes trace identity matching page generation/revision");
}

void testPackedOneMiBPageSlotsAndPageletDependencies()
{
  std::cout << "1 MiB packed physical page slots and pagelet dependencies\n";

  constexpr uint32_t nodesPerTerrainPagelet = 8192u;
  static_assert(
      nodesPerTerrainPagelet * sizeof(svr::SparseVoxelTree64NodePaddedGPU) ==
      svr::kSparseVoxelPackedPageSlotBytes / 8u);

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(
      table.configure(
          {
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            nodesPerTerrainPagelet * 10u,
            0u,
            0u,
            0u,
            0u,
            svr::kSparseVoxelPackedPageSlotBytes,
          },
          &error),
      "packed-slot table configures with 1 MiB physical slots: " + error);

  const auto terrain = producer(55u, svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain);
  std::vector<svr::SparseVoxelPageId> pages;
  for (uint32_t index = 0u; index < 10u; ++index)
    pages.push_back({terrain.producerId, index});
  check(table.addPage(pages[0], terrain, {}, false, &error), "packed parent page registers: " + error);
  check(table.addPage(pages[1], terrain, {pages[0]}, false, &error), "packed child page records parent dependency: " + error);
  for (uint32_t index = 2u; index < pages.size(); ++index)
    check(table.addPage(pages[index], terrain, {}, false, &error), "packed independent page registers: " + error);

  std::vector<svr::SparseVoxelPageId> closure;
  check(table.collectPageRequestClosure(pages[1], closure, &error), "trace request closure expands missing dependencies: " + error);
  check(closure == (std::vector<svr::SparseVoxelPageId>{pages[0], pages[1]}), "missing child trace request is dependency-first before any pagelet is resident");

  auto tree64Pagelet = [](const uint32_t tree64Nodes)
  {
    svr::SparseVoxelPhysicalPageAllocation req{};
    req.tree64Nodes.count = tree64Nodes;
    return req;
  };

  std::vector<svr::SparseVoxelPhysicalPageAllocation> allocations(10u);
  for (uint32_t index = 0u; index < 8u; ++index)
  {
    check(
        table.allocatePage(pages[index], tree64Pagelet(nodesPerTerrainPagelet), allocations[index], &error),
        "packed pagelet allocates into the first 1 MiB slot: " + error);
    check(allocations[index].packedSlot == 0u, "pagelet remains in slot zero while space is available");
    check(allocations[index].packedPagelet == index, "pagelet index is append ordered inside the slot");
    check(allocations[index].packedSlotOffsetBytes == index * (svr::kSparseVoxelPackedPageSlotBytes / 8u), "pagelet byte offset is contiguous and padded");
    check(allocations[index].packedSlotBytes == svr::kSparseVoxelPackedPageSlotBytes / 8u, "pagelet budgets one eighth of the 1 MiB slot");
    check(
        table.installPage(pages[index], allocations[index], {}, 100u + index, &error),
        "packed pagelet installs without exposing a partial slot: " + error);
    if (index == 0u)
    {
      check(table.collectPageRequestClosure(pages[1], closure, &error), "trace request closure updates after parent install: " + error);
      check(closure == (std::vector<svr::SparseVoxelPageId>{pages[1]}), "resident parent is omitted from later child trace request closure");
    }
  }

  auto slots = table.packedPageSlots();
  check(slots.size() == 1u, "eight current Tree64 pagelets fill exactly one packed slot");
  check(slots[0].usedBytes == svr::kSparseVoxelPackedPageSlotBytes, "slot zero reaches the 1 MiB byte budget exactly");
  check(slots[0].pageletCount == 8u && slots[0].residentPageletCount == 8u, "slot zero tracks all resident pagelets");

  check(
      table.allocatePage(pages[8], tree64Pagelet(nodesPerTerrainPagelet), allocations[8], &error),
      "ninth pagelet spills into a second packed slot: " + error);
  check(allocations[8].packedSlot == 1u && allocations[8].packedSlotOffsetBytes == 0u, "second slot starts at offset zero");
  check(table.installPage(pages[8], allocations[8], {}, 108u, &error), "second-slot pagelet installs: " + error);

  svr::SparseVoxelPackedPageSlotDecision decision{};
  check(!table.canReusePackedPageSlot(0u, &decision, &error), "slot zero cannot be reused while pagelets are resident");
  check(decision.residentPageletCount == 8u && decision.livePageletCount == 8u, "slot decision exposes live resident pagelet counts");

  svr::SparseVoxelPageEvictionDecision parentEviction{};
  check(!table.canEvictPage(pages[0], &parentEviction, &error), "parent pagelet cannot evict while child pagelet is live");
  check(parentEviction.liveDependentCount == 1u, "parent eviction decision sees the child dependency");

  check(table.uninstallPage(pages[1], {}, 5u, &error), "child pagelet enters fenced retirement: " + error);
  check(!table.canEvictPage(pages[0], &parentEviction, &error), "parent still cannot evict while child pagelet is retiring");
  check(parentEviction.liveDependentCount == 1u, "retiring child still protects the parent slot/pagelet");
  check(table.reclaim(5u) >= 1u, "retired child pagelet reclaims after its fence");
  check(table.canEvictPage(pages[0], &parentEviction, &error), "parent pagelet becomes evictable after child reclaim: " + error);

  for (const uint32_t index : {0u, 2u, 3u, 4u, 5u, 6u, 7u})
    check(table.uninstallPage(pages[index], {}, 6u, &error), "remaining first-slot pagelet retires: " + error);
  check(!table.canReusePackedPageSlot(0u, &decision, &error), "slot zero is not reusable until retirement fence completes");
  check(decision.retiringPageletCount == 7u, "slot decision reports retiring pagelets before fence completion");
  check(table.reclaim(6u) >= 1u, "remaining first-slot pagelets reclaim after fence");
  check(table.canReusePackedPageSlot(0u, &decision, &error), "slot zero becomes reusable only when every pagelet is unloaded: " + error);
  check(decision.livePageletCount == 0u && decision.usedBytes == svr::kSparseVoxelPackedPageSlotBytes, "reusable slot keeps prior fill for diagnostics until reused");

  check(
      table.allocatePage(pages[9], tree64Pagelet(nodesPerTerrainPagelet), allocations[9], &error),
      "new pagelet reuses the fully retired first slot: " + error);
  check(allocations[9].packedSlot == 0u && allocations[9].packedPagelet == 0u && allocations[9].packedSlotOffsetBytes == 0u, "reused slot is reset before new pagelet placement");

  const svr::SparseVoxelPageId oversized{terrain.producerId, 99u};
  check(table.addPage(oversized, terrain, {}, false, &error), "oversized page registers before allocation validation: " + error);
  svr::SparseVoxelPhysicalPageAllocation tooLarge = tree64Pagelet(nodesPerTerrainPagelet * 8u + 1u);
  svr::SparseVoxelPhysicalPageAllocation rejected{};
  check(!table.allocatePage(oversized, tooLarge, rejected, &error), "oversized pagelet is rejected before crossing the 1 MiB slot limit");
}

void testSmallerPackedSlotsPreserveMultiSlotDependencies()
{
  std::cout << "smaller packed slots preserve multi-slot dependency safety\n";

  constexpr uint32_t nodesPerTerrainPagelet = 8192u;
  constexpr uint32_t onePageletSlotBytes =
      nodesPerTerrainPagelet * sizeof(svr::SparseVoxelTree64NodePaddedGPU);

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(
      table.configure(
          {
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            nodesPerTerrainPagelet * 3u,
            0u,
            0u,
            0u,
            0u,
            onePageletSlotBytes,
          },
          &error),
      "smaller-slot table configures one pagelet per physical slot: " + error);

  const auto terrain = producer(56u, svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain);
  const svr::SparseVoxelPageId parent{terrain.producerId, 0u};
  const svr::SparseVoxelPageId child{terrain.producerId, 1u};
  const svr::SparseVoxelPageId grandchild{terrain.producerId, 2u};
  check(table.addPage(parent, terrain, {}, false, &error), "smaller-slot parent registers: " + error);
  check(table.addPage(child, terrain, {parent}, false, &error), "smaller-slot child depends on parent: " + error);
  check(table.addPage(grandchild, terrain, {child}, false, &error), "smaller-slot grandchild depends on child: " + error);

  auto tree64Pagelet = []()
  {
    svr::SparseVoxelPhysicalPageAllocation req{};
    req.tree64Nodes.count = nodesPerTerrainPagelet;
    return req;
  };

  svr::SparseVoxelPhysicalPageAllocation parentAllocation{};
  svr::SparseVoxelPhysicalPageAllocation childAllocation{};
  svr::SparseVoxelPhysicalPageAllocation grandchildAllocation{};
  check(table.allocatePage(parent, tree64Pagelet(), parentAllocation, &error), "parent allocates into first small slot: " + error);
  check(table.installPage(parent, parentAllocation, {}, 201u, &error), "parent installs in first small slot: " + error);
  check(table.allocatePage(child, tree64Pagelet(), childAllocation, &error), "child allocates into second small slot: " + error);
  check(table.installPage(child, childAllocation, {}, 202u, &error), "child installs in second small slot: " + error);
  check(table.allocatePage(grandchild, tree64Pagelet(), grandchildAllocation, &error), "grandchild allocates into third small slot: " + error);
  check(table.installPage(grandchild, grandchildAllocation, {}, 203u, &error), "grandchild installs in third small slot: " + error);

  check(parentAllocation.packedSlot == 0u, "parent occupies slot zero");
  check(childAllocation.packedSlot == 1u, "child spills into slot one when the smaller slot is full");
  check(grandchildAllocation.packedSlot == 2u, "grandchild spills into slot two when every slot fits one pagelet");
  const auto slots = table.packedPageSlots();
  check(slots.size() == 3u, "smaller slot policy creates one physical slot per pagelet");
  check(slots[0].slotBytes == onePageletSlotBytes && slots[0].usedBytes == onePageletSlotBytes, "small slot reports exact byte budget and fill");

  svr::SparseVoxelPackedPageSlotDecision slotDecision{};
  check(!table.canReusePackedPageSlot(parentAllocation.packedSlot, &slotDecision, &error), "parent slot cannot be reused while child chain is live");
  check(slotDecision.liveDependentCount == 1u, "slot decision sees live dependent pagelet in another slot");

  svr::SparseVoxelPageEvictionDecision eviction{};
  check(!table.canEvictPage(child, &eviction, &error), "middle pagelet cannot evict while grandchild is resident");
  check(eviction.liveDependentCount == 1u, "middle pagelet eviction sees cross-slot grandchild dependency");
  check(table.uninstallPage(grandchild, {}, 11u, &error), "grandchild retires first: " + error);
  check(table.reclaim(11u) >= 1u, "grandchild slot reclaims after fence");
  check(table.canEvictPage(child, &eviction, &error), "middle pagelet becomes evictable after grandchild reclaim: " + error);
  check(table.uninstallPage(child, {}, 12u, &error), "child retires after dependent reclaim: " + error);
  check(table.reclaim(12u) >= 1u, "child slot reclaims after fence");
  check(table.canEvictPage(parent, &eviction, &error), "parent pagelet becomes evictable after child reclaim: " + error);
}

void testRetiredRewriteRejection()
{
  std::cout << "shared page table retired range rewrite validation\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({3u, 3u, 0u, 0u, 0u, 0u, 0u, 0u}, &error), "retired-range table configures: " + error);

  const auto cpu = producer(40u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel);
  const svr::SparseVoxelPageId root{cpu.producerId, 0u};
  const svr::SparseVoxelPageId victim{cpu.producerId, 1u};
  const svr::SparseVoxelPageId candidate{cpu.producerId, 2u};
  check(table.addPage(root, cpu, {}, true, &error), "retired-range root is added: " + error);
  check(table.addPage(victim, cpu, {root}, false, &error), "retired-range victim is added: " + error);
  check(table.addPage(candidate, cpu, {root}, false, &error), "retired-range candidate is added: " + error);

  svr::SparseVoxelPhysicalPageAllocation rootAllocation{};
  svr::SparseVoxelPhysicalPageAllocation victimAllocation{};
  svr::SparseVoxelPhysicalPageAllocation candidateAllocation{};
  check(table.allocatePage(root, requirements(1u, 1u), rootAllocation, &error), "retired-range root allocates: " + error);
  check(installPage(table, root, rootAllocation, 1u, error), "retired-range root installs: " + error);
  check(table.allocatePage(victim, requirements(1u, 1u), victimAllocation, &error), "retired-range victim allocates: " + error);
  check(installPage(table, victim, victimAllocation, 2u, error), "retired-range victim installs: " + error);
  check(table.uninstallPage(victim, {uninstallRewrite(victim, victimAllocation, rootAllocation.hierarchyNodes.first)}, 20u, &error), "victim enters fenced retirement: " + error);

  check(table.allocatePage(candidate, requirements(1u, 1u), candidateAllocation, &error), "candidate allocates from still-free space while victim retires: " + error);
  const auto retiredTarget = installRewrite(candidate, candidateAllocation, victimAllocation.hierarchyNodes.first, 0u);
  check(!table.installPage(candidate, candidateAllocation, {retiredTarget}, 3u, &error), "install rewrite cannot target a fenced retired hierarchy range");

  const auto activeTarget = installRewrite(candidate, candidateAllocation, candidateAllocation.hierarchyNodes.first, candidateAllocation.childIndices.count);
  check(table.installPage(candidate, candidateAllocation, {activeTarget}, 4u, &error), "candidate installs when rewrite targets its active range: " + error);
}

void testDirtyPageTableRanges()
{
  std::cout << "shared page table dirty upload ranges\n";
  const auto coalesced = svr::coalesceSparseVoxelPageTableDirtyRanges({7u, 2u, 1u, 9u, 10u, 2u});
  check(
      coalesced.size() == 3u &&
          coalesced[0].first == 1u && coalesced[0].count == 2u &&
          coalesced[1].first == 7u && coalesced[1].count == 1u &&
          coalesced[2].first == 9u && coalesced[2].count == 2u,
      "dirty page-table indices coalesce into sorted upload ranges");

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(
      table.configure(
          {
            4u,
            4u,
            4u,
            4u,
            4u,
            4u,
          },
          &error),
      "dirty-range table configures: " + error);
  const auto cpu = producer(60u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel);
  const svr::SparseVoxelPageId root{cpu.producerId, 0u};
  const svr::SparseVoxelPageId child{cpu.producerId, 1u};
  check(table.addPage(root, cpu, {}, true, &error), "dirty-range root is added: " + error);
  check(table.addPage(child, cpu, {root}, false, &error), "dirty-range child is added: " + error);

  svr::SparseVoxelPhysicalPageAllocation rootAllocation{};
  svr::SparseVoxelPhysicalPageAllocation childAllocation{};
  check(table.allocatePage(root, requirements(1u, 1u, 0u, 1u), rootAllocation, &error), "dirty-range root allocates: " + error);
  check(installPage(table, root, rootAllocation, 11u, error), "dirty-range root installs: " + error);
  check(
      table.dirtyPageTableRanges().size() == 1u &&
          table.dirtyPageTableRanges()[0].first == 0u &&
          table.dirtyPageTableRanges()[0].count == 1u,
      "root install records only the touched page-table row");
  table.clearDirtyPageTableRanges();
  check(table.dirtyPageTableRanges().empty(), "dirty range list clears after the upload pass consumes it");

  check(table.allocatePage(child, requirements(1u, 1u, 0u, 1u), childAllocation, &error), "dirty-range child allocates: " + error);
  check(installPage(table, child, childAllocation, 12u, error), "dirty-range child installs: " + error);
  check(
      table.dirtyPageTableRanges().size() == 1u &&
          table.dirtyPageTableRanges()[0].first == 1u &&
          table.dirtyPageTableRanges()[0].count == 1u,
      "child install dirties its row without forcing a whole page-table upload");
  table.clearDirtyPageTableRanges();

  check(
      table.uninstallPage(child, {uninstallRewrite(child, childAllocation, rootAllocation.hierarchyNodes.first)}, 50u, &error),
      "dirty-range child uninstalls: " + error);
  check(
      table.dirtyPageTableRanges().size() == 1u &&
          table.dirtyPageTableRanges()[0].first == 1u &&
          table.dirtyPageTableRanges()[0].count == 1u,
      "uninstall dirties only the evicted row");
  table.clearDirtyPageTableRanges();
  table.reclaim(50u);
  check(
      table.dirtyPageTableRanges().size() == 1u &&
          table.dirtyPageTableRanges()[0].first == 1u &&
          table.dirtyPageTableRanges()[0].count == 1u,
      "fenced reclaim dirties only the retired row for the final zero/install-state upload");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 2 shared page table\n";
  testAllocatorLifecycleAndSafeEviction();
  testBadDependencyAndRewriteRejection();
  testProducedTracePayloadIdentity();
  testPackedOneMiBPageSlotsAndPageletDependencies();
  testSmallerPackedSlotsPreserveMultiSlotDependencies();
  testRetiredRewriteRejection();
  testDirtyPageTableRanges();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
