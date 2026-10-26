// Phase 3.7: the streaming manager.
//
// The headline assertion is TIER-INDEPENDENCE: the same access sequence must be
// served whatever the pool size, down to a single slot. That is the property
// that catches nearly every streaming bug, because it is what the multi-frame
// resume in the match kernels depends on.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/StreamingManager.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <memory>
#include <random>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Harness
{
  compiler::CompiledProgram program;
  compiler::UsageValues usage;
  EdgePageStore store;
  DiagnosticSink sink;
  std::vector<EdgeTypeId> partitionTypes;
};

void buildHarness(Harness &out)
{
  const char *source = "let A likes B.\n"
                       "let A knows B.\n"
                       "let A owns B.\n"
                       "rule r: when A likes B, B knows A, A owns B then A likes B.\n";

  compiler::compile(source, "t.graphwrite", compiler::CompileOptions(), out.sink, out.program, out.usage);

  out.store.configure(out.program.edgeTypes);

  std::mt19937 rng(77u);

  for (uint32_t i = 0u; i < 1500u; ++i)
  {
    out.store.addEdge(static_cast<EdgeTypeId>(rng() % 3u), Edge{rng() % 80u, rng() % 80u});
  }

  for (uint32_t partition = 0u; partition < out.store.partitionCount(); ++partition)
  {
    out.partitionTypes.push_back(out.store.partitionAt(partition).edgeType());
  }
}

// Serves a fixed request sequence with a given pool size, returning the order in
// which pages were admitted.
std::vector<uint32_t> serveSequence(rendering::RHI *rhi, Harness &harness, const uint32_t slotCount,
                                    const std::vector<uint32_t> &sequence, uint32_t &starvedCalls)
{
  auto graphStorage = std::make_unique<rendering::RenderGraph>(rhi);

  GpuPagePoolSettings settings;

  settings.slotCount = slotCount;

  GpuPagePool pool(*graphStorage, settings);

  pool.configure(harness.store);

  StreamingManager streaming(pool, harness.store, harness.usage);

  streaming.configure(harness.partitionTypes);

  std::vector<uint32_t> served;

  starvedCalls = 0u;

  for (const uint32_t page : sequence)
  {
    streaming.requestByMetaIndex(page);

    // Drain: keep servicing until this page is resident, exactly as a stalled
    // row would across successive frames.
    uint32_t guard = 0u;

    while (true)
    {
      const PageMeta &entry = pool.metadata()[page];
      const uint32_t partition = entry.partition;
      const uint32_t position = page - pool.partitionFirst()[partition];

      if (pool.isResident(partition, position))
      {
        break;
      }

      streaming.service(0u);

      if (streaming.lastCallStarved())
      {
        ++starvedCalls;

        break;
      }

      if (++guard > 64u)
      {
        break;
      }
    }

    served.push_back(page);
  }

  return served;
}

void tierIndependence(rendering::RHI *rhi)
{
  Harness harness;

  buildHarness(harness);

  std::mt19937 rng(5150u);

  std::vector<uint32_t> sequence;

  for (uint32_t i = 0u; i < 120u; ++i)
  {
    sequence.push_back(rng() % std::max(1u, harness.store.partitionAt(0u).pageCount()));
  }

  std::vector<std::vector<uint32_t>> results;

  uint32_t totalStarved = 0u;

  for (const uint32_t slots : {1u, 2u, 3u, 8u, 4096u})
  {
    uint32_t starved = 0u;

    results.push_back(serveSequence(rhi, harness, slots, sequence, starved));

    totalStarved += starved;

    checkEqual(results.back().size(), sequence.size(), "every request was eventually served at " + std::to_string(slots) + " slots");
  }

  bool identical = true;

  for (size_t i = 1u; i < results.size(); ++i)
  {
    identical = identical && results[i] == results[0];
  }

  check(identical, "the served sequence is identical at every pool size");
  checkEqual(totalStarved, 0u, "and no call ever starved, even with a single slot");
}

void beladyPrefersTheSoonerType(rendering::RHI *rhi)
{
  Harness harness;

  buildHarness(harness);

  auto graphStorage = std::make_unique<rendering::RenderGraph>(rhi);

  GpuPagePoolSettings settings;

  settings.slotCount = 2u;

  GpuPagePool pool(*graphStorage, settings);

  pool.configure(harness.store);

  StreamingManager streaming(pool, harness.store, harness.usage);

  streaming.configure(harness.partitionTypes);

  // Fill the pool with pages from two partitions, then ask for a third.
  const uint32_t first = pool.metaIndex(0u, 0u);
  const uint32_t second = pool.metaIndex(1u, 0u);

  streaming.requestByMetaIndex(first);
  streaming.requestByMetaIndex(second);

  checkEqual(streaming.service(0u), 2u, "both pages are admitted into an empty pool");
  checkEqual(pool.freeSlotCount(), 0u, "the pool is full");

  const uint32_t third = pool.metaIndex(0u, 1u);

  streaming.requestByMetaIndex(third);

  checkEqual(streaming.service(0u), 1u, "the third page is admitted by evicting one");
  check(!streaming.lastCallStarved(), "without starving");
  checkEqual(pool.freeSlotCount(), 0u, "and the pool is still full");
}

void consumedPagesAreNotPinned(rendering::RHI *rhi)
{
  // The regression: an earlier policy pinned every page the frame had advanced
  // over, which starved every other row and deadlocked outright at small pool
  // sizes.
  Harness harness;

  buildHarness(harness);

  auto graphStorage = std::make_unique<rendering::RenderGraph>(rhi);

  GpuPagePoolSettings settings;

  settings.slotCount = 1u;

  GpuPagePool pool(*graphStorage, settings);

  pool.configure(harness.store);

  StreamingManager streaming(pool, harness.store, harness.usage);

  streaming.configure(harness.partitionTypes);

  const uint32_t pages = std::min(20u, pool.totalPages());

  uint32_t admitted = 0u;

  for (uint32_t page = 0u; page < pages; ++page)
  {
    streaming.requestByMetaIndex(page);

    admitted += streaming.service(0u);

    check(!streaming.lastCallStarved(), "a one-slot pool still makes progress at page " + std::to_string(page));
  }

  checkEqual(admitted, pages, "every page was admitted in turn through a single slot");
}

void requestsForResidentPagesAreFree(rendering::RHI *rhi)
{
  Harness harness;

  buildHarness(harness);

  auto graphStorage = std::make_unique<rendering::RenderGraph>(rhi);

  GpuPagePoolSettings settings;

  settings.slotCount = 8u;

  GpuPagePool pool(*graphStorage, settings);

  pool.configure(harness.store);

  StreamingManager streaming(pool, harness.store, harness.usage);

  streaming.configure(harness.partitionTypes);

  streaming.requestByMetaIndex(0u);
  streaming.service(0u);

  streaming.resetStats();

  streaming.requestByMetaIndex(0u);

  checkEqual(streaming.service(0u), 0u, "a request for an already-resident page admits nothing");
  checkEqual(streaming.stats().alreadyResident, uint64_t(1), "and is counted as already resident");
  checkEqual(streaming.pendingRequests(), 0u, "and does not stay pending");
}

} // namespace

int main()
{
  rendering::DeviceRequiredLimits limits = {
      .minimumMemory = 0,
      .minimumComputeSharedMemory = 0,
      .minimumComputeWorkGroupInvocations = 0,
  };

  rendering::backend::vulkan::VulkanRHI *rhi = new rendering::backend::vulkan::VulkanRHI(
      rendering::backend::vulkan::Vulkan_1_2, limits, rendering::DeviceFeatures::DeviceFeatures_Compute, {});

  auto surfaces = std::vector<VkSurfaceKHR>();

  rhi->init(surfaces);

  tierIndependence(rhi);
  beladyPrefersTheSoonerType(rhi);
  consumedPagesAreNotPinned(rhi);
  requestsForResidentPagesAreFree(rhi);

  delete rhi;

  return report("GraphRewriteStreamingTests");
}
