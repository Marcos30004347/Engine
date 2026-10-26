#include "virtualgeometry/VirtualGeometryData.hpp"
#include "VirtualGeometryMetalRayTraceBackend.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace virtualgeometry::raytracing
{
namespace
{
struct KeyHash
{
  size_t operator()(const GeometryKey &k) const
  {
    size_t h = std::hash<uint64_t>{}(k.object);
    for (uint64_t v : {k.revision, uint64_t(k.cluster), uint64_t(k.format)})
      h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }
};
std::string errorText(NSError *error)
{
  return error ? std::string(error.localizedDescription.UTF8String) : "unknown Metal error";
}
void require(bool value, const char *message)
{
  if (!value) throw std::runtime_error(message);
}
id<MTLComputePipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *name)
{
  auto function = [library newFunctionWithName:name];
  require(function != nil, "Missing hardware ray trace metallib entry point");
  NSError *error = nil;
  auto result = [device newComputePipelineStateWithFunction:function error:&error];
  if (!result) throw std::runtime_error(errorText(error));
  return result;
}
struct Limits { uint32_t maxInstances, maxRays, handles, threads, lightCull, maxVolumes; };
static_assert(sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) == 72);
static_assert(offsetof(MTLIndirectAccelerationStructureInstanceDescriptor, accelerationStructureID) == 64);
}

struct VirtualGeometryMetalRayTraceBackend::Impl
{
  struct Entry
  {
    GeometryKey key;
    uint32_t handle = 0;
    id<MTLAccelerationStructure> as = nil;
    uint64_t resourceID = 0;
    MTLPrimitiveAccelerationStructureDescriptor *descriptor = nil;
    NSUInteger scratchSize = 0;
    uint64_t buildSerial = 0;
    GeometryState state = GeometryState::Pending;
    id<MTLBuffer> encodedInput = nil;
    id<MTLSharedEvent> readyEvent = nil;
    uint64_t readyValue = 0;
    struct Decode { uint32_t positionBase, indexBase, descriptorBase; float unitScale; uint32_t vertices; } decode{};

  };
  struct Slot
  {
    id<MTLAccelerationStructure> tlas = nil;
    id<MTLBuffer> instances = nil, statistics = nil, scratch = nil, blasScratch = nil;
    id<MTLBuffer> ids = nil;
    MTLIndirectInstanceAccelerationStructureDescriptor *descriptor = nil;
    id<MTLCommandBuffer> command = nil;
    id<MTLFence> fence = nil;
    std::vector<std::shared_ptr<Entry>> references;
    std::vector<id<MTLResource>> resources;
    id<MTLResidencySet> residency = nil;
    std::vector<std::shared_ptr<Entry>> buildInputs;
    uint64_t serial = 0;
    uint64_t tableVersion = 0;
    bool hasTlas = false;
  };
  Configuration configuration;
  mutable std::mutex mutex;
  Capabilities capabilities;
  CacheStatistics statistics;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLComputePipelineState> prepare = nil, prepareTrace = nil, instances = nil, finalizeInstances = nil, shadow = nil, closest = nil, decodeEncoded = nil;
  std::unordered_map<GeometryKey, std::shared_ptr<Entry>, KeyHash> cache;
  std::vector<std::shared_ptr<Entry>> handles;
  std::vector<std::shared_ptr<Entry>> pending;
  std::vector<std::weak_ptr<Entry>> retired;
  std::vector<Slot> slots;
  uint64_t serial = 0;
  uint64_t tableVersion = 1;
  // Completed serials are retained only until their slot is reused. Callers
  // must collect their completion first, just like any other frame-ring resource.

  Impl(Configuration config, void *nativeDevice) : configuration(std::move(config))
  {
    require(configuration.maxInstances > 0 && configuration.maxRays > 0 && configuration.frameSlots > 0,
            "Ray tracing capacities must be nonzero");
    require(configuration.maxInstances < UINT32_MAX - 255u && configuration.maxRays < UINT32_MAX - 255u,
            "Ray tracing capacities exceed dispatch arithmetic limits");
    device = nativeDevice ? (__bridge id<MTLDevice>)nativeDevice : MTLCreateSystemDefaultDevice();
    if (!device) { capabilities.reason = "No Metal device"; return; }
    capabilities.device = device.name.UTF8String;
    if (@available(macOS 14.0, *))
    {
      capabilities.appleFamily9OrNewer = [device supportsFamily:MTLGPUFamilyApple9];
      if (!device.supportsRaytracing) { capabilities.reason = "Metal ray tracing unavailable"; return; }
    }
    else { capabilities.reason = "Indirect TLAS requires macOS 14 or newer"; return; }
    NSError *error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:
        [NSString stringWithUTF8String:configuration.metallibPath.c_str()]] error:&error];
    if (!library) throw std::runtime_error("Hardware ray trace metallib: " + errorText(error));
    prepare = pipeline(device, library, @"vgRtPrepare");
    prepareTrace = pipeline(device, library, @"vgRtPrepareTrace");
    instances = pipeline(device, library, @"vgRtInstances");
    finalizeInstances = pipeline(device, library, @"vgRtFinalizeInstances");
    shadow = pipeline(device, library, @"vgRtShadow");
    closest = pipeline(device, library, @"vgRtClosest");
    decodeEncoded = pipeline(device, library, @"vgRtDecodeEncoded");
    queue = [device newCommandQueue];
    require(queue != nil, "Cannot allocate ray tracing queue");
    capabilities.available = true;

    // A degenerate triangle is a valid, non-intersectable BLAS. Always use a
    // valid masked reference for empty scenes and invalid handles.
    const float dummy[9] = {};
    auto vertices = [device newBufferWithBytes:dummy length:sizeof(dummy) options:MTLResourceStorageModeShared];
    Geometry g;
    g.vertexBuffer = (__bridge void *)vertices;
    g.vertexStride = 12; g.vertexCount = 3; g.triangleCount = 1;
    auto sentinel = createEntry(g, 0);
    handles.push_back(sentinel);
    pending.push_back(sentinel);
    slots.resize(configuration.frameSlots);
    for (auto &slot : slots)
    {
      if (@available(macOS 15.0, *)) {
        if (configuration.useResidencySets && [device supportsFamily:MTLGPUFamilyApple6]) {
          auto residencyDescriptor = [MTLResidencySetDescriptor new];
          residencyDescriptor.label = @"VG.ClusterBLASResidency";
          residencyDescriptor.initialCapacity = 1024;
          slot.residency = [device newResidencySetWithDescriptor:residencyDescriptor error:&error];
          require(slot.residency != nil, "Cannot allocate BLAS residency set");
          capabilities.residencySets = true;
        }
      }
      slot.instances = buffer(uint64_t(configuration.maxInstances) * 72, MTLResourceStorageModePrivate);
      slot.statistics = buffer(sizeof(RayTraceFrameStatistics), MTLResourceStorageModeShared);
      std::memset(slot.statistics.contents, 0, sizeof(RayTraceFrameStatistics));
      slot.descriptor = [MTLIndirectInstanceAccelerationStructureDescriptor descriptor];
      // The scene cut changes every frame; minimize TLAS construction latency.
      // Immutable cluster BLASes keep the default traversal-oriented build.
      slot.descriptor.usage = MTLAccelerationStructureUsagePreferFastBuild;
      slot.descriptor.instanceDescriptorBuffer = slot.instances;
      slot.descriptor.instanceDescriptorStride = 72;
      slot.descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
      slot.descriptor.instanceCountBuffer = slot.statistics;
      slot.descriptor.maxInstanceCount = configuration.maxInstances;
      const auto sizes = [device accelerationStructureSizesWithDescriptor:slot.descriptor];
      slot.tlas = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
      require(slot.tlas != nil, "Cannot allocate TLAS");
      slot.scratch = buffer(std::max<NSUInteger>(sizes.buildScratchBufferSize, 256), MTLResourceStorageModePrivate);
      slot.fence = [device newFence];
      require(slot.fence != nil, "Cannot allocate AS fence");
      statistics.tlasBytes += slot.tlas.size;
    }
  }

  id<MTLBuffer> buffer(uint64_t bytes, MTLResourceOptions options)
  {
    require(bytes <= device.maxBufferLength, "Ray tracing buffer exceeds device limit");
    auto result = [device newBufferWithLength:std::max<uint64_t>(bytes, 4) options:options];
    require(result != nil, "Cannot allocate ray tracing buffer");
    return result;
  }

  std::shared_ptr<Entry> createEntry(const Geometry &geometry, uint32_t handle)
  {
    id<MTLBuffer> vertices = (__bridge id<MTLBuffer>)geometry.vertexBuffer;
    id<MTLBuffer> indices = (__bridge id<MTLBuffer>)geometry.indexBuffer;
    require(vertices && vertices.device == device, "Invalid BLAS vertex buffer/device");
    require(geometry.vertexCount > 0 && geometry.triangleCount > 0 && geometry.vertexStride >= 12 &&
            geometry.vertexStride % 4 == 0 && geometry.vertexOffset % 4 == 0, "Invalid BLAS geometry range");
    const uint64_t end = uint64_t(geometry.vertexCount - 1) * geometry.vertexStride + 12;
    require(geometry.vertexOffset <= vertices.length && end <= vertices.length - geometry.vertexOffset,
            "BLAS vertex range exceeds buffer");
    const uint64_t indexBytes = uint64_t(geometry.triangleCount) * 3 * (geometry.index32 ? 4 : 2);
    if (indices)
    {
      require(indices.device == device && geometry.indexOffset % (geometry.index32 ? 4 : 2) == 0 &&
              geometry.indexOffset <= indices.length && indexBytes <= indices.length - geometry.indexOffset,
              "BLAS index range exceeds buffer or is misaligned");
    }
    else require(uint64_t(geometry.triangleCount) * 3 <= geometry.vertexCount, "Non-indexed BLAS has insufficient vertices");
    auto triangles = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    triangles.vertexBuffer = vertices; triangles.vertexBufferOffset = geometry.vertexOffset;
    triangles.vertexStride = geometry.vertexStride; triangles.vertexFormat = MTLAttributeFormatFloat3;
    triangles.indexBuffer = indices; triangles.indexBufferOffset = geometry.indexOffset;
    triangles.indexType = geometry.index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    triangles.triangleCount = geometry.triangleCount; triangles.opaque = YES;
    auto descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    descriptor.geometryDescriptors = @[triangles];
    const auto sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
    collect();
    if (sizes.accelerationStructureSize > configuration.blasBudgetBytes ||
        statistics.residentBytes + statistics.retiredBytes > configuration.blasBudgetBytes - sizes.accelerationStructureSize)
      throw BudgetExceeded("BLAS memory budget exhausted; retain the existing scene cut");
    auto entry = std::make_shared<Entry>();
    entry->key = geometry.key; entry->handle = handle; entry->descriptor = descriptor;
    entry->readyEvent=(__bridge id<MTLSharedEvent>)geometry.readyEvent;
    entry->readyValue=geometry.readyValue;
    require(!entry->readyEvent || ((!entry->readyEvent.device || entry->readyEvent.device==device) && entry->readyValue>0),
        "Invalid BLAS producer event");
    entry->scratchSize = sizes.buildScratchBufferSize;
    entry->as = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    require(entry->as != nil, "Cannot allocate cluster BLAS");
    entry->resourceID = entry->as.gpuResourceID._impl;
    statistics.residentBytes += entry->as.allocatedSize; statistics.requestedBytes += entry->as.size;
    return entry;
  }

  void collect()
  {
    statistics.retiredBytes = 0;
    retired.erase(std::remove_if(retired.begin(), retired.end(), [&](auto &weak) {
      if (auto entry = weak.lock()) { statistics.retiredBytes += entry->as.allocatedSize; return false; }
      return true;
    }), retired.end());
    // Descriptor retains vertex/index build inputs. Release them only after
    // their producing command has finished; the BLAS itself owns its triangles.
    for (auto &slot : slots)
      if (slot.command && (slot.command.status == MTLCommandBufferStatusCompleted || slot.command.status == MTLCommandBufferStatusError))
      {
        for (auto &entry : slot.buildInputs) {
          entry->state=slot.command.status == MTLCommandBufferStatusCompleted ? GeometryState::Ready : GeometryState::Failed;
          entry->descriptor = nil; entry->encodedInput=nil; entry->readyEvent=nil;
        }
        slot.buildInputs.clear();
      }
  }

  id<MTLBuffer> input(void *native, uint64_t minimum, const char *message)
  {
    auto b = (__bridge id<MTLBuffer>)native;
    require(b && b.device == device && b.length >= minimum, message);
    return b;
  }

  uint64_t submit(const Frame &frame)
  {
    require(capabilities.available, "Metal ray tracing is unavailable");
    for (const auto &existing : slots)
      require(!existing.command || existing.command.status != MTLCommandBufferStatusError,
              "An earlier ray tracing submission failed; recreate backend");
    require(frame.slot < slots.size(), "Invalid ray tracing frame slot");
    auto &slot = slots[frame.slot];
    require(!slot.command || slot.command.status == MTLCommandBufferStatusCompleted ||
            slot.command.status == MTLCommandBufferStatusError, "Ray tracing frame slot is still in flight");
    require(!slot.command || slot.command.status != MTLCommandBufferStatusError,
            "Previous ray tracing submission failed; recreate backend");
    collect();
    const bool traceOnly = frame.work == Frame::Work::TraceOnly;
    require(!traceOnly || slot.hasTlas, "Trace-only workload requires a built TLAS in the frame slot");
    require(!traceOnly || static_cast<RayTraceFrameStatistics *>(slot.statistics.contents)->invalidScene == 0,
            "Trace-only workload requires a valid completed scene");
    const bool buildOnly=frame.work == Frame::Work::BuildOnly;
    auto inputInstances = buildOnly ? nil : input(frame.instances, uint64_t(configuration.maxInstances) * sizeof(RayTraceInstance), "Invalid instance input buffer");
    auto instanceCount = buildOnly ? nil : input(frame.instanceCount, 4, "Invalid instance count buffer");
    auto inputRays = buildOnly ? nil : input(frame.rays, uint64_t(configuration.maxRays) * sizeof(ShadowRay), "Invalid ray input buffer");
    auto rayCount = buildOnly ? nil : input(frame.rayCount, 4, "Invalid ray count buffer");
    auto output = buildOnly ? nil : input(frame.results, uint64_t(configuration.maxRays) * (frame.query == Frame::Query::ClosestHit ? sizeof(ClosestHitResult) : sizeof(uint32_t)), "Invalid shadow output buffer");
    const bool lightCull = frame.casterBounds || frame.lightVolumes || frame.lightVolumeCount;
    auto bounds = lightCull ? input(frame.casterBounds, uint64_t(configuration.maxInstances) * sizeof(RayTraceBounds), "Invalid caster bounds buffer") : slot.statistics;
    auto volumes = lightCull ? input(frame.lightVolumes, uint64_t(configuration.maxLightVolumes) * sizeof(RayTraceBounds), "Invalid light volume buffer") : slot.statistics;
    auto volumeCount = lightCull ? input(frame.lightVolumeCount, 4, "Invalid light volume count buffer") : slot.statistics;
    auto command = [queue commandBuffer];
    require(command != nil, "Cannot allocate ray trace command buffer");
    command.label = @"VG.HardwareRayTrace";
    if (frame.sharedEvent)
    {
      require(frame.doneValue > frame.readyValue, "Invalid ray tracing shared-event values");
      [command encodeWaitForEvent:(__bridge id<MTLSharedEvent>)frame.sharedEvent value:frame.readyValue];
    }

    std::vector<std::shared_ptr<Entry>> builds;
    uint64_t buildBytes=0; uint32_t buildCount=0;
    if (!traceOnly) {
      for (auto &entry : pending) {
        if (!handles[entry->handle]) continue;
        // Even an empty install-managed scene needs the internal masked BLAS.
        if (frame.work == Frame::Work::TlasAndTrace && entry->handle != 0) continue;
        if (entry->handle!=0) {
          if (buildCount>=configuration.maxBlasBuildsPerFrame ||
              entry->as.size>configuration.maxBlasBuildBytesPerFrame-buildBytes) break;
          ++buildCount; buildBytes+=entry->as.size;
        }
        builds.push_back(entry);
      }
      if (!builds.empty()) ++tableVersion;
    }
    if (buildOnly) {
      if (@available(macOS 15.0, *)) {
        [slot.residency removeAllAllocations]; [slot.residency commit];
      }
      slot.references.clear(); slot.resources.clear(); slot.tableVersion=0;
    }
    if (!traceOnly && !buildOnly && slot.tableVersion != tableVersion)
    {
      std::vector<bool> admitted(handles.size(),false);
      for (const auto &entry : builds) admitted[entry->handle]=true;
      slot.references.clear();
      slot.resources.clear();
      std::vector<uint64_t> ids(handles.size(), 0);
      for (auto &entry : handles)
        if (entry && (entry->buildSerial!=0 || admitted[entry->handle]))
        {
          slot.references.push_back(entry);
          slot.resources.push_back(entry->as);
          ids[entry->handle] = entry->resourceID;
        }
      // Snapshot on cache changes; a frame never sees an in-flight table mutation.
      if (!slot.ids || slot.ids.length < ids.size() * sizeof(uint64_t))
        slot.ids = buffer(std::max<uint64_t>(ids.size() * sizeof(uint64_t),
            slot.ids ? slot.ids.length * 2 : 256), MTLResourceStorageModeShared);
      std::memcpy(slot.ids.contents, ids.data(), ids.size() * sizeof(uint64_t));
      if (@available(macOS 15.0, *)) {
        if (slot.residency) {
          // Mutate only the completed frame slot. Its set and strong AS
          // snapshot stay unchanged for the entire next GPU submission.
          [slot.residency removeAllAllocations];
          for (auto resource : slot.resources) [slot.residency addAllocation:resource];
          [slot.residency commit];
        }
      }
      slot.tableVersion = tableVersion;
    }
    if (@available(macOS 15.0, *))
      if (!buildOnly && slot.residency) [command useResidencySet:slot.residency];
    const uint64_t nextSerial = serial + 1;
    if (!builds.empty())
    {
      for (const auto &entry : builds)
        if (entry->readyEvent) [command encodeWaitForEvent:entry->readyEvent value:entry->readyValue];
      uint64_t scratchBytes = 0;
      for (auto &entry : builds) scratchBytes += (entry->scratchSize + 255u) & ~uint64_t(255u);
      if (!slot.blasScratch || slot.blasScratch.length < scratchBytes)
        slot.blasScratch = buffer(scratchBytes, MTLResourceStorageModePrivate);
      bool decoded=false;
      id<MTLComputeCommandEncoder> decoder=nil;
      for (const auto &entry : builds) {
        if (!entry->encodedInput) continue;
        if (!decoder) { decoder=[command computeCommandEncoder]; [decoder setComputePipelineState:decodeEncoded]; }
        auto triangles=(MTLAccelerationStructureTriangleGeometryDescriptor *)entry->descriptor.geometryDescriptors[0];
        [decoder setBuffer:entry->encodedInput offset:0 atIndex:0];
        [decoder setBuffer:triangles.vertexBuffer offset:0 atIndex:1];
        [decoder setBytes:&entry->decode length:sizeof(entry->decode) atIndex:2];
        [decoder dispatchThreadgroups:MTLSizeMake((entry->decode.vertices+63)/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
        decoded=true;
      }
      if (decoder) { [decoder updateFence:slot.fence]; [decoder endEncoding]; }
      auto encoder = [command accelerationStructureCommandEncoder];
      encoder.label = @"VG.ClusterBLASBuild";
      if (decoded) [encoder waitForFence:slot.fence];
      NSUInteger offset = 0;
      for (auto &entry : builds)
      {
        [encoder buildAccelerationStructure:entry->as descriptor:entry->descriptor
            scratchBuffer:slot.blasScratch scratchBufferOffset:offset];
        offset += (entry->scratchSize + 255u) & ~NSUInteger(255u);
      }
      [encoder updateFence:slot.fence];
      [encoder endEncoding];
    }

    const Limits limits{configuration.maxInstances, configuration.maxRays, uint32_t(handles.size()), 64,
        uint32_t(lightCull), configuration.maxLightVolumes};
    if (frame.work != Frame::Work::BuildOnly)
    {
      auto encoder = [command computeCommandEncoder];
      encoder.label = @"VG.RayTraceInstancePrepare";
      if (!builds.empty()) [encoder waitForFence:slot.fence];
      [encoder setComputePipelineState:traceOnly ? prepareTrace : prepare];
      [encoder setBuffer:instanceCount offset:0 atIndex:0];
      [encoder setBuffer:rayCount offset:0 atIndex:1];
      [encoder setBuffer:slot.statistics offset:0 atIndex:2];
      [encoder setBytes:&limits length:sizeof(limits) atIndex:3];
      [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
      if (!traceOnly)
      {
        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        [encoder setComputePipelineState:instances];
        [encoder setBuffer:inputInstances offset:0 atIndex:0];
        [encoder setBuffer:slot.ids offset:0 atIndex:1];
        [encoder setBuffer:slot.instances offset:0 atIndex:2];
        [encoder setBuffer:slot.statistics offset:0 atIndex:3];
        [encoder setBytes:&limits length:sizeof(limits) atIndex:4];
        [encoder setBuffer:bounds offset:0 atIndex:5];
        [encoder setBuffer:volumes offset:0 atIndex:6];
        [encoder setBuffer:volumeCount offset:0 atIndex:7];
        [encoder dispatchThreadgroupsWithIndirectBuffer:slot.statistics indirectBufferOffset:16
            threadsPerThreadgroup:MTLSizeMake(limits.threads,1,1)];
        if (lightCull)
        {
          [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
          [encoder setComputePipelineState:finalizeInstances];
          [encoder setBuffer:slot.instances offset:0 atIndex:0];
          [encoder setBuffer:slot.statistics offset:0 atIndex:1];
          [encoder setBuffer:slot.ids offset:0 atIndex:2];
          [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        }
      }
      [encoder updateFence:slot.fence];
      [encoder endEncoding];
      if (!traceOnly)
      {
        auto build = [command accelerationStructureCommandEncoder];
        build.label = @"VG.IndirectTLASBuild";
        [build waitForFence:slot.fence];
        if (!slot.residency && !slot.resources.empty())
          [build useResources:slot.resources.data() count:slot.resources.size() usage:MTLResourceUsageRead];
        [build buildAccelerationStructure:slot.tlas descriptor:slot.descriptor
            scratchBuffer:slot.scratch scratchBufferOffset:0];
        [build updateFence:slot.fence];
        [build endEncoding];
      }
      if (frame.work != Frame::Work::BuildAndTlas)
      {
        auto trace = [command computeCommandEncoder];
        trace.label = @"VG.OpaqueShadowTrace";
        [trace waitForFence:slot.fence];
        if (!slot.residency && !slot.resources.empty())
          [trace useResources:slot.resources.data() count:slot.resources.size() usage:MTLResourceUsageRead];
        [trace setComputePipelineState:frame.query == Frame::Query::ClosestHit ? closest : shadow];
        [trace setBuffer:inputRays offset:0 atIndex:0];
        [trace setBuffer:output offset:0 atIndex:1];
        [trace setBuffer:slot.statistics offset:0 atIndex:2];
        [trace setAccelerationStructure:slot.tlas atBufferIndex:3];
        [trace dispatchThreadgroupsWithIndirectBuffer:slot.statistics indirectBufferOffset:28
            threadsPerThreadgroup:MTLSizeMake(limits.threads,1,1)];
        [trace endEncoding];
      }
    }
    if (frame.sharedEvent)
      [command encodeSignalEvent:(__bridge id<MTLSharedEvent>)frame.sharedEvent value:frame.doneValue];
    [command commit];
    serial = nextSerial; slot.serial = serial; slot.command = command;
    if (!traceOnly) slot.hasTlas = frame.work != Frame::Work::BuildOnly;
    for (auto &entry : builds)
    {
      entry->buildSerial = serial;
      if (entry->handle != 0) ++statistics.builds;
    }
    if (!traceOnly) {
      slot.buildInputs = std::move(builds);
      pending.erase(std::remove_if(pending.begin(),pending.end(),[&](const auto &entry) {
        return !handles[entry->handle] || entry->buildSerial!=0;
      }),pending.end());
    }
    return serial;
  }

  Completion completion(uint64_t serial, bool wait)
  {
    for (auto &slot : slots)
      if (slot.serial == serial && slot.command)
      {
        if (wait) [slot.command waitUntilCompleted];
        Completion result;
        const auto status = slot.command.status;
        result.completed = status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
        if (!result.completed) return result;
        result.succeeded = status == MTLCommandBufferStatusCompleted;
        if (!result.succeeded) result.error = errorText(slot.command.error);
        result.gpuMilliseconds = (slot.command.GPUEndTime - slot.command.GPUStartTime) * 1000;
        std::memcpy(&result.statistics, slot.statistics.contents, sizeof(result.statistics));
        collect();
        return result;
      }
    throw std::runtime_error("Unknown or reused ray tracing completion serial");
  }
};

VirtualGeometryMetalRayTraceBackend::VirtualGeometryMetalRayTraceBackend(Configuration config, void *device)
{
  @autoreleasepool { impl_ = std::make_unique<Impl>(std::move(config), device); }
}
VirtualGeometryMetalRayTraceBackend::~VirtualGeometryMetalRayTraceBackend() = default;
const VirtualGeometryMetalRayTraceBackend::Capabilities &VirtualGeometryMetalRayTraceBackend::capabilities() const { return impl_->capabilities; }
const VirtualGeometryMetalRayTraceBackend::Configuration &VirtualGeometryMetalRayTraceBackend::configuration() const { return impl_->configuration; }
uint32_t VirtualGeometryMetalRayTraceBackend::cacheGeometry(const Geometry &geometry)
{
  @autoreleasepool {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  require(impl_->capabilities.available, "Metal ray tracing unavailable");
  auto found = impl_->cache.find(geometry.key);
  if (found != impl_->cache.end()) { ++impl_->statistics.hits; return found->second->handle; }
  require(impl_->handles.size() < 4u * 1024u * 1024u, "BLAS handle table lifetime limit reached");
  const uint32_t handle = uint32_t(impl_->handles.size());
  auto entry = impl_->createEntry(geometry, handle);
  impl_->cache.emplace(geometry.key, entry); impl_->handles.push_back(entry);
  impl_->pending.push_back(entry); ++impl_->tableVersion;
  ++impl_->statistics.misses;
  return handle;

  }
}
void VirtualGeometryMetalRayTraceBackend::evictGeometry(const GeometryKey &key)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->cache.find(key);
  if (it == impl_->cache.end()) return;
  const auto entry = it->second;
  impl_->statistics.residentBytes -= entry->as.allocatedSize; impl_->statistics.requestedBytes -= entry->as.size;
  impl_->retired.emplace_back(entry);
  impl_->handles[entry->handle].reset(); impl_->cache.erase(it);
  impl_->pending.erase(std::remove_if(impl_->pending.begin(),impl_->pending.end(),
      [&](const auto &pending) { return pending->handle==entry->handle; }),impl_->pending.end());
  ++impl_->tableVersion;
  ++impl_->statistics.evictions;
}
bool VirtualGeometryMetalRayTraceBackend::tryEvictGeometryAfterInputReads(const GeometryKey &key)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->collect();
  auto it = impl_->cache.find(key);
  if (it == impl_->cache.end()) return true;
  if (it->second->buildSerial && it->second->state==GeometryState::Pending) return false;
  const auto entry = it->second;
  impl_->statistics.residentBytes -= entry->as.allocatedSize; impl_->statistics.requestedBytes -= entry->as.size;
  impl_->retired.emplace_back(entry);
  impl_->handles[entry->handle].reset(); impl_->cache.erase(it);
  impl_->pending.erase(std::remove_if(impl_->pending.begin(),impl_->pending.end(),
      [&](const auto &pending) { return pending->handle==entry->handle; }),impl_->pending.end());
  ++impl_->tableVersion;
  ++impl_->statistics.evictions;
  return true;
}
VirtualGeometryMetalRayTraceBackend::GeometryState VirtualGeometryMetalRayTraceBackend::geometryState(uint32_t handle)
{
  std::lock_guard<std::mutex> lock(impl_->mutex); impl_->collect();
  if (handle==0 || handle>=impl_->handles.size() || !impl_->handles[handle]) return GeometryState::Missing;
  return impl_->handles[handle]->state;
}

VirtualGeometryMetalRayTraceBackend::GeometryState VirtualGeometryMetalRayTraceBackend::geometryStates(const std::vector<uint32_t> &handles)
{
  // A page is queried as one transaction. Collect completed inputs/retired
  // resources once, rather than repeating that work for every cluster.
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->collect();
  auto result = GeometryState::Ready;
  for (auto handle : handles)
  {
    if (!handle || handle >= impl_->handles.size() || !impl_->handles[handle]) return GeometryState::Missing;
    const auto state = impl_->handles[handle]->state;
    if (state == GeometryState::Failed) return state;
    if (state != GeometryState::Ready) result = GeometryState::Pending;
  }
  return result;
}

bool VirtualGeometryMetalRayTraceBackend::geometryInputsInFlight(uint32_t handle)
{
  std::lock_guard<std::mutex> lock(impl_->mutex); impl_->collect();
  return handle<impl_->handles.size() && impl_->handles[handle] &&
      impl_->handles[handle]->buildSerial!=0 && impl_->handles[handle]->state==GeometryState::Pending;
}
void VirtualGeometryMetalRayTraceBackend::drainGeometryInputs(uint32_t handle)
{
  std::lock_guard<std::mutex> lock(impl_->mutex); impl_->collect();
  if (handle<impl_->handles.size() && impl_->handles[handle] &&
      impl_->handles[handle]->buildSerial && impl_->handles[handle]->state==GeometryState::Pending)
    impl_->completion(impl_->handles[handle]->buildSerial,true);
  impl_->collect();
}

std::vector<uint32_t> VirtualGeometryMetalRayTraceBackend::cacheEncodedPage(uint64_t object,uint64_t revision,
    const uint32_t *words,uint64_t wordCount,uint32_t headerWords,float unitScale)
{
  @autoreleasepool {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  require(impl_->capabilities.available,"Metal ray tracing is unavailable");
  require(words && headerWords>=8 && wordCount>=headerWords && words[0]>0 && words[0]<=MAX_GROUPS_PER_PAGE * GroupSize,
      "Invalid encoded BLAS page header");
  require(std::isfinite(unitScale) && unitScale>0,"Invalid encoded BLAS unit scale");
  const uint64_t dataBase=uint64_t(headerWords)+uint64_t(words[0])*34;
  const uint64_t indexBase=dataBase+words[1]+words[2]+words[3];
  require(indexBase+words[4]+words[5]<=wordCount && wordCount<=UINT32_MAX,"Truncated encoded BLAS page");
  auto bits=[](uint32_t value) { uint32_t n=0; while(value) { ++n; value>>=1; } return n; };
  auto number=[](uint32_t word) { float value; std::memcpy(&value,&word,4); return value; };
  // Validate every descriptor and index before caching any cluster.
  for (uint32_t cluster=0;cluster<words[0];++cluster) {
    const auto *d=words+headerWords+cluster*34;
    const uint64_t vertexBits=uint64_t(d[8])*(bits(d[10])+bits(d[11])+bits(d[12]));
    require(d[8]>0 && d[8]<=256 && d[9]>0 && d[9]<=128 && d[13]<=31 && d[32]==0,
        "Encoded BLAS requires static triangle clusters with valid counts");
    require(uint64_t(d[0])+d[1]<=words[1] && vertexBits<=uint64_t(d[1])*32 &&
        uint64_t(d[6])+d[7]<=words[4] && (uint64_t(d[9])*3+3)/4<=d[7],"Encoded BLAS stream exceeds page bounds");
    const float scale=float(uint32_t(1)<<d[13])*unitScale;
    require(std::isfinite(scale) && scale>0,"Encoded BLAS quantization overflows");
    for (uint32_t axis=0;axis<3;++axis)
      require(std::isfinite(number(d[14+axis])) && std::isfinite((number(d[14+axis])+float(d[10+axis]))/scale),
          "Encoded BLAS position is non-finite");
    for (uint32_t i=0;i<d[9]*3;++i)
      require(((words[indexBase+d[6]+i/4]>>((i%4)*8))&255)<d[8],"Encoded BLAS index exceeds cluster vertices");
  }
  impl_->collect();
  const size_t originalHandleCount=impl_->handles.size();
  std::vector<uint32_t> result, created;
  uint32_t missing=0;
  for (uint32_t cluster=0;cluster<words[0];++cluster)
    missing += impl_->cache.find(GeometryKey{object,revision,cluster,1})==impl_->cache.end();
  // A page larger than the cap still goes through once nothing is waiting.
  if (missing && !impl_->pending.empty() &&
      impl_->pending.size()+missing>impl_->configuration.maxPendingBlasBuilds)
    throw QueueFull("BLAS build queue full; retry after the pending builds run");
  try {
  id<MTLBuffer> source=nil;
  for (uint32_t cluster=0;cluster<words[0];++cluster) {
    GeometryKey key{object,revision,cluster,1};
    const auto found=impl_->cache.find(key);
    if (found!=impl_->cache.end()) { result.push_back(found->second->handle); ++impl_->statistics.hits; continue; }
    if (!source) { source=impl_->buffer(wordCount*4,MTLResourceStorageModeShared); std::memcpy(source.contents,words,wordCount*4); }
    const auto *d=words+headerWords+cluster*34;
    auto vertices=impl_->buffer(uint64_t(d[9])*3*12,MTLResourceStorageModePrivate);
    Geometry geometry; geometry.key=key; geometry.vertexBuffer=(__bridge void *)vertices;
    geometry.vertexCount=d[9]*3; geometry.triangleCount=d[9]; geometry.vertexStride=12;
    require(impl_->handles.size()<UINT32_MAX,"BLAS handle table exhausted");
    const auto handle=uint32_t(impl_->handles.size());
    auto entry=impl_->createEntry(geometry,handle);
    entry->encodedInput=source;
    entry->decode={uint32_t(dataBase),uint32_t(indexBase),headerWords+cluster*34,unitScale,d[9]*3};
    impl_->handles.push_back(entry); impl_->cache.emplace(key,entry); impl_->pending.push_back(entry);
    ++impl_->tableVersion; ++impl_->statistics.misses; result.push_back(handle); created.push_back(handle);
  }
  return result;
  } catch (...) {
    // A page is a transaction: budget failure cannot strand half a page in
    // the cache and make every subsequent retry fail for lack of memory.
    for (auto handle:created) {
      auto entry=impl_->handles[handle];
      impl_->statistics.residentBytes-=entry->as.allocatedSize; impl_->statistics.requestedBytes-=entry->as.size;
      impl_->cache.erase(entry->key); impl_->handles[handle].reset();
    }
    impl_->pending.erase(std::remove_if(impl_->pending.begin(),impl_->pending.end(),
        [&](const auto &entry) { return !impl_->handles[entry->handle]; }),impl_->pending.end());
    impl_->handles.resize(originalHandleCount);
    ++impl_->tableVersion;
    throw;
  }

  }
}

uint32_t VirtualGeometryMetalRayTraceBackend::cacheEncodedPageCut(uint64_t object,uint64_t revision,
    const uint32_t *words,uint64_t wordCount,uint32_t headerWords,float unitScale,float cutError)
{
  @autoreleasepool {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  require(impl_->capabilities.available,"Metal ray tracing is unavailable");
  require(words && headerWords>=8 && wordCount>=headerWords && words[0]>0 && words[0]<=MAX_GROUPS_PER_PAGE * GroupSize,
      "Invalid encoded BLAS page header");
  require(std::isfinite(unitScale) && unitScale>0 && std::isfinite(cutError) && cutError>=0,
      "Invalid encoded page cut scale/error");
  const GeometryKey key{object,revision,kPageCutCluster,2};
  if (const auto found=impl_->cache.find(key); found!=impl_->cache.end()) {
    ++impl_->statistics.hits; return found->second->handle;
  }
  impl_->collect();
  if (!impl_->pending.empty() && impl_->pending.size()+1>impl_->configuration.maxPendingBlasBuilds)
    throw QueueFull("BLAS build queue full; retry after the pending builds run");
  const uint64_t dataBase=uint64_t(headerWords)+uint64_t(words[0])*34;
  const uint64_t indexBase=dataBase+words[1]+words[2]+words[3];
  require(indexBase+words[4]+words[5]<=wordCount && wordCount<=UINT32_MAX,"Truncated encoded BLAS page");
  auto bits=[](uint32_t value) { uint32_t n=0; while(value) { ++n; value>>=1; } return n; };
  auto number=[](uint32_t word) { float value; std::memcpy(&value,&word,4); return value; };
  // The cut: self error within it and parent error above it. A page of a
  // multi-page asset may hold none of it (the cut is in its children).
  const float error=cutError;
  std::vector<float> positions; std::vector<uint32_t> indices;
  for (uint32_t cluster=0;cluster<words[0];++cluster) {
    const auto *d=words+headerWords+cluster*34;
    if (!(number(d[21])<=error && number(d[26])>error)) continue;
    const uint32_t bx=bits(d[10]),by=bits(d[11]),bz=bits(d[12]);
    const uint64_t vertexBits=uint64_t(d[8])*(bx+by+bz);
    require(d[8]>0 && d[8]<=256 && d[9]>0 && d[9]<=128 && d[13]<=31 && d[32]==0,
        "Encoded BLAS requires static triangle clusters with valid counts");
    require(uint64_t(d[0])+d[1]<=words[1] && vertexBits<=uint64_t(d[1])*32 &&
        uint64_t(d[6])+d[7]<=words[4] && (uint64_t(d[9])*3+3)/4<=d[7],"Encoded BLAS stream exceeds page bounds");
    const float scale=float(uint32_t(1)<<d[13])*unitScale;
    const uint32_t first=uint32_t(positions.size()/3);
    uint64_t bit=uint64_t(dataBase+d[0])*32;
    const auto read=[&](uint32_t count) {
      uint32_t value=0;
      for (uint32_t k=0;k<count;++k,++bit) value|=((words[bit>>5]>>(bit&31))&1u)<<k;
      return value;
    };
    for (uint32_t v=0;v<d[8];++v) {
      const uint32_t q[3]={read(bx),read(by),read(bz)};
      for (uint32_t axis=0;axis<3;++axis) {
        const float p=(number(d[14+axis])+float(q[axis]))/scale;
        require(std::isfinite(p),"Encoded BLAS position is non-finite");
        positions.push_back(p);
      }
    }
    for (uint32_t i=0;i<d[9]*3;++i) {
      const uint32_t index=(words[indexBase+d[6]+i/4]>>((i%4)*8))&255;
      require(index<d[8],"Encoded BLAS index exceeds cluster vertices");
      indices.push_back(first+index);
    }
  }
  if (indices.empty()) throw EmptyCut("encoded page holds none of the cut");
  auto vertexBuffer=impl_->buffer(positions.size()*4,MTLResourceStorageModeShared);
  std::memcpy(vertexBuffer.contents,positions.data(),positions.size()*4);
  auto indexBuffer=impl_->buffer(indices.size()*4,MTLResourceStorageModeShared);
  std::memcpy(indexBuffer.contents,indices.data(),indices.size()*4);
  Geometry geometry; geometry.key=key;
  geometry.vertexBuffer=(__bridge void *)vertexBuffer; geometry.indexBuffer=(__bridge void *)indexBuffer;
  geometry.vertexStride=12; geometry.vertexCount=uint32_t(positions.size()/3);
  geometry.triangleCount=uint32_t(indices.size()/3); geometry.index32=true;
  require(impl_->handles.size()<UINT32_MAX/2,"BLAS handle table exhausted");
  const uint32_t handle=uint32_t(impl_->handles.size());
  auto entry=impl_->createEntry(geometry,handle);
  impl_->handles.push_back(entry); impl_->cache.emplace(key,entry); impl_->pending.push_back(entry);
  ++impl_->tableVersion; ++impl_->statistics.misses;
  return handle;
  }
}

uint64_t VirtualGeometryMetalRayTraceBackend::submit(const Frame &frame)
{
  @autoreleasepool { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->submit(frame);
  }
}
VirtualGeometryMetalRayTraceBackend::Completion VirtualGeometryMetalRayTraceBackend::poll(uint64_t serial)
{ std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->completion(serial, false); }
VirtualGeometryMetalRayTraceBackend::Completion VirtualGeometryMetalRayTraceBackend::waitForTesting(uint64_t serial)
{ std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->completion(serial, true); }
VirtualGeometryMetalRayTraceBackend::CacheStatistics VirtualGeometryMetalRayTraceBackend::cacheStatistics() const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->collect();
  auto result = impl_->statistics;
  for (const auto &entry : impl_->pending) if (entry->handle!=0 && impl_->handles[entry->handle]) ++result.pendingBuilds;
  result.scratchBytes = 0;
  for (const auto &slot : impl_->slots) result.scratchBytes += slot.scratch.length + slot.blasScratch.length;
  return result;
}
}
