// Phase 9 of docs/terrain-vg-unified-hierarchy-plan.md: split the packer into
// a route-specific front half (clusters + LOD DAG) and a shared back half
// (page packing, rewrites, encode, compress), then parallelise the back half.
//
// This is the safety net that has to exist first. The whole refactor is gated
// on "the encoded asset is byte-identical before and after", and that gate is
// meaningless unless the builder is deterministic to begin with - so this
// pins the property directly:
//
//   1. The same source mesh built twice produces byte-identical pages,
//      hierarchy and page dependencies.
//   2. It stays byte-identical when the pages are encoded in a different
//      order, which is what per-page parallel encode will do (phase 9c).
//
// Point 2 is the one that matters for 9d: a fan-out that accumulates into a
// shared container in completion order would pass point 1 and fail here, and
// it would take a byte-identical gate with it.

#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "editor/virtualgeometry/VirtualGeometryPacker.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "TerrainGenerationPhase0Utils.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace vg = virtualgeometry;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cout << "  FAILED " << what << "\n";
  ++failures;
}

// A grid mesh big enough to produce several clusters and at least one
// simplified LOD level - a single triangle would pass every check here while
// exercising none of the packer.
void makeGrid(const uint32_t cells, std::vector<vg::Vertex> &vertices, vg::Shape &shape)
{
  vertices.clear();
  shape.indices.clear();
  const uint32_t side = cells + 1u;
  for (uint32_t z = 0u; z < side; ++z)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const float fx = static_cast<float>(x);
      const float fz = static_cast<float>(z);
      // A little relief, so simplification has something to collapse and the
      // cluster bounds are not degenerate.
      const float fy = 0.25f * static_cast<float>((x * 7u + z * 13u) % 5u);
      vg::Vertex vertex(fx, fy, fz);
      vertex.norm[1] = 1.0f;
      vertex.uv[0] = fx / static_cast<float>(cells);
      vertex.uv[1] = fz / static_cast<float>(cells);
      vertices.push_back(vertex);
    }
  }
  for (uint32_t z = 0u; z < cells; ++z)
  {
    for (uint32_t x = 0u; x < cells; ++x)
    {
      const uint32_t base = z * side + x;
      shape.indices.push_back(base);
      shape.indices.push_back(base + side);
      shape.indices.push_back(base + 1u);
      shape.indices.push_back(base + 1u);
      shape.indices.push_back(base + side);
      shape.indices.push_back(base + side + 1u);
    }
  }
}

// Everything the back half emits, flattened: the page words exactly as they
// reach the scene, plus the tables the install path reads alongside them.
struct EncodedFingerprint
{
  std::vector<std::vector<uint32_t>> pageWords;
  std::vector<std::vector<uint32_t>> dependencies;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  uint32_t rootPageIndex = 0u;
};

EncodedFingerprint fingerprint(const vg::VirtualGeometryEncodedData &asset)
{
  EncodedFingerprint print;
  print.rootPageIndex = asset.rootPageIndex;
  print.hierarchy = asset.hierarchy;
  print.pageWords.reserve(asset.pages.size());
  print.dependencies.reserve(asset.pages.size());
  for (const auto &page : asset.pages)
  {
    print.pageWords.push_back(vg::PageBuffer::encode(page).data);
    print.dependencies.push_back(page.dependencies);
  }
  return print;
}

bool sameHierarchy(
    const std::vector<vg::VirtualGeometryHierarchy> &left,
    const std::vector<vg::VirtualGeometryHierarchy> &right)
{
  if (left.size() != right.size())
    return false;
  for (size_t index = 0u; index < left.size(); ++index)
    if (std::memcmp(&left[index], &right[index], sizeof(vg::VirtualGeometryHierarchy)) != 0)
      return false;
  return true;
}

vg::VirtualGeometryEncodedData buildGrid(const uint32_t cells)
{
  std::vector<vg::Vertex> vertices;
  vg::Shape shape;
  makeGrid(cells, vertices, shape);
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  vg::VirtualGeometryBuildSettings settings{};
  // Force the packer to spill across pages. A page holds 64 groups and this
  // fixture makes 6, so at the default the whole asset lands on one page and
  // the multi-page paths - dependencies, per-page rewrites, the root page
  // choice - would go untested.
  settings.maxGroupsPerPage = 2u;
  return vg::VirtualGeometryEncoder::encode(vertices, shape, quantization, settings);
}

// One number for everything the back half emits, so a refactor of the packer
// (phase 9a onward) is gated on "the encoded asset is byte-identical": the
// same fixture must hash to the same value before and after. The values are
// recorded from the builder as it stood before the split; they are what
// this machine produces (the triangle route runs meshoptimizer, whose
// floating point is deterministic here but not promised across platforms).
uint64_t fingerprintHash(const EncodedFingerprint &print)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](const uint64_t value)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(print.rootPageIndex);
  mix(print.pageWords.size());
  for (const auto &words : print.pageWords)
  {
    mix(words.size());
    for (const uint32_t word : words)
      mix(word);
  }
  mix(print.dependencies.size());
  for (const auto &dependencies : print.dependencies)
  {
    mix(dependencies.size());
    for (const uint32_t dependency : dependencies)
      mix(dependency);
  }
  mix(print.hierarchy.size());
  for (const auto &node : print.hierarchy)
  {
    uint32_t words[sizeof(vg::VirtualGeometryHierarchy) / sizeof(uint32_t)];
    std::memcpy(words, &node, sizeof(node));
    for (const uint32_t word : words)
      mix(word);
  }
  return hash;
}

std::string hex(const uint64_t value)
{
  std::ostringstream stream;
  stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return stream.str();
}

// The voxel route's fixture: one terrain chunk through the octree builder and
// the same back half, exactly as the terrain page cache builds it.
bool buildVoxelFixture(vg::VirtualGeometryEncodedData &asset)
{
  const auto bundle = terrain_phase0::makeFixture(terrain_phase0::FixtureKind::River);
  const auto settings = terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::River, 0u);
  voxel::svraster::PlanetVoxelChunkSet chunks{};
  std::string error;
  voxel::svraster::WorldMapVoxelGenerationMetrics metrics{};
  if (!voxel::svraster::buildWorldMapVoxelTerrain(bundle, settings, chunks, &error, {}, nullptr, &metrics) ||
      chunks.chunks.empty())
  {
    std::cout << "  voxel fixture terrain failed: " << error << "\n";
    return false;
  }
  voxel::svraster::SparseVoxelOctreeVirtualGeometrySettings octree{};
  octree.absoluteOrigin = true;
  octree.leafSelfError = static_cast<float>(chunks.chunks[0].voxelSizeMetres);
  octree.parentChunkError = static_cast<float>(chunks.chunks[0].voxelSizeMetres * 2.0);
  vg::VirtualGeometryBuildSettings build{};
  build.maxGroupsPerPage = 2u;
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  quantization.padMeshlets = true;
  if (!voxel::svraster::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
          chunks, 0u, octree, build, quantization, asset, nullptr, nullptr, &error))
  {
    std::cout << "  voxel fixture build failed: " << error << "\n";
    return false;
  }
  return !asset.pages.empty();
}

// Recorded 2026-09-08 from the builder before the phase 9 split. A zero means
// "not recorded yet": the test then prints the value to record and passes.
constexpr uint64_t kGoldenTriangleFingerprint = 0x798d7f109865a19eull;
// Updated for closed selectable blocks and complete root-page coverage.
// Face coverage, shared planes, and root-prefix completeness are checked by
// SparseVoxelOctreeVirtualGeometryTests; the triangle fixture is unchanged.
constexpr uint64_t kGoldenVoxelFingerprint = 0x7988f834270a5521ull;

void testGoldenFingerprints()
{
  std::cout << "testGoldenFingerprints\n";
  const uint64_t triangle = fingerprintHash(fingerprint(buildGrid(24u)));
  std::cout << "  triangle fingerprint " << hex(triangle) << "\n";
  if (kGoldenTriangleFingerprint != 0u)
    check(triangle == kGoldenTriangleFingerprint, "the triangle route's encoded asset is byte-identical to the recorded one");

  vg::VirtualGeometryEncodedData voxelAsset;
  if (!buildVoxelFixture(voxelAsset))
  {
    check(false, "the voxel fixture builds");
    return;
  }
  check(voxelAsset.pages.size() > 1u, "the voxel fixture spans more than one page");
  const uint64_t voxel = fingerprintHash(fingerprint(voxelAsset));
  std::cout << "  voxel fingerprint " << hex(voxel) << "\n";
  if (kGoldenVoxelFingerprint != 0u)
    check(voxel == kGoldenVoxelFingerprint, "the voxel route's encoded asset is byte-identical to the recorded one");
  // And the voxel route is deterministic too.
  vg::VirtualGeometryEncodedData again;
  check(buildVoxelFixture(again) && fingerprintHash(fingerprint(again)) == voxel, "the voxel route builds the same bytes twice");
}

void testRepeatedBuildsAreIdentical()
{
  std::cout << "testRepeatedBuildsAreIdentical\n";
  const auto first = buildGrid(24u);
  const auto second = buildGrid(24u);

  check(!first.pages.empty(), "the fixture produced pages");
  check(first.pages.size() > 1u, "and more than one, so page packing ran");
  check(!first.hierarchy.empty(), "and a hierarchy");

  const auto a = fingerprint(first);
  const auto b = fingerprint(second);
  check(a.pageWords.size() == b.pageWords.size(), "both builds produced the same page count");
  check(a.rootPageIndex == b.rootPageIndex, "both chose the same root page");
  check(sameHierarchy(a.hierarchy, b.hierarchy), "the hierarchy is byte-identical");
  bool wordsIdentical = a.pageWords.size() == b.pageWords.size();
  for (size_t page = 0u; wordsIdentical && page < a.pageWords.size(); ++page)
    wordsIdentical = a.pageWords[page] == b.pageWords[page];
  check(wordsIdentical, "every page's encoded words are byte-identical");
  bool dependenciesIdentical = a.dependencies.size() == b.dependencies.size();
  for (size_t page = 0u; dependenciesIdentical && page < a.dependencies.size(); ++page)
    dependenciesIdentical = a.dependencies[page] == b.dependencies[page];
  check(dependenciesIdentical, "every page's dependency list is byte-identical");
}

void testEncodeOrderDoesNotMatter()
{
  std::cout << "testEncodeOrderDoesNotMatter\n";
  // Phase 9c fans `PageBuffer::encode` out over pages. A page's encoding must
  // depend on nothing but that page, or the fan-out changes the bytes and the
  // refactor's byte-identical gate quietly stops meaning anything.
  const auto asset = buildGrid(24u);
  std::vector<std::vector<uint32_t>> forward;
  forward.reserve(asset.pages.size());
  for (const auto &page : asset.pages)
    forward.push_back(vg::PageBuffer::encode(page).data);

  std::vector<std::vector<uint32_t>> reverse(asset.pages.size());
  for (size_t index = asset.pages.size(); index != 0u; --index)
    reverse[index - 1u] = vg::PageBuffer::encode(asset.pages[index - 1u]).data;

  check(forward.size() == reverse.size(), "same page count both ways");
  bool identical = forward.size() == reverse.size();
  for (size_t page = 0u; identical && page < forward.size(); ++page)
    identical = forward[page] == reverse[page];
  check(identical, "encoding pages back-to-front gives the same bytes");

  // And encoding one page twice must be stable, which is what lets a retry or
  // a second worker produce the same result.
  bool stable = true;
  for (size_t page = 0u; stable && page < asset.pages.size(); ++page)
    stable = vg::PageBuffer::encode(asset.pages[page]).data == forward[page];
  check(stable, "re-encoding a page gives the same bytes");
}

// Phase 9c: the back half's fan-outs must not change a byte. A fixture big
// enough to cross the parallel threshold is encoded serially and on several
// threads, and written to a file both ways; the encoded fingerprints and the
// file bytes have to match exactly.
std::string tempPath(const std::string &name)
{
  const char *tmp = std::getenv("TMPDIR");
  std::string directory = tmp != nullptr ? tmp : "/tmp/";
  if (directory.back() != '/')
    directory.push_back('/');
  return directory + "vg-packer-determinism-" + name + ".vg";
}

std::vector<uint8_t> readFile(const std::string &path)
{
  std::vector<uint8_t> bytes;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return bytes;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0)
  {
    bytes.resize(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1u, bytes.size(), file) != bytes.size())
      bytes.clear();
  }
  std::fclose(file);
  return bytes;
}

void testParallelBackHalfIsByteIdentical()
{
  std::cout << "testParallelBackHalfIsByteIdentical\n";
  std::vector<vg::Vertex> vertices;
  vg::Shape shape;
  makeGrid(96u, vertices, shape);
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  vg::VirtualGeometryBuildSettings settings{};
  settings.maxGroupsPerPage = 2u;
  vg::VirtualGeometryBuildData build = vg::VirtualGeometryBuilder::build(vertices, std::vector<vg::Shape>{shape}, settings);
  check(build.pages.size() >= vg::VirtualGeometryPacker::kParallelThreshold, "the fixture has enough pages to fan out (" + std::to_string(build.pages.size()) + ")");
  check(build.timings.partition > 0.0 && build.timings.packPages > 0.0, "the front half and packPages report their time");

  vg::VirtualGeometryBuildTimings serialTimings{};
  vg::VirtualGeometryBuildTimings parallelTimings{};
  const auto serial = vg::VirtualGeometryPacker::encode(build, quantization, &serialTimings, 1u);
  const auto parallel = vg::VirtualGeometryPacker::encode(build, quantization, &parallelTimings, 4u);
  check(serialTimings.encodeThreads == 1u && parallelTimings.encodeThreads == 4u, "the requested thread counts were used");
  check(fingerprintHash(fingerprint(serial)) == fingerprintHash(fingerprint(parallel)), "serial and four-thread encode give byte-identical pages, dependencies and hierarchy");
  bool sameUpdates = serial.pages.size() == parallel.pages.size();
  for (size_t page = 0u; sameUpdates && page < serial.pages.size(); ++page)
    sameUpdates = serial.pages[page].installUpdates.hierarchyUpdates.size() == parallel.pages[page].installUpdates.hierarchyUpdates.size() &&
        serial.pages[page].groupMeshletOffsets == parallel.pages[page].groupMeshletOffsets;
  check(sameUpdates, "and the same per-page tables");
  // The automatic choice on this fixture is parallel, and identical too.
  const auto automatic = vg::VirtualGeometryCompressor::encode(build, quantization);
  check(fingerprintHash(fingerprint(automatic)) == fingerprintHash(fingerprint(serial)), "the default encode path is identical to the serial one");

  const std::string serialPath = tempPath("serial");
  const std::string parallelPath = tempPath("parallel");
  std::remove(serialPath.c_str());
  std::remove(parallelPath.c_str());
  {
    vg::VirtualGeometryFile writer(serialPath, true);
    vg::VirtualGeometryBuildTimings timings{};
    check(writer.isOpen() && writer.write(serial, build.pages, vg::MESHLET_LZ4, &timings, 1u), "writes serially");
    check(timings.compressThreads == 1u, "on one thread");
  }
  {
    vg::VirtualGeometryFile writer(parallelPath, true);
    vg::VirtualGeometryBuildTimings timings{};
    check(writer.isOpen() && writer.write(parallel, build.pages, vg::MESHLET_LZ4, &timings, 4u), "writes with four compression threads");
    check(timings.compressThreads == 4u, "on four threads");
  }
  const auto serialBytes = readFile(serialPath);
  const auto parallelBytes = readFile(parallelPath);
  check(!serialBytes.empty() && serialBytes == parallelBytes, "the two files are byte-identical");
  std::remove(serialPath.c_str());
  std::remove(parallelPath.c_str());
}

// Phase 9c, the front half. The same fixture is built serially and on four
// threads, once as a single shape (the per-level group fan-out) and once as
// many shapes (the per-part fan-out). METIS keeps its random state in globals,
// which is exactly the kind of thing this catches: before its calls were
// serialised, an eight-thread Sponza build differed from the serial one and
// from itself.
uint64_t buildFingerprint(
    std::vector<vg::Vertex> vertices,
    const std::vector<vg::Shape> &shapes,
    const uint32_t frontThreads,
    size_t *clusterCount)
{
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  vg::VirtualGeometryBuildSettings settings{};
  settings.maxGroupsPerPage = 2u;
  settings.frontThreads = frontThreads;
  vg::VirtualGeometryBuildData build = vg::VirtualGeometryBuilder::build(vertices, shapes, settings);
  if (clusterCount != nullptr)
    *clusterCount = build.clusters.size();
  return fingerprintHash(fingerprint(vg::VirtualGeometryPacker::encode(build, quantization, nullptr, 1u)));
}

void testParallelFrontHalfIsByteIdentical()
{
  std::cout << "testParallelFrontHalfIsByteIdentical\n";
  {
    std::vector<vg::Vertex> vertices;
    vg::Shape shape;
    makeGrid(96u, vertices, shape);
    size_t serialClusters = 0u;
    size_t parallelClusters = 0u;
    const uint64_t serial = buildFingerprint(vertices, {shape}, 1u, &serialClusters);
    const uint64_t parallel = buildFingerprint(vertices, {shape}, 4u, &parallelClusters);
    check(serialClusters == parallelClusters, "one shape: the same cluster count on one and four threads (" + std::to_string(serialClusters) + ")");
    check(serial == parallel, "one shape: the per-level group fan-out gives byte-identical bytes");
    const uint64_t again = buildFingerprint(vertices, {shape}, 4u, nullptr);
    check(again == parallel, "and the same bytes on a second four-thread build");
  }
  {
    // Twelve shapes, each its own grid offset along x, so the per-part fan-out
    // has more jobs than the parallel threshold.
    std::vector<vg::Vertex> vertices;
    std::vector<vg::Shape> shapes;
    for (uint32_t part = 0u; part < 12u; ++part)
    {
      std::vector<vg::Vertex> partVertices;
      vg::Shape partShape;
      makeGrid(12u + part, partVertices, partShape);
      const uint32_t base = static_cast<uint32_t>(vertices.size());
      for (auto vertex : partVertices)
      {
        vertex.pos[0] += 100.0f * static_cast<float>(part);
        vertices.push_back(vertex);
      }
      for (uint32_t &index : partShape.indices)
        index += base;
      shapes.push_back(std::move(partShape));
    }
    size_t serialClusters = 0u;
    size_t parallelClusters = 0u;
    const uint64_t serial = buildFingerprint(vertices, shapes, 1u, &serialClusters);
    const uint64_t parallel = buildFingerprint(vertices, shapes, 4u, &parallelClusters);
    check(serialClusters == parallelClusters, "twelve shapes: the same cluster count on one and four threads (" + std::to_string(serialClusters) + ")");
    check(serial == parallel, "twelve shapes: the per-part fan-out gives byte-identical bytes");
    const uint64_t again = buildFingerprint(vertices, shapes, 4u, nullptr);
    check(again == parallel, "and the same bytes on a second four-thread build");
  }
}

// The page order is a topological order of the page DAG, and that is what
// lets every dependency check collapse to a prefix test.
//
// `buildPagesAndRewrites` assigns groups to pages by expanding the cluster cut
// from the roots, so a cluster's DAG parents are always placed on an earlier
// page than the cluster itself, and it also chains every page to its
// predecessor. Together those say: page i's dependencies all lie in [0, i),
// and i-1 is among them. So "the pages installed are a prefix" is exactly
// equivalent to "every installed page has its dependencies installed", and a
// caller that installs prefixes needs no per-dependency walk at all - it needs
// to know that i-1 is resident.
//
// Asserting it here rather than assuming it: the whole install path, the
// ancestor-closure refusal and the scene's dependency reference counts are
// built on this, and a packer change that broke it would otherwise surface as
// a streaming-leaf bug days later.
void checkPageOrderIsTopological(
    const vg::VirtualGeometryEncodedData &asset, const std::string &what)
{
  bool backward = true;
  bool chained = true;
  bool selfOnly = true;
  for (size_t page = 0u; page < asset.pages.size(); ++page)
  {
    bool hasPredecessor = page == 0u;
    for (const uint32_t dependency : asset.pages[page].dependencies)
    {
      if (dependency == page)
      {
        // The builder may list a page as its own dependency; it is inert.
        continue;
      }
      if (dependency > page)
        backward = false;
      if (dependency + 1u == page)
        hasPredecessor = true;
      selfOnly = false;
    }
    if (!hasPredecessor)
      chained = false;
  }
  check(backward, what + ": every page dependency points at an earlier page");
  check(chained, what + ": every page after the first depends on its predecessor");
  check(!selfOnly || asset.pages.size() <= 1u, what + ": multi-page assets carry real dependencies");
  // The root page is inside the installable prefix, which is why a chunk
  // install writes pages 0..rootPageIndex rather than the root alone.
  check(asset.rootPageIndex < asset.pages.size(), what + ": the root page index is in range");
}

// Splitting groups across pages must be as deterministic as everything else,
// and must not disturb the page order the install path depends on.
void testSplitGroupsAreDeterministicAndOrdered()
{
  std::cout << "testSplitGroupsAreDeterministicAndOrdered\n";
  std::vector<vg::Vertex> vertices;
  vg::Shape shape;
  makeGrid(48u, vertices, shape);
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;

  const auto buildSplit = [&](const uint64_t budget, const uint32_t frontThreads)
  {
    vg::VirtualGeometryBuildSettings settings{};
    settings.maxGroupsPerPage = 2u;
    settings.splitGroupsAcrossPages = true;
    settings.maxPageBytes = budget;
    settings.frontThreads = frontThreads;
    std::vector<vg::Vertex> localVertices = vertices;
    return vg::VirtualGeometryBuilder::build(localVertices, std::vector<vg::Shape>{shape}, settings);
  };

  // A budget that lands inside a group rather than between two.
  uint64_t chosenBudget = 0u;
  vg::VirtualGeometryBuildData build;
  for (uint64_t budget = 2048u; budget <= 64u * 1024u && chosenBudget == 0u; budget += 512u)
  {
    auto candidate = buildSplit(budget, 1u);
    if (candidate.pages.size() <= 1u)
      continue;
    // A page whose group spans carry only part of a group is the signal.
    bool straddles = false;
    std::vector<uint32_t> placed(candidate.groupInfos.size(), 0u);
    for (const auto &page : candidate.pages)
      for (const auto &span : page.groups)
      {
        if (span.globalGroupId >= candidate.groupInfos.size())
          continue;
        const size_t groupSize =
            candidate.groupInfos[span.globalGroupId].originalClusterIndices.size();
        if (span.clusterCount < groupSize)
          straddles = true;
        placed[span.globalGroupId] += span.clusterCount;
      }
    if (!straddles)
      continue;
    chosenBudget = budget;
    build = std::move(candidate);
  }
  check(chosenBudget != 0u, "a byte budget splits a group across pages");
  if (chosenBudget == 0u)
    return;

  // Every group's clusters are placed exactly once across its spans.
  std::vector<uint32_t> placed(build.groupInfos.size(), 0u);
  for (const auto &page : build.pages)
    for (const auto &span : page.groups)
      if (span.globalGroupId < placed.size())
        placed[span.globalGroupId] += span.clusterCount;
  bool exact = true;
  for (size_t groupIndex = 0u; groupIndex < build.groupInfos.size(); ++groupIndex)
    if (placed[groupIndex] != build.groupInfos[groupIndex].originalClusterIndices.size())
      exact = false;
  check(exact, "every group's clusters are placed exactly once across its spans");

  // The page order property the prefix install depends on still holds.
  const auto encoded = vg::VirtualGeometryPacker::encode(build, quantization, nullptr, 1u);
  checkPageOrderIsTopological(encoded, "split build");

  // A split group becomes one leaf per page under a synthetic internal node,
  // so every leaf still names exactly one page.
  uint32_t leavesWithoutPage = 0u;
  for (const auto &node : build.lodLevelHierarchy)
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u && node.pageIndex == UINT32_MAX)
      ++leavesWithoutPage;
  check(leavesWithoutPage == 0u, "every leaf of a split build names a page");

  // Deterministic: same bytes twice, and the same under the front-half fan-out.
  auto again = buildSplit(chosenBudget, 1u);
  const uint64_t once = fingerprintHash(fingerprint(encoded));
  const uint64_t twice = fingerprintHash(
      fingerprint(vg::VirtualGeometryPacker::encode(again, quantization, nullptr, 1u)));
  check(once == twice, "a split build is byte-identical when repeated");
  auto threaded = buildSplit(chosenBudget, 4u);
  const uint64_t parallel = fingerprintHash(
      fingerprint(vg::VirtualGeometryPacker::encode(threaded, quantization, nullptr, 4u)));
  check(once == parallel, "and byte-identical on four threads");
}

void testPageOrderIsTopological()
{
  std::cout << "testPageOrderIsTopological\n";
  checkPageOrderIsTopological(buildGrid(24u), "triangle route");
  vg::VirtualGeometryEncodedData voxelAsset;
  if (buildVoxelFixture(voxelAsset))
    checkPageOrderIsTopological(voxelAsset, "voxel route");
  else
    check(false, "the voxel fixture builds");
}

} // namespace

int main()
{
  std::cout << "VirtualGeometryPackerDeterminismTests\n";
  testRepeatedBuildsAreIdentical();
  testEncodeOrderDoesNotMatter();
  testGoldenFingerprints();
  testParallelBackHalfIsByteIdentical();
  testParallelFrontHalfIsByteIdentical();
  testPageOrderIsTopological();
  testSplitGroupsAreDeterministicAndOrdered();
  if (failures != 0)
  {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
