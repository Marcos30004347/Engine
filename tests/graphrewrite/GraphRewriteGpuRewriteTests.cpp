// Phase 3.12b: the rewrite on the GPU.
//
// Removal is a tombstone bit set with atomicOr - idempotent, so "any copy is
// enough" falls out rather than needing a claim pass. Compaction is an INDIRECT
// dispatch over the dirty-page list, so pages nothing touched are never visited.

#include "TestHarness.hpp"
#include "graphrewrite/rendering/GraphRewritePass.hpp"
#include "graphrewrite/runtime/StreamingManager.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <cstring>
#include <memory>
#include <set>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::rendering;
using namespace graphrewrite::testing;

namespace {

struct Harness
{
  std::unique_ptr<::rendering::RenderGraph> graph;
  std::unique_ptr<GpuPagePool> pool;
  compiler::EdgeTypeTable types;
  EdgePageStore store;
  GraphRewritePass *pass = nullptr;

  ::rendering::Buffer table;
  ::rendering::Buffer countersStaging;
  ::rendering::Buffer dirtyStaging;
  ::rendering::Buffer deltaStaging;
  ::rendering::Buffer poolStaging;

  static constexpr uint32_t tableWords = 4096u;
};

// Builds a graph with one edge type, uploads every page, and wires the pass.
void buildHarness(::rendering::RHI *rhi, Harness &out, const std::vector<Edge> &edges)
{
  const compiler::EdgeTypeId type = out.types.intern("? a ?", 2u);

  out.types.mutableType(type).usedForward = true;

  out.store.configure(out.types);

  for (const Edge &edge : edges)
  {
    out.store.addEdge(type, edge);
  }

  out.graph = std::make_unique<::rendering::RenderGraph>(rhi);

  GpuPagePoolSettings poolSettings;

  poolSettings.slotCount = 64u;

  out.pool = std::make_unique<GpuPagePool>(*out.graph, poolSettings);
  out.pool->configure(out.store);

  out.table = out.graph->createBuffer(::rendering::BufferInfo{
      .name = "test.table",
      .size = Harness::tableWords * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
  });

  GraphRewritePass::Settings passSettings;

  passSettings.pool = out.pool.get();
  passSettings.tableBuffer = out.table;
  passSettings.tableBytes = Harness::tableWords * sizeof(uint32_t);
  passSettings.maxRows = 256u;

  out.pass = out.graph->registerPass<GraphRewritePass>("graphrewrite.rewrite", 0u, passSettings);

  const auto pull = [&out](const char *name, const uint64_t size) {
    return out.graph->createBuffer(::rendering::BufferInfo{
        .name = name,
        .size = size,
        .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
  };

  out.countersStaging = pull("test.counters", sizeof(RewriteCounters));
  out.dirtyStaging = pull("test.dirty", out.pass->dirtyPagesBytes());
  out.deltaStaging = pull("test.delta", out.pass->deltaBytes());
  out.poolStaging = pull("test.pool", out.pool->pageBytes() * poolSettings.slotCount);

  out.graph->registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyCounters", 1u, out.pass->countersBuffer(), 0u,
                                                             sizeof(RewriteCounters), out.countersStaging, 0u, sizeof(RewriteCounters));
  out.graph->registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyDirty", 2u, out.pass->dirtyPagesBuffer(), 0u,
                                                             out.pass->dirtyPagesBytes(), out.dirtyStaging, 0u, out.pass->dirtyPagesBytes());
  out.graph->registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyDelta", 3u, out.pass->deltaBuffer(), 0u, out.pass->deltaBytes(),
                                                             out.deltaStaging, 0u, out.pass->deltaBytes());
  out.graph->registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyPool", 4u, out.pool->pagesBuffer(), 0u,
                                                             out.pool->pageBytes() * poolSettings.slotCount, out.poolStaging, 0u,
                                                             out.pool->pageBytes() * poolSettings.slotCount);

  out.graph->compile();

  // Every page resident, so the removal scan can find anything.
  for (uint32_t position = 0u; position < out.store.partitionAt(0u).pageCount(); ++position)
  {
    uint32_t slot = notResident;

    out.pool->acquireSlot(slot);
    out.pool->upload(out.store, 0u, position, slot);
  }

  out.pool->flushMetadata();
}

void runGraph(Harness &harness, const uint32_t frame)
{
  ::rendering::RenderGraph::Frame gpuFrame;
  ::rendering::RenderGraph::Overrides overrides;

  harness.graph->setCurrentFrameIndex(frame % harness.graph->getMaxFramesInFlight());
  harness.graph->run(gpuFrame, overrides);
  harness.graph->waitFrame(gpuFrame);
}

RewriteCounters readCounters(Harness &harness)
{
  RewriteCounters counters;

  harness.graph->bufferRead(harness.countersStaging, 0u, sizeof(counters),
                            [&counters](const void *data) { std::memcpy(&counters, data, sizeof(counters)); });

  return counters;
}

std::vector<Edge> readPage(Harness &harness, const uint32_t slot)
{
  std::vector<Edge> all(harness.pool->settings().slotCount * pageEdges);

  harness.graph->bufferRead(harness.poolStaging, 0u, harness.pool->pageBytes() * harness.pool->settings().slotCount,
                            [&all](const void *data) { std::memcpy(all.data(), data, all.size() * sizeof(Edge)); });

  return std::vector<Edge>(all.begin() + static_cast<long>(slot) * pageEdges,
                           all.begin() + static_cast<long>(slot + 1u) * pageEdges);
}

void removalTombstonesAndCompacts(::rendering::RHI *rhi)
{
  Harness harness;

  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 20u; ++i)
  {
    edges.push_back(Edge{i, i * 10u});
  }

  buildHarness(rhi, harness, edges);

  checkEqual(harness.store.partitionAt(0u).pageCount(), 1u, "twenty edges fit in one page");

  // A table with three rows; columns 0 and 1 hold the edge endpoints of the
  // edges to remove.
  const std::vector<uint32_t> removeOrigins = {3u, 7u, 11u};

  std::vector<uint32_t> table(Harness::tableWords, 0u);

  const uint32_t stride = 16u;

  for (uint32_t row = 0u; row < removeOrigins.size(); ++row)
  {
    table[0u * stride + row] = removeOrigins[row];       // column 0: origin
    table[1u * stride + row] = removeOrigins[row] * 10u; // column 1: target
  }

  harness.graph->bufferWrite(harness.table, 0u, table.size() * sizeof(uint32_t), table.data());

  harness.pass->uploadRuleEdges({RuleEdgeDesc{0u, 1u, 0u, 0u}});
  harness.pass->clearMarks();

  RewriteParams params;

  params.rowCount = static_cast<uint32_t>(removeOrigins.size());
  params.tableWordBase = 0u;
  params.tableStride = stride;
  params.removalCount = 1u;
  params.removalBase = 0u;
  params.insertionCount = 0u;
  params.deltaCapacity = 65536u;
  params.dirtyCapacity = 4096u;

  harness.pass->setParams(params);

  runGraph(harness, 0u);

  const RewriteCounters counters = readCounters(harness);

  checkEqual(counters.removed, 3u, "three edges were tombstoned");
  checkEqual(counters.dirtyCount, 1u, "and they all live in one page, marked dirty once");
  checkEqual(counters.overflow, 0u, "nothing overflowed");

  const std::vector<Edge> page = readPage(harness, harness.pool->slotOf(0u, 0u));

  // The page must now hold the seventeen survivors, still in order, with no gaps.
  std::vector<Edge> survivors(page.begin(), page.begin() + 17);

  bool ordered = true;
  std::set<uint32_t> present;

  for (const Edge &edge : survivors)
  {
    present.insert(edge.origin);
  }

  for (size_t i = 1u; i < survivors.size(); ++i)
  {
    ordered = ordered && survivors[i - 1u].origin <= survivors[i].origin;
  }

  check(ordered, "the compacted page is still sorted");
  checkEqual(present.size(), size_t(17), "seventeen distinct survivors");

  for (const uint32_t removed : removeOrigins)
  {
    check(present.count(removed) == 0u, "removed edge " + std::to_string(removed) + " is gone from the page");
  }
}

void removingTheSameEdgeTwiceIsIdempotent(::rendering::RHI *rhi)
{
  Harness harness;

  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 10u; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  buildHarness(rhi, harness, edges);

  // Two rows naming the SAME edge: "any copy is enough".
  std::vector<uint32_t> table(Harness::tableWords, 0u);

  const uint32_t stride = 16u;

  for (uint32_t row = 0u; row < 2u; ++row)
  {
    table[0u * stride + row] = 4u;
    table[1u * stride + row] = 4u;
  }

  harness.graph->bufferWrite(harness.table, 0u, table.size() * sizeof(uint32_t), table.data());

  harness.pass->uploadRuleEdges({RuleEdgeDesc{0u, 1u, 0u, 0u}});
  harness.pass->clearMarks();

  RewriteParams params;

  params.rowCount = 2u;
  params.tableStride = stride;
  params.removalCount = 1u;
  params.deltaCapacity = 65536u;
  params.dirtyCapacity = 4096u;

  harness.pass->setParams(params);

  runGraph(harness, 0u);

  const RewriteCounters counters = readCounters(harness);

  checkEqual(counters.removed, 1u, "two rows naming one edge remove it exactly once");
  checkEqual(counters.dirtyCount, 1u, "and mark its page dirty exactly once");

  const std::vector<Edge> page = readPage(harness, harness.pool->slotOf(0u, 0u));

  uint32_t found = 0u;

  for (uint32_t i = 0u; i < 9u; ++i)
  {
    if (page[i].origin == 4u)
    {
      ++found;
    }
  }

  checkEqual(found, 0u, "the edge is gone");
}

void insertionsGoToTheDelta(::rendering::RHI *rhi)
{
  Harness harness;

  buildHarness(rhi, harness, {{1u, 2u}, {3u, 4u}});

  std::vector<uint32_t> table(Harness::tableWords, 0u);

  const uint32_t stride = 16u;

  // Two rows, each binding vertices 0 and 1 of the rule.
  table[0u * stride + 0u] = 10u;
  table[1u * stride + 0u] = 20u;
  table[0u * stride + 1u] = 30u;
  table[1u * stride + 1u] = 40u;

  harness.graph->bufferWrite(harness.table, 0u, table.size() * sizeof(uint32_t), table.data());

  // One insertion using the two matched vertices, and one whose target is the
  // row's first FRESH vertex - named by the `fresh` code, not by a column: a
  // column number is a table column now, whatever its value.
  harness.pass->uploadRuleEdges({RuleEdgeDesc{0u, 1u, 0u, 0u}, RuleEdgeDesc{0u, 0u, 0u, freshCode(0u, 1u)}});
  harness.pass->clearMarks();

  RewriteParams params;

  params.rowCount = 2u;
  params.tableStride = stride;
  params.removalCount = 0u;
  params.insertionCount = 2u;
  params.insertionBase = 0u;
  params.matchedVertices = 2u;
  params.totalVertices = 3u;
  params.vertexBase = 1000u;
  params.deltaCapacity = 65536u;
  params.dirtyCapacity = 4096u;

  harness.pass->setParams(params);

  runGraph(harness, 0u);

  const RewriteCounters counters = readCounters(harness);

  checkEqual(counters.deltaCount, 4u, "two rows times two insertions");
  checkEqual(counters.removed, 0u, "and nothing was removed");
  checkEqual(counters.dirtyCount, 0u, "so no page was dirtied - insertions do not touch the sorted runs");

  std::vector<DeltaEdge> delta(harness.pass->deltaBytes() / sizeof(DeltaEdge));

  harness.graph->bufferRead(harness.deltaStaging, 0u, harness.pass->deltaBytes(),
                            [&delta](const void *data) { std::memcpy(delta.data(), data, delta.size() * sizeof(DeltaEdge)); });

  std::set<std::pair<uint32_t, uint32_t>> emitted;

  for (uint32_t i = 0u; i < counters.deltaCount; ++i)
  {
    emitted.emplace(delta[i].origin, delta[i].dest);
  }

  check(emitted.count({10u, 20u}) == 1u, "row 0's matched-vertex edge");
  check(emitted.count({30u, 40u}) == 1u, "row 1's matched-vertex edge");

  // Fresh vertices are per row: row 0 gets 1000, row 1 gets 1001.
  check(emitted.count({10u, 1000u}) == 1u, "row 0's fresh vertex is 1000");
  check(emitted.count({30u, 1001u}) == 1u, "row 1 gets a DIFFERENT fresh vertex");

  checkEqual(counters.vertexCount, 1002u, "and the allocator advanced past both");
}

void anIdleTickChangesNothing(::rendering::RHI *rhi)
{
  Harness harness;

  buildHarness(rhi, harness, {{1u, 2u}, {3u, 4u}, {5u, 6u}});

  harness.pass->clearMarks();

  RewriteParams params; // rowCount stays 0

  params.tableStride = 16u;
  params.deltaCapacity = 65536u;
  params.dirtyCapacity = 4096u;

  harness.pass->setParams(params);

  runGraph(harness, 0u);

  const RewriteCounters counters = readCounters(harness);

  checkEqual(counters.removed, 0u, "a tick with no rows removes nothing");
  checkEqual(counters.deltaCount, 0u, "inserts nothing");
  checkEqual(counters.dirtyCount, 0u, "and dirties nothing - the recorded stages are inert");

  const std::vector<Edge> page = readPage(harness, harness.pool->slotOf(0u, 0u));

  check(page[0].origin == 1u && page[1].origin == 3u && page[2].origin == 5u, "and the page is untouched");
}

void removalsAcrossSeveralPages(::rendering::RHI *rhi)
{
  Harness harness;

  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 900u; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  buildHarness(rhi, harness, edges);

  const uint32_t pages = harness.store.partitionAt(0u).pageCount();

  check(pages > 2u, "the data spans several pages");

  // Remove one edge from each of three different pages.
  const std::vector<uint32_t> targets = {5u, 400u, 880u};

  std::vector<uint32_t> table(Harness::tableWords, 0u);

  const uint32_t stride = 16u;

  for (uint32_t row = 0u; row < targets.size(); ++row)
  {
    table[0u * stride + row] = targets[row];
    table[1u * stride + row] = targets[row];
  }

  harness.graph->bufferWrite(harness.table, 0u, table.size() * sizeof(uint32_t), table.data());

  harness.pass->uploadRuleEdges({RuleEdgeDesc{0u, 1u, 0u, 0u}});
  harness.pass->clearMarks();

  RewriteParams params;

  params.rowCount = static_cast<uint32_t>(targets.size());
  params.tableStride = stride;
  params.removalCount = 1u;
  params.deltaCapacity = 65536u;
  params.dirtyCapacity = 4096u;

  harness.pass->setParams(params);

  runGraph(harness, 0u);

  const RewriteCounters counters = readCounters(harness);

  checkEqual(counters.removed, 3u, "three edges removed");
  check(counters.dirtyCount >= 2u, "spread across more than one page");
  check(counters.dirtyCount <= pages, "but never more pages than exist");

  std::cout << "    removals across pages: " << counters.dirtyCount << " of " << pages
            << " pages dirtied - the indirect compaction visits only those\n";
}

} // namespace

int main()
{
  ::rendering::DeviceRequiredLimits limits = {
      .minimumMemory = 0,
      .minimumComputeSharedMemory = 0,
      .minimumComputeWorkGroupInvocations = 0,
  };

  ::rendering::backend::vulkan::VulkanRHI *rhi = new ::rendering::backend::vulkan::VulkanRHI(
      ::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  rhi->init(surfaces);

  removalTombstonesAndCompacts(rhi);
  removingTheSameEdgeTwiceIsIdempotent(rhi);
  insertionsGoToTheDelta(rhi);
  anIdleTickChangesNothing(rhi);
  removalsAcrossSeveralPages(rhi);

  delete rhi;

  return report("GraphRewriteGpuRewriteTests");
}
