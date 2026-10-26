#include "voxel/svraster/TerrainGPUChunkContract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ")
            << message << '\n';
  failures += condition ? 0 : 1;
}

svr::TerrainGPUChunkFace face(const uint32_t axis, const bool positive)
{
  static constexpr std::array<svr::TerrainGPUChunkFace, 6u> faces{
    svr::TerrainGPUChunkFace::NegativeX,
    svr::TerrainGPUChunkFace::PositiveX,
    svr::TerrainGPUChunkFace::NegativeY,
    svr::TerrainGPUChunkFace::PositiveY,
    svr::TerrainGPUChunkFace::NegativeZ,
    svr::TerrainGPUChunkFace::PositiveZ,
  };
  return faces[axis * 2u + (positive ? 1u : 0u)];
}

std::vector<svr::TerrainGPUBoundarySample> faceSamples(
    const std::array<int64_t, 3u> &minimum,
    const std::array<int64_t, 3u> &maximum,
    const uint32_t axis,
    const int64_t plane)
{
  std::array<uint32_t, 2u> tangent{};
  uint32_t count = 0u;
  for (uint32_t candidate = 0u; candidate < 3u; ++candidate)
    if (candidate != axis)
      tangent[count++] = candidate;
  std::vector<svr::TerrainGPUBoundarySample> result;
  for (uint32_t highV = 0u; highV < 2u; ++highV)
    for (uint32_t highU = 0u; highU < 2u; ++highU)
    {
      svr::TerrainGPUBoundarySample sample{};
      sample.canonicalCoordinate = minimum;
      sample.canonicalCoordinate[axis] = plane;
      sample.canonicalCoordinate[tangent[0]] = highU
          ? maximum[tangent[0]] : minimum[tangent[0]];
      sample.canonicalCoordinate[tangent[1]] = highV
          ? maximum[tangent[1]] : minimum[tangent[1]];
      sample.value = static_cast<int32_t>(
          sample.canonicalCoordinate[tangent[0]] * 7 +
          sample.canonicalCoordinate[tangent[1]] * 13);
      sample.flags = highU | (highV << 1u);
      result.push_back(sample);
    }
  return result;
}

uint64_t volume(const svr::TerrainGPUChunkBounds &bounds)
{
  return uint64_t{static_cast<uint32_t>(
             bounds.maximum[0] - bounds.minimum[0])} *
      static_cast<uint32_t>(bounds.maximum[1] - bounds.minimum[1]) *
      static_cast<uint32_t>(bounds.maximum[2] - bounds.minimum[2]);
}

void testABI()
{
  std::cout << "V2 chunk and stage ABI\n";
  check(sizeof(svr::TerrainGPUChunkKey) == 32u,
      "logical chunk key has the frozen 32-byte layout");
  check(sizeof(svr::TerrainGPUChunkShape) == 8u,
      "logical chunk shape has the frozen 8-byte layout");
  check(sizeof(svr::TerrainGPUChunkJobGPU) == 128u,
      "GPU job ABI is 128 bytes and 16-byte aligned");

  constexpr std::array<svr::TerrainGPUJobStage, 15u> stages{
    svr::TerrainGPUJobStage::WaitingForAtlas,
    svr::TerrainGPUJobStage::SampleColumns,
    svr::TerrainGPUJobStage::ClassifyFineBricks,
    svr::TerrainGPUJobStage::ScanFineBricks,
    svr::TerrainGPUJobStage::EmitFineBricks,
    svr::TerrainGPUJobStage::BuildRasterParents,
    svr::TerrainGPUJobStage::BuildTree64Masks,
    svr::TerrainGPUJobStage::ScanTree64,
    svr::TerrainGPUJobStage::LinkTree64,
    svr::TerrainGPUJobStage::Validate,
    svr::TerrainGPUJobStage::ReadyToPublish,
    svr::TerrainGPUJobStage::SubmittedForPublication,
    svr::TerrainGPUJobStage::Complete,
    svr::TerrainGPUJobStage::Cancelled,
    svr::TerrainGPUJobStage::Failed,
  };
  bool monotonic = true;
  for (size_t index = 0u; index < stages.size(); ++index)
    monotonic &= static_cast<uint32_t>(stages[index]) == index;
  check(monotonic, "job stage ABI values are stable and monotonic");

  svr::TerrainGPUChunkKey key{
    0x1020304050607080ull, -3, 4, -5, 7u, 19u, 23u, 29u};
  const svr::TerrainGPUChunkShape shape{2u, 3u, 4u, 8u};
  svr::TerrainGPUChunkJobGPU job{};
  std::string error;
  const bool built = svr::terrainGPUChunkJobGPU(
      key, shape, svr::TerrainGPUJobStage::ClassifyFineBricks,
      24u, job, &error);
  check(built && job.key.chunkX == -3 && job.key.chunkY == 4 &&
          job.key.chunkZ == -5 &&
          (job.key.packedLODAndGeneratorRevision & 0xffffu) == 7u &&
          (job.key.packedLODAndGeneratorRevision >> 16u) == 19u &&
          job.stage == static_cast<uint32_t>(
              svr::TerrainGPUJobStage::ClassifyFineBricks) &&
          job.stageItemCount == 24u && job.bounds.minimum[0] == -48 &&
          job.bounds.minimum[1] == 96 && job.bounds.minimum[2] == -160,
      "logical identity, shape, signed bounds, generation and stage pack into the GPU ABI: " + error);
}

void testCoordinates()
{
  std::cout << "negative and cross-domain coordinates\n";
  bool floorCorrect = true;
  for (int64_t denominator = 1; denominator <= 31; ++denominator)
    for (int64_t numerator = -257; numerator <= 257; ++numerator)
    {
      const int64_t quotient = svr::terrainGPUFloorDivide(
          numerator, denominator);
      const int64_t remainder = numerator - quotient * denominator;
      floorCorrect &= remainder >= 0 && remainder < denominator;
    }
  check(floorCorrect,
      "floor division preserves negative chunk ownership");

  svr::TerrainGPUCanonicalVoxel canonical{};
  std::string error;
  const bool quantized = svr::terrainGPUWorldToCanonicalVoxel(
      {9.875, -19.999, 3.999}, {10.0, -20.0, 3.0}, 0.125,
      canonical, &error);
  check(quantized && canonical.coordinate ==
          std::array<int64_t, 3u>{-1, 0, 7},
      "world metres are quantized once onto the finest integer lattice: " + error);

  canonical.coordinate = {-1, -8, -9};
  svr::TerrainGPUClipmapVoxel clipmap{};
  svr::TerrainGPUCanonicalVoxel reconstructed{};
  const bool clipmapConverted = svr::terrainGPUCanonicalToClipmapVoxel(
      canonical, 3u, clipmap, &error) &&
      svr::terrainGPUClipmapVoxelMinimum(clipmap, reconstructed, &error);
  check(clipmapConverted && clipmap.coordinate ==
          std::array<int32_t, 3u>{-1, -1, -2} &&
          reconstructed.coordinate ==
          std::array<int64_t, 3u>{-8, -8, -16},
      "canonical and clipmap voxel conversion floors negative coordinates: " + error);

  clipmap = {{-1, 31, -33}, 2u};
  svr::TerrainGPUColumnTileCoordinate tile{};
  svr::TerrainGPUChunkKey chunkIdentity{
    11u, 0, 0, 0, 0u, 3u, 5u, 7u};
  svr::TerrainGPUChunkKey chunk{};
  svr::TerrainGPUBrickCoordinate brick{};
  const svr::TerrainGPUChunkShape shape{2u, 2u, 2u, 8u};
  const bool converted = svr::terrainGPUColumnTileForVoxel(
          clipmap, 16u, 16u, tile, &error) &&
      svr::terrainGPUChunkKeyForVoxel(
          clipmap, shape, chunkIdentity, chunk, &error) &&
      svr::terrainGPUBrickForVoxel(clipmap, 8u, brick, &error);
  check(converted && tile.tileX == -1 && tile.tileZ == -3 &&
          chunk.chunkX == -1 && chunk.chunkY == 1 && chunk.chunkZ == -3 &&
          chunk.clipmapLOD == 2u && brick.coordinate ==
          std::array<int32_t, 3u>{-1, 3, -5},
      "voxel converts deterministically to column tile, 3D chunk and brick: " + error);

  std::array<int32_t, 2u> tileMinimum{};
  std::array<int32_t, 2u> tileMaximum{};
  svr::TerrainGPUChunkBounds brickBounds{};
  std::array<double, 3u> worldMinimum{};
  std::array<double, 3u> worldMaximum{};
  const bool reversed = svr::terrainGPUColumnTileVoxelBounds(
          tile, 16u, 16u, tileMinimum, tileMaximum, &error) &&
      svr::terrainGPUBrickVoxelBounds(brick, 8u, brickBounds, &error) &&
      svr::terrainGPUClipmapVoxelWorldBounds(
          clipmap, {10.0, -20.0, 3.0}, 0.125,
          worldMinimum, worldMaximum, &error);
  check(reversed && clipmap.coordinate[0] >= tileMinimum[0] &&
          clipmap.coordinate[0] < tileMaximum[0] &&
          clipmap.coordinate[2] >= tileMinimum[1] &&
          clipmap.coordinate[2] < tileMaximum[1] &&
          svr::terrainGPUChunkContainsVoxel(
              brickBounds, clipmap.coordinate) &&
          worldMinimum[0] == 9.5 && worldMaximum[0] == 10.0 &&
          worldMinimum[1] == -4.5 && worldMaximum[1] == -4.0 &&
          worldMinimum[2] == -13.5 && worldMaximum[2] == -13.0,
      "clipmap, column-tile and brick coordinates convert back to exact half-open bounds: " + error);

  svr::TerrainGPUChunkBounds bounds{};
  check(svr::terrainGPUChunkBounds(chunk, shape, bounds, &error) &&
          svr::terrainGPUChunkContainsVoxel(bounds, clipmap.coordinate),
      "derived half-open chunk bounds contain the source voxel: " + error);

  clipmap.clipmapLOD = 31u;
  check(!svr::terrainGPUChunkKeyForVoxel(
          clipmap, shape, chunkIdentity, chunk, &error) &&
          !svr::terrainGPUBrickForVoxel(clipmap, 8u, brick, &error) &&
          !svr::terrainGPUColumnTileForVoxel(
              clipmap, 16u, 16u, tile, &error),
      "coordinates outside the signed 32-bit clipmap shader domain are rejected");
}

void testAdjacentChunksAndBoundaries()
{
  std::cout << "half-open adjacency and shared boundaries\n";
  const svr::TerrainGPUChunkShape shape{2u, 2u, 2u, 2u};
  const svr::TerrainGPUChunkKey center{
    1u, -1, -1, -1, 0u, 1u, 2u, 3u};
  bool allFaces = true;
  std::string error;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    for (uint32_t direction = 0u; direction < 2u; ++direction)
    {
      const int32_t step = direction == 0u ? -1 : 1;
      svr::TerrainGPUChunkKey neighbour = center;
      if (axis == 0u)
        neighbour.chunkX += step;
      else if (axis == 1u)
        neighbour.chunkY += step;
      else
        neighbour.chunkZ += step;
      svr::TerrainGPUChunkBounds centerBounds{};
      svr::TerrainGPUChunkBounds neighbourBounds{};
      allFaces &= svr::terrainGPUChunkBounds(
          center, shape, centerBounds, &error);
      allFaces &= svr::terrainGPUChunkBounds(
          neighbour, shape, neighbourBounds, &error);
      allFaces &= !svr::terrainGPUChunkBoundsOverlap(
          centerBounds, neighbourBounds);

      svr::TerrainGPUChunkBounds unionBounds{};
      for (uint32_t component = 0u; component < 3u; ++component)
      {
        unionBounds.minimum[component] = std::min(
            centerBounds.minimum[component],
            neighbourBounds.minimum[component]);
        unionBounds.maximum[component] = std::max(
            centerBounds.maximum[component],
            neighbourBounds.maximum[component]);
      }
      allFaces &= volume(unionBounds) ==
          volume(centerBounds) + volume(neighbourBounds);
      for (int32_t z = unionBounds.minimum[2];
           z < unionBounds.maximum[2]; ++z)
        for (int32_t y = unionBounds.minimum[1];
             y < unionBounds.maximum[1]; ++y)
          for (int32_t x = unionBounds.minimum[0];
               x < unionBounds.maximum[0]; ++x)
          {
            const std::array<int32_t, 3u> voxel{x, y, z};
            const uint32_t owners =
                (svr::terrainGPUChunkContainsVoxel(centerBounds, voxel)
                     ? 1u : 0u) +
                (svr::terrainGPUChunkContainsVoxel(neighbourBounds, voxel)
                     ? 1u : 0u);
            allFaces &= owners == 1u;
          }

      const int32_t sharedPlane = step > 0
          ? centerBounds.maximum[axis] : centerBounds.minimum[axis];
      svr::TerrainGPUCanonicalVoxel point{{
        centerBounds.minimum[0], centerBounds.minimum[1],
        centerBounds.minimum[2]}};
      point.coordinate[axis] = sharedPlane;
      const bool centerOwns = svr::terrainGPUChunkOwnsCanonicalPoint(
          center, shape, point, &error);
      const bool neighbourOwns = svr::terrainGPUChunkOwnsCanonicalPoint(
          neighbour, shape, point, &error);
      allFaces &= centerOwns != neighbourOwns;
      allFaces &= centerOwns == (step < 0);

      std::array<int64_t, 3u> canonicalMinimum{};
      std::array<int64_t, 3u> canonicalMaximum{};
      allFaces &= svr::terrainGPUCanonicalChunkBounds(
          center, shape, canonicalMinimum, canonicalMaximum, &error);
      auto samples = faceSamples(
          canonicalMinimum, canonicalMaximum, axis, sharedPlane);
      auto reversed = samples;
      std::reverse(reversed.begin(), reversed.end());
      svr::TerrainGPUBoundarySignature centerSignature{};
      svr::TerrainGPUBoundarySignature neighbourSignature{};
      allFaces &= svr::terrainGPUBoundarySignatureForChunk(
          center, shape, face(axis, step > 0), samples,
          centerSignature, &error);
      allFaces &= svr::terrainGPUBoundarySignatureForChunk(
          neighbour, shape, face(axis, step < 0), reversed,
          neighbourSignature, &error);
      allFaces &= centerSignature == neighbourSignature;
    }
  check(allFaces,
      "all six adjacent faces are disjoint, complete, singly owned and signature-identical: " + error);

  const svr::TerrainGPUChunkShape fineShape{2u, 2u, 2u, 8u};
  const svr::TerrainGPUChunkShape coarseShape{1u, 1u, 1u, 8u};
  const svr::TerrainGPUChunkKey fine{
    9u, 0, 0, 0, 0u, 4u, 6u, 8u};
  const svr::TerrainGPUChunkKey coarse{
    9u, 1, 0, 0, 1u, 4u, 6u, 8u};
  std::array<int64_t, 3u> fineMinimum{};
  std::array<int64_t, 3u> fineMaximum{};
  std::array<int64_t, 3u> coarseMinimum{};
  std::array<int64_t, 3u> coarseMaximum{};
  bool crossLOD = svr::terrainGPUCanonicalChunkBounds(
      fine, fineShape, fineMinimum, fineMaximum, &error) &&
      svr::terrainGPUCanonicalChunkBounds(
          coarse, coarseShape, coarseMinimum, coarseMaximum, &error);
  auto samples = faceSamples(
      fineMinimum, fineMaximum, 0u, fineMaximum[0]);
  svr::TerrainGPUBoundarySignature fineSignature{};
  svr::TerrainGPUBoundarySignature coarseSignature{};
  crossLOD &= fineMaximum[0] == coarseMinimum[0] &&
      fineMinimum[1] == coarseMinimum[1] &&
      fineMaximum[1] == coarseMaximum[1] &&
      fineMinimum[2] == coarseMinimum[2] &&
      fineMaximum[2] == coarseMaximum[2] &&
      svr::terrainGPUBoundarySignatureForChunk(
          fine, fineShape, svr::TerrainGPUChunkFace::PositiveX,
          samples, fineSignature, &error) &&
      svr::terrainGPUBoundarySignatureForChunk(
          coarse, coarseShape, svr::TerrainGPUChunkFace::NegativeX,
          samples, coarseSignature, &error) &&
      fineSignature == coarseSignature;
  check(crossLOD,
      "adjacent LODs derive the same finest-grid boundary and signature: " + error);

  if (!samples.empty())
    samples.front().canonicalCoordinate[0] += 1;
  svr::TerrainGPUBoundarySignature rejected{};
  check(!svr::terrainGPUBoundarySignatureForChunk(
          fine, fineShape, svr::TerrainGPUChunkFace::PositiveX,
          samples, rejected, &error),
      "a boundary sample displaced off the canonical face is rejected");
}

void testVerticalStacks()
{
  std::cout << "vertical stack enumeration\n";
  const svr::TerrainGPUChunkShape shape{2u, 2u, 2u, 8u};
  const svr::TerrainGPUChunkKey horizontal{
    5u, 3, 99, -4, 0u, 2u, 7u, 11u};
  const std::vector<svr::TerrainGPUColumnRun> runs{
    {-17, -1}, {-4, 8}, {15, 15}, {9, 8}};
  std::vector<svr::TerrainGPUChunkKey> chunks;
  std::string error;
  bool correct = svr::terrainGPUEnumerateVerticalChunks(
      horizontal, shape, runs, chunks, &error);
  correct &= chunks.size() == 3u && chunks[0].chunkY == -2 &&
      chunks[1].chunkY == -1 && chunks[2].chunkY == 0;
  for (const auto &chunk : chunks)
  {
    svr::TerrainGPUChunkBounds bounds{};
    correct &= svr::terrainGPUChunkBounds(chunk, shape, bounds, &error);
    bool intersects = false;
    for (const auto &run : runs)
      intersects |= svr::terrainGPUColumnRunIntersectsChunk(run, bounds);
    correct &= intersects;
  }
  check(correct,
      "signed surface/foundation runs enumerate the exact intersecting stack: " + error);

  const std::vector<svr::TerrainGPUColumnRun> foundation{{-65, 3}};
  check(svr::terrainGPUEnumerateVerticalChunks(
          horizontal, shape, foundation, chunks, &error) &&
          chunks.size() == 6u && chunks.front().chunkY == -5 &&
          chunks.back().chunkY == 0,
      "a foundation crossing renderer zero stacks chunks below and above the datum: " + error);

  const std::vector<svr::TerrainGPUColumnRun> empty{{4, 3}, {0, -1}};
  check(svr::terrainGPUEnumerateVerticalChunks(
          horizontal, shape, empty, chunks, &error) && chunks.empty(),
      "empty column runs schedule no vertical jobs");

  const std::vector<svr::TerrainGPUColumnRun> overflow{{
    0, std::numeric_limits<int32_t>::max()}};
  check(!svr::terrainGPUEnumerateVerticalChunks(
          horizontal, shape, overflow, chunks, &error),
      "an inclusive top that cannot become a half-open bound is rejected");
}

void testDenseBricksAndLimits()
{
  std::cout << "dense brick oracle and shader indexing limits\n";
  const svr::TerrainGPUChunkShape shape{3u, 2u, 4u, 8u};
  const svr::TerrainGPUChunkKey key{
    12u, -1, -2, 2, 0u, 1u, 3u, 4u};
  std::vector<svr::TerrainGPUBrickCandidate> candidates;
  std::string error;
  bool exact = svr::terrainGPUEnumerateDenseBrickCandidates(
      key, shape, candidates, &error);
  exact &= candidates.size() == 24u;
  std::set<std::array<int32_t, 3u>> unique;
  for (size_t index = 0u; index < candidates.size(); ++index)
  {
    exact &= candidates[index].linearIndex == index;
    exact &= unique.insert(candidates[index].canonicalBrick).second;
  }
  exact &= !candidates.empty() &&
      candidates.front().canonicalBrick ==
          std::array<int32_t, 3u>{-3, -4, 8} &&
      candidates.back().localBrick ==
          std::array<uint32_t, 3u>{2u, 1u, 3u} &&
      candidates.back().minimumVoxel ==
          std::array<int32_t, 3u>{-8, -24, 88};
  check(exact,
      "CPU oracle emits every dense brick once in deterministic X-fastest order: " + error);

  svr::TerrainGPUChunkShapeCounts counts{};
  check(svr::terrainGPUChunkShapeCounts(
          {255u, 255u, 129u, 8u}, counts, &error) &&
          counts.denseVoxelCount < std::numeric_limits<uint32_t>::max(),
      "largest near-limit fixture accepted below the 32-bit dense-cell limit: " + error);
  check(!svr::terrainGPUChunkShapeCounts(
          {256u, 256u, 128u, 8u}, counts, &error),
      "shape producing exactly 2^32 dense cells is rejected");
  check(!svr::terrainGPUChunkShapeCounts(
          {1u, 1u, 1u, 7u}, counts, &error),
      "non-power-of-two brick side is rejected");
  check(!svr::terrainGPUChunkShapeCounts(
          {65535u, 65535u, 65535u, 32768u}, counts, &error),
      "overflowing extreme shape is rejected before multiplication wraps");

  svr::TerrainGPUChunkBounds bounds{};
  svr::TerrainGPUChunkKey edgeKey = key;
  edgeKey.chunkX = std::numeric_limits<int32_t>::max();
  check(!svr::terrainGPUChunkBounds(
          edgeKey, {1u, 1u, 1u, 8u}, bounds, &error),
      "chunk-origin multiplication that exceeds signed shader coordinates is rejected");

  std::mt19937 random(0x6f5f4a21u);
  bool randomized = true;
  constexpr std::array<uint16_t, 5u> sides{1u, 2u, 4u, 8u, 16u};
  for (uint32_t trial = 0u; trial < 4000u; ++trial)
  {
    const svr::TerrainGPUChunkShape candidateShape{
      static_cast<uint16_t>(1u + random() % 1024u),
      static_cast<uint16_t>(1u + random() % 1024u),
      static_cast<uint16_t>(1u + random() % 1024u),
      sides[random() % sides.size()]};
    const uint64_t bricks = uint64_t{candidateShape.bricksX} *
        candidateShape.bricksY * candidateShape.bricksZ;
    const uint64_t side = candidateShape.brickSide;
    const uint64_t denseCells = bricks * side * side * side;
    const uint64_t voxelsX = uint64_t{candidateShape.bricksX} * side;
    const uint64_t voxelsY = uint64_t{candidateShape.bricksY} * side;
    const uint64_t voxelsZ = uint64_t{candidateShape.bricksZ} * side;
    const uint64_t columns = voxelsX * voxelsZ;
    const bool expected = bricks <= std::numeric_limits<uint32_t>::max() &&
        denseCells <= std::numeric_limits<uint32_t>::max() &&
        columns <= std::numeric_limits<uint32_t>::max() &&
        voxelsX <= std::numeric_limits<int32_t>::max() &&
        voxelsY <= std::numeric_limits<int32_t>::max() &&
        voxelsZ <= std::numeric_limits<int32_t>::max();
    const bool accepted = svr::terrainGPUChunkShapeCounts(
        candidateShape, counts, nullptr);
    randomized &= accepted == expected;
    if (accepted && counts.denseBrickCount <= 4096u)
    {
      svr::TerrainGPUChunkKey randomKey = key;
      randomKey.chunkX = static_cast<int32_t>(random() % 201u) - 100;
      randomKey.chunkY = static_cast<int32_t>(random() % 201u) - 100;
      randomKey.chunkZ = static_cast<int32_t>(random() % 201u) - 100;
      randomized &= svr::terrainGPUEnumerateDenseBrickCandidates(
          randomKey, candidateShape, candidates, nullptr);
      randomized &= candidates.size() == counts.denseBrickCount;
    }
  }
  check(randomized,
      "4000 randomized shapes agree with independent 64-bit shader-index bounds");
}

} // namespace

int main()
{
  std::cout << "GPU terrain V2 Phase 1 contract tests\n";
  testABI();
  testCoordinates();
  testAdjacentChunksAndBoundaries();
  testVerticalStacks();
  testDenseBricksAndLimits();
  if (failures != 0)
  {
    std::cerr << failures << " V2 Phase 1 checks failed\n";
    return 1;
  }
  std::cout << "GPU terrain V2 Phase 1 contract passed\n";
  return 0;
}
