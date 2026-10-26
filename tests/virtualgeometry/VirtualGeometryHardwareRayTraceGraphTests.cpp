#include "virtualgeometry/rendering/VirtualGeometryHardwareRayTracePass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryRayTraceCutPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "os/Logger.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapRayTraceDepthPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryScreenFirstShadowPass.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelHardwareRayTraceScene.hpp"
#include "virtualgeometry/CityVoxelHardwareRayTraceScene.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

using namespace rendering;
using namespace virtualgeometry::raytracing;

class ReadPublishedDepth final : public Pass {
  virtualgeometry::VirtualShadowMapManager &manager_; virtualgeometry::gpgpu::VirtualShadowMapRayTraceDepthPass &producer_;
  Buffer output_; BindingsLayout layout_; BindingGroups groups_; Shader shader_; ComputePipeline pipeline_;
public:
  ReadPublishedDepth(virtualgeometry::VirtualShadowMapManager &m,virtualgeometry::gpgpu::VirtualShadowMapRayTraceDepthPass &p,Buffer out):manager_(m),producer_(p),output_(out) {}
  ~ReadPublishedDepth() override { renderGraph->deleteComputePipeline(pipeline_); renderGraph->deleteShader(shader_); renderGraph->deleteBindingGroups(groups_); renderGraph->deleteBindingsLayout(layout_); }
  void recordCommandBuffer(CommandRecorder &cmd) override {
    BindingGroupLayout layout; GroupInfo group; group.name="readAtlas";
    for (auto pair:std::vector<std::pair<uint32_t,Buffer>>{{0,producer_.getUniformBuffer()},{7,producer_.getPageJobsBuffer()},{8,producer_.getRayCountBuffer()},{15,output_}}) {
      auto [i,b]=pair;
      layout.buffers.push_back({.name="b"+std::to_string(i),.binding=i,.type=i==0 ? BufferBindingType_UniformBuffer : BufferBindingType_StorageBuffer,.visibility=BindingVisibility_Compute});
      group.buffers.push_back({.binding=i,.bufferView={.buffer=b,.access=i==15 ? AccessPattern::SHADER_WRITE : AccessPattern::SHADER_READ,.offset=0,.size=renderGraph->getBufferSize(b)}});
    }
    layout.textures.push_back({.name="atlas",.binding=14,.visibility=BindingVisibility_Compute});
    group.textures.push_back({.binding=14,.textureView={.texture=manager_.getShadowAtlasTexture(),.access=AccessPattern::SHADER_READ,.layout=ResourceLayout::SHADER_READ_ONLY,.flags=ImageAspectFlags::Depth,.layerCount=1,.levelCount=1}});
    layout_=renderGraph->createBindingsLayout(BindingsLayoutInfo{.name=passName+"layout",.groups={layout}});
    groups_=renderGraph->createBindingGroups(BindingGroupsInfo{.name=passName+"groups",.layout=layout_,.groups={group}});
    shader_=renderGraph->createShader(ShaderInfo{.name=passName+"shader",.src=os::io::readRelativeFile("assets/shaders/spirv/vsm-raytrace-publish.spirv"),.layout=layout_,.type=ShaderType::SpirV});
    pipeline_=renderGraph->createComputePipeline(ComputePipelineInfo{.entry="sample_published_depth",.name=passName+"pipeline",.layout=layout_,.shader=shader_});
    cmd.cmdBindComputePipeline(pipeline_); cmd.cmdBindBindingGroups(groups_,nullptr,0); cmd.cmdDispatch(1,1,1);
  }
};

static void testDepthPages(backend::vulkan::VulkanRHI &rhi, bool reverseZ,uint32_t lightType=0) {
  using namespace virtualgeometry;
  RenderGraph graph(&rhi);
  VirtualShadowMapManager::Settings settings;
  settings.virtualPageTableResolution=2; settings.physicalPageSize=8; settings.physicalAtlasResolution=16;
  settings.cascadeCount=1; settings.firstCascadeWorldExtent=4; settings.lightDistance=10; settings.reverseZ=reverseZ;
  settings.maxDirectionalLights=lightType==0 ? 1u : 0u;
  settings.maxSpotLights=lightType==1 ? 1u : 0u;
  settings.maxPointLights=lightType==2 ? 1u : 0u;
  VirtualShadowMapManager manager(&graph,settings);
  if (lightType==0) manager.createDirectionalLight(math::Vec3f(0,-1,0));
  else if (lightType==1) manager.createSpotLight(math::Vec3f(0,10,0),math::Vec3f(0,-1,0),1.5707963f,20);
  else manager.createPointLight(math::Vec3f(0,10,0),20);
  const uint32_t layer=lightType==2 ? 6u : 0u;
  const uint32_t pageIndex=layer*4;
  Camera camera(1.0f,math::Vec3f(0,2,0),math::Vec3f(0,0,-1),reverseZ);
  camera.setAspectRatio(1); camera.setNearFar(.1f,100); camera.updateMatrices(); manager.update(camera);
  VirtualGeometryMetalRayTraceBackend::Configuration config;
  config.metallibPath=VG_RT_METALLIB_PATH; config.maxInstances=1; config.maxRays=64; config.frameSlots=2;
  auto backend=std::make_shared<VirtualGeometryMetalRayTraceBackend>(config,rhi.exportMetalDevice());
  auto buffer=[&](const char *name,uint64_t size,bool pull=false) {
    return graph.createBuffer(BufferInfo{.name=name,.size=size,
      .usage=pull ? BufferUsage_Pull|BufferUsage_CopyDst : BufferUsage_Storage|BufferUsage_Push|BufferUsage_CopySrc,
      .metalExportable=!pull});
  };
  float vertices[18]={-10,0,-10, 10,0,-10, -10,0,10, -10,0,10, 10,0,-10, 10,0,10};
  auto vb=buffer("depthVertices",sizeof(vertices)); graph.bufferWrite(vb,0,sizeof(vertices),vertices);
  VirtualGeometryMetalRayTraceBackend::Geometry geometry;
  geometry.key={999,1,0,0}; geometry.vertexBuffer=rhi.exportMetalBuffer(graph.resolveBufferId(vb));
  geometry.vertexStride=12; geometry.vertexCount=6; geometry.triangleCount=2;
  RayTraceInstance instance; instance.geometryHandle=backend->cacheGeometry(geometry);
  VirtualGeometryMetalRayTraceBackend::Frame build; build.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::BuildOnly; build.slot=1;
  if (!backend->waitForTesting(backend->submit(build)).succeeded) throw std::runtime_error("Depth test BLAS build failed");
  auto instances=buffer("depthInstances",sizeof(instance)); graph.bufferWrite(instances,0,sizeof(instance),&instance);
  auto count=buffer("depthInstanceCount",4); uint32_t one=1; graph.bufferWrite(count,0,4,&one);
  auto *pass=graph.registerPass<virtualgeometry::gpgpu::VirtualShadowMapRayTraceDepthPass>("RTDepth",0,manager,backend,instances,count,
      virtualgeometry::gpgpu::VirtualShadowMapRayTraceDepthPass::Settings{64});
  auto atlasSamples=buffer("atlasSamples",256);
  graph.registerPass<ReadPublishedDepth>("SamplePublishedAtlas",9,manager,*pass,atlasSamples);
  auto vptRead=buffer("vptRead",manager.getVirtualPageTableBufferSize(),true);
  auto depthRead=buffer("depthRead",64*4,true);
  auto rayRead=buffer("rayRead",4,true);
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadVPT",10,manager.getVirtualPageTableBuffer(),0,manager.getVirtualPageTableBufferSize(),vptRead,0,manager.getVirtualPageTableBufferSize());
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadDepths",11,atlasSamples,0,256,depthRead,0,256);
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadRayCount",12,pass->getRayCountBuffer(),0,4,rayRead,0,4);
  auto requestSeed=buffer("requestSeed",manager.getFutureAllocationRequestsBufferSize());
  graph.compile(); RenderGraph::Overrides overrides; manager.appendFrameOverrides(overrides,0);
  overrides.bufferOverrides.erase(manager.getFutureAllocationRequestsBuffer().name);
  overrides.bufferOverrides.emplace(manager.getFutureAllocationRequestsBuffer().name,RenderGraphBufferOverride{.bufferId=graph.resolveBufferId(requestSeed)});
  auto demand=[&] {
    std::vector<uint32_t> entries(manager.getVirtualPageTableBufferSize()/4,0); entries[pageIndex]=5;
    graph.bufferWrite(manager.getVirtualPageTableBuffer(),0,entries.size()*4,entries.data());
    uint32_t counters[8]={0,1,0,0,0,0,0,0}; graph.bufferWrite(manager.getAllocatorCountersBuffer(),0,sizeof(counters),counters);
    uint32_t packed=layer<<16;
    // Seed through a host-writable override; production requests are GPU-owned.
    graph.bufferWrite(requestSeed,0,4,&packed);
  };
  auto run=[&](uint32_t expectedRays,bool valid) {
    RenderGraph::Frame frame; graph.run(frame,overrides); graph.waitFrame(frame);
    uint32_t rays=0,entry=0;
    graph.bufferRead(rayRead,0,4,[&](const void *p){std::memcpy(&rays,p,4);});
    graph.bufferRead(vptRead,uint64_t(pageIndex)*4,4,[&](const void *p){std::memcpy(&entry,p,4);});
    if (rays!=expectedRays || ((entry&1u)==0u)!=valid) throw std::runtime_error("RT page budget/publication mismatch type="+std::to_string(lightType)+" reverse="+std::to_string(reverseZ)+" rays="+std::to_string(rays)+" expected="+std::to_string(expectedRays)+" entry="+std::to_string(entry)+" valid="+std::to_string(valid));
  };
  demand(); pass->setRayBudget(63); run(0,false);
  demand(); pass->setRayBudget(64); run(64,true);
  std::array<float,64> depths{};
  graph.bufferRead(depthRead,0,256,[&](const void *p){std::memcpy(depths.data(),p,256);});
  const float forward=lightType==0 ? 9.9f/19.9f : 19.8f/19.9f;
  const float expected=reverseZ ? 1-forward : forward;
  for (auto depth:depths) if (std::abs(depth-expected)>1e-4f) throw std::runtime_error("RT depth projection mismatch");
  demand(); uint32_t emptyScene=0; graph.bufferWrite(count,0,4,&emptyScene); run(64,true);
  graph.bufferRead(depthRead,0,256,[&](const void *p){std::memcpy(depths.data(),p,256);});
  for (auto depth:depths) if (depth!=(reverseZ ? 0.0f : 1.0f)) throw std::runtime_error("RT miss did not publish far depth");
  // Invalid native scenes must leave the page dirty for fallback/retry.
  demand(); uint32_t overflow=2; graph.bufferWrite(count,0,4,&overflow); run(64,false);
  if (backend->cacheStatistics().builds!=1) throw std::runtime_error("RT page generation rebuilt BLAS");
}

static void testScreenFirst(backend::vulkan::VulkanRHI &rhi,bool worklist=false) {
  using namespace voxel::svraster;
  RenderGraph graph(&rhi,RenderGraph::Settings{.maxFramesInFlight=2u});
  VirtualGeometryMetalRayTraceBackend::Configuration config;
  config.metallibPath=VG_RT_METALLIB_PATH; config.maxInstances=1; config.maxRays=300; config.frameSlots=3;
  auto backend=std::make_shared<VirtualGeometryMetalRayTraceBackend>(config,rhi.exportMetalDevice());
  auto buffer=[&](const char *name,uint64_t size,bool pull=false) {
    return graph.createBuffer(BufferInfo{.name=name,.size=size,.usage=pull ? BufferUsage_Pull|BufferUsage_CopyDst : BufferUsage_Storage|BufferUsage_Push|BufferUsage_CopySrc,.metalExportable=!pull});
  };
  float vertices[9]={-1,-1,-4,1,-1,-4,0,1,-4};
  auto vb=buffer("screenFirstVertices",sizeof(vertices)); graph.bufferWrite(vb,0,sizeof(vertices),vertices);
  VirtualGeometryMetalRayTraceBackend::Geometry geometry;
  geometry.key={1000,1,0,0}; geometry.vertexBuffer=rhi.exportMetalBuffer(graph.resolveBufferId(vb)); geometry.vertexStride=12; geometry.vertexCount=3; geometry.triangleCount=1;
  RayTraceInstance instance; instance.geometryHandle=backend->cacheGeometry(geometry);
  VirtualGeometryMetalRayTraceBackend::Frame build; build.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::BuildOnly; build.slot=2;
  if (!backend->waitForTesting(backend->submit(build)).succeeded) throw std::runtime_error("Screen-first BLAS build failed");
  auto instances=buffer("sfInstances",sizeof(instance)); graph.bufferWrite(instances,0,sizeof(instance),&instance);
  auto instanceCount=buffer("sfInstanceCount",4); uint32_t one=1; graph.bufferWrite(instanceCount,0,4,&one);
  std::vector<SparseVoxelSceneRayGPU> rays(300); std::vector<SparseVoxelSceneRayHitGPU> hits(300);
  std::vector<uint32_t> expected(300);
  for (uint32_t i=0;i<300;++i) {
    rays[i].originMinimumDistance={i%4==1 ? 0.0f : 3.0f,0,0,.001f}; rays[i].directionMaximumDistance={0,0,-1,10};
    rays[i].seedFlags={i,i%4==3 ? 0u : 3u,7,0}; rays[i].pixelRayLight={0,0,i,2};
    hits[i].status=i%4==0 ? 2u : 0u; hits[i].identity={i,2,1,7}; hits[i].hitPositionDistance[3]=1;
    expected[i]=i%4==3 ? 2u : i%4==2 ? 0u : 1u;
  }
  hits[256].identity[3]=8; expected[256]=0; // stale screen blocker must go to world.
  hits[0].hitPositionDistance[3]=0; expected[0]=0; // before tMin is not a blocker.
  hits[4].hitPositionDistance[3]=11; expected[4]=0; // beyond the light is not a blocker.
  rays[9].directionMaximumDistance[2]=-.1f; // scene-ray tMin/tMax stay in world units.
  auto rb=buffer("sfRays",rays.size()*sizeof(rays[0])); graph.bufferWrite(rb,0,rays.size()*sizeof(rays[0]),rays.data());
  auto hb=buffer("sfScreenHits",hits.size()*sizeof(hits[0])); graph.bufferWrite(hb,0,hits.size()*sizeof(hits[0]),hits.data());
  auto count=buffer("sfSourceCount",4); uint32_t n=300; graph.bufferWrite(count,0,4,&n);
  auto indices=buffer("sfActiveIndices",1200); auto state=buffer("sfActiveState",16);
  std::vector<uint32_t> active(300); for (uint32_t i=0;i<300;++i) active[i]=299-i;
  graph.bufferWrite(indices,0,1200,active.data());
  uint32_t activeState[4]={5,1,1,300}; graph.bufferWrite(state,0,16,activeState);
  auto merged=buffer("sfMergedHits",300*48); auto mergedRead=buffer("sfMergedRead",300*48,true);
  std::vector<SparseVoxelSceneRayHitGPU> cleared(300); graph.bufferWrite(merged,0,300*48,cleared.data());
  auto *pass=graph.registerPass<virtualgeometry::gpgpu::VirtualGeometryScreenFirstShadowPass>("ScreenFirst",0,backend,
      virtualgeometry::gpgpu::VirtualGeometryScreenFirstShadowPass::Inputs{rb,hb,count,instances,instanceCount,
        worklist ? indices : Buffer{},worklist ? state : Buffer{},worklist ? merged : Buffer{}});
  if (worklist) graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadMerged",7,merged,0,300*48,mergedRead,0,300*48);
  auto readback=buffer("sfRead",1200,true); auto countRead=buffer("sfCountRead",4,true);
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadVisibility",5,pass->getVisibilityBuffer(),0,1200,readback,0,1200);
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadWorldCount",6,pass->getWorldRayCountBuffer(),0,4,countRead,0,4);
  graph.compile();
  for (uint32_t frameIndex=0;frameIndex<3;++frameIndex) {
    RenderGraph::Frame frame; frame.frameIndex=frameIndex%2; RenderGraph::Overrides overrides; graph.run(frame,overrides); graph.waitFrame(frame);
    std::vector<uint32_t> actual(300); uint32_t worldCount=0;
    graph.bufferRead(readback,0,1200,[&](const void *p){std::memcpy(actual.data(),p,1200);});
    graph.bufferRead(countRead,0,4,[&](const void *p){std::memcpy(&worldCount,p,4);});
    if (actual!=expected || worldCount!=153u) throw std::runtime_error("Screen-first compact/trace/scatter mismatch");
    if (worklist) {
      graph.bufferRead(mergedRead,0,300*48,[&](const void *p){std::memcpy(cleared.data(),p,300*48);});
      for (uint32_t i=0;i<300;++i) {
        if (i%4==3) { if (cleared[i].status) throw std::runtime_error("Inactive ReSTIR history was overwritten"); continue; }
        const uint32_t status=expected[i]==0 ? 3 : 2;
        if (cleared[i].status!=status || cleared[i].identity[0]!=i || cleared[i].identity[1]!=2 || cleared[i].identity[3]!=7)
          throw std::runtime_error("Hardware ReSTIR canonical result/identity mismatch");
      }
    }
  }
  n=301; graph.bufferWrite(count,0,4,&n);
  if (worklist) { activeState[3]=301; graph.bufferWrite(state,0,16,activeState); }
  RenderGraph::Frame frame; RenderGraph::Overrides overrides; graph.run(frame,overrides); graph.waitFrame(frame);
  std::vector<uint32_t> overflow(300);
  graph.bufferRead(readback,0,1200,[&](const void *p){std::memcpy(overflow.data(),p,1200);});
  if (std::any_of(overflow.begin(),overflow.end(),[](uint32_t v){return v!=2u;})) throw std::runtime_error("Screen-first overflow silently dropped rays");
}

static void testResidentVoxelScene(backend::vulkan::VulkanRHI &rhi)
{
  using namespace voxel::svraster;
  RenderGraph graph(&rhi,RenderGraph::Settings{.maxFramesInFlight=2u});
  SparseVoxelScene authored;
  SparseVoxelBLAS model;
  model.rootExtent=2; model.maxLevel=1; model.rootCount=1;
  SparseVoxelLODNodeGPU leaf; leaf.voxelIndex=0; leaf.level=1;
  model.hierarchyNodes={leaf}; model.octreePaths={0}; model.octreeLevels={1};
  model.centersLengths={SparseVoxelCenterLengthGPU{{-.5f,-.5f,-.5f},1}};
  SparseVoxelMaterialGPU material; material.albedoRGBA8=0xffffffffu; model.materials={material};
  authored.models={model}; SparseVoxelInstance instance;
  instance.modelToWorld={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; authored.instances={instance};
  SparseVoxelSceneManager manager; std::string error;
  if (!manager.setScene(authored,&error)) throw std::runtime_error(error);
  SparseVoxelHardwareRayTraceScene scene(graph,manager,3,2);
  if (scene.triangleCount()!=12) throw std::runtime_error("Resident voxel geometry is not one closed leaf box");
  auto buffer=[&](const char *name,uint64_t size,bool read=false) {
    return graph.createBuffer({.name=name,.size=size,.usage=read ? BufferUsage_Pull|BufferUsage_CopyDst : BufferUsage_Storage|BufferUsage_Push});
  };
  SparseVoxelSceneRayGPU rays[3];
  for (uint32_t i=0;i<2;++i) {
    rays[i].originMinimumDistance={i==0 ? -.5f : 2.5f,-.5f,2,.001f};
    rays[i].directionMaximumDistance={0,0,-1,5}; rays[i].seedFlags={i,3,1,0};
  }
  // A receiver reconstructed one micron inside its own front face must not
  // shadow itself. The other ray still hits the actual box farther away.
  rays[2].originMinimumDistance={-.5f,-.5f,-.000001f,0};
  rays[2].directionMaximumDistance={0,0,1,5}; rays[2].seedFlags={2,3u|(5u<<4u),1,0};
  auto rb=buffer("voxelRestirRays",sizeof(rays)); graph.bufferWrite(rb,0,sizeof(rays),rays);
  // World-only mode binds one unused screen record, regardless of ray count.
  auto hits=buffer("voxelRestirScreen",48); auto count=buffer("voxelRestirCount",4);
  uint32_t n=3; graph.bufferWrite(count,0,4,&n);
  using HardwarePass=virtualgeometry::gpgpu::VirtualGeometryScreenFirstShadowPass;
  auto *pass=graph.registerPass<HardwarePass>("VoxelRestir",0,scene.backend(),HardwarePass::Inputs{
      rb,hits,count,scene.instances(),scene.instanceCount(),{},{},{},false,.0001f});
  auto read=buffer("voxelRestirRead",12,true);
  graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadVoxelRestir",4,pass->getVisibilityBuffer(),0,12,read,0,12);
  graph.compile();
  for (uint32_t i=0;i<6;++i) {
    if (i==3) {
      auto moved=instance.modelToWorld; moved[12]=3;
      if (!manager.setInstanceTransform(0,moved,&error)) throw std::runtime_error(error);
    }
    graph.setCurrentFrameIndex(i%2); scene.update(manager);
    RenderGraph::Frame frame; frame.frameIndex=i%2; RenderGraph::Overrides overrides;
    graph.run(frame,overrides); graph.waitFrame(frame);
    std::array<uint32_t,3> result;
    graph.bufferRead(read,0,12,[&](const void *p){std::memcpy(result.data(),p,12);});
    if (i==0 && result==std::array<uint32_t,3>{2,2,2}) continue; // asynchronous installation.
    const std::array<uint32_t,3> expected=i<3 ? std::array<uint32_t,3>{1,0,0} : std::array<uint32_t,3>{0,1,0};
    if (result!=expected) throw std::runtime_error("Cached voxel BLAS failed moving-instance shadow visibility");
    const auto timing=pass->completion(i%2);
    if (!timing.completed || !timing.succeeded || timing.statistics.rays!=3 ||
        timing.statistics.instances!=1 || timing.statistics.invalidScene || timing.gpuMilliseconds<=0)
      throw std::runtime_error("Native ReSTIR completion timing/statistics are invalid");
  }
  if (scene.backend()->cacheStatistics().builds!=1) throw std::runtime_error("Moving instance rebuilt immutable BLAS");
}

static void testCitySharedHardwareScene(backend::vulkan::VulkanRHI &rhi)
{
  using namespace virtualgeometry;
  namespace svr = voxel::svraster;
  svr::CityVoxelAssetCompiler compiler;
  svr::CityVoxelCompiledPlacement first, second;
  std::string error;
  if (!compiler.submit(1,100000,{{{{0,0,0}},0xff0000ffu,10u}},first,&error) ||
      !compiler.submit(2,100000,{{{{0,0,0}},0xff00ff00u,11u}},second,&error))
    throw std::runtime_error(error);
  CityVoxelStaticScene city;
  if (!city.load({{first.geometryId,"city-shared",0}},
      {first.appearanceId,second.appearanceId},{{{0,0,0},0}},
      {{{0,0,0},{20,20,20},0,2}},
      {{0x100000001ull,0,0,0,{0,0,0}},
       {0x200000002ull,0,1,0,{1,0,0}}},100000,&error))
    throw std::runtime_error(error);
  RenderGraph graph(&rhi,RenderGraph::Settings{.maxFramesInFlight=2u});
  CityVoxelHardwareRayTraceScene scene(graph,city,compiler.geometry(),compiler.appearances(),2,2);
  if (!scene.backend()->capabilities().available) return;
  if (scene.triangleCount()!=0u || scene.residentGeometryCount()!=0u)
    throw std::runtime_error("City hardware scene eagerly built nonresident geometry");
  scene.publishResidency({1u},{1u,1u});
  if (scene.coverageComplete())
    throw std::runtime_error("Pending city BLAS was published as complete coverage");
  if (scene.triangleCount()!=12u) throw std::runtime_error("Shared city geometry duplicated its BLAS");
  if (scene.residentGeometryCount()!=1u)
    throw std::runtime_error("City geometry sharing created more than one BLAS input");
  VirtualGeometryMetalRayTraceBackend::Frame build;
  build.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::BuildOnly;
  build.slot=1;
  if (!scene.backend()->waitForTesting(scene.backend()->submit(build)).succeeded)
    throw std::runtime_error("City BLAS build failed");
  graph.setCurrentFrameIndex(0);
  scene.update();
  if (!scene.coverageComplete() || scene.activeRayInstances()!=2u)
    throw std::runtime_error("Ready city BLAS did not publish both placements");
  auto buffer=[&](const char *name,uint64_t bytes) {
    return graph.createBuffer({.name=name,.size=bytes,
        .usage=BufferUsage_Storage|BufferUsage_Push|BufferUsage_CopySrc,
        .metalExportable=true});
  };
  auto rays=buffer("cityRayParityRays",2*sizeof(ShadowRay));
  auto rayCount=buffer("cityRayParityCount",4);
  auto results=buffer("cityRayParityResults",2*sizeof(ClosestHitResult));
  ShadowRay input[2];
  for (uint32_t i=0;i<2;++i) {
    input[i].origin[0]=i ? .15f : .05f;
    input[i].origin[1]=.05f; input[i].origin[2]=1.0f;
    input[i].direction[0]=0; input[i].direction[1]=0; input[i].direction[2]=-1;
    input[i].maxDistance=2;
  }
  uint32_t two=2;
  graph.bufferWrite(rays,0,sizeof(input),input);
  graph.bufferWrite(rayCount,0,4,&two);
  VirtualGeometryMetalRayTraceBackend::Frame frame;
  frame.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::TlasAndTrace;
  frame.query=VirtualGeometryMetalRayTraceBackend::Frame::Query::ClosestHit;
  if (!scene.bindTraceFrame(frame,&error))
    throw std::runtime_error("Ready city cut refused native frame: "+error);
  frame.rays=rhi.exportMetalBuffer(graph.resolveBufferId(rays));
  frame.rayCount=rhi.exportMetalBuffer(graph.resolveBufferId(rayCount));
  frame.results=rhi.exportMetalBuffer(graph.resolveBufferId(results));
  frame.slot=0;
  const auto completed=scene.backend()->waitForTesting(scene.backend()->submit(frame));
  if (!completed.succeeded || completed.statistics.instances!=2)
    throw std::runtime_error("City TLAS or closest-hit query failed");
  std::array<ClosestHitResult,2> hits;
  graph.bufferRead(results,0,sizeof(hits),[&](const void *data){std::memcpy(hits.data(),data,sizeof(hits));});
  for (uint32_t i=0;i<2;++i) {
    CityVoxelHardwareRayTraceScene::ResolvedHit resolved;
    if (!scene.resolveClosestHit(hits[i],resolved) ||
        resolved.stableId!=(i ? 0x200000002ull : 0x100000001ull) ||
        resolved.rgba!=(i ? 0xff00ff00u : 0xff0000ffu) ||
        resolved.materialProfile!=(i ? 11u : 10u) ||
        resolved.worldNormal!=std::array<float,3>{0,0,1})
      throw std::runtime_error("City hardware hit identity or appearance differs from raster");
  }
  if (scene.backend()->cacheStatistics().builds!=1)
    throw std::runtime_error("City appearance variant built duplicate BLAS");
  scene.publishResidency({0u},{0u,0u});
  if (scene.residentGeometryCount()!=0u || scene.triangleCount()!=0u ||
      scene.residentInputBytes()!=0u)
    throw std::runtime_error("City geometry eviction retained the unused ray input");
  const auto path=std::filesystem::temp_directory_path()/"city-hardware-ray-stream-test.cvscene";
  if (!writeCityVoxelCompiledScene(path.string(),city,compiler.geometry(),
      compiler.appearances(),{{10u,0u,101u},{11u,0u,102u}},11u,22u,{},&error))
    throw std::runtime_error(error);
  CityVoxelSceneRuntime runtime;
  CityVoxelSceneRuntime::Budgets budgets;
  if (!runtime.open(path.string(),budgets,{},&error) ||
      !runtime.updateRadius(1u,{0.1,0.1,0.1},3.0,&error))
    throw std::runtime_error(error);
  if (runtime.statistics().detailedCells!=1u)
    throw std::runtime_error("City streamed ray cut was not resident");
  CityVoxelHardwareRayTraceScene streamedScene(graph,city,runtime,2u,2u);
  streamedScene.publishFromRuntime(runtime);
  if (streamedScene.residentGeometryCount()!=1u || streamedScene.triangleCount()!=12u ||
      streamedScene.backend()->cacheStatistics().builds!=0u ||
      streamedScene.coverageComplete())
    throw std::runtime_error("City trace page did not install as one shared BLAS");
  if (!streamedScene.backend()->waitForTesting(streamedScene.backend()->submit(build)).succeeded)
    throw std::runtime_error("Streamed city BLAS build failed");
  streamedScene.update();
  if (!streamedScene.coverageComplete() || streamedScene.activeRayInstances()!=2u)
    throw std::runtime_error("Streamed city ray cut was not complete after BLAS build");
  if (!streamedScene.bindTraceFrame(frame,&error))
    throw std::runtime_error("Ready streamed city cut refused native frame: "+error);
  if (!streamedScene.backend()->waitForTesting(streamedScene.backend()->submit(frame)).succeeded)
    throw std::runtime_error("Streamed city TLAS and trace failed");
  graph.bufferRead(results,0,sizeof(hits),[&](const void *data){std::memcpy(hits.data(),data,sizeof(hits));});
  for (uint32_t i=0;i<2;++i) {
    CityVoxelHardwareRayTraceScene::ResolvedHit resolved;
    if (!streamedScene.resolveClosestHit(hits[i],resolved) ||
        resolved.rgba!=(i ? 0xff00ff00u : 0xff0000ffu))
      throw std::runtime_error("File-backed city ray appearance parity failed");
  }
  if (!runtime.updateRadius(2u,{100,0,0},1.0,&error))
    throw std::runtime_error(error);
  streamedScene.publishFromRuntime(runtime);
  if (streamedScene.residentGeometryCount()!=0u ||
      streamedScene.residentInputBytes()!=0u)
    throw std::runtime_error("City streamed ray cut failed to evict BLAS");
  std::filesystem::remove(path);
}

static void testCityGroupedHardwareScene(backend::vulkan::VulkanRHI &rhi)
{
  using namespace virtualgeometry;
  namespace svr=voxel::svraster;
  svr::CityVoxelAssetCompiler compiler;
  std::vector<svr::CityVoxelCell> red,green;
  for (int x=0;x<700;++x) {
    red.push_back({{{x*2,0,0}},0xff0000ffu,10u});
    green.push_back({{{1500+x*2,0,0}},0xff00ff00u,11u});
  }
  svr::CityVoxelCompiledPlacement first,second;
  std::string error;
  if (!compiler.submit(1,100000,red,first,&error) ||
      !compiler.submit(2,100000,green,second,&error) ||
      compiler.geometry().size()!=1u)
    throw std::runtime_error("Grouped city geometry did not canonicalize: "+error);
  CityVoxelStaticScene city;
  if (!city.load({{first.geometryId,"grouped-city",0}},
      {first.appearanceId,second.appearanceId},{{{0,0,0},0}},
      {{{0,0,0},{3000,10,10},0,2}},
      {{0x100000001ull,0,0,0,{0,0,0}},
       {0x200000002ull,0,1,0,{1500,0,0}}},100000,&error))
    throw std::runtime_error(error);
  const auto path=std::filesystem::temp_directory_path()/"city-hardware-ray-groups.cvscene";
  if (!writeCityVoxelCompiledScene(path.string(),city,compiler.geometry(),
      compiler.appearances(),{{10u,0u,101u},{11u,0u,102u}},33u,44u,{},&error))
    throw std::runtime_error(error);
  CityVoxelSceneRuntime runtime;
  if (!runtime.open(path.string(),{}, {},&error) ||
      runtime.traceGroupCount(0)!=2u ||
      !runtime.updateRadius(1u,{0,0,0},500.0,&error) ||
      runtime.statistics().detailedCells!=1u)
    throw std::runtime_error("Grouped city pages failed to stream: "+error);
  CityVoxelStaticScene fileCity;
  if (!fileCity.loadSceneIndex(runtime.index(),&error) ||
      fileCity.placements().size()!=2u ||
      std::abs(fileCity.worldPosition(fileCity.placements()[1]).x()-150.0f)>0.0001f)
    throw std::runtime_error("Grouped city placements failed path-only reconstruction: "+error);
  RenderGraph graph(&rhi,RenderGraph::Settings{.maxFramesInFlight=2u});
  CityVoxelHardwareRayBudgets budget;
  budget.maximumInstallsPerUpdate=1u;
  CityVoxelHardwareRayTraceScene scene(graph,fileCity,runtime,3u,2u,budget);
  if (!scene.backend()->capabilities().available) return;
  scene.publishFromRuntime(runtime);
  if (scene.residentGroupCount()!=1u || scene.coverageComplete())
    throw std::runtime_error("Grouped city ray scene exceeded one install per update");
  VirtualGeometryMetalRayTraceBackend::Frame incomplete;
  if (scene.bindTraceFrame(incomplete,&error))
    throw std::runtime_error("Incomplete grouped city cut accepted native trace frame");
  VirtualGeometryMetalRayTraceBackend::Frame build;
  build.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::BuildOnly;
  build.slot=0u;
  if (!scene.backend()->waitForTesting(scene.backend()->submit(build)).succeeded)
    throw std::runtime_error("First grouped city BLAS build failed");
  scene.publishFromRuntime(runtime);
  if (scene.residentGroupCount()!=2u || scene.coverageComplete())
    throw std::runtime_error("Grouped city ray cut promoted before both BLAS builds");
  build.slot=1u;
  if (!scene.backend()->waitForTesting(scene.backend()->submit(build)).succeeded)
    throw std::runtime_error("Second grouped city BLAS build failed");
  scene.update();
  if (!scene.coverageComplete() || scene.activeRayInstances()!=4u ||
      scene.backend()->cacheStatistics().builds!=2u)
    throw std::runtime_error("Grouped city cut failed to share two BLASes across placements");
  auto buffer=[&](const char *name,uint64_t bytes) {
    return graph.createBuffer({.name=name,.size=bytes,
        .usage=BufferUsage_Storage|BufferUsage_Push|BufferUsage_CopySrc,
        .metalExportable=true});
  };
  auto rays=buffer("groupedCityRays",3*sizeof(ShadowRay));
  auto rayCount=buffer("groupedCityRayCount",4u);
  auto results=buffer("groupedCityResults",3*sizeof(ClosestHitResult));
  ShadowRay input[3];
  for (uint32_t i=0;i<3u;++i) {
    input[i].origin[0]=i==0 ? 0.05f : i==1 ? 139.85f : 150.05f;
    input[i].origin[1]=0.05f;input[i].origin[2]=1.0f;
    input[i].direction[2]=-1.0f;input[i].maxDistance=2.0f;
  }
  uint32_t three=3u;
  graph.bufferWrite(rays,0,sizeof(input),input);
  graph.bufferWrite(rayCount,0,sizeof(three),&three);
  VirtualGeometryMetalRayTraceBackend::Frame trace;
  trace.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::TlasAndTrace;
  trace.query=VirtualGeometryMetalRayTraceBackend::Frame::Query::ClosestHit;
  if (!scene.bindTraceFrame(trace,&error))
    throw std::runtime_error("Ready grouped city cut refused native frame: "+error);
  trace.rays=rhi.exportMetalBuffer(graph.resolveBufferId(rays));
  trace.rayCount=rhi.exportMetalBuffer(graph.resolveBufferId(rayCount));
  trace.results=rhi.exportMetalBuffer(graph.resolveBufferId(results));
  trace.slot=0u;
  if (!scene.backend()->waitForTesting(scene.backend()->submit(trace)).succeeded)
    throw std::runtime_error("Grouped city closest-hit trace failed");
  std::array<ClosestHitResult,3> hits;
  graph.bufferRead(results,0,sizeof(hits),[&](const void *data){
    std::memcpy(hits.data(),data,sizeof(hits));});
  for (uint32_t i=0;i<3u;++i) {
    CityVoxelHardwareRayTraceScene::ResolvedHit resolved;
    if (!scene.resolveClosestHit(hits[i],resolved) ||
        resolved.rgba!=(i==2u ? 0xff00ff00u : 0xff0000ffu) ||
        resolved.materialProfile!=(i==2u ? 11u : 10u) ||
        resolved.placementIndex!=(i==2u ? 1u : 0u))
      throw std::runtime_error("Grouped city hit lost exact per-instance appearance");
  }
  if (!runtime.updateRadius(2u,{1000,0,0},1.0,&error))
    throw std::runtime_error(error);
  scene.publishFromRuntime(runtime);
  if (scene.residentGroupCount()!=0u || scene.residentInputBytes()!=0u)
    throw std::runtime_error("Grouped city BLAS inputs did not evict");
  std::filesystem::remove(path);
}

int main()
{
  try
  {
    VirtualGeometryMetalRayTraceBackend::Configuration config;
    config.metallibPath = VG_RT_METALLIB_PATH;
    config.maxInstances = 1; config.maxRays = 2; config.frameSlots = 2;
    VirtualGeometryMetalRayTraceBackend probe(config);
    if (!probe.capabilities().available) return 77;
    os::Logger::start();
    backend::vulkan::VulkanRHI rhi(backend::vulkan::Vulkan_1_2, {}, DeviceFeatures::DeviceFeatures_Compute, {});
    std::vector<VkSurfaceKHR> surfaces;
    rhi.init(surfaces);
    if (!rhi.exportMetalDevice()) return 77;
    testCitySharedHardwareScene(rhi);
    testCityGroupedHardwareScene(rhi);
    RenderGraph graph(&rhi, RenderGraph::Settings{.maxFramesInFlight=2});
    auto rt = std::make_shared<VirtualGeometryMetalRayTraceBackend>(config,rhi.exportMetalDevice());
    auto makeBuffer = [&](const char *name,uint64_t size,bool readback=false) {
      return graph.createBuffer(BufferInfo{.name=name,.size=size,
          .usage=readback ? (BufferUsage::BufferUsage_Pull|BufferUsage::BufferUsage_CopyDst) :
                           (BufferUsage::BufferUsage_Storage|BufferUsage::BufferUsage_CopySrc|BufferUsage::BufferUsage_Push),
          .metalExportable=true});
    };
    auto view = [](Buffer buffer,uint64_t size) { return BufferView{.buffer=buffer,.offset=0,.size=size}; };
    auto vertices=makeBuffer("rtGraphVertices",36);

    auto rays=makeBuffer("rtGraphRays",2*sizeof(ShadowRay));
    auto rayCount=makeBuffer("rtGraphRayCount",4);
    auto results=makeBuffer("rtGraphResults",8);
    auto readback=makeBuffer("rtGraphReadback",8,true);
    const float triangle[9]={-1,-1,-4,1,-1,-4,0,1,-4};
    graph.bufferWrite(vertices,0,sizeof(triangle),const_cast<float *>(triangle));
    VirtualGeometryMetalRayTraceBackend::Geometry geometry;
    geometry.key={1,1,0,0}; geometry.vertexBuffer=rhi.exportMetalBuffer(graph.resolveBufferId(vertices));
    geometry.vertexStride=12; geometry.vertexCount=3; geometry.triangleCount=1;
    RayTraceInstance instance{};
    instance.transform[0]=instance.transform[4]=instance.transform[8]=1;
    instance.geometryHandle=rt->cacheGeometry(geometry); instance.mask=255;
    using namespace virtualgeometry;
    auto upload=[&](const char *name,auto &value) {
      auto buffer=makeBuffer(name,sizeof(value)); graph.bufferWrite(buffer,0,sizeof(value),&value); return buffer;
    };
    VisibleClusterInfo visibleData{};
    uint32_t cutCounters[33]{}; cutCounters[2]=1;
    PageTableEntry pageData{}; pageData.isInstalled=1; pageData.clusterCount=1;
    uint32_t handleData[2]={instance.geometryHandle,0};
    VirtualGeometryInstanceGPUData instanceData{}; instanceData.modelMatrix=math::Mat4f::identity();
    VirtualGeometryHierarchy hierarchyData{}; hierarchyData.meshPartIndex=UINT32_MAX;
    DecodedClusterCullData clusterData{};
    clusterData.localMinX=-1; clusterData.localMinY=-1; clusterData.localMinZ=-4;
    clusterData.localMaxX=1; clusterData.localMaxY=1; clusterData.localMaxZ=-4;
    virtualgeometry::gpgpu::VirtualGeometryRayTraceCutPass::Inputs cutInputs{
      upload("visible",visibleData),upload("cutCounters",cutCounters),upload("pages",pageData),
      upload("handles",handleData),upload("sceneInstances",instanceData),upload("hierarchy",hierarchyData),upload("clusters",clusterData)};
    auto *cut=graph.registerPass<virtualgeometry::gpgpu::VirtualGeometryRayTraceCutPass>("RayTraceCut",0,cutInputs,1u);
    auto instances=cut->getInstancesBuffer(); auto instanceCount=cut->getInstanceCountBuffer();
    std::array<ShadowRay,2> rayData{};
    for (auto &ray:rayData) { ray.direction[2]=-1; ray.maxDistance=10; ray.mask=255; }
    rayData[1].origin[0]=3;
    graph.bufferWrite(rays,0,sizeof(rayData),rayData.data());
    uint32_t one=1,two=2;
    graph.bufferWrite(rayCount,0,4,&two);
    virtualgeometry::gpgpu::VirtualGeometryHardwareRayTracePass::Inputs inputs;
    inputs.instances=view(instances,sizeof(instance)); inputs.instanceCount=view(instanceCount,4);
    inputs.rays=view(rays,sizeof(rayData)); inputs.rayCount=view(rayCount,4); inputs.results=view(results,8);
    inputs.geometryBuildInputs={view(vertices,sizeof(triangle))};
    graph.registerPass<virtualgeometry::gpgpu::VirtualGeometryHardwareRayTracePass>("HardwareShadow",1,rt,inputs);
    graph.registerPass<rendering::gpgpu::CopyBufferPass>("ReadShadow",2,results,0,8,readback,0,8);
    graph.compile();
    for (uint32_t frameIndex=0;frameIndex<4;++frameIndex)
    {
      RenderGraph::Frame frame; frame.frameIndex=frameIndex%2; RenderGraph::Overrides overrides;
      graph.run(frame,overrides); graph.waitFrame(frame);
      std::array<uint32_t,2> visibility{};
      graph.bufferRead(readback,0,8,[&](const void *data){std::memcpy(visibility.data(),data,8);});
      if (visibility!=std::array<uint32_t,2>{1,0}) throw std::runtime_error("Native graph shadow result mismatch");
    }
    // Every incomplete/invalid cut must propagate InvalidScene, never an empty lit scene.
    for (uint32_t mode=0;mode<5;++mode) {
      cutCounters[2]=mode==0 ? 2u : 1u;
      cutCounters[32]=mode==1 ? 1u : 0u;
      cutCounters[0]=mode==2 ? 1u : 0u;
      handleData[1]=mode==3 ? 99u : 0u;
      pageData.isInstalled=mode==4 ? 0u : 1u;
      graph.bufferWrite(cutInputs.counters,0,sizeof(cutCounters),cutCounters);
      graph.bufferWrite(cutInputs.handles,0,sizeof(handleData),handleData);
      graph.bufferWrite(cutInputs.pages,0,sizeof(pageData),&pageData);
      RenderGraph::Frame frame; RenderGraph::Overrides overrides;
      graph.run(frame,overrides); graph.waitFrame(frame);
      std::array<uint32_t,2> visibility{};
      graph.bufferRead(readback,0,8,[&](const void *data){std::memcpy(visibility.data(),data,8);});
      if (visibility!=std::array<uint32_t,2>{2,2}) throw std::runtime_error("Incomplete scene cut was accepted by native trace");
    }
    if (rt->cacheStatistics().builds!=1) throw std::runtime_error("Native graph rebuilt warm BLAS");
    for (uint32_t type=0;type<3;++type) { testDepthPages(rhi,true,type); testDepthPages(rhi,false,type); }
    testScreenFirst(rhi);
    testScreenFirst(rhi,true);
    testResidentVoxelScene(rhi);
    std::cout << "PASS: MoltenVK -> native Metal BLAS/TLAS/trace -> graph copy, four frames, one BLAS build\n";
    return 0;
  }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
