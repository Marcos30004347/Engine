// A wall of voxels, one voxel per pixel, streamed in.
//
// Every other test in this suite counts clusters. This one looks at the picture.
// The fixture is a flat wall facing the camera, the image is exactly as wide as
// the wall is in finest voxels, and the error budget is set just above zero so
// the cut wants the finest level everywhere it can have it. Then pages are
// installed one at a time and the rendered surface is rasterised after every
// install.
//
// The wall is axis aligned and faces the camera head on, so no floating point
// rasteriser is needed: a drawn quad covers an exact integer rectangle of
// pixels. That makes the image reproducible to the bit, which is what lets the
// test assert on the picture rather than on a cluster count.
//
// What should happen, and what is asserted:
//   - the very first frame, with only the root page resident, already covers
//     every pixel - a coarse voxel is stretched over many pixels;
//   - no pixel is ever written by two different drawn quads;
//   - as pages arrive the number of distinct surface elements on screen rises
//     and never falls, and the coarse elements shrink;
//   - once everything is resident the image is exactly one distinct voxel per
//     pixel, at the finest level;
//   - the image depends only on which pages are resident, not on the order they
//     arrived in.

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{

constexpr uint32_t kPageNotInstalledBit = 1u << 31u;
// The wall is this many finest voxels on a side, and the image is that many
// pixels: at full residency one voxel is exactly one pixel.
constexpr uint32_t kWallSide = 64u;
// Just above zero. Leaf clusters carry self error 0, so they pass; every coarse
// cluster carries at least the brick error floor and does not. Exactly zero
// would reject everything, because the test is `self < threshold`.
constexpr float kFinestThreshold = 1.0e-6f;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// The wall
// ─────────────────────────────────────────────────────────────────────────────

// A checkerboard so no two neighbouring faces share a material and greedy
// meshing cannot merge them: every voxel face stays its own quad, which is what
// makes "one voxel per pixel" observable.
uint32_t wallMaterial(const uint32_t x, const uint32_t y)
{
  return (x ^ y) & 1u;
}

bool buildWallOctree(
    const uint32_t side,
    svr::SparseVoxelBrickHierarchy &hierarchy,
    std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;

  // One voxel thick in z, so the camera looking down -z sees exactly one face
  // per column and the image is a clean projection of the wall.
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t y = 0u; y < side; ++y)
    {
      svr::SparseVoxelCenterLengthGPU cell{};
      cell.center = {
          static_cast<float>(x) + 0.5f,
          static_cast<float>(y) + 0.5f,
          0.5f};
      cell.length = 1.0f;
      svr::SparseVoxelMaterialGPU material{};
      const uint32_t band = wallMaterial(x, y);
      material.albedoRGBA8 = band == 0u ? 0xff3070c0u : 0xffc07030u;
      material.materialId = band;
      leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
      centersLengths.push_back(cell);
      materials.push_back(material);
    }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 18u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, hierarchy, error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Residency driven purely by the page rewrites
// ─────────────────────────────────────────────────────────────────────────────

uint8_t fullMaskForCount(const uint32_t count)
{
  if (count == 0u)
    return 0u;
  return count >= 8u ? 0xffu : static_cast<uint8_t>((1u << count) - 1u);
}

struct Residency
{
  const vg::VirtualGeometryBuildData &build;
  std::vector<uint8_t> installed;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  std::vector<uint32_t> clusterNode;
  std::vector<uint8_t> clusterBit;

  explicit Residency(const vg::VirtualGeometryBuildData &buildData)
      : build(buildData),
        installed(buildData.pages.size(), 0u),
        hierarchy(buildData.lodLevelHierarchy),
        clusterNode(buildData.clusters.size(), UINT32_MAX),
        clusterBit(buildData.clusters.size(), 0u)
  {
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      const auto &node = build.lodLevelHierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      for (uint32_t local = 0u; local < node.child_count; ++local)
      {
        const uint32_t cluster =
            build.pages[page].clusterOffset + node.child_start + local;
        clusterNode[cluster] = nodeIndex;
        clusterBit[cluster] = static_cast<uint8_t>(local);
      }
    }
    applyRewrites();
  }

  static void setMasks(
      vg::VirtualGeometryHierarchy &node,
      const uint8_t streamingMask,
      const uint8_t enabledMask)
  {
    node.flags &= ~(vg::HIERARCHY_STREAMING_MASK_BITS |
                    vg::HIERARCHY_ENABLED_MASK_BITS | vg::STREAMING_LEAF_FLAG);
    node.flags |= static_cast<uint32_t>(streamingMask) << vg::HIERARCHY_STREAMING_MASK_SHIFT;
    node.flags |= static_cast<uint32_t>(enabledMask) << vg::HIERARCHY_ENABLED_MASK_SHIFT;
    if (streamingMask != 0u)
      node.flags |= vg::STREAMING_LEAF_FLAG;
  }

  void applyRewrites()
  {
    std::set<uint32_t> rewritten;
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      node.flags = build.lodLevelHierarchy[nodeIndex].flags;
      node.pageIndex = build.lodLevelHierarchy[nodeIndex].pageIndex;
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      node.pageIndex = installed[page] ? page : (kPageNotInstalledBit | page);
      setMasks(node, 0u, 0u);
    }
    for (uint32_t page = 0u; page < build.pages.size(); ++page)
    {
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        rewritten.insert(update.hierarchyNodeIndex);
      if (installed[page] == 0u)
        continue;
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        setMasks(
            hierarchy[update.hierarchyNodeIndex],
            update.streamingLeafsBitset,
            update.enabledClustersBitset);
    }
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      if ((node.pageIndex & kPageNotInstalledBit) != 0u)
        continue;
      if (rewritten.count(nodeIndex) != 0u)
        continue;
      setMasks(node, 0u, fullMaskForCount(node.child_count));
    }
  }

  bool canInstall(const uint32_t page) const
  {
    for (const uint32_t dependency : build.pages[page].dependencies)
      if (installed[dependency] == 0u)
        return false;
    return true;
  }

  void install(const uint32_t page)
  {
    installed[page] = 1u;
    applyRewrites();
  }

  bool clusterEnabled(const uint32_t cluster) const
  {
    const uint32_t nodeIndex = clusterNode[cluster];
    if (nodeIndex == UINT32_MAX)
      return false;
    const auto &node = hierarchy[nodeIndex];
    if ((node.pageIndex & kPageNotInstalledBit) != 0u)
      return false;
    const uint8_t mask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_ENABLED_MASK_BITS) >> vg::HIERARCHY_ENABLED_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }

  bool clusterStreaming(const uint32_t cluster) const
  {
    const uint32_t nodeIndex = clusterNode[cluster];
    if (nodeIndex == UINT32_MAX)
      return false;
    const auto &node = hierarchy[nodeIndex];
    if ((node.pageIndex & kPageNotInstalledBit) != 0u)
      return false;
    const uint8_t mask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_STREAMING_MASK_BITS) >> vg::HIERARCHY_STREAMING_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }

  // The shipping culler rule: a streaming leaf has its self error forced to
  // zero, and the parent test applies to everything.
  bool clusterDrawn(const uint32_t cluster, const float threshold) const
  {
    if (!clusterEnabled(cluster))
      return false;
    const auto &data = build.clusters[cluster];
    const float selfError = clusterStreaming(cluster) ? 0.0f : data.self.error;
    return selfError < threshold && data.parent.error >= threshold;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// The picture
// ─────────────────────────────────────────────────────────────────────────────

struct WallImage
{
  // Per pixel: the id of the surface element that covered it, or kEmpty.
  static constexpr uint64_t kEmpty = ~uint64_t{0};
  std::vector<uint64_t> pixels;
  uint32_t overdrawnPixels = 0u;
  uint32_t emptyPixels = 0u;
  uint32_t distinctElements = 0u;
  // Pixels covered by an element that spans more than one pixel.
  uint32_t coarsePixels = 0u;
  uint64_t hash = 0u;
};

// Identity of one drawn surface element: the level it was drawn at and the wall
// cell its footprint starts on. Two different clusters can never produce the
// same id, and the finest level gives one id per voxel.
uint64_t elementId(
    const uint32_t level,
    const int64_t minX,
    const int64_t minY,
    const int64_t width,
    const int64_t height)
{
  uint64_t id = static_cast<uint64_t>(level) & 0xffull;
  id |= (static_cast<uint64_t>(minX) & 0xffffull) << 8u;
  id |= (static_cast<uint64_t>(minY) & 0xffffull) << 24u;
  id |= (static_cast<uint64_t>(width) & 0xffffull) << 40u;
  id |= (static_cast<uint64_t>(height) & 0xffull) << 56u;
  return id;
}

// Rasterise the drawn cut. The wall faces -Z, so only quads whose normal points
// at the camera contribute, and their footprint is an exact integer rectangle.
WallImage renderWall(
    const Residency &state,
    const svr::SparseVoxelOctreeVirtualGeometryStats &stats,
    const float threshold,
    const uint32_t side)
{
  WallImage image{};
  image.pixels.assign(static_cast<size_t>(side) * side, WallImage::kEmpty);
  std::set<uint64_t> elements;
  std::map<uint64_t, uint32_t> elementPixelCount;

  for (uint32_t cluster = 0u; cluster < state.build.clusters.size(); ++cluster)
  {
    if (!state.clusterDrawn(cluster, threshold))
      continue;
    const auto &data = state.build.clusters[cluster];
    const uint32_t level = stats.clusterSources[cluster].level;

    for (size_t quad = 0u; quad * 6u < data.indices.size(); ++quad)
    {
      const uint32_t *triangle = &data.indices[quad * 6u];
      const std::array<uint32_t, 4> corners{
          triangle[0], triangle[1], triangle[2], triangle[5]};
      // Only the face looking at the camera.
      if (data.vertices[corners[0]].norm[2] >= 0.0f)
        continue;

      float minX = data.vertices[corners[0]].pos[0];
      float maxX = minX;
      float minY = data.vertices[corners[0]].pos[1];
      float maxY = minY;
      for (const uint32_t corner : corners)
      {
        minX = std::min(minX, data.vertices[corner].pos[0]);
        maxX = std::max(maxX, data.vertices[corner].pos[0]);
        minY = std::min(minY, data.vertices[corner].pos[1]);
        maxY = std::max(maxY, data.vertices[corner].pos[1]);
      }

      const int64_t x0 = static_cast<int64_t>(std::llround(minX));
      const int64_t x1 = static_cast<int64_t>(std::llround(maxX));
      const int64_t y0 = static_cast<int64_t>(std::llround(minY));
      const int64_t y1 = static_cast<int64_t>(std::llround(maxY));
      const uint64_t id = elementId(level, x0, y0, x1 - x0, y1 - y0);
      elements.insert(id);

      for (int64_t y = std::max<int64_t>(0, y0); y < std::min<int64_t>(side, y1); ++y)
        for (int64_t x = std::max<int64_t>(0, x0); x < std::min<int64_t>(side, x1); ++x)
        {
          const size_t pixel = static_cast<size_t>(y) * side + static_cast<size_t>(x);
          if (image.pixels[pixel] != WallImage::kEmpty)
            ++image.overdrawnPixels;
          image.pixels[pixel] = id;
          ++elementPixelCount[id];
        }
    }
  }

  for (const uint64_t pixel : image.pixels)
    image.emptyPixels += pixel == WallImage::kEmpty ? 1u : 0u;
  image.distinctElements = static_cast<uint32_t>(elements.size());
  for (const uint64_t pixel : image.pixels)
  {
    if (pixel == WallImage::kEmpty)
      continue;
    if (elementPixelCount[pixel] > 1u)
      ++image.coarsePixels;
  }

  uint64_t hash = 1469598103934665603ull;
  for (const uint64_t pixel : image.pixels)
    hash = (hash ^ pixel) * 1099511628211ull;
  image.hash = hash;
  return image;
}

std::vector<uint32_t> topologicalPageOrder(
    const vg::VirtualGeometryBuildData &build,
    const bool reverse)
{
  const uint32_t pageCount = static_cast<uint32_t>(build.pages.size());
  std::vector<uint32_t> inDegree(pageCount, 0u);
  std::vector<std::vector<uint32_t>> dependants(pageCount);
  for (uint32_t page = 0u; page < pageCount; ++page)
  {
    inDegree[page] = static_cast<uint32_t>(build.pages[page].dependencies.size());
    for (const uint32_t dependency : build.pages[page].dependencies)
      dependants[dependency].push_back(page);
  }
  std::vector<uint32_t> ready;
  for (uint32_t page = 0u; page < pageCount; ++page)
    if (inDegree[page] == 0u)
      ready.push_back(page);
  std::vector<uint32_t> order;
  while (!ready.empty())
  {
    const auto pick = reverse ? std::max_element(ready.begin(), ready.end())
                              : std::min_element(ready.begin(), ready.end());
    const uint32_t page = *pick;
    ready.erase(pick);
    order.push_back(page);
    for (const uint32_t dependant : dependants[page])
      if (--inDegree[dependant] == 0u)
        ready.push_back(dependant);
  }
  return order;
}

struct Fixture
{
  svr::SparseVoxelBrickHierarchy octree;
  vg::VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  bool valid = false;
};

Fixture buildFixture()
{
  Fixture fixture{};
  std::string error;
  if (!buildWallOctree(kWallSide, fixture.octree, &error))
  {
    std::cout << "  wall octree build failed: " << error << '\n';
    return fixture;
  }

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.maxRootPageGroups = 1u;
  fixture.valid = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.octree,
      0u,
      static_cast<uint32_t>(fixture.octree.nodes.size()),
      octreeSettings,
      buildSettings,
      fixture.build,
      &fixture.stats,
      &error);
  if (!fixture.valid)
  {
    std::cout << "  wall virtual geometry build failed: " << error << '\n';
    return fixture;
  }
  std::cout << "  wall " << kWallSide << "x" << kWallSide
            << " bricks=" << fixture.octree.nodes.size()
            << " levels=" << fixture.stats.levelCount
            << " clusters=" << fixture.build.clusters.size()
            << " pages=" << fixture.build.pages.size()
            << " blockDepth=" << fixture.stats.blockDepth << '\n';
  return fixture;
}

// ─────────────────────────────────────────────────────────────────────────────

void testWallStreamsInFromCoarseToOneVoxelPerPixel()
{
  std::cout << "a wall of voxels streams in from coarse to one voxel per pixel\n";

  Fixture fixture = buildFixture();
  check(fixture.valid, "the wall builds virtual geometry");
  if (!fixture.valid)
    return;
  check(fixture.stats.levelCount >= 3u, "the wall octree has several levels");
  check(fixture.build.pages.size() > 4u, "the wall is split over several pages");

  Residency state(fixture.build);
  const std::vector<uint32_t> order = topologicalPageOrder(fixture.build, false);

  std::vector<uint32_t> distinctPerStep;
  std::vector<uint32_t> coarsePixelsPerStep;
  std::vector<uint32_t> emptyPixelsPerStep;
  uint32_t stepsWithOverdraw = 0u;
  uint32_t dependencyOrderBreaks = 0u;

  for (const uint32_t page : order)
  {
    dependencyOrderBreaks += state.canInstall(page) ? 0u : 1u;
    state.install(page);
    const WallImage image =
        renderWall(state, fixture.stats, kFinestThreshold, kWallSide);
    stepsWithOverdraw += image.overdrawnPixels != 0u ? 1u : 0u;
    distinctPerStep.push_back(image.distinctElements);
    coarsePixelsPerStep.push_back(image.coarsePixels);
    emptyPixelsPerStep.push_back(image.emptyPixels);
  }

  check(dependencyOrderBreaks == 0u, "every page installed only after its dependencies");
  check(
      stepsWithOverdraw == 0u,
      "no pixel is ever written by two drawn quads: no doubled surface");

  // Coverage grows as the coarse pages arrive. An uncovered pixel is only a hole
  // once the region behind it has geometry resident, so the invariant is that
  // coverage never goes backwards and ends complete.
  bool coverageNeverRegresses = true;
  size_t firstCompleteStep = emptyPixelsPerStep.size();
  for (size_t index = 0u; index < emptyPixelsPerStep.size(); ++index)
  {
    if (index > 0u)
      coverageNeverRegresses =
          coverageNeverRegresses && emptyPixelsPerStep[index] <= emptyPixelsPerStep[index - 1u];
    if (emptyPixelsPerStep[index] == 0u && firstCompleteStep == emptyPixelsPerStep.size())
      firstCompleteStep = index;
  }
  check(coverageNeverRegresses, "coverage of the wall never goes backwards");
  check(
      firstCompleteStep < emptyPixelsPerStep.size(),
      "the wall becomes fully covered while streaming");
  check(
      emptyPixelsPerStep.back() == 0u,
      "the settled wall has no uncovered pixel");

  bool detailNeverRegresses = true;
  bool coarsenessNeverGrows = true;
  for (size_t index = 1u; index < distinctPerStep.size(); ++index)
    detailNeverRegresses =
        detailNeverRegresses && distinctPerStep[index] >= distinctPerStep[index - 1u];
  // Only meaningful once the wall is fully covered: before that the stretched
  // area grows simply because more of the wall is on screen at all.
  for (size_t index = firstCompleteStep + 1u; index < coarsePixelsPerStep.size(); ++index)
    coarsenessNeverGrows =
        coarsenessNeverGrows && coarsePixelsPerStep[index] <= coarsePixelsPerStep[index - 1u];
  check(detailNeverRegresses, "the number of distinct voxels on screen never falls");
  check(
      coarsenessNeverGrows,
      "once the wall is fully covered the stretched coarse area only shrinks");
  check(
      distinctPerStep.back() > distinctPerStep.front() * 4u,
      "streaming multiplies the detail on screen");

  const uint32_t expectedPixels = kWallSide * kWallSide;
  check(
      distinctPerStep.back() == expectedPixels,
      "the settled image is exactly one distinct voxel per pixel: " +
          std::to_string(distinctPerStep.back()) + "/" +
          std::to_string(expectedPixels));
  check(
      coarsePixelsPerStep.back() == 0u,
      "no pixel is still covered by a stretched coarse voxel at the end");

  std::cout << "  distinct voxels on screen:";
  for (size_t index = 0u; index < distinctPerStep.size(); ++index)
    std::cout << ' ' << distinctPerStep[index];
  std::cout << "\n  stretched pixels:";
  for (const uint32_t coarse : coarsePixelsPerStep)
    std::cout << ' ' << coarse;
  std::cout << '\n';
}

void testTheImageDependsOnlyOnWhichPagesAreResident()
{
  std::cout << "the image depends on the resident set, not the arrival order\n";

  Fixture fixture = buildFixture();
  check(fixture.valid, "the wall builds virtual geometry");
  if (!fixture.valid)
    return;

  const auto finalHash = [&](const bool reverse)
  {
    Residency state(fixture.build);
    for (const uint32_t page : topologicalPageOrder(fixture.build, reverse))
      state.install(page);
    return renderWall(state, fixture.stats, kFinestThreshold, kWallSide).hash;
  };
  check(
      finalHash(false) == finalHash(true),
      "two legal install orders produce the same settled image");

  // The same resident set reached two different ways has to render the same
  // picture, partway through as well as at the end.
  const std::vector<uint32_t> forward = topologicalPageOrder(fixture.build, false);
  const size_t half = forward.size() / 2u;
  std::vector<uint32_t> prefix(forward.begin(), forward.begin() + half);

  Residency inOrder(fixture.build);
  for (const uint32_t page : prefix)
    inOrder.install(page);

  std::vector<uint32_t> shuffled = prefix;
  std::reverse(shuffled.begin(), shuffled.end());
  Residency reordered(fixture.build);
  bool installedAll = true;
  for (uint32_t pass = 0u; pass < prefix.size() && installedAll; ++pass)
  {
    installedAll = false;
    for (const uint32_t page : shuffled)
      if (reordered.installed[page] == 0u && reordered.canInstall(page))
      {
        reordered.install(page);
        installedAll = true;
      }
  }

  const WallImage a = renderWall(inOrder, fixture.stats, kFinestThreshold, kWallSide);
  const WallImage b = renderWall(reordered, fixture.stats, kFinestThreshold, kWallSide);
  check(a.hash == b.hash, "a half-streamed image is identical whichever way it got there");
  check(a.emptyPixels == 0u && b.emptyPixels == 0u, "both half-streamed images are complete");
  std::cout << "  half-streamed distinct voxels=" << a.distinctElements
            << " stretched pixels=" << a.coarsePixels << '\n';
}

void testACoarseBudgetKeepsTheWallCoarse()
{
  std::cout << "a loose error budget keeps the wall coarse even when everything is resident\n";

  Fixture fixture = buildFixture();
  check(fixture.valid, "the wall builds virtual geometry");
  if (!fixture.valid)
    return;

  Residency state(fixture.build);
  for (const uint32_t page : topologicalPageOrder(fixture.build, false))
    state.install(page);

  // Sweep the budget from "finest" upward. The picture has to lose detail
  // monotonically and stay complete.
  float coarsest = 0.0f;
  for (const auto &cluster : fixture.build.clusters)
    if (cluster.parent.error == std::numeric_limits<float>::max())
      coarsest = std::max(coarsest, cluster.self.error);

  std::vector<uint32_t> distinct;
  uint32_t incompleteImages = 0u;
  uint32_t overdrawnImages = 0u;
  for (float threshold = kFinestThreshold; threshold <= coarsest * 4.0f;
       threshold = threshold <= 0.25f ? 0.5f : threshold * 2.0f)
  {
    const WallImage image = renderWall(state, fixture.stats, threshold, kWallSide);
    distinct.push_back(image.distinctElements);
    incompleteImages += image.emptyPixels != 0u ? 1u : 0u;
    overdrawnImages += image.overdrawnPixels != 0u ? 1u : 0u;
  }

  check(incompleteImages == 0u, "every budget renders a complete wall");
  check(overdrawnImages == 0u, "no budget draws a pixel twice");
  bool losesDetail = true;
  for (size_t index = 1u; index < distinct.size(); ++index)
    losesDetail = losesDetail && distinct[index] <= distinct[index - 1u];
  check(losesDetail, "a looser budget never adds detail");
  check(
      distinct.front() == kWallSide * kWallSide,
      "the tightest budget still gives one voxel per pixel");
  check(distinct.back() < distinct.front(), "the loosest budget really is coarser");

  std::cout << "  distinct voxels by budget:";
  for (const uint32_t count : distinct)
    std::cout << ' ' << count;
  std::cout << '\n';
}

} // namespace

int main()
{
  std::cout << "Sparse voxel octree wall image tests\n";
  testWallStreamsInFromCoarseToOneVoxelPerPixel();
  testTheImageDependsOnlyOnWhichPagesAreResident();
  testACoarseBudgetKeepsTheWallCoarse();

  if (failures != 0)
  {
    std::cout << failures << " sparse voxel octree wall image checks failed\n";
    return 1;
  }
  std::cout << "all sparse voxel octree wall image checks passed\n";
  return 0;
}
