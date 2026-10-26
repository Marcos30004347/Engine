#pragma once
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/raytracing/VirtualGeometryRayTraceTypes.hpp"
#include "os/File.hpp"

namespace virtualgeometry::gpgpu {
// Converts the renderer's independent scene cut, not the camera visibility cut.
// The native backend used with these outputs must have exactly the same capacity.
class VirtualGeometryRayTraceCutPass final : public rendering::Pass {
public:
  struct Inputs {
    rendering::Buffer visible, counters, pages, handles, instances, hierarchy, clusters;
  };
  VirtualGeometryRayTraceCutPass(Inputs inputs,uint32_t capacity)
      : inputs_(inputs),capacity_(capacity) {
    if (!capacity || capacity==UINT32_MAX) throw std::invalid_argument("Invalid RT cut capacity");
    for (const auto &buffer:{inputs.visible,inputs.counters,inputs.pages,inputs.handles,inputs.instances,inputs.hierarchy,inputs.clusters})
      if (!buffer.isValid()) throw std::invalid_argument("RT cut requires installed BLAS manifests and complete scene inputs");
  }
  VirtualGeometryRayTraceCutPass(VirtualGeometryScene &scene,VirtualGeometryCullingMultipleDispatchesPass &cut,uint32_t capacity)
      : VirtualGeometryRayTraceCutPass(Inputs{cut.getHWVisibleClusterInfosBuffer(),cut.getCullingStatisticsBuffer(),
          scene.pageTableBuffer,scene.rayTraceClusterHandlesBuffer,scene.instanceBuffer,scene.hierarchyBuffer,scene.decodedClusterCullBuffer},capacity) {
    if (!cut.settings.sceneCut) throw std::invalid_argument("RT requires an independent scene cut");
  }
  ~VirtualGeometryRayTraceCutPass() override {
    for (const auto &b:{uniforms_,instances_,count_,bounds_,dispatch_}) renderGraph->deleteBuffer(b);
    renderGraph->deleteComputePipeline(prepare_); renderGraph->deleteComputePipeline(convert_);
    renderGraph->deleteBindingGroups(groups_); renderGraph->deleteShader(shader_); renderGraph->deleteBindingsLayout(layout_);
  }
  const rendering::Buffer &getInstancesBuffer() const { return instances_; }
  const rendering::Buffer &getInstanceCountBuffer() const { return count_; }
  const rendering::Buffer &getCasterBoundsBuffer() const { return bounds_; }
  uint32_t getCapacity() const { return capacity_; }
  void recordCommandBuffer(rendering::CommandRecorder &cmd) override {
    using namespace rendering;
    auto create=[&](const char *name,uint64_t size,BufferUsage usage,bool native=false) {
      return createFrameLocalBuffer(BufferInfo{.name=passName+name,.size=size,.usage=usage,.metalExportable=native});
    };
    // Capacity is immutable. A one-time write to a frame-local uniform leaves
    // the other slot at zero and invalidates every second terrain scene cut.
    uniforms_=renderGraph->createBuffer(BufferInfo{.name=passName+".Uniforms",.size=16,
        .usage=BufferUsage_Uniform|BufferUsage_Push,.frameLocal=false});
    uint32_t config[4]={capacity_,0,0,0}; renderGraph->bufferWrite(uniforms_,0,16,config);
    instances_=create(".Instances",uint64_t(capacity_)*sizeof(raytracing::RayTraceInstance),BufferUsage_Storage|BufferUsage_CopySrc,true);
    count_=create(".Count",4,BufferUsage_Storage|BufferUsage_CopySrc,true);
    bounds_=create(".Bounds",uint64_t(capacity_)*sizeof(raytracing::RayTraceBounds),BufferUsage_Storage|BufferUsage_CopySrc,true);
    // Indirect arguments, then a cut stamp and four stamps per scene instance
    // (whole-page BLAS dedup, one per page of a kit geometry). An instance
    // record is 176 bytes, so one word per 32 bytes covers four each.
    dispatch_=create(".Dispatch",16+renderGraph->getBufferSize(inputs_.instances)/32*4,BufferUsage_Storage|BufferUsage_Indirect);
    BindingGroupLayout group; GroupInfo bindings; bindings.name=passName+".Bindings";
    std::vector<Buffer> buffers={uniforms_,inputs_.visible,inputs_.counters,inputs_.pages,inputs_.handles,
        inputs_.instances,inputs_.hierarchy,inputs_.clusters,instances_,count_,bounds_,dispatch_};
    if (renderGraph->getBufferSize(inputs_.counters)<33*4) throw std::invalid_argument("RT cut requires overflow-aware culling counters");
    for (uint32_t i=0;i<buffers.size();++i) {
      group.buffers.push_back({.name="buffer"+std::to_string(i),.binding=i,
          .type=i==0 ? BufferBindingType_UniformBuffer : BufferBindingType_StorageBuffer,.visibility=BindingVisibility_Compute});
      bindings.buffers.push_back({.binding=i,.bufferView={.buffer=buffers[i],
          .access=i>=8 ? AccessPattern::SHADER_READ_WRITE : AccessPattern::SHADER_READ,.offset=0,.size=renderGraph->getBufferSize(buffers[i])}});
    }
    layout_=renderGraph->createBindingsLayout(BindingsLayoutInfo{.name=passName+".Layout",.groups={group}});
    groups_=renderGraph->createBindingGroups(BindingGroupsInfo{.layout=layout_,.name=passName+".Groups",.groups={bindings}});
    shader_=renderGraph->createShader(ShaderInfo{.name=passName+".Shader",.layout=layout_,
        .src=os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-raytrace-cut.spirv"),.type=ShaderType::SpirV});
    prepare_=renderGraph->createComputePipeline(ComputePipelineInfo{.entry="prepare_cut",.layout=layout_,.name=passName+".Prepare",.shader=shader_});
    convert_=renderGraph->createComputePipeline(ComputePipelineInfo{.entry="convert_cut",.layout=layout_,.name=passName+".Convert",.shader=shader_});
    cmd.cmdBindComputePipeline(prepare_); cmd.cmdBindBindingGroups(groups_,nullptr,0); cmd.cmdDispatch(1,1,1);
    cmd.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER,PipelineStage::ALL_COMMANDS,
        AccessPattern::SHADER_WRITE,AccessPattern::SHADER_READ_WRITE|AccessPattern::INDIRECT_COMMAND_READ);
    cmd.cmdBindComputePipeline(convert_);
    cmd.cmdDispatchIndirect(dispatch_,0);
  }
private:
  Inputs inputs_; uint32_t capacity_;
  rendering::Buffer uniforms_,instances_,count_,bounds_,dispatch_;
  rendering::BindingsLayout layout_; rendering::BindingGroups groups_; rendering::Shader shader_;
  rendering::ComputePipeline prepare_,convert_;
};
}
