#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
#include "virtualgeometry/raytracing/metal/MetalPageBlasGate.hpp"
#include <thread>
#include <chrono>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace virtualgeometry::raytracing;
using Backend = VirtualGeometryMetalRayTraceBackend;
namespace
{
void check(bool condition, const char *message)
{
  if (!condition) throw std::runtime_error(message);
}
template<class Fn> void rejects(Fn fn, const char *message)
{
  bool rejected = false;
  try { fn(); } catch (const std::runtime_error &) { rejected = true; }
  check(rejected, message);
}
std::vector<uint32_t> encodedTrianglePage()
{
  // Two clusters with packed, non-indexed source vertices and byte indices.
  std::vector<uint32_t> words(8+2*34+4,0);
  words[0]=2; words[1]=2; words[4]=2;
  for (uint32_t c=0;c<2;++c) {
    auto *d=words.data()+8+c*34;
    d[0]=c; d[1]=1; d[6]=c; d[7]=1; d[8]=3; d[9]=1;
    d[10]=2; d[11]=2;
    const float minimum[]={-1,-1,-4}; std::memcpy(d+14,minimum,12);
    words[8+68+c]=0x920; words[8+68+2+c]=0x020100;
  }
  return words;
}
struct Fixture
{
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> producer;
  id<MTLSharedEvent> event;
  id<MTLComputePipelineState> counts;
  id<MTLBuffer> instances, instanceCount, rays, rayCount, results, vertices;
  id<MTLBuffer> bounds, volumes, volumeCount;
  bool lightCull = false;
  Backend::Frame::Query query=Backend::Frame::Query::AnyHit;
  uint64_t eventValue = 0;
  Backend backend;
  Fixture(uint32_t buildLimit=UINT32_MAX, uint64_t byteLimit=UINT64_MAX, bool residencySets=true) : backend([&] {
    Backend::Configuration config{VG_RT_METALLIB_PATH, 8, 256, 3, 64 * 1024 * 1024};
    config.useResidencySets=residencySets;
    config.maxBlasBuildsPerFrame=buildLimit; config.maxBlasBuildBytesPerFrame=byteLimit; return config;
  }(), (__bridge void *)device)
  {
    if (!backend.capabilities().available) return;
    producer = [device newCommandQueue]; event = [device newSharedEvent];
    auto make = [&](NSUInteger bytes) { return [device newBufferWithLength:bytes options:MTLResourceStorageModeShared]; };
    instances = make(8 * sizeof(RayTraceInstance)); rays = make(256 * sizeof(ShadowRay)); results = make(256 * sizeof(ClosestHitResult));
    bounds=make(8*sizeof(RayTraceBounds)); volumes=make(256*sizeof(RayTraceBounds)); volumeCount=make(4);
    instanceCount = [device newBufferWithLength:4 options:MTLResourceStorageModePrivate];
    rayCount = [device newBufferWithLength:4 options:MTLResourceStorageModePrivate];
    const float triangle[] = {-1,-1,-4, 1,-1,-4, 0,1,-4};
    vertices = [device newBufferWithBytes:triangle length:sizeof(triangle) options:MTLResourceStorageModeShared];
    for (uint32_t i=0; i<256; ++i) static_cast<ShadowRay *>(rays.contents)[i] = ShadowRay{};
    for (uint32_t i=0; i<8; ++i) static_cast<RayTraceInstance *>(instances.contents)[i] = RayTraceInstance{};
    // Test-only producer: prove the count buffers never need CPU mapping and
    // that a separate GPU queue's writes reach the native AS encoder.
    NSError *error = nil;
    auto library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void counts(device uint &a [[buffer(0)]], device uint &b [[buffer(1)]], constant uint2 &v [[buffer(2)]]) { a=v.x; b=v.y; }"
        options:nil error:&error];
    check(library != nil, "Cannot compile test count producer");
    counts = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"counts"] error:&error];
    check(counts != nil, "Cannot create test count producer");
  }
  uint32_t geometry(uint64_t revision=1)
  {
    Backend::Geometry g;
    g.key = {1,revision,0,0}; g.vertexBuffer = (__bridge void *)vertices;
    g.vertexStride=12; g.vertexCount=3; g.triangleCount=1;
    return backend.cacheGeometry(g);
  }
  uint64_t submit(uint32_t n, uint32_t r, uint32_t slot=0)
  {
    auto command = [producer commandBuffer];
    auto encoder = [command computeCommandEncoder];
    const uint32_t values[] = {n,r};
    [encoder setComputePipelineState:counts];
    [encoder setBuffer:instanceCount offset:0 atIndex:0]; [encoder setBuffer:rayCount offset:0 atIndex:1];
    [encoder setBytes:values length:sizeof(values) atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [encoder endEncoding];
    const uint64_t ready = ++eventValue, done = ++eventValue;
    [command encodeSignalEvent:event value:ready]; [command commit];
    Backend::Frame frame;
    frame.query=query;
    frame.instances=(__bridge void *)instances; frame.instanceCount=(__bridge void *)instanceCount;
    frame.rays=(__bridge void *)rays; frame.rayCount=(__bridge void *)rayCount;
    frame.results=(__bridge void *)results; frame.sharedEvent=(__bridge void *)event;
    if (lightCull) {
      frame.casterBounds=(__bridge void *)bounds; frame.lightVolumes=(__bridge void *)volumes;
      frame.lightVolumeCount=(__bridge void *)volumeCount;
    }
    frame.readyValue=ready; frame.doneValue=done; frame.slot=slot;
    return backend.submit(frame);
  }
  Backend::Completion run(uint32_t n, uint32_t r)
  {
    auto completion = backend.waitForTesting(submit(n,r));
    if (!completion.succeeded) throw std::runtime_error(completion.error);
    check(event.signaledValue == eventValue, "Native done event did not cover AS/trace work");
    return completion;
  }
  uint32_t result(uint32_t i=0) const { return static_cast<uint32_t *>(results.contents)[i]; }
};
}

int main()
{
  @autoreleasepool {
    try
    {
      Fixture f;
      if (!f.backend.capabilities().available)
      {
        std::cout << "SKIP: " << f.backend.capabilities().reason << '\n'; return 77;
      }
      if (@available(macOS 15.0, *))
        if ([f.device supportsFamily:MTLGPUFamilyApple6])
          check(f.backend.capabilities().residencySets, "Expected native BLAS residency sets");
      {
        Fixture fallback(UINT32_MAX, UINT64_MAX, false);
        static_cast<RayTraceInstance *>(fallback.instances.contents)[0].geometryHandle=fallback.geometry();
        check(!fallback.backend.capabilities().residencySets, "Residency fallback not selected");
        fallback.run(1,1);
        check(fallback.result()==1, "Legacy residency declaration lost shadow hit");
      }
      auto &instance = static_cast<RayTraceInstance *>(f.instances.contents)[0];
      auto *rays = static_cast<ShadowRay *>(f.rays.contents);
      instance.geometryHandle = f.geometry();
      check(f.geometry() == instance.geometryHandle, "Geometry cache failed to deduplicate");
      rays[1].direction[0] = 2;
      rays[2].maxDistance = 2;
      rays[3].mask = 0;
      auto first = f.run(1,4);
      check(first.statistics.instances == 1 && first.statistics.rays == 4 && first.statistics.invalidScene == 0, "GPU counts invalid");
      check(f.result(0)==1 && f.result(1)==0 && f.result(2)==0 && f.result(3)==0, "Hit/miss/distance/mask parity failed");
      check(f.backend.cacheStatistics().builds == 1, "Expected exactly one non-sentinel BLAS build");
      for (int i=0; i<8; ++i) f.run(1,4);
      check(f.backend.cacheStatistics().builds == 1, "Warm geometry was rebuilt");
      instance.transform[9]=3;
      f.run(1,1); check(f.result()==0, "Translated instance did not move");
      instance.transform[9]=0; instance.transform[0]=-2; instance.transform[4]=0.5f;
      f.run(1,1); check(f.result()==1, "Negative/nonuniform scale failed");
      instance.transform[0]=1; instance.transform[4]=1;
      instance.transform[0]=0;
      f.run(1,1); check(f.result()==2, "Singular transform must not reach TLAS as valid geometry");
      instance.transform[0]=1;
      f.run(0,1); check(f.result()==0, "Empty scene must be unoccluded");
      f.run(1,0);
      instance.mask=0; f.run(1,1); check(f.result()==0, "Instance mask ignored"); instance.mask=0xff;
      instance.geometryHandle=999999; f.run(1,1); check(f.result()==2, "Invalid BLAS handle must not become a false-lit result");
      instance.geometryHandle=f.geometry();
      f.run(9,1); check(f.result()==2, "Instance overflow must invalidate the result");
      f.run(1,257); check(f.result()==2, "Ray overflow must invalidate the result");
      auto serial=f.submit(1,1);
      f.backend.evictGeometry({1,1,0,0});
      auto inFlight=f.backend.waitForTesting(serial);
      check(inFlight.succeeded && f.result()==1, "Eviction invalidated an in-flight TLAS");
      f.run(1,1); check(f.result()==2, "Evicted handle must not alias another geometry");
      const auto old=instance.geometryHandle;
      instance.geometryHandle=f.geometry(2);
      check(instance.geometryHandle!=old, "Reused a stale geometry handle");
      f.run(1,1); check(f.result()==1, "Replacement version failed");
      check(f.backend.cacheStatistics().builds==2, "Version build count incorrect");
      // Rotate the triangle into the YZ plane at x=8, z=-4: outside a 90-degree
      // camera frustum, but between the visible receiver and point light.
      instance.transform[0]=0; instance.transform[2]=-1;
      instance.transform[6]=1; instance.transform[8]=0;
      instance.transform[9]=12; instance.transform[11]=-4;
      auto &second=static_cast<RayTraceInstance *>(f.instances.contents)[1];
      second=instance; second.transform[9]=50;
      rays[0]=ShadowRay{}; rays[0].origin[2]=-4;
      rays[0].direction[0]=1; rays[0].direction[2]=0; rays[0].maxDistance=10;
      f.run(2,1); check(f.result()==1,"Unculled offscreen blocker fixture failed");
      auto *bounds=static_cast<RayTraceBounds *>(f.bounds.contents);
      bounds[0]={{8,-1,-5},0,{8,1,-3},0}; bounds[1]={{46,-1,-5},0,{46,1,-3},0};
      static_cast<RayTraceBounds *>(f.volumes.contents)[0]={{0,-0.1f,-4.1f},0,{10,0.1f,-3.9f},0};
      *static_cast<uint32_t *>(f.volumeCount.contents)=1;
      f.lightCull=true;
      auto culled=f.run(2,1);
      check(f.result()==1 && culled.statistics.instances==1,"Light culling lost offscreen shadow or retained irrelevant caster");
      *static_cast<uint32_t *>(f.volumeCount.contents)=0;
      f.run(2,1); check(f.result()==0,"Empty light-volume set must produce empty caster set");
      *static_cast<uint32_t *>(f.volumeCount.contents)=257;
      f.run(2,1); check(f.result()==2,"Light-volume overflow must be reported");
      // Two overlapping blockers in reverse depth order: any-hit is not enough.
      f.lightCull=false; f.query=Backend::Frame::Query::ClosestHit;
      instance=RayTraceInstance{}; instance.geometryHandle=f.geometry(2);
      second=instance; instance.transform[11]=-3;
      rays[0]=ShadowRay{}; rays[1]=ShadowRay{}; rays[1].origin[0]=20;
      f.run(2,2);
      auto *hits=static_cast<ClosestHitResult *>(f.results.contents);
      check(hits[0].state==1 && std::abs(hits[0].distance-4.0f)<1e-5f && hits[0].instance==1,
            "Closest hit did not choose the nearest transformed blocker");
      check(hits[1].state==0,"Closest-hit miss state failed");
      rays[0].maxDistance=2; f.run(2,1); check(hits[0].state==0,"Closest hit ignored ray interval");
      instance.geometryHandle=99999; f.run(2,1); check(hits[0].state==2,"Incomplete closest scene became valid");
      Backend::Geometry invalid;
      invalid.key={7,1,0,0}; invalid.vertexBuffer=(__bridge void *)f.vertices;
      invalid.vertexCount=900; invalid.triangleCount=300;
      rejects([&]{ f.backend.cacheGeometry(invalid); }, "Out-of-range geometry was accepted");
      Fixture limited(1);
      limited.geometry(1);
      static_cast<RayTraceInstance *>(limited.instances.contents)[0].geometryHandle=limited.geometry(2);
      limited.run(1,1);
      check(limited.backend.cacheStatistics().builds==1 && limited.backend.cacheStatistics().pendingBuilds==1,
            "BLAS count budget was exceeded or dropped deferred work");
      check(limited.result()==2,"Unbuilt BLAS must not enter a valid TLAS");
      limited.run(1,1);
      check(limited.backend.cacheStatistics().builds==2 && limited.backend.cacheStatistics().pendingBuilds==0 && limited.result()==1,
            "Deferred BLAS was not admitted next frame");
      Fixture encoded(1);
      auto page=encodedTrianglePage();
      auto handles=encoded.backend.cacheEncodedPage(90,1,page.data(),page.size(),8,1);
      check(handles.size()==2 && encoded.backend.geometryState(handles[0])==Backend::GeometryState::Pending,
          "Encoded page was published before build submission");
      Backend::Frame build; build.work=Backend::Frame::Work::BuildOnly;
      serial=encoded.backend.submit(build); // No ray/instance buffers.
      check(encoded.backend.waitForTesting(serial).succeeded,"Install-time GPU decode/build failed");
      check(encoded.backend.geometryState(handles[0])==Backend::GeometryState::Ready &&
          encoded.backend.geometryState(handles[1])==Backend::GeometryState::Pending,"Partial page ignored the build quota");
      serial=encoded.backend.submit(build);
      check(encoded.backend.waitForTesting(serial).succeeded,"Second cluster build failed");
      check(encoded.backend.geometryState(handles[0])==Backend::GeometryState::Ready &&
          encoded.backend.geometryState(handles[1])==Backend::GeometryState::Ready,"Completion was lost on frame reuse");
      static_cast<RayTraceInstance *>(encoded.instances.contents)[0].geometryHandle=handles[1];
      encoded.query=Backend::Frame::Query::ClosestHit; encoded.run(1,1);
      auto decodedHit=static_cast<ClosestHitResult *>(encoded.results.contents)[0];
      check(decodedHit.state==1 && std::abs(decodedHit.distance-4.0f)<1e-5f,"Encoded GPU BLAS differs from decoded triangle");
      auto before=encoded.backend.cacheStatistics();
      auto again=encoded.backend.cacheEncodedPage(90,1,page.data(),page.size(),8,1);
      check(again==handles && encoded.backend.cacheStatistics().builds==before.builds,"Reinstall rebuilt immutable cluster BLAS");
      page.back()=0x030100;
      rejects([&]{ encoded.backend.cacheEncodedPage(91,1,page.data(),page.size(),8,1); },"Invalid encoded indices were accepted");
      check(encoded.backend.cacheStatistics().residentBytes==before.residentBytes,"Rejected page stranded cache entries");
      Backend::Configuration serviceConfig{VG_RT_METALLIB_PATH,8,256,3,64*1024*1024};
      serviceConfig.maxBlasPreparationsPerFrame=64;
      auto serviceBackend=std::make_shared<Backend>(serviceConfig,
          (__bridge void *)encoded.device);
      MetalPageBlasGate gate(serviceBackend,2);
      page=encodedTrianglePage(); const PageBlasKey pageKey{77,4,UINT64_MAX,7};
      check(gate.prepareEncoded(pageKey,page.data(),page.size(),8,1)==PageBlasGate::State::Pending && gate.handles(pageKey).empty(),
          "Page gate exposed incomplete handles");
      for (uint32_t attempt=0;attempt<2000 && gate.state(pageKey,2)!=PageBlasGate::State::Ready;++attempt) {
        gate.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(gate.handles(pageKey).size()==2,"Asynchronous page gate never completed");
      check(gate.state({78,4,UINT64_MAX,7},2)==PageBlasGate::State::Missing,"Recycled page slot aliased an owner");
      check(gate.state(pageKey,1)==PageBlasGate::State::Missing,"Incomplete manifest was accepted");
      gate.releaseOwner(77); check(gate.handles(pageKey).empty(),"Released owner retained publishable BLAS handles");
      // Generated input stays private; its producer signals only after the
      // installer has submitted a wait, proving completion is not submission.
      auto generatedVertices=[encoded.device newBufferWithLength:encoded.vertices.length options:MTLResourceStorageModePrivate];
      auto generatedEvent=[encoded.device newSharedEvent];
      auto uploadGenerated=[encoded.producer commandBuffer];
      auto generatedBlit=[uploadGenerated blitCommandEncoder];
      [generatedBlit copyFromBuffer:encoded.vertices sourceOffset:0 toBuffer:generatedVertices destinationOffset:0 size:encoded.vertices.length];
      [generatedBlit endEncoding]; [uploadGenerated encodeSignalEvent:generatedEvent value:1];
      PageBlasGate::ClusterGeometry generated;
      generated.vertices=(__bridge void *)generatedVertices; generated.vertexStride=12; generated.vertexCount=3; generated.triangleCount=1;
      const PageBlasKey generatedKey{79,0,1,1};
      check(gate.prepareDecoded(generatedKey,{generated},(__bridge void *)generatedEvent,1)==PageBlasGate::State::Pending,
          "Generated input published before producer completion");
      gate.pump();
      check(gate.state(generatedKey,1)==PageBlasGate::State::Pending,"BLAS ignored the unsignaled producer event");
      check(!gate.releasePage(generatedKey),"Cancellation released a BLAS with outstanding GPU input reads");
      [uploadGenerated commit];
      for (uint32_t attempt=0;attempt<2000 && gate.state(generatedKey,1)!=PageBlasGate::State::Ready;++attempt) {
        gate.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      auto generatedHandles=gate.handles(generatedKey);
      check(generatedHandles.size()==1,"Generated page did not complete after GPU producer");
      static_cast<RayTraceInstance *>(encoded.instances.contents)[0].geometryHandle=generatedHandles[0];
      Backend::Frame generatedTrace;
      generatedTrace.work=Backend::Frame::Work::TlasAndTrace;
      generatedTrace.instances=(__bridge void *)encoded.instances; generatedTrace.instanceCount=(__bridge void *)encoded.instanceCount;
      generatedTrace.rays=(__bridge void *)encoded.rays; generatedTrace.rayCount=(__bridge void *)encoded.rayCount;
      generatedTrace.results=(__bridge void *)encoded.results;
      const auto buildsBeforeTrace=serviceBackend->cacheStatistics().builds;
      check(serviceBackend->waitForTesting(serviceBackend->submit(generatedTrace)).succeeded && encoded.result()==1,
          "Generated event-ordered BLAS produced wrong visibility");
      check(serviceBackend->cacheStatistics().builds==buildsBeforeTrace,"Install-managed trace unexpectedly built geometry");
      // Generated pages support 64 groups of seven clusters, not just 256.
      const PageBlasKey fullPageKey{8001,0,1,1};
      std::vector<PageBlasGate::ClusterGeometry> fullPage(448u, generated);
      check(gate.prepareDecoded(fullPageKey,fullPage,nullptr,0,true)==PageBlasGate::State::Pending,
          "Full terrain page manifest was rejected");
      for (uint32_t attempt=0;attempt<2000 && gate.state(fullPageKey,448u)!=PageBlasGate::State::Ready;++attempt) {
        gate.pump();
        if (gate.prepareDecoded(fullPageKey,fullPage,nullptr,0,true)!=PageBlasGate::State::Ready)
          check(gate.handles(fullPageKey).empty(),"Partially prepared page exposed BLAS handles");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(gate.handles(fullPageKey).size()==448u,"Full terrain page BLAS did not complete");
      gate.releaseOwner(fullPageKey.owner);
      Fixture sizing;
      const auto sentinelBytes=sizing.backend.cacheStatistics().residentBytes;
      sizing.geometry();
      const auto oneClusterBudget=sizing.backend.cacheStatistics().residentBytes;
      auto tightBackend=std::make_shared<Backend>(Backend::Configuration{VG_RT_METALLIB_PATH,8,256,3,oneClusterBudget},
          (__bridge void *)encoded.device);
      MetalPageBlasGate tightGate(tightBackend,2);
      page=encodedTrianglePage();
      for (uint32_t attempt=0;attempt<3;++attempt)
        check(tightGate.prepareEncoded({80,0,1,1},page.data(),page.size(),8,1)==PageBlasGate::State::Pending &&
            tightBackend->cacheStatistics().residentBytes==sentinelBytes && tightBackend->cacheStatistics().pendingBuilds==0,
            "Page budget failure stranded a partial BLAS or became a fatal install error");
      encoded.run(0,0);
      Backend emptyManaged({VG_RT_METALLIB_PATH,8,256,1,64*1024*1024},(__bridge void *)encoded.device);
      auto emptyCompletion=emptyManaged.waitForTesting(emptyManaged.submit(generatedTrace));
      check(emptyCompletion.succeeded && emptyCompletion.statistics.invalidScene==0 &&
          emptyManaged.cacheStatistics().builds==0,"Empty install-managed scene did not initialize its masked sentinel");
      auto cancelledBackend=std::make_shared<Backend>(Backend::Configuration{VG_RT_METALLIB_PATH,8,256,1,64*1024*1024},
          (__bridge void *)encoded.device);
      MetalPageBlasGate cancelledGate(cancelledBackend,0);
      page=encodedTrianglePage();
      check(cancelledGate.prepareEncoded({88,0,1,1},page.data(),page.size(),8,1)==PageBlasGate::State::Pending &&
          cancelledGate.releasePage({88,0,1,1}) && cancelledBackend->cacheStatistics().pendingBuilds==0,
          "Unsubmitted cancelled geometry should not consume a future BLAS budget");
      Fixture zeroBytes(UINT32_MAX,0);
      static_cast<RayTraceInstance *>(zeroBytes.instances.contents)[0].geometryHandle=zeroBytes.geometry();
      zeroBytes.run(1,1);
      check(zeroBytes.backend.cacheStatistics().builds==0 && zeroBytes.result()==2,"BLAS byte quota was exceeded");
      std::cout << "PASS: " << f.backend.capabilities().device
                << "; GPU indirect counts, AS hits/misses, transforms, masks, empty scene, overflow, cache and eviction\n";
      return 0;
    }
    catch(const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
  }
}
