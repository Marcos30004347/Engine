#include "voxel/svraster/SparseVoxelTree64.hpp"
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
using namespace voxel::svraster;

size_t cellIndex(const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  return static_cast<size_t>(x) + static_cast<size_t>(side) * (static_cast<size_t>(y) + static_cast<size_t>(side) * z);
}

void occupy(std::vector<uint8_t> &cells, const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  cells[cellIndex(side, x, y, z)] = 1u;
}

bool identical(const SparseVoxelTree64Chunk &left, const SparseVoxelTree64Chunk &right)
{
  return left.side == right.side && left.nodes.size() == right.nodes.size() && (left.nodes.empty() || std::memcmp(left.nodes.data(), right.nodes.data(), left.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
}

void testABIAndMaskHelpers()
{
  assert(sizeof(SparseVoxelTree64NodeGPU) == 12u);
  assert(sizeof(SparseVoxelTree64ChunkGPU) == 32u);
  assert(sizeof(SparseVoxelTree64StateGPU) == 112u);
  assert(sizeof(SparseVoxelTree64TraceDispatchGPU) == 48u);
  assert(sizeof(SparseVoxelTree64DynamicTraceDispatchGPU) == 64u);
  assert(offsetof(SparseVoxelTree64NodeGPU, populationMaskLo) == 4u);
  assert(offsetof(SparseVoxelTree64NodeGPU, populationMaskHi) == 8u);
  assert(offsetof(SparseVoxelTree64ChunkGPU, nodeSlot) == 12u);
  assert(offsetof(SparseVoxelTree64ChunkGPU, flags) == 28u);
  assert(offsetof(SparseVoxelTree64StateGPU, originWorldChunk) == 16u);
  assert(offsetof(SparseVoxelTree64StateGPU, wrapOffset) == 32u);
  assert(offsetof(SparseVoxelTree64StateGPU, originCell) == 48u);
  assert(offsetof(SparseVoxelTree64StateGPU, logicalCellResolution) == 64u);
  assert(offsetof(SparseVoxelTree64StateGPU, cellSize) == 80u);
  assert(offsetof(SparseVoxelTree64StateGPU, worldExtent) == 96u);
  assert(offsetof(SparseVoxelTree64StateGPU, overflowStage) == 108u);
  assert(!sparseVoxelTree64SupportedSide(0u));
  assert(!sparseVoxelTree64SupportedSide(8u));
  assert(sparseVoxelTree64SupportedSide(4u));
  assert(sparseVoxelTree64SupportedSide(16u));
  assert(sparseVoxelTree64SupportedSide(64u));
  assert(sparseVoxelTree64Depth(4u) == 1u);
  assert(sparseVoxelTree64Depth(16u) == 2u);
  assert(sparseVoxelTree64Depth(64u) == 3u);
  assert(sparseVoxelTree64MaximumNodeCount(4u) == 1u);
  assert(sparseVoxelTree64MaximumNodeCount(16u) == 65u);
  assert(sparseVoxelTree64MaximumNodeCount(64u) == 4161u);
  assert(sparseVoxelTree64ChildIndex(0u, 0u, 0u) == 0u);
  assert(sparseVoxelTree64ChildIndex(3u, 3u, 3u) == 63u);

  SparseVoxelTree64NodeGPU node{};
  node.childBaseAndFlags = (123u << 1u) | kSparseVoxelTree64TerminalFlag;
  node.populationMaskLo = 0x80000003u;
  node.populationMaskHi = 0x80000001u;
  assert(sparseVoxelTree64NodeTerminal(node));
  assert(sparseVoxelTree64NodeChildBase(node) == 123u);
  assert(sparseVoxelTree64PopulationMask(node) == 0x8000000180000003ull);
  for (uint32_t child = 0u; child <= 64u; ++child)
  {
    uint32_t expected = 0u;
    const uint64_t mask = sparseVoxelTree64PopulationMask(node);
    for (uint32_t bit = 0u; bit < child; ++bit)
      expected += (mask & (uint64_t{1u} << bit)) != 0u ? 1u : 0u;
    assert(sparseVoxelTree64PopulationCountBefore(node, child) == expected);
  }
}

void testDistanceTraversalLOD()
{
  const SparseVoxelTree64TraceLODGPU exact{};
  assert(!exact.enabled());
  assert(exact.cutoffWorldSize(100.0f) == 0.0f);

  const SparseVoxelTree64TraceLODGPU lod{
    .worldSize = 0.25f,
    .distanceSlope = 0.01f,
    .startDistance = 1.0f,
    .maximumWorldSize = 2.0f,
  };
  assert(lod.enabled());
  assert(lod.cutoffWorldSize(0.999f) == 0.0f);
  assert(std::abs(lod.cutoffWorldSize(1.0f) - 0.25f) < 1.0e-6f);
  assert(std::abs(lod.cutoffWorldSize(51.0f) - 0.75f) < 1.0e-6f);
  assert(lod.cutoffWorldSize(1000.0f) == 2.0f);

  const SparseVoxelTree64TraceLODGPU uncapped{
    .distanceSlope = 0.02f,
    .startDistance = 5.0f,
  };
  assert(std::abs(uncapped.cutoffWorldSize(15.0f) - 0.2f) < 1.0e-6f);
}

void testSignedCoordinatesAndToroidalDirectory()
{
  assert(sparseVoxelTree64FloorDivide(65, 64u) == 1);
  assert(sparseVoxelTree64FloorDivide(64, 64u) == 1);
  assert(sparseVoxelTree64FloorDivide(63, 64u) == 0);
  assert(sparseVoxelTree64FloorDivide(0, 64u) == 0);
  assert(sparseVoxelTree64FloorDivide(-1, 64u) == -1);
  assert(sparseVoxelTree64FloorDivide(-64, 64u) == -1);
  assert(sparseVoxelTree64FloorDivide(-65, 64u) == -2);
  assert(sparseVoxelTree64FloorDivide(std::numeric_limits<int32_t>::min(), 64u) == -33554432);
  assert(sparseVoxelTree64PositiveModulo(-1, 64u) == 63u);
  assert(sparseVoxelTree64PositiveModulo(-64, 64u) == 0u);
  assert(sparseVoxelTree64PositiveModulo(-65, 64u) == 63u);

  assert(sparseVoxelTree64CellAddress({-1, -64, -65}, 64u).worldChunk == (std::array<int32_t, 3>{-1, -1, -2}));
  assert(sparseVoxelTree64CellAddress({-1, -64, -65}, 64u).chunkCell == (std::array<uint32_t, 3>{63u, 0u, 63u}));

  const std::array<int32_t, 3> origin{-4, 7, -9};
  const std::array<uint32_t, 3> resolution{9u, 9u, 9u};
  const std::array<uint32_t, 3> wrap{8u, 3u, 5u};
  std::vector<uint8_t> seen(9u * 9u * 9u, 0u);
  for (uint32_t z = 0u; z < 9u; ++z)
    for (uint32_t y = 0u; y < 9u; ++y)
      for (uint32_t x = 0u; x < 9u; ++x)
      {
        std::array<uint32_t, 3> slot{};
        assert(sparseVoxelTree64DirectorySlot(
            {
              origin[0] + static_cast<int32_t>(x),
              origin[1] + static_cast<int32_t>(y),
              origin[2] + static_cast<int32_t>(z),
            },
            origin,
            resolution,
            wrap,
            slot));
        const size_t index = slot[0] + 9u * (slot[1] + 9u * slot[2]);
        assert(seen[index] == 0u);
        seen[index] = 1u;
      }
  assert(
      std::all_of(
          seen.begin(),
          seen.end(),
          [](const uint8_t value)
          {
            return value == 1u;
          }));
  std::array<uint32_t, 3> outside{};
  assert(!sparseVoxelTree64DirectorySlot({origin[0] - 1, origin[1], origin[2]}, origin, resolution, wrap, outside));
}

void testEmptySingleAndFullChunks()
{
  std::string error;
  for (const uint32_t side : {4u, 16u, 64u})
  {
    std::vector<uint8_t> empty(static_cast<size_t>(side) * side * side, 0u);
    SparseVoxelTree64Chunk first{};
    SparseVoxelTree64Chunk second{};
    assert(buildSparseVoxelTree64(side, empty, first, &error));
    assert(error.empty());
    assert(buildSparseVoxelTree64(side, empty, second, &error));
    assert(identical(first, second));
    assert(first.nodes.size() == 1u);
    assert(validateSparseVoxelTree64(first, &error));
    for (const auto cell : {
           std::array<uint32_t, 3>{0u, 0u, 0u},
           std::array<uint32_t, 3>{side - 1u, side - 1u, side - 1u},
         })
      assert(!sparseVoxelTree64Occupied(first, cell));
  }

  std::vector<uint8_t> sparse(64u * 64u * 64u, 0u);
  occupy(sparse, 64u, 0u, 0u, 0u);
  occupy(sparse, 64u, 3u, 3u, 3u);
  occupy(sparse, 64u, 4u, 4u, 4u);
  occupy(sparse, 64u, 63u, 63u, 63u);
  SparseVoxelTree64Chunk sparseChunk{};
  assert(buildSparseVoxelTree64(64u, sparse, sparseChunk, &error));
  assert(sparseChunk.nodes.size() < 32u);
  for (uint32_t z = 0u; z < 64u; ++z)
    for (uint32_t y = 0u; y < 64u; ++y)
      for (uint32_t x = 0u; x < 64u; ++x)
        assert(sparseVoxelTree64Occupied(sparseChunk, {x, y, z}) == (sparse[cellIndex(64u, x, y, z)] != 0u));

  std::vector<uint8_t> full16(16u * 16u * 16u, 1u);
  SparseVoxelTree64Chunk full16Chunk{};
  assert(buildSparseVoxelTree64(16u, full16, full16Chunk, &error));
  assert(full16Chunk.nodes.size() == 65u);

  std::vector<uint8_t> full64(64u * 64u * 64u, 1u);
  SparseVoxelTree64Chunk full64Chunk{};
  assert(buildSparseVoxelTree64(64u, full64, full64Chunk, &error));
  assert(full64Chunk.nodes.size() == 4161u);
  assert(validateSparseVoxelTree64(full64Chunk, &error));
  assert(sparseVoxelTree64Occupied(full64Chunk, {63u, 63u, 63u}));
}

void testConservativeAABBsAndValidationFailures()
{
  std::string error;
  SparseVoxelTree64Chunk chunk{};
  const std::vector<SparseVoxelTree64AABB> aabbs{
    {{2.1, 2.1, 2.1}, {2.9, 2.9, 2.9}},
    {{4.0, 4.0, 4.0}, {5.0, 5.0, 5.0}},
    {{-2.0, -2.0, -2.0}, {0.5, 0.5, 0.5}},
    {{8.0, 8.0, 8.0}, {8.0, 9.0, 9.0}},
    {{15.5, 15.5, 15.5}, {18.0, 18.0, 18.0}},
  };
  assert(buildSparseVoxelTree64(16u, aabbs, chunk, &error));
  assert(sparseVoxelTree64Occupied(chunk, {0u, 0u, 0u}));
  assert(sparseVoxelTree64Occupied(chunk, {2u, 2u, 2u}));
  assert(sparseVoxelTree64Occupied(chunk, {4u, 4u, 4u}));
  assert(sparseVoxelTree64Occupied(chunk, {15u, 15u, 15u}));
  assert(!sparseVoxelTree64Occupied(chunk, {3u, 3u, 3u}));
  assert(!sparseVoxelTree64Occupied(chunk, {5u, 5u, 5u}));
  assert(!sparseVoxelTree64Occupied(chunk, {8u, 8u, 8u}));

  const SparseVoxelTree64Chunk retained = chunk;
  assert(!buildSparseVoxelTree64(8u, std::vector<uint8_t>(8u * 8u * 8u), chunk, &error));
  assert(identical(chunk, retained));
  assert(!buildSparseVoxelTree64(16u, std::vector<uint8_t>(1u), chunk, &error));
  assert(identical(chunk, retained));
  assert(!buildSparseVoxelTree64(16u, {{{0.0, 0.0, 0.0}, {std::numeric_limits<double>::infinity(), 1.0, 1.0}}}, chunk, &error));
  assert(identical(chunk, retained));

  SparseVoxelTree64Chunk corrupted = retained;
  corrupted.nodes.push_back({});
  assert(!validateSparseVoxelTree64(corrupted, &error));
  corrupted = retained;
  corrupted.nodes[0].childBaseAndFlags = std::numeric_limits<uint32_t>::max() & ~1u;
  assert(!validateSparseVoxelTree64(corrupted, &error));
  corrupted = retained;
  corrupted.nodes[0].childBaseAndFlags |= 1u;
  assert(!validateSparseVoxelTree64(corrupted, &error));
}

void testConservativeLowerDepthReduction()
{
  constexpr uint32_t sourceSide = 64u;
  constexpr uint32_t targetSide = 16u;
  constexpr uint32_t reduction = sourceSide / targetSide;
  std::vector<uint8_t> source(sourceSide * sourceSide * sourceSide, 0u);
  for (const auto cell : {
           std::array<uint32_t, 3>{0u, 0u, 0u},
           std::array<uint32_t, 3>{3u, 3u, 3u},
           std::array<uint32_t, 3>{4u, 4u, 4u},
           std::array<uint32_t, 3>{17u, 31u, 47u},
           std::array<uint32_t, 3>{63u, 63u, 63u},
       })
    occupy(source, sourceSide, cell[0], cell[1], cell[2]);

  std::string error;
  SparseVoxelTree64Chunk coarse{};
  assert(buildConservativeSparseVoxelTree64(
      sourceSide, source, targetSide, coarse, &error));
  assert(error.empty());
  assert(coarse.side == targetSide);
  assert(coarse.nodes.size() <=
      sparseVoxelTree64MaximumNodeCount(targetSide));
  assert(validateSparseVoxelTree64(coarse, &error));

  // The lower-depth tree must contain every occupied fine cell after aligned
  // scale conversion. Cells outside those conservative groups remain empty.
  for (uint32_t z = 0u; z < sourceSide; ++z)
    for (uint32_t y = 0u; y < sourceSide; ++y)
      for (uint32_t x = 0u; x < sourceSide; ++x)
        if (source[cellIndex(sourceSide, x, y, z)] != 0u)
          assert(sparseVoxelTree64Occupied(
              coarse, {x / reduction, y / reduction, z / reduction}));
  assert(!sparseVoxelTree64Occupied(coarse, {2u, 2u, 2u}));
  assert(!sparseVoxelTree64Occupied(coarse, {14u, 14u, 14u}));

  SparseVoxelTree64Chunk exact{};
  SparseVoxelTree64Chunk conservativeExact{};
  assert(buildSparseVoxelTree64(sourceSide, source, exact, &error));
  assert(buildConservativeSparseVoxelTree64(
      sourceSide, source, sourceSide, conservativeExact, &error));
  assert(identical(exact, conservativeExact));

  SparseVoxelTree64Chunk veryCoarse{};
  assert(buildConservativeSparseVoxelTree64(
      sourceSide, source, 4u, veryCoarse, &error));
  assert(veryCoarse.side == 4u);
  assert(sparseVoxelTree64Occupied(veryCoarse, {0u, 0u, 0u}));
  assert(sparseVoxelTree64Occupied(veryCoarse, {3u, 3u, 3u}));

  const SparseVoxelTree64Chunk retained = coarse;
  // Invalid requests fail transactionally and leave the previous tree intact.
  assert(!buildConservativeSparseVoxelTree64(
      16u, std::vector<uint8_t>(16u * 16u * 16u), 64u, coarse, &error));
  assert(identical(coarse, retained));
  assert(!buildConservativeSparseVoxelTree64(
      sourceSide, std::vector<uint8_t>(1u), targetSide, coarse, &error));
  assert(identical(coarse, retained));
}

void testExtendedSparseModelTree()
{
  assert(!sparseVoxelTree64SupportedSide(256u));
  assert(sparseVoxelTree64SupportedModelSide(256u));
  assert(sparseVoxelTree64SupportedModelSide(65536u));
  assert(!sparseVoxelTree64SupportedModelSide(262144u));
  assert(sparseVoxelTree64ModelDepth(65536u) == 8u);

  std::string error;
  SparseVoxelTree64Chunk tree;
  const std::vector<SparseVoxelTree64AABB> occupied{
    {{0.0, 0.0, 0.0}, {16384.0, 16384.0, 16384.0}},
    {{65535.0, 65535.0, 65535.0}, {65536.0, 65536.0, 65536.0}},
  };
  assert(buildSparseVoxelTree64Model(65536u, occupied, tree, &error));
  assert(validateSparseVoxelTree64Model(tree, &error));
  assert(!validateSparseVoxelTree64(tree, &error));
  assert(tree.nodes.size() < 32u);
  assert(sparseVoxelTree64Occupied(tree, {0u, 0u, 0u}));
  assert(sparseVoxelTree64Occupied(tree, {8192u, 8192u, 8192u}));
  assert(sparseVoxelTree64Occupied(tree, {65535u, 65535u, 65535u}));
  assert(!sparseVoxelTree64Occupied(tree, {16384u, 16384u, 16384u}));
  assert(!sparseVoxelTree64Occupied(tree, {32768u, 32768u, 32768u}));

  const auto coarseHit = traceSparseVoxelTree64(
      tree,
      {
        {-1.0, 1024.0, 1024.0},
        {1.0, 0.0, 0.0},
        70000.0,
      });
  assert(coarseHit.hit());
  assert(std::abs(coarseHit.distance - 1.0) < 1.0e-6);

  SparseVoxelTree64Chunk full;
  assert(buildSparseVoxelTree64Model(
      65536u,
      {{{0.0, 0.0, 0.0}, {65536.0, 65536.0, 65536.0}}},
      full,
      &error));
  assert(full.nodes.size() == 1u);
  assert(sparseVoxelTree64NodeTerminal(full.nodes[0]));
  assert(sparseVoxelTree64PopulationMask(full.nodes[0]) ==
      ~uint64_t{0u});
}

void assertEquivalent(const SparseVoxelTree64Chunk &chunk, const std::vector<uint8_t> &cells, const SparseVoxelTree64Ray &ray)
{
  const auto dda = traceSparseVoxelTree64CellsDDA(chunk.side, cells, ray);
  const auto tree = traceSparseVoxelTree64(chunk, ray);
  assert(dda.status != SparseVoxelTree64TraceStatus::Invalid);
  assert(dda.status != SparseVoxelTree64TraceStatus::QueryLimit);
  assert(tree.status != SparseVoxelTree64TraceStatus::Invalid);
  assert(tree.status != SparseVoxelTree64TraceStatus::QueryLimit);
  assert(dda.hit() == tree.hit());
  if (dda.hit())
  {
    assert(dda.cell == tree.cell);
    if (std::abs(dda.distance - tree.distance) >= 1.0e-9)
      std::cerr << "Tree64 distance mismatch side=" << chunk.side << " cell=(" << dda.cell[0] << "," << dda.cell[1] << "," << dda.cell[2] << ") dda=" << dda.distance << " tree=" << tree.distance << " origin=("
                << ray.origin[0] << "," << ray.origin[1] << "," << ray.origin[2] << ") direction=(" << ray.direction[0] << "," << ray.direction[1] << "," << ray.direction[2] << ") maximum=" << ray.maximumDistance
                << "\n";
    assert(std::abs(dda.distance - tree.distance) < 1.0e-9);
    assert(sparseVoxelTree64Occupied(
        chunk,
        {
          static_cast<uint32_t>(tree.cell[0]),
          static_cast<uint32_t>(tree.cell[1]),
          static_cast<uint32_t>(tree.cell[2]),
        }));
  }
}

void testAnalyticTraversal()
{
  constexpr uint32_t side = 16u;
  std::vector<uint8_t> cells(side * side * side, 0u);
  occupy(cells, side, 0u, 0u, 0u);
  occupy(cells, side, 2u, 2u, 2u);
  occupy(cells, side, 7u, 8u, 9u);
  occupy(cells, side, 15u, 15u, 15u);
  SparseVoxelTree64Chunk chunk{};
  std::string error;
  assert(buildSparseVoxelTree64(side, cells, chunk, &error));

  const std::vector<SparseVoxelTree64Ray> rays{
    {{-4.0, 2.5, 2.5}, {1.0, 0.0, 0.0}},
    {{20.0, 2.5, 2.5}, {-1.0, 0.0, 0.0}},
    {{2.5, 2.5, 2.5}, {1.0, 0.0, 0.0}},
    {{3.0, 2.5, 2.5}, {-1.0, 0.0, 0.0}},
    {{2.0, 2.5, 2.5}, {1.0, 0.0, 0.0}},
    {{-4.0, -4.0, -4.0}, {1.0, 1.0, 1.0}},
    {{16.0, 15.5, 15.5}, {-1.0, 0.0, 0.0}},
    {{-4.0, 3.0, 3.0}, {1.0, 0.0, 0.0}},
    {{-4.0, 2.5, 2.5}, {1.0, 0.0, 0.0}, 5.0},
    {{7.5, 8.5, 20.0}, {0.0, 0.0, -1.0}},
  };
  for (const auto &ray : rays)
    assertEquivalent(chunk, cells, ray);
  const auto forward = traceSparseVoxelTree64(chunk, rays[0]);
  assert(forward.hit());
  assert(forward.cell == (std::array<int32_t, 3>{2, 2, 2}));
  const auto occupiedStart = traceSparseVoxelTree64(chunk, rays[2]);
  assert(occupiedStart.hit() && occupiedStart.distance == 0.0);
  assert(!traceSparseVoxelTree64(chunk, rays[7]).hit());
  assert(!traceSparseVoxelTree64(chunk, rays[8]).hit());

  SparseVoxelTree64Ray invalid = rays.front();
  invalid.direction = {0.0, 0.0, 0.0};
  assert(traceSparseVoxelTree64(chunk, invalid).status == SparseVoxelTree64TraceStatus::Invalid);
  assert(traceSparseVoxelTree64(chunk, rays.front(), 0u).status == SparseVoxelTree64TraceStatus::QueryLimit);
}

uint32_t nextRandom(uint32_t &state)
{
  state = state * 1664525u + 1013904223u;
  return state;
}

double randomRange(uint32_t &state, const double minimum, const double maximum)
{
  const double unit = static_cast<double>(nextRandom(state)) / static_cast<double>(std::numeric_limits<uint32_t>::max());
  return minimum + (maximum - minimum) * unit;
}

void testRandomizedTraversalAndDeterminism()
{
  uint32_t random = 0x4d595df4u;
  for (const uint32_t side : {16u, 64u})
  {
    const uint32_t patternCount = side == 16u ? 24u : 8u;
    const uint32_t rayCount = side == 16u ? 500u : 300u;
    for (uint32_t pattern = 0u; pattern < patternCount; ++pattern)
    {
      std::vector<uint8_t> cells(static_cast<size_t>(side) * side * side, 0u);
      const uint32_t threshold = pattern % 4u == 0u ? 1u : pattern % 4u == 1u ? 8u : pattern % 4u == 2u ? 64u : 256u;
      for (uint8_t &cell : cells)
        cell = (nextRandom(random) & 1023u) < threshold ? 1u : 0u;
      SparseVoxelTree64Chunk first{};
      SparseVoxelTree64Chunk second{};
      std::string error;
      assert(buildSparseVoxelTree64(side, cells, first, &error));
      assert(buildSparseVoxelTree64(side, cells, second, &error));
      assert(identical(first, second));
      for (uint32_t rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
      {
        SparseVoxelTree64Ray ray{};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          ray.origin[axis] = randomRange(random, -16.0, side + 16.0);
          ray.direction[axis] = randomRange(random, -1.0, 1.0);
        }
        if ((rayIndex % 9u) == 0u)
          ray.direction[rayIndex % 3u] = 0.0;
        if (ray.direction == (std::array<double, 3>{0.0, 0.0, 0.0}))
          ray.direction[0] = 1.0;
        ray.maximumDistance = randomRange(random, 0.0, side * 4.0);
        assertEquivalent(first, cells, ray);
      }
    }
  }
}
} // namespace

int main()
{
  testABIAndMaskHelpers();
  testDistanceTraversalLOD();
  testSignedCoordinatesAndToroidalDirectory();
  testEmptySingleAndFullChunks();
  testConservativeAABBsAndValidationFailures();
  testConservativeLowerDepthReduction();
  testExtendedSparseModelTree();
  testAnalyticTraversal();
  testRandomizedTraversalAndDeterminism();
  std::cout << "Sparse voxel Tree64 T1 CPU tests passed\n";
  return 0;
}
