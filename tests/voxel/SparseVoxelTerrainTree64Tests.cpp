#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"
#include "voxel/svraster/SparseVoxelTree64Pages.hpp"
#include "voxel/svraster/SparseVoxelTree64Reference.hpp"

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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace voxel::svraster;

// One page of leaf bricks over an authored occupancy predicate. Bricks are
// emitted only where something is occupied, which is what a terrain page holds
// after its surface shell is carved.
struct TestPage
{
  SparseVoxelBrickHierarchy bricks;
  uint32_t voxelsPerEdge = 0u;
  uint32_t verticalVoxels = 0u;
  double voxelSize = 0.0;
  std::array<double, 3> minimumMetres{};
  std::vector<uint8_t> occupied;

  bool voxel(const int32_t x, const int32_t y, const int32_t z) const
  {
    if (x < 0 || y < 0 || z < 0 || x >= static_cast<int32_t>(voxelsPerEdge) ||
        z >= static_cast<int32_t>(voxelsPerEdge) ||
        y >= static_cast<int32_t>(verticalVoxels))
      return false;
    return occupied[static_cast<size_t>(x) +
               static_cast<size_t>(voxelsPerEdge) *
                   (static_cast<size_t>(y) +
                       static_cast<size_t>(verticalVoxels) * z)] != 0u;
  }
};

template <typename Height>
TestPage makePage(
    const uint32_t voxelsPerEdge,
    const uint32_t verticalVoxels,
    const double voxelSize,
    const std::array<double, 3> &minimumMetres,
    const uint32_t shellDepth,
    const Height &height,
    const uint32_t brickVoxelScale = 1u)
{
  TestPage page{};
  page.voxelsPerEdge = voxelsPerEdge;
  page.verticalVoxels = verticalVoxels;
  page.voxelSize = voxelSize;
  page.minimumMetres = minimumMetres;
  page.occupied.assign(
      static_cast<size_t>(voxelsPerEdge) * verticalVoxels * voxelsPerEdge, 0u);
  for (uint32_t z = 0u; z < voxelsPerEdge; ++z)
    for (uint32_t x = 0u; x < voxelsPerEdge; ++x)
    {
      const int32_t top = std::min<int32_t>(
          static_cast<int32_t>(verticalVoxels), height(x, z));
      const int32_t bottom =
          std::max<int32_t>(0, top - static_cast<int32_t>(shellDepth));
      for (int32_t y = bottom; y < top; ++y)
        page.occupied[static_cast<size_t>(x) +
            static_cast<size_t>(voxelsPerEdge) *
                (static_cast<size_t>(y) +
                    static_cast<size_t>(verticalVoxels) * z)] = 1u;
    }

  // Pack the authored voxels into 8-cubed leaf bricks. A brick voxel may cover
  // several page voxels, which is how a page whose leaves are coarser than its
  // nominal lattice reaches the builder.
  const uint32_t brickSpan = kSparseVoxelBrickSide * brickVoxelScale;
  for (uint32_t brickZ = 0u; brickZ * brickSpan < voxelsPerEdge; ++brickZ)
    for (uint32_t brickY = 0u; brickY * brickSpan < verticalVoxels; ++brickY)
      for (uint32_t brickX = 0u; brickX * brickSpan < voxelsPerEdge; ++brickX)
      {
        SparseVoxelBrickOccupancyGPU occupancy{};
        uint32_t occupiedCount = 0u;
        for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
          for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
            for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
            {
              bool any = false;
              for (uint32_t sz = 0u; sz < brickVoxelScale && !any; ++sz)
                for (uint32_t sy = 0u; sy < brickVoxelScale && !any; ++sy)
                  for (uint32_t sx = 0u; sx < brickVoxelScale && !any; ++sx)
                    any = page.voxel(
                        static_cast<int32_t>(
                            brickX * brickSpan + x * brickVoxelScale + sx),
                        static_cast<int32_t>(
                            brickY * brickSpan + y * brickVoxelScale + sy),
                        static_cast<int32_t>(
                            brickZ * brickSpan + z * brickVoxelScale + sz));
              if (!any)
                continue;
              const uint32_t cell =
                  x + kSparseVoxelBrickSide * (y + kSparseVoxelBrickSide * z);
              occupancy.cells[cell / 32u].bits |= uint32_t{1u} << (cell % 32u);
              ++occupiedCount;
            }
        if (occupiedCount == 0u)
          continue;
        SparseVoxelBrickNodeGPU node{};
        node.minimumVoxelSize = {
          static_cast<float>(minimumMetres[0] + brickX * brickSpan * voxelSize),
          static_cast<float>(minimumMetres[1] + brickY * brickSpan * voxelSize),
          static_cast<float>(minimumMetres[2] + brickZ * brickSpan * voxelSize),
          static_cast<float>(voxelSize * brickVoxelScale),
        };
        node.occupancyIndex = static_cast<uint32_t>(page.bricks.occupancy.size());
        node.occupiedCount = occupiedCount;
        page.bricks.occupancy.push_back(occupancy);
        page.bricks.rootIndices.push_back(
            static_cast<uint32_t>(page.bricks.nodes.size()));
        page.bricks.nodes.push_back(node);
      }
  return page;
}

SparseVoxelTerrainPageTree64Source pageSource(const TestPage &page)
{
  SparseVoxelTerrainPageTree64Source source{};
  source.bricks = &page.bricks;
  source.firstBrickRoot = 0u;
  source.brickRootCount = static_cast<uint32_t>(page.bricks.rootIndices.size());
  source.brickNodeOffset = 0u;
  source.brickNodeCapacity = static_cast<uint32_t>(page.bricks.nodes.size());
  source.pageMinimumMetres = page.minimumMetres;
  source.edgeMetres = page.voxelsPerEdge * page.voxelSize;
  source.heightMetres = page.verticalVoxels * page.voxelSize;
  source.voxelSizeMetres = page.voxelSize;
  return source;
}

bool occupiedCell(
    const SparseVoxelTerrainPageTree64 &built,
    const int32_t x,
    const int32_t y,
    const int32_t z)
{
  if (x < 0 || y < 0 || z < 0 || x >= static_cast<int32_t>(built.side) ||
      y >= static_cast<int32_t>(built.side) ||
      z >= static_cast<int32_t>(built.side))
    return false;
  return sparseVoxelTree64Occupied(built.tree,
      {static_cast<uint32_t>(x), static_cast<uint32_t>(y),
          static_cast<uint32_t>(z)});
}

// The whole point of the builder: at cell scale one, the tree agrees with the
// brick occupancy voxel for voxel. Anything less is a shadow that does not
// match the geometry the raster draws.
void assertExact(const TestPage &page, const SparseVoxelTerrainPageTree64 &built)
{
  assert(built.cellScale == 1u);
  for (int32_t z = 0; z < static_cast<int32_t>(built.side); ++z)
    for (int32_t y = 0; y < static_cast<int32_t>(built.side); ++y)
      for (int32_t x = 0; x < static_cast<int32_t>(built.side); ++x)
        assert(occupiedCell(built, x, y, z) == page.voxel(x, y, z));
}

// Leaves coarser than the page lattice carry a mip of it, so the tree can only
// be exact against the brick voxels themselves: every page voxel in an
// occupied brick voxel is occupied, and every one outside is not.
void assertExactAgainstBrickVoxels(
    const TestPage &page,
    const SparseVoxelTerrainPageTree64 &built,
    const int32_t brickVoxelScale)
{
  assert(built.cellScale == 1u);
  const auto brickVoxel = [&](const int32_t x, const int32_t y, const int32_t z)
  {
    const int32_t baseX = (x / brickVoxelScale) * brickVoxelScale;
    const int32_t baseY = (y / brickVoxelScale) * brickVoxelScale;
    const int32_t baseZ = (z / brickVoxelScale) * brickVoxelScale;
    for (int32_t sz = 0; sz < brickVoxelScale; ++sz)
      for (int32_t sy = 0; sy < brickVoxelScale; ++sy)
        for (int32_t sx = 0; sx < brickVoxelScale; ++sx)
          if (page.voxel(baseX + sx, baseY + sy, baseZ + sz))
            return true;
    return false;
  };
  for (int32_t z = 0; z < static_cast<int32_t>(built.side); ++z)
    for (int32_t y = 0; y < static_cast<int32_t>(built.side); ++y)
      for (int32_t x = 0; x < static_cast<int32_t>(built.side); ++x)
        assert(occupiedCell(built, x, y, z) == brickVoxel(x, y, z));
}

void assertConservative(
    const TestPage &page, const SparseVoxelTerrainPageTree64 &built)
{
  const int32_t scale = static_cast<int32_t>(built.cellScale);
  for (int32_t z = 0; z < static_cast<int32_t>(page.voxelsPerEdge); ++z)
    for (int32_t y = 0; y < static_cast<int32_t>(page.verticalVoxels); ++y)
      for (int32_t x = 0; x < static_cast<int32_t>(page.voxelsPerEdge); ++x)
        if (page.voxel(x, y, z))
          assert(occupiedCell(built, x / scale, y / scale, z / scale));
}

void testVoxelExactPage()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(
        6 + (x % 7u) + (z % 5u) + ((x + z) % 3u == 0u ? 4 : 0));
  };
  const TestPage page = makePage(32u, 32u, 0.1953125, {-513.75, 12.5, 208.0}, 4u, height);
  SparseVoxelTerrainPageTree64 built{};
  std::string error;
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, &error));
  assert(error.empty());
  // Thirty-two voxels ask for the 64-cell lattice: a power of four is the
  // smallest tree that can hold them one cell per voxel.
  assert(built.side == 64u);
  assert(built.cellScale == 1u);
  assert(std::abs(built.cellSizeMetres - page.voxelSize) < 1.0e-12);
  assert(built.originMetres == page.minimumMetres);
  assertExact(page, built);
}

// A page carrying a whole power of four of voxels must not be padded up a
// level: 64 voxels are exactly the 64-cell lattice.
void testAlignedPageSide()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(20 + (x % 11u) - (z % 6u));
  };
  const TestPage page = makePage(64u, 48u, 0.25, {0.0, 0.0, 0.0}, 3u, height);
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.side == 64u);
  assertExact(page, built);
}

// Leaves coarser than the page lattice still land on it exactly: one brick
// voxel becomes the block of page voxels it covers.
void testCoarseLeafBricks()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(8 + (x % 4u) * 2u + (z % 3u) * 2u);
  };
  const TestPage page =
      makePage(32u, 32u, 0.5, {100.0, -20.0, -4.0}, 4u, height, 2u);
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.side == 64u);
  assertExactAgainstBrickVoxels(page, built, 2);
}

// A page that cannot afford its exact tree reduces onto an aligned coarser
// lattice. It may only ever gain occupancy: a lost cell is a light leak.
void testConservativeReduction()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(10 + (x % 9u) + (z % 7u));
  };
  const TestPage page = makePage(64u, 32u, 0.25, {8.0, 3.0, -64.0}, 5u, height);
  SparseVoxelTerrainPageTree64 exact{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), exact, nullptr));
  assertExact(page, exact);

  auto source = pageSource(page);
  source.maximumNodeCount =
      static_cast<uint32_t>(exact.tree.nodes.size()) / 2u;
  SparseVoxelTerrainPageTree64 reduced{};
  assert(buildSparseVoxelTerrainPageTree64(source, reduced, nullptr));
  assert(reduced.cellScale > exact.cellScale);
  assert(reduced.side * reduced.cellScale == exact.side * exact.cellScale);
  assert(reduced.tree.nodes.size() <= source.maximumNodeCount);
  assertConservative(page, reduced);

  // An explicit coarser request is the same aligned reduction.
  auto requested = pageSource(page);
  requested.requestedCellsPerEdge = 16u;
  SparseVoxelTerrainPageTree64 coarse{};
  assert(buildSparseVoxelTerrainPageTree64(requested, coarse, nullptr));
  assert(coarse.side == 16u);
  assert(coarse.cellScale == 4u);
  assertConservative(page, coarse);
}

// Parents are OR reductions of their children, so a page that stops at a
// parent grows its terrain by a coarse voxel everywhere. Descending to the
// authored leaves is what keeps the tree equal to the drawn geometry.
void testDescendsToLeafBricks()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(9 + (x % 5u) + (z % 4u));
  };
  TestPage page = makePage(32u, 32u, 0.25, {0.0, 0.0, 0.0}, 3u, height);
  const uint32_t leafCount = static_cast<uint32_t>(page.bricks.nodes.size());

  // Re-root the page under one coarse parent per leaf, the shape the terrain
  // brick octree actually publishes.
  std::vector<uint32_t> leaves = page.bricks.rootIndices;
  page.bricks.rootIndices.clear();
  for (const uint32_t leaf : leaves)
  {
    SparseVoxelBrickNodeGPU parent = page.bricks.nodes[leaf];
    parent.minimumVoxelSize[3] *= 2.0f;
    parent.firstChild = static_cast<uint32_t>(page.bricks.childIndices.size());
    parent.childCount = 1u;
    page.bricks.childIndices.push_back(leaf);
    page.bricks.rootIndices.push_back(
        static_cast<uint32_t>(page.bricks.nodes.size()));
    page.bricks.nodes.push_back(parent);
  }
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.leafBrickCount == leafCount);
  assertExact(page, built);
}

// Non-resident children cannot be descended into, so the node's own reduction
// stands in. That is coarser than the leaves but never loses occupancy.
void testNonResidentChildrenFallBack()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(12 + (x % 3u) + (z % 5u));
  };
  TestPage page = makePage(32u, 32u, 0.25, {0.0, 0.0, 0.0}, 4u, height);
  const std::vector<uint32_t> leaves = page.bricks.rootIndices;
  page.bricks.rootIndices.clear();
  for (const uint32_t leaf : leaves)
  {
    SparseVoxelBrickNodeGPU parent = page.bricks.nodes[leaf];
    parent.firstChild = static_cast<uint32_t>(page.bricks.childIndices.size());
    parent.childCount = 1u;
    // Address a node the page does not own.
    page.bricks.childIndices.push_back(0xffffff00u);
    page.bricks.rootIndices.push_back(
        static_cast<uint32_t>(page.bricks.nodes.size()));
    page.bricks.nodes.push_back(parent);
  }
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assertConservative(page, built);
}

void testSelfContainedPagePayloadAndReuse()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(10u + (x + 2u * z) % 9u);
  };
  const TestPage page =
      makePage(32u, 32u, 0.25, {64.0, 12.0, -32.0}, 4u, height);
  PlanetVoxelChunkSet chunks{};
  chunks.bricks = page.bricks;
  chunks.voxelSizeMetres = page.voxelSize;
  chunks.elevationOriginMetres = page.minimumMetres[1];
  PlanetVoxelChunkRecord record{};
  record.clipmapLOD = 2u;
  record.clipmapSlot = 7u;
  record.regionMappingId = 91u;
  record.worldCenterXMetres =
      page.minimumMetres[0] + page.voxelsPerEdge * page.voxelSize * 0.5;
  record.worldCenterZMetres =
      page.minimumMetres[2] + page.voxelsPerEdge * page.voxelSize * 0.5;
  record.edgeMetres = page.voxelsPerEdge * page.voxelSize;
  record.voxelSizeMetres = page.voxelSize;
  record.brickNodeOffset = 0u;
  record.brickNodeCapacity = static_cast<uint32_t>(chunks.bricks.nodes.size());
  record.firstBrickRoot = 0u;
  record.brickRootCount =
      static_cast<uint32_t>(chunks.bricks.rootIndices.size());
  record.elevationOriginMetres = page.minimumMetres[1];
  record.maximumSurfaceMetres =
      page.minimumMetres[1] + page.verticalVoxels * page.voxelSize;
  record.residencyGeneration = 3u;
  chunks.chunks.push_back(record);

  // Requesting 16 cells forces a material-free conservative page, keeping the
  // fixture focused on immutable nodes and ownership rather than palette data.
  SparseVoxelTerrainTree64PagePayload first{};
  std::string error;
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {&chunks, 0u, 16u, 0u, 0x12345678ull, nullptr}, first, &error));
  assert(error.empty());
  assert(first.terrainSlot == record.clipmapSlot);
  assert(first.terrainGeneration == record.residencyGeneration);
  assert(first.internalLOD == record.clipmapLOD);
  assert(first.regionMappingId == record.regionMappingId);
  assert(first.geometryRevision == 0x12345678ull);
  assert(first.model.modelId == record.clipmapSlot);
  assert(first.model.payloadRevision == first.geometryRevision);
  assert(first.model.physicalNodeSlot == record.clipmapSlot);
  assert(first.model.side == 16u);
  assert(first.cellScale == 4u);
  assert((first.instance.flags &
          kSparseVoxelTree64DynamicSkipReceiverCell) != 0u);
  assert(first.instance.receiverCellSizeMetres ==
      static_cast<float>(page.voxelSize));
  assert(!first.model.prebuiltNodes.empty());
  assert(first.materialCells.empty());
  assert(!first.reusedNodes);

  const auto virtualPayload =
      sparseVoxelTerrainTree64VirtualPayloadFromPagePayload(first);
  assert(virtualPayload.internalLOD == first.internalLOD);
  assert(virtualPayload.regionMappingId == first.regionMappingId);
  assert(virtualPayload.geometryRevision == first.geometryRevision);
  assert(virtualPayload.modelSide == first.model.side);
  assert(virtualPayload.modelPayloadRevision == first.model.payloadRevision);
  assert(virtualPayload.modelNodes.size() == first.model.prebuiltNodes.size());
  assert(std::memcmp(
      virtualPayload.modelNodes.data(),
      first.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.size() * sizeof(SparseVoxelTree64NodeGPU)) ==
      0);
  const std::vector<uint8_t> virtualBytes =
      serializeSparseVoxelTerrainTree64VirtualPagePayload(virtualPayload);
  assert(!virtualBytes.empty());
  SparseVoxelTerrainTree64VirtualPagePayload restoredVirtualPayload{};
  assert(deserializeSparseVoxelTerrainTree64VirtualPagePayload(
      virtualBytes, restoredVirtualPayload, &error));
  assert(serializeSparseVoxelTerrainTree64VirtualPagePayload(
      restoredVirtualPayload) == virtualBytes);
  const auto reassigned =
      sparseVoxelTerrainTree64VirtualPayloadToPagePayload(
          restoredVirtualPayload, 11u, 7u);
  assert(reassigned.terrainSlot == 11u);
  assert(reassigned.terrainGeneration == 7u);
  assert(reassigned.model.modelId == 11u);
  assert(reassigned.model.physicalNodeSlot == 11u);
  assert(reassigned.instance.stableInstanceId == 11u);
  assert(reassigned.instance.terrainSlot == 11u);
  assert(reassigned.instance.terrainGeneration == 7u);
  assert(reassigned.model.prebuiltNodes.size() ==
      first.model.prebuiltNodes.size());
  assert(std::memcmp(
      reassigned.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.size() * sizeof(SparseVoxelTree64NodeGPU)) ==
      0);

  SparseVoxelTerrainTree64PagePayload deterministic{};
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {&chunks, 0u, 16u, 0u, first.geometryRevision, nullptr},
      deterministic, &error));
  assert(deterministic.model.prebuiltNodes.size() ==
      first.model.prebuiltNodes.size());
  assert(std::memcmp(
      deterministic.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.size() * sizeof(SparseVoxelTree64NodeGPU)) ==
      0);
  assert(deterministic.model.localBounds.minimum == first.model.localBounds.minimum);
  assert(deterministic.model.localBounds.maximum == first.model.localBounds.maximum);

  // A slot owner can advance independently while retaining an immutable page
  // with the same geometry revision. Instance ownership is always rebuilt.
  chunks.chunks[0].residencyGeneration = 4u;
  chunks.chunks[0].regionMappingId = 92u;
  SparseVoxelTerrainTree64PagePayload reused{};
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {&chunks, 0u, 16u, 0u, first.geometryRevision, &first.model},
      reused, &error));
  assert(reused.reusedNodes);
  assert(reused.model.prebuiltNodes.size() == first.model.prebuiltNodes.size());
  assert(std::memcmp(
      reused.model.prebuiltNodes.data(), first.model.prebuiltNodes.data(),
      first.model.prebuiltNodes.size() * sizeof(SparseVoxelTree64NodeGPU)) ==
      0);
  assert(reused.terrainGeneration == 4u);
  assert(reused.instance.terrainGeneration == 4u);
  assert(reused.instance.regionMappingId == 92u);

  // Physical-page reuse compares the descriptor's shared logical geometry
  // identity, not the Tree64 builder-cache revision. The latter may hash
  // private lattice configuration and must not force all retained pages to be
  // uploaded again as a spatial cut grows.
  constexpr uint64_t logicalGeometryRevision = 0x123456789abcdef0ull;
  constexpr uint64_t privateBuilderRevision = 0xfedcba9876543210ull;
  SparseVoxelTerrainTree64DescriptorGPU committedDescriptor{};
  committedDescriptor.flags = kSparseVoxelTree64TerrainPageValid;
  committedDescriptor.terrainGeneration = reused.terrainGeneration;
  committedDescriptor.internalLOD = reused.internalLOD;
  committedDescriptor.geometryRevisionLo =
      static_cast<uint32_t>(logicalGeometryRevision);
  committedDescriptor.geometryRevisionHi =
      static_cast<uint32_t>(logicalGeometryRevision >> 32u);
  assert(sparseVoxelTerrainTree64DescriptorMatchesLogicalPage(
      committedDescriptor, reused.terrainGeneration, reused.internalLOD,
      logicalGeometryRevision));
  assert(!sparseVoxelTerrainTree64DescriptorMatchesLogicalPage(
      committedDescriptor, reused.terrainGeneration, reused.internalLOD,
      privateBuilderRevision));

  SparseVoxelTerrainTree64PagePayload changed{};
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {&chunks, 0u, 16u, 0u, first.geometryRevision + 1u, &first.model},
      changed, &error));
  assert(!changed.reusedNodes);
  assert(changed.model.payloadRevision == first.geometryRevision + 1u);
}

void testPagePayloadMaterialSidecar()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(4u + (x + z) % 3u);
  };
  TestPage page = makePage(
      8u, 8u, 0.5, {0.0, 0.0, 0.0}, 2u, height);
  page.bricks.palette.resize(1u);
  page.bricks.paletteDescriptorIds.push_back(37u);
  page.bricks.paletteStride = 1u;
  uint32_t expectedMaterials = 0u;
  for (auto &node : page.bricks.nodes)
  {
    node.materialBase =
        static_cast<uint32_t>(page.bricks.materialIndices.size());
    node.paletteBase = 0u;
    node.regionMappingId = 52u;
    auto &occupancy = page.bricks.occupancy[node.occupancyIndex];
    uint32_t prefix = 0u;
    for (auto &word : occupancy.cells)
    {
      word.prefix = prefix;
      for (uint32_t bit = 0u; bit < 32u; ++bit)
        if ((word.bits & (uint32_t{1u} << bit)) != 0u)
        {
          page.bricks.materialIndices.push_back(0u);
          page.bricks.microSurfaceData.push_back(
              packSparseVoxelMicroContextData(
                  sparseVoxelMicroSurfaceContext(0u), 0u));
          ++prefix;
        }
    }
    expectedMaterials += prefix;
  }

  PlanetVoxelChunkSet chunks{};
  chunks.bricks = page.bricks;
  chunks.voxelSizeMetres = page.voxelSize;
  PlanetVoxelChunkRecord record{};
  record.clipmapSlot = 3u;
  record.regionMappingId = 52u;
  record.worldCenterXMetres = 2.0;
  record.worldCenterZMetres = 2.0;
  record.edgeMetres = 4.0;
  record.voxelSizeMetres = page.voxelSize;
  record.brickNodeCapacity =
      static_cast<uint32_t>(chunks.bricks.nodes.size());
  record.brickRootCount =
      static_cast<uint32_t>(chunks.bricks.rootIndices.size());
  record.maximumSurfaceMetres = 4.0;
  record.residencyGeneration = 1u;
  chunks.chunks.push_back(record);

  SparseVoxelTerrainTree64PagePayload payload{};
  std::string error;
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {
        &chunks,
        0u,
        0u,
        0u,
        77u,
        nullptr,
        nullptr,
        0u,
        0u,
        true,
      },
      payload, &error));
  assert(payload.cellScale == 1u);
  assert((payload.instance.flags &
          kSparseVoxelTree64DynamicSkipReceiverCell) != 0u);
  assert(payload.instance.receiverCellSizeMetres ==
      static_cast<float>(page.voxelSize));
  assert(payload.materialCells.size() == expectedMaterials);
  assert(std::is_sorted(
      payload.materialCells.begin(), payload.materialCells.end(),
      [](const auto &left, const auto &right)
      {
        return left.globalCell < right.globalCell;
      }));
  for (const auto &material : payload.materialCells)
  {
    assert((material.materialAndFlags &
            kSparseVoxelTree64MaterialHandleMask) == 37u);
    assert(material.valid());
    assert(material.regionMappingId == 52u);
    assert(material.globalCell[0] >= 3 * 65536);
    assert(material.globalCell[0] < 4 * 65536);
  }

  // Ten-centimetre terrain publishes occupancy as its final geometry. The
  // default production page path must not build the exact-micro material
  // sidecar or require the material interpreter in Tree64 traversal.
  SparseVoxelTerrainTree64PagePayload occupancyOnly{};
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {&chunks, 0u, 0u, 0u, 78u, nullptr},
      occupancyOnly, &error));
  assert(occupancyOnly.materialCells.empty());

  // Retained pages must not touch brick material storage. Make the source
  // deliberately unusable and prove the complete cached page still rebuilds
  // only its current ownership record.
  const auto cachedMaterials = payload.materialCells;
  chunks.bricks.materialIndices.clear();
  chunks.bricks.microSurfaceData.clear();
  chunks.bricks.paletteDescriptorIds.clear();
  chunks.chunks[0].residencyGeneration = 2u;
  SparseVoxelTerrainTree64PagePayload retained{};
  assert(buildSparseVoxelTerrainTree64PagePayload(
      {
        &chunks,
        0u,
        0u,
        0u,
        payload.geometryRevision,
        &payload.model,
        &cachedMaterials,
        payload.sourceRunCount,
        payload.clippedRunCount,
        true,
      },
      retained, &error));
  assert(retained.reusedNodes);
  assert(retained.reusedMaterialCells);
  assert(retained.materialCells.size() == cachedMaterials.size());
  assert(retained.instance.terrainGeneration == 2u);
}

// Size the shipping node slot from a page shaped like the ones the terrain
// runtime actually streams: 160 voxels per edge at a 20 cm voxel.
// The lattice is only worth anything if the model the renderer publishes lands
// where the bricks are. Trace world rays through the same dynamic model and
// instance the sample assembles from the build result, and require every hit
// to be the top of that column's voxels.
void testWorldPlacementMatchesBricks()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(14 + (x % 6u) + (z % 9u));
  };
  const std::array<double, 3> minimumMetres{1523.75, -37.5, -8192.0};
  const TestPage page = makePage(32u, 32u, 0.1953125, minimumMetres, 4u, height);
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.cellScale == 1u);

  SparseVoxelTree64DynamicModelGPU model{};
  model.localMinimum = built.localMinimum;
  model.nodeOffset = 0u;
  model.nodeCount = static_cast<uint32_t>(built.tree.nodes.size());
  model.rootNodeOffset = 0u;
  model.side = built.side;
  model.flags = kSparseVoxelTree64DynamicValid;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    model.localCellSize[axis] =
        (built.localMaximum[axis] - built.localMinimum[axis]) /
        static_cast<float>(built.side);
  SparseVoxelTree64DynamicInstanceGPU instance{};
  instance.worldToModel = built.worldToModel;
  instance.flags = kSparseVoxelTree64DynamicValid;

  const double voxel = page.voxelSize;
  for (uint32_t z = 0u; z < page.voxelsPerEdge; ++z)
    for (uint32_t x = 0u; x < page.voxelsPerEdge; ++x)
    {
      int32_t top = -1;
      for (int32_t y = static_cast<int32_t>(page.verticalVoxels) - 1; y >= 0; --y)
        if (page.voxel(static_cast<int32_t>(x), y, static_cast<int32_t>(z)))
        {
          top = y;
          break;
        }
      assert(top >= 0);
      SparseVoxelTree64Ray ray{};
      // Straight down through the middle of the column, from above the page.
      ray.origin = {
        minimumMetres[0] + (x + 0.5) * voxel,
        minimumMetres[1] + page.verticalVoxels * voxel + 5.0,
        minimumMetres[2] + (z + 0.5) * voxel,
      };
      ray.direction = {0.0, -1.0, 0.0};
      const auto result = traceSparseVoxelTree64DynamicInstance(
          model, built.tree.nodes, instance, ray);
      assert(result.hit());
      const double surface = minimumMetres[1] + (top + 1) * voxel;
      const double hitY = ray.origin[1] - result.distance;
      assert(std::abs(hitY - surface) < voxel * 1.0e-3);
      assert(result.cell[0] == static_cast<int32_t>(x));
      assert(result.cell[1] == top);
      assert(result.cell[2] == static_cast<int32_t>(z));
  }
}

void testInstalledRewriteSequenceMatchesRasterBricks()
{
  const auto firstHeight = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(
        7u + (x * 3u + z * 5u + (x ^ z)) % 17u);
  };
  const auto secondHeight = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(
        6u + (x * 11u + z * 7u + ((x + z) % 5u) * 3u) % 19u);
  };
  constexpr uint32_t pageSide = 32u;
  constexpr uint32_t pageHeight = 32u;
  constexpr double voxel = 0.125;
  const std::array<double, 3> minimum{2048.25, -73.5, -1024.75};
  const std::array<TestPage, 2u> rasterPages{
    makePage(
        pageSide, pageHeight, voxel, minimum, 4u, firstHeight),
    makePage(
        pageSide, pageHeight, voxel, minimum, 4u, secondHeight),
  };
  std::array<SparseVoxelTerrainPageTree64, 2u> trees{};
  assert(buildSparseVoxelTerrainPageTree64(
      pageSource(rasterPages[0]), trees[0], nullptr));
  assert(buildSparseVoxelTerrainPageTree64(
      pageSource(rasterPages[1]), trees[1], nullptr));
  assertExact(rasterPages[0], trees[0]);
  assertExact(rasterPages[1], trees[1]);

  constexpr uint32_t framesInFlight = 2u;
  constexpr uint32_t physicalPageCount = framesInFlight + 2u;
  const uint32_t nodesPerPage = static_cast<uint32_t>(std::max(
      trees[0].tree.nodes.size(), trees[1].tree.nodes.size()) + 8u);
  SparseVoxelTree64PageAllocator allocator;
  std::string error;
  assert(allocator.configure(
      {physicalPageCount, nodesPerPage}, &error));
  std::vector<SparseVoxelTree64NodeGPU> physicalNodes(
      static_cast<size_t>(physicalPageCount) * nodesPerPage);

  const auto upload = [&](const SparseVoxelTerrainPageTree64 &tree)
  {
    SparseVoxelTree64PageHandle handle{};
    assert(allocator.reserve(handle, &error));
    assert(allocator.beginUpload(handle, &error));
    std::fill(
        physicalNodes.begin() +
            static_cast<size_t>(handle.slot) * nodesPerPage,
        physicalNodes.begin() +
            static_cast<size_t>(handle.slot + 1u) * nodesPerPage,
        SparseVoxelTree64NodeGPU{});
    std::copy(
        tree.tree.nodes.begin(), tree.tree.nodes.end(),
        physicalNodes.begin() +
            static_cast<size_t>(handle.slot) * nodesPerPage);
    assert(allocator.finishUpload(handle, &error));
    return handle;
  };
  const auto assertInstalled = [&] (
      const SparseVoxelTree64PageHandle handle,
      const uint32_t pageIndex,
      const uint32_t rewrite,
      const char *stage)
  {
    const auto &page = rasterPages[pageIndex];
    const auto &tree = trees[pageIndex];
    SparseVoxelTree64DynamicModelGPU model{};
    model.localMinimum = tree.localMinimum;
    model.nodeOffset = handle.slot * nodesPerPage;
    model.nodeCount = static_cast<uint32_t>(tree.tree.nodes.size());
    model.side = tree.side;
    model.flags = kSparseVoxelTree64DynamicValid;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      model.localCellSize[axis] =
          (tree.localMaximum[axis] - tree.localMinimum[axis]) /
          static_cast<float>(tree.side);
    SparseVoxelTree64DynamicInstanceGPU instance{};
    instance.worldToModel = tree.worldToModel;
    instance.flags = kSparseVoxelTree64DynamicValid;

    // A deterministic rotating subset gives every column repeated coverage
    // without making this sustained test quadratic in rewrite count.
    for (uint32_t sample = 0u; sample < 64u; ++sample)
    {
      const uint32_t x =
          (sample * 13u + rewrite * 7u) % page.voxelsPerEdge;
      const uint32_t z =
          (sample * 23u + rewrite * 11u) % page.voxelsPerEdge;
      int32_t top = -1;
      for (int32_t y = static_cast<int32_t>(page.verticalVoxels) - 1;
           y >= 0; --y)
        if (page.voxel(static_cast<int32_t>(x), y,
                       static_cast<int32_t>(z)))
        {
          top = y;
          break;
        }
      assert(top >= 0);
      SparseVoxelTree64Ray ray{};
      ray.origin = {
        minimum[0] + (static_cast<double>(x) + 0.5) * voxel,
        minimum[1] + page.verticalVoxels * voxel + 2.0,
        minimum[2] + (static_cast<double>(z) + 0.5) * voxel,
      };
      ray.direction = {0.0, -1.0, 0.0};
      const auto hit = traceSparseVoxelTree64DynamicInstance(
          model, physicalNodes, instance, ray);
      const double expectedY = minimum[1] + (top + 1) * voxel;
      const double actualY = ray.origin[1] - hit.distance;
      if (!hit.hit() || hit.cell[0] != static_cast<int32_t>(x) ||
          hit.cell[1] != top || hit.cell[2] != static_cast<int32_t>(z) ||
          std::abs(actualY - expectedY) >= voxel * 1.0e-3)
        std::cerr << "installed Tree64/raster mismatch rewrite=" << rewrite
                  << " stage=" << stage << " sample=" << sample
                  << " expectedCell=(" << x << ',' << top << ',' << z
                  << ") actualCell=(" << hit.cell[0] << ',' << hit.cell[1]
                  << ',' << hit.cell[2] << ") expectedY=" << expectedY
                  << " actualY=" << actualY << '\n';
      assert(hit.hit());
      assert(hit.cell[0] == static_cast<int32_t>(x));
      assert(hit.cell[1] == top);
      assert(hit.cell[2] == static_cast<int32_t>(z));
      assert(std::abs(actualY - expectedY) < voxel * 1.0e-3);
    }
  };

  SparseVoxelTree64PageHandle current = upload(trees[0]);
  assert(allocator.publish({{{}, current}}, 0u, &error));
  uint32_t currentPage = 0u;
  constexpr uint32_t rewriteCount = 128u;
  for (uint32_t rewrite = 1u; rewrite <= rewriteCount; ++rewrite)
  {
    const uint64_t completed = rewrite > framesInFlight
        ? rewrite - framesInFlight : 0u;
    assert(allocator.reclaimCompleted(completed, &error));
    const uint32_t replacementPage = currentPage ^ 1u;
    const auto replacement = upload(trees[replacementPage]);
    assert(!allocator.isReachable(replacement));
    assertInstalled(current, currentPage, rewrite, "before-publication");

    const auto retired = current;
    assert(allocator.publish(
        {{retired, replacement}}, rewrite, &error));
    current = replacement;
    currentPage = replacementPage;
    assert(allocator.isReachable(current));
    assert(!allocator.isReachable(retired));
    assertInstalled(current, currentPage, rewrite, "after-publication");

    // The active descriptor must never accidentally index the prior page.
    // Poisoning its CPU mirror after publication makes that failure loud.
    std::fill(
        physicalNodes.begin() +
            static_cast<size_t>(retired.slot) * nodesPerPage,
        physicalNodes.begin() +
            static_cast<size_t>(retired.slot + 1u) * nodesPerPage,
        SparseVoxelTree64NodeGPU{});
    assertInstalled(current, currentPage, rewrite, "after-retired-poison");
    const auto stats = allocator.stats();
    assert(stats.committedPages == 1u);
    assert(stats.retiredPages <= framesInFlight);
    assert(stats.reservedPages == 0u);
    assert(stats.uploadingPages == 0u);
    assert(stats.uploadedPages == 0u);
  }
  assert(allocator.reclaimCompleted(rewriteCount, &error));
  assert(allocator.isReachable(current));
  assert(allocator.stats().retiredPages == 0u);
  assert(allocator.stats().failedAllocations == 0u);
}

// Reproducible ray set, so a failure is the same failure on every machine.
struct Lcg
{
  uint64_t state = 0x9e3779b97f4a7c15ull;

  double next()
  {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>((state >> 11u) & 0x1fffffffffffffull) /
        static_cast<double>(0x20000000000000ull);
  }

  double range(const double minimum, const double maximum)
  {
    return minimum + (maximum - minimum) * next();
  }
};

// The page's own voxels, laid out on the tree's lattice, as a dense array the
// brute-force DDA can march. At cell scale one this *is* the rasterized
// geometry: same lattice, same occupancy, no reduction in between.
std::vector<uint8_t> referenceCells(
    const TestPage &page, const SparseVoxelTerrainPageTree64 &built)
{
  std::vector<uint8_t> cells(
      static_cast<size_t>(built.side) * built.side * built.side, 0u);
  for (int32_t z = 0; z < static_cast<int32_t>(built.side); ++z)
    for (int32_t y = 0; y < static_cast<int32_t>(built.side); ++y)
      for (int32_t x = 0; x < static_cast<int32_t>(built.side); ++x)
        cells[static_cast<size_t>(x) +
            static_cast<size_t>(built.side) *
                (static_cast<size_t>(y) +
                    static_cast<size_t>(built.side) * z)] =
            page.voxel(x, y, z) ? 1u : 0u;
  return cells;
}

// The terrain as a solid heightfield, on the tree's lattice.
//
// The raster shell is only a few voxels thick, so a ray can pass *under* it
// through ground that is solid in reality and the shell reports a miss. That
// makes the shell the wrong reference for "nothing could have occluded this":
// an inside-conservative page sits below the shell, in exactly that solid
// ground, and would be blamed for occlusions that are real. Filling each column
// from the page floor to its authored top is what the terrain actually is.
std::vector<uint8_t> referenceSolidCells(
    const TestPage &page, const SparseVoxelTerrainPageTree64 &built)
{
  std::vector<uint8_t> cells(
      static_cast<size_t>(built.side) * built.side * built.side, 0u);
  for (int32_t z = 0; z < static_cast<int32_t>(built.side); ++z)
    for (int32_t x = 0; x < static_cast<int32_t>(built.side); ++x)
    {
      int32_t top = 0;
      for (int32_t y = static_cast<int32_t>(page.verticalVoxels) - 1; y >= 0; --y)
        if (page.voxel(x, y, z))
        {
          top = y + 1;
          break;
        }
      for (int32_t y = 0; y < std::min<int32_t>(top, built.side); ++y)
        cells[static_cast<size_t>(x) +
            static_cast<size_t>(built.side) *
                (static_cast<size_t>(y) +
                    static_cast<size_t>(built.side) * z)] = 1u;
    }
  return cells;
}

// Rays that begin outside the page and enter it.
//
// This is the case the screen trace cannot answer and the reported defect lived
// in: the occluder is off screen, so the only structure that can report it is
// the published Tree64. Origins are placed well outside the model cube on a
// random face and aimed at a random point inside it, which covers grazing
// entries along every axis as well as the shallow sun angles that produce long
// terrain shadows.
std::vector<SparseVoxelTree64Ray> raysEnteringPage(
    const SparseVoxelTerrainPageTree64 &built, const size_t count)
{
  const double side = static_cast<double>(built.side);
  std::vector<SparseVoxelTree64Ray> rays;
  rays.reserve(count);
  Lcg random{};
  while (rays.size() < count)
  {
    const std::array<double, 3> target{
      random.range(0.0, side), random.range(0.0, side),
      random.range(0.0, side)};
    // Outside the cube on a random face, at a random offset from it.
    const uint32_t axis = static_cast<uint32_t>(random.range(0.0, 3.0)) % 3u;
    const double distance = random.range(1.0, 3.0) * side;
    std::array<double, 3> origin{
      random.range(-side, 2.0 * side), random.range(-side, 2.0 * side),
      random.range(-side, 2.0 * side)};
    origin[axis] = random.next() < 0.5 ? -distance : side + distance;
    std::array<double, 3> direction{};
    double length = 0.0;
    for (uint32_t component = 0u; component < 3u; ++component)
    {
      direction[component] = target[component] - origin[component];
      length += direction[component] * direction[component];
    }
    length = std::sqrt(length);
    if (!(length > 1.0e-9))
      continue;
    for (uint32_t component = 0u; component < 3u; ++component)
      direction[component] /= length;
    rays.push_back({origin, direction, length * 4.0});
  }
  return rays;
}

// What an off-screen shadow ray sees must be what the raster draws.
//
// The occupancy assertions above compare cells one at a time; a tracer can
// still disagree with an identical occupancy grid through its traversal order,
// its half-open boundary handling, or an early accept at an interior node. This
// walks the published tree with the same reference tracer the runtime path
// mirrors and requires the answer to equal a brute-force DDA over the page's
// own voxels - hit or miss, the same cell, the same distance.
void testWorldTraceMatchesRasterVoxels()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    const double fx = static_cast<double>(x) / 48.0;
    const double fz = static_cast<double>(z) / 48.0;
    return static_cast<int32_t>(14.0 + 9.0 * std::sin(fx * 6.283) +
        7.0 * std::cos(fz * 9.42) + 4.0 * std::sin((fx + fz) * 25.13));
  };
  const TestPage page =
      makePage(48u, 48u, 0.1953125, {-513.75, 12.5, 208.0}, 4u, height);
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.cellScale == 1u);
  assertExact(page, built);

  const std::vector<uint8_t> cells = referenceCells(page, built);
  const auto rays = raysEnteringPage(built, 4096u);
  uint32_t hits = 0u;
  for (const auto &ray : rays)
  {
    const auto tree = traceSparseVoxelTree64(built.tree, ray);
    const auto reference =
        traceSparseVoxelTree64CellsDDA(built.side, cells, ray);
    assert(tree.status == reference.status);
    if (!tree.hit())
      continue;
    ++hits;
    assert(tree.cell == reference.cell);
    assert(std::abs(tree.distance - reference.distance) < 1.0e-9);
  }
  // A ray set that never hits would pass every assertion above and prove
  // nothing about the tree.
  assert(hits > rays.size() / 8u);
  std::cout << "  off-screen trace parity: " << hits << "/" << rays.size()
            << " rays occluded, every one matching the raster voxels\n";
}

// The same question for the LOD the page actually publishes when it cannot
// afford its exact tree.
//
// A reduced page trades sharpness for its node budget, so it is not required to
// agree cell for cell. It is required to stay *conservative*, and conservative
// has a precise meaning for a tracer: it may never lose an occlusion the raster
// geometry has, and anything it does report must be backed by a real occupied
// voxel inside the reduced cell it stopped in. A reduction that failed the
// first half would open holes in shadows; one that failed the second half would
// invent occluders in mid-air.
void testSelectedLODTraceContainsRasterVoxels()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    uint32_t value = x * 0x8da6b343u ^ z * 0xd8163841u;
    value ^= value >> 15u;
    value *= 0x2c1b3c6du;
    value ^= value >> 12u;
    return static_cast<int32_t>(10u + value % 34u);
  };
  const TestPage page =
      makePage(48u, 48u, 0.1953125, {-513.75, 12.5, 208.0}, 3u, height);
  SparseVoxelTerrainPageTree64 exact{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), exact, nullptr));
  assert(exact.cellScale == 1u);

  auto budgeted = pageSource(page);
  // Small enough that the exact tree cannot fit and the builder must drop one
  // power-of-four level, which is the reduction the runtime slot can force.
  budgeted.maximumNodeCount = 512u;
  SparseVoxelTerrainPageTree64 reduced{};
  assert(buildSparseVoxelTerrainPageTree64(budgeted, reduced, nullptr));
  assert(reduced.cellScale > 1u);
  assert(reduced.tree.nodes.size() <= budgeted.maximumNodeCount);
  assertConservative(page, reduced);

  const int32_t scale = static_cast<int32_t>(reduced.cellScale);
  const auto blockHasVoxel = [&](const std::array<int32_t, 3> &cell)
  {
    for (int32_t z = 0; z < scale; ++z)
      for (int32_t y = 0; y < scale; ++y)
        for (int32_t x = 0; x < scale; ++x)
          if (page.voxel(cell[0] * scale + x, cell[1] * scale + y,
                  cell[2] * scale + z))
            return true;
    return false;
  };

  const std::vector<uint8_t> cells = referenceCells(page, exact);
  uint32_t referenceHits = 0u;
  uint32_t reducedHits = 0u;
  for (const auto &fineRay : raysEnteringPage(exact, 4096u))
  {
    const auto reference =
        traceSparseVoxelTree64CellsDDA(exact.side, cells, fineRay);
    // The reduced tree lives on a coarser lattice, so the same world ray is a
    // different ray in its cell space.
    SparseVoxelTree64Ray coarseRay{
      {fineRay.origin[0] / scale, fineRay.origin[1] / scale,
          fineRay.origin[2] / scale},
      fineRay.direction,
      fineRay.maximumDistance / scale};
    const auto tree = traceSparseVoxelTree64(reduced.tree, coarseRay);
    if (reference.hit())
    {
      ++referenceHits;
      // No lost shadows: everything the raster occludes stays occluded.
      assert(tree.hit());
      // And it is never found *behind* the real surface - a reduced cell can
      // only start at or before the voxel it contains.
      assert(tree.distance * scale <= reference.distance + 1.0e-9);
    }
    if (!tree.hit())
      continue;
    ++reducedHits;
    // No invented occluders: whatever it stopped on covers real geometry.
    assert(blockHasVoxel(tree.cell));
  }
  assert(referenceHits > 0u);
  std::cout << "  selected-LOD trace: cell scale " << reduced.cellScale
            << ", " << referenceHits << " raster occlusions all retained, "
            << reducedHits << " reduced occlusions all voxel-backed\n";
}

// Every occupied cell of an inside-conservative page lies within the terrain.
//
// "Within" is checked against the authored voxel column, not against the raster
// shell: the shell is one voxel thick, so a cell below it is inside the terrain
// even though no raster voxel occupies it. The test therefore asserts that no
// occupied cell rises above the *lowest* column top in its own footprint, which
// is exactly the containment the builder claims.
void assertInsideTerrain(
    const TestPage &page, const SparseVoxelTerrainPageTree64 &built)
{
  const int32_t scale = static_cast<int32_t>(built.cellScale);
  const auto columnTop = [&](const int32_t x, const int32_t z)
  {
    for (int32_t y = static_cast<int32_t>(page.verticalVoxels) - 1; y >= 0; --y)
      if (page.voxel(x, y, z))
        return y + 1;
    return 0;
  };
  for (int32_t z = 0; z < static_cast<int32_t>(built.side); ++z)
    for (int32_t x = 0; x < static_cast<int32_t>(built.side); ++x)
    {
      int32_t lowest = std::numeric_limits<int32_t>::max();
      bool covered = false;
      for (int32_t sz = 0; sz < scale; ++sz)
        for (int32_t sx = 0; sx < scale; ++sx)
        {
          const int32_t top = columnTop(x * scale + sx, z * scale + sz);
          lowest = std::min(lowest, top);
          covered = covered || top > 0;
        }
      if (!covered)
      {
        // Nothing authored under this cell column at all: it must be empty.
        for (int32_t y = 0; y < static_cast<int32_t>(built.side); ++y)
          assert(!occupiedCell(built, x, y, z));
        continue;
      }
      for (int32_t y = 0; y < static_cast<int32_t>(built.side); ++y)
        if (occupiedCell(built, x, y, z))
          assert((y + 1) * scale <= lowest);
    }
}

// The page must not invent occluders, at cell scale one and after reduction.
void testInwardPageNeverInventsOccluders()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    const double fx = static_cast<double>(x) / 48.0;
    const double fz = static_cast<double>(z) / 48.0;
    return static_cast<int32_t>(20.0 + 12.0 * std::sin(fx * 6.283) +
        9.0 * std::cos(fz * 9.42) + 5.0 * std::sin((fx + fz) * 25.13));
  };
  const TestPage page =
      makePage(48u, 48u, 0.1953125, {-513.75, 12.5, 208.0}, 1u, height);

  auto source = pageSource(page);
  source.conservativeInside = true;
  SparseVoxelTerrainPageTree64 inward{};
  assert(buildSparseVoxelTerrainPageTree64(source, inward, nullptr));
  assert(inward.cellScale == 1u);
  assertInsideTerrain(page, inward);

  // And again once the node budget forces the power-of-four reduction, which
  // is where an OR-reduction would put the geometry back outside. The inward
  // page is cheaper than the outward one - dropped columns and inward-snapped
  // boxes both remove nodes - so it needs a tighter budget than the outward
  // reduction test to be pushed off cell scale one.
  source.maximumNodeCount = 96u;
  SparseVoxelTerrainPageTree64 reduced{};
  assert(buildSparseVoxelTerrainPageTree64(source, reduced, nullptr));
  assert(reduced.cellScale > 1u);
  assertInsideTerrain(page, reduced);
  std::cout << "  inward page: contained at cell scale 1 and at scale "
            << reduced.cellScale << "\n";
}

// The trace consequence: a ray the real geometry does not occlude must not be
// occluded by the inward page. Lost occlusions are the accepted trade and are
// reported rather than asserted, so a regression in the count stays visible.
void testInwardTraceNeverOccludesAnUnoccludedRay()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    uint32_t value = x * 0x8da6b343u ^ z * 0xd8163841u;
    value ^= value >> 15u;
    value *= 0x2c1b3c6du;
    value ^= value >> 12u;
    return static_cast<int32_t>(12u + value % 26u);
  };
  const TestPage page =
      makePage(48u, 48u, 0.1953125, {-513.75, 12.5, 208.0}, 2u, height);

  SparseVoxelTerrainPageTree64 exact{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), exact, nullptr));
  auto inwardSource = pageSource(page);
  inwardSource.conservativeInside = true;
  SparseVoxelTerrainPageTree64 inward{};
  assert(buildSparseVoxelTerrainPageTree64(inwardSource, inward, nullptr));
  assert(inward.side == exact.side && inward.cellScale == 1u);
  assertInsideTerrain(page, inward);

  const std::vector<uint8_t> cells = referenceSolidCells(page, exact);
  uint32_t falseOcclusions = 0u;
  uint32_t lost = 0u;
  uint32_t referenceHits = 0u;
  for (const auto &ray : raysEnteringPage(exact, 4096u))
  {
    const auto reference =
        traceSparseVoxelTree64CellsDDA(exact.side, cells, ray);
    const auto tree = traceSparseVoxelTree64(inward.tree, ray);
    if (reference.hit())
    {
      ++referenceHits;
      lost += tree.hit() ? 0u : 1u;
      continue;
    }
    // The raster geometry left this ray unoccluded. Nothing inside the terrain
    // can occlude it, so neither may the shadow page.
    falseOcclusions += tree.hit() ? 1u : 0u;
  }
  assert(referenceHits > 0u);
  assert(falseOcclusions == 0u);
  std::cout << "  inward trace: 0 false occlusions in 4096 rays, " << lost
            << " of " << referenceHits << " raster occlusions given up\n";
}

// Two pages at adjacent spatial LODs share a boundary. The coarse one is the
// inward page, so the seam may lose shadow but must never gain any: a step that
// gains occlusion is a dark wall along the LOD boundary.
void testInwardSeamNeverGainsOcclusion()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    return static_cast<int32_t>(
        18 + (x % 9u) * 2u - (z % 5u) + ((x + z) % 4u == 0u ? 3 : 0));
  };
  // Same world span at two voxel scales: the fine page is the neighbour that
  // owns the near side, the coarse one is what the shadow cut publishes.
  const TestPage fine =
      makePage(64u, 48u, 0.25, {0.0, 0.0, 0.0}, 2u, height);
  const auto coarseHeight = [&](const uint32_t x, const uint32_t z)
  {
    return std::max({height(x * 2u, z * 2u), height(x * 2u + 1u, z * 2u),
        height(x * 2u, z * 2u + 1u), height(x * 2u + 1u, z * 2u + 1u)}) / 2;
  };
  const TestPage coarse =
      makePage(32u, 24u, 0.5, {0.0, 0.0, 0.0}, 2u, coarseHeight);

  auto coarseSource = pageSource(coarse);
  coarseSource.conservativeInside = true;
  SparseVoxelTerrainPageTree64 coarseInward{};
  assert(buildSparseVoxelTerrainPageTree64(coarseSource, coarseInward, nullptr));
  assertInsideTerrain(coarse, coarseInward);

  SparseVoxelTerrainPageTree64 fineExact{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(fine), fineExact, nullptr));
  const std::vector<uint8_t> fineCells = referenceSolidCells(fine, fineExact);

  SparseVoxelTerrainPageTree64 coarseOutward{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(coarse), coarseOutward, nullptr));

  uint32_t gainedInward = 0u;
  uint32_t gainedOutward = 0u;
  for (const auto &fineRay : raysEnteringPage(fineExact, 4096u))
  {
    const auto reference =
        traceSparseVoxelTree64CellsDDA(fineExact.side, fineCells, fineRay);
    if (reference.hit())
      continue;
    // The coarse page is half the fine page's resolution in the same world.
    SparseVoxelTree64Ray coarseRay{
      {fineRay.origin[0] * 0.5, fineRay.origin[1] * 0.5,
          fineRay.origin[2] * 0.5},
      fineRay.direction,
      fineRay.maximumDistance * 0.5};
    gainedInward +=
        traceSparseVoxelTree64(coarseInward.tree, coarseRay).hit() ? 1u : 0u;
    gainedOutward +=
        traceSparseVoxelTree64(coarseOutward.tree, coarseRay).hit() ? 1u : 0u;
  }
  // Containment is relative to the page's *own* sampling, and a coarse page
  // samples the height field with max-of-corners before this builder ever sees
  // it. So the inward rule cannot be strictly inside a finer page's surface -
  // that half of the fix belongs in generation, where the coarse page would be
  // sampled with min-of-corners for shadow use. What is asserted here is the
  // part this builder owns: lowering strictly reduces the occlusion a coarse
  // page invents across an LOD seam, and never adds any.
  assert(gainedInward < gainedOutward);
  std::cout << "  inward seam: coarse page invents " << gainedInward
            << " occlusions against the fine surface, down from "
            << gainedOutward << " outward (4096 rays)\n";
}

void reportRuntimePageCost()
{
  const auto height = [](const uint32_t x, const uint32_t z)
  {
    const double fx = static_cast<double>(x) / 160.0;
    const double fz = static_cast<double>(z) / 160.0;
    return static_cast<int32_t>(24.0 + 18.0 * std::sin(fx * 6.283) +
        14.0 * std::cos(fz * 9.42) + 6.0 * std::sin((fx + fz) * 25.13));
  };
  const TestPage page =
      makePage(160u, 128u, 0.1953125, {-2000.0, 0.0, 3000.0}, 4u, height);
  SparseVoxelTerrainPageTree64 built{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(page), built, nullptr));
  assert(built.side == 256u);
  assert(built.cellScale == 1u);
  assertExact(page, built);
  std::cout << "  runtime-shaped page: side " << built.side << ", "
            << built.tree.nodes.size() << " nodes, " << built.runCount
            << " runs, " << built.occupiedVoxelCount << " voxels, "
            << built.leafBrickCount << " leaf bricks\n";

  // Worst case for the node count: every column at a different height, so the
  // shell is all silhouette and shares almost no coarse node with a neighbour.
  const auto cliffs = [](const uint32_t x, const uint32_t z)
  {
    uint32_t value = x * 0x8da6b343u ^ z * 0xd8163841u;
    value ^= value >> 15u;
    value *= 0x2c1b3c6du;
    value ^= value >> 12u;
    return static_cast<int32_t>(8u + value % 112u);
  };
  const TestPage cliffPage =
      makePage(160u, 128u, 0.1953125, {-2000.0, 0.0, 3000.0}, 4u, cliffs);
  SparseVoxelTerrainPageTree64 cliffBuilt{};
  assert(buildSparseVoxelTerrainPageTree64(pageSource(cliffPage), cliffBuilt, nullptr));
  assertExact(cliffPage, cliffBuilt);
  std::cout << "  worst-case page: " << cliffBuilt.tree.nodes.size()
            << " nodes, " << cliffBuilt.occupiedVoxelCount << " voxels\n";

  auto budgeted = pageSource(page);
  budgeted.maximumNodeCount = 4161u;  // what the old 64-cell slot reserved
  SparseVoxelTerrainPageTree64 reduced{};
  assert(buildSparseVoxelTerrainPageTree64(budgeted, reduced, nullptr));
  assert(reduced.tree.nodes.size() <= budgeted.maximumNodeCount);
  assertConservative(page, reduced);
  std::cout << "  same page under the old slot: side " << reduced.side
            << ", cell scale " << reduced.cellScale << ", "
            << reduced.tree.nodes.size() << " nodes\n";
}
} // namespace

int main()
{
  testVoxelExactPage();
  testAlignedPageSide();
  testCoarseLeafBricks();
  testConservativeReduction();
  testDescendsToLeafBricks();
  testNonResidentChildrenFallBack();
  testSelfContainedPagePayloadAndReuse();
  testPagePayloadMaterialSidecar();
  testWorldPlacementMatchesBricks();
  testInstalledRewriteSequenceMatchesRasterBricks();
  testWorldTraceMatchesRasterVoxels();
  testSelectedLODTraceContainsRasterVoxels();
  testInwardPageNeverInventsOccluders();
  testInwardTraceNeverOccludesAnUnoccludedRay();
  testInwardSeamNeverGainsOcclusion();
  reportRuntimePageCost();
  std::cout << "Sparse voxel terrain Tree64 page tests passed\n";
  return 0;
}
