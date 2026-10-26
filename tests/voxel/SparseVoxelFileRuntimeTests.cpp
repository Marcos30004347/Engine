#include "voxel/svraster/SparseVoxelFileRuntime.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"

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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
using namespace voxel::svraster;

std::array<float, 16> identity()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

SparseVoxelScene fixtureScene()
{
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.0f, 0.0f, 0.0f};
  model.rootExtent = 4.0f;
  // A shallow authored cut may still declare the schema's maximum refinement
  // depth. G18 must size runtime queues from this value, not resident levels.
  model.maxLevel = kMaxOctreeLevels;
  model.firstRoot = 0u;
  model.rootCount = 8u;
  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff806040u;
  material.roughnessMetallicUNorm8 = 0x6040u;
  for (uint32_t level = 1u; level <= 2u; ++level)
  {
    const uint32_t resolution = 1u << level;
    std::vector<uint64_t> paths;
    for (uint32_t z = 0u; z < resolution; ++z)
      for (uint32_t y = 0u; y < resolution; ++y)
        for (uint32_t x = 0u; x < resolution; ++x)
          paths.push_back(encodeOctreePath(x, y, z, level));
    std::sort(paths.begin(), paths.end());
    for (const uint64_t path : paths)
    {
      const uint32_t localIndex = static_cast<uint32_t>(model.hierarchyNodes.size());
      const bool parent = level == 1u;
      model.hierarchyNodes.push_back({parent ? 8u + localIndex * 8u : 0u, parent ? 8u : 0u, localIndex, level});
      model.octreePaths.push_back(path);
      model.octreeLevels.push_back(static_cast<uint8_t>(level));
      const auto coordinate = decodeOctreePath(path, level);
      const float length = model.rootExtent / static_cast<float>(1u << level);
      model.centersLengths.push_back({{
                                        -model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * length,
                                        -model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * length,
                                        -model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * length,
                                      },
                                      length});
      material.materialId = localIndex;
      model.materials.push_back(material);
    }
  }
  SparseVoxelBLAS secondModel = model;
  scene.models.push_back(std::move(model));
  scene.models.push_back(std::move(secondModel));
  scene.instances.push_back({0u, 0u, identity()});
  auto translated = identity();
  translated[12] = 6.0f;
  scene.instances.push_back({1u, 0u, translated});
  scene.sourceContentHash = 0x12345678u;
  scene.buildSettingsHash = 0xabcdef01u;
  return scene;
}

std::vector<uint8_t> readBytes(const std::filesystem::path &path)
{
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  assert(stream);
  const auto size = stream.tellg();
  assert(size >= 0);
  std::vector<uint8_t> result(static_cast<size_t>(size));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(result.data()), size);
  assert(stream);
  return result;
}

template <typename Predicate> bool pumpUntil(SparseVoxelFileRuntime &runtime, const Predicate &predicate)
{
  for (uint32_t attempt = 0u; attempt < 2000u; ++attempt)
  {
    SparseVoxelFileRuntimeUpdate update;
    std::string error;
    assert(runtime.update(update, &error));
    assert(error.empty());
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

void testMetadataFirstSelectiveOpenAndNoWrites()
{
  const auto path = std::filesystem::temp_directory_path() / "engine-svraster-g18-runtime.vsvo";
  const auto invalid = std::filesystem::temp_directory_path() / "engine-svraster-g18-invalid.vsvo";
  std::string error;
  const SparseVoxelScene source = fixtureScene();
  assert(writeVirtualSparseVoxelScene(path.string(), source, {VirtualSparseVoxelFileCompression::LZ4, 704u}, &error));
  const auto originalBytes = readBytes(path);

  SparseVoxelFileRuntime runtime;
  assert(runtime.openMetadata(path.string(), {}, &error));
  assert(runtime.metadata().models[0].maxLevel == kMaxOctreeLevels);
  assert(runtime.isOpen());
  assert(!runtime.rootsResident());
  assert(runtime.residency().residentNodeCount() == 0u);
  assert(readBytes(path) == originalBytes);

  assert(pumpUntil(runtime, [&] { return runtime.rootsResident(); }));
  uint32_t rootNodeCount = 0u;
  for (const auto &page : runtime.metadata().pages)
    if ((page.flags & kVirtualSparseVoxelPageRoot) != 0u)
      rootNodeCount += page.nodeCount;
  assert(runtime.residency().residentNodeCount() == rootNodeCount);
  assert(!runtime.fullyResident());
  SparseVoxelScene rootScene;
  assert(runtime.makeRootScene(rootScene, &error));
  assert(validateSparseVoxelSceneSource(rootScene, &error));
  SparseVoxelSceneManager manager;
  assert(manager.setScene(rootScene, &error));
  assert(manager.blasRootRanges().size() == 2u);
  assert(manager.blasRootRanges()[0].firstRoot == 0u);
  assert(manager.blasRootRanges()[0].rootCount == 8u);
  assert(manager.blasRootRanges()[1].firstRoot == 8u);
  const auto rootOccupancyBounds =
      collectSparseVoxelOccupancyLeafBounds(manager, runtime);
  assert(!rootOccupancyBounds.empty());

  std::vector<uint32_t> bits((runtime.metadata().pageCount + 31u) / 32u, 0u);
  std::vector<uint32_t> priorities(runtime.metadata().pageCount, 0u);
  std::vector<uint32_t> instances(runtime.metadata().pageCount, kInvalidSparseVoxelIndex);
  const uint32_t requestedPage = static_cast<uint32_t>(std::find_if(
      runtime.metadata().pages.begin(), runtime.metadata().pages.end(),
      [](const auto &page) { return (page.flags & kVirtualSparseVoxelPageRoot) == 0u; }) - runtime.metadata().pages.begin());
  assert(requestedPage < runtime.metadata().pageCount);
  bits[requestedPage >> 5u] |= 1u << (requestedPage & 31u);
  priorities[requestedPage] = 0x80001000u;
  instances[requestedPage] = 0u;
  assert(runtime.requestPages(bits, priorities, instances, 3u, &error));
  assert(pumpUntil(runtime, [&] { return runtime.residency().pages()[requestedPage].state == SparseVoxelPageState::Resident; }));
  assert(runtime.residency().residentNodeCount() > rootNodeCount);
  assert(!runtime.fullyResident());
  const auto refinedOccupancyBounds =
      collectSparseVoxelOccupancyLeafBounds(manager, runtime);
  assert(!refinedOccupancyBounds.empty());
  assert(refinedOccupancyBounds.size() != rootOccupancyBounds.size());
  for (const SparseVoxelAABB &bounds : refinedOccupancyBounds)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(std::isfinite(bounds.minimum[axis]));
      assert(std::isfinite(bounds.maximum[axis]));
      assert(bounds.minimum[axis] <= bounds.maximum[axis]);
    }
  assert(readBytes(path) == originalBytes);

  {
    std::ofstream stream(invalid, std::ios::binary);
    stream << "not a vsvo";
  }
  const uint32_t residentBeforeFailure = runtime.residency().residentNodeCount();
  assert(!runtime.openMetadata(invalid.string(), {}, &error));
  assert(runtime.path() == path.string());
  assert(runtime.residency().residentNodeCount() == residentBeforeFailure);

  assert(runtime.requestAllPages(4u, &error));
  assert(pumpUntil(runtime, [&] { return runtime.fullyResident(); }));
  assert(runtime.residency().residentNodeCount() == runtime.metadata().nodeCount);
  for (uint32_t logical = 0u; logical < runtime.metadata().nodeCount; ++logical)
  {
    const uint32_t physical = runtime.residency().logicalToPhysical(logical);
    assert(physical != kInvalidSparseVoxelIndex);
    assert(runtime.residency().physicalMetadata()[physical].logicalNodeIndex == logical);
  }
  assert(readBytes(path) == originalBytes);
  std::filesystem::remove(path);
  std::filesystem::remove(invalid);
}

void testBudgetValidationPreservesClosedState()
{
  SparseVoxelFileRuntime runtime;
  SparseVoxelFileRuntimeOptions options{};
  options.maxInFlightReads = 0u;
  std::string error;
  assert(!runtime.openMetadata("unused.vsvo", options, &error));
  assert(!runtime.isOpen());
}
} // namespace

int main()
{
  testMetadataFirstSelectiveOpenAndNoWrites();
  testBudgetValidationPreservesClosedState();
  return 0;
}
