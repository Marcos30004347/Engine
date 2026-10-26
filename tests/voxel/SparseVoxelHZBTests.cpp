#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/DepthPyramidPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/rendering/SparseVoxelHZBReadbackPass.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
namespace svrRendering = voxel::svraster::rendering;

uint32_t mipCount(uint32_t width, uint32_t height)
{
  uint32_t levels = 1u;
  for (uint32_t maximum = std::max(width, height); maximum > 1u; maximum >>= 1u)
    ++levels;
  return levels;
}

std::vector<std::vector<float>> cpuReverseZPyramid(
    const uint32_t width,
    const uint32_t height,
    const std::vector<float> &source)
{
  auto mipDimension = [](const uint32_t base, const uint32_t mip)
  {
    return std::max(1u, base >> mip);
  };
  auto reduceDirect = [](
                          const uint32_t sourceWidth,
                          const uint32_t sourceHeight,
                          const std::vector<float> &sourceMip,
                          const uint32_t reductionLevel)
  {
    const uint32_t destinationWidth = std::max(1u, sourceWidth >> reductionLevel);
    const uint32_t destinationHeight = std::max(1u, sourceHeight >> reductionLevel);
    const uint32_t footprint = 1u << reductionLevel;
    std::vector<float> destination(uint64_t{destinationWidth} * destinationHeight);
    for (uint32_t y = 0u; y < destinationHeight; ++y)
      for (uint32_t x = 0u; x < destinationWidth; ++x)
      {
        float reduced = 1.0f;
        for (uint32_t offsetY = 0u; offsetY < footprint; ++offsetY)
          for (uint32_t offsetX = 0u; offsetX < footprint; ++offsetX)
          {
            const uint32_t sourceX = std::min(x * footprint + offsetX, sourceWidth - 1u);
            const uint32_t sourceY = std::min(y * footprint + offsetY, sourceHeight - 1u);
            reduced = std::min(reduced, sourceMip[sourceY * sourceWidth + sourceX]);
          }
        destination[y * destinationWidth + x] = reduced;
      }
    return destination;
  };

  std::vector<std::vector<float>> result(mipCount(width, height));
  result[0] = source;
  if (result.size() == 1u)
    return result;

  constexpr uint32_t kSpdLocalReductionLevels = 6u;
  uint32_t producedMipLevels = 1u;
  const uint32_t sourceDepthLevels =
      std::min<uint32_t>(kSpdLocalReductionLevels, static_cast<uint32_t>(result.size()) - 1u);
  for (uint32_t localLevel = 1u; localLevel <= sourceDepthLevels; ++localLevel)
    result[localLevel] = reduceDirect(width, height, source, localLevel);
  producedMipLevels += sourceDepthLevels;

  while (producedMipLevels < result.size())
  {
    const uint32_t sourceMip = producedMipLevels - 1u;
    const uint32_t sourceWidth = mipDimension(width, sourceMip);
    const uint32_t sourceHeight = mipDimension(height, sourceMip);
    const uint32_t levelsToWrite =
        std::min<uint32_t>(kSpdLocalReductionLevels, static_cast<uint32_t>(result.size()) - producedMipLevels);
    for (uint32_t localLevel = 1u; localLevel <= levelsToWrite; ++localLevel)
      result[producedMipLevels + localLevel - 1u] =
          reduceDirect(sourceWidth, sourceHeight, result[sourceMip], localLevel);
    producedMipLevels += levelsToWrite;
  }
  return result;
}

std::vector<float> makeDepth(const uint32_t width, const uint32_t height, const uint32_t seed)
{
  std::vector<float> result(width * height);
  for (uint32_t index = 0u; index < result.size(); ++index)
  {
    const uint32_t mixed = (index * 1664525u + seed * 1013904223u) & 0x00ffffffu;
    result[index] = 0.05f + 0.9f * (static_cast<float>(mixed) / static_cast<float>(0x01000000u));
  }
  return result;
}

void testCase(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const uint32_t width,
    const uint32_t height,
    const uint32_t requestedFirstValidMip = 0xffffffffu)
{
  RenderGraph graph(&rhi);
  const uint32_t levels = mipCount(width, height);
  const uint32_t firstValidMip = requestedFirstValidMip == 0xffffffffu
      ? (levels > 1u ? 1u : 0u)
      : std::min(requestedFirstValidMip, levels - 1u);
  const Texture sourceDepth = graph.createTexture({
    .name = "g20.source." + std::to_string(width) + "x" +
        std::to_string(height) + ".first" + std::to_string(firstValidMip),
    .format = Format::Format_Depth32Float,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
  });
  const Texture hzb = graph.createTexture({
    .name = "g20.hzb." + std::to_string(width) + "x" +
        std::to_string(height) + ".first" + std::to_string(firstValidMip),
    .format = Format::Format_R32Float,
    .height = height,
    .width = width,
    .memoryProperties = BufferUsage::BufferUsage_Storage,
    .mipLevels = levels,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  uint32_t outputCount = 0u;
  for (uint32_t mip = firstValidMip; mip < levels; ++mip)
    outputCount += std::max(1u, width >> mip) * std::max(1u, height >> mip);
  const Buffer gpuOutput = graph.createBuffer({
    .name = "g20.output",
    .size = uint64_t{outputCount} * sizeof(float),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = "g20.readback",
    .size = uint64_t{outputCount} * sizeof(float),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });

  std::vector<Pass *> passes;
  uint32_t passIndex = 0u;
  passes.push_back(graph.registerPass<::rendering::gpgpu::DepthPyramidPass>(
      "g20.BuildHZB", passIndex++, sourceDepth, hzb,
      ::rendering::gpgpu::DepthPyramidPass::Settings{width, height, width, height, levels}));
  uint32_t outputOffset = 0u;
  for (uint32_t mip = firstValidMip; mip < levels; ++mip)
  {
    const uint32_t mipWidth = std::max(1u, width >> mip);
    const uint32_t mipHeight = std::max(1u, height >> mip);
    passes.push_back(graph.registerPass<svrRendering::SparseVoxelHZBReadbackPass>(
        "g20.ReadMip" + std::to_string(mip), passIndex++, hzb, mip, gpuOutput,
        uint64_t{outputCount} * sizeof(float),
        svrRendering::SparseVoxelHZBReadbackPass::Uniforms{mipWidth, mipHeight, outputOffset, 0u}));
    outputOffset += mipWidth * mipHeight;
  }
  passes.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g20.Copy", passIndex++, gpuOutput, 0u, uint64_t{outputCount} * sizeof(float),
      readback, 0u, uint64_t{outputCount} * sizeof(float)));
  graph.compile();

  for (uint32_t iteration = 0u; iteration < 2u; ++iteration)
  {
    const std::vector<float> source = makeDepth(width, height, iteration + 1u);
    graph.textureWrite(sourceDepth, source.size() * sizeof(float), const_cast<float *>(source.data()));
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<float> actual(outputCount);
    graph.bufferRead(readback, 0u, actual.size() * sizeof(float), [&](const void *data) {
      std::memcpy(actual.data(), data, actual.size() * sizeof(float));
    });
    const auto expectedMips = cpuReverseZPyramid(width, height, source);
    std::vector<float> expected;
    for (uint32_t mip = firstValidMip; mip < levels; ++mip)
      expected.insert(expected.end(), expectedMips[mip].begin(), expectedMips[mip].end());
    if (actual != expected)
    {
      size_t mismatch = 0u;
      while (mismatch < actual.size() && mismatch < expected.size() && actual[mismatch] == expected[mismatch])
        ++mismatch;
      std::cerr << "HZB mismatch: size=" << width << "x" << height
                << " firstValidMip=" << firstValidMip
                << " iteration=" << iteration
                << " actualCount=" << actual.size()
                << " expectedCount=" << expected.size()
                << " mismatch=" << mismatch;
      if (mismatch < actual.size() && mismatch < expected.size())
        std::cerr << " actual=" << actual[mismatch] << " expected=" << expected[mismatch];
      std::cerr << '\n';
    }
    assert(actual == expected);
  }

  for (auto it = passes.rbegin(); it != passes.rend(); ++it)
    delete *it;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(gpuOutput);
  graph.deleteTexture(hzb);
  graph.deleteTexture(sourceDepth);
}

} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 256u};
  ::rendering::backend::vulkan::VulkanRHI rhi(
      ::rendering::backend::vulkan::Vulkan_1_2, limits,
      ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  const std::array<std::array<uint32_t, 2>, 7> cases{{
    {1u, 1u}, {8u, 8u}, {7u, 5u}, {1u, 17u}, {19u, 1u}, {130u, 70u}, {513u, 257u},
  }};
  for (const auto dimensions : cases)
    testCase(rhi, dimensions[0], dimensions[1]);
  testCase(rhi, 33u, 19u, 2u);
  testCase(rhi, 513u, 257u, 3u);
  std::cout << "Sparse voxel G20 shared SPD HZB tests passed\n";
  os::Logger::shutdown();
  return 0;
}
