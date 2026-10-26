#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ColumnChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2FineRasterChain.hpp"
#include "virtualgeometry/VirtualGeometryGeneratedPayloadManager.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Terrain GPU v2 - generated virtual-geometry page emit.
//
// One lane of fine raster bricks (the GPU terrain generator's output for one
// page at one level) becomes decoded virtual-geometry clusters in two GPU
// transactions: a count pass sizes the page per tile so the scene reserves
// exactly what it needs, then the emit pass writes the faces into that
// reservation, tile by tile, so each cluster is spatially coherent and belongs
// to one quadrant of the page. A third, independent kernel rewrites the
// streaming-leaf flags of an installed page when its child pages come and go.
// The WGSL side is terrain_gpu_v2_generated_vg_emit.wgsl.
namespace voxel::svraster::rendering
{

enum class TerrainGPUV2GeneratedVGEmitStatus : uint32_t
{
  Empty = 0u,
  Ready = 1u,
  Overflow = 2u,
  Invalid = 3u,
  StaleSource = 4u,
  // A count transaction completed: tileFaces and emittedFaces hold the page's
  // exposed face counts and nothing was written to the scene.
  Counted = 5u,
};

inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitFacesPerCluster = 48u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitTrianglesPerFace = 2u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitVerticesPerFace = 4u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitIndicesPerFace = 6u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitTilesPerAxis = 8u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitTileCount =
    kTerrainGPUV2GeneratedVGEmitTilesPerAxis * kTerrainGPUV2GeneratedVGEmitTilesPerAxis;
// Per request: the page's face total plus one counter per tile.
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitCounterWords =
    1u + kTerrainGPUV2GeneratedVGEmitTileCount;

inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitOmitNegativeYFace = 1u << 0u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitCountMode = 1u << 1u;
// The page cannot be refined (finest level): every cluster is a streaming
// leaf, whatever the child mask says.
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitTrueLeaf = 1u << 2u;

inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitErrorInvalidDescriptor = 1u << 0u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitErrorStaleSource = 1u << 1u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitErrorFaceOverflow = 1u << 2u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitErrorClusterOverflow = 1u << 3u;
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitErrorTileOverflow = 1u << 4u;

struct alignas(16) TerrainGPUV2GeneratedVGEmitRequestGPU
{
  uint32_t ticketIdLow = 0u;
  uint32_t ticketIdHigh = 0u;
  uint32_t localPageIndex = UINT32_MAX;
  uint32_t globalPageTableIndex = UINT32_MAX;
  uint32_t sourceLane = 0u;
  uint32_t sourceNodeOffset = 0u;
  uint32_t sourceNodeCount = 0u;
  uint32_t sourceNodeCapacity = 0u;
  uint32_t clusterOffset = 0u;
  uint32_t clusterCount = 0u;
  uint32_t decodedVertexOffset = 0u;
  uint32_t decodedVertexCount = 0u;
  uint32_t decodedIndexOffset = 0u;
  uint32_t decodedIndexCount = 0u;
  uint32_t maxFacesPerCluster = kTerrainGPUV2GeneratedVGEmitFacesPerCluster;
  uint32_t expectedTerrainGeneration = 0u;
  std::array<float, 4u> boundsMin{};
  std::array<float, 4u> boundsMax{};
  float selfError = 0.0f;
  float parentError = 0.0f;
  uint32_t rootPage = 0u;
  uint32_t flags = 0u;
  std::array<float, 4u> parentBoundsMin{};
  std::array<float, 4u> parentBoundsMax{};
  // Bit q set when the child page covering quadrant q (bit 0: +x half, bit 1:
  // +z half) is resident; the emitted clusters of the other quadrants are
  // streaming leaves.
  uint32_t residentChildMask = 0u;
  uint32_t pageBricksPerAxis = kTerrainGPUV2GeneratedVGEmitTilesPerAxis;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  // Emit mode: first local cluster of each tile (prefix sum of the tile
  // cluster counts from the count transaction) and the faces of every tile
  // before it, which pack the decoded ranges tight.
  std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> tileClusterBase{};
  std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> tileFaceBase{};
};

struct alignas(16) TerrainGPUV2GeneratedVGEmitResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2GeneratedVGEmitStatus::Empty);
  uint32_t validationErrors = 0u;
  uint32_t ticketIdLow = 0u;
  uint32_t ticketIdHigh = 0u;
  uint32_t localPageIndex = UINT32_MAX;
  uint32_t globalPageTableIndex = UINT32_MAX;
  uint32_t emittedFaces = 0u;
  uint32_t emittedTriangles = 0u;
  uint32_t emittedVertices = 0u;
  uint32_t emittedIndices = 0u;
  uint32_t emittedClusters = 0u;
  uint32_t overflowFaces = 0u;
  uint32_t sourceNodes = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> tileFaces{};
};

struct alignas(16) TerrainGPUV2GeneratedVGFlagRequestGPU
{
  uint32_t clusterOffset = 0u;
  uint32_t clusterCount = 0u;
  uint32_t residentChildMask = 0u;
  uint32_t flags = 0u;
  // Reparenting a resident root changes its cut metric as well as its flags.
  std::array<float, 4u> boundsMin{};
  std::array<float, 4u> boundsMax{};
  std::array<float, 4u> parentBoundsMin{};
  std::array<float, 4u> parentBoundsMax{};
  float selfError = 0.0f;
  float parentError = 0.0f;
  uint32_t rootPage = 0u;
  uint32_t updateCullMetadata = 0u;
};

struct alignas(16) TerrainGPUV2GeneratedVGEmitUniformsGPU
{
  uint32_t requestCount = 0u;
  uint32_t counterStride = 0u;
  uint32_t sourceNodeLaneStride = 0u;
  uint32_t flagRequestCount = 0u;
  // Threads the two face passes are dispatched with; each strides over its
  // share of the page's candidates. A page's real candidate count is a GPU
  // result, so a dispatch sized from the lane's brick capacity launched 25
  // million threads for a page holding a hundred bricks.
  uint32_t faceThreadCount = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
};

static_assert(sizeof(TerrainGPUV2GeneratedVGEmitRequestGPU) == 672u);
static_assert(sizeof(TerrainGPUV2GeneratedVGEmitResultGPU) == 320u);
static_assert(sizeof(TerrainGPUV2GeneratedVGFlagRequestGPU) == 96u);
static_assert(sizeof(TerrainGPUV2GeneratedVGEmitUniformsGPU) == 32u);
// Fixed grid for the face passes: 1M threads covers a page of ~340 bricks in
// one stride and any page in a bounded number.
inline constexpr uint32_t kTerrainGPUV2GeneratedVGEmitFaceWorkgroups = 8192u;

inline uint64_t terrainGPUV2GeneratedVGEmitTicket(
    const TerrainGPUV2GeneratedVGEmitResultGPU &result)
{
  return uint64_t{result.ticketIdLow} |
      (uint64_t{result.ticketIdHigh} << 32u);
}

// What the CPU knows about a page before either transaction: its bounds and
// its parent's, the errors of its level and its parent's level, which of its
// four children are resident, and whether it can refine at all.
struct TerrainGPUV2GeneratedVGEmitPageInfo
{
  std::array<float, 3u> boundsMin{};
  std::array<float, 3u> boundsMax{};
  std::array<float, 3u> parentBoundsMin{};
  std::array<float, 3u> parentBoundsMax{};
  float selfError = 0.0f;
  float parentError = 0.0f;
  bool rootPage = false;
  bool trueLeaf = false;
  bool omitNegativeYFace = false;
  uint32_t residentChildMask = 0u;
  uint32_t pageBricksPerAxis = kTerrainGPUV2GeneratedVGEmitTilesPerAxis;
};

// The cluster layout of a page from its counted tile faces: clusters are
// allocated tile by tile, each tile owning ceil(faces / facesPerCluster).
struct TerrainGPUV2GeneratedVGClusterPlan
{
  std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> tileClusterBase{};
  // Faces of every tile before this one: the decoded ranges are packed by
  // face, not by whole clusters, so a tile's partial last cluster costs only
  // the faces it holds.
  std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> tileFaceBase{};
  uint32_t clusterCount = 0u;
  uint32_t faceCount = 0u;
};

inline TerrainGPUV2GeneratedVGClusterPlan terrainGPUV2GeneratedVGClusterPlan(
    const std::array<uint32_t, kTerrainGPUV2GeneratedVGEmitTileCount> &tileFaces,
    const uint32_t facesPerCluster = kTerrainGPUV2GeneratedVGEmitFacesPerCluster)
{
  TerrainGPUV2GeneratedVGClusterPlan plan{};
  const uint32_t perCluster = std::max(facesPerCluster, 1u);
  for (uint32_t tile = 0u; tile < kTerrainGPUV2GeneratedVGEmitTileCount; ++tile)
  {
    plan.tileClusterBase[tile] = plan.clusterCount;
    plan.tileFaceBase[tile] = plan.faceCount;
    plan.clusterCount += (tileFaces[tile] + perCluster - 1u) / perCluster;
    plan.faceCount += tileFaces[tile];
  }
  return plan;
}

// Quadrant bit of a child page inside its parent: bit 0 for the +x half, bit 1
// for the +z half, matching quadrantOfTile in the shader.
inline uint32_t terrainGPUV2GeneratedVGChildQuadrant(
    const std::array<float, 3u> &parentBoundsMin,
    const std::array<float, 3u> &parentBoundsMax,
    const std::array<float, 3u> &childBoundsMin,
    const std::array<float, 3u> &childBoundsMax)
{
  const float parentCenterX = (parentBoundsMin[0] + parentBoundsMax[0]) * 0.5f;
  const float parentCenterZ = (parentBoundsMin[2] + parentBoundsMax[2]) * 0.5f;
  const float childCenterX = (childBoundsMin[0] + childBoundsMax[0]) * 0.5f;
  const float childCenterZ = (childBoundsMin[2] + childBoundsMax[2]) * 0.5f;
  return (childCenterX >= parentCenterX ? 1u : 0u) |
      (childCenterZ >= parentCenterZ ? 2u : 0u);
}

namespace detail
{
inline void applyTerrainGPUV2GeneratedVGPageInfo(
    const TerrainGPUV2GeneratedVGEmitPageInfo &page,
    TerrainGPUV2GeneratedVGEmitRequestGPU &request)
{
  request.boundsMin = {page.boundsMin[0], page.boundsMin[1], page.boundsMin[2], 0.0f};
  request.boundsMax = {page.boundsMax[0], page.boundsMax[1], page.boundsMax[2], 0.0f};
  request.parentBoundsMin = {page.parentBoundsMin[0], page.parentBoundsMin[1], page.parentBoundsMin[2], 0.0f};
  request.parentBoundsMax = {page.parentBoundsMax[0], page.parentBoundsMax[1], page.parentBoundsMax[2], 0.0f};
  request.selfError = std::max(page.selfError, 0.0f);
  request.parentError = std::max(page.parentError, request.selfError);
  request.rootPage = page.rootPage ? 1u : 0u;
  request.flags = (page.omitNegativeYFace ? kTerrainGPUV2GeneratedVGEmitOmitNegativeYFace : 0u) |
      (page.trueLeaf ? kTerrainGPUV2GeneratedVGEmitTrueLeaf : 0u);
  request.residentChildMask = page.residentChildMask & 0xfu;
  request.pageBricksPerAxis = std::max(page.pageBricksPerAxis, 1u);
  request.maxFacesPerCluster = kTerrainGPUV2GeneratedVGEmitFacesPerCluster;
}
} // namespace detail

// A count transaction needs no reservation: only the source lane and the
// page's geometry.
inline bool buildTerrainGPUV2GeneratedVGCountRequest(
    const TerrainGPUV2GeneratedVGEmitPageInfo &page,
    const uint32_t sourceLane,
    const uint32_t sourceNodeCapacity,
    const uint32_t sourceNodeCount,
    const uint32_t expectedTerrainGeneration,
    TerrainGPUV2GeneratedVGEmitRequestGPU &request,
    std::string *error = nullptr)
{
  request = {};
  if (error != nullptr)
    error->clear();
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (sourceNodeCapacity == 0u || sourceNodeCount == 0u ||
      sourceNodeCount > sourceNodeCapacity)
    return fail("terrain generated VG count request source node range is invalid");
  if (expectedTerrainGeneration == 0u)
    return fail("terrain generated VG count request needs a terrain generation");
  request.sourceLane = sourceLane;
  request.sourceNodeOffset = 0u;
  request.sourceNodeCount = sourceNodeCount;
  request.sourceNodeCapacity = sourceNodeCapacity;
  request.expectedTerrainGeneration = expectedTerrainGeneration;
  detail::applyTerrainGPUV2GeneratedVGPageInfo(page, request);
  request.flags |= kTerrainGPUV2GeneratedVGEmitCountMode;
  return true;
}

// An emit transaction writes into the reservation the count sized; the plan
// (from the counted tile faces) must fit the descriptor exactly or better.
inline bool buildTerrainGPUV2GeneratedVGEmitRequest(
    const virtualgeometry::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor &descriptor,
    const TerrainGPUV2GeneratedVGEmitPageInfo &page,
    const uint32_t sourceLane,
    const uint32_t sourceNodeCapacity,
    const uint32_t sourceNodeCount,
    const uint32_t expectedTerrainGeneration,
    const TerrainGPUV2GeneratedVGClusterPlan &plan,
    TerrainGPUV2GeneratedVGEmitRequestGPU &request,
    std::string *error = nullptr)
{
  request = {};
  if (error != nullptr)
    error->clear();
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (descriptor.ticketId ==
      virtualgeometry::VirtualGeometryGeneratedPayloadManager::INVALID_TICKET_ID)
    return fail("terrain generated VG emit request needs a payload-manager ticket");
  if (descriptor.localPageIndex == UINT32_MAX ||
      descriptor.globalPageTableIndex == UINT32_MAX)
    return fail("terrain generated VG emit request needs a valid page-table address");
  if (descriptor.clusterCount == 0u ||
      descriptor.clusterCount > virtualgeometry::VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE)
    return fail("terrain generated VG emit request cluster reservation is invalid");
  if (descriptor.decodedVertexCount < kTerrainGPUV2GeneratedVGEmitVerticesPerFace ||
      descriptor.decodedIndexCount < kTerrainGPUV2GeneratedVGEmitIndicesPerFace)
    return fail("terrain generated VG emit request decoded reservation is too small");
  if (sourceNodeCapacity == 0u || sourceNodeCount == 0u ||
      sourceNodeCount > sourceNodeCapacity)
    return fail("terrain generated VG emit request source node range is invalid");
  if (expectedTerrainGeneration == 0u)
    return fail("terrain generated VG emit request needs a terrain generation");
  if (plan.clusterCount == 0u || plan.faceCount == 0u)
    return fail("terrain generated VG emit request has no counted faces");
  if (plan.clusterCount > descriptor.clusterCount)
    return fail("terrain generated VG emit request cluster plan exceeds the reservation");
  const uint64_t verticesNeeded =
      uint64_t{plan.faceCount} * kTerrainGPUV2GeneratedVGEmitVerticesPerFace;
  const uint64_t indicesNeeded =
      uint64_t{plan.faceCount} * kTerrainGPUV2GeneratedVGEmitIndicesPerFace;
  if (verticesNeeded > descriptor.decodedVertexCount ||
      indicesNeeded > descriptor.decodedIndexCount)
    return fail("terrain generated VG emit request cluster plan exceeds the decoded reservation");
  request.ticketIdLow = static_cast<uint32_t>(descriptor.ticketId);
  request.ticketIdHigh = static_cast<uint32_t>(descriptor.ticketId >> 32u);
  request.localPageIndex = descriptor.localPageIndex;
  request.globalPageTableIndex = descriptor.globalPageTableIndex;
  request.sourceLane = sourceLane;
  request.sourceNodeOffset = 0u;
  request.sourceNodeCount = sourceNodeCount;
  request.sourceNodeCapacity = sourceNodeCapacity;
  request.clusterOffset = descriptor.clusterOffset;
  request.clusterCount = plan.clusterCount;
  request.decodedVertexOffset = descriptor.decodedVertexOffset;
  request.decodedVertexCount = descriptor.decodedVertexCount;
  request.decodedIndexOffset = descriptor.decodedIndexOffset;
  request.decodedIndexCount = descriptor.decodedIndexCount;
  request.expectedTerrainGeneration = expectedTerrainGeneration;
  detail::applyTerrainGPUV2GeneratedVGPageInfo(page, request);
  request.tileClusterBase = plan.tileClusterBase;
  request.tileFaceBase = plan.tileFaceBase;
  return true;
}

// The decoded reservation an emit needs for a counted page: clusters,
// vertices and indices, all at cluster granularity.
inline void terrainGPUV2GeneratedVGReservationForPlan(
    const TerrainGPUV2GeneratedVGClusterPlan &plan,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::Request &request)
{
  request.clusterCount = plan.clusterCount;
  request.decodedVertexCount =
      plan.faceCount * kTerrainGPUV2GeneratedVGEmitVerticesPerFace;
  request.decodedIndexCount =
      plan.faceCount * kTerrainGPUV2GeneratedVGEmitIndicesPerFace;
  request.decodedBoneWeightCount = 0u;
  request.encodedPageBytes = 0u;
}

// Uses the completed tile counters, never a readback of vertices/indices.
inline bool terrainGPUV2GeneratedVGBlasManifest(
    const TerrainGPUV2GeneratedVGEmitResultGPU &result,
    std::vector<virtualgeometry::VirtualGeometryScene::GeneratedBlasCluster> &clusters)
{
  clusters.clear();
  uint64_t faces = 0;
  for (uint32_t tileFaces : result.tileFaces)
  {
    if (faces + tileFaces > result.emittedFaces) return false;
    for (uint32_t offset = 0; offset < tileFaces;)
    {
      if (clusters.size() >= virtualgeometry::VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE) return false;
      const auto count = std::min(kTerrainGPUV2GeneratedVGEmitFacesPerCluster, tileFaces-offset);
      if ((faces+offset+count)*6 > UINT32_MAX) return false;
      clusters.push_back({uint32_t((faces+offset)*4), uint32_t((faces+offset)*6), count*4, count*2});
      offset += count;
    }
    faces += tileFaces;
  }
  return faces > 0 && faces == result.emittedFaces && faces*4 == result.emittedVertices &&
      faces*6 == result.emittedIndices && faces*2 == result.emittedTriangles && clusters.size() == result.emittedClusters;
}

inline bool publishTerrainGPUV2GeneratedVGEmitResult(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    const TerrainGPUV2GeneratedVGEmitResultGPU &result,
    std::string *error = nullptr, bool prepareOnly = false)
{
  if (error != nullptr)
    error->clear();
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (result.status != static_cast<uint32_t>(TerrainGPUV2GeneratedVGEmitStatus::Ready))
    return fail("terrain generated VG emit result is not ready");
  if (result.validationErrors != 0u)
    return fail("terrain generated VG emit result has validation errors");
  if (result.emittedFaces == 0u ||
      result.emittedTriangles == 0u ||
      result.emittedVertices == 0u ||
      result.emittedIndices == 0u ||
      result.emittedClusters == 0u)
    return fail("terrain generated VG emit result has no drawable payload");
  virtualgeometry::VirtualGeometryGeneratedPayloadManager::PublishInfo publish{};
  publish.ticketId = terrainGPUV2GeneratedVGEmitTicket(result);
  publish.gpuWroteEncodedPageData = false;
  publish.gpuWroteDecodedGeometry = true;
  publish.enqueuePageDecode = false;
  publish.emittedClusterCount = result.emittedClusters;
  publish.emittedDecodedVertexCount = result.emittedVertices;
  publish.emittedDecodedIndexCount = result.emittedIndices;
  if (manager.requiresBlasManifest() && !terrainGPUV2GeneratedVGBlasManifest(result, publish.blasClusters))
    return fail("terrain generated VG tile counts do not describe the emitted BLAS clusters");
  return prepareOnly ? manager.prepareCompleted(publish, error) : manager.publishCompleted(publish, error);
}

inline bool publishTerrainGPUV2GeneratedVGEmitResult(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    const virtualgeometry::VirtualGeometryGeneratedPayloadManager::
        GpuWriteDescriptor &descriptor,
    const TerrainGPUV2GeneratedVGEmitResultGPU &result,
    std::string *error = nullptr, bool prepareOnly = false)
{
  if (error != nullptr)
    error->clear();
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };
  if (result.status != static_cast<uint32_t>(TerrainGPUV2GeneratedVGEmitStatus::Ready))
    return fail("terrain generated VG emit result is not ready");
  if (result.validationErrors != 0u)
    return fail("terrain generated VG emit result has validation errors");
  if (result.localPageIndex != descriptor.localPageIndex ||
      result.globalPageTableIndex != descriptor.globalPageTableIndex)
    return fail("terrain generated VG emit result does not match the reserved VG page");
  if (terrainGPUV2GeneratedVGEmitTicket(result) != descriptor.ticketId)
    return fail("terrain generated VG emit result does not match the reserved VG ticket");
  if (result.emittedFaces == 0u ||
      result.emittedTriangles == 0u ||
      result.emittedVertices == 0u ||
      result.emittedIndices == 0u ||
      result.emittedClusters == 0u)
    return fail("terrain generated VG emit result has no drawable payload");
  if (result.emittedClusters > descriptor.clusterCount ||
      result.emittedVertices > descriptor.decodedVertexCount ||
      result.emittedIndices > descriptor.decodedIndexCount)
    return fail("terrain generated VG emit result exceeds the reserved VG page payload");
  virtualgeometry::VirtualGeometryGeneratedPayloadManager::PublishInfo publish{};
  publish.ticketId = descriptor.ticketId;
  publish.gpuWroteEncodedPageData = false;
  publish.gpuWroteDecodedGeometry = true;
  publish.enqueuePageDecode = false;
  publish.emittedClusterCount = result.emittedClusters;
  publish.emittedDecodedVertexCount = result.emittedVertices;
  publish.emittedDecodedIndexCount = result.emittedIndices;
  if (manager.requiresBlasManifest() && !terrainGPUV2GeneratedVGBlasManifest(result, publish.blasClusters))
    return fail("terrain generated VG tile counts do not describe the emitted BLAS clusters");
  return prepareOnly ? manager.prepareCompleted(publish, error) : manager.publishCompleted(publish, error);
}

class TerrainGPUV2GeneratedVGEmitPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset = 0u,
    // Six neighbour brick indices per brick, built once per transaction so the
    // face tests are a lookup rather than a search of the page.
    Neighbours = 1u,
    Count = 2u,
    CountFinalize = 3u,
    EmitFaces = 4u,
    Finalize = 5u,
    UpdateFlags = 6u,
  };
  static constexpr uint32_t kPassCount = 7u;

  struct Buffers
  {
    ::rendering::Buffer uniforms;
    ::rendering::Buffer requests;
    ::rendering::Buffer fineResults;
    ::rendering::Buffer sourceNodes;
    ::rendering::Buffer sourceOccupancy;
    ::rendering::Buffer sourceMaterialWords;
    ::rendering::Buffer decodedClusterCull;
    ::rendering::Buffer decodedClusterGeometry;
    ::rendering::Buffer decodedVertices;
    ::rendering::Buffer decodedIndices;
    ::rendering::Buffer counters;
    ::rendering::Buffer results;
    ::rendering::Buffer indirectArgs;
    ::rendering::Buffer flagRequests;
    ::rendering::Buffer neighbourNodes;
  };

  struct Capacities
  {
    uint32_t requests = 1u;
    uint32_t sourceNodeLaneStride = 1u;
    uint32_t sourceLanes = 1u;
    uint32_t maxClustersPerRequest = virtualgeometry::VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE;
    uint32_t decodedClusterCapacity = virtualgeometry::VirtualGeometryScene::MAX_DECODED_CLUSTERS;
    uint32_t decodedVertexCapacity = 1u;
    uint32_t decodedIndexCapacity = 1u;
    uint32_t materialWordCapacity = 1u;
    uint32_t flagRequests = 64u;
    uint32_t workgroup = 128u;
  };

  TerrainGPUV2GeneratedVGEmitPass(
      const Stage stage,
      const Buffers buffers,
      const Capacities capacities,
      std::string nameSuffix = {})
      : stage_(stage), buffers_(buffers), capacities_(capacities),
        nameSuffix_(std::move(nameSuffix))
  {
    if ((capacities_.workgroup != 64u && capacities_.workgroup != 128u &&
            capacities_.workgroup != 256u) ||
        capacities_.requests == 0u ||
        capacities_.sourceNodeLaneStride == 0u ||
        capacities_.sourceLanes == 0u ||
        capacities_.maxClustersPerRequest == 0u ||
        capacities_.decodedClusterCapacity == 0u ||
        capacities_.decodedVertexCapacity == 0u ||
        capacities_.decodedIndexCapacity == 0u ||
        capacities_.flagRequests == 0u ||
        !buffers_.uniforms.isValid() ||
        !buffers_.requests.isValid() ||
        !buffers_.fineResults.isValid() ||
        !buffers_.sourceNodes.isValid() ||
        !buffers_.sourceOccupancy.isValid() ||
        !buffers_.sourceMaterialWords.isValid() ||
        !buffers_.decodedClusterCull.isValid() ||
        !buffers_.decodedClusterGeometry.isValid() ||
        !buffers_.decodedVertices.isValid() ||
        !buffers_.decodedIndices.isValid() ||
        !buffers_.counters.isValid() ||
        !buffers_.results.isValid() ||
        !buffers_.indirectArgs.isValid() ||
        !buffers_.flagRequests.isValid() ||
        !buffers_.neighbourNodes.isValid())
    {
      throw std::invalid_argument("terrain GPU V2 generated VG emit pass is invalid");
    }
  }

  ~TerrainGPUV2GeneratedVGEmitPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    commands.cmdBindComputePipeline(pipeline_, enabled_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u, enabled_);
    commands.cmdDispatchIndirect(
        buffers_.indirectArgs,
        static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU),
        enabled_);
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

  static const char *entryPoint(const Stage stage)
  {
    switch (stage)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2GeneratedVGEmit";
    case Stage::Neighbours:
      return "buildTerrainGPUV2GeneratedVGNeighbours";
    case Stage::Count:
      return "countTerrainGPUV2GeneratedVGFaces";
    case Stage::CountFinalize:
      return "finalizeTerrainGPUV2GeneratedVGCount";
    case Stage::EmitFaces:
      return "emitTerrainGPUV2GeneratedVGFaces";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2GeneratedVGEmit";
    case Stage::UpdateFlags:
      return "updateTerrainGPUV2GeneratedVGFlags";
    }
    return "";
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer,
      const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    ::rendering::BindingGroupLayout group{};
    const auto add = [&](const uint32_t binding, const std::string &name)
    {
      group.buffers.push_back({
        .name = name,
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    };
    group.buffers.push_back({
      .name = "uniforms",
      .binding = 0u,
      .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      .isDynamic = false,
    });
    add(1u, "requests");
    add(2u, "fineResults");
    add(3u, "sourceNodes");
    add(4u, "sourceOccupancy");
    add(5u, "sourceMaterialWords");
    add(6u, "decodedClusterCull");
    add(7u, "decodedClusterGeometry");
    add(8u, "decodedVertices");
    add(9u, "decodedIndices");
    add(10u, "counters");
    add(11u, "results");
    add(12u, "flagRequests");
    add(13u, "neighbourNodes");
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + ".Layout" + nameSuffix_,
         .groups = {std::move(group)}});
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    const uint64_t sourceNodes =
        uint64_t{capacities_.sourceLanes} *
        capacities_.sourceNodeLaneStride;
    const uint64_t counterWords =
        uint64_t{capacities_.requests} * kTerrainGPUV2GeneratedVGEmitCounterWords;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups" + nameSuffix_,
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.uniforms, sizeof(TerrainGPUV2GeneratedVGEmitUniformsGPU), read), .binding = 0u},
             {.bufferView = view(buffers_.requests, uint64_t{capacities_.requests} * sizeof(TerrainGPUV2GeneratedVGEmitRequestGPU), read), .binding = 1u},
             {.bufferView = view(buffers_.fineResults, uint64_t{capacities_.sourceLanes} * sizeof(TerrainGPUV2FineRasterResultGPU), read), .binding = 2u},
             {.bufferView = view(buffers_.sourceNodes, sourceNodes * sizeof(SparseVoxelBrickNodeGPU), read), .binding = 3u},
             {.bufferView = view(buffers_.sourceOccupancy, sourceNodes * sizeof(SparseVoxelBrickOccupancyGPU), read), .binding = 4u},
             {.bufferView = view(buffers_.sourceMaterialWords, uint64_t{std::max(capacities_.materialWordCapacity, 1u)} * sizeof(uint32_t), read), .binding = 5u},
             {.bufferView = view(buffers_.decodedClusterCull, uint64_t{capacities_.decodedClusterCapacity} * sizeof(virtualgeometry::DecodedClusterCullData), rw), .binding = 6u},
             {.bufferView = view(buffers_.decodedClusterGeometry, uint64_t{capacities_.decodedClusterCapacity} * sizeof(virtualgeometry::DecodedClusterGeometryInfo), rw), .binding = 7u},
             {.bufferView = view(buffers_.decodedVertices, uint64_t{capacities_.decodedVertexCapacity} * sizeof(virtualgeometry::DecodedVertexData), rw), .binding = 8u},
             {.bufferView = view(buffers_.decodedIndices, uint64_t{capacities_.decodedIndexCapacity} * sizeof(uint32_t), rw), .binding = 9u},
             {.bufferView = view(buffers_.counters, counterWords * sizeof(uint32_t), rw), .binding = 10u},
             {.bufferView = view(buffers_.results, uint64_t{capacities_.requests} * sizeof(TerrainGPUV2GeneratedVGEmitResultGPU), rw), .binding = 11u},
             {.bufferView = view(buffers_.flagRequests, uint64_t{capacities_.flagRequests} * sizeof(TerrainGPUV2GeneratedVGFlagRequestGPU), read), .binding = 12u},
             {.bufferView = view(buffers_.neighbourNodes, sourceNodes * 6u * sizeof(uint32_t), rw), .binding = 13u},
           }}}});
    const std::string path =
        "assets/shaders/spirv/svraster/terrain-gpu-v2-generated-vg-emit-" +
        std::to_string(capacities_.workgroup) + ".spirv";
    shader_ = renderGraph->createShader(
        {.name = passName + ".Shader" + nameSuffix_,
         .src = os::io::readRelativeFile(path),
         .type = ::rendering::ShaderType::SpirV,
         .layout = layout_});
    pipeline_ = renderGraph->createComputePipeline(
        {.name = passName + ".Pipeline" + nameSuffix_,
         .shader = shader_,
         .entry = entryPoint(stage_),
         .layout = layout_});
  }

  Stage stage_{};
  Buffers buffers_{};
  Capacities capacities_{};
  std::string nameSuffix_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
};

// The six emit stages over one request batch plus the flag requests.
class TerrainGPUV2GeneratedVGEmitChain final
{
public:
  struct SourceBuffers
  {
    ::rendering::Buffer fineResults;
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
  };
  struct DestinationBuffers
  {
    ::rendering::Buffer decodedClusterCull;
    ::rendering::Buffer decodedClusterGeometry;
    ::rendering::Buffer decodedVertices;
    ::rendering::Buffer decodedIndices;
  };

  TerrainGPUV2GeneratedVGEmitChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const SourceBuffers source,
      const DestinationBuffers destination,
      const TerrainGPUV2GeneratedVGEmitPass::Capacities capacities,
      std::string name = "TerrainGPUV2.GeneratedVGEmit")
      : graph_(&graph), capacities_(capacities), name_(std::move(name))
  {
    if (!source.fineResults.isValid() ||
        !source.nodes.isValid() ||
        !source.occupancy.isValid() ||
        !source.materialWords.isValid() ||
        !destination.decodedClusterCull.isValid() ||
        !destination.decodedClusterGeometry.isValid() ||
        !destination.decodedVertices.isValid() ||
        !destination.decodedIndices.isValid())
      throw std::invalid_argument("terrain generated VG emit chain buffers are invalid");
    createBuffers();
    const TerrainGPUV2GeneratedVGEmitPass::Buffers buffers{
      uniforms_,
      requests_,
      source.fineResults,
      source.nodes,
      source.occupancy,
      source.materialWords,
      destination.decodedClusterCull,
      destination.decodedClusterGeometry,
      destination.decodedVertices,
      destination.decodedIndices,
      counters_,
      results_,
      indirectArgs_,
      flagRequests_,
      neighbourNodes_};
    for (uint32_t stage = 0u; stage < TerrainGPUV2GeneratedVGEmitPass::kPassCount; ++stage)
    {
      passes_[stage] = graph_->registerPass<TerrainGPUV2GeneratedVGEmitPass>(
          name_ + ".Stage" + std::to_string(stage),
          passIndex++,
          static_cast<TerrainGPUV2GeneratedVGEmitPass::Stage>(stage),
          buffers,
          capacities_,
          ".Stage" + std::to_string(stage));
    }
  }

  ~TerrainGPUV2GeneratedVGEmitChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(neighbourNodes_);
    graph_->deleteBuffer(flagRequests_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(counters_);
    graph_->deleteBuffer(requests_);
    graph_->deleteBuffer(uniforms_);
  }

  // Requests carry their own mode in `flags` (kTerrainGPUV2GeneratedVGEmitCountMode):
  // a batch may mix pages being counted with pages being emitted. Flag
  // requests run in the same submission, after the emit stages.
  bool update(
      const std::vector<TerrainGPUV2GeneratedVGEmitRequestGPU> &requests,
      const std::vector<TerrainGPUV2GeneratedVGFlagRequestGPU> &flagRequests = {},
      std::string *error = nullptr)
  {
    if (error != nullptr)
      error->clear();
    if (requests.size() > capacities_.requests)
    {
      if (error != nullptr)
        *error = "terrain generated VG emit request batch exceeds capacity";
      return false;
    }
    if (flagRequests.size() > capacities_.flagRequests)
    {
      if (error != nullptr)
        *error = "terrain generated VG flag request batch exceeds capacity";
      return false;
    }
    for (const auto &request : requests)
      if (request.clusterCount > capacities_.maxClustersPerRequest)
      {
        if (error != nullptr)
          *error = "terrain generated VG emit request exceeds per-page cluster capacity";
        return false;
      }
    const uint32_t requestCount = static_cast<uint32_t>(requests.size());
    const uint32_t flagRequestCount = static_cast<uint32_t>(flagRequests.size());
    TerrainGPUV2GeneratedVGEmitUniformsGPU uniforms{};
    uniforms.requestCount = requestCount;
    uniforms.counterStride = kTerrainGPUV2GeneratedVGEmitCounterWords;
    uniforms.sourceNodeLaneStride = capacities_.sourceNodeLaneStride;
    uniforms.flagRequestCount = flagRequestCount;
    uniforms.faceThreadCount =
        kTerrainGPUV2GeneratedVGEmitFaceWorkgroups * capacities_.workgroup;
    graph_->bufferWrite(uniforms_, 0u, sizeof(uniforms), &uniforms);
    if (!requests.empty())
      graph_->bufferWrite(
          requests_, 0u, requests.size() * sizeof(requests[0]),
          const_cast<TerrainGPUV2GeneratedVGEmitRequestGPU *>(requests.data()));
    if (!flagRequests.empty())
      graph_->bufferWrite(
          flagRequests_, 0u, flagRequests.size() * sizeof(flagRequests[0]),
          const_cast<TerrainGPUV2GeneratedVGFlagRequestGPU *>(flagRequests.data()));
    std::array<IndirectDispatchArgsGPU, TerrainGPUV2GeneratedVGEmitPass::kPassCount> indirect{};
    const auto groups = [&](const uint64_t work)
    {
      return static_cast<uint32_t>(
          (work + capacities_.workgroup - 1u) / capacities_.workgroup);
    };
    if (!requests.empty())
    {
      uint32_t maxClusters = 1u;
      uint32_t maxSourceNodes = 1u;
      bool anyCount = false;
      bool anyEmit = false;
      for (const auto &request : requests)
      {
        maxClusters = std::max(maxClusters, request.clusterCount);
        maxSourceNodes = std::max(maxSourceNodes, request.sourceNodeCount);
        if ((request.flags & kTerrainGPUV2GeneratedVGEmitCountMode) != 0u)
          anyCount = true;
        else
          anyEmit = true;
      }
      (void)maxSourceNodes;
      const uint32_t faceCandidates = kTerrainGPUV2GeneratedVGEmitFaceWorkgroups;
      const uint32_t resetWork = groups(
          std::max<uint32_t>(maxClusters, kTerrainGPUV2GeneratedVGEmitCounterWords));
      indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::Reset)] =
          {resetWork, 1u, requestCount, 0u};
      indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::Neighbours)] =
          {groups(uint64_t{maxSourceNodes} * 6u), 1u, requestCount, 0u};
      if (anyCount)
      {
        indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::Count)] =
            {faceCandidates, 1u, requestCount, 0u};
        indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::CountFinalize)] =
            {groups(kTerrainGPUV2GeneratedVGEmitTileCount), 1u, requestCount, 0u};
      }
      if (anyEmit)
      {
        indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::EmitFaces)] =
            {faceCandidates, 1u, requestCount, 0u};
        indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::Finalize)] =
            {groups(std::max<uint32_t>(maxClusters, kTerrainGPUV2GeneratedVGEmitTileCount)), 1u, requestCount, 0u};
      }
    }
    if (!flagRequests.empty())
    {
      uint32_t maxFlagClusters = 1u;
      for (const auto &request : flagRequests)
        maxFlagClusters = std::max(maxFlagClusters, request.clusterCount);
      indirect[static_cast<size_t>(TerrainGPUV2GeneratedVGEmitPass::Stage::UpdateFlags)] =
          {groups(maxFlagClusters), 1u, flagRequestCount, 0u};
    }
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    const bool enabled = !requests.empty() || !flagRequests.empty();
    for (auto *pass : passes_)
      pass->setEnabled(enabled);
    return true;
  }

  ::rendering::Buffer results() const
  {
    return results_;
  }
  ::rendering::Buffer requests() const
  {
    return requests_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirectArgs_;
  }
  uint64_t scratchBytes() const
  {
    return uint64_t{capacities_.requests} *
        kTerrainGPUV2GeneratedVGEmitCounterWords * sizeof(uint32_t);
  }

private:
  ::rendering::Buffer storage(
      const std::string &suffix,
      const uint64_t size,
      const ::rendering::BufferUsage extra =
          ::rendering::BufferUsage::BufferUsage_None)
  {
    return graph_->createBuffer({
      .name = name_ + suffix,
      .size = size,
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra,
      // CPU updates must not overwrite inputs of an in-flight frame.
      .frameLocal = (extra & ::rendering::BufferUsage::BufferUsage_Push) != 0});
  }

  void createBuffers()
  {
    if (capacities_.requests == 0u ||
        capacities_.maxClustersPerRequest == 0u ||
        capacities_.sourceNodeLaneStride == 0u ||
        capacities_.flagRequests == 0u)
      throw std::invalid_argument("terrain generated VG emit capacity is invalid");
    uniforms_ = graph_->createBuffer({
      .name = name_ + ".Uniforms",
      .size = sizeof(TerrainGPUV2GeneratedVGEmitUniformsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    requests_ = storage(
        ".Requests",
        uint64_t{capacities_.requests} *
            sizeof(TerrainGPUV2GeneratedVGEmitRequestGPU),
        ::rendering::BufferUsage::BufferUsage_Push);
    neighbourNodes_ = storage(
        ".NeighbourNodes",
        uint64_t{capacities_.sourceLanes} * capacities_.sourceNodeLaneStride *
            6ull * sizeof(uint32_t));
    flagRequests_ = storage(
        ".FlagRequests",
        uint64_t{capacities_.flagRequests} *
            sizeof(TerrainGPUV2GeneratedVGFlagRequestGPU),
        ::rendering::BufferUsage::BufferUsage_Push);
    counters_ = storage(
        ".Counters",
        uint64_t{capacities_.requests} *
            kTerrainGPUV2GeneratedVGEmitCounterWords * sizeof(uint32_t));
    results_ = storage(
        ".Results",
        uint64_t{capacities_.requests} *
            sizeof(TerrainGPUV2GeneratedVGEmitResultGPU),
        ::rendering::BufferUsage::BufferUsage_CopySrc);
    indirectArgs_ = graph_->createBuffer({
      .name = name_ + ".IndirectArgs",
      .size = TerrainGPUV2GeneratedVGEmitPass::kPassCount *
          sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    std::array<IndirectDispatchArgsGPU, TerrainGPUV2GeneratedVGEmitPass::kPassCount> idle{};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(idle), idle.data());
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  TerrainGPUV2GeneratedVGEmitPass::Capacities capacities_{};
  std::string name_{};
  std::array<TerrainGPUV2GeneratedVGEmitPass *, TerrainGPUV2GeneratedVGEmitPass::kPassCount> passes_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer requests_{};
  ::rendering::Buffer flagRequests_{};
  ::rendering::Buffer neighbourNodes_{};
  ::rendering::Buffer counters_{};
  ::rendering::Buffer results_{};
  ::rendering::Buffer indirectArgs_{};
};

// Columns -> fine raster -> emit for a batch of lanes. A lane's second
// transaction (the emit after the count) reuses the columns and bricks the
// first one produced: `reuseSource` keeps the column and raster stages idle.
class TerrainGPUV2GeneratedVGPageBuildChain final
{
public:
  struct Capacities
  {
    TerrainGPUV2ColumnChain::Capacities columns{};
    TerrainGPUV2FineRasterConfigGPU fineRaster{};
    TerrainGPUV2GeneratedVGEmitPass::Capacities emit{};
    uint32_t workgroup = 128u;
    uint32_t lanes = 1u;
  };

  enum class EmitMode : uint8_t
  {
    None,
    Count,
    Emit,
  };

  struct Update
  {
    TerrainGPUV2ColumnConfigGPU columns{};
    TerrainGPUV2FineRasterConfigGPU raster{};
    TerrainGPUV2GeneratedVGEmitRequestGPU emit{};
    EmitMode emitMode = EmitMode::None;
    bool reuseSource = false;
  };

  TerrainGPUV2GeneratedVGPageBuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPUV2ColumnChain::AtlasBuffers atlas,
      const TerrainGPUV2GeneratedVGEmitChain::DestinationBuffers destination,
      const Capacities capacities,
      std::string name = "TerrainGPUV2.GeneratedVG")
      : graph_(&graph), capacities_(capacities), name_(std::move(name))
  {
    if (capacities_.fineRaster.nodeCapacity == 0u ||
        capacities_.fineRaster.materialCapacity == 0u ||
        capacities_.lanes == 0u)
      throw std::invalid_argument("terrain generated VG page-build capacity is invalid");
    auto columnCapacities = capacities_.columns;
    columnCapacities.lanes = capacities_.lanes;
    columns_ = std::make_unique<TerrainGPUV2ColumnChain>(
        graph, passIndex, atlas, columnCapacities, name_ + ".Columns");
    fine_ = std::make_unique<TerrainGPUV2FineRasterChain>(
        graph,
        passIndex,
        TerrainGPUV2FineRasterChain::ColumnBuffers{
          columns_->columns(), columns_->result()},
        capacities_.fineRaster,
        capacities_.workgroup,
        capacities_.lanes,
        name_ + ".FineRaster");
    auto emitCapacities = capacities_.emit;
    emitCapacities.requests = std::max(emitCapacities.requests, capacities_.lanes);
    emitCapacities.sourceNodeLaneStride = capacities_.fineRaster.nodeCapacity;
    emitCapacities.sourceLanes = capacities_.lanes;
    emitCapacities.materialWordCapacity =
        capacities_.fineRaster.materialWordCapacity * capacities_.lanes;
    emitCapacities.workgroup = capacities_.workgroup;
    emit_ = std::make_unique<TerrainGPUV2GeneratedVGEmitChain>(
        graph,
        passIndex,
        TerrainGPUV2GeneratedVGEmitChain::SourceBuffers{
          fine_->result(), fine_->nodes(), fine_->occupancy(),
          fine_->materialWords()},
        destination,
        emitCapacities,
        name_ + ".EmitDecodedVG");
  }

  ~TerrainGPUV2GeneratedVGPageBuildChain()
  {
    emit_.reset();
    fine_.reset();
    columns_.reset();
  }

  bool updateBatch(
      const std::vector<Update> &updates,
      const std::vector<TerrainGPUV2GeneratedVGFlagRequestGPU> &flagRequests = {},
      std::string *error = nullptr)
  {
    if (error != nullptr)
      error->clear();
    if (updates.empty() || updates.size() > capacities_.lanes)
    {
      if (error != nullptr)
        *error = "terrain generated VG page-build lane count is invalid";
      return false;
    }
    std::vector<TerrainGPUV2ColumnConfigGPU> columnConfigs;
    std::vector<TerrainGPUV2FineRasterConfigGPU> rasterConfigs;
    std::vector<TerrainGPUV2GeneratedVGEmitRequestGPU> emitRequests;
    columnConfigs.reserve(updates.size());
    rasterConfigs.reserve(updates.size());
    emitRequests.reserve(updates.size());
    bool anySource = false;
    for (uint32_t lane = 0u; lane < updates.size(); ++lane)
    {
      columnConfigs.push_back(updates[lane].columns);
      rasterConfigs.push_back(updates[lane].raster);
      anySource = anySource || !updates[lane].reuseSource;
      if (updates[lane].emitMode != EmitMode::None)
      {
        auto request = updates[lane].emit;
        request.sourceLane = lane;
        request.sourceNodeCapacity = capacities_.fineRaster.nodeCapacity;
        if (request.sourceNodeCount == 0u ||
            request.sourceNodeCount > request.sourceNodeCapacity)
          request.sourceNodeCount = request.sourceNodeCapacity;
        if (updates[lane].emitMode == EmitMode::Count)
          request.flags |= kTerrainGPUV2GeneratedVGEmitCountMode;
        else
          request.flags &= ~kTerrainGPUV2GeneratedVGEmitCountMode;
        emitRequests.push_back(request);
      }
    }
    // A lane batch either regenerates every source or reuses every source:
    // the column and raster chains dispatch all lanes together.
    if (anySource)
    {
      columns_->setEnabled(true);
      fine_->setEnabled(true);
      if (!columns_->updateBatch(columnConfigs) ||
          !fine_->updateBatch(rasterConfigs))
      {
        if (error != nullptr)
          *error = "terrain generated VG page-build source update failed";
        return false;
      }
    }
    else
    {
      columns_->setEnabled(false);
      fine_->setEnabled(false);
    }
    return emit_->update(emitRequests, flagRequests, error);
  }

  bool update(const Update &update, std::string *error = nullptr)
  {
    return updateBatch(std::vector<Update>{update}, {}, error);
  }

  // Flag rewrites alone, with every generation stage idle.
  bool updateFlags(
      const std::vector<TerrainGPUV2GeneratedVGFlagRequestGPU> &flagRequests,
      std::string *error = nullptr)
  {
    if (error != nullptr)
      error->clear();
    columns_->setEnabled(false);
    fine_->setEnabled(false);
    return emit_->update({}, flagRequests, error);
  }

  bool idle(std::string *error = nullptr)
  {
    return updateFlags({}, error);
  }

  ::rendering::Buffer results() const
  {
    return emit_->results();
  }
  ::rendering::Buffer columnResults() const
  {
    return columns_->result();
  }
  ::rendering::Buffer fineRasterResults() const
  {
    return fine_->result();
  }
  ::rendering::Buffer fineRasterNodes() const
  {
    return fine_->nodes();
  }
  ::rendering::Buffer fineRasterOccupancy() const
  {
    return fine_->occupancy();
  }
  ::rendering::Buffer fineRasterMaterialWords() const
  {
    return fine_->materialWords();
  }
  uint64_t scratchBytes() const
  {
    return columns_->scratchBytes() + fine_->scratchBytes() + emit_->scratchBytes();
  }
  uint64_t hiddenOutputBytes() const
  {
    return columns_->outputBytes() + fine_->outputBytes();
  }

private:
  ::rendering::RenderGraph *graph_ = nullptr;
  Capacities capacities_{};
  std::string name_{};
  std::unique_ptr<TerrainGPUV2ColumnChain> columns_{};
  std::unique_ptr<TerrainGPUV2FineRasterChain> fine_{};
  std::unique_ptr<TerrainGPUV2GeneratedVGEmitChain> emit_{};
};

} // namespace voxel::svraster::rendering
