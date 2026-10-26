#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "editor/virtualgeometry/SparseSDFBuilder.hpp"
#include "sdf/SDFSceneFile.hpp"
#include "sdf/SDFSceneManager.hpp"

namespace
{

namespace fs = std::filesystem;

bool nearEqual(float a, float b, float eps = 1.0e-5f)
{
  return std::fabs(a - b) <= eps;
}

uint32_t countNonZeroBytes(const std::vector<uint8_t> &bytes)
{
  uint32_t count = 0u;
  for (uint8_t byte : bytes)
    count += byte != 0u ? 1u : 0u;
  return count;
}

uint32_t countNonZeroWords(const std::vector<uint32_t> &words)
{
  uint32_t count = 0u;
  for (uint32_t word : words)
    count += word != 0u ? 1u : 0u;
  return count;
}

uint32_t countValidTraceBricks(const std::vector<uint32_t> &words)
{
  uint32_t count = 0u;
  for (uint32_t word : words)
    count += word != sdf::VSDF_TRACE_INVALID_BRICK ? 1u : 0u;
  return count;
}

fs::path tempPath(const std::string &name)
{
  return fs::temp_directory_path() / name;
}

virtualgeometry::VirtualGeometrySourceAsset makeTriangleAsset()
{
  virtualgeometry::VirtualGeometrySourceAsset asset;
  asset.vertices = {
      virtualgeometry::Vertex(0.0f, 0.0f, 0.0f),
      virtualgeometry::Vertex(1.0f, 0.0f, 0.0f),
      virtualgeometry::Vertex(0.0f, 1.0f, 0.0f),
  };

  virtualgeometry::MeshPart part;
  part.shape.indices = {0u, 1u, 2u};

  virtualgeometry::SourceShapeBuildInput shape;
  shape.meshParts.push_back(part);
  asset.shapes.push_back(shape);
  return asset;
}

virtualgeometry::VirtualGeometrySourceAsset makeTwoTriangleAsset()
{
  virtualgeometry::VirtualGeometrySourceAsset asset = makeTriangleAsset();
  asset.vertices.push_back(virtualgeometry::Vertex(2.0f, 0.0f, 0.0f));
  asset.vertices.push_back(virtualgeometry::Vertex(3.0f, 0.0f, 0.0f));
  asset.vertices.push_back(virtualgeometry::Vertex(2.0f, 1.0f, 0.0f));

  virtualgeometry::MeshPart part;
  part.shape.indices = {3u, 4u, 5u};

  virtualgeometry::SourceShapeBuildInput shape;
  shape.materialIndex = 1u;
  shape.meshParts.push_back(part);
  asset.shapes.push_back(shape);
  return asset;
}

virtualgeometry::VirtualGeometrySourceAsset makeCubeAsset()
{
  virtualgeometry::VirtualGeometrySourceAsset asset;
  asset.vertices = {
      virtualgeometry::Vertex(-1.0f, -1.0f, -1.0f),
      virtualgeometry::Vertex(1.0f, -1.0f, -1.0f),
      virtualgeometry::Vertex(1.0f, 1.0f, -1.0f),
      virtualgeometry::Vertex(-1.0f, 1.0f, -1.0f),
      virtualgeometry::Vertex(-1.0f, -1.0f, 1.0f),
      virtualgeometry::Vertex(1.0f, -1.0f, 1.0f),
      virtualgeometry::Vertex(1.0f, 1.0f, 1.0f),
      virtualgeometry::Vertex(-1.0f, 1.0f, 1.0f),
  };

  virtualgeometry::MeshPart part;
  part.shape.indices = {
      0u, 2u, 1u, 0u, 3u, 2u,
      4u, 5u, 6u, 4u, 6u, 7u,
      0u, 1u, 5u, 0u, 5u, 4u,
      1u, 2u, 6u, 1u, 6u, 5u,
      2u, 3u, 7u, 2u, 7u, 6u,
      3u, 0u, 4u, 3u, 4u, 7u};

  virtualgeometry::SourceShapeBuildInput shape;
  shape.meshParts.push_back(part);
  asset.shapes.push_back(shape);
  return asset;
}

virtualgeometry::VirtualGeometrySourceAsset makeNearVoxelFaceTriangleAsset()
{
  virtualgeometry::VirtualGeometrySourceAsset asset;
  asset.vertices = {
      virtualgeometry::Vertex(0.0f, 0.0f, 0.500001f),
      virtualgeometry::Vertex(1.0f, 0.0f, 0.500001f),
      virtualgeometry::Vertex(0.0f, 1.0f, 0.500001f),
      virtualgeometry::Vertex(0.0f, 0.0f, 0.0f),
      virtualgeometry::Vertex(0.0f, 0.0f, 1.0f),
      virtualgeometry::Vertex(0.0f, 0.0f, 1.0f),
  };

  virtualgeometry::MeshPart part;
  part.shape.indices = {
      0u, 1u, 2u,
      3u, 4u, 5u,
  };

  virtualgeometry::SourceShapeBuildInput shape;
  shape.meshParts.push_back(part);
  asset.shapes.push_back(shape);
  return asset;
}

void testVSDFLZ4RoundTrip()
{
  const fs::path path = tempPath("VSDFLZ4RoundTrip.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  settings.requestedError = 0.25f;
  const virtualgeometry::VirtualGeometrySourceAsset asset = makeTriangleAsset();

  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), asset, settings, &error));

  sdf::SDFSceneFile file;
  assert(file.load(path.string(), &error));
  assert(file.header().magic == sdf::VSDF_MAGIC);
  assert(file.header().instanceCount == 1u);
  assert(file.instances().size() == 1u);
  assert(file.instances()[0].compression == sdf::VSDF_COMPRESSION_LZ4);
  assert(file.instances()[0].compressedSize > 0u);
  assert(file.instances()[0].uncompressedSize > 0u);

  sdf::VSDFDecodedInstance instance;
  assert(file.loadInstance(0u, instance, &error));
  assert(instance.header.nodeCount == 1u);
  assert(instance.header.brickCount == 1u);
  assert(instance.header.traceGridResolution == sdf::VSDF_TRACE_GRID_RESOLUTION);
  assert(instance.header.traceGridByteCount == 0u);
  assert(instance.header.traceGridOffset >= instance.header.distancesOffset + instance.header.distanceByteCount);
  assert(instance.header.tracePageTableEntryCount == sdf::VSDF_TRACE_ROOT_CELL_COUNT);
  assert(instance.header.tracePageTableOffset >= instance.header.traceGridOffset + instance.header.traceGridByteCount);
  assert(instance.header.tracePageCount == 0u);
  assert(instance.header.tracePageMaskWordCount == instance.header.tracePageCount * sdf::VSDF_TRACE_PAGE_MASK_WORD_COUNT);
  assert(instance.header.traceBrickMapWordCount == 0u);
  assert(instance.header.traceBrickMapOffset >= instance.header.tracePageMasksOffset + instance.header.tracePageMaskWordCount * sizeof(uint32_t));
  assert(instance.header.traceAabbTreeWordCount == sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT);
  assert(instance.header.traceAabbTreeOffset >= instance.header.traceBrickMapOffset + instance.header.traceBrickMapWordCount * sizeof(uint32_t));
  assert(instance.nodes.size() == 1u);
  assert(instance.bricks.size() == instance.header.brickCount);
  assert(instance.distances.size() == instance.bricks.size() * sdf::VSDF_BRICK_DISTANCE_STRIDE);
  assert(instance.traceGrid.empty());
  assert(instance.tracePageTable.size() == sdf::VSDF_TRACE_ROOT_CELL_COUNT);
  assert(instance.tracePages.size() == instance.header.tracePageCount);
  assert(instance.tracePageMasks.size() == instance.tracePages.size() * sdf::VSDF_TRACE_PAGE_MASK_WORD_COUNT);
  assert(instance.traceBrickMap.size() == instance.header.traceBrickMapWordCount);
  assert(instance.traceAabbTree.size() == sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT);
  assert(countValidTraceBricks(instance.traceBrickMap) == 0u);
  assert(countNonZeroWords(instance.traceAabbTree) == 0u);
  assert(nearEqual(instance.header.rootVoxelSize, 0.125f));

  fs::remove(path);
}

void testVSDFMultipleInstanceRandomAccess()
{
  const fs::path path = tempPath("VSDFMultipleInstanceRandomAccess.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  const virtualgeometry::VirtualGeometrySourceAsset asset = makeTwoTriangleAsset();

  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), asset, settings, &error));

  sdf::SDFSceneFile file;
  assert(file.load(path.string(), &error));
  assert(file.header().instanceCount == 2u);
  assert(file.instances()[0].instanceId == 0u);
  assert(file.instances()[1].instanceId == 1u);
  assert(file.instances()[1].sourceMeshIndex == 1u);
  assert(file.instances()[1].materialRangeBegin == 1u);

  sdf::VSDFDecodedInstance instance;
  assert(file.loadInstance(1u, instance, &error));
  assert(instance.header.nodeCount == 1u);
  assert(nearEqual(instance.header.localBoundsMin[0], 2.0f));
  assert(nearEqual(instance.header.localBoundsMax[0], 3.0f));

  fs::remove(path);
}

void testVSDFRejectsBadMagic()
{
  const fs::path path = tempPath("VSDFRejectsBadMagic.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), makeTriangleAsset(), settings, &error));

  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  const uint32_t badMagic = 0u;
  file.write(reinterpret_cast<const char *>(&badMagic), sizeof(badMagic));
  file.close();

  sdf::SDFSceneFile sceneFile;
  assert(!sceneFile.load(path.string(), &error));
  fs::remove(path);
}

void testSDFErrorRefinesMore()
{
  virtualgeometry::SparseSDFBuildSettings coarse;
  coarse.maxLevel = 0u;
  coarse.requestedError = 1.0f;

  virtualgeometry::SparseSDFBuildSettings fine;
  fine.maxLevel = 1u;
  fine.requestedError = 0.005f;

  const virtualgeometry::VirtualGeometrySourceAsset asset = makeCubeAsset();
  const auto coarseInstances = virtualgeometry::SparseSDFBuilder::buildInstances(asset, coarse);
  const auto fineInstances = virtualgeometry::SparseSDFBuilder::buildInstances(asset, fine);
  assert(coarseInstances.size() == 1u);
  assert(fineInstances.size() == 1u);
  assert(coarseInstances[0].nodes.size() == 1u);
  assert(fineInstances[0].nodes.size() > coarseInstances[0].nodes.size());
  assert(fineInstances[0].bricks.size() > coarseInstances[0].bricks.size());
}

uint32_t countBits(const uint32_t words[16])
{
  uint32_t count = 0u;
  for (uint32_t wordIndex = 0u; wordIndex < 16u; ++wordIndex)
    count += static_cast<uint32_t>(__builtin_popcount(words[wordIndex]));
  return count;
}

struct TestBounds
{
  float min[3] = {};
  float max[3] = {};
};

struct NodeBoundsRecord
{
  uint32_t nodeIndex = 0u;
  uint32_t brickIndex = 0u;
  uint32_t level = 0u;
  TestBounds bounds;
};

uint32_t testPopcountBefore(const uint32_t words[16], uint32_t bit)
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

TestBounds testCellBounds(const TestBounds &bounds, uint32_t cell)
{
  const uint32_t x = cell % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t y = (cell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t z = cell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
  const uint32_t coords[3] = {x, y, z};
  TestBounds result;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float extent = (bounds.max[axis] - bounds.min[axis]) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
    result.min[axis] = bounds.min[axis] + extent * static_cast<float>(coords[axis]);
    result.max[axis] = result.min[axis] + extent;
  }
  return result;
}

void collectNodeBounds(
    const sdf::VSDFInstanceWriteData &instance,
    uint32_t nodeIndex,
    const TestBounds &bounds,
    std::vector<NodeBoundsRecord> &records)
{
  if (nodeIndex >= instance.nodes.size())
    return;
  const sdf::VSDFNodeDisk &node = instance.nodes[nodeIndex];
  records.push_back(NodeBoundsRecord{nodeIndex, node.brickIndex, node.level, bounds});
  if (node.firstChild == UINT32_MAX)
    return;
  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    const uint32_t bit = 1u << (cell % 32u);
    if ((node.childMaskWords[cell / 32u] & bit) == 0u)
      continue;
    collectNodeBounds(instance, node.firstChild + testPopcountBefore(node.childMaskWords, cell), testCellBounds(bounds, cell), records);
  }
}

float decodeTestDistance(const sdf::VSDFInstanceWriteData &instance, uint32_t brickIndex, uint32_t flatCell)
{
  const sdf::VSDFBrickDisk &brick = instance.bricks[brickIndex];
  const uint32_t offset = brick.distanceOffset + flatCell;
  assert(offset < instance.distances.size());
  const int32_t signedValue = static_cast<int32_t>(static_cast<int8_t>(instance.distances[offset]));
  return std::max(-1.0f, static_cast<float>(signedValue) / 127.0f) *
         static_cast<float>(std::max(instance.header.narrowBandVoxels, 1u)) *
         brick.voxelSize;
}

float decodeTestDistanceSample(const sdf::VSDFInstanceWriteData &instance, uint32_t brickIndex, uint32_t x, uint32_t y, uint32_t z)
{
  const uint32_t sample = x + y * sdf::VSDF_BRICK_DISTANCE_RESOLUTION + z * sdf::VSDF_BRICK_DISTANCE_RESOLUTION * sdf::VSDF_BRICK_DISTANCE_RESOLUTION;
  return decodeTestDistance(instance, brickIndex, sample);
}

void testFaceDistanceSample(uint32_t axis, uint32_t side, uint32_t u, uint32_t v, uint32_t (&coord)[3])
{
  coord[axis] = side == 0u ? 0u : sdf::VSDF_BRICK_DISTANCE_RESOLUTION - 1u;
  coord[(axis + 1u) % 3u] = u;
  coord[(axis + 2u) % 3u] = v;
}

bool sameInterval(float aMin, float aMax, float bMin, float bMax, float epsilon)
{
  return std::fabs(aMin - bMin) <= epsilon && std::fabs(aMax - bMax) <= epsilon;
}

void testSDFAdjacentBrickFacesAreStitched()
{
  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 1u;
  settings.requestedError = 0.001f;
  settings.maxRefinedCellsPerBrick = sdf::VSDF_BRICK_CELL_COUNT;

  const auto instances = virtualgeometry::SparseSDFBuilder::buildInstances(makeCubeAsset(), settings);
  assert(instances.size() == 1u);
  assert(instances[0].nodes.size() > 1u);

  TestBounds rootBounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    rootBounds.min[axis] = instances[0].header.localBoundsMin[axis];
    rootBounds.max[axis] = instances[0].header.localBoundsMax[axis];
  }
  std::vector<NodeBoundsRecord> records;
  collectNodeBounds(instances[0], instances[0].header.rootNode, rootBounds, records);

  bool foundAdjacentFace = false;
  uint32_t comparedNearSurfaceSamples = 0u;
  float maxFaceDelta = 0.0f;
  float maxVoxelSize = 0.0f;
  for (size_t a = 0u; a < records.size(); ++a)
  {
    for (size_t b = a + 1u; b < records.size(); ++b)
    {
      if (records[a].level != records[b].level || records[a].level == 0u)
        continue;
      const float epsilon = std::max(instances[0].bricks[records[a].brickIndex].voxelSize, instances[0].bricks[records[b].brickIndex].voxelSize) * 1.0e-4f;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const bool aToB = std::fabs(records[a].bounds.max[axis] - records[b].bounds.min[axis]) <= epsilon;
        const bool bToA = std::fabs(records[b].bounds.max[axis] - records[a].bounds.min[axis]) <= epsilon;
        if (!aToB && !bToA)
          continue;
        bool sameProjection = true;
        for (uint32_t otherAxis = 0u; otherAxis < 3u; ++otherAxis)
        {
          if (otherAxis == axis)
            continue;
          sameProjection = sameProjection && sameInterval(records[a].bounds.min[otherAxis], records[a].bounds.max[otherAxis], records[b].bounds.min[otherAxis], records[b].bounds.max[otherAxis], epsilon);
        }
        if (!sameProjection)
          continue;

        foundAdjacentFace = true;
        const uint32_t sideA = aToB ? 1u : 0u;
        const uint32_t sideB = aToB ? 0u : 1u;
        maxVoxelSize = std::max(maxVoxelSize, std::max(instances[0].bricks[records[a].brickIndex].voxelSize, instances[0].bricks[records[b].brickIndex].voxelSize));
        for (uint32_t v = 1u; v + 1u < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++v)
        {
          for (uint32_t u = 1u; u + 1u < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++u)
          {
            uint32_t coordA[3] = {};
            uint32_t coordB[3] = {};
            testFaceDistanceSample(axis, sideA, u, v, coordA);
            testFaceDistanceSample(axis, sideB, u, v, coordB);
            const float da = decodeTestDistanceSample(instances[0], records[a].brickIndex, coordA[0], coordA[1], coordA[2]);
            const float db = decodeTestDistanceSample(instances[0], records[b].brickIndex, coordB[0], coordB[1], coordB[2]);
            if (std::min(std::fabs(da), std::fabs(db)) > maxVoxelSize * 1.5f)
              continue;
            ++comparedNearSurfaceSamples;
            maxFaceDelta = std::max(maxFaceDelta, std::fabs(da - db));
          }
        }
      }
    }
  }

  assert(foundAdjacentFace);
  if (comparedNearSurfaceSamples > 0u)
    assert(maxFaceDelta <= std::max(maxVoxelSize * 1.5f, 1.0e-5f));
}

void testSDFSurfaceValidMaskExcludesInteriorCells()
{
  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  settings.requestedError = 1.0f;

  const auto instances = virtualgeometry::SparseSDFBuilder::buildInstances(makeCubeAsset(), settings);
  assert(instances.size() == 1u);
  assert(instances[0].nodes.size() == 1u);

  const sdf::VSDFNodeDisk &root = instances[0].nodes[0];
  const uint32_t validCellCount = countBits(root.validMaskWords);
  const uint32_t traceCellCount = countBits(root.traceMaskWords);
  assert(validCellCount > 0u);
  assert(traceCellCount > 0u);
  assert(validCellCount < sdf::VSDF_BRICK_CELL_COUNT);
  assert(traceCellCount >= validCellCount);
  for (uint32_t wordIndex = 0u; wordIndex < 16u; ++wordIndex)
    assert((root.validMaskWords[wordIndex] & ~root.traceMaskWords[wordIndex]) == 0u);

  const uint32_t interiorCell = 4u + 4u * sdf::VSDF_BRICK_RESOLUTION + 4u * sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION;
  assert((root.validMaskWords[interiorCell / 32u] & (1u << (interiorCell % 32u))) == 0u);
}

void testSDFVoxelizationIsConservativeNearCellFaces()
{
  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  settings.requestedError = 1.0f;
  settings.surfaceCloseVoxels = 0u;

  const auto instances = virtualgeometry::SparseSDFBuilder::buildInstances(makeNearVoxelFaceTriangleAsset(), settings);
  assert(instances.size() == 1u);
  assert(instances[0].nodes.size() == 1u);

  const sdf::VSDFNodeDisk &root = instances[0].nodes[0];
  const uint32_t lowerCell = 2u + 2u * sdf::VSDF_BRICK_RESOLUTION + 3u * sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t upperCell = 2u + 2u * sdf::VSDF_BRICK_RESOLUTION + 4u * sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t lowerBit = 1u << (lowerCell % 32u);
  const uint32_t upperBit = 1u << (upperCell % 32u);

  assert((root.validMaskWords[lowerCell / 32u] & lowerBit) != 0u);
  assert((root.validMaskWords[upperCell / 32u] & upperBit) != 0u);
  assert((root.traceMaskWords[lowerCell / 32u] & lowerBit) != 0u);
  assert((root.traceMaskWords[upperCell / 32u] & upperBit) != 0u);
}

void testSDFTraceGridIsSerialized()
{
  const fs::path path = tempPath("SDFTraceGridIsSerialized.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 1u;
  settings.requestedError = 0.01f;

  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), makeCubeAsset(), settings, &error));

  sdf::SDFSceneFile file;
  assert(file.load(path.string(), &error));

  sdf::VSDFDecodedInstance instance;
  assert(file.loadInstance(0u, instance, &error));

  assert(instance.header.traceGridResolution == sdf::VSDF_TRACE_GRID_RESOLUTION);
  assert(instance.header.traceGridByteCount == 0u);
  assert(instance.header.tracePageTableEntryCount == sdf::VSDF_TRACE_ROOT_CELL_COUNT);
  assert(instance.header.tracePageTableOffset >= instance.header.traceGridOffset + instance.header.traceGridByteCount);
  assert(instance.header.tracePageCount == 0u);
  assert(instance.header.tracePageMaskWordCount == instance.header.tracePageCount * sdf::VSDF_TRACE_PAGE_MASK_WORD_COUNT);
  assert(instance.header.traceBrickMapWordCount == 0u);
  assert(instance.header.traceBrickMapOffset >= instance.header.tracePageMasksOffset + instance.header.tracePageMaskWordCount * sizeof(uint32_t));
  assert(instance.header.traceAabbTreeWordCount == sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT);
  assert(instance.header.traceAabbTreeOffset >= instance.header.traceBrickMapOffset + instance.header.traceBrickMapWordCount * sizeof(uint32_t));
  assert(instance.traceGrid.empty());
  assert(instance.tracePageTable.size() == sdf::VSDF_TRACE_ROOT_CELL_COUNT);
  assert(instance.tracePages.size() == instance.header.tracePageCount);
  assert(instance.tracePageMasks.size() == instance.tracePages.size() * sdf::VSDF_TRACE_PAGE_MASK_WORD_COUNT);
  assert(instance.traceBrickMap.size() == instance.header.traceBrickMapWordCount);
  assert(instance.traceAabbTree.size() == sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT);

  const uint32_t occupied = countValidTraceBricks(instance.traceBrickMap);
  assert(occupied == 0u);
  assert(countNonZeroWords(instance.traceAabbTree) == 0u);

  fs::remove(path);
}

void testSDFSceneManagerLoadsAllInstances()
{
  const fs::path path = tempPath("SDFSceneManagerLoadsAllInstances.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), makeTwoTriangleAsset(), settings, &error));

  sdf::SDFSceneManager manager;
  assert(manager.loadVSDF(path.string(), &error));
  assert(manager.instanceCount() == 2u);
  assert(manager.blasCount() == 2u);
  assert(manager.tlasNodeCount() >= 1u);
  assert(manager.gpuInstances().size() == 2u);
  assert(manager.gpuBlases().size() == 2u);
  assert(manager.gpuInstances()[0].idsAndBlas[0] == 0u);
  assert(manager.gpuInstances()[1].idsAndBlas[0] == 1u);

  fs::remove(path);
}

void testSDFTlasCandidateQuery()
{
  const fs::path path = tempPath("SDFTlasCandidateQuery.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), makeTwoTriangleAsset(), settings, &error));

  sdf::SDFSceneManager manager;
  assert(manager.loadVSDF(path.string(), &error));

  sdf::SDFRay ray;
  ray.origin = math::Vec3f(2.5f, 0.5f, 4.0f);
  ray.direction = math::Vec3f(0.0f, 0.0f, -1.0f);
  ray.tMin = 0.0f;
  ray.tMax = 16.0f;
  const std::vector<uint32_t> candidates = manager.queryCandidates(ray);
  assert(!candidates.empty());
  bool foundSecond = false;
  for (uint32_t candidate : candidates)
    foundSecond = foundSecond || candidate == 1u;
  assert(foundSecond);

  fs::remove(path);
}

void testSDFSceneManagerSyncsVirtualGeometryInstanceTransforms()
{
  const fs::path path = tempPath("SDFSceneManagerSyncsVirtualGeometryInstanceTransforms.vsdf");
  fs::remove(path);

  virtualgeometry::SparseSDFBuildSettings settings;
  settings.maxLevel = 0u;
  std::string error;
  assert(virtualgeometry::SparseSDFBuilder::writeVSDF(path.string(), makeTriangleAsset(), settings, &error));

  sdf::SDFSceneManager manager;
  assert(manager.loadVSDF(path.string(), &error));
  assert(manager.instanceCount() == 1u);
  assert(manager.gpuInstances().size() == 1u);
  assert(nearEqual(manager.gpuInstances()[0].localToWorld[0], 1.0f));

  virtualgeometry::VirtualGeometryScene::InstanceManager instanceManager;
  virtualgeometry::QuantizationConfig quantizationConfig;
  const virtualgeometry::SceneInstanceId sceneInstanceId = instanceManager.allocateInstance(
      "triangle",
      math::Vec3f(0.0f, 0.0f, 0.0f),
      math::Quatf::identity(),
      1.0f,
      0u,
      0u,
      0u,
      0u,
      0u,
      quantizationConfig);

  const math::Quatf rotation = math::Quatf::fromAxisAngle(math::Vec3f(0.0f, 0.0f, 1.0f), 1.57079632679f);
  assert(instanceManager.updateInstance(sceneInstanceId, math::Vec3f(0.0f, 0.0f, 0.0f), rotation, math::Vec3f(1.0f, 1.0f, 1.0f)));
  assert(manager.syncInstanceTransforms(instanceManager, std::vector<virtualgeometry::SceneInstanceId>{sceneInstanceId}));

  const sdf::SDFInstanceGPU &gpuInstance = manager.gpuInstances()[0];
  assert(nearEqual(gpuInstance.localToWorld[0], 0.0f, 1.0e-4f));
  assert(nearEqual(gpuInstance.localToWorld[1], 1.0f, 1.0e-4f));
  assert(nearEqual(gpuInstance.localToWorld[4], -1.0f, 1.0e-4f));
  assert(nearEqual(gpuInstance.worldToLocal[1], -1.0f, 1.0e-4f));
  assert(manager.tlasNodeCount() >= 1u);

  fs::remove(path);
}

} // namespace

int main()
{
  testVSDFLZ4RoundTrip();
  testVSDFMultipleInstanceRandomAccess();
  testVSDFRejectsBadMagic();
  testSDFErrorRefinesMore();
  testSDFAdjacentBrickFacesAreStitched();
  testSDFSurfaceValidMaskExcludesInteriorCells();
  testSDFVoxelizationIsConservativeNearCellFaces();
  testSDFTraceGridIsSerialized();
  testSDFSceneManagerLoadsAllInstances();
  testSDFTlasCandidateQuery();
  testSDFSceneManagerSyncsVirtualGeometryInstanceTransforms();
  return 0;
}
