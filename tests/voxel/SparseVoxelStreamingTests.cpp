#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelSceneFile.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#undef assert
#define assert(condition)                                                                                                                                        \
  do                                                                                                                                                             \
  {                                                                                                                                                              \
    if (!(condition))                                                                                                                                            \
    {                                                                                                                                                            \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);                                                           \
      std::abort();                                                                                                                                              \
    }                                                                                                                                                            \
  } while (false)

namespace
{

using namespace voxel::svraster;

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

SparseVoxelCenterLengthGPU centerLength(const SparseVoxelBLAS &model, const uint64_t path, const uint32_t level)
{
  const auto coordinate = decodeOctreePath(path, level);
  const float length = model.rootExtent / static_cast<float>(uint32_t{1} << level);
  return {{
            model.rootCenter[0] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * length,
            model.rootCenter[1] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * length,
            model.rootCenter[2] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * length,
          },
          length};
}

SparseVoxelScene tinyScene()
{
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.0f, 0.0f, 8.0f};
  model.rootExtent = 4.0f;
  model.maxLevel = 2u;
  model.firstRoot = 0u;
  model.rootCount = 8u;
  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff806040u;
  material.roughnessMetallicUNorm8 = 0x6040u;
  material.materialId = 5u;
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
      const uint32_t nodeIndex = static_cast<uint32_t>(model.hierarchyNodes.size());
      const bool parent = level == 1u;
      model.hierarchyNodes.push_back({parent ? 8u + nodeIndex * 8u : 0u, parent ? 8u : 0u, nodeIndex, level});
      model.octreePaths.push_back(path);
      model.octreeLevels.push_back(static_cast<uint8_t>(level));
      model.centersLengths.push_back(centerLength(model, path, level));
      model.materials.push_back(material);
    }
  }
  scene.models.push_back(std::move(model));
  scene.instances.push_back({0u, 0u, identityMatrix()});
  scene.instances.push_back({0u, 0u, identityMatrix()}); // shared BLAS request source
  return scene;
}

struct Fixture
{
  std::filesystem::path path;
  VirtualSparseVoxelSceneMetadata metadata;

  explicit Fixture(const bool createDefault = true)
  {
    if (!createDefault)
      return;
    path = std::filesystem::temp_directory_path() / "engine-svraster-g16-streaming.vsvo";
    std::string error;
    assert(writeVirtualSparseVoxelScene(path.string(), tinyScene(), {VirtualSparseVoxelFileCompression::LZ4, 704u}, &error));
    assert(error.empty());
    assert(readVirtualSparseVoxelSceneMetadata(path.string(), metadata, &error));
    assert(error.empty());
    assert(metadata.pages.size() == 9u);
    assert((metadata.pages[0].flags & kVirtualSparseVoxelPageRoot) != 0u);
    assert(metadata.pages[0].nodeCount == 8u);
  }

  ~Fixture()
  {
    if (!path.empty())
      std::filesystem::remove(path);
  }
};

SparseVoxelPageInstallBatch installPage(Fixture &fixture, SparseVoxelPageResidency &residency, const uint32_t pageIndex)
{
  std::string error;
  assert(residency.markReading(pageIndex, &error));
  VirtualSparseVoxelPageData page{};
  assert(readVirtualSparseVoxelPage(fixture.path.string(), fixture.metadata, pageIndex, page, &error));
  assert(residency.markDecoded(pageIndex, &error));
  SparseVoxelPageInstallBatch batch{};
  assert(residency.prepareInstall(page, batch, &error));
  const uint64_t previousContentRevision =
      residency.contentRevision();
  assert(residency.commitInstall(batch, &error));
  assert(
      residency.contentRevision() ==
      previousContentRevision + 1u);
  assert(residency.changedPagesSince(previousContentRevision) == std::vector<uint32_t>{pageIndex});
  assert(error.empty());
  return batch;
}

std::set<uint32_t> selectedLogicalCut(const Fixture &fixture, const SparseVoxelPageResidency &residency, const uint32_t modelIndex = 0u)
{
  std::set<uint32_t> selected;
  const auto &model = fixture.metadata.models[modelIndex];
  std::vector<uint32_t> stack;
  for (uint32_t root = 0u; root < model.rootCount; ++root)
    stack.push_back(residency.logicalToPhysical(model.firstRoot + root));
  while (!stack.empty())
  {
    const uint32_t physical = stack.back();
    stack.pop_back();
    assert(physical != kInvalidSparseVoxelIndex);
    const auto &node = residency.physicalNodes()[physical];
    if (node.childCount == 0u)
      selected.insert(residency.physicalMetadata()[physical].logicalNodeIndex);
    else
      for (uint32_t child = 0u; child < node.childCount; ++child)
        stack.push_back(node.firstChild + child);
  }
  return selected;
}

SparseVoxelScene multiModelScene()
{
  SparseVoxelScene scene = tinyScene();
  SparseVoxelBLAS second = scene.models[0];
  second.rootCenter[0] = 12.0f;
  for (uint32_t nodeIndex = 0u; nodeIndex < second.hierarchyNodes.size(); ++nodeIndex)
    second.centersLengths[nodeIndex] = centerLength(second, second.octreePaths[nodeIndex], second.octreeLevels[nodeIndex]);
  scene.models.push_back(std::move(second));
  auto overlapTransform = identityMatrix();
  overlapTransform[12] = -12.0f;
  scene.instances.push_back({1u, 0u, overlapTransform});
  return scene;
}

void testPriorityQuantizationAndSharedBLASReduction()
{
  VirtualSparseVoxelFilePageRecord page{};
  page.localBoundsMin = {-1.0f, -1.0f, 7.0f};
  page.localBoundsMax = {1.0f, 1.0f, 9.0f};
  SparseVoxelPagePriorityView nearView{};
  nearView.modelToWorld = identityMatrix();
  nearView.worldToCamera = identityMatrix();
  nearView.focalPixels = {800.0f, 800.0f};
  nearView.cameraWorldPosition = {0.0f, 0.0f, 0.0f};
  SparseVoxelPagePriorityView farView = nearView;
  farView.modelToWorld[14] = 32.0f;
  const uint32_t nearPriority = scoreSparseVoxelPagePriority(page, nearView);
  const uint32_t farPriority = scoreSparseVoxelPagePriority(page, farView);
  assert((nearPriority & 0x80000000u) != 0u);
  assert(nearPriority > farPriority);
  assert(quantizeSparseVoxelPagePriority(false, 1000.0f, 1.0f) < quantizeSparseVoxelPagePriority(true, 0.0f, 1000.0f));
  assert(quantizeSparseVoxelPagePriority(true, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()) == 0x80000000u);

  Fixture fixture;
  SparseVoxelPageResidency residency;
  std::string error;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  assert(residency.requestPage(1u, farPriority, 7u, 3u, &error));
  assert(residency.requestPage(1u, nearPriority, 3u, 4u, &error));
  assert(residency.requestPage(1u, nearPriority, 1u, 4u, &error));
  assert(residency.pages()[1].priority == nearPriority);
  assert(residency.pages()[1].requestingInstance == 1u);
  assert(residency.pages()[1].lastRequestedFrame == 4u);
}

void testDependencyAdmissionPhysicalRewritesAndCoarseFallback()
{
  Fixture fixture;
  SparseVoxelPageResidency residency;
  std::string error;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  assert(residency.pages()[0].state == SparseVoxelPageState::Requested);
  auto admissions = residency.planAdmissions({8u, 64u, std::numeric_limits<uint64_t>::max()});
  assert(admissions == std::vector<uint32_t>{0u});
  const auto rootBatch = installPage(fixture, residency, 0u);
  assert(rootBatch.physicalFirstNode == 0u);
  assert(residency.residentNodeCount() == 8u);
  assert(selectedLogicalCut(fixture, residency).size() == 8u);

  const uint32_t low = quantizeSparseVoxelPagePriority(true, 16.0f, 20.0f);
  const uint32_t high = quantizeSparseVoxelPagePriority(true, 32.0f, 10.0f);
  assert(residency.requestPage(1u, low, 0u, 10u, &error));
  assert(residency.requestPage(2u, high, 1u, 10u, &error));
  admissions = residency.planAdmissions({2u, 16u, std::numeric_limits<uint64_t>::max()});
  assert(admissions == (std::vector<uint32_t>{2u, 1u}));
  const auto page2Batch = installPage(fixture, residency, 2u);
  const auto page1Batch = installPage(fixture, residency, 1u);
  assert(page2Batch.physicalFirstNode == 8u);
  assert(page1Batch.physicalFirstNode == 16u);
  assert(residency.freeNodeCount() == 0u);
  assert(selectedLogicalCut(fixture, residency).size() == 22u);

  for (const auto &rewrite : page2Batch.publishRewrites)
  {
    assert(rewrite.nodeIndex < 8u);
    assert(rewrite.firstChild >= page2Batch.physicalFirstNode);
    assert(rewrite.firstChild + rewrite.childCount <= page2Batch.physicalFirstNode + page2Batch.nodes.size());
    assert(residency.physicalNodes()[rewrite.nodeIndex].firstChild == rewrite.firstChild);
  }
  for (uint32_t offset = 0u; offset < page2Batch.metadata.size(); ++offset)
  {
    const auto &metadata = residency.physicalMetadata()[page2Batch.physicalFirstNode + offset];
    assert(metadata.logicalNodeIndex == fixture.metadata.pages[2].firstNode + offset);
    assert(metadata.owningPageIndex == 2u);
  }

  SparseVoxelPageEvictionBatch invalidRoot{};
  assert(!residency.prepareEviction(0u, invalidRoot, &error));
  assert(!error.empty());
  const auto evictions = residency.planEvictions(8u, {1u});
  assert(evictions == std::vector<uint32_t>{2u});
  SparseVoxelPageEvictionBatch eviction{};
  assert(residency.prepareEviction(2u, eviction, &error));
  const uint64_t previousContentRevision =
      residency.contentRevision();
  assert(residency.commitEviction(eviction, &error));
  assert(
      residency.contentRevision() ==
      previousContentRevision + 1u);
  assert(residency.changedPagesSince(previousContentRevision) == std::vector<uint32_t>{2u});
  assert(residency.freeNodeCount() == 8u);
  assert(selectedLogicalCut(fixture, residency).size() == 15u);
  for (const auto &rewrite : eviction.unpublishRewrites)
    assert(residency.physicalNodes()[rewrite.nodeIndex].childCount == 0u);

  assert(residency.requestPage(3u, high, 0u, 11u, &error));
  const auto page3Batch = installPage(fixture, residency, 3u);
  assert(page3Batch.physicalFirstNode == page2Batch.physicalFirstNode);
  assert(selectedLogicalCut(fixture, residency).size() == 22u);
}

void testFailureCancellationBudgetsAndBatchValidation()
{
  Fixture fixture;
  SparseVoxelPageResidency residency;
  std::string error;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  installPage(fixture, residency, 0u);

  const uint32_t priority = quantizeSparseVoxelPagePriority(true, 64.0f, 2.0f);
  assert(residency.requestPage(4u, priority, 0u, 20u, &error));
  assert(residency.markReading(4u, &error));
  assert(residency.pages()[4].state == SparseVoxelPageState::Reading); // delayed IO keeps the coarse cut
  assert(selectedLogicalCut(fixture, residency).size() == 8u);
  assert(residency.failIO(4u, true, &error));
  assert(residency.pages()[4].state == SparseVoxelPageState::Requested);
  assert(residency.markReading(4u, &error));
  assert(residency.failIO(4u, false, &error));
  assert(residency.pages()[4].state == SparseVoxelPageState::Unloaded);
  assert(selectedLogicalCut(fixture, residency).size() == 8u);

  assert(residency.requestPage(5u, priority, 0u, 21u, &error));
  assert(residency.planAdmissions({1u, 7u, std::numeric_limits<uint64_t>::max()}).empty());
  assert(residency.planAdmissions({1u, 8u, fixture.metadata.pages[5].storedSize - 1u}).empty());
  assert(residency.planAdmissions({1u, 8u, fixture.metadata.pages[5].storedSize}) == std::vector<uint32_t>{5u});
  assert(residency.markReading(5u, &error));
  VirtualSparseVoxelPageData page{};
  assert(readVirtualSparseVoxelPage(fixture.path.string(), fixture.metadata, 5u, page, &error));
  assert(residency.markDecoded(5u, &error));
  SparseVoxelPageInstallBatch batch{};
  assert(residency.prepareInstall(page, batch, &error));
  assert(residency.freeNodeCount() == 8u); // excludes the queued physical range
  SparseVoxelPageInstallBatch malformed = batch;
  malformed.metadata.clear();
  assert(!residency.commitInstall(malformed, &error));
  assert(residency.cancelInstall(batch, &error));
  assert(residency.pages()[5].state == SparseVoxelPageState::Decoded);
  assert(residency.freeNodeCount() == 16u);
  assert(selectedLogicalCut(fixture, residency).size() == 8u);

  SparseVoxelPageInstallBatch stale = batch;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  assert(!residency.commitInstall(stale, &error));
  assert(!error.empty());
}

void testRecursiveDependencyAdmissionAndDescendantFirstEviction()
{
  Fixture fixture;
  // Turn the valid two-level fixture into a scheduler-only dependency chain.
  fixture.metadata.pages[2].dependencyFirst = fixture.metadata.pageDependencies.size();
  fixture.metadata.pages[2].dependencyCount = 1u;
  fixture.metadata.pageDependencies.push_back(1u);
  SparseVoxelPageResidency residency;
  std::string error;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  assert(residency.requestPage(2u, 123u, 4u, 9u, &error));
  const auto admissions = residency.planAdmissions({3u, 24u, std::numeric_limits<uint64_t>::max()});
  assert(admissions == (std::vector<uint32_t>{0u, 1u, 2u}));
  assert(residency.pages()[1].priority == 123u);
  assert(residency.pages()[1].requestingInstance == 4u);
}

void testMultiModelSelectiveCutsAndRendererPayloadParity()
{
  const SparseVoxelScene source = multiModelScene();
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "engine-svraster-g16-multi-model.vsvo";
  std::string error;
  assert(writeVirtualSparseVoxelScene(path.string(), source, {VirtualSparseVoxelFileCompression::None, 704u}, &error));
  Fixture fixture(false);
  fixture.path = path;
  assert(readVirtualSparseVoxelSceneMetadata(path.string(), fixture.metadata, &error));
  assert(fixture.metadata.modelCount == 2u);
  assert(fixture.metadata.instanceCount == 3u);
  assert(fixture.metadata.models[0].rootPage != fixture.metadata.models[1].rootPage);
  assert(fixture.metadata.instances[2].blasIndex == 1u);
  assert(fixture.metadata.instances[2].modelToWorld[12] == -12.0f); // transformed overlap

  SparseVoxelPageResidency residency;
  assert(residency.initialize(fixture.metadata, 32u, &error));
  auto admissions = residency.planAdmissions({4u, 32u, std::numeric_limits<uint64_t>::max()});
  assert(admissions.size() == 2u);
  assert(admissions[0] == fixture.metadata.models[0].rootPage);
  assert(admissions[1] == fixture.metadata.models[1].rootPage);
  for (const uint32_t pageIndex : admissions)
    installPage(fixture, residency, pageIndex);

  auto firstFinePage = [&](const uint32_t modelIndex)
  {
    for (uint32_t pageIndex = 0u; pageIndex < fixture.metadata.pages.size(); ++pageIndex)
      if (fixture.metadata.pages[pageIndex].modelIndex == modelIndex && (fixture.metadata.pages[pageIndex].flags & kVirtualSparseVoxelPageRoot) == 0u)
        return pageIndex;
    return kInvalidSparseVoxelIndex;
  };
  const uint32_t model0Fine = firstFinePage(0u);
  const uint32_t model1Fine = firstFinePage(1u);
  assert(fixture.metadata.pages[model0Fine].modelIndex == 0u);
  assert(fixture.metadata.pages[model1Fine].modelIndex == 1u);
  assert(residency.requestPage(model1Fine, 200u, 2u, 8u, &error));
  assert(residency.requestPage(model0Fine, 100u, 0u, 8u, &error));
  admissions = residency.planAdmissions({2u, 16u, std::numeric_limits<uint64_t>::max()});
  assert(admissions == (std::vector<uint32_t>{model1Fine, model0Fine}));
  for (const uint32_t pageIndex : admissions)
    installPage(fixture, residency, pageIndex);
  assert(selectedLogicalCut(fixture, residency, 0u).size() == 15u);
  assert(selectedLogicalCut(fixture, residency, 1u).size() == 15u);

  // The accepted raster passes index all geometry/material arrays by physical
  // voxel index. Prove every selected record is byte-identical to its authored
  // logical record so a selective cut and the equivalent dense cut feed those
  // passes identically despite adversarial physical page order.
  for (uint32_t physical = 0u; physical < residency.physicalNodeCapacity(); ++physical)
  {
    const uint32_t logical = residency.physicalMetadata()[physical].logicalNodeIndex;
    if (logical == kInvalidSparseVoxelIndex)
      continue;
    uint32_t modelIndex = 0u;
    while (modelIndex + 1u < fixture.metadata.models.size() && logical >= fixture.metadata.models[modelIndex].firstNode + fixture.metadata.models[modelIndex].nodeCount)
      ++modelIndex;
    const uint32_t local = logical - fixture.metadata.models[modelIndex].firstNode;
    const auto &model = source.models[modelIndex];
    const auto &pathLevel = residency.physicalPathsLevels()[physical];
    assert(pathLevel.pathLo == static_cast<uint32_t>(model.octreePaths[local]));
    assert(pathLevel.pathHi == static_cast<uint32_t>(model.octreePaths[local] >> 32u));
    assert(pathLevel.level == model.octreeLevels[local]);
    assert(std::memcmp(&residency.physicalCentersLengths()[physical], &model.centersLengths[local], sizeof(SparseVoxelCenterLengthGPU)) == 0);
    assert(std::memcmp(&residency.physicalMaterials()[physical], &model.materials[local], sizeof(SparseVoxelMaterialGPU)) == 0);
  }
  std::filesystem::remove(path);
}

void testBoundedAsyncIOAndGenerationCancellation()
{
  Fixture fixture;
  SparseVoxelPageResidency residency;
  SparseVoxelAsyncPageReader reader;
  std::string error;
  assert(residency.initialize(fixture.metadata, 24u, &error));
  installPage(fixture, residency, 0u);
  assert(reader.initialize(fixture.path.string(), fixture.metadata, residency.generation(), 1u, &error));
  assert(residency.requestPage(1u, 10u, 0u, 1u, &error));
  assert(residency.markReading(1u, &error));
  assert(reader.enqueue(1u, &error));
  assert(!reader.enqueue(2u, &error));
  assert(!error.empty());
  std::vector<SparseVoxelPageReadCompletion> completions;
  for (uint32_t attempt = 0u; attempt < 1000u && completions.empty(); ++attempt)
  {
    completions = reader.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(completions.size() == 1u);
  assert(completions[0].success && !completions[0].stale && completions[0].pageIndex == 1u);
  assert(residency.markDecoded(1u, &error));
  SparseVoxelPageInstallBatch install{};
  assert(residency.prepareInstall(completions[0].page, install, &error));
  assert(residency.commitInstall(install, &error));

  assert(residency.requestPage(2u, 20u, 0u, 2u, &error));
  assert(residency.markReading(2u, &error));
  assert(reader.enqueue(2u, &error));
  reader.cancelAndAdvanceGeneration();
  completions.clear();
  for (uint32_t attempt = 0u; attempt < 1000u && completions.empty(); ++attempt)
  {
    completions = reader.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(completions.size() == 1u);
  assert(!completions[0].success && completions[0].stale && completions[0].page.nodes.empty());
  assert(residency.failIO(2u, false, &error));
  assert(residency.pages()[2].state == SparseVoxelPageState::Unloaded);
  assert(selectedLogicalCut(fixture, residency).size() == 15u);
}

} // namespace

int main()
{
  testPriorityQuantizationAndSharedBLASReduction();
  testDependencyAdmissionPhysicalRewritesAndCoarseFallback();
  testFailureCancellationBudgetsAndBatchValidation();
  testRecursiveDependencyAdmissionAndDescendantFirstEviction();
  testMultiModelSelectiveCutsAndRendererPayloadParity();
  testBoundedAsyncIOAndGenerationCancellation();
  return 0;
}
