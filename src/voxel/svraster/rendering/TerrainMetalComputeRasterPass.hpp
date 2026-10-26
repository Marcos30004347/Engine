#pragma once

#include "rendering/gpu/RHI.hpp"
#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace voxel::svraster::rendering
{

inline constexpr const char *kTerrainMetalComputeRasterShaderPath =
    "assets/shaders/voxel/svraster/metal/"
    "sparse_voxel_terrain_compute_raster_atomic64.metal";
#if defined(SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METALLIB_PATH)
inline constexpr const char *kTerrainMetalComputeRasterMetallibPath =
    SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METALLIB_PATH;
#else
inline constexpr const char *kTerrainMetalComputeRasterMetallibPath =
    "tests/assets/shaders/metal/svraster/"
    "sparse-voxel-terrain-compute-raster-atomic64.metallib";
#endif
inline constexpr uint32_t kTerrainMetalComputeRasterTileSize = 16u;
inline constexpr uint32_t kTerrainMetalComputeRasterKernelCount = 9u;

struct TerrainMetalComputeRasterPassPlan
{
  bool enabled = false;
  bool allocateVisibilityKeyBuffer = false;
  bool allocateTileRangeBuffers = false;
  bool allocateResolvedPayloadBuffer = false;
  bool registerNativeMetalEncoder = false;
  bool requiresMetalBufferInterop = true;
  bool usesPacked64DepthPayloadAtomic = false;
  bool bypassesMoltenVKAtomic64Feature = false;
  uint32_t tileSize = kTerrainMetalComputeRasterTileSize;
  double projectedTriangleEdgePixelThreshold = 0.0;
  const char *shaderPath = kTerrainMetalComputeRasterShaderPath;
  std::array<const char *, kTerrainMetalComputeRasterKernelCount> entryPoints{
      "terrainComputeRasterAtomic64Clear",
      "terrainComputeRasterAtomic64ClearTiles",
      "terrainComputeRasterAtomic64CountTiles",
      "terrainComputeRasterAtomic64ScatterTiles",
      "terrainComputeRasterAtomic64RasterizeTiles",
      "terrainComputeRasterAtomic64CountDecodedVGClusters",
      "terrainComputeRasterAtomic64ScatterDecodedVGClusters",
      "terrainComputeRasterAtomic64RasterizeDecodedVG",
      "terrainComputeRasterAtomic64Merge"};
  std::string reason{};
};

inline bool buildTerrainMetalComputeRasterPassPlan(
    const SparseVoxelTerrainComputeRasterRouteDecision &route,
    const SparseVoxelTerrainComputeRasterCapabilities &capabilities,
    const SparseVoxelTerrainComputeRasterSettings &settings,
    TerrainMetalComputeRasterPassPlan &plan,
    std::string *error = nullptr)
{
  plan = {};
  if (error != nullptr)
    error->clear();

  if (!route.enabled ||
      route.backend != SparseVoxelTerrainComputeRasterBackend::NativeMetalAtomic64)
  {
    plan.reason = route.reason.empty()
        ? "native Metal compute raster route is not selected"
        : route.reason;
    return true;
  }

  if (!capabilities.nativeMetalDeviceAvailable ||
      !capabilities.nativeMetalQueueCompatibleWithMoltenVK)
  {
    if (error != nullptr)
      *error =
          "native Metal compute raster needs the MoltenVK-exported Metal "
          "device and compatible command queue";
    return false;
  }

  if (!capabilities.nativeMetalBufferInteropAvailable)
  {
    if (error != nullptr)
      *error =
          "native Metal compute raster needs shared/exported Metal buffers for "
          "decoded VG clusters/vertices/indices, SW cluster queues, "
          "visibility keys and merged depth/payload outputs";
    return false;
  }

  if (!capabilities.nativeMetalSupportsAtomic64MinMax ||
      !route.usesPacked64DepthPayloadAtomic)
  {
    if (error != nullptr)
      *error =
          "native Metal compute raster needs 64-bit min/max atomics for packed "
          "depth/payload visibility keys";
    return false;
  }

  plan.enabled = true;
  plan.allocateVisibilityKeyBuffer = true;
  plan.allocateTileRangeBuffers = true;
  plan.allocateResolvedPayloadBuffer = true;
  plan.registerNativeMetalEncoder = true;
  plan.requiresMetalBufferInterop = true;
  plan.usesPacked64DepthPayloadAtomic = true;
  plan.bypassesMoltenVKAtomic64Feature =
      route.bypassesMoltenVKAtomic64Feature;
  plan.tileSize = kTerrainMetalComputeRasterTileSize;
  plan.projectedTriangleEdgePixelThreshold =
      settings.projectedTriangleEdgePixelThreshold;
  plan.reason =
      "native Metal VG compute raster registers the atomic64 clear, bin, "
      "scatter, raster and deterministic hardware/SW depth-merge kernels";
  return true;
}

#if defined(__APPLE__)

class TerrainMetalComputeRasterPass
{
public:
  struct Configuration
  {
    std::string metallibPath = kTerrainMetalComputeRasterMetallibPath;
    bool visibilityOnly = false;
  };

  struct Uniforms
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t tileGridWidth = 0u;
    uint32_t tileGridHeight = 0u;
    uint32_t triangleCount = 0u;
    // Total number of entries in tileTriangleIndices.
    uint32_t tileReferenceCapacity = 0u;
    uint32_t payloadMask = 0xffffffffu;
    // Per-tile fixed bin size for the native path. Keeping this in the old
    // reserved slot preserves the compact 32-byte ABI while avoiding a GPU
    // prefix-sum dependency between CountTiles and ScatterTiles.
    uint32_t tileCandidateCapacity = 0u;
  };

  struct Frame
  {
    ::rendering::BufferId visibilityKeys = ::rendering::BufferId::Invalid;
    ::rendering::BufferId resolvedPayloads = ::rendering::BufferId::Invalid;
    ::rendering::BufferId uniforms = ::rendering::BufferId::Invalid;
    ::rendering::BufferId triangles = ::rendering::BufferId::Invalid;
    ::rendering::BufferId tileRanges = ::rendering::BufferId::Invalid;
    ::rendering::BufferId tileTriangleIndices = ::rendering::BufferId::Invalid;
    ::rendering::BufferId overflowCounter = ::rendering::BufferId::Invalid;
    ::rendering::BufferId cullingUniforms = ::rendering::BufferId::Invalid;
    ::rendering::BufferId cullingCounters = ::rendering::BufferId::Invalid;
    ::rendering::BufferId swVisibleClusterInfos = ::rendering::BufferId::Invalid;
    ::rendering::BufferId instances = ::rendering::BufferId::Invalid;
    ::rendering::BufferId pageTable = ::rendering::BufferId::Invalid;
    ::rendering::BufferId decodedClusterCull = ::rendering::BufferId::Invalid;
    ::rendering::BufferId decodedClusterGeometry = ::rendering::BufferId::Invalid;
    ::rendering::BufferId decodedVertices = ::rendering::BufferId::Invalid;
    ::rendering::BufferId decodedIndices = ::rendering::BufferId::Invalid;
    // Sparse scene-instance id -> dense index, for resolving software-raster
    // payloads back to their owning instance.
    ::rendering::BufferId instanceSparseToDense = ::rendering::BufferId::Invalid;
    bool tileRangesPrepared = false;
    bool directDecodedVGPrepared = false;
  };

  explicit TerrainMetalComputeRasterPass(::rendering::RHI &rhi);
  TerrainMetalComputeRasterPass(
      ::rendering::RHI &rhi,
      const Configuration &configuration);
  ~TerrainMetalComputeRasterPass();

  TerrainMetalComputeRasterPass(const TerrainMetalComputeRasterPass &) = delete;
  TerrainMetalComputeRasterPass &operator=(
      const TerrainMetalComputeRasterPass &) = delete;

  bool available() const;
  struct VisibilityFrame
  {
    // Same bindings 0..8 as VirtualGeometrySoftwareRasterPass; 9 is packed
    // visibility, 10/11 are the merge inputs, 12 is indirect dispatch args,
    // and 13 is the encoded geometry page pool.
    std::array<::rendering::BufferId, 14> buffers{};
    uint32_t width = 0u, height = 0u;
  };
  bool visibilityAvailable() const;
  void encodeVisibility(const VisibilityFrame &, void *sharedEvent,
      uint64_t readyValue, uint64_t doneValue);

  bool encodePreparedTiles(
      const Frame &frame,
      const Uniforms &uniforms,
      std::string *error = nullptr);
  bool encodeTriangleStream(
      const Frame &frame,
      const Uniforms &uniforms,
      std::string *error = nullptr);
  bool encodeDecodedVG(
      const Frame &frame,
      const Uniforms &uniforms,
      std::string *error = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif // defined(__APPLE__)

} // namespace voxel::svraster::rendering
