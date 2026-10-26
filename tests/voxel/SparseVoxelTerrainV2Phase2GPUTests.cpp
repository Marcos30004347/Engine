#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2Scan.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ScanChain.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

template <typename T> T readOne(::rendering::RenderGraph &graph, const ::rendering::Buffer buffer)
{
  T value{};
  graph.bufferRead(
      buffer,
      0u,
      sizeof(T),
      [&](const void *data)
      {
        std::memcpy(&value, data, sizeof(T));
      });
  return value;
}

std::vector<uint32_t> readWords(::rendering::RenderGraph &graph, const ::rendering::Buffer buffer, const uint32_t count)
{
  std::vector<uint32_t> values(count);
  if (count != 0u)
    graph.bufferRead(
        buffer,
        0u,
        uint64_t{count} * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(values.data(), data, values.size() * sizeof(uint32_t));
        });
  return values;
}

void testGPUScan(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  using namespace ::rendering;
  constexpr uint32_t capacity = svr::kTerrainGPUV2ScanMaximumElements;
  RenderGraph graph(&rhi);
  uint32_t passIndex = 0u;
  const Buffer outputReadback = graph.createBuffer({
    .name = "TerrainGPUV2.Scan.OutputReadback",
    .size = uint64_t{capacity} * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer compactReadback = graph.createBuffer({
    .name = "TerrainGPUV2.Scan.CompactReadback",
    .size = uint64_t{capacity} * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer statusReadback = graph.createBuffer({
    .name = "TerrainGPUV2.Scan.StatusReadback",
    .size = sizeof(svr::TerrainGPUV2ScanStatusGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer indirectReadback = graph.createBuffer({
    .name = "TerrainGPUV2.Scan.IndirectReadback",
    .size = sizeof(svr::IndirectDispatchArgsGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  {
    svrRendering::TerrainGPUV2ScanChain chain(graph, passIndex, capacity, capacity);
    auto *copyOutput =
        graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.Scan.CopyOutput", passIndex++, chain.output(), 0u, uint64_t{capacity} * sizeof(uint32_t), outputReadback, 0u, uint64_t{capacity} * sizeof(uint32_t));
    auto *copyCompact = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.Scan.CopyCompact", passIndex++, chain.compactedIndices(), 0u, uint64_t{capacity} * sizeof(uint32_t), compactReadback, 0u, uint64_t{capacity} * sizeof(uint32_t));
    auto *copyStatus =
        graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.Scan.CopyStatus", passIndex++, chain.status(), 0u, sizeof(svr::TerrainGPUV2ScanStatusGPU), statusReadback, 0u, sizeof(svr::TerrainGPUV2ScanStatusGPU));
    auto *copyIndirect = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.Scan.CopyIndirect", passIndex++, chain.indirectArgs(), 0u, sizeof(svr::IndirectDispatchArgsGPU), indirectReadback, 0u, sizeof(svr::IndirectDispatchArgsGPU));
    graph.compile();

    const auto run = [&]()
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    };

    const std::vector<uint32_t> sizes{0u, 1u, 255u, 256u, 257u, 65535u, 65536u, 65537u, capacity};
    for (const uint32_t size : sizes)
    {
      std::vector<uint32_t> input(size);
      for (uint32_t index = 0u; index < size; ++index)
        input[index] = index % 3u;
      std::vector<uint32_t> expected;
      svr::TerrainGPUV2ScanStatusGPU expectedStatus{};
      std::string error;
      const bool cpu = svr::terrainGPUV2ExclusiveScanCPU(input, expected, expectedStatus, &error);
      check(cpu && chain.update(input, false, capacity), "GPU scan accepts size " + std::to_string(size));
      run();
      const auto status = readOne<svr::TerrainGPUV2ScanStatusGPU>(graph, statusReadback);
      const auto actual = readWords(graph, outputReadback, size);
      if (actual != expected)
      {
        const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
        const auto index = static_cast<uint32_t>(std::distance(actual.begin(), mismatch.first));
        std::cout << "    first mismatch index=" << index << " actual=" << (index < actual.size() ? actual[index] : 0u) << " expected=" << (index < expected.size() ? expected[index] : 0u) << " total=" << status.total
                  << " overflow=" << status.overflow << '\n';
      }
      check(actual == expected && status.total == expectedStatus.total && status.overflow == 0u, "GPU exclusive scan matches CPU at size " + std::to_string(size));
    }

    std::mt19937 random(0x97123u);
    for (uint32_t iteration = 0u; iteration < 12u; ++iteration)
    {
      const uint32_t size = 257u + random() % 50000u;
      std::vector<uint32_t> flags(size);
      std::vector<uint32_t> expected;
      for (uint32_t index = 0u; index < size; ++index)
        if ((flags[index] = (random() % 5u) == 0u ? 1u : 0u) != 0u)
          expected.push_back(index);
      check(chain.update(flags, true, static_cast<uint32_t>(expected.size())), "random GPU compact job is accepted");
      run();
      const auto status = readOne<svr::TerrainGPUV2ScanStatusGPU>(graph, statusReadback);
      const auto indirect = readOne<svr::IndirectDispatchArgsGPU>(graph, indirectReadback);
      check(
          status.overflow == 0u && status.total == expected.size() && readWords(graph, compactReadback, status.total) == expected && indirect.x == (expected.size() + 63u) / 64u && indirect.y == 1u && indirect.z == 1u,
          "GPU compaction is stable and emits exact indirect arguments");
    }

    const std::vector<uint32_t> poison(capacity, 0xC001D00Du);
    graph.bufferWrite(chain.compactedIndices(), 0u, uint64_t{poison.size()} * sizeof(uint32_t), const_cast<uint32_t *>(poison.data()));
    check(chain.update({1u, 0u, 1u, 1u}, true, 2u), "overflow fixture is accepted");
    run();
    const auto overflowStatus = readOne<svr::TerrainGPUV2ScanStatusGPU>(graph, statusReadback);
    const auto overflowIndirect = readOne<svr::IndirectDispatchArgsGPU>(graph, indirectReadback);
    check(overflowStatus.overflow == 1u && overflowIndirect.x == 0u && readWords(graph, compactReadback, 4u) == std::vector<uint32_t>(4u, 0xC001D00Du), "GPU capacity overflow is latched before compact output writes");

    graph.bufferWrite(chain.compactedIndices(), 0u, uint64_t{poison.size()} * sizeof(uint32_t), const_cast<uint32_t *>(poison.data()));
    check(chain.update({1u, 2u, 0u}, true, 3u), "invalid flag fixture is accepted for fail-closed validation");
    run();
    const auto invalidStatus = readOne<svr::TerrainGPUV2ScanStatusGPU>(graph, statusReadback);
    check(invalidStatus.overflow == 1u && readWords(graph, compactReadback, 3u) == std::vector<uint32_t>(3u, 0xC001D00Du), "GPU non-binary flags fail before compact output writes");

    const std::vector<uint32_t> idleSentinel{0x1D1E0001u, 0x1D1E0002u, 0x1D1E0003u, 0x1D1E0004u};
    graph.bufferWrite(chain.compactedIndices(), 0u, idleSentinel.size() * sizeof(uint32_t), const_cast<uint32_t *>(idleSentinel.data()));
    chain.setEnabled(false);
    run();
    check(readWords(graph, compactReadback, static_cast<uint32_t>(idleSentinel.size())) == idleSentinel, "zero indirect arguments idle the permanently registered scan chain");
    chain.setEnabled(true);

    delete copyIndirect;
    delete copyStatus;
    delete copyCompact;
    delete copyOutput;
  }
  graph.deleteBuffer(indirectReadback);
  graph.deleteBuffer(statusReadback);
  graph.deleteBuffer(compactReadback);
  graph.deleteBuffer(outputReadback);
}

} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 1024u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUScan(rhi);
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 2 GPU tests passed\n" : "GPU terrain V2 Phase 2 GPU tests failed\n");
  return failures == 0 ? 0 : 1;
}
