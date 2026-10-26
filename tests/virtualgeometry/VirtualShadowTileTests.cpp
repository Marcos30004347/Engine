#include "virtualshadowmap/rendering/VirtualShadowMapBookkeepingPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDebugPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDrawPass.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "os/Logger.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>
using namespace rendering;
using namespace virtualgeometry;
static void check(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
struct Result { std::vector<uint32_t> tiles,states,counters,vpt; std::vector<double> gpuMs; };
struct BenchmarkConfig { uint32_t width=1280,height=720,locals=200,frames=80; };
static Result run(backend::vulkan::VulkanRHI &rhi, bool tilesEnabled, float cutoff, bool empty=false, uint32_t budget=UINT32_MAX, bool drawEmpty=false, float texelTarget=0, bool bvh=true, bool many=false, bool move=false,const BenchmarkConfig *benchmark=nullptr) {
  RenderGraph::Settings graphSettings; graphSettings.enableAutomaticNodeTimers=benchmark!=nullptr;
  RenderGraph graph(&rhi,graphSettings);
  VirtualShadowMapManager::Settings config;
  config.virtualPageTableResolution=4; config.physicalPageSize=16; config.physicalAtlasResolution=drawEmpty ? 512 : 256;
  config.cascadeCount=2; config.maxDirectionalLights=32; config.maxPointLights=2; config.maxSpotLights=many ? 32u : 0u;
  config.firstCascadeWorldExtent=16; config.maxAllocationRequestsPerFrame=2048;
  config.shadowDistance={cutoff,cutoff}; config.maxPageUpdatesPerFrame=budget; config.targetShadowTexelPixels=texelTarget;
  const uint32_t width=benchmark ? benchmark->width : 32u, height=benchmark ? benchmark->height : 16u;
  if (benchmark) {
    config.virtualPageTableResolution=1; config.cascadeCount=1; config.maxSpotLights=benchmark->locals;
    config.physicalAtlasResolution=512;
  }
  VirtualShadowMapManager manager(&graph,config);
  for (uint32_t i=0;i<32;++i) manager.createDirectionalLight(math::Vec3f(0,-1,-1));
  manager.createPointLight(math::Vec3f(0,0,-4),2);
  manager.createPointLight(math::Vec3f(100,0,-4),2);
  if (many) for (uint32_t i=0;i<(benchmark ? benchmark->locals : 32u);++i)
    manager.createSpotLight(math::Vec3f(float(i%8)*8.0f-24.0f,benchmark ? float(i/8)*8.0f : 0.0f,-4),math::Vec3f(0,0,-1),.7f,3.0f);
  Camera camera(1.04719755f,math::Vec3f(0,0,0),math::Vec3f(0,0,-1),true);
  camera.setAspectRatio(2); camera.setNearFar(.1f,100); camera.updateMatrices(); manager.update(camera);
  const auto buildCount=manager.getLightBVHBuildCount();
  manager.update(camera);
  manager.setLightColor(0,math::Vec3f(.5f)); manager.update(camera);
  check(manager.getLightBVHBuildCount()==buildCount,"Unchanged light bounds rebuilt BVH");
  if (move) {
    manager.setPointLightPosition(33,math::Vec3f(0,0,-4)); manager.update(camera);
    check(manager.getLightBVHBuildCount()==buildCount+1,"Moved light failed to rebuild BVH");
  }
  TextureInfo depthInfo{}; depthInfo.name="depth"; depthInfo.width=width; depthInfo.height=height;
  depthInfo.mipLevels=1; depthInfo.format=Format_Depth32Float; depthInfo.usage=ImageUsage_Sampled|ImageUsage_TransferDst;
  depthInfo.initialLayout=ResourceLayout::SHADER_READ_ONLY;
  auto depth=graph.createTexture(depthInfo);
  auto projected=camera.getProjectionMatrix()*math::Vec4f(0,0,-4,1);
  std::vector<float> depthData(width*height,empty ? 0.0f : projected[2]/projected[3]);
  graph.textureWrite(depth,depthData.size()*4,depthData.data());
  TextureInfo outputInfo{}; outputInfo.name="debug"; outputInfo.width=width; outputInfo.height=height;
  outputInfo.mipLevels=1; outputInfo.format=Format_RGBA16Float; outputInfo.usage=ImageUsage_Storage|ImageUsage_Sampled;
  auto output=graph.createTexture(outputInfo);
  virtualgeometry::gpgpu::VirtualShadowMapBookkeepingPass::Settings settings;
  settings.depthTextureWidth=width; settings.depthTextureHeight=height; settings.enableLightTiles=tilesEnabled; settings.enableLightBVH=bvh; settings.lightBVHMinLocalLights=0;
  auto *pass=graph.registerPass<virtualgeometry::gpgpu::VirtualShadowMapBookkeepingPass>("bookkeeping",0,manager,depth,output,settings);
  std::unique_ptr<VirtualGeometryScene> scene;
  if (drawEmpty) {
    VirtualGeometryScene::Settings geometry; geometry.instanceCapacity=1;
    geometry.decodedVertexCapacity=3; geometry.decodedIndexCapacity=3; geometry.decodedBoneWeightCapacity=1;
    scene=std::make_unique<VirtualGeometryScene>(&graph,4096,65536,geometry);
    virtualgeometry::gpgpu::VirtualShadowMapDrawPass::Settings draw;
    draw.hierarchyQueueCapacity=64; draw.clusterQueueCapacity=64; draw.maxVisibleClusterDraws=64; draw.maxHierarchyLevels=1;
    graph.registerPass<virtualgeometry::gpgpu::VirtualShadowMapDrawPass>("drawEmpty",1,*scene,manager,draw);
  }
  std::vector<Buffer> staging;
  auto copy=[&](Buffer src,uint64_t size) {
    auto buffer=graph.createBuffer(BufferInfo{.name="read"+std::to_string(staging.size()),.size=size,
        .usage=BufferUsage_CopyDst|BufferUsage_Pull});
    graph.registerPass<rendering::gpgpu::CopyBufferPass>("copy"+std::to_string(staging.size()),2+staging.size(),src,0,size,buffer,0,size);
    staging.push_back(buffer);
  };
  copy(pass->getLightTilesBuffer(),16+uint64_t((width+15)/16)*((height+15)/16)*((manager.getActiveLightCount()+31)/32)*4);
  copy(manager.getVirtualPageStateBuffer(),manager.getVirtualPageStateBufferSize());
  copy(manager.getAllocatorCountersBuffer(),manager.getAllocatorCountersBufferSize());
  copy(manager.getVirtualPageTableBuffer(),manager.getVirtualPageTableBufferSize());
  // Compile the actual final-lighting binding layout too, with the same tile list.
  outputInfo.name="screenShadows"; outputInfo.arrayLayers=32; auto screen=graph.createTexture(outputInfo); outputInfo.arrayLayers=1;
  outputInfo.name="lighting"; auto lighting=graph.createTexture(outputInfo);
  virtualgeometry::gpgpu::VirtualShadowMapShadowPcfPass::Settings resolve;
  resolve.width=width; resolve.height=height; resolve.screenSpaceShadowEnabled=false; resolve.normalLightingEnabled=false;
  graph.registerPass<virtualgeometry::gpgpu::VirtualShadowMapShadowPcfPass>("resolve",6,manager,depth,screen,nullptr,output,lighting,resolve,pass->getLightTilesBuffer());
  graph.compile();
  RenderGraph::Frame frame; RenderGraph::Overrides overrides;
  manager.appendFrameOverrides(overrides,0);
  if (scene) scene->appendFrameOverrides(overrides,0);
  graph.run(frame,overrides); graph.waitFrame(frame);
  if (drawEmpty) {
    for (uint32_t warm=0;warm<4;++warm) {
      manager.update(camera);
      graph.run(frame,overrides); graph.waitFrame(frame);
    }
  }
  Result result;
  if (benchmark) {
    for (uint32_t sample=0;sample<benchmark->frames+20;++sample) {
      manager.update(camera); graph.run(frame,overrides); graph.waitFrame(frame);
      double ms=0;
      for (const auto &timer:graph.readResolvedTimerRecords(0)) {
        if (timer.passName.find("_LightTiles.pipeline")!=std::string::npos) ms+=timer.gpuTimeMs;
      }
      if (sample>=20) result.gpuMs.push_back(ms);
    }
  }
  std::vector<uint32_t> *values[]={&result.tiles,&result.states,&result.counters,&result.vpt};
  for (uint32_t i=0;i<4;++i) { auto &v=*values[i]; v.resize(graph.getBufferSize(staging[i])/4);
    graph.bufferRead(staging[i],0,v.size()*4,[&](const void *p){std::memcpy(v.data(),p,v.size()*4);}); }
  return result;
}
int main(int argc,char **argv) {
  try {
    os::Logger::start(); backend::vulkan::VulkanRHI rhi(backend::vulkan::Vulkan_1_2,{},DeviceFeatures_Compute,{});
    std::vector<VkSurfaceKHR> surfaces; rhi.init(surfaces);
    if (argc>1 && std::string(argv[1])=="--benchmark") {
      BenchmarkConfig config;
      for (uint32_t repeat=0;repeat<4;++repeat) for (uint32_t mode=0;mode<2;++mode) {
        const bool bvh=((mode+repeat)%2)==0;
        auto result=run(rhi,true,100,false,64,false,0,bvh,true,false,&config);
        auto sorted=result.gpuMs; std::sort(sorted.begin(),sorted.end());
        if (sorted.empty() || sorted.back()<=0) throw std::runtime_error("Missing GPU benchmark timestamps");
        std::cout<<"BENCH repeat="<<repeat<<" bvh="<<bvh<<" width="<<config.width<<" height="<<config.height
          <<" local_lights="<<config.locals+2<<" global_lights=32 p50_ms="<<sorted[sorted.size()/2]
          <<" p95_ms="<<sorted[sorted.size()*95/100]<<" samples_ms=";
        for (double ms:result.gpuMs) std::cout<<std::setprecision(9)<<ms<<",";
        std::cout<<"\n";
      }
      return 0;
    }
    VirtualShadowMapManager::Settings quality;
    quality.matchDirectionalResolution(1080,1.04719755f,1,{800,1000});
    check(quality.cascadeCount==10 && quality.targetShadowTexelPixels==1 && quality.cameraRelativeDirectionalDepth,
          "Resolution preset did not derive a complete level ladder");
    { RenderGraph graph(&rhi); auto invalid=quality; invalid.maxDirectionalLights=26;
      bool rejected=false; try { VirtualShadowMapManager manager(&graph,invalid); } catch (const std::invalid_argument &) { rejected=true; }
      check(rejected,"Layer ownership overflow was accepted"); }
    auto tiled=run(rhi,true,100); auto full=run(rhi,false,100);
    check(tiled.tiles[3]==1 && tiled.tiles[2]==2,"Tile header invalid");
    for (uint32_t tile=0;tile<2;++tile) {
      check(tiled.tiles[4+tile*2]==0xffffffffu,"Directional lights lost at word boundary");
      check(tiled.tiles[5+tile*2]==1,"Local influence culling failed");
    }
    check(tiled.states==full.states,"Tile culling changed requested virtual pages");
    auto linear=run(rhi,true,100,false,UINT32_MAX,false,0,false);
    check(linear.tiles==tiled.tiles && linear.states==tiled.states,"BVH changed tile lists or page demand");
    auto treeMany=run(rhi,true,100,false,UINT32_MAX,false,0,true,true);
    auto linearMany=run(rhi,true,100,false,UINT32_MAX,false,0,false,true);
    check(treeMany.tiles==linearMany.tiles && treeMany.states==linearMany.states,"Mixed BVH traversal lost lights");
    auto movedTree=run(rhi,true,100,false,UINT32_MAX,false,0,true,true,true);
    auto movedLinear=run(rhi,true,100,false,UINT32_MAX,false,0,false,true,true);
    check(movedTree.tiles==movedLinear.tiles && movedTree.states==movedLinear.states,"Moving light BVH is stale");
    check(movedTree.tiles!=treeMany.tiles,"Moved point light did not enter tile list");
    auto footprint=run(rhi,true,100,false,UINT32_MAX,false,1);
    check(footprint.states==tiled.states,"Shared footprint selector changed finest-plane coverage");
    check(tiled.counters[0]+tiled.counters[1]>0,"Visible scene requested no pages");
    auto bounded=run(rhi,true,100,false,3); auto repeated=run(rhi,true,100,false,3);
    check(bounded.counters[0]+bounded.counters[1]==3,"Exact page quota was not honored");
    check(bounded.counters[0]==0,"Fine requests displaced coarse coverage");
    check(bounded.states==repeated.states,"Page admission is not deterministic");
    auto paused=run(rhi,true,100,false,0);
    check(paused.counters[0]+paused.counters[1]==0,"Zero page budget schedules work");
    auto emptyCasters=run(rhi,true,100,false,UINT32_MAX,true);
    check(emptyCasters.counters[0]+emptyCasters.counters[1]==0,"Completed empty scene still schedules shadow pages");
    for (uint32_t i=0;i<emptyCasters.vpt.size();++i) {
      auto entry=emptyCasters.vpt[i];
      if ((entry&4u)!=0u && (entry&1u)!=0u) {
        std::cerr<<"dirty "<<i<<" state "<<emptyCasters.states[i]<<" counts "<<emptyCasters.counters[0]<<","<<emptyCasters.counters[1]<<"\n";
        check(false,"Empty scheduled page did not become valid");
      }
    }
    auto disabled=run(rhi,true,0);
    check(disabled.counters[0]==0 && disabled.counters[1]==0,"Disabled shadows still request pages");
    auto empty=run(rhi,true,100,true);
    check(empty.tiles[4]==0 && empty.tiles[5]==0 && empty.tiles[6]==0 && empty.tiles[7]==0,"Empty tile retains lights");
    std::cout<<"PASS: GPU tile bitsets, 32-light boundary, page-demand parity, shadow cutoff, empty depth, resolve bindings\n";
  } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
