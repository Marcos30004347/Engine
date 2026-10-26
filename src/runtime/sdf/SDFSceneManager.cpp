#include "sdf/SDFSceneManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>

namespace sdf
{
namespace
{

constexpr uint32_t kInvalid = UINT32_MAX;
constexpr uint32_t kTraceGridResolution = VSDF_TRACE_GRID_RESOLUTION;

float axis(const math::Vec3f &v, uint32_t i)
{
  return v[i];
}

math::Vec3f minVec(const math::Vec3f &a, const math::Vec3f &b)
{
  return {std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2])};
}

math::Vec3f maxVec(const math::Vec3f &a, const math::Vec3f &b)
{
  return {std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])};
}

math::Vec3f boundsExtent(const SDFBounds &bounds)
{
  return bounds.max - bounds.min;
}

math::Vec3f boundsCenter(const SDFBounds &bounds)
{
  return (bounds.min + bounds.max) * 0.5f;
}

float maxExtent(const SDFBounds &bounds)
{
  const math::Vec3f e = boundsExtent(bounds);
  return std::max(e[0], std::max(e[1], e[2]));
}

float volume(const SDFBounds &bounds)
{
  const math::Vec3f e = boundsExtent(bounds);
  return std::max(e[0], 0.0f) * std::max(e[1], 0.0f) * std::max(e[2], 0.0f);
}

bool validBounds(const SDFBounds &bounds)
{
  return bounds.max[0] >= bounds.min[0] && bounds.max[1] >= bounds.min[1] && bounds.max[2] >= bounds.min[2];
}

SDFBounds makeBounds(const float minV[3], const float maxV[3])
{
  return {{minV[0], minV[1], minV[2]}, {maxV[0], maxV[1], maxV[2]}};
}

SDFBounds emptyBounds()
{
  const float hi = std::numeric_limits<float>::max();
  const float lo = std::numeric_limits<float>::lowest();
  return {{hi, hi, hi}, {lo, lo, lo}};
}

void includeBounds(SDFBounds &bounds, const SDFBounds &other)
{
  bounds.min = minVec(bounds.min, other.min);
  bounds.max = maxVec(bounds.max, other.max);
}

SDFBounds makeCubeBounds(SDFBounds bounds)
{
  const math::Vec3f center = boundsCenter(bounds);
  const float halfExtent = std::max(maxExtent(bounds) * 0.5f, 1.0e-3f);
  const math::Vec3f h(halfExtent, halfExtent, halfExtent);
  return {center - h, center + h};
}

SDFBounds childBounds(const SDFBounds &bounds, uint32_t child)
{
  const math::Vec3f center = boundsCenter(bounds);
  SDFBounds out = bounds;
  for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
  {
    if (((child >> axisIndex) & 1u) != 0u)
      out.min[axisIndex] = center[axisIndex];
    else
      out.max[axisIndex] = center[axisIndex];
  }
  return out;
}

SDFBounds brickCellBounds(const SDFBounds &bounds, uint32_t cell)
{
  const uint32_t x = cell % VSDF_BRICK_RESOLUTION;
  const uint32_t y = (cell / VSDF_BRICK_RESOLUTION) % VSDF_BRICK_RESOLUTION;
  const uint32_t z = cell / (VSDF_BRICK_RESOLUTION * VSDF_BRICK_RESOLUTION);
  const math::Vec3f e = boundsExtent(bounds);
  SDFBounds out;
  out.min = {
      bounds.min[0] + e[0] * static_cast<float>(x) / static_cast<float>(VSDF_BRICK_RESOLUTION),
      bounds.min[1] + e[1] * static_cast<float>(y) / static_cast<float>(VSDF_BRICK_RESOLUTION),
      bounds.min[2] + e[2] * static_cast<float>(z) / static_cast<float>(VSDF_BRICK_RESOLUTION)};
  out.max = {
      bounds.min[0] + e[0] * static_cast<float>(x + 1u) / static_cast<float>(VSDF_BRICK_RESOLUTION),
      bounds.min[1] + e[1] * static_cast<float>(y + 1u) / static_cast<float>(VSDF_BRICK_RESOLUTION),
      bounds.min[2] + e[2] * static_cast<float>(z + 1u) / static_cast<float>(VSDF_BRICK_RESOLUTION)};
  return out;
}

bool overlaps(const SDFBounds &a, const SDFBounds &b)
{
  return a.min[0] <= b.max[0] && a.max[0] >= b.min[0] &&
         a.min[1] <= b.max[1] && a.max[1] >= b.min[1] &&
         a.min[2] <= b.max[2] && a.max[2] >= b.min[2];
}

float overlapVolume(const SDFBounds &a, const SDFBounds &b)
{
  SDFBounds o;
  o.min = maxVec(a.min, b.min);
  o.max = minVec(a.max, b.max);
  return volume(o);
}

bool rayBounds(const SDFRay &ray, const SDFBounds &bounds, float &outMin, float &outMax)
{
  float tMin = ray.tMin;
  float tMax = ray.tMax;
  for (uint32_t i = 0u; i < 3u; ++i)
  {
    const float o = axis(ray.origin, i);
    const float d = axis(ray.direction, i);
    if (std::fabs(d) < 1.0e-8f)
    {
      if (o < axis(bounds.min, i) || o > axis(bounds.max, i))
        return false;
      continue;
    }
    const float invD = 1.0f / d;
    float t0 = (axis(bounds.min, i) - o) * invD;
    float t1 = (axis(bounds.max, i) - o) * invD;
    if (t0 > t1)
      std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMax < tMin)
      return false;
  }
  outMin = tMin;
  outMax = tMax;
  return true;
}

bool boundsApproximatelyEqual(const SDFBounds &a, const SDFBounds &b, float epsilon = 1.0e-5f)
{
  for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
  {
    if (std::fabs(a.min[axisIndex] - b.min[axisIndex]) > epsilon ||
        std::fabs(a.max[axisIndex] - b.max[axisIndex]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

bool matricesApproximatelyEqual(const float a[16], const math::Mat4f &b, float epsilon = 1.0e-5f)
{
  for (uint32_t index = 0u; index < 16u; ++index)
  {
    if (std::fabs(a[index] - b.data[index]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

math::Vec3f transformPoint(const float m[16], const math::Vec3f &p)
{
  return {
      m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
      m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
      m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

math::Vec3f transformVector(const float m[16], const math::Vec3f &v)
{
  return {
      m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
      m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
      m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}

void copyMat(std::array<float, 16> &dst, const float src[16])
{
  std::memcpy(dst.data(), src, sizeof(float) * 16u);
}

void copyMat(float dst[16], const math::Mat4f &src)
{
  std::memcpy(dst, src.data, sizeof(float) * 16u);
}

SDFBounds transformBounds(const SDFBounds &localBounds, const math::Mat4f &transform)
{
  SDFBounds transformed = emptyBounds();
  for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
  {
    const math::Vec3f localCorner(
        (cornerIndex & 1u) ? localBounds.max[0] : localBounds.min[0],
        (cornerIndex & 2u) ? localBounds.max[1] : localBounds.min[1],
        (cornerIndex & 4u) ? localBounds.max[2] : localBounds.min[2]);
    const math::Vec4f worldCorner = transform * math::Vec4f(localCorner[0], localCorner[1], localCorner[2], 1.0f);
    transformed.min = minVec(transformed.min, math::Vec3f(worldCorner[0], worldCorner[1], worldCorner[2]));
    transformed.max = maxVec(transformed.max, math::Vec3f(worldCorner[0], worldCorner[1], worldCorner[2]));
  }
  return transformed;
}

float conservativeTransformScale(const math::Mat4f &transform)
{
  const float scaleX = math::Vec3f(transform.data[0], transform.data[1], transform.data[2]).length();
  const float scaleY = math::Vec3f(transform.data[4], transform.data[5], transform.data[6]).length();
  const float scaleZ = math::Vec3f(transform.data[8], transform.data[9], transform.data[10]).length();
  return std::max(std::max(scaleX, scaleY), std::max(scaleZ, 1.0e-6f));
}

uint32_t clampGridResolution(uint32_t resolution)
{
  return std::max(1u, std::min(resolution, 128u));
}

uint32_t flattenGridCell(uint32_t x, uint32_t y, uint32_t z, uint32_t resolution)
{
  return x + y * resolution + z * resolution * resolution;
}

uint32_t gridCellOverlapCount(const SDFBounds &sceneBounds, const SDFBounds &bounds, uint32_t resolution)
{
  if (!validBounds(sceneBounds) || !validBounds(bounds))
    return 0u;

  const math::Vec3f extent = boundsExtent(sceneBounds);
  std::array<uint32_t, 3u> minCell{};
  std::array<uint32_t, 3u> maxCell{};
  for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
  {
    if (extent[axisIndex] <= 1.0e-8f)
      return 0u;
    const float scale = static_cast<float>(resolution) / extent[axisIndex];
    const int minIndex = static_cast<int>(std::floor((bounds.min[axisIndex] - sceneBounds.min[axisIndex]) * scale));
    const int maxIndex = static_cast<int>(std::floor((bounds.max[axisIndex] - sceneBounds.min[axisIndex]) * scale));
    minCell[axisIndex] = static_cast<uint32_t>(std::clamp(minIndex, 0, static_cast<int>(resolution) - 1));
    maxCell[axisIndex] = static_cast<uint32_t>(std::clamp(maxIndex, 0, static_cast<int>(resolution) - 1));
  }
  return (maxCell[0] - minCell[0] + 1u) * (maxCell[1] - minCell[1] + 1u) * (maxCell[2] - minCell[2] + 1u);
}

SDFBounds traceRootPageLocalBounds(const VSDFInstancePayloadHeader &header, uint32_t rootPageFlat)
{
  const uint32_t rootX = rootPageFlat % VSDF_TRACE_ROOT_RESOLUTION;
  const uint32_t rootY = (rootPageFlat / VSDF_TRACE_ROOT_RESOLUTION) % VSDF_TRACE_ROOT_RESOLUTION;
  const uint32_t rootZ = rootPageFlat / (VSDF_TRACE_ROOT_RESOLUTION * VSDF_TRACE_ROOT_RESOLUTION);
  const SDFBounds localBounds = makeBounds(header.localBoundsMin, header.localBoundsMax);
  const math::Vec3f cellSize = (localBounds.max - localBounds.min) * (1.0f / static_cast<float>(VSDF_TRACE_GRID_RESOLUTION));
  const math::Vec3f pageMin = localBounds.min + math::Vec3f(
      static_cast<float>(rootX * VSDF_TRACE_PAGE_RESOLUTION) * cellSize[0],
      static_cast<float>(rootY * VSDF_TRACE_PAGE_RESOLUTION) * cellSize[1],
      static_cast<float>(rootZ * VSDF_TRACE_PAGE_RESOLUTION) * cellSize[2]);
  const math::Vec3f pageExtent = cellSize * static_cast<float>(VSDF_TRACE_PAGE_RESOLUTION);
  return {pageMin, pageMin + pageExtent};
}

uint32_t popcountBefore(const uint32_t words[16], uint32_t bit)
{
  uint32_t count = 0u;
  const uint32_t wordIndex = bit / 32u;
  for (uint32_t i = 0u; i < wordIndex; ++i)
    count += static_cast<uint32_t>(__builtin_popcount(words[i]));
  const uint32_t localBit = bit % 32u;
  if (localBit != 0u)
    count += static_cast<uint32_t>(__builtin_popcount(words[wordIndex] & ((1u << localBit) - 1u)));
  return count;
}

bool hasChild(const VSDFNodeDisk &node, uint32_t cell)
{
  return (node.childMaskWords[cell / 32u] & (1u << (cell % 32u))) != 0u;
}

bool hasValidSurfaceCell(const VSDFNodeDisk &node, uint32_t cell)
{
  return (node.validMaskWords[cell / 32u] & (1u << (cell % 32u))) != 0u;
}

bool hasTraceSurfaceCell(const VSDFNodeDisk &node, uint32_t cell)
{
  return (node.traceMaskWords[cell / 32u] & (1u << (cell % 32u))) != 0u;
}

float decodeSnorm8(uint8_t value)
{
  const int32_t s = static_cast<int32_t>(static_cast<int8_t>(value));
  return std::max(-1.0f, static_cast<float>(s) / 127.0f);
}

uint8_t encodeSnorm8(float value)
{
  const float clamped = std::max(-1.0f, std::min(1.0f, value));
  const int encoded = static_cast<int>(std::round(clamped * 127.0f));
  return static_cast<uint8_t>(static_cast<int8_t>(std::max(-127, std::min(127, encoded))));
}

uint32_t byteLoadWord(const std::vector<uint32_t> &words, uint32_t byteOffset)
{
  const uint32_t wordIndex = byteOffset / 4u;
  const uint32_t byteShift = (byteOffset & 3u) * 8u;
  if (wordIndex >= words.size())
    return 0u;
  return (words[wordIndex] >> byteShift) & 0xffu;
}

float emptyCellAdvance(const math::Vec3f &localPoint, const math::Vec3f &localDirection, const SDFBounds &cellBounds, float voxelSize)
{
  float exitT = std::numeric_limits<float>::infinity();
  for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
  {
    const float direction = localDirection[axisIndex];
    if (std::fabs(direction) <= 1.0e-8f)
      continue;
    const float boundary = direction > 0.0f ? cellBounds.max[axisIndex] : cellBounds.min[axisIndex];
    const float candidate = (boundary - localPoint[axisIndex]) / direction;
    if (candidate > 1.0e-6f)
      exitT = std::min(exitT, candidate);
  }
  if (std::isfinite(exitT))
    return exitT;
  return voxelSize / std::max(localDirection.length(), 1.0e-6f);
}

void appendBytesAsWords(std::vector<uint32_t> &words, const std::vector<uint8_t> &bytes)
{
  const uint32_t oldByteSize = static_cast<uint32_t>(words.size() * sizeof(uint32_t));
  const uint32_t paddedByteSize = ((static_cast<uint32_t>(bytes.size()) + 3u) / 4u) * 4u;
  words.resize(words.size() + paddedByteSize / 4u, 0u);
  uint8_t *dst = reinterpret_cast<uint8_t *>(words.data()) + oldByteSize;
  if (!bytes.empty())
    std::memcpy(dst, bytes.data(), bytes.size());
}

uint32_t ceilCubeRoot(uint32_t value)
{
  uint32_t root = 1u;
  while (root * root * root < std::max(value, 1u))
    ++root;
  return root;
}

void copyBrickToAtlas(
    std::vector<uint8_t> &atlas,
    uint32_t atlasBricksPerAxis,
    uint32_t atlasBrickIndex,
    const uint8_t *src)
{
  const uint32_t atlasResolution = atlasBricksPerAxis * VSDF_BRICK_DISTANCE_RESOLUTION;
  const uint32_t brickX = atlasBrickIndex % atlasBricksPerAxis;
  const uint32_t brickY = (atlasBrickIndex / atlasBricksPerAxis) % atlasBricksPerAxis;
  const uint32_t brickZ = atlasBrickIndex / (atlasBricksPerAxis * atlasBricksPerAxis);
  for (uint32_t z = 0u; z < VSDF_BRICK_DISTANCE_RESOLUTION; ++z)
  {
    for (uint32_t y = 0u; y < VSDF_BRICK_DISTANCE_RESOLUTION; ++y)
    {
      const uint32_t dstX = brickX * VSDF_BRICK_DISTANCE_RESOLUTION;
      const uint32_t dstY = brickY * VSDF_BRICK_DISTANCE_RESOLUTION + y;
      const uint32_t dstZ = brickZ * VSDF_BRICK_DISTANCE_RESOLUTION + z;
      const size_t dstOffset = (static_cast<size_t>(dstZ) * atlasResolution + dstY) * atlasResolution + dstX;
      const size_t srcOffset = (static_cast<size_t>(z) * VSDF_BRICK_DISTANCE_RESOLUTION + y) * VSDF_BRICK_DISTANCE_RESOLUTION;
      std::memcpy(atlas.data() + dstOffset, src + srcOffset, VSDF_BRICK_DISTANCE_RESOLUTION);
    }
  }
}

uint32_t traceGridCellIndex(uint32_t x, uint32_t y, uint32_t z)
{
  return x + y * kTraceGridResolution + z * kTraceGridResolution * kTraceGridResolution;
}

void markTraceGridBounds(std::vector<uint8_t> &grid, const SDFBounds &rootBounds, const SDFBounds &cellBounds)
{
  constexpr uint32_t kTraceGridPaddingCells = 2u;
  const math::Vec3f rootExtent = maxVec(boundsExtent(rootBounds), math::Vec3f(1.0e-8f));
  const auto toGridMin =
      [&](float value, float minValue, float extent) -> uint32_t
  {
    const float u = std::clamp((value - minValue) / extent, 0.0f, 0.999999f);
    return std::min<uint32_t>(kTraceGridResolution - 1u, static_cast<uint32_t>(std::floor(u * static_cast<float>(kTraceGridResolution))));
  };
  const auto toGridMax =
      [&](float value, float minValue, float extent) -> uint32_t
  {
    const float u = std::clamp((value - minValue) / extent, 0.0f, 1.0f);
    const float scaled = u * static_cast<float>(kTraceGridResolution);
    const int32_t index = static_cast<int32_t>(std::ceil(scaled)) - 1;
    return static_cast<uint32_t>(std::clamp(index, 0, static_cast<int32_t>(kTraceGridResolution - 1u)));
  };

  const uint32_t gridMinX = toGridMin(cellBounds.min[0], rootBounds.min[0], rootExtent[0]);
  const uint32_t gridMinY = toGridMin(cellBounds.min[1], rootBounds.min[1], rootExtent[1]);
  const uint32_t gridMinZ = toGridMin(cellBounds.min[2], rootBounds.min[2], rootExtent[2]);
  const uint32_t minX = gridMinX > kTraceGridPaddingCells ? gridMinX - kTraceGridPaddingCells : 0u;
  const uint32_t minY = gridMinY > kTraceGridPaddingCells ? gridMinY - kTraceGridPaddingCells : 0u;
  const uint32_t minZ = gridMinZ > kTraceGridPaddingCells ? gridMinZ - kTraceGridPaddingCells : 0u;
  const uint32_t maxX = std::min(toGridMax(cellBounds.max[0], rootBounds.min[0], rootExtent[0]) + kTraceGridPaddingCells, kTraceGridResolution - 1u);
  const uint32_t maxY = std::min(toGridMax(cellBounds.max[1], rootBounds.min[1], rootExtent[1]) + kTraceGridPaddingCells, kTraceGridResolution - 1u);
  const uint32_t maxZ = std::min(toGridMax(cellBounds.max[2], rootBounds.min[2], rootExtent[2]) + kTraceGridPaddingCells, kTraceGridResolution - 1u);

  for (uint32_t z = minZ; z <= maxZ; ++z)
    for (uint32_t y = minY; y <= maxY; ++y)
      for (uint32_t x = minX; x <= maxX; ++x)
        grid[traceGridCellIndex(x, y, z)] = 255u;
}

void buildTraceGridNode(
    const VSDFDecodedInstance &payload,
    uint32_t nodeIndex,
    const SDFBounds &nodeBounds,
    const SDFBounds &rootBounds,
    std::vector<uint8_t> &grid)
{
  if (nodeIndex >= payload.nodes.size())
    return;
  const VSDFNodeDisk &node = payload.nodes[nodeIndex];
  for (uint32_t cell = 0u; cell < VSDF_BRICK_CELL_COUNT; ++cell)
  {
    const uint32_t bit = 1u << (cell % 32u);
    const uint32_t word = cell / 32u;
    const bool isTraceCell = (node.traceMaskWords[word] & bit) != 0u;
    const bool hasChildCell = node.firstChild != kInvalid && (node.childMaskWords[word] & bit) != 0u;
    if (!isTraceCell && !hasChildCell)
      continue;

    const SDFBounds childBounds = brickCellBounds(nodeBounds, cell);
    if (isTraceCell)
      markTraceGridBounds(grid, rootBounds, childBounds);
    if (hasChildCell)
      buildTraceGridNode(payload, node.firstChild + popcountBefore(node.childMaskWords, cell), childBounds, rootBounds, grid);
  }
}

void copyTraceGridToAtlas(
    std::vector<uint8_t> &atlas,
    uint32_t atlasGridsPerAxis,
    uint32_t gridIndex,
    const std::vector<uint8_t> &grid)
{
  if (atlasGridsPerAxis == 0u || grid.empty())
    return;
  const uint32_t atlasResolution = atlasGridsPerAxis * kTraceGridResolution;
  const uint32_t gridX = gridIndex % atlasGridsPerAxis;
  const uint32_t gridY = (gridIndex / atlasGridsPerAxis) % atlasGridsPerAxis;
  const uint32_t gridZ = gridIndex / (atlasGridsPerAxis * atlasGridsPerAxis);
  for (uint32_t z = 0u; z < kTraceGridResolution; ++z)
  {
    for (uint32_t y = 0u; y < kTraceGridResolution; ++y)
    {
      const uint32_t dstX = gridX * kTraceGridResolution;
      const uint32_t dstY = gridY * kTraceGridResolution + y;
      const uint32_t dstZ = gridZ * kTraceGridResolution + z;
      const size_t dstOffset = (static_cast<size_t>(dstZ) * atlasResolution + dstY) * atlasResolution + dstX;
      const size_t srcOffset = (static_cast<size_t>(z) * kTraceGridResolution + y) * kTraceGridResolution;
      std::memcpy(atlas.data() + dstOffset, grid.data() + srcOffset, kTraceGridResolution);
    }
  }
}

std::vector<uint8_t> traceGridFromSparseTracePages(
    const std::vector<uint32_t> &pageTable,
    const std::vector<VSDFTracePageDisk> &pages,
    const std::vector<uint32_t> &pageMasks)
{
  std::vector<uint8_t> grid(VSDF_TRACE_GRID_CELL_COUNT, 0u);
  if (pageTable.size() != VSDF_TRACE_ROOT_CELL_COUNT)
    return grid;
  for (uint32_t rootZ = 0u; rootZ < VSDF_TRACE_ROOT_RESOLUTION; ++rootZ)
  {
    for (uint32_t rootY = 0u; rootY < VSDF_TRACE_ROOT_RESOLUTION; ++rootY)
    {
      for (uint32_t rootX = 0u; rootX < VSDF_TRACE_ROOT_RESOLUTION; ++rootX)
      {
        const uint32_t rootIndex = rootX + rootY * VSDF_TRACE_ROOT_RESOLUTION + rootZ * VSDF_TRACE_ROOT_RESOLUTION * VSDF_TRACE_ROOT_RESOLUTION;
        const uint32_t pageIndex = pageTable[rootIndex];
        if (pageIndex == VSDF_TRACE_INVALID_BRICK || pageIndex >= pages.size())
          continue;
        const VSDFTracePageDisk &page = pages[pageIndex];
        if (page.maskOffset + VSDF_TRACE_PAGE_MASK_WORD_COUNT > pageMasks.size())
          continue;
        for (uint32_t localFlat = 0u; localFlat < VSDF_TRACE_PAGE_CELL_COUNT; ++localFlat)
        {
          const uint32_t word = localFlat / 32u;
          const uint32_t bit = 1u << (localFlat % 32u);
          if ((pageMasks[page.maskOffset + word] & bit) == 0u)
            continue;
          const uint32_t localX = localFlat % VSDF_TRACE_PAGE_RESOLUTION;
          const uint32_t localY = (localFlat / VSDF_TRACE_PAGE_RESOLUTION) % VSDF_TRACE_PAGE_RESOLUTION;
          const uint32_t localZ = localFlat / (VSDF_TRACE_PAGE_RESOLUTION * VSDF_TRACE_PAGE_RESOLUTION);
          const uint32_t x = rootX * VSDF_TRACE_PAGE_RESOLUTION + localX;
          const uint32_t y = rootY * VSDF_TRACE_PAGE_RESOLUTION + localY;
          const uint32_t z = rootZ * VSDF_TRACE_PAGE_RESOLUTION + localZ;
          grid[traceGridCellIndex(x, y, z)] = 255u;
        }
      }
    }
  }
  return grid;
}

uint32_t flattenTraceAabbCoord(uint32_t x, uint32_t y, uint32_t z, uint32_t dimension)
{
  return x + y * dimension + z * dimension * dimension;
}

void writeTraceAabbBounds(
    std::vector<uint32_t> &tree,
    uint32_t wordOffset,
    uint32_t minX,
    uint32_t minY,
    uint32_t minZ,
    uint32_t maxX,
    uint32_t maxY,
    uint32_t maxZ)
{
  tree[wordOffset + 0u] = minX;
  tree[wordOffset + 1u] = minY;
  tree[wordOffset + 2u] = minZ;
  tree[wordOffset + 3u] = maxX;
  tree[wordOffset + 4u] = maxY;
  tree[wordOffset + 5u] = maxZ;
}

void accumulateTraceGridBounds(
    const std::vector<uint8_t> &grid,
    uint32_t beginX,
    uint32_t beginY,
    uint32_t beginZ,
    uint32_t endX,
    uint32_t endY,
    uint32_t endZ,
    uint32_t &minX,
    uint32_t &minY,
    uint32_t &minZ,
    uint32_t &maxX,
    uint32_t &maxY,
    uint32_t &maxZ,
    bool &any)
{
  for (uint32_t z = beginZ; z < endZ; ++z)
    for (uint32_t y = beginY; y < endY; ++y)
      for (uint32_t x = beginX; x < endX; ++x)
        if (grid[traceGridCellIndex(x, y, z)] != 0u)
        {
          minX = std::min(minX, x * VSDF_BRICK_RESOLUTION);
          minY = std::min(minY, y * VSDF_BRICK_RESOLUTION);
          minZ = std::min(minZ, z * VSDF_BRICK_RESOLUTION);
          maxX = std::max(maxX, (x + 1u) * VSDF_BRICK_RESOLUTION);
          maxY = std::max(maxY, (y + 1u) * VSDF_BRICK_RESOLUTION);
          maxZ = std::max(maxZ, (z + 1u) * VSDF_BRICK_RESOLUTION);
          any = true;
        }
}

std::vector<uint32_t> buildTraceAabbTree(const std::vector<uint8_t> &grid)
{
  std::vector<uint32_t> tree(VSDF_TRACE_AABB_TREE_WORD_COUNT, 0u);
  for (uint32_t i = 0u; i < VSDF_TRACE_AABB_BOTTOM_NODE_COUNT; ++i)
    tree[VSDF_TRACE_AABB_BOTTOM_WORD_OFFSET + i] = 0x00007fffu;

  for (uint32_t bz = 0u; bz < 16u; ++bz)
  {
    for (uint32_t by = 0u; by < 16u; ++by)
    {
      for (uint32_t bx = 0u; bx < 16u; ++bx)
      {
        uint32_t minX = 32u;
        uint32_t minY = 32u;
        uint32_t minZ = 32u;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;
        uint32_t maxZ = 0u;
        bool any = false;
        for (uint32_t z = 0u; z < 4u; ++z)
          for (uint32_t y = 0u; y < 4u; ++y)
            for (uint32_t x = 0u; x < 4u; ++x)
              if (grid[traceGridCellIndex(bx * 4u + x, by * 4u + y, bz * 4u + z)] != 0u)
              {
                minX = std::min(minX, x * VSDF_BRICK_RESOLUTION);
                minY = std::min(minY, y * VSDF_BRICK_RESOLUTION);
                minZ = std::min(minZ, z * VSDF_BRICK_RESOLUTION);
                maxX = std::max(maxX, (x + 1u) * VSDF_BRICK_RESOLUTION - 1u);
                maxY = std::max(maxY, (y + 1u) * VSDF_BRICK_RESOLUTION - 1u);
                maxZ = std::max(maxZ, (z + 1u) * VSDF_BRICK_RESOLUTION - 1u);
                any = true;
              }
        if (any)
        {
          const uint32_t minFlat = flattenTraceAabbCoord(minX, minY, minZ, 32u);
          const uint32_t maxFlat = flattenTraceAabbCoord(maxX, maxY, maxZ, 32u);
          tree[VSDF_TRACE_AABB_BOTTOM_WORD_OFFSET + flattenTraceAabbCoord(bx, by, bz, 16u)] =
              (minFlat & 0x7fffu) | ((maxFlat & 0x7fffu) << 16u);
        }
      }
    }
  }

  for (uint32_t mz = 0u; mz < 4u; ++mz)
    for (uint32_t my = 0u; my < 4u; ++my)
      for (uint32_t mx = 0u; mx < 4u; ++mx)
      {
        uint32_t minX = UINT32_MAX;
        uint32_t minY = UINT32_MAX;
        uint32_t minZ = UINT32_MAX;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;
        uint32_t maxZ = 0u;
        bool any = false;
        accumulateTraceGridBounds(grid, mx * 16u, my * 16u, mz * 16u, (mx + 1u) * 16u, (my + 1u) * 16u, (mz + 1u) * 16u, minX, minY, minZ, maxX, maxY, maxZ, any);
        if (any)
          writeTraceAabbBounds(tree, VSDF_TRACE_AABB_MID_WORD_OFFSET + flattenTraceAabbCoord(mx, my, mz, 4u) * 6u, minX, minY, minZ, maxX, maxY, maxZ);
      }

  uint32_t minX = UINT32_MAX;
  uint32_t minY = UINT32_MAX;
  uint32_t minZ = UINT32_MAX;
  uint32_t maxX = 0u;
  uint32_t maxY = 0u;
  uint32_t maxZ = 0u;
  bool any = false;
  accumulateTraceGridBounds(grid, 0u, 0u, 0u, kTraceGridResolution, kTraceGridResolution, kTraceGridResolution, minX, minY, minZ, maxX, maxY, maxZ, any);
  if (any)
    writeTraceAabbBounds(tree, VSDF_TRACE_AABB_ROOT_WORD_OFFSET, minX, minY, minZ, maxX, maxY, maxZ);
  return tree;
}

} // namespace

SDFSceneManager::SDFSceneManager(rendering::RenderGraph *renderGraph)
    : SDFSceneManager(renderGraph, Settings{})
{
}

SDFSceneManager::SDFSceneManager(rendering::RenderGraph *renderGraph, Settings settings)
    : settings_(settings), renderGraph_(renderGraph)
{
}

SDFSceneManager::~SDFSceneManager()
{
  destroyGPUResources();
}

void SDFSceneManager::setRenderGraph(rendering::RenderGraph *renderGraph)
{
  if (renderGraph_ != renderGraph)
    destroyGPUResources();
  renderGraph_ = renderGraph;
}

bool SDFSceneManager::loadVSDF(const std::string &path, std::string *error)
{
  SDFSceneFile file;
  if (!file.load(path, error))
    return false;
  return buildFromFile(file, error);
}

void SDFSceneManager::clear()
{
  destroyGPUResources();
  instances_.clear();
  blases_.clear();
  tlasNodes_.clear();
  tlasInstanceRefs_.clear();
  gpuInstances_.clear();
  gpuBlases_.clear();
  gpuBlasNodes_.clear();
  gpuBlasNodeChildWordPrefixes_.clear();
  gpuBlasBricks_.clear();
  gpuDistanceWords_.clear();
  gpuTracePageTables_.clear();
  gpuTracePages_.clear();
  gpuTracePageMasks_.clear();
  gpuTraceBrickMaps_.clear();
  gpuDistanceAtlasBytes_.clear();
  gpuDistanceAtlasBricksPerAxis_ = 1u;
  gpuTlasNodes_.clear();
  gpuSceneInfo_ = {};
  rootTlasNode_ = kInvalid;
  worldBounds_ = {};
}

bool SDFSceneManager::syncInstanceTransforms(
    const virtualgeometry::VirtualGeometryScene &scene,
    const virtualgeometry::InstanceId objectInstanceId)
{
  if (objectInstanceId >= scene.objectSparseToDense.size())
    return false;

  const uint32_t denseIndex = scene.objectSparseToDense[objectInstanceId];
  if (denseIndex == virtualgeometry::INVALID_INSTANCE_ID || denseIndex >= scene.objectInstances.size())
    return false;

  return syncInstanceTransforms(scene.instanceManager, scene.objectInstances[denseIndex].sceneInstanceIds);
}

bool SDFSceneManager::syncInstanceTransforms(
    const virtualgeometry::VirtualGeometryScene::InstanceManager &instanceManager,
    const std::vector<virtualgeometry::SceneInstanceId> &sceneInstanceIds)
{
  if (instances_.empty() || sceneInstanceIds.empty())
    return false;

  bool changed = false;
  SDFBounds updatedWorldBounds = emptyBounds();
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instances_.size()); ++instanceIndex)
  {
    RuntimeInstance &instance = instances_[instanceIndex];
    uint32_t shapeIndex = instance.directory.sourceMeshIndex;
    if (shapeIndex >= sceneInstanceIds.size())
      shapeIndex = instanceIndex;
    if (shapeIndex >= sceneInstanceIds.size())
      continue;

    const virtualgeometry::VirtualGeometryInstanceGPUData *sceneInstance =
        instanceManager.getInstance(sceneInstanceIds[shapeIndex]);
    if (sceneInstance == nullptr)
      continue;

    const math::Mat4f &localToWorld = sceneInstance->modelMatrix;
    const math::Mat4f worldToLocal = localToWorld.inverse();
    const SDFBounds worldBounds = transformBounds(instance.localBounds, localToWorld);
    if (!validBounds(worldBounds))
      continue;

    const float conservativeScale = conservativeTransformScale(localToWorld);
    const bool instanceChanged =
        !matricesApproximatelyEqual(instance.header.localToWorld, localToWorld) ||
        !matricesApproximatelyEqual(instance.header.worldToLocal, worldToLocal) ||
        !boundsApproximatelyEqual(instance.worldBounds, worldBounds) ||
        std::fabs(instance.directory.conservativeScale - conservativeScale) > 1.0e-5f;

    if (instanceChanged)
    {
      copyMat(instance.header.localToWorld, localToWorld);
      copyMat(instance.header.worldToLocal, worldToLocal);
      instance.worldBounds = worldBounds;
      instance.directory.conservativeScale = conservativeScale;
      for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
      {
        instance.directory.worldBoundsMin[axisIndex] = worldBounds.min[axisIndex];
        instance.directory.worldBoundsMax[axisIndex] = worldBounds.max[axisIndex];
      }
      changed = true;
    }

    includeBounds(updatedWorldBounds, instance.worldBounds);
  }

  if (!validBounds(updatedWorldBounds))
    return changed;

  updatedWorldBounds = makeCubeBounds(updatedWorldBounds);
  if (!boundsApproximatelyEqual(worldBounds_, updatedWorldBounds))
  {
    worldBounds_ = updatedWorldBounds;
    changed = true;
  }

  if (!changed)
    return false;

  buildTLAS();
  rebuildGPUInstanceTlasArrays();
  if (gpuResources_.isValid())
    uploadDynamicGPUResources();
  return true;
}

bool SDFSceneManager::buildFromFile(const SDFSceneFile &file, std::string *error)
{
  clear();
  const auto &directory = file.instances();
  instances_.reserve(directory.size());
  blases_.reserve(directory.size());
  worldBounds_ = emptyBounds();

  for (uint32_t i = 0u; i < directory.size(); ++i)
  {
    VSDFDecodedInstance decoded;
    if (!file.loadInstance(i, decoded, error))
    {
      clear();
      return false;
    }
    if (decoded.nodes.empty() || decoded.bricks.empty() || decoded.distances.empty())
    {
      if (error != nullptr)
        *error = "VSDF instance payload is missing BLAS nodes, bricks, or distances";
      clear();
      return false;
    }

    RuntimeBlas blas;
    blas.payload = std::move(decoded);
    const uint32_t blasIndex = static_cast<uint32_t>(blases_.size());
    blases_.push_back(std::move(blas));

    RuntimeInstance instance;
    instance.directory = directory[i];
    instance.header = blases_.back().payload.header;
    instance.worldBounds = makeBounds(directory[i].worldBoundsMin, directory[i].worldBoundsMax);
    instance.localBounds = makeBounds(instance.header.localBoundsMin, instance.header.localBoundsMax);
    instance.blasIndex = blasIndex;
    includeBounds(worldBounds_, instance.worldBounds);
    instances_.push_back(instance);
  }

  if (!validBounds(worldBounds_))
    worldBounds_ = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  worldBounds_ = makeCubeBounds(worldBounds_);
  buildTLAS();
  rebuildGPUArrays();
  return true;
}

void SDFSceneManager::buildTLAS()
{
  tlasNodes_.clear();
  tlasInstanceRefs_.clear();
  rootTlasNode_ = kInvalid;
  if (instances_.empty())
    return;

  std::vector<uint32_t> instanceIndices(instances_.size());
  std::iota(instanceIndices.begin(), instanceIndices.end(), 0u);
  rootTlasNode_ = buildTlasNode(worldBounds_, instanceIndices, 0u);

  std::vector<TlasNode> sourceNodes = std::move(tlasNodes_);
  std::vector<uint32_t> oldToNew(sourceNodes.size(), kInvalid);
  tlasNodes_.clear();

  std::function<uint32_t(uint32_t)> flatten = [&](uint32_t oldIndex) -> uint32_t
  {
    const uint32_t newIndex = static_cast<uint32_t>(tlasNodes_.size());
    oldToNew[oldIndex] = newIndex;
    TlasNode node = sourceNodes[oldIndex];
    node.firstChild = kInvalid;
    node.childIndices.fill(kInvalid);
    tlasNodes_.push_back(node);

    uint32_t firstChild = kInvalid;
    uint32_t childOrdinal = 0u;
    for (uint32_t child = 0u; child < 8u; ++child)
    {
      const uint32_t oldChild = sourceNodes[oldIndex].childIndices[child];
      if (oldChild == kInvalid)
        continue;
      const uint32_t newChild = flatten(oldChild);
      if (firstChild == kInvalid)
        firstChild = newChild;
      tlasNodes_[newIndex].childIndices[child] = newChild;
      ++childOrdinal;
    }
    (void)childOrdinal;
    tlasNodes_[newIndex].firstChild = firstChild;
    return newIndex;
  };
  rootTlasNode_ = flatten(rootTlasNode_);
}

uint32_t SDFSceneManager::buildTlasNode(const SDFBounds &bounds, const std::vector<uint32_t> &instanceIndices, uint32_t depth)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(tlasNodes_.size());
  TlasNode node;
  node.bounds = bounds;
  node.depth = depth;
  node.childIndices.fill(kInvalid);
  tlasNodes_.push_back(node);

  const bool stopForDepth = depth >= settings_.tlasMaxDepth;
  const bool stopForCount = instanceIndices.size() <= settings_.tlasLeafTargetInstanceCount;
  const bool stopForSize = maxExtent(bounds) <= 1.0e-4f;
  if (stopForDepth || stopForCount || stopForSize)
  {
    tlasNodes_[nodeIndex].firstInstanceRef = static_cast<uint32_t>(tlasInstanceRefs_.size());
    tlasNodes_[nodeIndex].instanceRefCount = static_cast<uint32_t>(instanceIndices.size());
    tlasNodes_[nodeIndex].flags = 1u;
    tlasInstanceRefs_.insert(tlasInstanceRefs_.end(), instanceIndices.begin(), instanceIndices.end());
    return nodeIndex;
  }

  std::array<std::vector<uint32_t>, 8u> childLists;
  std::vector<uint32_t> localRefs;
  const float nodeVolume = std::max(volume(bounds), 1.0e-12f);
  for (uint32_t instanceIndex : instanceIndices)
  {
    const SDFBounds &instanceBounds = instances_[instanceIndex].worldBounds;
    uint32_t overlapMask = 0u;
    uint32_t overlapCount = 0u;
    float coveredVolume = 0.0f;
    for (uint32_t child = 0u; child < 8u; ++child)
    {
      const SDFBounds cb = childBounds(bounds, child);
      if (!overlaps(cb, instanceBounds))
        continue;
      overlapMask |= 1u << child;
      ++overlapCount;
      coveredVolume += overlapVolume(cb, instanceBounds);
    }

    if (overlapCount == 0u)
      continue;
    const bool tooBroad = overlapCount > 4u || (coveredVolume / nodeVolume) > 0.65f;
    if (tooBroad)
    {
      localRefs.push_back(instanceIndex);
      continue;
    }
    for (uint32_t child = 0u; child < 8u; ++child)
    {
      if ((overlapMask & (1u << child)) != 0u)
        childLists[child].push_back(instanceIndex);
    }
  }

  if (!localRefs.empty())
  {
    tlasNodes_[nodeIndex].firstInstanceRef = static_cast<uint32_t>(tlasInstanceRefs_.size());
    tlasNodes_[nodeIndex].instanceRefCount = static_cast<uint32_t>(localRefs.size());
    tlasInstanceRefs_.insert(tlasInstanceRefs_.end(), localRefs.begin(), localRefs.end());
  }

  for (uint32_t child = 0u; child < 8u; ++child)
  {
    if (childLists[child].empty())
      continue;
    const uint32_t childIndex = buildTlasNode(childBounds(bounds, child), childLists[child], depth + 1u);
    tlasNodes_[nodeIndex].childMask |= 1u << child;
    tlasNodes_[nodeIndex].childIndices[child] = childIndex;
  }

  if (tlasNodes_[nodeIndex].childMask == 0u && tlasNodes_[nodeIndex].instanceRefCount == 0u)
  {
    tlasNodes_[nodeIndex].firstInstanceRef = static_cast<uint32_t>(tlasInstanceRefs_.size());
    tlasNodes_[nodeIndex].instanceRefCount = static_cast<uint32_t>(instanceIndices.size());
    tlasNodes_[nodeIndex].flags = 1u;
    tlasInstanceRefs_.insert(tlasInstanceRefs_.end(), instanceIndices.begin(), instanceIndices.end());
  }
  return nodeIndex;
}

std::vector<uint32_t> SDFSceneManager::queryCandidates(const SDFRay &ray) const
{
  std::vector<uint32_t> candidates;
  if (rootTlasNode_ == kInvalid || tlasNodes_.empty())
    return candidates;

  std::vector<uint8_t> visited(instances_.size(), 0u);
  struct StackItem
  {
    uint32_t nodeIndex;
    float tMin;
    float tMax;
  };
  std::vector<StackItem> stack;
  float rootMin = 0.0f;
  float rootMax = 0.0f;
  if (!rayBounds(ray, worldBounds_, rootMin, rootMax))
    return candidates;
  stack.push_back({rootTlasNode_, rootMin, rootMax});

  while (!stack.empty())
  {
    std::sort(stack.begin(), stack.end(), [](const StackItem &a, const StackItem &b) { return a.tMin > b.tMin; });
    const StackItem item = stack.back();
    stack.pop_back();
    const TlasNode &node = tlasNodes_[item.nodeIndex];
    for (uint32_t i = 0u; i < node.instanceRefCount; ++i)
    {
      const uint32_t instanceIndex = tlasInstanceRefs_[node.firstInstanceRef + i];
      if (instanceIndex >= instances_.size() || visited[instanceIndex] != 0u)
        continue;
      float hitMin = 0.0f;
      float hitMax = 0.0f;
      if (rayBounds(ray, instances_[instanceIndex].worldBounds, hitMin, hitMax))
      {
        visited[instanceIndex] = 1u;
        candidates.push_back(instanceIndex);
      }
    }
    for (uint32_t child = 0u; child < 8u; ++child)
    {
      const uint32_t childIndex = node.childIndices[child];
      if (childIndex == kInvalid)
        continue;
      float hitMin = 0.0f;
      float hitMax = 0.0f;
      if (rayBounds(ray, tlasNodes_[childIndex].bounds, hitMin, hitMax))
        stack.push_back({childIndex, hitMin, hitMax});
    }
  }
  return candidates;
}

SDFSceneManager::SampleResult SDFSceneManager::sampleLocal(uint32_t instanceIndex, const math::Vec3f &localPoint, uint32_t debugMaxLevel) const
{
  SampleResult result;
  if (instanceIndex >= instances_.size())
    return result;
  const RuntimeInstance &instance = instances_[instanceIndex];
  const RuntimeBlas &blas = blases_[instance.blasIndex];
  if (blas.payload.nodes.empty() || blas.payload.bricks.empty())
    return result;

  uint32_t nodeIndex = std::min(instance.header.rootNode, static_cast<uint32_t>(blas.payload.nodes.size() - 1u));
  SDFBounds nodeBounds = instance.localBounds;
  for (;;)
  {
    const VSDFNodeDisk &node = blas.payload.nodes[nodeIndex];
    if (node.brickIndex >= blas.payload.bricks.size())
      return result;
    const VSDFBrickDisk &brick = blas.payload.bricks[node.brickIndex];

    const math::Vec3f extent = boundsExtent(nodeBounds);
    math::Vec3f uvw{
        extent[0] > 1.0e-8f ? (localPoint[0] - nodeBounds.min[0]) / extent[0] : 0.0f,
        extent[1] > 1.0e-8f ? (localPoint[1] - nodeBounds.min[1]) / extent[1] : 0.0f,
        extent[2] > 1.0e-8f ? (localPoint[2] - nodeBounds.min[2]) / extent[2] : 0.0f};
    uvw[0] = std::clamp(uvw[0], 0.0f, 0.999999f);
    uvw[1] = std::clamp(uvw[1], 0.0f, 0.999999f);
    uvw[2] = std::clamp(uvw[2], 0.0f, 0.999999f);
    const uint32_t x = std::min<uint32_t>(VSDF_BRICK_RESOLUTION - 1u, static_cast<uint32_t>(uvw[0] * VSDF_BRICK_RESOLUTION));
    const uint32_t y = std::min<uint32_t>(VSDF_BRICK_RESOLUTION - 1u, static_cast<uint32_t>(uvw[1] * VSDF_BRICK_RESOLUTION));
    const uint32_t z = std::min<uint32_t>(VSDF_BRICK_RESOLUTION - 1u, static_cast<uint32_t>(uvw[2] * VSDF_BRICK_RESOLUTION));
    const uint32_t cell = x + y * VSDF_BRICK_RESOLUTION + z * VSDF_BRICK_RESOLUTION * VSDF_BRICK_RESOLUTION;
    if (node.level < debugMaxLevel && node.firstChild != kInvalid && hasChild(node, cell))
    {
      nodeIndex = node.firstChild + popcountBefore(node.childMaskWords, cell);
      if (nodeIndex >= blas.payload.nodes.size())
        return result;
      nodeBounds = brickCellBounds(nodeBounds, cell);
      continue;
    }

    result.nodeIndex = nodeIndex;
    result.brickIndex = node.brickIndex;
    result.level = node.level;
    result.voxelSize = brick.voxelSize;
    result.cellBounds = brickCellBounds(nodeBounds, cell);
    result.validSurfaceCell = hasValidSurfaceCell(node, cell);
    result.traceSurfaceCell = hasTraceSurfaceCell(node, cell);
    if (!result.traceSurfaceCell)
    {
      result.distance = brick.voxelSize;
      return result;
    }

    const auto sampleDistance =
        [&blas, &brick, narrowBand = instance.header.narrowBandVoxels](uint32_t sx, uint32_t sy, uint32_t sz) -> float
        {
          const uint32_t sampleCell = sx + sy * VSDF_BRICK_DISTANCE_RESOLUTION + sz * VSDF_BRICK_DISTANCE_RESOLUTION * VSDF_BRICK_DISTANCE_RESOLUTION;
          const uint32_t distanceOffset = brick.distanceOffset + sampleCell;
          if (distanceOffset >= blas.payload.distances.size())
            return 1.0e30f;
          return decodeSnorm8(blas.payload.distances[distanceOffset]) * static_cast<float>(std::max(narrowBand, 1u)) * brick.voxelSize;
        };
    const auto lerp = [](float a, float b, float t) -> float { return a + (b - a) * t; };
    const float gx = std::clamp(uvw[0] * static_cast<float>(VSDF_BRICK_RESOLUTION), 0.0f, static_cast<float>(VSDF_BRICK_RESOLUTION));
    const float gy = std::clamp(uvw[1] * static_cast<float>(VSDF_BRICK_RESOLUTION), 0.0f, static_cast<float>(VSDF_BRICK_RESOLUTION));
    const float gz = std::clamp(uvw[2] * static_cast<float>(VSDF_BRICK_RESOLUTION), 0.0f, static_cast<float>(VSDF_BRICK_RESOLUTION));
    const uint32_t x0 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, static_cast<uint32_t>(std::floor(gx)));
    const uint32_t y0 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, static_cast<uint32_t>(std::floor(gy)));
    const uint32_t z0 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, static_cast<uint32_t>(std::floor(gz)));
    const uint32_t x1 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, x0 + 1u);
    const uint32_t y1 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, y0 + 1u);
    const uint32_t z1 = std::min<uint32_t>(VSDF_BRICK_DISTANCE_RESOLUTION - 1u, z0 + 1u);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);
    const float fz = gz - static_cast<float>(z0);
    const float d00 = lerp(sampleDistance(x0, y0, z0), sampleDistance(x1, y0, z0), fx);
    const float d10 = lerp(sampleDistance(x0, y1, z0), sampleDistance(x1, y1, z0), fx);
    const float d01 = lerp(sampleDistance(x0, y0, z1), sampleDistance(x1, y0, z1), fx);
    const float d11 = lerp(sampleDistance(x0, y1, z1), sampleDistance(x1, y1, z1), fx);
    result.distance = lerp(lerp(d00, d10, fy), lerp(d01, d11, fy), fz);
    return result;
  }
}

SDFSceneHit SDFSceneManager::traceInstanceCPU(uint32_t instanceIndex, const SDFRay &ray, SDFSceneHit bestHit) const
{
  if (instanceIndex >= instances_.size())
    return bestHit;
  const RuntimeInstance &instance = instances_[instanceIndex];
  const math::Vec3f localOrigin = transformPoint(instance.header.worldToLocal, ray.origin);
  const math::Vec3f localEnd = transformPoint(instance.header.worldToLocal, ray.origin + ray.direction * std::min(ray.tMax, bestHit.t));
  const math::Vec3f localDirection = localEnd - localOrigin;
  const float localLength = localDirection.length();
  if (localLength <= 1.0e-8f)
    return bestHit;

  SDFRay localRay;
  localRay.origin = localOrigin;
  localRay.direction = localDirection * (1.0f / std::min(ray.tMax, bestHit.t));
  localRay.tMin = ray.tMin;
  localRay.tMax = std::min(ray.tMax, bestHit.t);
  float tMin = 0.0f;
  float tMax = 0.0f;
  if (!rayBounds(localRay, instance.localBounds, tMin, tMax))
    return bestHit;

  float t = std::max(ray.tMin, tMin);
  bool hasPreviousSample = false;
  float previousT = t;
  float previousWorldDistance = std::numeric_limits<float>::infinity();
  SampleResult previousSample;
  for (uint32_t step = 0u; step < settings_.maxInstanceTraceSteps && t <= tMax && t < bestHit.t; ++step)
  {
    const math::Vec3f p = localOrigin + localRay.direction * t;
    const SampleResult sample = sampleLocal(instanceIndex, p);
    const float worldDistance = sample.distance * instance.directory.conservativeScale;
    const float threshold = std::max(sample.voxelSize * instance.directory.conservativeScale * settings_.hitThresholdScale, 1.0e-5f);
    const bool crossedSurface = worldDistance <= 0.0f && hasPreviousSample && previousWorldDistance > 0.0f;
    const bool validCrossing = crossedSurface && (sample.validSurfaceCell || previousSample.validSurfaceCell);
    if ((sample.validSurfaceCell && std::fabs(worldDistance) <= threshold) || validCrossing)
    {
      float hitT = t;
      math::Vec3f hitLocalPosition = p;
      SampleResult hitSample = sample;
      if (!hitSample.validSurfaceCell && previousSample.validSurfaceCell)
        hitSample = previousSample;
      if (crossedSurface)
      {
        float loT = previousT;
        float hiT = t;
        for (uint32_t refine = 0u; refine < 6u; ++refine)
        {
          const float midT = (loT + hiT) * 0.5f;
          const math::Vec3f midP = localOrigin + localRay.direction * midT;
          const SampleResult midSample = sampleLocal(instanceIndex, midP);
          const float midWorldDistance = midSample.distance * instance.directory.conservativeScale;
          if (midSample.validSurfaceCell && midWorldDistance <= 0.0f)
          {
            hiT = midT;
            hitSample = midSample;
          }
          else
          {
            loT = midT;
          }
        }
        hitT = hiT;
        hitLocalPosition = localOrigin + localRay.direction * hitT;
      }

      bestHit.hit = true;
      bestHit.instanceIndex = instanceIndex;
      bestHit.instanceId = instance.directory.instanceId;
      bestHit.blasIndex = instance.blasIndex;
      bestHit.sdfNode = hitSample.nodeIndex;
      bestHit.brickIndex = hitSample.brickIndex;
      bestHit.sdfLevel = hitSample.level;
      bestHit.steps = step + 1u;
      bestHit.t = hitT;
      bestHit.localPosition = hitLocalPosition;
      bestHit.worldPosition = ray.origin + ray.direction * hitT;
      const float eps = std::max(hitSample.voxelSize * 0.5f, 1.0e-4f);
      const math::Vec3f nLocal{
          sampleLocal(instanceIndex, hitLocalPosition + math::Vec3f(eps, 0.0f, 0.0f)).distance - sampleLocal(instanceIndex, hitLocalPosition - math::Vec3f(eps, 0.0f, 0.0f)).distance,
          sampleLocal(instanceIndex, hitLocalPosition + math::Vec3f(0.0f, eps, 0.0f)).distance - sampleLocal(instanceIndex, hitLocalPosition - math::Vec3f(0.0f, eps, 0.0f)).distance,
          sampleLocal(instanceIndex, hitLocalPosition + math::Vec3f(0.0f, 0.0f, eps)).distance - sampleLocal(instanceIndex, hitLocalPosition - math::Vec3f(0.0f, 0.0f, eps)).distance};
      bestHit.worldNormal = transformVector(instance.header.worldToLocal, nLocal).normalize();
      return bestHit;
    }
    const float minStep = std::max(sample.voxelSize * instance.directory.conservativeScale * settings_.minStepScale, 1.0e-5f);
    previousT = t;
    previousWorldDistance = worldDistance;
    previousSample = sample;
    hasPreviousSample = true;
    const float signedDistanceAdvance = std::max(std::fabs(worldDistance) * settings_.stepScale, minStep);
    const float emptyAdvance = std::max(emptyCellAdvance(p, localRay.direction, sample.cellBounds, sample.voxelSize), minStep) + minStep * 0.25f;
    const float surfaceAdvance = std::min(signedDistanceAdvance, std::max(emptyAdvance * 0.75f, minStep));
    t += sample.traceSurfaceCell ? surfaceAdvance : emptyAdvance;
  }
  return bestHit;
}

SDFSceneHit SDFSceneManager::traceCPU(const SDFRay &ray) const
{
  SDFSceneHit bestHit;
  bestHit.t = ray.tMax;
  const std::vector<uint32_t> candidates = queryCandidates(ray);
  for (uint32_t instanceIndex : candidates)
    bestHit = traceInstanceCPU(instanceIndex, ray, bestHit);
  return bestHit;
}

void SDFSceneManager::rebuildGPUArrays()
{
  gpuInstances_.clear();
  gpuBlases_.clear();
  gpuBlasNodes_.clear();
  gpuBlasNodeChildWordPrefixes_.clear();
  gpuBlasBricks_.clear();
  gpuDistanceWords_.clear();
  gpuTracePageTables_.clear();
  gpuTracePages_.clear();
  gpuTracePageMasks_.clear();
  gpuTraceBrickMaps_.clear();
  gpuDistanceAtlasBytes_.clear();
  gpuDistanceAtlasBricksPerAxis_ = 1u;
  gpuTlasNodes_.clear();
  gpuSceneInfo_ = {};

  uint32_t atlasBrickIndex = 0u;
  uint32_t totalBrickCount = 0u;
  for (const RuntimeBlas &blas : blases_)
    totalBrickCount += static_cast<uint32_t>(blas.payload.bricks.size());
  gpuDistanceAtlasBricksPerAxis_ = ceilCubeRoot(totalBrickCount);
  const uint32_t atlasResolution = gpuDistanceAtlasBricksPerAxis_ * VSDF_BRICK_DISTANCE_RESOLUTION;
  gpuDistanceAtlasBytes_.assign(
      static_cast<size_t>(atlasResolution) * atlasResolution * atlasResolution,
      encodeSnorm8(1.0f));
  for (uint32_t blasIndex = 0u; blasIndex < blases_.size(); ++blasIndex)
  {
    RuntimeBlas &blas = blases_[blasIndex];
    blas.nodeOffset = static_cast<uint32_t>(gpuBlasNodes_.size());
    blas.brickOffset = static_cast<uint32_t>(gpuBlasBricks_.size());
    blas.distanceByteOffset = static_cast<uint32_t>(gpuDistanceWords_.size() * sizeof(uint32_t));
    blas.distanceWordOffset = static_cast<uint32_t>(gpuDistanceWords_.size());
    gpuBlasNodes_.insert(gpuBlasNodes_.end(), blas.payload.nodes.begin(), blas.payload.nodes.end());
    gpuBlasNodeChildWordPrefixes_.reserve(gpuBlasNodeChildWordPrefixes_.size() + blas.payload.nodes.size() * 16u);
    for (const VSDFNodeDisk &node : blas.payload.nodes)
    {
      uint32_t prefix = 0u;
      for (uint32_t wordIndex = 0u; wordIndex < 16u; ++wordIndex)
      {
        gpuBlasNodeChildWordPrefixes_.push_back(prefix);
        prefix += static_cast<uint32_t>(__builtin_popcount(node.childMaskWords[wordIndex]));
      }
    }
    gpuBlasBricks_.insert(gpuBlasBricks_.end(), blas.payload.bricks.begin(), blas.payload.bricks.end());
    for (const VSDFBrickDisk &brick : blas.payload.bricks)
    {
      if (brick.distanceOffset + VSDF_BRICK_DISTANCE_COUNT <= blas.payload.distances.size())
        copyBrickToAtlas(gpuDistanceAtlasBytes_, gpuDistanceAtlasBricksPerAxis_, atlasBrickIndex, blas.payload.distances.data() + brick.distanceOffset);
      ++atlasBrickIndex;
    }
    if (blas.payload.tracePageTable.size() == VSDF_TRACE_ROOT_CELL_COUNT)
    {
      const uint32_t pageBase = static_cast<uint32_t>(gpuTracePages_.size());
      const uint32_t maskBase = static_cast<uint32_t>(gpuTracePageMasks_.size());
      const uint32_t brickBase = static_cast<uint32_t>(gpuTraceBrickMaps_.size());
      for (uint32_t pageIndex : blas.payload.tracePageTable)
        gpuTracePageTables_.push_back(pageIndex == VSDF_TRACE_INVALID_BRICK ? VSDF_TRACE_INVALID_BRICK : pageBase + pageIndex);
      for (VSDFTracePageDisk page : blas.payload.tracePages)
      {
        page.maskOffset += maskBase;
        page.brickOffset += brickBase;
        gpuTracePages_.push_back(page);
      }
      gpuTracePageMasks_.insert(gpuTracePageMasks_.end(), blas.payload.tracePageMasks.begin(), blas.payload.tracePageMasks.end());
      gpuTraceBrickMaps_.insert(gpuTraceBrickMaps_.end(), blas.payload.traceBrickMap.begin(), blas.payload.traceBrickMap.end());
    }
    else
    {
      gpuTracePageTables_.insert(gpuTracePageTables_.end(), VSDF_TRACE_ROOT_CELL_COUNT, VSDF_TRACE_INVALID_BRICK);
      gpuTraceBrickMaps_.insert(gpuTraceBrickMaps_.end(), VSDF_TRACE_GRID_CELL_COUNT, VSDF_TRACE_INVALID_BRICK);
    }

    appendBytesAsWords(gpuDistanceWords_, blas.payload.distances);
  }

  gpuBlases_.reserve(blases_.size());
  for (uint32_t blasIndex = 0u; blasIndex < blases_.size(); ++blasIndex)
  {
    const RuntimeBlas &blas = blases_[blasIndex];
    const VSDFInstancePayloadHeader &header = blas.payload.header;
    SDFBlasGPU gpu{};
    gpu.localBoundsMin = {header.localBoundsMin[0], header.localBoundsMin[1], header.localBoundsMin[2], 0.0f};
    gpu.localBoundsMaxAndRootVoxelSize = {header.localBoundsMax[0], header.localBoundsMax[1], header.localBoundsMax[2], header.rootVoxelSize};
    gpu.offsetsAndFormat = {header.rootNode, blas.nodeOffset, blas.brickOffset, blas.distanceByteOffset};
    gpu.countsAndNarrowBand = {VSDF_DISTANCE_R8_SNORM, header.nodeCount, header.brickCount, header.narrowBandVoxels};
    gpuBlases_.push_back(gpu);
  }

  rebuildGPUInstanceTlasArrays();
}

void SDFSceneManager::rebuildGPUInstanceTlasArrays()
{
  gpuInstances_.clear();
  gpuTlasNodes_.clear();

  gpuInstances_.reserve(instances_.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < instances_.size(); ++instanceIndex)
  {
    const RuntimeInstance &instance = instances_[instanceIndex];
    SDFInstanceGPU gpu{};
    copyMat(gpu.localToWorld, instance.header.localToWorld);
    copyMat(gpu.worldToLocal, instance.header.worldToLocal);
    gpu.worldBoundsMin = {instance.worldBounds.min[0], instance.worldBounds.min[1], instance.worldBounds.min[2], 0.0f};
    gpu.worldBoundsMaxAndConservativeScale = {instance.worldBounds.max[0], instance.worldBounds.max[1], instance.worldBounds.max[2], instance.directory.conservativeScale};
    gpu.idsAndBlas = {instance.directory.instanceId, instance.blasIndex, instance.directory.sourceMeshIndex, instance.directory.maxLevel};
    gpu.materialAndFlags = {instance.directory.materialRangeBegin, instance.directory.materialRangeCount, instance.directory.flags, 0u};
    gpuInstances_.push_back(gpu);
  }

  gpuTlasNodes_.reserve(tlasNodes_.size());
  for (const TlasNode &node : tlasNodes_)
  {
    SDFTlasNodeGPU gpu{};
    gpu.boundsMin = {node.bounds.min[0], node.bounds.min[1], node.bounds.min[2], 0.0f};
    gpu.boundsMax = {node.bounds.max[0], node.bounds.max[1], node.bounds.max[2], 0.0f};
    gpu.childAndRefs = {node.childMask, node.firstChild, node.firstInstanceRef, node.instanceRefCount};
    gpu.depthFlags = {node.depth, node.flags, 0u, 0u};
    gpu.childIndices0 = {node.childIndices[0], node.childIndices[1], node.childIndices[2], node.childIndices[3]};
    gpu.childIndices1 = {node.childIndices[4], node.childIndices[5], node.childIndices[6], node.childIndices[7]};
    gpuTlasNodes_.push_back(gpu);
  }

  gpuSceneInfo_.worldBoundsMinAndMaxDepth = {worldBounds_.min[0], worldBounds_.min[1], worldBounds_.min[2], static_cast<float>(settings_.tlasMaxDepth)};
  gpuSceneInfo_.worldBoundsMaxAndRootNode = {worldBounds_.max[0], worldBounds_.max[1], worldBounds_.max[2], static_cast<float>(rootTlasNode_)};
  gpuSceneInfo_.counts = {
      static_cast<uint32_t>(instances_.size()),
      static_cast<uint32_t>(blases_.size()),
      static_cast<uint32_t>(tlasNodes_.size()),
      static_cast<uint32_t>(tlasInstanceRefs_.size())};
  gpuSceneInfo_.settings = {
      settings_.maxInstanceTraceSteps,
      settings_.enableMailbox ? 1u : 0u,
      gpuDistanceAtlasBricksPerAxis_,
      0u};
  rebuildGPUSceneGridArrays();
}

void SDFSceneManager::rebuildGPUSceneGridArrays()
{
  gpuSceneGridInfo_ = {};
  gpuSceneGridCells_.clear();
  gpuSceneGridRefs_.clear();
  gpuSceneGridGlobalRefs_.clear();

  if (!settings_.enableSceneGrid || instances_.empty() || !validBounds(worldBounds_))
    return;

  const uint32_t resolution = clampGridResolution(settings_.sceneGridResolution);
  const uint32_t cellCapacity = std::max(1u, std::min(settings_.sceneGridCellCapacity, 64u));
  const uint32_t cellCount = resolution * resolution * resolution;
  gpuSceneGridCells_.resize(cellCount);
  gpuSceneGridRefs_.resize(static_cast<size_t>(cellCount) * cellCapacity);

  gpuSceneGridInfo_.worldBoundsMin = {worldBounds_.min[0], worldBounds_.min[1], worldBounds_.min[2], 0.0f};
  gpuSceneGridInfo_.worldBoundsMaxAndResolution = {worldBounds_.max[0], worldBounds_.max[1], worldBounds_.max[2], static_cast<float>(resolution)};
  gpuSceneGridInfo_.counts = {cellCount, cellCapacity, static_cast<uint32_t>(gpuSceneGridRefs_.size()), 0u};
  gpuSceneGridInfo_.settings = {settings_.enableSceneGridPageRefs ? 1u : 0u, 0u, 0u, 0u};

  for (uint32_t cellIndex = 0u; cellIndex < cellCount; ++cellIndex)
    gpuSceneGridCells_[cellIndex].offsetCountAndFlags = {cellIndex * cellCapacity, 0u, 0u, 0u};

  const auto makeRef = [](uint32_t kind, uint32_t instanceIndex, uint32_t pageRootFlat, const SDFBounds &bounds)
  {
    SDFSceneGridRefGPU ref{};
    ref.boundsMin = {bounds.min[0], bounds.min[1], bounds.min[2], 0.0f};
    ref.boundsMax = {bounds.max[0], bounds.max[1], bounds.max[2], 0.0f};
    ref.kindInstanceAndPage = {kind, instanceIndex, pageRootFlat, 0u};
    return ref;
  };

  auto emitRef = [&](const SDFSceneGridRefGPU &ref)
  {
    SDFBounds bounds{{ref.boundsMin[0], ref.boundsMin[1], ref.boundsMin[2]},
                     {ref.boundsMax[0], ref.boundsMax[1], ref.boundsMax[2]}};
    const uint32_t overlapCount = gridCellOverlapCount(worldBounds_, bounds, resolution);
    if (overlapCount == 0u)
      return;
    if (overlapCount > std::max(settings_.sceneGridMaxCellsPerRef, 1u))
    {
      gpuSceneGridGlobalRefs_.push_back(ref);
      return;
    }

    const math::Vec3f extent = boundsExtent(worldBounds_);
    std::array<uint32_t, 3u> minCell{};
    std::array<uint32_t, 3u> maxCell{};
    for (uint32_t axisIndex = 0u; axisIndex < 3u; ++axisIndex)
    {
      const float scale = static_cast<float>(resolution) / std::max(extent[axisIndex], 1.0e-8f);
      const int minIndex = static_cast<int>(std::floor((bounds.min[axisIndex] - worldBounds_.min[axisIndex]) * scale));
      const int maxIndex = static_cast<int>(std::floor((bounds.max[axisIndex] - worldBounds_.min[axisIndex]) * scale));
      minCell[axisIndex] = static_cast<uint32_t>(std::clamp(minIndex, 0, static_cast<int>(resolution) - 1));
      maxCell[axisIndex] = static_cast<uint32_t>(std::clamp(maxIndex, 0, static_cast<int>(resolution) - 1));
    }

    bool overflowed = false;
    for (uint32_t z = minCell[2]; z <= maxCell[2]; ++z)
    {
      for (uint32_t y = minCell[1]; y <= maxCell[1]; ++y)
      {
        for (uint32_t x = minCell[0]; x <= maxCell[0]; ++x)
        {
          const uint32_t cellIndex = flattenGridCell(x, y, z, resolution);
          SDFSceneGridCellGPU &cell = gpuSceneGridCells_[cellIndex];
          const uint32_t count = cell.offsetCountAndFlags[1];
          if (count < cellCapacity)
          {
            gpuSceneGridRefs_[static_cast<size_t>(cell.offsetCountAndFlags[0]) + count] = ref;
            cell.offsetCountAndFlags[1] = count + 1u;
          }
          else
          {
            overflowed = true;
            cell.offsetCountAndFlags[2] = 1u;
          }
        }
      }
    }
    if (overflowed)
      gpuSceneGridGlobalRefs_.push_back(ref);
  };

  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instances_.size()); ++instanceIndex)
  {
    const RuntimeInstance &instance = instances_[instanceIndex];
    if (instance.blasIndex >= blases_.size())
      continue;

    const RuntimeBlas &blas = blases_[instance.blasIndex];
    const uint32_t instanceCellCount = gridCellOverlapCount(worldBounds_, instance.worldBounds, resolution);
    const bool canUsePageRefs =
        settings_.enableSceneGridPageRefs &&
        instanceCellCount > std::max(settings_.sceneGridHugeInstanceCellThreshold, 1u) &&
        blas.payload.tracePageTable.size() == VSDF_TRACE_ROOT_CELL_COUNT &&
        !blas.payload.tracePages.empty();

    if (!canUsePageRefs)
    {
      emitRef(makeRef(1u, instanceIndex, kInvalid, instance.worldBounds));
      continue;
    }

    math::Mat4f localToWorld = math::Mat4f::identity();
    std::memcpy(localToWorld.data, instance.header.localToWorld, sizeof(float) * 16u);
    uint32_t emittedPages = 0u;
    for (uint32_t rootPageFlat = 0u; rootPageFlat < static_cast<uint32_t>(blas.payload.tracePageTable.size()); ++rootPageFlat)
    {
      if (blas.payload.tracePageTable[rootPageFlat] == VSDF_TRACE_INVALID_BRICK)
        continue;
      const SDFBounds localPageBounds = traceRootPageLocalBounds(blas.payload.header, rootPageFlat);
      const SDFBounds worldPageBounds = transformBounds(localPageBounds, localToWorld);
      if (!validBounds(worldPageBounds))
        continue;
      emitRef(makeRef(2u, instanceIndex, rootPageFlat, worldPageBounds));
      ++emittedPages;
    }
    if (emittedPages == 0u)
      emitRef(makeRef(1u, instanceIndex, kInvalid, instance.worldBounds));
  }

  const uint32_t globalRefCount = static_cast<uint32_t>(gpuSceneGridGlobalRefs_.size());
  gpuSceneGridInfo_.counts[3] = globalRefCount;
  gpuSceneGridGlobalRefs_.resize(std::max<uint32_t>(globalRefCount, std::max(settings_.sceneGridGlobalRefCapacity, 1u)));
}

rendering::Buffer SDFSceneManager::createUploadedBuffer(const std::string &name, uint64_t size, const void *data, rendering::BufferUsage extraUsage) const
{
  const uint32_t zero = 0u;
  const uint64_t finalSize = std::max<uint64_t>(size, sizeof(uint32_t));
  rendering::Buffer buffer = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = name,
          .size = finalSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | extraUsage,
      });
  renderGraph_->bufferWrite(buffer, 0u, size != 0u ? size : sizeof(uint32_t), const_cast<void *>(data != nullptr ? data : static_cast<const void *>(&zero)));
  return buffer;
}

void SDFSceneManager::uploadGPUResources()
{
  if (renderGraph_ == nullptr)
    return;
  destroyGPUResources();
  rebuildGPUArrays();

  gpuResources_.sceneInfo = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "SDFSceneInfo.buffer",
          .size = sizeof(SDFSceneInfoGPU),
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
      });
  renderGraph_->bufferWrite(gpuResources_.sceneInfo, 0u, sizeof(SDFSceneInfoGPU), &gpuSceneInfo_);

  gpuResources_.instances = createUploadedBuffer("SDFInstances.buffer", gpuInstances_.size() * sizeof(SDFInstanceGPU), gpuInstances_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.blases = createUploadedBuffer("SDFBlases.buffer", gpuBlases_.size() * sizeof(SDFBlasGPU), gpuBlases_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.blasNodes = createUploadedBuffer("SDFBlasNodes.buffer", gpuBlasNodes_.size() * sizeof(VSDFNodeDisk), gpuBlasNodes_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.blasNodeChildWordPrefixes = createUploadedBuffer("SDFBlasNodeChildWordPrefixes.buffer", gpuBlasNodeChildWordPrefixes_.size() * sizeof(uint32_t), gpuBlasNodeChildWordPrefixes_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.blasBricks = createUploadedBuffer("SDFBlasBricks.buffer", gpuBlasBricks_.size() * sizeof(VSDFBrickDisk), gpuBlasBricks_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.distanceWords = createUploadedBuffer("SDFDistanceWords.buffer", gpuDistanceWords_.size() * sizeof(uint32_t), gpuDistanceWords_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.tracePageTables = createUploadedBuffer("SDFTracePageTables.buffer", gpuTracePageTables_.size() * sizeof(uint32_t), gpuTracePageTables_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.tracePages = createUploadedBuffer("SDFTracePages.buffer", gpuTracePages_.size() * sizeof(VSDFTracePageDisk), gpuTracePages_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.tracePageMasks = createUploadedBuffer("SDFTracePageMasks.buffer", gpuTracePageMasks_.size() * sizeof(uint32_t), gpuTracePageMasks_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.traceBrickMaps = createUploadedBuffer("SDFTraceBrickMaps.buffer", gpuTraceBrickMaps_.size() * sizeof(uint32_t), gpuTraceBrickMaps_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.tlasNodes = createUploadedBuffer("SDFTlasNodes.buffer", gpuTlasNodes_.size() * sizeof(SDFTlasNodeGPU), gpuTlasNodes_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.tlasInstanceRefs = createUploadedBuffer("SDFTlasInstanceRefs.buffer", tlasInstanceRefs_.size() * sizeof(uint32_t), tlasInstanceRefs_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.sceneGridInfo = createUploadedBuffer("SDFSceneGridInfo.buffer", sizeof(SDFSceneGridInfoGPU), &gpuSceneGridInfo_, rendering::BufferUsage::BufferUsage_None);
  gpuResources_.sceneGridCells = createUploadedBuffer("SDFSceneGridCells.buffer", gpuSceneGridCells_.size() * sizeof(SDFSceneGridCellGPU), gpuSceneGridCells_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.sceneGridRefs = createUploadedBuffer("SDFSceneGridRefs.buffer", gpuSceneGridRefs_.size() * sizeof(SDFSceneGridRefGPU), gpuSceneGridRefs_.data(), rendering::BufferUsage::BufferUsage_None);
  gpuResources_.sceneGridGlobalRefs = createUploadedBuffer("SDFSceneGridGlobalRefs.buffer", gpuSceneGridGlobalRefs_.size() * sizeof(SDFSceneGridRefGPU), gpuSceneGridGlobalRefs_.data(), rendering::BufferUsage::BufferUsage_None);
  const uint32_t atlasResolution = std::max(gpuDistanceAtlasBricksPerAxis_, 1u) * VSDF_BRICK_DISTANCE_RESOLUTION;
  gpuResources_.distanceAtlasBricksPerAxis = std::max(gpuDistanceAtlasBricksPerAxis_, 1u);
  gpuResources_.distanceAtlas = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = "SDFDistanceAtlas.texture",
          .format = rendering::Format::Format_R8Snorm,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_TransferDst,
          .width = atlasResolution,
          .height = atlasResolution,
          .depth = atlasResolution,
          .mipLevels = 1u,
          .initialLayout = rendering::ResourceLayout::SHADER_READ_ONLY,
      });
  renderGraph_->textureWrite(
      gpuResources_.distanceAtlas,
      gpuDistanceAtlasBytes_.size(),
      gpuDistanceAtlasBytes_.data(),
      rendering::ResourceLayout::UNDEFINED);
  ++gpuResourceRevision_;
}

void SDFSceneManager::uploadDynamicGPUResources()
{
  if (renderGraph_ == nullptr)
    return;
  if (!gpuResources_.isValid())
  {
    uploadGPUResources();
    return;
  }

  renderGraph_->bufferWrite(gpuResources_.sceneInfo, 0u, sizeof(SDFSceneInfoGPU), &gpuSceneInfo_);

  bool recreatedBuffers = false;
  auto uploadOrGrow = [&](rendering::Buffer &buffer, const std::string &name, uint64_t byteSize, const void *data)
  {
    const uint32_t zero = 0u;
    const uint64_t finalSize = std::max<uint64_t>(byteSize, sizeof(uint32_t));
    if (!buffer.isValid() || renderGraph_->getBufferSize(buffer) < finalSize)
    {
      if (buffer.isValid())
        renderGraph_->deleteBuffer(buffer);
      buffer = createUploadedBuffer(name, byteSize, data, rendering::BufferUsage::BufferUsage_None);
      recreatedBuffers = true;
      return;
    }

    renderGraph_->bufferWrite(buffer, 0u, byteSize != 0u ? byteSize : sizeof(uint32_t), const_cast<void *>(data != nullptr ? data : static_cast<const void *>(&zero)));
  };

  uploadOrGrow(
      gpuResources_.instances,
      "SDFInstances.buffer",
      gpuInstances_.size() * sizeof(SDFInstanceGPU),
      gpuInstances_.data());
  uploadOrGrow(
      gpuResources_.tlasNodes,
      "SDFTlasNodes.buffer",
      gpuTlasNodes_.size() * sizeof(SDFTlasNodeGPU),
      gpuTlasNodes_.data());
  uploadOrGrow(
      gpuResources_.tlasInstanceRefs,
      "SDFTlasInstanceRefs.buffer",
      tlasInstanceRefs_.size() * sizeof(uint32_t),
      tlasInstanceRefs_.data());
  uploadOrGrow(
      gpuResources_.sceneGridInfo,
      "SDFSceneGridInfo.buffer",
      sizeof(SDFSceneGridInfoGPU),
      &gpuSceneGridInfo_);
  uploadOrGrow(
      gpuResources_.sceneGridCells,
      "SDFSceneGridCells.buffer",
      gpuSceneGridCells_.size() * sizeof(SDFSceneGridCellGPU),
      gpuSceneGridCells_.data());
  uploadOrGrow(
      gpuResources_.sceneGridRefs,
      "SDFSceneGridRefs.buffer",
      gpuSceneGridRefs_.size() * sizeof(SDFSceneGridRefGPU),
      gpuSceneGridRefs_.data());
  uploadOrGrow(
      gpuResources_.sceneGridGlobalRefs,
      "SDFSceneGridGlobalRefs.buffer",
      gpuSceneGridGlobalRefs_.size() * sizeof(SDFSceneGridRefGPU),
      gpuSceneGridGlobalRefs_.data());

  if (recreatedBuffers)
    ++gpuResourceRevision_;
}

void SDFSceneManager::destroyGPUResources()
{
  bool destroyedAny = false;
  if (renderGraph_ == nullptr)
  {
    destroyedAny = gpuResources_.isValid();
    gpuResources_ = {};
    if (destroyedAny)
      ++gpuResourceRevision_;
    return;
  }
  auto deleteBuffer = [&](rendering::Buffer &buffer)
  {
    if (buffer.isValid())
    {
      renderGraph_->deleteBuffer(buffer);
      buffer = {};
      destroyedAny = true;
    }
  };
  deleteBuffer(gpuResources_.sceneInfo);
  deleteBuffer(gpuResources_.instances);
  deleteBuffer(gpuResources_.blases);
  deleteBuffer(gpuResources_.blasNodes);
  deleteBuffer(gpuResources_.blasNodeChildWordPrefixes);
  deleteBuffer(gpuResources_.blasBricks);
  deleteBuffer(gpuResources_.distanceWords);
  deleteBuffer(gpuResources_.tracePageTables);
  deleteBuffer(gpuResources_.tracePages);
  deleteBuffer(gpuResources_.tracePageMasks);
  deleteBuffer(gpuResources_.traceBrickMaps);
  deleteBuffer(gpuResources_.tlasNodes);
  deleteBuffer(gpuResources_.tlasInstanceRefs);
  deleteBuffer(gpuResources_.sceneGridInfo);
  deleteBuffer(gpuResources_.sceneGridCells);
  deleteBuffer(gpuResources_.sceneGridRefs);
  deleteBuffer(gpuResources_.sceneGridGlobalRefs);
  if (gpuResources_.distanceAtlas.isValid())
  {
    renderGraph_->deleteTexture(gpuResources_.distanceAtlas);
    gpuResources_.distanceAtlas = {};
    destroyedAny = true;
  }
  gpuResources_.distanceAtlasBricksPerAxis = 0u;
  if (destroyedAny)
    ++gpuResourceRevision_;
}

} // namespace sdf
