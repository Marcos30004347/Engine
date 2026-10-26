// Phase 3.10: the resumable match kernels, on a real device.
//
// The headline assertion is POOL-INDEPENDENCE: the same instruction, run with a
// pool that can hold every page and with one that can hold a single page, must
// produce the same rows. That is the property the multi-frame resume exists to
// provide, and it is what catches almost every streaming bug.

#include "TestHarness.hpp"
#include "graphrewrite/rendering/GraphMatchPass.hpp"
#include "graphrewrite/runtime/StreamingManager.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <set>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Fixture
{
  compiler::EdgeTypeTable types;
  EdgePageStore store;
  compiler::UsageValues usage;
  std::vector<EdgeTypeId> partitionTypes;
  std::vector<Edge> edges;
};

void buildFixture(Fixture &out, const uint32_t edgeCount, const uint32_t vertices)
{
  const compiler::EdgeTypeId type = out.types.intern("? a ?", 2u);

  out.types.mutableType(type).usedForward = true;

  out.store.configure(out.types);

  std::mt19937 rng(31337u);

  std::set<std::pair<uint32_t, uint32_t>> unique;

  while (unique.size() < edgeCount)
  {
    const uint32_t origin = rng() % vertices;
    const uint32_t target = rng() % vertices;

    if (origin != target)
    {
      unique.emplace(origin, target);
    }
  }

  for (const auto &entry : unique)
  {
    const Edge edge{entry.first, entry.second};

    out.store.addEdge(0u, edge);
    out.edges.push_back(edge);
  }

  out.partitionTypes.push_back(0u);
}

// Runs one FORWARD instruction over the whole edge set, seeded with a read table
// of one row per vertex, resuming until every row is complete.
struct RunResult
{
  std::vector<uint32_t> rows; // flattened (from, to) pairs
  uint32_t frames = 0u;
  uint32_t stalls = 0u;
  bool overflowed = false;
};

RunResult runInstruction(::rendering::RHI *rhi, Fixture &fixture, const uint32_t slotCount, const uint32_t vertices)
{
  auto graphStorage = std::make_unique<::rendering::RenderGraph>(rhi);
  ::rendering::RenderGraph &graph = *graphStorage;

  GpuPagePoolSettings poolSettings;

  poolSettings.slotCount = slotCount;

  GpuPagePool pool(graph, poolSettings);

  pool.configure(fixture.store);

  StreamingManager streaming(pool, fixture.store, fixture.usage);

  streaming.configure(fixture.partitionTypes);

  graphrewrite::rendering::GraphMatchPass::Settings passSettings;

  passSettings.pool = &pool;
  passSettings.maxRows = 4096u;
  passSettings.tableWords = 1u << 16u;

  auto *matchPass = graph.registerPass<graphrewrite::rendering::GraphMatchPass>("graphrewrite.match", 0u, passSettings);

  const ::rendering::Buffer writeStaging = graph.createBuffer(::rendering::BufferInfo{
      .name = "test.writeStaging",
      .size = matchPass->tableBytes(),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  const ::rendering::Buffer statusStaging = graph.createBuffer(::rendering::BufferInfo{
      .name = "test.statusStaging",
      .size = sizeof(graphrewrite::rendering::MatchStatus),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  const ::rendering::Buffer requestStagingLocal = graph.createBuffer(::rendering::BufferInfo{
      .name = "test.requestStaging",
      .size = pool.requestBytes(),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyWrite", 1u, matchPass->writeTable(), 0u, matchPass->tableBytes(),
                                                         writeStaging, 0u, matchPass->tableBytes());
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyStatus", 2u, matchPass->statusBuffer(), 0u,
                                                         sizeof(graphrewrite::rendering::MatchStatus), statusStaging, 0u,
                                                         sizeof(graphrewrite::rendering::MatchStatus));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyRequests", 3u, pool.requestBuffer(), 0u, pool.requestBytes(),
                                                         requestStagingLocal, 0u, pool.requestBytes());

  graph.compile();

  // The read table: one row per vertex, column 0 = the vertex.
  std::vector<uint32_t> readTable(vertices, 0u);

  for (uint32_t vertex = 0u; vertex < vertices; ++vertex)
  {
    readTable[vertex] = vertex;
  }

  graph.bufferWrite(matchPass->readTable(), 0u, readTable.size() * sizeof(uint32_t), readTable.data());

  std::vector<uint32_t> rowCounts(256u, 0u);

  rowCounts[0] = vertices;

  graph.bufferWrite(matchPass->tableRowsBuffer(), 0u, rowCounts.size() * sizeof(uint32_t), rowCounts.data());

  std::vector<uint32_t> zeroState(matchPass->rowStateBytes() / sizeof(uint32_t), 0u);

  graph.bufferWrite(matchPass->rowState(), 0u, zeroState.size() * sizeof(uint32_t), zeroState.data());

  std::vector<uint32_t> zeroRequests(pool.requestBytes() / sizeof(uint32_t), 0u);

  graph.bufferWrite(pool.requestBuffer(), 0u, pool.requestBytes(), zeroRequests.data());

  RunResult result;

  // Read and write tables share one buffer, so they must not overlap.
  constexpr uint32_t kWriteWordBase = 16384u;

  graphrewrite::rendering::MatchParams params;

  params.partition = 0u;
  params.direction = 1u; // forward
  params.fromColumn = 0u;
  params.writeColumn = 1u;
  // Row counts now live on the GPU, so the seed count is written there rather
  // than handed over in params.
  params.readRegister = 0u;
  params.writeRegister = 1u;
  params.readCols = 1u;
  params.readStride = vertices;  // this test writes its read table packed

  // Without this the scan's block sums land on row 0's state and quietly
  // corrupt the cursor.
  params.blockSumsBase = matchPass->blockSumsBase();
  params.writeCols = 2u;
  params.writeCapacity = 8192u;
  params.readBase = 0u;
  params.writeWordBase = kWriteWordBase;
  params.firstFrame = 1u;
  params.boundMask = 0x1u; // only column 0 holds a vertex

  uint32_t writeBase = 0u;

  for (uint32_t frame = 0u; frame < 512u; ++frame)
  {
    matchPass->setParams(params);

    ::rendering::RenderGraph::Frame gpuFrame;
    ::rendering::RenderGraph::Overrides overrides;

    graph.setCurrentFrameIndex(frame % graph.getMaxFramesInFlight());
    graph.run(gpuFrame, overrides);
    graph.waitFrame(gpuFrame);

    ++result.frames;

    graphrewrite::rendering::MatchStatus status;

    graph.bufferRead(statusStaging, 0u, sizeof(status), [&status](const void *data) { std::memcpy(&status, data, sizeof(status)); });

    writeBase += status.writtenRows;

    result.stalls += status.stalledRows;
    result.overflowed = result.overflowed || status.overflowed != 0u;

    params.firstFrame = 0u;

    if (status.incompleteRows == 0u)
    {
      break;
    }

    // Service what the GPU asked for.
    std::vector<uint32_t> bits(pool.requestBytes() / sizeof(uint32_t), 0u);

    graph.bufferRead(requestStagingLocal, 0u, pool.requestBytes(),
                     [&bits](const void *data) { std::memcpy(bits.data(), data, bits.size() * sizeof(uint32_t)); });

    bool anyRequest = false;

    for (size_t word = 0u; word < bits.size(); ++word)
    {
      uint32_t value = bits[word];

      while (value != 0u)
      {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(value));

        value &= value - 1u;

        streaming.requestByMetaIndex(static_cast<uint32_t>(word) * 32u + bit);

        anyRequest = true;
      }
    }

    if (!anyRequest)
    {
      break;
    }

    streaming.service(0u);

    graph.bufferWrite(pool.requestBuffer(), 0u, pool.requestBytes(), zeroRequests.data());
  }

  std::vector<uint32_t> written(matchPass->tableBytes() / sizeof(uint32_t), 0u);

  graph.bufferRead(writeStaging, 0u, matchPass->tableBytes(),
                   [&written](const void *data) { std::memcpy(written.data(), data, written.size() * sizeof(uint32_t)); });

  for (uint32_t row = 0u; row < writeBase; ++row)
  {
    result.rows.push_back(written[kWriteWordBase + row]);                        // column 0
    result.rows.push_back(written[kWriteWordBase + params.writeCapacity + row]);  // column 1
  }

  return result;
}

std::set<std::pair<uint32_t, uint32_t>> asSet(const RunResult &result)
{
  std::set<std::pair<uint32_t, uint32_t>> out;

  for (size_t i = 0u; i + 1u < result.rows.size(); i += 2u)
  {
    out.emplace(result.rows[i], result.rows[i + 1u]);
  }

  return out;
}

void runAll(::rendering::RHI *rhi)
{
  Fixture fixture;

  const uint32_t vertices = 40u;

  buildFixture(fixture, 700u, vertices);

  check(fixture.store.partitionAt(0u).pageCount() > 2u, "the data spans several pages");

  // Every edge whose origin is one of the seeded rows is a match.
  std::set<std::pair<uint32_t, uint32_t>> expected;

  for (const Edge &edge : fixture.edges)
  {
    expected.emplace(edge.origin, edge.target);
  }

  std::vector<std::set<std::pair<uint32_t, uint32_t>>> results;

  for (const uint32_t slots : {1u, 2u, 4096u})
  {
    const RunResult run = runInstruction(rhi, fixture, slots, vertices);

    check(!run.overflowed, "no overflow at " + std::to_string(slots) + " slots");

    const std::set<std::pair<uint32_t, uint32_t>> got = asSet(run);

    checkEqual(got.size(), expected.size(), "match count at " + std::to_string(slots) + " slots");
    check(got == expected, "the matches themselves at " + std::to_string(slots) + " slots");

    results.push_back(got);

    std::cout << "    " << slots << " slots: " << run.frames << " frame(s), " << run.stalls << " stall event(s), " << got.size()
              << " rows\n";
  }

  bool identical = true;

  for (size_t i = 1u; i < results.size(); ++i)
  {
    identical = identical && results[i] == results[0];
  }

  check(identical, "POOL-INDEPENDENCE: the result is identical at 1, 2 and 4096 slots");
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

  runAll(rhi);

  delete rhi;

  return report("GraphRewriteMatchTests");
}
