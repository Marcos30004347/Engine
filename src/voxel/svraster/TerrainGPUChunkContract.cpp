#include "TerrainGPUChunkContract.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace voxel::svraster
{
namespace
{
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint16_t kMaximumShaderClipmapLOD = 30u;

void setError(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
}

bool checkedU32(const uint64_t value, uint32_t &result)
{
  if (value > std::numeric_limits<uint32_t>::max())
    return false;
  result = static_cast<uint32_t>(value);
  return true;
}

bool checkedMultiply(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (right != 0u && left > std::numeric_limits<uint64_t>::max() / right)
    return false;
  result = left * right;
  return true;
}

bool checkedI32(const int64_t value, int32_t &result)
{
  if (value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max())
    return false;
  result = static_cast<int32_t>(value);
  return true;
}

void hashU64(uint64_t &hash, const uint64_t value)
{
  for (uint32_t byte = 0u; byte < 8u; ++byte)
  {
    hash ^= (value >> (byte * 8u)) & 0xffu;
    hash *= kFnvPrime;
  }
}

std::pair<uint32_t, bool> faceAxis(const TerrainGPUChunkFace face)
{
  switch (face)
  {
  case TerrainGPUChunkFace::NegativeX: return {0u, false};
  case TerrainGPUChunkFace::PositiveX: return {0u, true};
  case TerrainGPUChunkFace::NegativeY: return {1u, false};
  case TerrainGPUChunkFace::PositiveY: return {1u, true};
  case TerrainGPUChunkFace::NegativeZ: return {2u, false};
  case TerrainGPUChunkFace::PositiveZ: return {2u, true};
  }
  return {0u, false};
}

} // namespace

bool TerrainGPUChunkKey::operator==(const TerrainGPUChunkKey &other) const
{
  return worldBundleRevision == other.worldBundleRevision &&
      chunkX == other.chunkX && chunkY == other.chunkY &&
      chunkZ == other.chunkZ && clipmapLOD == other.clipmapLOD &&
      generatorRevision == other.generatorRevision &&
      terrainSlot == other.terrainSlot &&
      expectedGeneration == other.expectedGeneration;
}

bool TerrainGPUChunkKey::operator<(const TerrainGPUChunkKey &other) const
{
  return std::tie(
             worldBundleRevision, chunkX, chunkY, chunkZ, clipmapLOD,
             generatorRevision, terrainSlot, expectedGeneration) <
      std::tie(
             other.worldBundleRevision, other.chunkX, other.chunkY,
             other.chunkZ, other.clipmapLOD, other.generatorRevision,
             other.terrainSlot, other.expectedGeneration);
}

int64_t terrainGPUFloorDivide(
    const int64_t numerator, const int64_t positiveDenominator)
{
  if (positiveDenominator <= 0)
    return 0;
  const int64_t quotient = numerator / positiveDenominator;
  const int64_t remainder = numerator % positiveDenominator;
  return remainder < 0 ? quotient - 1 : quotient;
}

bool terrainGPUChunkShapeCounts(
    const TerrainGPUChunkShape &shape,
    TerrainGPUChunkShapeCounts &counts,
    std::string *error)
{
  counts = {};
  if (shape.bricksX == 0u || shape.bricksY == 0u ||
      shape.bricksZ == 0u || shape.brickSide == 0u)
  {
    setError(error, "terrain GPU chunk shape dimensions must be nonzero");
    return false;
  }
  if ((shape.brickSide & (shape.brickSide - 1u)) != 0u)
  {
    setError(error, "terrain GPU brick side must be a power of two");
    return false;
  }

  const uint64_t voxelsX = uint64_t{shape.bricksX} * shape.brickSide;
  const uint64_t voxelsY = uint64_t{shape.bricksY} * shape.brickSide;
  const uint64_t voxelsZ = uint64_t{shape.bricksZ} * shape.brickSide;
  uint64_t bricksXY = 0u;
  uint64_t bricks = 0u;
  uint64_t cellsPerBrickXY = 0u;
  uint64_t cellsPerBrick = 0u;
  uint64_t denseCells = 0u;
  uint64_t columns = 0u;
  const bool productsFit =
      checkedMultiply(shape.bricksX, shape.bricksY, bricksXY) &&
      checkedMultiply(bricksXY, shape.bricksZ, bricks) &&
      checkedMultiply(shape.brickSide, shape.brickSide, cellsPerBrickXY) &&
      checkedMultiply(cellsPerBrickXY, shape.brickSide, cellsPerBrick) &&
      checkedMultiply(bricks, cellsPerBrick, denseCells) &&
      checkedMultiply(voxelsX, voxelsZ, columns);
  if (voxelsX > std::numeric_limits<int32_t>::max() ||
      voxelsY > std::numeric_limits<int32_t>::max() ||
      voxelsZ > std::numeric_limits<int32_t>::max() ||
      !productsFit || !checkedU32(voxelsX, counts.voxelDimensions[0]) ||
      !checkedU32(voxelsY, counts.voxelDimensions[1]) ||
      !checkedU32(voxelsZ, counts.voxelDimensions[2]) ||
      !checkedU32(bricks, counts.denseBrickCount) ||
      !checkedU32(denseCells, counts.denseVoxelCount) ||
      !checkedU32(columns, counts.columnCount))
  {
    counts = {};
    setError(error, "terrain GPU chunk shape exceeds 32-bit shader indexing");
    return false;
  }
  return true;
}

bool terrainGPUChunkBounds(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    TerrainGPUChunkBounds &bounds,
    std::string *error)
{
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(shape, counts, error))
    return false;
  const std::array<int32_t, 3u> chunks{key.chunkX, key.chunkY, key.chunkZ};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t minimum = int64_t{chunks[axis]} *
        counts.voxelDimensions[axis];
    const int64_t maximum = minimum + counts.voxelDimensions[axis];
    if (!checkedI32(minimum, bounds.minimum[axis]) ||
        !checkedI32(maximum, bounds.maximum[axis]))
    {
      bounds = {};
      setError(error, "terrain GPU chunk bounds exceed signed shader coordinates");
      return false;
    }
  }
  return true;
}

bool terrainGPUWorldToCanonicalVoxel(
    const std::array<double, 3u> &worldPositionMetres,
    const std::array<double, 3u> &worldOriginMetres,
    const double finestVoxelSizeMetres,
    TerrainGPUCanonicalVoxel &voxel,
    std::string *error)
{
  voxel = {};
  if (!(finestVoxelSizeMetres > 0.0) ||
      !std::isfinite(finestVoxelSizeMetres))
  {
    setError(error, "terrain GPU finest voxel size must be finite and positive");
    return false;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(worldPositionMetres[axis]) ||
        !std::isfinite(worldOriginMetres[axis]))
    {
      setError(error, "terrain GPU world coordinate must be finite");
      return false;
    }
    const long double scaled =
        (static_cast<long double>(worldPositionMetres[axis]) -
         static_cast<long double>(worldOriginMetres[axis])) /
        static_cast<long double>(finestVoxelSizeMetres);
    const long double quantized = std::floor(scaled);
    if (quantized < static_cast<long double>(
                        std::numeric_limits<int64_t>::min()) ||
        quantized > static_cast<long double>(
                        std::numeric_limits<int64_t>::max()))
    {
      voxel = {};
      setError(error, "terrain GPU canonical world coordinate overflows int64");
      return false;
    }
    voxel.coordinate[axis] = static_cast<int64_t>(quantized);
  }
  return true;
}

bool terrainGPUCanonicalToClipmapVoxel(
    const TerrainGPUCanonicalVoxel &canonical,
    const uint16_t clipmapLOD,
    TerrainGPUClipmapVoxel &voxel,
    std::string *error)
{
  voxel = {};
  if (clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU clipmap LOD exceeds the signed shader domain");
    return false;
  }
  const int64_t scale = int64_t{1} << clipmapLOD;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t value = terrainGPUFloorDivide(
        canonical.coordinate[axis], scale);
    if (!checkedI32(value, voxel.coordinate[axis]))
    {
      voxel = {};
      setError(error, "terrain GPU clipmap voxel overflows int32");
      return false;
    }
  }
  voxel.clipmapLOD = clipmapLOD;
  return true;
}

bool terrainGPUClipmapVoxelMinimum(
    const TerrainGPUClipmapVoxel &voxel,
    TerrainGPUCanonicalVoxel &canonical,
    std::string *error)
{
  canonical = {};
  if (voxel.clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU clipmap LOD exceeds the signed shader domain");
    return false;
  }
  const int64_t scale = int64_t{1} << voxel.clipmapLOD;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    canonical.coordinate[axis] = int64_t{voxel.coordinate[axis]} * scale;
  return true;
}

bool terrainGPUClipmapVoxelWorldBounds(
    const TerrainGPUClipmapVoxel &voxel,
    const std::array<double, 3u> &worldOriginMetres,
    const double finestVoxelSizeMetres,
    std::array<double, 3u> &minimumMetres,
    std::array<double, 3u> &maximumMetres,
    std::string *error)
{
  minimumMetres = {};
  maximumMetres = {};
  if (!(finestVoxelSizeMetres > 0.0) ||
      !std::isfinite(finestVoxelSizeMetres) ||
      voxel.clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU clipmap world conversion parameters are invalid");
    return false;
  }
  const int64_t scale = int64_t{1} << voxel.clipmapLOD;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(worldOriginMetres[axis]))
    {
      setError(error, "terrain GPU world origin must be finite");
      return false;
    }
    const int64_t canonicalMinimum = int64_t{voxel.coordinate[axis]} * scale;
    const int64_t canonicalMaximum =
        (int64_t{voxel.coordinate[axis]} + 1) * scale;
    minimumMetres[axis] = worldOriginMetres[axis] +
        static_cast<double>(canonicalMinimum) * finestVoxelSizeMetres;
    maximumMetres[axis] = worldOriginMetres[axis] +
        static_cast<double>(canonicalMaximum) * finestVoxelSizeMetres;
    if (!std::isfinite(minimumMetres[axis]) ||
        !std::isfinite(maximumMetres[axis]))
    {
      minimumMetres = {};
      maximumMetres = {};
      setError(error, "terrain GPU clipmap world bounds are not finite");
      return false;
    }
  }
  return true;
}

bool terrainGPUColumnTileForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    const uint32_t tileVoxelsX,
    const uint32_t tileVoxelsZ,
    TerrainGPUColumnTileCoordinate &tile,
    std::string *error)
{
  tile = {};
  if (voxel.clipmapLOD > kMaximumShaderClipmapLOD ||
      tileVoxelsX == 0u || tileVoxelsZ == 0u ||
      tileVoxelsX > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      tileVoxelsZ > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
  {
    setError(error, "terrain GPU column tile dimensions are invalid");
    return false;
  }
  tile.tileX = static_cast<int32_t>(terrainGPUFloorDivide(
      voxel.coordinate[0], tileVoxelsX));
  tile.tileZ = static_cast<int32_t>(terrainGPUFloorDivide(
      voxel.coordinate[2], tileVoxelsZ));
  tile.clipmapLOD = voxel.clipmapLOD;
  return true;
}

bool terrainGPUColumnTileVoxelBounds(
    const TerrainGPUColumnTileCoordinate &tile,
    const uint32_t tileVoxelsX,
    const uint32_t tileVoxelsZ,
    std::array<int32_t, 2u> &minimumXZ,
    std::array<int32_t, 2u> &maximumXZ,
    std::string *error)
{
  minimumXZ = {};
  maximumXZ = {};
  if (tileVoxelsX == 0u || tileVoxelsZ == 0u ||
      tileVoxelsX > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      tileVoxelsZ > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
  {
    setError(error, "terrain GPU column tile dimensions are invalid");
    return false;
  }
  const std::array<int32_t, 2u> coordinates{tile.tileX, tile.tileZ};
  const std::array<uint32_t, 2u> dimensions{tileVoxelsX, tileVoxelsZ};
  for (uint32_t axis = 0u; axis < 2u; ++axis)
  {
    const int64_t minimum = int64_t{coordinates[axis]} * dimensions[axis];
    const int64_t maximum = minimum + dimensions[axis];
    if (!checkedI32(minimum, minimumXZ[axis]) ||
        !checkedI32(maximum, maximumXZ[axis]))
    {
      minimumXZ = {};
      maximumXZ = {};
      setError(error, "terrain GPU column tile bounds exceed signed shader coordinates");
      return false;
    }
  }
  return true;
}

bool terrainGPUChunkKeyForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUChunkKey &identity,
    TerrainGPUChunkKey &key,
    std::string *error)
{
  TerrainGPUChunkShapeCounts counts{};
  if (voxel.clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU voxel clipmap LOD exceeds the shader domain");
    return false;
  }
  if (!terrainGPUChunkShapeCounts(shape, counts, error))
    return false;
  key = identity;
  key.chunkX = static_cast<int32_t>(terrainGPUFloorDivide(
      voxel.coordinate[0], counts.voxelDimensions[0]));
  key.chunkY = static_cast<int32_t>(terrainGPUFloorDivide(
      voxel.coordinate[1], counts.voxelDimensions[1]));
  key.chunkZ = static_cast<int32_t>(terrainGPUFloorDivide(
      voxel.coordinate[2], counts.voxelDimensions[2]));
  key.clipmapLOD = voxel.clipmapLOD;
  return true;
}

bool terrainGPUBrickForVoxel(
    const TerrainGPUClipmapVoxel &voxel,
    const uint32_t brickSide,
    TerrainGPUBrickCoordinate &brick,
    std::string *error)
{
  brick = {};
  if (voxel.clipmapLOD > kMaximumShaderClipmapLOD || brickSide == 0u ||
      brickSide > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
  {
    setError(error, "terrain GPU brick side is invalid");
    return false;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    brick.coordinate[axis] = static_cast<int32_t>(terrainGPUFloorDivide(
        voxel.coordinate[axis], brickSide));
  brick.clipmapLOD = voxel.clipmapLOD;
  return true;
}

bool terrainGPUBrickVoxelBounds(
    const TerrainGPUBrickCoordinate &brick,
    const uint32_t brickSide,
    TerrainGPUChunkBounds &bounds,
    std::string *error)
{
  bounds = {};
  if (brickSide == 0u ||
      brickSide > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
  {
    setError(error, "terrain GPU brick side is invalid");
    return false;
  }
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t minimum = int64_t{brick.coordinate[axis]} * brickSide;
    const int64_t maximum = minimum + brickSide;
    if (!checkedI32(minimum, bounds.minimum[axis]) ||
        !checkedI32(maximum, bounds.maximum[axis]))
    {
      bounds = {};
      setError(error, "terrain GPU brick bounds exceed signed shader coordinates");
      return false;
    }
  }
  return true;
}

bool terrainGPUCanonicalChunkBounds(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    std::array<int64_t, 3u> &minimum,
    std::array<int64_t, 3u> &maximum,
    std::string *error)
{
  minimum = {};
  maximum = {};
  TerrainGPUChunkBounds bounds{};
  if (!terrainGPUChunkBounds(key, shape, bounds, error))
    return false;
  if (key.clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU clipmap LOD exceeds the signed shader domain");
    return false;
  }
  const int64_t scale = int64_t{1} << key.clipmapLOD;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    minimum[axis] = int64_t{bounds.minimum[axis]} * scale;
    maximum[axis] = int64_t{bounds.maximum[axis]} * scale;
  }
  return true;
}

bool terrainGPUChunkContainsVoxel(
    const TerrainGPUChunkBounds &bounds,
    const std::array<int32_t, 3u> &voxel)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (voxel[axis] < bounds.minimum[axis] ||
        voxel[axis] >= bounds.maximum[axis])
      return false;
  return true;
}

bool terrainGPUChunkBoundsOverlap(
    const TerrainGPUChunkBounds &left,
    const TerrainGPUChunkBounds &right)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (left.maximum[axis] <= right.minimum[axis] ||
        right.maximum[axis] <= left.minimum[axis])
      return false;
  return true;
}

bool terrainGPUChunkOwnsCanonicalPoint(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUCanonicalVoxel &point,
    std::string *error)
{
  std::array<int64_t, 3u> minimum{};
  std::array<int64_t, 3u> maximum{};
  if (!terrainGPUCanonicalChunkBounds(
          key, shape, minimum, maximum, error))
    return false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (point.coordinate[axis] < minimum[axis] ||
        point.coordinate[axis] >= maximum[axis])
      return false;
  return true;
}

bool terrainGPUColumnRunIntersectsChunk(
    const TerrainGPUColumnRun &run,
    const TerrainGPUChunkBounds &bounds)
{
  if (run.topY < run.firstSolidY || run.topY ==
          std::numeric_limits<int32_t>::max())
    return false;
  const int64_t runMaximum = int64_t{run.topY} + 1;
  return int64_t{run.firstSolidY} < bounds.maximum[1] &&
      runMaximum > bounds.minimum[1];
}

bool terrainGPUEnumerateVerticalChunks(
    const TerrainGPUChunkKey &horizontalIdentity,
    const TerrainGPUChunkShape &shape,
    const std::vector<TerrainGPUColumnRun> &columns,
    std::vector<TerrainGPUChunkKey> &chunks,
    std::string *error)
{
  chunks.clear();
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(shape, counts, error))
    return false;
  bool found = false;
  int32_t minimumY = std::numeric_limits<int32_t>::max();
  int64_t maximumYExclusive = std::numeric_limits<int64_t>::min();
  for (const auto &column : columns)
  {
    if (column.topY < column.firstSolidY)
      continue;
    if (column.topY == std::numeric_limits<int32_t>::max())
    {
      setError(error, "terrain GPU column top cannot form a half-open bound");
      return false;
    }
    found = true;
    minimumY = std::min(minimumY, column.firstSolidY);
    maximumYExclusive = std::max(
        maximumYExclusive, int64_t{column.topY} + 1);
  }
  if (!found)
    return true;

  const int64_t height = counts.voxelDimensions[1];
  const int64_t firstChunk = terrainGPUFloorDivide(minimumY, height);
  const int64_t lastChunk = terrainGPUFloorDivide(
      maximumYExclusive - 1, height);
  if (firstChunk < std::numeric_limits<int32_t>::min() ||
      lastChunk > std::numeric_limits<int32_t>::max())
  {
    setError(error, "terrain GPU vertical stack exceeds signed chunk coordinates");
    return false;
  }
  const uint64_t chunkCount = static_cast<uint64_t>(
      lastChunk - firstChunk + 1);
  if (chunkCount > std::numeric_limits<uint32_t>::max() ||
      chunkCount > std::numeric_limits<size_t>::max())
  {
    setError(error, "terrain GPU vertical stack exceeds enumerable job count");
    return false;
  }
  chunks.reserve(static_cast<size_t>(chunkCount));
  for (int64_t chunkY = firstChunk; chunkY <= lastChunk; ++chunkY)
  {
    TerrainGPUChunkKey key = horizontalIdentity;
    key.chunkY = static_cast<int32_t>(chunkY);
    TerrainGPUChunkBounds bounds{};
    if (!terrainGPUChunkBounds(key, shape, bounds, error))
    {
      chunks.clear();
      return false;
    }
    chunks.push_back(key);
  }
  return true;
}

bool terrainGPUEnumerateDenseBrickCandidates(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    std::vector<TerrainGPUBrickCandidate> &candidates,
    std::string *error)
{
  candidates.clear();
  TerrainGPUChunkShapeCounts counts{};
  TerrainGPUChunkBounds bounds{};
  if (!terrainGPUChunkShapeCounts(shape, counts, error) ||
      !terrainGPUChunkBounds(key, shape, bounds, error))
    return false;
  candidates.reserve(counts.denseBrickCount);
  for (uint32_t z = 0u; z < shape.bricksZ; ++z)
    for (uint32_t y = 0u; y < shape.bricksY; ++y)
      for (uint32_t x = 0u; x < shape.bricksX; ++x)
      {
        TerrainGPUBrickCandidate candidate{};
        candidate.localBrick = {x, y, z};
        candidate.linearIndex =
            (z * uint32_t{shape.bricksY} + y) * shape.bricksX + x;
        const std::array<uint32_t, 3u> local{x, y, z};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          candidate.minimumVoxel[axis] = bounds.minimum[axis] +
              static_cast<int32_t>(local[axis] * shape.brickSide);
          candidate.canonicalBrick[axis] = static_cast<int32_t>(
              terrainGPUFloorDivide(
                  candidate.minimumVoxel[axis], shape.brickSide));
        }
        candidates.push_back(candidate);
      }
  return true;
}

bool terrainGPUBoundarySignatureForChunk(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUChunkFace face,
    const std::vector<TerrainGPUBoundarySample> &samples,
    TerrainGPUBoundarySignature &signature,
    std::string *error)
{
  signature = {};
  std::array<int64_t, 3u> minimum{};
  std::array<int64_t, 3u> maximum{};
  if (!terrainGPUCanonicalChunkBounds(
          key, shape, minimum, maximum, error))
    return false;
  const auto [axis, positive] = faceAxis(face);
  const int64_t plane = positive ? maximum[axis] : minimum[axis];
  std::array<uint32_t, 2u> tangents{};
  uint32_t tangentIndex = 0u;
  for (uint32_t candidate = 0u; candidate < 3u; ++candidate)
    if (candidate != axis)
      tangents[tangentIndex++] = candidate;

  std::vector<TerrainGPUBoundarySample> ordered = samples;
  if (ordered.size() > std::numeric_limits<uint32_t>::max())
  {
    setError(error, "terrain GPU boundary signature has too many samples");
    return false;
  }
  for (const auto &sample : ordered)
  {
    if (sample.canonicalCoordinate[axis] != plane)
    {
      setError(error, "terrain GPU boundary sample is not on the selected face");
      return false;
    }
    for (const uint32_t tangent : tangents)
      if (sample.canonicalCoordinate[tangent] < minimum[tangent] ||
          sample.canonicalCoordinate[tangent] > maximum[tangent])
      {
        setError(error, "terrain GPU boundary sample is outside the face extent");
        return false;
      }
  }
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto &left, const auto &right)
      {
        return std::tie(
                   left.canonicalCoordinate, left.value, left.flags) <
            std::tie(
                   right.canonicalCoordinate, right.value, right.flags);
      });

  uint64_t hash = kFnvOffset;
  hashU64(hash, axis);
  hashU64(hash, static_cast<uint64_t>(plane));
  for (const uint32_t tangent : tangents)
  {
    hashU64(hash, static_cast<uint64_t>(minimum[tangent]));
    hashU64(hash, static_cast<uint64_t>(maximum[tangent]));
  }
  hashU64(hash, ordered.size());
  for (const auto &sample : ordered)
  {
    for (const int64_t coordinate : sample.canonicalCoordinate)
      hashU64(hash, static_cast<uint64_t>(coordinate));
    hashU64(hash, static_cast<uint32_t>(sample.value));
    hashU64(hash, sample.flags);
  }
  signature.hash = hash;
  signature.canonicalPlane = plane;
  signature.sampleCount = static_cast<uint32_t>(ordered.size());
  signature.axis = axis;
  return true;
}

TerrainGPUChunkKeyGPU terrainGPUChunkKeyGPU(const TerrainGPUChunkKey &key)
{
  return {
    key.worldBundleRevision,
    key.chunkX,
    key.chunkY,
    key.chunkZ,
    uint32_t{key.clipmapLOD} |
        (uint32_t{key.generatorRevision} << 16u),
    key.terrainSlot,
    key.expectedGeneration,
  };
}

bool terrainGPUChunkJobGPU(
    const TerrainGPUChunkKey &key,
    const TerrainGPUChunkShape &shape,
    const TerrainGPUJobStage stage,
    const uint32_t stageItemCount,
    TerrainGPUChunkJobGPU &job,
    std::string *error)
{
  job = {};
  if (key.clipmapLOD > kMaximumShaderClipmapLOD)
  {
    setError(error, "terrain GPU job clipmap LOD exceeds the shader domain");
    return false;
  }
  TerrainGPUChunkBounds bounds{};
  if (!terrainGPUChunkBounds(key, shape, bounds, error))
    return false;
  job.key = terrainGPUChunkKeyGPU(key);
  job.bounds = bounds;
  job.bricksX = shape.bricksX;
  job.bricksY = shape.bricksY;
  job.bricksZ = shape.bricksZ;
  job.brickSide = shape.brickSide;
  job.stage = static_cast<uint32_t>(stage);
  job.stageItemCount = stageItemCount;
  return true;
}

} // namespace voxel::svraster
