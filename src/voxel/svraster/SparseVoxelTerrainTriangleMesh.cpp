#include "voxel/svraster/SparseVoxelTerrainTriangleMesh.hpp"

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <unordered_map>

namespace voxel::svraster
{
namespace
{
struct CellKey
{
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;

  bool operator==(const CellKey &other) const
  {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct CellKeyHash
{
  size_t operator()(const CellKey &key) const
  {
    const auto mix = [](uint64_t value)
    {
      value ^= value >> 30u;
      value *= 0xbf58476d1ce4e5b9ull;
      value ^= value >> 27u;
      value *= 0x94d049bb133111ebull;
      value ^= value >> 31u;
      return value;
    };
    uint64_t hash = mix(static_cast<uint32_t>(key.x));
    hash ^= mix(static_cast<uint32_t>(key.y) + 0x9e3779b9u) +
        0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= mix(static_cast<uint32_t>(key.z) + 0x85ebca6bu) +
        0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return static_cast<size_t>(hash);
  }
};

struct CellValue
{
  SparseVoxelBrickMaterialGPU material{};
  uint32_t sourceBrickNode = kSparseVoxelTerrainTriangleInvalidIndex;
};

struct BoundsI
{
  std::array<int32_t, 3> min{
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int32_t>::max()};
  std::array<int32_t, 3> max{
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::min()};
};

struct MaskCell
{
  bool filled = false;
  SparseVoxelBrickMaterialGPU material{};
};

using Occupancy = std::unordered_map<CellKey, CellValue, CellKeyHash>;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint32_t populationCount(uint32_t value)
{
  value -= (value >> 1u) & 0x55555555u;
  value = (value & 0x33333333u) + ((value >> 2u) & 0x33333333u);
  value = (value + (value >> 4u)) & 0x0f0f0f0fu;
  return (value * 0x01010101u) >> 24u;
}

bool sameMaterial(
    const SparseVoxelBrickMaterialGPU &left,
    const SparseVoxelBrickMaterialGPU &right)
{
  return left.packed == right.packed;
}

bool contains(
    const Occupancy &occupancy,
    const int32_t x,
    const int32_t y,
    const int32_t z)
{
  return occupancy.find(CellKey{x, y, z}) != occupancy.end();
}

void include(BoundsI &bounds, const std::array<int32_t, 3> &point)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
    bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
  }
}

BoundsI computeOccupancyBounds(const Occupancy &occupancy)
{
  BoundsI bounds{};
  for (const auto &[key, value] : occupancy)
  {
    (void)value;
    include(bounds, {key.x, key.y, key.z});
  }
  return bounds;
}

std::array<int32_t, 3> makeCoord(
    const int axis,
    const int uAxis,
    const int vAxis,
    const int32_t axisValue,
    const int32_t u,
    const int32_t v)
{
  std::array<int32_t, 3> coord{};
  coord[axis] = axisValue;
  coord[uAxis] = u;
  coord[vAxis] = v;
  return coord;
}

std::array<int8_t, 3> normalFor(const int axis, const int32_t sign)
{
  std::array<int8_t, 3> normal{};
  normal[axis] = static_cast<int8_t>(sign);
  return normal;
}

void include(
    SparseVoxelTerrainTriangleCluster &cluster,
    const std::array<float, 3> &point)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    cluster.boundsMin[axis] = std::min(cluster.boundsMin[axis], point[axis]);
    cluster.boundsMax[axis] = std::max(cluster.boundsMax[axis], point[axis]);
  }
}

void beginCluster(
    SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const uint32_t sourceBrickNode,
    const uint32_t lodGroup,
    const float selfError,
    const float parentError)
{
  SparseVoxelTerrainTriangleCluster cluster{};
  cluster.firstVertex = static_cast<uint32_t>(sidecar.vertices.size());
  cluster.firstIndex = static_cast<uint32_t>(sidecar.indices.size());
  cluster.firstQuad = static_cast<uint32_t>(sidecar.greedyQuads);
  cluster.sourceBrickNode = sourceBrickNode;
  cluster.lodGroup = lodGroup;
  cluster.selfError = selfError;
  cluster.parentError = parentError;
  cluster.boundsMin = {
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::max()};
  cluster.boundsMax = {
      -std::numeric_limits<float>::max(),
      -std::numeric_limits<float>::max(),
      -std::numeric_limits<float>::max()};
  sidecar.clusters.push_back(cluster);
}

std::array<float, 3> worldPosition(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const std::array<int32_t, 3> &grid)
{
  return {
      static_cast<float>(
          sidecar.pageMinimum[0] +
          static_cast<double>(grid[0]) * sidecar.voxelSizeMetres),
      static_cast<float>(
          sidecar.pageMinimum[1] +
          static_cast<double>(grid[1]) * sidecar.voxelSizeMetres),
      static_cast<float>(
          sidecar.pageMinimum[2] +
          static_cast<double>(grid[2]) * sidecar.voxelSizeMetres)};
}

void appendQuad(
    SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    const uint32_t sourceBrickNode,
    const uint32_t lodGroup,
    const float selfError,
    const float parentError,
    const int axis,
    const int uAxis,
    const int vAxis,
    const int32_t sign,
    const int32_t axisPlane,
    const int32_t u,
    const int32_t v,
    const int32_t width,
    const int32_t height,
    const SparseVoxelBrickMaterialGPU &material)
{
  const uint32_t maxQuadsPerCluster =
      std::max(settings.maxQuadsPerCluster, 1u);
  if (sidecar.clusters.empty() ||
      sidecar.clusters.back().sourceBrickNode != sourceBrickNode ||
      sidecar.clusters.back().quadCount >= maxQuadsPerCluster)
  {
    beginCluster(sidecar, sourceBrickNode, lodGroup, selfError, parentError);
  }

  auto &cluster = sidecar.clusters.back();
  const auto normal = normalFor(axis, sign);
  std::array<std::array<int32_t, 3>, 4> corners{};
  corners[0] = makeCoord(axis, uAxis, vAxis, axisPlane, u, v);
  corners[1] = makeCoord(axis, uAxis, vAxis, axisPlane, u + width, v);
  corners[2] = makeCoord(axis, uAxis, vAxis, axisPlane, u + width, v + height);
  corners[3] = makeCoord(axis, uAxis, vAxis, axisPlane, u, v + height);
  if (sign < 0)
    std::swap(corners[1], corners[3]);

  const uint32_t firstVertex = static_cast<uint32_t>(sidecar.vertices.size());
  for (uint32_t corner = 0u; corner < 4u; ++corner)
  {
    SparseVoxelTerrainTriangleVertex vertex{};
    vertex.position = worldPosition(sidecar, corners[corner]);
    vertex.normal = normal;
    vertex.face =
        static_cast<uint8_t>(axis * 2 + (sign > 0 ? 1 : 0));
    vertex.uv = {
        static_cast<float>(corner == 1u || corner == 2u ? width : 0),
        static_cast<float>(corner >= 2u ? height : 0)};
    vertex.material = material;
    include(cluster, vertex.position);
    sidecar.vertices.push_back(vertex);
  }
  sidecar.indices.insert(
      sidecar.indices.end(),
      {firstVertex + 0u,
       firstVertex + 1u,
       firstVertex + 2u,
       firstVertex + 0u,
       firstVertex + 2u,
       firstVertex + 3u});
  cluster.vertexCount += 4u;
  cluster.indexCount += 6u;
  ++cluster.quadCount;
  ++sidecar.greedyQuads;
}

void greedyMeshBounds(
    const Occupancy &occupancy,
    const std::unordered_map<uint32_t, uint32_t> &nodeToLODGroup,
    const std::vector<SparseVoxelBrickNodeGPU> &nodes,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    const uint32_t sourceBrickNode,
    const BoundsI &bounds,
    SparseVoxelTerrainTrianglePageSidecar &sidecar)
{
  if (bounds.min[0] > bounds.max[0] ||
      bounds.min[1] > bounds.max[1] ||
      bounds.min[2] > bounds.max[2])
    return;

  uint32_t lodGroup = kSparseVoxelBrickLODGroupInvalidIndex;
  float selfError = 0.0f;
  float parentError = 0.0f;
  const auto groupIt = nodeToLODGroup.find(sourceBrickNode);
  if (groupIt != nodeToLODGroup.end())
    lodGroup = groupIt->second;
  if (sourceBrickNode < nodes.size())
  {
    selfError = nodes[sourceBrickNode].selfError;
    parentError = nodes[sourceBrickNode].parentError;
  }

  for (int axis = 0; axis < 3; ++axis)
  {
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;
    const int32_t uMin = bounds.min[uAxis];
    const int32_t uMax = bounds.max[uAxis];
    const int32_t vMin = bounds.min[vAxis];
    const int32_t vMax = bounds.max[vAxis];
    const int32_t uCount = uMax >= uMin ? uMax - uMin + 1 : 0;
    const int32_t vCount = vMax >= vMin ? vMax - vMin + 1 : 0;
    if (uCount <= 0 || vCount <= 0)
      continue;

    std::vector<MaskCell> mask(
        static_cast<size_t>(uCount) * static_cast<size_t>(vCount));
    for (int signIndex = 0; signIndex < 2; ++signIndex)
    {
      const int32_t sign = signIndex == 0 ? -1 : 1;
      for (int32_t axisValue = bounds.min[axis];
           axisValue <= bounds.max[axis];
           ++axisValue)
      {
        std::fill(mask.begin(), mask.end(), MaskCell{});
        for (int32_t v = vMin; v <= vMax; ++v)
        {
          for (int32_t u = uMin; u <= uMax; ++u)
          {
            const auto coord = makeCoord(axis, uAxis, vAxis, axisValue, u, v);
            const auto cell =
                occupancy.find(CellKey{coord[0], coord[1], coord[2]});
            if (cell == occupancy.end())
              continue;
            auto neighbour = coord;
            neighbour[axis] += sign;
            if (contains(
                    occupancy,
                    neighbour[0],
                    neighbour[1],
                    neighbour[2]))
              continue;
            mask[static_cast<size_t>(v - vMin) *
                     static_cast<size_t>(uCount) +
                 static_cast<size_t>(u - uMin)] =
                MaskCell{true, cell->second.material};
          }
        }

        for (int32_t v = 0; v < vCount; ++v)
        {
          for (int32_t u = 0; u < uCount;)
          {
            auto &origin =
                mask[static_cast<size_t>(v) *
                         static_cast<size_t>(uCount) +
                     static_cast<size_t>(u)];
            if (!origin.filled)
            {
              ++u;
              continue;
            }

            int32_t width = 1;
            while (!settings.emitOneQuadPerExposedFace &&
                   u + width < uCount)
            {
              const auto &candidate =
                  mask[static_cast<size_t>(v) *
                           static_cast<size_t>(uCount) +
                       static_cast<size_t>(u + width)];
              if (!candidate.filled ||
                  !sameMaterial(candidate.material, origin.material))
                break;
              ++width;
            }

            int32_t height = 1;
            for (; !settings.emitOneQuadPerExposedFace &&
                 v + height < vCount; ++height)
            {
              bool rowMatches = true;
              for (int32_t x = 0; x < width; ++x)
              {
                const auto &candidate =
                    mask[static_cast<size_t>(v + height) *
                             static_cast<size_t>(uCount) +
                         static_cast<size_t>(u + x)];
                if (!candidate.filled ||
                    !sameMaterial(candidate.material, origin.material))
                {
                  rowMatches = false;
                  break;
                }
              }
              if (!rowMatches)
                break;
            }

            for (int32_t y = 0; y < height; ++y)
              for (int32_t x = 0; x < width; ++x)
                mask[static_cast<size_t>(v + y) *
                         static_cast<size_t>(uCount) +
                     static_cast<size_t>(u + x)]
                    .filled = false;

            appendQuad(
                sidecar,
                settings,
                sourceBrickNode,
                lodGroup,
                selfError,
                parentError,
                axis,
                uAxis,
                vAxis,
                sign,
                axisValue + (sign > 0 ? 1 : 0),
                uMin + u,
                vMin + v,
                width,
                height,
                origin.material);
            u += width;
          }
        }
      }
    }
  }
}

bool decodeChunkOccupancy(
    const PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex,
    SparseVoxelTerrainTrianglePageSidecar &sidecar,
    Occupancy &occupancy,
    std::unordered_map<uint32_t, BoundsI> &nodeBounds,
    std::string *error)
{
  if (chunkIndex >= chunks.chunks.size())
    return fail(error, "Triangle terrain sidecar needs a valid chunk index");
  const auto &record = chunks.chunks[chunkIndex];
  if (!(record.voxelSizeMetres > 0.0) ||
      !(record.edgeMetres > 0.0) ||
      record.brickNodeOffset > chunks.bricks.nodes.size() ||
      record.brickNodeCapacity >
          chunks.bricks.nodes.size() - record.brickNodeOffset)
    return fail(error, "Triangle terrain sidecar found invalid page ranges");

  sidecar.chunkIndex = chunkIndex;
  sidecar.brickPayloadRevision = record.brickPayloadRevision;
  sidecar.tree64Revision = record.tree64Revision;
  sidecar.clipmapLOD = record.clipmapLOD;
  sidecar.regionMappingId = record.regionMappingId;
  sidecar.voxelSizeMetres = record.voxelSizeMetres;
  sidecar.edgeMetres = record.edgeMetres;
  sidecar.pageMinimum = {
      record.worldCenterXMetres - record.edgeMetres * 0.5,
      record.elevationOriginMetres - chunks.elevationOriginMetres,
      record.worldCenterZMetres - record.edgeMetres * 0.5};

  const uint32_t nodeEnd = record.brickNodeOffset + record.brickNodeCapacity;
  occupancy.reserve(record.leafVoxelCount * 2u + 1u);
  for (uint32_t nodeIndex = record.brickNodeOffset;
       nodeIndex < nodeEnd;
       ++nodeIndex)
  {
    const auto &node = chunks.bricks.nodes[nodeIndex];
    if (std::abs(
            static_cast<double>(node.minimumVoxelSize[3]) -
            record.voxelSizeMetres) >
        std::max(1.0e-6, record.voxelSizeMetres * 1.0e-5))
      continue;
    if (node.occupancyIndex >= chunks.bricks.occupancy.size())
      return fail(error, "Triangle terrain sidecar found invalid occupancy");
    const auto &brickOccupancy = chunks.bricks.occupancy[node.occupancyIndex];
    for (uint32_t linearCell = 0u;
         linearCell < kSparseVoxelBrickVoxelCount;
         ++linearCell)
    {
      const uint32_t word = linearCell / 32u;
      const uint32_t bit = linearCell & 31u;
      const uint32_t bits = brickOccupancy.cells[word].bits;
      if ((bits & (uint32_t{1u} << bit)) == 0u)
        continue;

      const uint32_t preceding =
          bit == 0u ? 0u : bits & ((uint32_t{1u} << bit) - 1u);
      const uint64_t materialAddress =
          uint64_t{node.materialBase} +
          brickOccupancy.cells[word].prefix +
          populationCount(preceding);
      if (materialAddress >= chunks.bricks.materialIndices.size())
        return fail(error, "Triangle terrain sidecar found invalid material address");
      const uint64_t paletteAddress =
          uint64_t{node.paletteBase} +
          chunks.bricks.materialIndices[materialAddress];
      if (paletteAddress >= chunks.bricks.palette.size())
        return fail(error, "Triangle terrain sidecar found invalid palette address");

      const std::array<uint32_t, 3> local{
          linearCell % kSparseVoxelBrickSide,
          (linearCell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide,
          linearCell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide)};

      CellKey key{};
      bool coordinateValid = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double coordinate =
            (static_cast<double>(node.minimumVoxelSize[axis]) +
             static_cast<double>(local[axis]) * record.voxelSizeMetres -
             sidecar.pageMinimum[axis]) /
            record.voxelSizeMetres;
        const double rounded = std::round(coordinate);
        coordinateValid =
            coordinateValid && std::isfinite(coordinate) &&
            std::abs(coordinate - rounded) <=
                std::max(1.0e-4, std::abs(coordinate) * 1.0e-6) &&
            rounded >= std::numeric_limits<int32_t>::min() &&
            rounded <= std::numeric_limits<int32_t>::max();
        const int32_t cellCoordinate = static_cast<int32_t>(rounded);
        if (axis == 0u)
          key.x = cellCoordinate;
        else if (axis == 1u)
          key.y = cellCoordinate;
        else
          key.z = cellCoordinate;
      }
      if (!coordinateValid)
        return fail(error, "Triangle terrain sidecar found a misaligned cell");

      occupancy.insert_or_assign(
          key,
          CellValue{chunks.bricks.palette[paletteAddress], nodeIndex});
      include(nodeBounds[nodeIndex], {key.x, key.y, key.z});
    }
  }
  sidecar.occupiedCells = occupancy.size();
  return true;
}

uint64_t countExposedFaces(const Occupancy &occupancy)
{
  static constexpr std::array<std::array<int32_t, 3>, 6> kDirections{{
      {{1, 0, 0}},
      {{-1, 0, 0}},
      {{0, 1, 0}},
      {{0, -1, 0}},
      {{0, 0, 1}},
      {{0, 0, -1}},
  }};
  uint64_t faces = 0u;
  for (const auto &[key, value] : occupancy)
  {
    (void)value;
    for (const auto &direction : kDirections)
      if (!contains(
              occupancy,
              key.x + direction[0],
              key.y + direction[1],
              key.z + direction[2]))
        ++faces;
  }
  return faces;
}

std::unordered_map<uint32_t, uint32_t> buildNodeToGroupMap(
    const SparseVoxelBrickHierarchy &hierarchy)
{
  std::unordered_map<uint32_t, uint32_t> result;
  result.reserve(hierarchy.nodeLODGroupIndices.size());
  for (uint32_t nodeIndex = 0u;
       nodeIndex < hierarchy.nodeLODGroupIndices.size();
       ++nodeIndex)
  {
    const uint32_t groupIndex = hierarchy.nodeLODGroupIndices[nodeIndex];
    if (groupIndex != kSparseVoxelBrickLODGroupInvalidIndex)
      result.emplace(nodeIndex, groupIndex);
  }
  return result;
}

void hashBytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

template <typename T> void hashValue(uint64_t &hash, const T &value)
{
  hashBytes(hash, &value, sizeof(T));
}

bool finite(const float value)
{
  return std::isfinite(value);
}

bool finite(const double value)
{
  return std::isfinite(value);
}

bool nearlyEqual(const float left, const float right)
{
  const float scale = std::max({1.0f, std::abs(left), std::abs(right)});
  return std::abs(left - right) <= scale * 1.0e-5f;
}

bool validateNormalAndFace(
    const SparseVoxelTerrainTriangleVertex &vertex,
    std::string *error)
{
  uint32_t nonZeroAxis = 3u;
  uint32_t nonZeroCount = 0u;
  int8_t sign = 0;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int8_t value = vertex.normal[axis];
    if (value == 0)
      continue;
    if (value != int8_t{-1} && value != int8_t{1})
      return fail(error, "Triangle terrain sidecar has a non-unit normal");
    nonZeroAxis = axis;
    sign = value;
    ++nonZeroCount;
  }
  if (nonZeroCount != 1u)
    return fail(error, "Triangle terrain sidecar has a non-axis-aligned normal");
  const uint8_t expectedFace =
      static_cast<uint8_t>(nonZeroAxis * 2u + (sign > 0 ? 1u : 0u));
  if (vertex.face != expectedFace)
    return fail(error, "Triangle terrain sidecar face id does not match normal");
  return true;
}

} // namespace

bool buildSparseVoxelTerrainTrianglePageSidecar(
    const PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    SparseVoxelTerrainTrianglePageSidecar &result,
    std::string *error)
{
  result = {};
  if (error != nullptr)
    error->clear();
  if (settings.maxQuadsPerCluster == 0u)
    return fail(error, "Triangle terrain sidecar needs non-zero cluster capacity");

  Occupancy occupancy;
  std::unordered_map<uint32_t, BoundsI> nodeBounds;
  if (!decodeChunkOccupancy(
          chunks,
          chunkIndex,
          result,
          occupancy,
          nodeBounds,
          error))
    return false;
  if (occupancy.empty())
    return true;

  result.exposedFaces = countExposedFaces(occupancy);
  const auto nodeToGroup = buildNodeToGroupMap(chunks.bricks);
  if (settings.brickLocalGreedy)
  {
    std::vector<uint32_t> nodeIndices;
    nodeIndices.reserve(nodeBounds.size());
    for (const auto &[nodeIndex, bounds] : nodeBounds)
    {
      (void)bounds;
      nodeIndices.push_back(nodeIndex);
    }
    std::sort(nodeIndices.begin(), nodeIndices.end());
    for (const uint32_t nodeIndex : nodeIndices)
      greedyMeshBounds(
          occupancy,
          nodeToGroup,
          chunks.bricks.nodes,
          settings,
          nodeIndex,
          nodeBounds[nodeIndex],
          result);
  }
  else
  {
    greedyMeshBounds(
        occupancy,
        nodeToGroup,
        chunks.bricks.nodes,
        settings,
        kSparseVoxelTerrainTriangleInvalidIndex,
        computeOccupancyBounds(occupancy),
        result);
  }
  return true;
}

SparseVoxelTerrainTriangleVirtualGeometryPreview
summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar)
{
  SparseVoxelTerrainTriangleVirtualGeometryPreview result{};
  result.pageCount = sidecar.empty() ? 0u : 1u;
  result.hierarchyNodeCount = sidecar.empty() ? 0u : 1u;
  result.meshletCount = static_cast<uint32_t>(sidecar.clusters.size());

  std::vector<uint32_t> groups;
  groups.reserve(sidecar.clusters.size());
  for (const auto &cluster : sidecar.clusters)
  {
    result.maxTrianglesPerMeshlet =
        std::max(result.maxTrianglesPerMeshlet, cluster.indexCount / 3u);
    result.maxVerticesPerMeshlet =
        std::max(result.maxVerticesPerMeshlet, cluster.vertexCount);
    if (cluster.lodGroup != kSparseVoxelBrickLODGroupInvalidIndex)
      groups.push_back(cluster.lodGroup);
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  result.groupCount = static_cast<uint32_t>(
      groups.empty() && !sidecar.empty() ? 1u : groups.size());

  result.estimatedVertexBytes =
      sidecar.vertices.size() * sizeof(SparseVoxelTerrainTriangleVertex);
  result.estimatedIndexBytes = sidecar.indices.size() * sizeof(uint32_t);
  result.estimatedClusterBytes =
      sidecar.clusters.size() * sizeof(SparseVoxelTerrainTriangleCluster);
  result.estimatedTotalBytes =
      result.estimatedVertexBytes +
      result.estimatedIndexBytes +
      result.estimatedClusterBytes;
  // Meshlet::vertex_count is a uint8, so a cluster over MaxVertices does not
  // fail loudly - it wraps and decodes as an empty meshlet.
  result.virtualGeometryCompatible =
      result.meshletCount > 0u &&
      result.maxTrianglesPerMeshlet <= virtualgeometry::ClusterSize &&
      result.maxVerticesPerMeshlet <= virtualgeometry::MaxVertices;
  return result;
}

bool buildSparseVoxelTerrainTriangleVirtualGeometryPreview(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    virtualgeometry::VirtualGeometryBuildData &result,
    std::string *error)
{
  result = {};
  if (error != nullptr)
    error->clear();
  const auto summary =
      summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview(sidecar);
  if (!summary.virtualGeometryCompatible && !sidecar.empty())
    return fail(error, "Triangle terrain sidecar has clusters too large for virtual geometry meshlets");
  if (sidecar.empty())
    return true;

  result.buildSettings.maxGroupsPerPage = virtualgeometry::MAX_GROUPS_PER_PAGE;
  result.shapes.push_back({});
  result.shapes.back().root_node_index = 0u;
  result.shapes.back().root_page_index = 0u;
  result.shapes.back().hierarchy_node_count = 1u;
  result.meshParts.push_back({});

  BoundsI ignored{};
  (void)ignored;
  virtualgeometry::VirtualGeometryHierarchy root{};
  root.pageIndex = 0u;
  root.meshPartIndex = 0u;
  root.child_start = 0u;
  root.child_count = summary.meshletCount;
  root.flags = virtualgeometry::HIERARCHY_LEAF_FLAG;
  root.min_lod_error = std::numeric_limits<float>::max();
  root.max_parent_lod_error = 0.0f;
  root.min_x = root.min_y = root.min_z = std::numeric_limits<float>::max();
  root.max_x = root.max_y = root.max_z = -std::numeric_limits<float>::max();

  result.clusters.reserve(sidecar.clusters.size());
  for (uint32_t clusterIndex = 0u;
       clusterIndex < sidecar.clusters.size();
       ++clusterIndex)
  {
    const auto &source = sidecar.clusters[clusterIndex];
    virtualgeometry::VirtualGeometryCluster cluster{};
    cluster.groupId =
        source.lodGroup == kSparseVoxelBrickLODGroupInvalidIndex
        ? 0u
        : source.lodGroup;
    cluster.meshPartIndex = 0u;
    cluster.self.error = source.selfError;
    cluster.parent.error = source.parentError;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      cluster.self.center[axis] =
          (source.boundsMin[axis] + source.boundsMax[axis]) * 0.5f;
      cluster.parent.center[axis] = cluster.self.center[axis];
      root.min_x = axis == 0u ? std::min(root.min_x, source.boundsMin[axis]) : root.min_x;
      root.max_x = axis == 0u ? std::max(root.max_x, source.boundsMax[axis]) : root.max_x;
      root.min_y = axis == 1u ? std::min(root.min_y, source.boundsMin[axis]) : root.min_y;
      root.max_y = axis == 1u ? std::max(root.max_y, source.boundsMax[axis]) : root.max_y;
      root.min_z = axis == 2u ? std::min(root.min_z, source.boundsMin[axis]) : root.min_z;
      root.max_z = axis == 2u ? std::max(root.max_z, source.boundsMax[axis]) : root.max_z;
    }
    const float dx = source.boundsMax[0] - source.boundsMin[0];
    const float dy = source.boundsMax[1] - source.boundsMin[1];
    const float dz = source.boundsMax[2] - source.boundsMin[2];
    cluster.self.radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
    cluster.parent.radius = cluster.self.radius;
    root.min_lod_error = std::min(root.min_lod_error, cluster.self.error);
    root.max_parent_lod_error =
        std::max(root.max_parent_lod_error, cluster.parent.error);

    cluster.vertices.reserve(source.vertexCount);
    for (uint32_t vertexIndex = 0u; vertexIndex < source.vertexCount; ++vertexIndex)
    {
      const auto &vertex = sidecar.vertices[source.firstVertex + vertexIndex];
      virtualgeometry::Vertex out{};
      out.pos[0] = vertex.position[0];
      out.pos[1] = vertex.position[1];
      out.pos[2] = vertex.position[2];
      out.norm[0] = static_cast<float>(vertex.normal[0]);
      out.norm[1] = static_cast<float>(vertex.normal[1]);
      out.norm[2] = static_cast<float>(vertex.normal[2]);
      out.uv[0] = vertex.uv[0];
      out.uv[1] = vertex.uv[1];
      cluster.vertices.push_back(out);
    }
    cluster.indices.reserve(source.indexCount);
    for (uint32_t index = 0u; index < source.indexCount; ++index)
      cluster.indices.push_back(
          sidecar.indices[source.firstIndex + index] - source.firstVertex);
    result.clusters.push_back(std::move(cluster));
  }
  root.max_center_x = (root.min_x + root.max_x) * 0.5f;
  root.max_center_y = (root.min_y + root.max_y) * 0.5f;
  root.max_center_z = (root.min_z + root.max_z) * 0.5f;
  const float rdx = root.max_x - root.min_x;
  const float rdy = root.max_y - root.min_y;
  const float rdz = root.max_z - root.min_z;
  root.max_radius = std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz) * 0.5f;
  if (root.min_lod_error == std::numeric_limits<float>::max())
    root.min_lod_error = 0.0f;
  result.lodLevelHierarchy.push_back(root);

  virtualgeometry::VirtualGeometryBuildPage page{};
  page.clusterOffset = 0u;
  page.clusterCount = summary.meshletCount;
  page.hierarchyOffset = 0u;
  page.hierarchyCount = 1u;
  page.maxHierarchyDepth = 1u;
  result.pages.push_back(std::move(page));

  virtualgeometry::ClusterGroupInfo group{};
  group.lodLevel = sidecar.clipmapLOD;
  for (uint32_t clusterIndex = 0u;
       clusterIndex < result.clusters.size();
       ++clusterIndex)
    group.originalClusterIndices.push_back(static_cast<int32_t>(clusterIndex));
  result.groupInfos.push_back(std::move(group));
  result.buildToLinearizedClusterIndex.resize(result.clusters.size());
  for (uint32_t index = 0u; index < result.buildToLinearizedClusterIndex.size(); ++index)
    result.buildToLinearizedClusterIndex[index] = index;
  return true;
}

bool validateSparseVoxelTerrainTrianglePageSidecar(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.maxQuadsPerCluster == 0u)
    return fail(error, "Triangle terrain sidecar validation needs non-zero cluster capacity");
  if (!finite(sidecar.voxelSizeMetres) || sidecar.voxelSizeMetres <= 0.0 ||
      !finite(sidecar.edgeMetres) || sidecar.edgeMetres <= 0.0)
    return fail(error, "Triangle terrain sidecar has invalid page dimensions");
  for (const double value : sidecar.pageMinimum)
    if (!finite(value))
      return fail(error, "Triangle terrain sidecar has invalid page origin");

  if (sidecar.clusters.empty())
  {
    if (!sidecar.vertices.empty() || !sidecar.indices.empty() ||
        sidecar.greedyQuads != 0u)
      return fail(error, "Empty triangle terrain sidecar has payload data");
    return true;
  }

  if (sidecar.greedyQuads == 0u)
    return fail(error, "Triangle terrain sidecar has clusters without quads");
  if (sidecar.exposedFaces < sidecar.greedyQuads)
    return fail(error, "Triangle terrain sidecar has more greedy quads than exposed faces");
  if (settings.emitOneQuadPerExposedFace &&
      sidecar.greedyQuads != sidecar.exposedFaces)
    return fail(error, "Face-local triangle terrain sidecar must emit one quad per exposed voxel face");
  if (sidecar.vertices.size() != sidecar.greedyQuads * 4u)
    return fail(error, "Triangle terrain sidecar vertex count does not match exact quads");
  if (sidecar.indices.size() != sidecar.greedyQuads * 6u)
    return fail(error, "Triangle terrain sidecar index count does not match exact quads");

  const float pageMin[3] = {
      static_cast<float>(sidecar.pageMinimum[0]),
      static_cast<float>(sidecar.pageMinimum[1]),
      static_cast<float>(sidecar.pageMinimum[2])};
  const float pageMax[3] = {
      static_cast<float>(sidecar.pageMinimum[0] + sidecar.edgeMetres),
      static_cast<float>(sidecar.pageMinimum[1] + sidecar.edgeMetres),
      static_cast<float>(sidecar.pageMinimum[2] + sidecar.edgeMetres)};
  const float pageEpsilon =
      std::max(1.0e-4f, static_cast<float>(sidecar.voxelSizeMetres) * 1.0e-3f);

  uint64_t runningQuads = 0u;
  uint64_t runningVertices = 0u;
  uint64_t runningIndices = 0u;
  for (const auto &cluster : sidecar.clusters)
  {
    if (cluster.quadCount == 0u ||
        cluster.quadCount > settings.maxQuadsPerCluster)
      return fail(error, "Triangle terrain sidecar cluster exceeds quad budget");
    if (cluster.vertexCount != cluster.quadCount * 4u ||
        cluster.indexCount != cluster.quadCount * 6u)
      return fail(error, "Triangle terrain sidecar cluster counts do not match exact quads");
    if (cluster.firstQuad != runningQuads ||
        cluster.firstVertex != runningVertices ||
        cluster.firstIndex != runningIndices)
      return fail(error, "Triangle terrain sidecar cluster ranges are not contiguous");
    if (uint64_t{cluster.firstVertex} + cluster.vertexCount >
            sidecar.vertices.size() ||
        uint64_t{cluster.firstIndex} + cluster.indexCount >
            sidecar.indices.size())
      return fail(error, "Triangle terrain sidecar cluster range is out of bounds");
    if (!finite(cluster.selfError) || !finite(cluster.parentError) ||
        cluster.selfError < 0.0f || cluster.parentError < 0.0f)
      return fail(error, "Triangle terrain sidecar cluster has invalid error metrics");

    std::array<float, 3> recomputedMin{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> recomputedMax{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};

    for (uint32_t vertexOffset = 0u;
         vertexOffset < cluster.vertexCount;
         ++vertexOffset)
    {
      const auto &vertex = sidecar.vertices[cluster.firstVertex + vertexOffset];
      if (!validateNormalAndFace(vertex, error))
        return false;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const float position = vertex.position[axis];
        if (!finite(position))
          return fail(error, "Triangle terrain sidecar vertex has invalid position");
        if (axis != 1u &&
            (position < pageMin[axis] - pageEpsilon ||
             position > pageMax[axis] + pageEpsilon))
          return fail(error, "Triangle terrain sidecar vertex escaped page bounds");
        recomputedMin[axis] = std::min(recomputedMin[axis], position);
        recomputedMax[axis] = std::max(recomputedMax[axis], position);
      }
      for (const float value : vertex.uv)
        if (!finite(value) || value < 0.0f)
          return fail(error, "Triangle terrain sidecar vertex has invalid UV");
    }

    for (uint32_t indexOffset = 0u;
         indexOffset < cluster.indexCount;
         ++indexOffset)
    {
      const uint32_t index = sidecar.indices[cluster.firstIndex + indexOffset];
      if (index < cluster.firstVertex ||
          index >= cluster.firstVertex + cluster.vertexCount)
        return fail(error, "Triangle terrain sidecar cluster index escapes cluster vertices");
    }
    if (settings.emitOneQuadPerExposedFace)
    {
      for (uint32_t quad = 0u; quad < cluster.quadCount; ++quad)
      {
        const uint32_t vertexBase = cluster.firstVertex + quad * 4u;
        const uint32_t indexBase = cluster.firstIndex + quad * 6u;
        const std::array<uint32_t, 6> expected{
          vertexBase + 0u, vertexBase + 1u, vertexBase + 2u,
          vertexBase + 0u, vertexBase + 2u, vertexBase + 3u};
        for (uint32_t lane = 0u; lane < expected.size(); ++lane)
          if (sidecar.indices[indexBase + lane] != expected[lane])
            return fail(error, "Face-local triangle terrain sidecar reuses vertices across voxel faces");

        const auto &firstVertex = sidecar.vertices[vertexBase];
        const std::array<std::array<float, 2>, 4> expectedUVs{{
          {{0.0f, 0.0f}},
          {{1.0f, 0.0f}},
          {{1.0f, 1.0f}},
          {{0.0f, 1.0f}},
        }};
        for (uint32_t corner = 0u; corner < 4u; ++corner)
        {
          const auto &vertex = sidecar.vertices[vertexBase + corner];
          if (vertex.normal != firstVertex.normal ||
              vertex.face != firstVertex.face ||
              vertex.material.packed != firstVertex.material.packed)
            return fail(error, "Face-local triangle terrain sidecar mixes normal/face/material within a voxel face");
          if (!nearlyEqual(vertex.uv[0], expectedUVs[corner][0]) ||
              !nearlyEqual(vertex.uv[1], expectedUVs[corner][1]))
            return fail(error, "Face-local triangle terrain sidecar does not use the canonical cube-face UV unwrap");
        }
      }
    }

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (!finite(cluster.boundsMin[axis]) ||
          !finite(cluster.boundsMax[axis]) ||
          cluster.boundsMin[axis] > cluster.boundsMax[axis])
        return fail(error, "Triangle terrain sidecar cluster has invalid bounds");
      if (!nearlyEqual(cluster.boundsMin[axis], recomputedMin[axis]) ||
          !nearlyEqual(cluster.boundsMax[axis], recomputedMax[axis]))
        return fail(error, "Triangle terrain sidecar cluster bounds do not match vertices");
    }

    runningQuads += cluster.quadCount;
    runningVertices += cluster.vertexCount;
    runningIndices += cluster.indexCount;
  }

  if (runningQuads != sidecar.greedyQuads ||
      runningVertices != sidecar.vertices.size() ||
      runningIndices != sidecar.indices.size())
    return fail(error, "Triangle terrain sidecar totals do not match cluster ranges");

  return true;
}

uint64_t sparseVoxelTerrainTrianglePageSidecarHash(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar)
{
  uint64_t hash = 1469598103934665603ull;
  hashValue(hash, sidecar.chunkIndex);
  hashValue(hash, sidecar.brickPayloadRevision);
  hashValue(hash, sidecar.tree64Revision);
  hashValue(hash, sidecar.clipmapLOD);
  hashValue(hash, sidecar.regionMappingId);
  hashValue(hash, sidecar.voxelSizeMetres);
  hashValue(hash, sidecar.edgeMetres);
  for (const auto &value : sidecar.pageMinimum)
    hashValue(hash, value);
  hashValue(hash, sidecar.occupiedCells);
  hashValue(hash, sidecar.exposedFaces);
  hashValue(hash, sidecar.greedyQuads);
  for (const auto &vertex : sidecar.vertices)
  {
    for (const float value : vertex.position)
      hashValue(hash, value);
    for (const int8_t value : vertex.normal)
      hashValue(hash, value);
    hashValue(hash, vertex.face);
    for (const float value : vertex.uv)
      hashValue(hash, value);
    for (const uint32_t value : vertex.material.packed)
      hashValue(hash, value);
  }
  for (const uint32_t index : sidecar.indices)
    hashValue(hash, index);
  for (const auto &cluster : sidecar.clusters)
  {
    hashValue(hash, cluster.firstVertex);
    hashValue(hash, cluster.vertexCount);
    hashValue(hash, cluster.firstIndex);
    hashValue(hash, cluster.indexCount);
    hashValue(hash, cluster.firstQuad);
    hashValue(hash, cluster.quadCount);
    hashValue(hash, cluster.sourceBrickNode);
    hashValue(hash, cluster.lodGroup);
    for (const float value : cluster.boundsMin)
      hashValue(hash, value);
    for (const float value : cluster.boundsMax)
      hashValue(hash, value);
    hashValue(hash, cluster.selfError);
    hashValue(hash, cluster.parentError);
  }
  return hash;
}

} // namespace voxel::svraster
