// Phase 3.9: publish and readback.

#include "TestHarness.hpp"
#include "graphrewrite/rendering/GraphPublishPass.hpp"
#include "graphrewrite/runtime/MatchTable.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <cstring>
#include <memory>
#include <vector>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

void runAll(::rendering::RHI *rhi)
{
  auto graphStorage = std::make_unique<::rendering::RenderGraph>(rhi);
  ::rendering::RenderGraph &graph = *graphStorage;

  graphrewrite::rendering::GraphPublishPass::Settings settings;

  settings.patternCount = 4u;
  settings.resultCapacityWords = 4096u;
  settings.sourceCapacityWords = 4096u;

  auto *publishPass = graph.registerPass<graphrewrite::rendering::GraphPublishPass>("graphrewrite.publish", 0u, settings);

  // The buffers must exist before the copy passes below can name them, and
  // before the table is written into them.
  publishPass->createResources();

  // A packed 3-column, 5-row table, column-major.
  MatchTable table;

  table.reset(3u, 5u);

  for (uint32_t row = 0u; row < 5u; ++row)
  {
    table.appendRow();
  }

  for (uint32_t column = 0u; column < 3u; ++column)
  {
    for (uint32_t row = 0u; row < 5u; ++row)
    {
      table.set(column, row, 1000u * column + row);
    }
  }

  MatchTable packed;

  table.repackInto(packed);

  const uint32_t generation = 7u;
  const uint32_t resultOffset = 64u;

  PublishParams params;

  params.patternId = 2u;
  params.rows = packed.rows();
  params.cols = packed.columns();
  params.generation = generation;
  params.resultOffset = resultOffset;

  publishPass->clearQueue();
  publishPass->publish(params);

  // Readback mirrors: Pull buffers may not carry a Storage binding.
  const ::rendering::Buffer resultStaging = graph.createBuffer(::rendering::BufferInfo{
      .name = "test.resultStaging",
      .size = settings.resultCapacityWords * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  const ::rendering::Buffer statusStaging = graph.createBuffer(::rendering::BufferInfo{
      .name = "test.statusStaging",
      .size = settings.patternCount * sizeof(PatternStatus),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull | ::rendering::BufferUsage::BufferUsage_CopyDst,
  });

  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyResult", 1u, publishPass->resultBuffer(), 0u,
                                                         settings.resultCapacityWords * sizeof(uint32_t), resultStaging, 0u,
                                                         settings.resultCapacityWords * sizeof(uint32_t));

  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("test.copyStatus", 2u, publishPass->statusBuffer(), 0u,
                                                         settings.patternCount * sizeof(PatternStatus), statusStaging, 0u,
                                                         settings.patternCount * sizeof(PatternStatus));

  // Zero the status buffer first, so "Complete" cannot be left over from
  // uninitialised memory.
  std::vector<PatternStatus> zeroed(settings.patternCount);

  graph.compile();

  graph.bufferWrite(publishPass->statusBuffer(), 0u, zeroed.size() * sizeof(PatternStatus), zeroed.data());
  graph.bufferWrite(publishPass->sourceBuffer(), 0u, packed.raw().size() * sizeof(uint32_t),
                    const_cast<uint32_t *>(packed.raw().data()));

  ::rendering::RenderGraph::Frame frame;
  ::rendering::RenderGraph::Overrides overrides;

  graph.setCurrentFrameIndex(0u);
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  std::vector<PatternStatus> statuses(settings.patternCount);

  graph.bufferRead(statusStaging, 0u, statuses.size() * sizeof(PatternStatus),
                   [&statuses](const void *data) { std::memcpy(statuses.data(), data, statuses.size() * sizeof(PatternStatus)); });

  std::vector<uint32_t> result(settings.resultCapacityWords, 0u);

  graph.bufferRead(resultStaging, 0u, result.size() * sizeof(uint32_t),
                   [&result](const void *data) { std::memcpy(result.data(), data, result.size() * sizeof(uint32_t)); });

  // --- the status record --------------------------------------------------
  const PatternStatus &status = statuses[2];

  check(status.isComplete(), "the published pattern is marked complete");
  checkEqual(status.rows, 5u, "with its row count");
  checkEqual(status.cols, 3u, "and column count");
  checkEqual(status.resultOffset, resultOffset, "and where its rows landed");
  check(status.isFresh(generation), "and the generation of the step that produced it");

  // Patterns nobody published must stay Running, or the CPU would enqueue a
  // rewrite for a pattern that never matched.
  for (const uint32_t other : {0u, 1u, 3u})
  {
    check(!statuses[other].isComplete(), "pattern " + std::to_string(other) + " was not published and stays running");
  }

  // --- a stale generation is detectable -----------------------------------
  check(!status.isFresh(generation + 1u), "a status from an earlier step is rejected by generation");
  check(!status.isFresh(generation - 1u), "and so is one from a later step");

  // --- the table itself ---------------------------------------------------
  bool identical = true;

  for (uint32_t i = 0u; i < packed.raw().size(); ++i)
  {
    identical = identical && result[resultOffset + i] == packed.raw()[i];
  }

  check(identical, "every cell of the table arrived in the result buffer");

  // And nothing was written outside the pattern's own region.
  bool cleanBefore = true;

  for (uint32_t i = 0u; i < resultOffset; ++i)
  {
    cleanBefore = cleanBefore && result[i] == 0u;
  }

  check(cleanBefore, "nothing was written before the pattern's result offset");

  // Column-major really did survive: column 1 starts `rows` words in.
  checkEqual(result[resultOffset + 0u], 0u, "column 0 row 0");
  checkEqual(result[resultOffset + 5u], 1000u, "column 1 begins at offset rows");
  checkEqual(result[resultOffset + 10u], 2000u, "column 2 begins at offset 2*rows");
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

  return report("GraphRewritePublishTests");
}
