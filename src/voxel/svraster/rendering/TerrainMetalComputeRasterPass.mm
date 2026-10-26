#include "voxel/svraster/rendering/TerrainMetalComputeRasterPass.hpp"

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <limits>

namespace voxel::svraster::rendering
{
namespace
{

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

id<MTLBuffer> exportedBuffer(::rendering::RHI &rhi, const ::rendering::BufferId buffer)
{
  if (buffer == ::rendering::BufferId::Invalid)
    return nil;
  return (__bridge id<MTLBuffer>)rhi.exportMetalBuffer(buffer);
}

id<MTLComputePipelineState> pipeline(
    id<MTLDevice> device,
    id<MTLLibrary> library,
    NSString *name,
    std::string *error)
{
  NSError *metalError = nil;
  id<MTLFunction> function = [library newFunctionWithName:name];
  if (function == nil)
  {
    if (error != nullptr)
      *error = "native Metal terrain compute raster missing kernel: " +
          std::string([name UTF8String]);
    return nil;
  }
  id<MTLComputePipelineState> state =
      [device newComputePipelineStateWithFunction:function error:&metalError];
  if (state == nil)
  {
    if (error != nullptr)
      *error = metalError != nil
          ? std::string([[metalError localizedDescription] UTF8String])
          : "native Metal terrain compute raster pipeline creation failed";
    return nil;
  }
  return state;
}

MTLSize groups1D(const uint32_t items, const NSUInteger threadsPerGroup)
{
  const NSUInteger groupCount =
      (static_cast<NSUInteger>(items) + threadsPerGroup - 1u) /
      threadsPerGroup;
  return MTLSizeMake(std::max<NSUInteger>(groupCount, 1u), 1u, 1u);
}

MTLSize groups2D(
    const uint32_t width,
    const uint32_t height,
    const NSUInteger x,
    const NSUInteger y)
{
  return MTLSizeMake(
      (static_cast<NSUInteger>(width) + x - 1u) / x,
      (static_cast<NSUInteger>(height) + y - 1u) / y,
      1u);
}

} // namespace

struct TerrainMetalComputeRasterPass::Impl
{
  ::rendering::RHI &rhi;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLCommandQueue> visibilityQueue = nil;
  id<MTLComputePipelineState> visibilityClear = nil;
  id<MTLComputePipelineState> visibilityRaster = nil;
  id<MTLComputePipelineState> visibilityResolve = nil;
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> clear = nil;
  id<MTLComputePipelineState> clearTiles = nil;
  id<MTLComputePipelineState> countTiles = nil;
  id<MTLComputePipelineState> scatterTiles = nil;
  id<MTLComputePipelineState> rasterize = nil;
  id<MTLComputePipelineState> countDecodedVGClusters = nil;
  id<MTLComputePipelineState> scatterDecodedVGClusters = nil;
  id<MTLComputePipelineState> rasterizeDecodedVG = nil;
  id<MTLComputePipelineState> merge = nil;
  id<MTLComputePipelineState> mergeDecodedVG = nil;
  bool supported = false;

  struct MetalBuffers
  {
    id<MTLBuffer> visibilityKeys = nil;
    id<MTLBuffer> resolvedPayloads = nil;
    id<MTLBuffer> uniformBuffer = nil;
    id<MTLBuffer> triangles = nil;
    id<MTLBuffer> tileRanges = nil;
    id<MTLBuffer> tileTriangleIndices = nil;
    id<MTLBuffer> overflowCounter = nil;
    id<MTLBuffer> cullingUniforms = nil;
    id<MTLBuffer> cullingCounters = nil;
    id<MTLBuffer> swVisibleClusterInfos = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> pageTable = nil;
    id<MTLBuffer> decodedClusterCull = nil;
    id<MTLBuffer> decodedClusterGeometry = nil;
    id<MTLBuffer> decodedVertices = nil;
    id<MTLBuffer> decodedIndices = nil;
    id<MTLBuffer> instanceSparseToDense = nil;
  };

  Impl(::rendering::RHI &inRhi, const Configuration &configuration)
      : rhi(inRhi)
  {
    device = (__bridge id<MTLDevice>)rhi.exportMetalDevice();
    queue = (__bridge id<MTLCommandQueue>)
        rhi.exportMetalCommandQueue(::rendering::Queue::Graphics);
    if (device == nil || queue == nil)
      return;

    NSError *metalError = nil;
    NSString *path =
        [NSString stringWithUTF8String:configuration.metallibPath.c_str()];
    NSURL *url = [NSURL fileURLWithPath:path];
    library = [device newLibraryWithURL:url error:&metalError];
    if (library == nil)
      return;

    std::string pipelineError;
    visibilityQueue = [device newCommandQueue];
    visibilityClear = pipeline(device, library, @"vgVisibilityClear", &pipelineError);
    visibilityRaster = pipeline(device, library, @"vgVisibilityRaster", &pipelineError);
    visibilityResolve = pipeline(device, library, @"vgVisibilityResolve", &pipelineError);

    if (configuration.visibilityOnly) return;

    clear = pipeline(
        device, library, @"terrainComputeRasterAtomic64Clear",
        &pipelineError);
    clearTiles = pipeline(
        device, library, @"terrainComputeRasterAtomic64ClearTiles",
        &pipelineError);
    countTiles = pipeline(
        device, library, @"terrainComputeRasterAtomic64CountTiles",
        &pipelineError);
    scatterTiles = pipeline(
        device, library, @"terrainComputeRasterAtomic64ScatterTiles",
        &pipelineError);
    rasterize = pipeline(
        device, library, @"terrainComputeRasterAtomic64RasterizeTiles",
        &pipelineError);
    countDecodedVGClusters = pipeline(
        device, library, @"terrainComputeRasterAtomic64CountDecodedVGClusters",
        &pipelineError);
    scatterDecodedVGClusters = pipeline(
        device, library,
        @"terrainComputeRasterAtomic64ScatterDecodedVGClusters",
        &pipelineError);
    rasterizeDecodedVG = pipeline(
        device, library, @"terrainComputeRasterAtomic64RasterizeDecodedVG",
        &pipelineError);
    merge = pipeline(
        device, library, @"terrainComputeRasterAtomic64Merge",
        &pipelineError);
    mergeDecodedVG = pipeline(
        device, library, @"terrainComputeRasterAtomic64MergeDecodedVG",
        &pipelineError);
    supported = clear != nil && clearTiles != nil && countTiles != nil &&
        scatterTiles != nil && rasterize != nil &&
        countDecodedVGClusters != nil && scatterDecodedVGClusters != nil &&
        rasterizeDecodedVG != nil && merge != nil && mergeDecodedVG != nil;
  }

  bool validateCommonFrame(
      const Frame &frame,
      const Uniforms &uniforms,
      MetalBuffers &buffers,
      std::string *error)
  {
    if (!supported)
      return fail(error, "native Metal terrain compute raster is unavailable");
    if (uniforms.viewportWidth == 0u || uniforms.viewportHeight == 0u ||
        uniforms.tileGridWidth == 0u || uniforms.tileGridHeight == 0u ||
        uniforms.tileReferenceCapacity == 0u)
      return fail(error, "native Metal terrain compute raster uniforms are invalid");

    buffers.visibilityKeys = exportedBuffer(rhi, frame.visibilityKeys);
    buffers.resolvedPayloads = exportedBuffer(rhi, frame.resolvedPayloads);
    buffers.uniformBuffer = exportedBuffer(rhi, frame.uniforms);
    buffers.tileRanges = exportedBuffer(rhi, frame.tileRanges);
    buffers.tileTriangleIndices = exportedBuffer(rhi, frame.tileTriangleIndices);
    buffers.overflowCounter = exportedBuffer(rhi, frame.overflowCounter);
    if (buffers.visibilityKeys == nil || buffers.resolvedPayloads == nil ||
        buffers.uniformBuffer == nil || buffers.tileRanges == nil ||
        buffers.tileTriangleIndices == nil || buffers.overflowCounter == nil)
    {
      return fail(error, "native Metal terrain compute raster buffers were not exported");
    }

    if ([buffers.uniformBuffer length] < sizeof(Uniforms))
      return fail(error, "native Metal terrain compute raster uniform buffer is too small");
    // The uniform buffer is owned by the renderer/RHI timeline, just like the
    // terrain cluster and tile-list buffers. Upload the same Uniforms through
    // the normal RHI path before calling this external Metal encoder; do not
    // require the exported MTLBuffer to be CPU-visible here.
    return true;
  }

  bool validateTriangleStreamFrame(
      const Frame &frame,
      const Uniforms &uniforms,
      MetalBuffers &buffers,
      std::string *error)
  {
    if (uniforms.triangleCount == 0u)
      return fail(error, "native Metal terrain compute raster has no triangles");
    if (!validateCommonFrame(frame, uniforms, buffers, error))
      return false;
    buffers.triangles = exportedBuffer(rhi, frame.triangles);
    if (buffers.triangles == nil)
      return fail(error, "native Metal terrain compute raster triangle stream was not exported");
    return true;
  }

  bool validateDecodedVGFrame(
      const Frame &frame,
      const Uniforms &uniforms,
      MetalBuffers &buffers,
      std::string *error)
  {
    if (!frame.directDecodedVGPrepared)
      return fail(error, "native Metal terrain compute raster needs decoded VG buffers and final merge");
    if (uniforms.triangleCount == 0u)
      return fail(error, "native Metal terrain compute raster has no SW cluster capacity");
    if (!validateCommonFrame(frame, uniforms, buffers, error))
      return false;

    buffers.cullingUniforms = exportedBuffer(rhi, frame.cullingUniforms);
    buffers.cullingCounters = exportedBuffer(rhi, frame.cullingCounters);
    buffers.swVisibleClusterInfos =
        exportedBuffer(rhi, frame.swVisibleClusterInfos);
    buffers.instances = exportedBuffer(rhi, frame.instances);
    buffers.pageTable = exportedBuffer(rhi, frame.pageTable);
    buffers.decodedClusterCull = exportedBuffer(rhi, frame.decodedClusterCull);
    buffers.decodedClusterGeometry =
        exportedBuffer(rhi, frame.decodedClusterGeometry);
    buffers.decodedVertices = exportedBuffer(rhi, frame.decodedVertices);
    buffers.decodedIndices = exportedBuffer(rhi, frame.decodedIndices);
    buffers.instanceSparseToDense =
        exportedBuffer(rhi, frame.instanceSparseToDense);
    if (buffers.instanceSparseToDense == nil)
      return fail(error, "native Metal terrain compute raster needs the sparse-to-dense instance table");
    if (buffers.cullingUniforms == nil || buffers.cullingCounters == nil ||
        buffers.swVisibleClusterInfos == nil || buffers.instances == nil ||
        buffers.pageTable == nil || buffers.decodedClusterCull == nil ||
        buffers.decodedClusterGeometry == nil ||
        buffers.decodedVertices == nil || buffers.decodedIndices == nil)
    {
      return fail(error, "native Metal terrain compute raster decoded VG buffers were not exported");
    }
    return true;
  }

  bool encodePreparedTiles(const Frame &frame, const Uniforms &uniforms, std::string *error)
  {
    if (!frame.tileRangesPrepared)
      return fail(error, "native Metal terrain compute raster needs direct decoded-VG tile ranges and a final depth merge");

    MetalBuffers buffers{};
    if (!validateTriangleStreamFrame(
            frame,
            uniforms,
            buffers,
            error))
      return false;

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (commandBuffer == nil)
      return fail(error, "native Metal terrain compute raster could not allocate a command buffer");
    commandBuffer.label = @"Terrain.MetalComputeRaster";
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (encoder == nil)
      return fail(error, "native Metal terrain compute raster could not allocate an encoder");
    encoder.label = @"Terrain.MetalComputeRaster.PreparedTiles";

    constexpr NSUInteger threads1D = 256u;
    const uint64_t pixelCount64 =
        static_cast<uint64_t>(uniforms.viewportWidth) *
        static_cast<uint64_t>(uniforms.viewportHeight);
    if (pixelCount64 > std::numeric_limits<uint32_t>::max())
      return fail(error, "native Metal terrain compute raster viewport is too large");
    const uint32_t pixelCount = static_cast<uint32_t>(pixelCount64);
    [encoder setComputePipelineState:clear];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:0];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:1];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:rasterize];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.triangles offset:0 atIndex:1];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:2];
    [encoder setBuffer:buffers.tileTriangleIndices offset:0 atIndex:3];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:4];
    [encoder dispatchThreadgroups:groups2D(uniforms.viewportWidth, uniforms.viewportHeight, 8u, 8u)
            threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];

    [encoder setComputePipelineState:merge];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:1];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder endEncoding];
    [commandBuffer commit];
    if (error != nullptr)
      error->clear();
    return true;
  }

  bool encodeTriangleStream(const Frame &frame, const Uniforms &uniforms, std::string *error)
  {
    MetalBuffers buffers{};
    if (!validateTriangleStreamFrame(
            frame,
            uniforms,
            buffers,
            error))
      return false;

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (commandBuffer == nil)
      return fail(error, "native Metal terrain compute raster could not allocate a command buffer");
    commandBuffer.label = @"Terrain.MetalComputeRaster";
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (encoder == nil)
      return fail(error, "native Metal terrain compute raster could not allocate an encoder");
    encoder.label = @"Terrain.MetalComputeRaster.TriangleStream";

    constexpr NSUInteger threads1D = 256u;
    const uint64_t pixelCount64 =
        static_cast<uint64_t>(uniforms.viewportWidth) *
        static_cast<uint64_t>(uniforms.viewportHeight);
    if (pixelCount64 > std::numeric_limits<uint32_t>::max())
      return fail(error, "native Metal terrain compute raster viewport is too large");
    const uint32_t pixelCount = static_cast<uint32_t>(pixelCount64);
    const uint32_t tileCount = uniforms.tileGridWidth * uniforms.tileGridHeight;

    [encoder setComputePipelineState:clear];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:0];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:1];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:clearTiles];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:1];
    [encoder setBuffer:buffers.overflowCounter offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(tileCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:countTiles];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.triangles offset:0 atIndex:1];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(uniforms.triangleCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:scatterTiles];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.triangles offset:0 atIndex:1];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:2];
    [encoder setBuffer:buffers.tileTriangleIndices offset:0 atIndex:3];
    [encoder setBuffer:buffers.overflowCounter offset:0 atIndex:4];
    [encoder dispatchThreadgroups:groups1D(uniforms.triangleCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:rasterize];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.triangles offset:0 atIndex:1];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:2];
    [encoder setBuffer:buffers.tileTriangleIndices offset:0 atIndex:3];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:4];
    [encoder dispatchThreadgroups:groups2D(uniforms.viewportWidth, uniforms.viewportHeight, 8u, 8u)
            threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];

    [encoder setComputePipelineState:merge];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:1];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder endEncoding];
    [commandBuffer commit];
    if (error != nullptr)
      error->clear();
    return true;
  }

  bool encodeDecodedVG(const Frame &frame, const Uniforms &uniforms, std::string *error)
  {
    MetalBuffers buffers{};
    if (!validateDecodedVGFrame(
            frame,
            uniforms,
            buffers,
            error))
      return false;

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (commandBuffer == nil)
      return fail(error, "native Metal terrain compute raster could not allocate a command buffer");
    commandBuffer.label = @"Terrain.MetalComputeRaster.DecodedVG";
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (encoder == nil)
      return fail(error, "native Metal terrain compute raster could not allocate an encoder");
    encoder.label = @"Terrain.MetalComputeRaster.DecodedVG";

    constexpr NSUInteger threads1D = 256u;
    const uint64_t pixelCount64 =
        static_cast<uint64_t>(uniforms.viewportWidth) *
        static_cast<uint64_t>(uniforms.viewportHeight);
    if (pixelCount64 > std::numeric_limits<uint32_t>::max())
      return fail(error, "native Metal terrain compute raster viewport is too large");
    const uint32_t pixelCount = static_cast<uint32_t>(pixelCount64);
    const uint32_t tileCount = uniforms.tileGridWidth * uniforms.tileGridHeight;

    [encoder setComputePipelineState:clear];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:0];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:1];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:clearTiles];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:1];
    [encoder setBuffer:buffers.overflowCounter offset:0 atIndex:2];
    [encoder dispatchThreadgroups:groups1D(tileCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:countDecodedVGClusters];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.cullingUniforms offset:0 atIndex:1];
    [encoder setBuffer:buffers.cullingCounters offset:0 atIndex:2];
    [encoder setBuffer:buffers.swVisibleClusterInfos offset:0 atIndex:3];
    [encoder setBuffer:buffers.instances offset:0 atIndex:4];
    [encoder setBuffer:buffers.pageTable offset:0 atIndex:5];
    [encoder setBuffer:buffers.decodedClusterCull offset:0 atIndex:6];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:7];
    [encoder dispatchThreadgroups:groups1D(uniforms.triangleCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:scatterDecodedVGClusters];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.cullingUniforms offset:0 atIndex:1];
    [encoder setBuffer:buffers.cullingCounters offset:0 atIndex:2];
    [encoder setBuffer:buffers.swVisibleClusterInfos offset:0 atIndex:3];
    [encoder setBuffer:buffers.instances offset:0 atIndex:4];
    [encoder setBuffer:buffers.pageTable offset:0 atIndex:5];
    [encoder setBuffer:buffers.decodedClusterCull offset:0 atIndex:6];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:7];
    [encoder setBuffer:buffers.tileTriangleIndices offset:0 atIndex:8];
    [encoder setBuffer:buffers.overflowCounter offset:0 atIndex:9];
    [encoder dispatchThreadgroups:groups1D(uniforms.triangleCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder setComputePipelineState:rasterizeDecodedVG];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.cullingUniforms offset:0 atIndex:1];
    [encoder setBuffer:buffers.swVisibleClusterInfos offset:0 atIndex:2];
    [encoder setBuffer:buffers.instances offset:0 atIndex:3];
    [encoder setBuffer:buffers.pageTable offset:0 atIndex:4];
    [encoder setBuffer:buffers.decodedClusterGeometry offset:0 atIndex:5];
    [encoder setBuffer:buffers.decodedVertices offset:0 atIndex:6];
    [encoder setBuffer:buffers.decodedIndices offset:0 atIndex:7];
    [encoder setBuffer:buffers.tileRanges offset:0 atIndex:8];
    [encoder setBuffer:buffers.tileTriangleIndices offset:0 atIndex:9];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:10];
    [encoder dispatchThreadgroups:groups2D(uniforms.viewportWidth, uniforms.viewportHeight, 8u, 8u)
            threadsPerThreadgroup:MTLSizeMake(8u, 8u, 1u)];

    [encoder setComputePipelineState:mergeDecodedVG];
    [encoder setBuffer:buffers.uniformBuffer offset:0 atIndex:0];
    [encoder setBuffer:buffers.visibilityKeys offset:0 atIndex:1];
    [encoder setBuffer:buffers.resolvedPayloads offset:0 atIndex:2];
    [encoder setBuffer:buffers.decodedClusterGeometry offset:0 atIndex:3];
    [encoder setBuffer:buffers.decodedVertices offset:0 atIndex:4];
    [encoder setBuffer:buffers.decodedIndices offset:0 atIndex:5];
    [encoder setBuffer:buffers.instances offset:0 atIndex:6];
    [encoder setBuffer:buffers.instanceSparseToDense offset:0 atIndex:7];
    [encoder dispatchThreadgroups:groups1D(pixelCount, threads1D)
            threadsPerThreadgroup:MTLSizeMake(threads1D, 1u, 1u)];

    [encoder endEncoding];
    [commandBuffer commit];
    if (error != nullptr)
      error->clear();
    return true;
  }
};

TerrainMetalComputeRasterPass::TerrainMetalComputeRasterPass(
    ::rendering::RHI &rhi)
    : TerrainMetalComputeRasterPass(rhi, Configuration{})
{
}

TerrainMetalComputeRasterPass::TerrainMetalComputeRasterPass(
    ::rendering::RHI &rhi,
    const Configuration &configuration)
    : impl_(std::make_unique<Impl>(rhi, configuration))
{
}

TerrainMetalComputeRasterPass::~TerrainMetalComputeRasterPass() = default;

bool TerrainMetalComputeRasterPass::available() const
{
  return impl_ != nullptr && impl_->supported;
}

bool TerrainMetalComputeRasterPass::encodePreparedTiles(
    const Frame &frame,
    const Uniforms &uniforms,
    std::string *error)
{
  if (impl_ == nullptr)
    return fail(error, "native Metal terrain compute raster has no implementation");
  return impl_->encodePreparedTiles(frame, uniforms, error);
}

bool TerrainMetalComputeRasterPass::encodeTriangleStream(
    const Frame &frame,
    const Uniforms &uniforms,
    std::string *error)
{
  if (impl_ == nullptr)
    return fail(error, "native Metal terrain compute raster has no implementation");
  return impl_->encodeTriangleStream(frame, uniforms, error);
}

bool TerrainMetalComputeRasterPass::visibilityAvailable() const
{
  return impl_ && impl_->visibilityQueue && impl_->visibilityClear &&
      impl_->visibilityRaster && impl_->visibilityResolve;
}

void TerrainMetalComputeRasterPass::encodeVisibility(const VisibilityFrame &frame,
    void *sharedEvent, uint64_t readyValue, uint64_t doneValue)
{
  @autoreleasepool {
    if (!visibilityAvailable())
      throw std::runtime_error("VG atomic64 visibility pipeline unavailable");
    id<MTLBuffer> buffers[14];
    for (size_t i=0; i<14; ++i)
    {
      buffers[i] = exportedBuffer(impl_->rhi, frame.buffers[i]);
      if (buffers[i] == nil)
        throw std::runtime_error("VG atomic64 buffer export failed at binding " + std::to_string(i));
    }
    id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)sharedEvent;
    id<MTLCommandBuffer> command = [impl_->visibilityQueue commandBuffer];
    if (command == nil) throw std::runtime_error("Cannot allocate VG native command buffer");
    command.label = @"VG.Atomic64Visibility";
    [command encodeWaitForEvent:event value:readyValue];
    const uint32_t viewport[2] = {frame.width, frame.height};
    const auto pixels = groups1D(frame.width*frame.height,256u);
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) throw std::runtime_error("Cannot allocate VG native compute encoder");
    [encoder setComputePipelineState:impl_->visibilityClear];
    [encoder setBuffer:buffers[9] offset:0 atIndex:0];
    [encoder setBytes:viewport length:sizeof(viewport) atIndex:1];
    [encoder dispatchThreadgroups:pixels threadsPerThreadgroup:MTLSizeMake(256,1,1)];
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [encoder setComputePipelineState:impl_->visibilityRaster];
    for (NSUInteger i=0; i<10; ++i)
      [encoder setBuffer:buffers[i] offset:0 atIndex:i];
    [encoder setBuffer:buffers[13] offset:0 atIndex:10];
    [encoder setBytes:viewport length:sizeof(viewport) atIndex:1];
    [encoder dispatchThreadgroupsWithIndirectBuffer:buffers[12] indirectBufferOffset:0
        threadsPerThreadgroup:MTLSizeMake(128,1,1)];
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    [encoder setComputePipelineState:impl_->visibilityResolve];
    [encoder setBuffer:buffers[9] offset:0 atIndex:0];
    [encoder setBytes:viewport length:sizeof(viewport) atIndex:1];
    [encoder setBuffer:buffers[10] offset:0 atIndex:2];
    [encoder setBuffer:buffers[11] offset:0 atIndex:3];
    [encoder dispatchThreadgroups:pixels threadsPerThreadgroup:MTLSizeMake(256,1,1)];
    [encoder endEncoding];
    [command encodeSignalEvent:event value:doneValue];
    [command commit];
  }
}

bool TerrainMetalComputeRasterPass::encodeDecodedVG(
    const Frame &frame,
    const Uniforms &uniforms,
    std::string *error)
{
  if (impl_ == nullptr)
    return fail(error, "native Metal terrain compute raster has no implementation");
  return impl_->encodeDecodedVG(frame, uniforms, error);
}

} // namespace voxel::svraster::rendering

#endif // defined(__APPLE__)
