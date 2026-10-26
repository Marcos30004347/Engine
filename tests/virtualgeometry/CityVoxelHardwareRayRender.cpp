#include "virtualgeometry/CityVoxelHardwareRayTraceScene.hpp"
#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "atmosphere/AtmosphereReference.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "os/Logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace virtualgeometry;
using namespace virtualgeometry::raytracing;
using namespace rendering;

namespace {
using V3=std::array<float,3>;
V3 add(V3 a,V3 b) {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
V3 multiply(V3 a,float s) {return {a[0]*s,a[1]*s,a[2]*s};}
V3 subtract(V3 a,V3 b) {return add(a,multiply(b,-1.0f));}
float dot(V3 a,V3 b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
float shadeSrgb(uint8_t component,float irradiance) {
  const float srgb=float(component)/255.0f;
  const float linear=srgb<=0.04045f?srgb/12.92f:
      std::pow((srgb+0.055f)/1.055f,2.4f);
  const float shaded=std::clamp(linear*irradiance,0.0f,1.0f);
  return 255.0f*(shaded<=0.0031308f?12.92f*shaded:
      1.055f*std::pow(shaded,1.0f/2.4f)-0.055f);
}
V3 cross(V3 a,V3 b) {
  return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
V3 unit(V3 a) {
  const float length=std::sqrt(dot(a,a));
  if (!(length>0.0f)) throw std::invalid_argument("Camera direction is zero");
  return multiply(a,1.0f/length);
}
struct ReferenceSky {
  atmosphere::AtmosphereProfile profile=atmosphere::earthAtmosphereProfile();
  V3 sun{};
  std::array<atmosphere::Vec3d,201> sunTransmittance{};
  atmosphere::AtmosphereAmbientSH1 ambient{};
  explicit ReferenceSky(V3 towardSun):sun(towardSun) {
    profile.atmosphereRadianceScale=8.0f;
    for(unsigned i=0;i<sunTransmittance.size();++i)
      sunTransmittance[i]=atmosphere::referenceTransmittanceToAtmosphereTop(
          profile,profile.groundRadiusMetres+double(i)*500.0,
          std::max(0.01,double(sun[1])),16u);
    std::vector<atmosphere::Vec3d> directions,radiances;
    constexpr unsigned samples=128;
    directions.reserve(samples);radiances.reserve(samples);
    for(unsigned i=0;i<samples;++i) {
      const double y=1.0-2.0*(double(i)+.5)/samples;
      const double azimuth=double(i)*2.39996322972865332;
      const double r=std::sqrt(std::max(0.0,1.0-y*y));
      V3 direction{float(r*std::cos(azimuth)),float(y),float(r*std::sin(azimuth))};
      directions.push_back({direction[0],direction[1],direction[2]});
      radiances.push_back(radiance(direction,50.0));
    }
    ambient=atmosphere::projectAtmosphereAmbientSH1(directions,radiances);
  }
  atmosphere::Vec3d radiance(V3 direction,double altitude) const {
    const double radius=profile.groundRadiusMetres+std::max(0.0,altitude);
    const atmosphere::Vec3d origin{0.0,radius,0.0};
    const atmosphere::Vec3d view{direction[0],direction[1],direction[2]};
    double distance=atmosphere::distanceToAtmosphereTop(profile,radius,view[1]);
    const auto ground=atmosphere::intersectRaySphere(origin,view,profile.groundRadiusMetres);
    if(ground.hit && ground.nearDistance>0.0)
      distance=std::min(distance,ground.nearDistance);
    if(!(distance>0.0))return {0.0,0.0,0.0};
    const double mu=std::clamp(double(dot(direction,sun)),-1.0,1.0);
    constexpr double pi=3.14159265358979323846;
    const double rayleighPhase=3.0*(1.0+mu*mu)/(16.0*pi);
    const double g=profile.mieAnisotropy;
    const double miePhase=(1.0-g*g)/(4.0*pi*std::pow(1.0+g*g-2.0*g*mu,1.5));
    atmosphere::Vec3d light{},trans{1.0,1.0,1.0};
    constexpr unsigned steps=24;
    double previous=0.0;
    for(unsigned step=0;step<steps;++step) {
      const double u=double(step+1)/steps;
      const double end=distance*u*u;
      const double mid=(previous+end)*.5;
      const double height=std::sqrt(radius*radius+mid*mid+
          2.0*radius*mid*view[1])-profile.groundRadiusMetres;
      const auto density=atmosphere::atmosphereDensity(profile,height);
      const auto extinction=atmosphere::atmosphereExtinctionPerMetre(profile,height);
      const size_t altitudeIndex=std::min<size_t>(sunTransmittance.size()-1,
          size_t(std::max(0.0,height)/500.0));
      const double length=end-previous;
      for(unsigned channel=0;channel<3;++channel) {
        const double scattering=double(profile.rayleighScatteringPerMetre[channel])*
                density.rayleigh*rayleighPhase+
            double(profile.mieScatteringPerMetre[channel])*density.mie*miePhase;
        const double extinctionStep=std::exp(-extinction[channel]*length);
        const double integral=extinction[channel]>1e-12?
            (1.0-extinctionStep)/extinction[channel]:length;
        light[channel]+=trans[channel]*sunTransmittance[altitudeIndex][channel]*
            scattering*integral*profile.atmosphereRadianceScale;
        trans[channel]*=extinctionStep;
      }
      previous=end;
    }
    return light;
  }
};
uint8_t displayLinear(double value) {
  const double mapped=1.0-std::exp(-std::max(0.0,value)*2.0);
  const double encoded=mapped<=0.0031308?12.92*mapped:
      1.055*std::pow(mapped,1.0/2.4)-0.055;
  return uint8_t(std::clamp(encoded*255.0,0.0,255.0));
}
void savePPM(const char *path,uint32_t width,uint32_t height,
    const std::vector<uint8_t> &rgb) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out<<"P6\n"<<width<<' '<<height<<"\n255\n";
  out.write(reinterpret_cast<const char *>(rgb.data()),std::streamsize(rgb.size()));
  if(!out)throw std::runtime_error("Cannot write ray render PPM");
}
}

int main(int argc,char **argv) {
  try {
    if(argc!=11 && argc!=12) {
      std::cerr<<"Usage: CityVoxelHardwareRayRender scene.cvscene output.ppm width height "
          "eyeX eyeY eyeZ targetX targetY targetZ [streamRadiusMetres]\n";
      return 2;
    }
    const uint32_t width=uint32_t(std::stoul(argv[3]));
    const uint32_t height=uint32_t(std::stoul(argv[4]));
    if(!width || !height || uint64_t(width)*height>2'000'000u)
      throw std::invalid_argument("Ray render resolution is invalid");
    const V3 eye{std::stof(argv[5]),std::stof(argv[6]),std::stof(argv[7])};
    const V3 target{std::stof(argv[8]),std::stof(argv[9]),std::stof(argv[10])};
    const float streamRadius=argc==12?std::stof(argv[11]):0.0f;
    if(argc==12 && !(streamRadius>0.0f))
      throw std::invalid_argument("Stream radius must be positive");
    const V3 forward=unit(subtract(target,eye));
    const V3 right=unit(cross(forward,{0,1,0}));
    const V3 up=cross(right,forward);
    const V3 sun=unit({0.20f,0.35f,-0.92f});
    const ReferenceSky sky(sun);
    uint32_t pixels=width*height;
    os::Logger::start();
    backend::vulkan::VulkanRHI rhi(backend::vulkan::Vulkan_1_2,{},
        DeviceFeatures::DeviceFeatures_Compute,{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi.init(surfaces);
    if(!rhi.exportMetalDevice())throw std::runtime_error("Metal export is unavailable");
    CityVoxelSceneRuntime runtime;
    std::string error;
    CityVoxelSceneRuntime::Budgets residency;
    residency.requireVirtualGeometry=false; // This diagnostic consumes trace groups.
    if(!runtime.open(argv[1],residency,{},&error))throw std::runtime_error(error);
    const uint64_t maximumFrames=2u+runtime.index().pages.size()/
        std::max(1u,runtime.budgets().maximumReadsPerFrame);
    for(uint64_t frame=1;frame<=maximumFrames;++frame) {
      if(!runtime.update(frame,[&](uint32_t,const CityVoxelSceneRuntime::Bounds &bounds){
            if(!streamRadius)return true;
            float distance2=0.0f;
            for(unsigned axis=0;axis<3;++axis) {
              const float closest=std::clamp(eye[axis],float(bounds.minimum[axis]),
                  float(bounds.maximum[axis]));
              const float delta=eye[axis]-closest;
              distance2+=delta*delta;
            }
            return distance2<=streamRadius*streamRadius;
          },&error))
        throw std::runtime_error(error);
      if(runtime.statistics().fallbackCells==0u)break;
    }
    if(runtime.statistics().fallbackCells)
      throw std::runtime_error("Ray render scene did not fit the file residency budgets");
    CityVoxelStaticScene city;
    if(!city.loadSceneIndex(runtime.index(),&error))throw std::runtime_error(error);
    RenderGraph graph(&rhi,RenderGraph::Settings{.maxFramesInFlight=2u});
    graph.setCurrentFrameIndex(0u);
    CityVoxelHardwareRayTraceScene scene(graph,city,runtime,pixels,2u);
    if(!scene.backend()->capabilities().available)
      throw std::runtime_error(scene.backend()->capabilities().reason);
    uint32_t totalGroups=0;
    for(uint32_t g=0;g<runtime.index().geometry.size();++g)
      totalGroups+=runtime.traceGroupCount(g);
    for(uint32_t attempt=0;attempt<totalGroups+2u;++attempt) {
      scene.publishFromRuntime(runtime);
      if(scene.coverageComplete())break;
      VirtualGeometryMetalRayTraceBackend::Frame build;
      build.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::BuildOnly;
      build.slot=attempt%2u;
      const auto done=scene.backend()->waitForTesting(scene.backend()->submit(build));
      if(!done.succeeded)throw std::runtime_error("BLAS build: "+done.error);
      scene.update();
      if(scene.coverageComplete())break;
    }
    if(!scene.coverageComplete())
      throw std::runtime_error("Ray render never reached complete BLAS coverage");
    auto buffer=[&](const char *name,uint64_t bytes) {
      return graph.createBuffer({.name=name,.size=bytes,
          .usage=BufferUsage_Storage|BufferUsage_Push|BufferUsage_CopySrc,
          .metalExportable=true});
    };
    auto raysBuffer=buffer("cityRayRenderRays",uint64_t(pixels)*sizeof(ShadowRay));
    auto countBuffer=buffer("cityRayRenderCount",sizeof(uint32_t));
    auto hitBuffer=buffer("cityRayRenderHits",uint64_t(pixels)*sizeof(ClosestHitResult));
    std::vector<ShadowRay> rays(pixels);
    const float tanHalfHorizontal=std::tan(91.5f*0.5f*0.0174532925199433f);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
      const uint32_t index=y*width+x;
      const float sx=(2.0f*(float(x)+0.5f)/float(width)-1.0f)*tanHalfHorizontal;
      const float sy=(1.0f-2.0f*(float(y)+0.5f)/float(height))*
          tanHalfHorizontal*float(height)/float(width);
      const V3 direction=unit(add(forward,add(multiply(right,sx),multiply(up,sy))));
      for(unsigned axis=0;axis<3;++axis) {
        rays[index].origin[axis]=eye[axis];
        rays[index].direction[axis]=direction[axis];
      }
      rays[index].maxDistance=streamRadius?streamRadius:1000.0f;
    }
    graph.bufferWrite(raysBuffer,0,rays.size()*sizeof(rays[0]),rays.data());
    graph.bufferWrite(countBuffer,0,sizeof(pixels),&pixels);
    VirtualGeometryMetalRayTraceBackend::Frame frame;
    frame.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::TlasAndTrace;
    frame.query=VirtualGeometryMetalRayTraceBackend::Frame::Query::ClosestHit;
    if(!scene.bindTraceFrame(frame,&error))throw std::runtime_error(error);
    frame.rays=rhi.exportMetalBuffer(graph.resolveBufferId(raysBuffer));
    frame.rayCount=rhi.exportMetalBuffer(graph.resolveBufferId(countBuffer));
    frame.results=rhi.exportMetalBuffer(graph.resolveBufferId(hitBuffer));
    frame.slot=0u;
    const auto first=scene.backend()->waitForTesting(scene.backend()->submit(frame));
    if(!first.succeeded || first.statistics.invalidScene)
      throw std::runtime_error("Closest-hit trace failed: "+first.error);
    std::vector<ClosestHitResult> hits(pixels);
    graph.bufferRead(hitBuffer,0,hits.size()*sizeof(hits[0]),
        [&](const void *data){std::memcpy(hits.data(),data,hits.size()*sizeof(hits[0]));});
    std::vector<CityVoxelHardwareRayTraceScene::ResolvedHit> resolved(pixels);
    std::vector<ShadowRay> shadows;
    std::vector<uint32_t> shadowPixel;
    for(uint32_t i=0;i<pixels;++i) {
      if(hits[i].state!=1u)continue;
      if(!scene.resolveClosestHit(hits[i],resolved[i]))
        throw std::runtime_error("Hardware hit has no exact city voxel appearance");
      const V3 normal=resolved[i].worldNormal;
      if(dot(normal,sun)<=0.0f)continue;
      ShadowRay shadow;
      const V3 point=add(eye,multiply({rays[i].direction[0],rays[i].direction[1],
          rays[i].direction[2]},hits[i].distance));
      for(unsigned axis=0;axis<3;++axis) {
        shadow.origin[axis]=point[axis]+normal[axis]*0.02f;
        shadow.direction[axis]=sun[axis];
      }
      shadow.minDistance=0.02f;
      shadow.maxDistance=streamRadius?streamRadius:1000.0f;
      shadowPixel.push_back(i);
      shadows.push_back(shadow);
    }
    std::vector<uint32_t> shadowResults(shadows.size());
    if(!shadows.empty()) {
      auto shadowRays=buffer("cityRayRenderShadowRays",uint64_t(pixels)*sizeof(ShadowRay));
      auto shadowCount=buffer("cityRayRenderShadowCount",sizeof(uint32_t));
      auto shadowOut=buffer("cityRayRenderShadowOut",uint64_t(pixels)*sizeof(uint32_t));
      uint32_t count=uint32_t(shadows.size());
      graph.bufferWrite(shadowRays,0,shadows.size()*sizeof(ShadowRay),shadows.data());
      graph.bufferWrite(shadowCount,0,sizeof(count),&count);
      VirtualGeometryMetalRayTraceBackend::Frame shadowFrame;
      shadowFrame.work=VirtualGeometryMetalRayTraceBackend::Frame::Work::TlasAndTrace;
      shadowFrame.query=VirtualGeometryMetalRayTraceBackend::Frame::Query::AnyHit;
      if(!scene.bindTraceFrame(shadowFrame,&error))throw std::runtime_error(error);
      shadowFrame.rays=rhi.exportMetalBuffer(graph.resolveBufferId(shadowRays));
      shadowFrame.rayCount=rhi.exportMetalBuffer(graph.resolveBufferId(shadowCount));
      shadowFrame.results=rhi.exportMetalBuffer(graph.resolveBufferId(shadowOut));
      shadowFrame.slot=1u;
      const auto second=scene.backend()->waitForTesting(scene.backend()->submit(shadowFrame));
      if(!second.succeeded || second.statistics.invalidScene)
        throw std::runtime_error("Sun shadow trace failed: "+second.error);
      graph.bufferRead(shadowOut,0,shadowResults.size()*sizeof(uint32_t),
          [&](const void *data){std::memcpy(shadowResults.data(),data,
              shadowResults.size()*sizeof(uint32_t));});
    }
    std::vector<uint8_t> shadowed(pixels,0u);
    uint32_t occluded=0;
    for(size_t i=0;i<shadowResults.size();++i) {
      if(shadowResults[i]==uint32_t(ShadowResult::InvalidScene))
        throw std::runtime_error("Sun shadow ray encountered incomplete scene");
      shadowed[shadowPixel[i]]=shadowResults[i]==uint32_t(ShadowResult::Occluded);
      occluded+=shadowed[shadowPixel[i]];
    }
    std::vector<uint8_t> rgb(uint64_t(pixels)*3u);
    uint32_t hitCount=0;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
      const uint32_t i=y*width+x;
      const uint64_t at=uint64_t(i)*3u;
      if(hits[i].state!=1u) {
        const auto color=sky.radiance({rays[i].direction[0],rays[i].direction[1],
            rays[i].direction[2]},std::max(0.0f,eye[1]));
        for(unsigned channel=0;channel<3;++channel)
          rgb[at+channel]=displayLinear(color[channel]);
        continue;
      }
      ++hitCount;
      const auto &surface=resolved[i];
      const float diffuse=std::max(0.0f,dot(surface.worldNormal,sun));
      const auto ambient=atmosphere::evaluateAtmosphereAmbientSH1(sky.ambient,
          {surface.worldNormal[0],surface.worldNormal[1],surface.worldNormal[2]});
      const uint32_t color=surface.rgba;
      for(unsigned channel=0;channel<3;++channel) {
        const float skyLight=std::clamp(float(ambient[channel])*1.5f,0.12f,0.55f);
        const float direct=shadowed[i]?0.0f:0.72f*diffuse;
        rgb[at+channel]=uint8_t(shadeSrgb(uint8_t((color>>(channel*8u))&255u),
            skyLight+direct));
      }
    }
    savePPM(argv[2],width,height,rgb);
    if(std::getenv("CITY_VOXEL_DEBUG_RENDER")) {
      std::vector<uint8_t> normals(rgb.size(),0u),visibility(rgb.size(),0u);
      for(uint32_t i=0;i<pixels;++i) {
        if(hits[i].state!=1u)continue;
        for(unsigned axis=0;axis<3;++axis)
          normals[uint64_t(i)*3u+axis]=uint8_t(std::clamp(
              (resolved[i].worldNormal[axis]*.5f+.5f)*255.0f,0.0f,255.0f));
        const uint8_t value=shadowed[i]?0u:255u;
        for(unsigned axis=0;axis<3;++axis)visibility[uint64_t(i)*3u+axis]=value;
      }
      savePPM((std::string(argv[2])+".normals.ppm").c_str(),width,height,normals);
      savePPM((std::string(argv[2])+".visibility.ppm").c_str(),width,height,visibility);
      for(uint32_t y=height/2u-20u;y<height/2u+10u;++y) {
        const uint32_t x=width*5u/8u,i=y*width+x;
        if(hits[i].state!=1u)continue;
        const V3 p=add(eye,multiply({rays[i].direction[0],rays[i].direction[1],
            rays[i].direction[2]},hits[i].distance));
        std::cout<<"probe "<<x<<","<<y<<" p="<<p[0]<<","<<p[1]<<","<<p[2]
            <<" n="<<resolved[i].worldNormal[0]<<","<<resolved[i].worldNormal[1]
            <<","<<resolved[i].worldNormal[2]<<" owner="<<resolved[i].stableId
            <<" shadow="<<int(shadowed[i])<<"\n";
      }
    }
    std::cout<<"Ray render: "<<pixels<<" primary rays, "<<hitCount<<" hits, "
        <<shadows.size()<<" sun rays, "<<occluded<<" occluded, "
        <<scene.residentGroupCount()<<" shared BLAS groups, "
        <<scene.activeRayInstances()<<" TLAS group instances\n";
    return 0;
  } catch(const std::exception &exception) {
    std::cerr<<exception.what()<<'\n';
    return 1;
  }
}
