#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace virtualgeometry::gpgpu
{

// Virtual-geometry software/compute raster side path.
//
// Culling owns the cluster routing decision and emits the SW-visible cluster
// list. The production path consumes VirtualGeometryScene's decoded cluster,
// vertex and index buffers directly, rasterizes routed clusters into its own
// depth/payload attachment, then merges deterministically with the hardware
// path. The transient triangle stream below remains a smoke/test backend for
// the native Metal atomic64 encoder; it is not the required production
// hand-off.
class VirtualGeometryComputeRasterPass : public rendering::Pass
{
public:
  static constexpr uint32_t TILE_SIZE = 16u;
  static constexpr uint32_t DEFAULT_TRIANGLES_PER_CLUSTER = 128u;

  enum class Backend : uint32_t
  {
    None = 0u,
    NativeMetalAtomic64,
    VulkanTiledAtomic32,
  };

  struct Settings
  {
    bool enabled = false;
    Backend backend = Backend::None;
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t maxSoftwareClusters = VirtualGeometryScene::MAX_VISIBLE_CLUSTERS;
    uint32_t maxTriangles = 0u;
    uint32_t tileCandidateCapacity = 4096u;
    float projectedTriangleEdgePixelThreshold = 32.0f;
    bool metalExportable = false;
    bool registerNativeMetalEncoder = false;
    // `enabled` means the pass may be registered/allocated for native encoder
    // bring-up and smoke tests. Clusters are only allowed to route here once
    // both production pieces exist: direct decoded-VG consumption and a
    // deterministic depth/payload merge with the hardware path.
    bool directDecodedVGConsumer = false;
    bool finalDepthPayloadMerge = false;
  };

  struct BufferPlan
  {
    bool enabled = false;
    bool metalExportable = false;
    bool directDecodedVGConsumer = false;
    bool finalDepthPayloadMerge = false;
    bool routesSoftwareClusters = false;
    uint32_t tileGridWidth = 0u;
    uint32_t tileGridHeight = 0u;
    uint32_t tileCount = 0u;
    uint32_t triangleCapacity = 0u;
    uint32_t tileReferenceCapacity = 0u;
    uint64_t visibilityKeyBytes = 0u;
    uint64_t resolvedPayloadBytes = 0u;
    uint64_t uniformBytes = 0u;
    uint64_t triangleBytes = 0u;
    uint64_t tileRangeBytes = 0u;
    uint64_t tileTriangleIndexBytes = 0u;
    uint64_t overflowCounterBytes = 0u;
  };

  struct Uniforms
  {
    uint32_t viewportWidth = 0u;
    uint32_t viewportHeight = 0u;
    uint32_t tileGridWidth = 0u;
    uint32_t tileGridHeight = 0u;
    uint32_t triangleCount = 0u;
    uint32_t tileReferenceCapacity = 0u;
    uint32_t payloadMask = 0xffffffffu;
    uint32_t tileCandidateCapacity = 0u;
  };
  static_assert(sizeof(Uniforms) == 32u);

  struct Triangle
  {
    float p0[2]{};
    float p1[2]{};
    float p2[2]{};
    float z0 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
    uint32_t payload = 0u;
    uint32_t flags = 0u;
  };
  static_assert(sizeof(Triangle) == 44u);

  struct TileRange
  {
    uint32_t count = 0u;
    uint32_t cursor = 0u;
    uint32_t offset = 0u;
    uint32_t reserved0 = 0u;
  };
  static_assert(sizeof(TileRange) == 16u);

  struct PixelPayload
  {
    uint32_t payload = 0u;
    // Reverse-Z device depth; 0 means "no software hit".
    uint32_t depthBits = 0u;
    // Oct-encoded world normal and the dense instance index, resolved by the
    // decoded-VG merge kernel so G-buffer consumers need no geometry access.
    uint32_t normalOct = 0u;
    uint32_t denseInstance = 0xffffffffu;
  };
  static_assert(sizeof(PixelPayload) == 16u);

  struct FrameBuffers
  {
    rendering::Buffer visibilityKeys{};
    rendering::Buffer resolvedPayloads{};
    rendering::Buffer uniforms{};
    rendering::Buffer triangles{};
    rendering::Buffer tileRanges{};
    rendering::Buffer tileTriangleIndices{};
    rendering::Buffer overflowCounter{};
    rendering::Buffer swDrawIndirect{};
    rendering::Buffer swVisibleClusterInfos{};
    rendering::Buffer cullingCounters{};
    rendering::Buffer cullingUniforms{};
    rendering::Buffer instances{};
    rendering::Buffer pageTable{};
    rendering::Buffer decodedClusterCull{};
    rendering::Buffer decodedClusterGeometry{};
    rendering::Buffer decodedVertices{};
    rendering::Buffer decodedIndices{};
  };

  static bool canRouteSoftwareClusters(const Settings &settings)
  {
    return settings.enabled && settings.backend != Backend::None &&
        settings.directDecodedVGConsumer && settings.finalDepthPayloadMerge &&
        settings.projectedTriangleEdgePixelThreshold > 0.0f;
  }

  static bool buildBufferPlan(
      const Settings &settings,
      BufferPlan &plan,
      std::string *error = nullptr)
  {
    plan = {};
    if (error != nullptr)
      error->clear();
    if (!settings.enabled)
      return true;
    if (settings.backend == Backend::None)
    {
      if (error != nullptr)
        *error = "VG compute raster is enabled without a backend";
      return false;
    }
    if (settings.viewportWidth == 0u || settings.viewportHeight == 0u)
    {
      if (error != nullptr)
        *error = "VG compute raster needs a non-zero viewport";
      return false;
    }
    if (settings.tileCandidateCapacity == 0u)
    {
      if (error != nullptr)
        *error = "VG compute raster needs non-zero tile candidate capacity";
      return false;
    }
    const uint64_t pixelCount =
        uint64_t{settings.viewportWidth} * uint64_t{settings.viewportHeight};
    if (pixelCount > std::numeric_limits<uint32_t>::max())
    {
      if (error != nullptr)
        *error = "VG compute raster viewport exceeds 32-bit pixel addressing";
      return false;
    }
    plan.tileGridWidth =
        (settings.viewportWidth + TILE_SIZE - 1u) / TILE_SIZE;
    plan.tileGridHeight =
        (settings.viewportHeight + TILE_SIZE - 1u) / TILE_SIZE;
    plan.tileCount = plan.tileGridWidth * plan.tileGridHeight;
    const uint64_t tileReferences =
        uint64_t{plan.tileCount} * uint64_t{settings.tileCandidateCapacity};
    if (tileReferences > std::numeric_limits<uint32_t>::max())
    {
      if (error != nullptr)
        *error =
            "VG compute raster tile references exceed the 32-bit Metal ABI";
      return false;
    }
    plan.triangleCapacity = settings.maxTriangles != 0u
        ? settings.maxTriangles
        : settings.maxSoftwareClusters * DEFAULT_TRIANGLES_PER_CLUSTER;
    if (plan.triangleCapacity == 0u)
    {
      if (error != nullptr)
        *error = "VG compute raster needs non-zero triangle capacity";
      return false;
    }
    plan.enabled = true;
    plan.metalExportable =
        settings.metalExportable ||
        settings.backend == Backend::NativeMetalAtomic64 ||
        settings.registerNativeMetalEncoder;
    plan.directDecodedVGConsumer = settings.directDecodedVGConsumer;
    plan.finalDepthPayloadMerge = settings.finalDepthPayloadMerge;
    plan.routesSoftwareClusters = canRouteSoftwareClusters(settings);
    plan.tileReferenceCapacity = static_cast<uint32_t>(tileReferences);
    plan.visibilityKeyBytes = pixelCount * sizeof(uint64_t);
    plan.resolvedPayloadBytes = pixelCount * sizeof(PixelPayload);
    plan.uniformBytes = sizeof(Uniforms);
    plan.triangleBytes =
        uint64_t{plan.triangleCapacity} * sizeof(Triangle);
    plan.tileRangeBytes = uint64_t{plan.tileCount} * sizeof(TileRange);
    plan.tileTriangleIndexBytes =
        uint64_t{plan.tileReferenceCapacity} * sizeof(uint32_t);
    plan.overflowCounterBytes = sizeof(uint32_t);
    return true;
  }

  VirtualGeometryComputeRasterPass(
      VirtualGeometryScene &scene,
      rendering::Buffer swDrawIndirect,
      rendering::Buffer swVisibleClusterInfos,
      rendering::Buffer cullingCounters,
      rendering::Buffer cullingUniforms,
      VirtualGeometryHardwareDrawPass::FrameTarget frameTarget,
      Settings settings)
      : scene_(scene),
        swDrawIndirect_(swDrawIndirect),
        swVisibleClusterInfos_(swVisibleClusterInfos),
        cullingCounters_(cullingCounters),
        cullingUniforms_(cullingUniforms),
        frameTarget_(frameTarget),
        settings_(settings)
  {
  }

  ~VirtualGeometryComputeRasterPass() override
  {
    renderGraph->deleteBuffer(buffers_.overflowCounter);
    renderGraph->deleteBuffer(buffers_.tileTriangleIndices);
    renderGraph->deleteBuffer(buffers_.tileRanges);
    renderGraph->deleteBuffer(buffers_.triangles);
    renderGraph->deleteBuffer(buffers_.uniforms);
    renderGraph->deleteBuffer(buffers_.resolvedPayloads);
    renderGraph->deleteBuffer(buffers_.visibilityKeys);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    std::string error;
    if (!buildBufferPlan(settings_, plan_, &error) || !plan_.enabled)
      return;

    const auto storageExportable = [&](std::string suffix, uint64_t bytes)
    {
      return createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName + suffix,
            .size = bytes,
            .usage = rendering::BufferUsage::BufferUsage_Storage |
                rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = plan_.metalExportable,
          });
    };
    const auto uniformExportable = [&](std::string suffix, uint64_t bytes)
    {
      return createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName + suffix,
            .size = bytes,
            .usage = rendering::BufferUsage::BufferUsage_Uniform |
                rendering::BufferUsage::BufferUsage_Push,
            .metalExportable = plan_.metalExportable,
          });
    };

    // Written entirely by the native Metal encoder, which runs after the
    // graph's submission on the same hardware queue - execution is already
    // serialized, so frame-local instancing buys nothing and actively breaks
    // the G-buffer adapter, whose cached binding group would pin one instance
    // while the encoder alternates between them.
    const auto storageExportablePersistent =
        [&](std::string suffix, uint64_t bytes)
    {
      return renderGraph->createBuffer(
          rendering::BufferInfo{
            .name = passName + suffix,
            .size = bytes,
            .usage = rendering::BufferUsage::BufferUsage_Storage |
                rendering::BufferUsage::BufferUsage_CopySrc |
                rendering::BufferUsage::BufferUsage_Push,
            .metalExportable = plan_.metalExportable,
          });
    };
    buffers_.visibilityKeys = storageExportablePersistent(
        ".VisibilityKeys.buffer", plan_.visibilityKeyBytes);
    buffers_.resolvedPayloads = storageExportablePersistent(
        ".ResolvedPayloads.buffer", plan_.resolvedPayloadBytes);
    buffers_.uniforms =
        uniformExportable(".Uniforms.buffer", plan_.uniformBytes);
    buffers_.triangles = storageExportablePersistent(
        ".Triangles.buffer", plan_.triangleBytes);
    buffers_.tileRanges = storageExportablePersistent(
        ".TileRanges.buffer", plan_.tileRangeBytes);
    buffers_.tileTriangleIndices = storageExportablePersistent(
        ".TileTriangleIndices.buffer", plan_.tileTriangleIndexBytes);
    buffers_.overflowCounter = storageExportablePersistent(
        ".OverflowCounter.buffer", plan_.overflowCounterBytes);
    buffers_.swDrawIndirect = swDrawIndirect_;
    buffers_.swVisibleClusterInfos = swVisibleClusterInfos_;
    buffers_.cullingCounters = cullingCounters_;
    buffers_.cullingUniforms = cullingUniforms_;
    buffers_.instances = scene_.instanceBuffer;
    buffers_.pageTable = scene_.pageTableBuffer;
    buffers_.decodedClusterCull = scene_.decodedClusterCullBuffer;
    buffers_.decodedClusterGeometry = scene_.decodedClusterGeometryBuffer;
    buffers_.decodedVertices = scene_.decodedVertexBuffer;
    buffers_.decodedIndices = scene_.decodedIndexBuffer;

    uniforms_ = {};
    uniforms_.viewportWidth = settings_.viewportWidth;
    uniforms_.viewportHeight = settings_.viewportHeight;
    uniforms_.tileGridWidth = plan_.tileGridWidth;
    uniforms_.tileGridHeight = plan_.tileGridHeight;
    // The binning kernels iterate min(swVisibleClusters, triangleCount) SW
    // cluster slots, so this is the SW cluster slot bound, not a triangle
    // count (the name is historical). Leaving it zero made the guard reject
    // every cluster and the software raster produced nothing.
    uniforms_.triangleCount = settings_.maxSoftwareClusters;
    uniforms_.tileReferenceCapacity = plan_.tileReferenceCapacity;
    uniforms_.payloadMask = 0xffffffffu;
    uniforms_.tileCandidateCapacity = settings_.tileCandidateCapacity;
    renderGraph->bufferWrite(
        buffers_.uniforms, 0u, sizeof(uniforms_), &uniforms_);

    // The production native encoder consumes the decoded VG buffers and the
    // SW-visible cluster queue directly. Keep a conservative barrier in the VG
    // chain so the external Metal encoder sees completed culling and decoded
    // page publications before raster/merge.
    commandBuffer.cmdMemoryBarrier(
        rendering::PipelineStage::ALL_COMMANDS,
        rendering::PipelineStage::ALL_COMMANDS,
        rendering::AccessPattern::SHADER_WRITE |
            rendering::AccessPattern::MEMORY_WRITE,
        rendering::AccessPattern::SHADER_READ |
            rendering::AccessPattern::MEMORY_READ);
  }

  const Settings &settings() const { return settings_; }
  const BufferPlan &bufferPlan() const { return plan_; }
  const FrameBuffers &frameBuffers() const { return buffers_; }
  const Uniforms &uniforms() const { return uniforms_; }
  bool nativeMetalEncoderRequired() const
  {
    return settings_.enabled &&
        settings_.backend == Backend::NativeMetalAtomic64 &&
        settings_.registerNativeMetalEncoder;
  }
  bool routesSoftwareClusters() const
  {
    return canRouteSoftwareClusters(settings_);
  }

private:
  VirtualGeometryScene &scene_;
  rendering::Buffer swDrawIndirect_{};
  rendering::Buffer swVisibleClusterInfos_{};
  rendering::Buffer cullingCounters_{};
  rendering::Buffer cullingUniforms_{};
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_{};
  Settings settings_{};
  BufferPlan plan_{};
  FrameBuffers buffers_{};
  Uniforms uniforms_{};
};

inline VirtualGeometryComputeRasterPass::Settings
makeVirtualGeometryComputeRasterSettings(
    bool enabled,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    float projectedTriangleEdgePixelThreshold = 32.0f,
    uint32_t tileCandidateCapacity = 4096u,
    VirtualGeometryComputeRasterPass::Backend backend =
        VirtualGeometryComputeRasterPass::Backend::NativeMetalAtomic64)
{
  VirtualGeometryComputeRasterPass::Settings settings{};
  settings.enabled = enabled;
  settings.backend = enabled
      ? backend
      : VirtualGeometryComputeRasterPass::Backend::None;
  settings.viewportWidth = viewportWidth;
  settings.viewportHeight = viewportHeight;
  settings.tileCandidateCapacity = tileCandidateCapacity;
  settings.projectedTriangleEdgePixelThreshold =
      projectedTriangleEdgePixelThreshold;
  settings.metalExportable =
      backend == VirtualGeometryComputeRasterPass::Backend::NativeMetalAtomic64;
  settings.registerNativeMetalEncoder = settings.metalExportable;
  return settings;
}

} // namespace virtualgeometry::gpgpu
