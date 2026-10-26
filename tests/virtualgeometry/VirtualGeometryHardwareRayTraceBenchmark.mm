#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace virtualgeometry::raytracing;
using Backend = VirtualGeometryMetalRayTraceBackend;
namespace
{
uint32_t number(const char *s)
{
  size_t end=0; const auto value=std::stoull(s,&end);
  if (end!=std::strlen(s) || value>UINT32_MAX) throw std::runtime_error("Invalid numeric option");
  return uint32_t(value);
}
double percentile(std::vector<double> values, double p)
{
  std::sort(values.begin(),values.end());
  return values.at(size_t(std::ceil(p * values.size()))-1);
}
NSString *str(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::string hashBytes(const void *data, size_t bytes)
{
  uint64_t h=14695981039346656037ull;
  auto *p=static_cast<const uint8_t *>(data);
  for(size_t i=0;i<bytes;++i) { h^=p[i]; h*=1099511628211ull; }
  std::ostringstream out; out<<std::hex<<std::setw(16)<<std::setfill('0')<<h; return out.str();
}
}

int main(int argc, char **argv)
{
  @autoreleasepool {
    try
    {
      std::string suite="full", output, query="any", geometryInput="decoded";
      uint32_t instanceCount=1000, rayCount=100000, clusterCount=1, triangles=64, warmup=120, frames=600;
      for (int i=1;i<argc;++i)
      {
        const std::string key=argv[i];
        if (i+1>=argc) throw std::runtime_error("Each option requires a value");
        const char *value=argv[++i];
        if(key=="--suite") suite=value;
        else if(key=="--query") query=value;
        else if(key=="--geometry") geometryInput=value;
        else if(key=="--output") output=value;
        else if(key=="--instances") instanceCount=number(value);
        else if(key=="--rays") rayCount=number(value);
        else if(key=="--clusters") clusterCount=number(value);
        else if(key=="--triangles") triangles=number(value);
        else if(key=="--warmup") warmup=number(value);
        else if(key=="--frames") frames=number(value);
        else if(key=="--backend") { if(std::string(value)!="metal") throw std::runtime_error("Only native Metal is implemented"); }
        else throw std::runtime_error("Unknown option: " + key);
      }
      require(query=="any" || query=="closest","Query must be any or closest");
      require(suite=="blas" || suite=="tlas" || suite=="trace" || suite=="full", "Suite must be blas, tlas, trace or full");
      require(instanceCount>0 && rayCount>0 && clusterCount>0 && frames>0 && triangles>0 && triangles<=128,
              "Counts must be positive and triangles must be <=128");
      require(uint64_t(frames)+warmup < UINT32_MAX, "Too many frames");
      require(geometryInput=="decoded" || geometryInput=="encoded","Geometry must be decoded or encoded");
      require(geometryInput!="encoded" || triangles<=85,"Encoded synthetic clusters require <=255 vertices (85 triangles)");
      auto device=MTLCreateSystemDefaultDevice();
      Backend backend({VG_RT_METALLIB_PATH,instanceCount,rayCount,3,1024ull*1024*1024},(__bridge void *)device);
      if (!backend.capabilities().available)
      {
        std::cout<<"SKIP: "<<backend.capabilities().reason<<'\n'; return 77;
      }
      auto make=[&](uint64_t bytes) {
        require(bytes<=device.maxBufferLength,"Benchmark buffer exceeds device limit");
        auto buffer=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        require(buffer!=nil,"Benchmark allocation failed"); return buffer;
      };
      // Synthetic opaque clusters, each a triangle grid in its local Z=-4 plane.
      // Independent immutable identities deliberately measure per-cluster AS
      // overhead, even though their local shapes are identical.
      std::vector<float> positions(uint64_t(triangles)*9);
      for(uint32_t t=0;t<triangles;++t)
      {
        float x=float(t%8)*0.2f-0.8f, y=float(t/8)*0.2f-0.8f;
        const float v[]={x,y,-4,x+0.18f,y,-4,x,y+0.18f,-4};
        std::memcpy(positions.data()+t*9,v,sizeof(v));
      }
      auto vertices=make(positions.size()*4); std::memcpy(vertices.contents,positions.data(),positions.size()*4);
      auto instances=make(uint64_t(instanceCount)*sizeof(RayTraceInstance));
      auto rays=make(uint64_t(rayCount)*sizeof(ShadowRay));
      const uint32_t resultStride=query=="closest" ? sizeof(ClosestHitResult) : 4;
      auto results=make(uint64_t(rayCount)*resultStride);
      auto gpuInstances=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];
      auto gpuRays=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];
      // GPU count upload completed during setup. Timed frames have no count reads
      // or writes on the CPU; correctness tests separately vary them in compute.
      const uint32_t counts[]={instanceCount,rayCount};
      auto staging=make(sizeof(counts)); std::memcpy(staging.contents,counts,sizeof(counts));
      auto uploadQueue=[device newCommandQueue]; auto upload=[uploadQueue commandBuffer];
      auto blit=[upload blitCommandEncoder];
      [blit copyFromBuffer:staging sourceOffset:0 toBuffer:gpuInstances destinationOffset:0 size:4];
      [blit copyFromBuffer:staging sourceOffset:4 toBuffer:gpuRays destinationOffset:0 size:4];
      [blit endEncoding]; [upload commit]; [upload waitUntilCompleted];
      require(upload.status==MTLCommandBufferStatusCompleted,"Benchmark count upload failed");

      const uint32_t side=uint32_t(std::ceil(std::sqrt(double(instanceCount))));
      auto *rayData=static_cast<ShadowRay *>(rays.contents);
      for(uint32_t i=0;i<rayCount;++i)
      {
        ShadowRay r;
        const uint32_t tile=i%instanceCount;
        r.origin[0]=float(tile%side)*2-0.75f;
        r.origin[1]=float(tile/side)*2-0.75f;
        if(i&1) r.origin[0]+=1.5f; // deterministic 50% misses, away from boundaries
        rayData[i]=r;
      }
      // Fixed 9-bit XY quantization, zero-bit Z, packed byte triangle indices.
      // Construct outside the measured install path; timed work copies/validates
      // the page, expands on GPU and builds its per-cluster BLASes.
      std::vector<std::vector<uint32_t>> encodedPages;
      if (geometryInput=="encoded") for (uint32_t base=0;base<clusterCount;base+=256) {
        const uint32_t n=std::min(256u,clusterCount-base), verticesPerCluster=triangles*3;
        const uint32_t posWords=(verticesPerCluster*18+31)/32, indexWords=(verticesPerCluster+3)/4;
        std::vector<uint32_t> page(8+n*34+n*(posWords+indexWords),0);
        page[0]=n; page[1]=n*posWords; page[4]=n*indexWords;
        for (uint32_t c=0;c<n;++c) {
          auto *d=page.data()+8+c*34;
          d[0]=c*posWords; d[1]=posWords; d[6]=c*indexWords; d[7]=indexWords;
          d[8]=verticesPerCluster; d[9]=triangles; d[10]=511; d[11]=511;
          const float minima[]={-80,-80,-400}; std::memcpy(d+14,minima,12);
          auto *position=page.data()+8+n*34+c*posWords;
          auto *indices=page.data()+8+n*34+n*posWords+c*indexWords;
          for (uint32_t v=0;v<verticesPerCluster;++v) {
            for (uint32_t axis=0;axis<2;++axis) {
              const uint32_t value=uint32_t(std::lround(positions[v*3+axis]*100+80));
              const uint32_t bit=v*18+axis*9;
              position[bit/32]|=value<<(bit%32);
              if (bit%32+9>32) position[bit/32+1]|=value>>(32-bit%32);
            }
            indices[v/4]|=v<<((v%4)*8);
          }
        }
        encodedPages.push_back(std::move(page));
      }
      auto install=[&](uint64_t revision) {
        std::vector<uint32_t> handles;
        if (geometryInput=="encoded") {
          for (uint32_t p=0;p<encodedPages.size();++p) {
            auto &page=encodedPages[p];
            auto added=backend.cacheEncodedPage(p+1,revision,page.data(),page.size(),8,100);
            handles.insert(handles.end(),added.begin(),added.end());
          }
        } else for(uint32_t i=0;i<clusterCount;++i)
        {
          Backend::Geometry g;
          g.key={1,revision,i,0}; g.vertexBuffer=(__bridge void *)vertices;
          g.vertexStride=12; g.vertexCount=triangles*3; g.triangleCount=triangles;
          handles.push_back(backend.cacheGeometry(g));
        }
        auto *data=static_cast<RayTraceInstance *>(instances.contents);
        for(uint32_t i=0;i<instanceCount;++i)
        {
          data[i]=RayTraceInstance{}; data[i].geometryHandle=handles[i%clusterCount];
          data[i].transform[9]=float(i%side)*2; data[i].transform[10]=float(i/side)*2;
        }
      };
      install(1);
      Backend::Frame frame;
      frame.query=query=="closest" ? Backend::Frame::Query::ClosestHit : Backend::Frame::Query::AnyHit;
      frame.instances=(__bridge void *)instances; frame.instanceCount=(__bridge void *)gpuInstances;
      frame.rays=(__bridge void *)rays; frame.rayCount=(__bridge void *)gpuRays; frame.results=(__bridge void *)results;
      // Fill all frame TLAS slots before measuring warm work; verify the trace
      // baseline before collecting timings, so fast invalid results never count.
      for(uint32_t slot=0;slot<3;++slot)
      {
        frame.slot=slot;
        auto completion=backend.waitForTesting(backend.submit(frame));
        require(completion.succeeded && completion.statistics.invalidScene==0,"Benchmark warmup failed");
      }
      auto *hitData=static_cast<uint32_t *>(results.contents);
      uint64_t hits=0;
      for(uint32_t i=0;i<rayCount;++i)
      {
        const auto state=query=="closest" ? static_cast<ClosestHitResult *>(results.contents)[i].state : hitData[i];
        require(state==uint32_t((i&1)==0),"Benchmark baseline visibility is incorrect");
        if (query=="closest" && state==1)
          require(std::abs(static_cast<ClosestHitResult *>(results.contents)[i].distance-4)<1e-4,"Benchmark closest distance incorrect");
        hits+=state;
      }
      const auto warmBuilds=backend.cacheStatistics().builds;
      if(suite=="trace") frame.work=Backend::Frame::Work::TraceOnly;
      else if(suite=="tlas") frame.work=Backend::Frame::Work::BuildAndTlas;
      else if(suite=="blas") frame.work=Backend::Frame::Work::BuildOnly;
      std::vector<double> gpu, cpu, preparation;
      uint64_t revision=1;
      const auto wallStart=std::chrono::steady_clock::now();
      for(uint32_t i=0;i<warmup+frames;++i)
      {
        const auto prepareStart=std::chrono::steady_clock::now();
        if(suite=="blas")
        {
          for(uint32_t c=0;c<clusterCount;++c)
            backend.evictGeometry(geometryInput=="encoded" ? GeometryKey{c/256+1,revision,c%256,1} : GeometryKey{1,revision,c,0});
          install(++revision);
        }
        frame.slot=i%3;
        const auto start=std::chrono::steady_clock::now();
        auto serial=backend.submit(frame);
        const auto encoded=std::chrono::steady_clock::now();
        const auto completion=backend.waitForTesting(serial);
        require(completion.succeeded,"GPU benchmark command failed");
        if(suite!="blas") require(completion.statistics.invalidScene==0,"Benchmark cut overflow/invalidity");
        if(i>=warmup)
        {
          gpu.push_back(completion.gpuMilliseconds);
          cpu.push_back(std::chrono::duration<double,std::milli>(encoded-start).count());
          preparation.push_back(std::chrono::duration<double,std::milli>(start-prepareStart).count());
        }
      }
      const auto stats=backend.cacheStatistics();
      require(suite=="blas" || stats.builds==warmBuilds,"Warm benchmark rebuilt immutable BLAS");
      const double p50=percentile(gpu,.5), p95=percentile(gpu,.95), p99=percentile(gpu,.99);
      NSMutableDictionary *report=[@{
        @"schema_version":@1, @"backend":@"metal", @"geometry_input":str(geometryInput), @"suite":str(suite), @"query":str(query),
        @"device":str(backend.capabilities().device), @"apple_family9_or_newer":@(backend.capabilities().appleFamily9OrNewer),
        @"os":NSProcessInfo.processInfo.operatingSystemVersionString,
        @"timing_scope":@"native_command_buffer_gpu_elapsed", @"submission_mode":@"serialized_microbenchmark",
        @"scene":@"synthetic_opaque_cluster_grid", @"quality_verified":@YES,
        @"ray_hash":str(hashBytes(rays.contents,uint64_t(rayCount)*sizeof(ShadowRay))),
        @"geometry_hash":str(hashBytes(vertices.contents,vertices.length)),
        @"baseline_result_hash":str(hashBytes(results.contents,uint64_t(rayCount)*resultStride)),
        @"instances":@(instanceCount), @"unique_clusters":@(clusterCount), @"triangles_per_cluster":@(triangles),
        @"rays":@(rayCount), @"baseline_hits":@(hits), @"warmup":@(warmup), @"frames":@(frames),
        @"gpu_ms_p50":@(p50), @"gpu_ms_p95":@(p95), @"gpu_ms_p99":@(p99),
        @"cpu_encode_ms_p50":@(percentile(cpu,.5)), @"cpu_encode_ms_p95":@(percentile(cpu,.95)),
        @"cpu_install_prepare_ms_p50":@(percentile(preparation,.5)), @"cpu_install_prepare_ms_p95":@(percentile(preparation,.95)),
        @"blas_builds_total":@(stats.builds), @"blas_builds_during_measurement_and_warmup":@(stats.builds-warmBuilds),
        @"blas_bytes":@(stats.residentBytes), @"retired_blas_bytes":@(stats.retiredBytes),
        @"tlas_bytes":@(stats.tlasBytes), @"scratch_bytes":@(stats.scratchBytes),
        @"wall_ms_including_waits_and_warmup":@(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wallStart).count())
      } mutableCopy];
#ifdef NDEBUG
      report[@"build_type"]=@"release";
#else
      report[@"build_type"]=@"debug";
#endif
      if(suite=="trace" || suite=="full") report[@"native_pipeline_mray_per_s_p50"]=@(double(rayCount)/(p50*1000));
      if(suite=="blas") report[@"clusters_built_per_s_p50"]=@(double(clusterCount)/(p50/1000));
      NSMutableArray *samples=[NSMutableArray array];
      for(double value:gpu) [samples addObject:@(value)];
      report[@"gpu_ms_samples"]=samples;
      NSError *error=nil;
      auto json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:&error];
      require(json!=nil,"Cannot serialize benchmark report");
      if(!output.empty()) require([json writeToFile:str(output) options:NSDataWritingAtomic error:&error],"Cannot write benchmark report");
      std::cout.write(static_cast<const char *>(json.bytes),json.length); std::cout<<'\n';
      return 0;
    }
    catch(const std::exception &e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
  }
}
