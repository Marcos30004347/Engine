#pragma once
#include "virtualgeometry/rendering/VirtualGeometryHardwareRayTracePass.hpp"
#include "os/File.hpp"
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
namespace virtualgeometry::gpgpu {
// Takes current results from SparseVoxelSceneScreenTracePass. A confirmed
// blocker finishes an opaque shadow ray; every other active shadow ray goes
// through stable GPU compaction and a full original world segment.
class VirtualGeometryScreenFirstShadowPass final : public rendering::Pass {
  using Backend=raytracing::VirtualGeometryMetalRayTraceBackend;
  struct State {
    std::vector<rendering::Buffer> buffers;
    rendering::BindingsLayout layout; rendering::BindingGroups groups; rendering::Shader shader;
    uint32_t blocks=0;
  };
  class Stage final : public rendering::Pass {
    std::shared_ptr<State> state_; bool merge_; std::vector<rendering::ComputePipeline> pipelines_;
  public:
    Stage(std::shared_ptr<State> s,bool merge):state_(std::move(s)),merge_(merge) {}
    ~Stage() override { for (auto p:pipelines_) renderGraph->deleteComputePipeline(p); }
    void recordCommandBuffer(rendering::CommandRecorder &cmd) override {
      using namespace rendering;
      auto dispatch=[&](const char *entry,uint32_t blocks) {
        auto p=renderGraph->createComputePipeline(ComputePipelineInfo{.entry=entry,.name=passName+entry,.layout=state_->layout,.shader=state_->shader});
        pipelines_.push_back(p); cmd.cmdBindComputePipeline(p); cmd.cmdBindBindingGroups(state_->groups,nullptr,0); cmd.cmdDispatch(blocks,1,1);
        cmd.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER,PipelineStage::COMPUTE_SHADER,AccessPattern::SHADER_WRITE,AccessPattern::SHADER_READ_WRITE);
      };
      if (merge_) dispatch("merge_visibility",state_->blocks);
      else { dispatch("rank_unresolved",state_->blocks); dispatch("prefix_blocks",1); dispatch("compact_rays",state_->blocks); }
    }
  };
public:
  struct Inputs {
    rendering::Buffer sceneRays,screenHits,rayCount,instances,instanceCount;
    // Optional current-frame worklist: activeRayState contains dispatch XYZ and
    // count at word 3. Results are scattered to canonical IDs for ReSTIR.
    rendering::Buffer activeRayIndices,activeRayState,mergedHits;
    bool screenEnabled=true;
    // Optional offset for raster receivers, whose face is encoded in ray flags.
    // Zero leaves generic/VG rays unchanged. Distances remain world-space.
    float receiverBias=0.0f;
  };
  VirtualGeometryScreenFirstShadowPass(std::shared_ptr<Backend> backend,Inputs inputs):backend_(std::move(backend)),inputs_(inputs) {
    if (!backend_ || !backend_->capabilities().available) throw std::invalid_argument("Screen-first hardware shadows require Metal");
  }
  ~VirtualGeometryScreenFirstShadowPass() override {
    for (auto *child:children_) delete child;
    if (!state_) return;
    for (uint32_t i:{0u,4u,5u,6u,7u,8u,9u}) renderGraph->deleteBuffer(state_->buffers[i]);
    if (!inputs_.mergedHits.isValid()) renderGraph->deleteBuffer(state_->buffers[11]);
    renderGraph->deleteShader(state_->shader); renderGraph->deleteBindingGroups(state_->groups); renderGraph->deleteBindingsLayout(state_->layout);
  }
  const rendering::Buffer &getVisibilityBuffer() const { return state_->buffers[9]; }
  const rendering::Buffer &getWorldRayCountBuffer() const { return state_->buffers[7]; }
  Backend::Completion completion(uint32_t frameSlot) { return trace_->completion(frameSlot); }
  void recordCommandBuffer(rendering::CommandRecorder &) override {
    using namespace rendering;
    const auto &limits=backend_->configuration(); const uint32_t capacity=limits.maxRays;
    const bool worklist=inputs_.activeRayIndices.isValid();
    if (worklist!=inputs_.activeRayState.isValid()) throw std::invalid_argument("Hardware shadows require both worklist buffers");
    auto source=worklist ? inputs_.activeRayState : inputs_.rayCount;
    for (const auto &pair:std::vector<std::pair<Buffer,uint64_t>>{{inputs_.sceneRays,uint64_t(capacity)*80},{inputs_.screenHits,inputs_.screenEnabled ? uint64_t(capacity)*48 : 48u},
        {source,worklist ? 16u : 4u},{inputs_.instances,uint64_t(limits.maxInstances)*56},{inputs_.instanceCount,4}})
      if (!pair.first.isValid() || renderGraph->getBufferSize(pair.first)<pair.second)
        throw std::invalid_argument("Screen-first inputs do not cover backend capacities");
    if (worklist && renderGraph->getBufferSize(inputs_.activeRayIndices)<uint64_t(capacity)*4)
      throw std::invalid_argument("Hardware shadow worklist is too small");
    if (inputs_.mergedHits.isValid() && renderGraph->getBufferSize(inputs_.mergedHits)<uint64_t(capacity)*48)
      throw std::invalid_argument("Hardware shadow merged results are too small");
    state_=std::make_shared<State>(); state_->blocks=(capacity+255)/256;
    auto create=[&](const char *name,uint64_t size,BufferUsage usage,bool native=false) {
      return createFrameLocalBuffer(BufferInfo{.name=passName+name,.size=size,.usage=usage,.metalExportable=native});
    };
    // This configuration is immutable. A frame-local allocation would leave
    // newly recorded frame slots with zero settings until explicitly uploaded.
    auto config=renderGraph->createBuffer(BufferInfo{.name=passName+".Uniforms",.size=16,
        .usage=BufferUsage_Uniform|BufferUsage_Push,.frameLocal=false});
    state_->buffers={config,inputs_.sceneRays,inputs_.screenHits,source,
      create(".Ranks",uint64_t(capacity)*4,BufferUsage_Storage),create(".Blocks",uint64_t(state_->blocks)*4,BufferUsage_Storage),
      create(".WorldRays",uint64_t(capacity)*48,BufferUsage_Storage,true),create(".WorldCount",4,BufferUsage_Storage|BufferUsage_CopySrc,true),
      create(".WorldResults",uint64_t(capacity)*4,BufferUsage_Storage,true),create(".Visibility",uint64_t(capacity)*4,BufferUsage_Storage|BufferUsage_CopySrc),
      worklist ? inputs_.activeRayIndices : source,
      inputs_.mergedHits.isValid() ? inputs_.mergedHits : create(".UnusedMergedHits",4,BufferUsage_Storage)};
    uint32_t data[4]={capacity,state_->blocks,uint32_t(worklist)|(inputs_.screenEnabled ? 2u : 0u)|(inputs_.mergedHits.isValid() ? 4u : 0u),0};
    std::memcpy(&data[3],&inputs_.receiverBias,4);
    renderGraph->bufferWrite(state_->buffers[0],0,16,data);
    BindingGroupLayout layout; GroupInfo group; group.name=passName+".Bindings";
    for (uint32_t i=0;i<state_->buffers.size();++i) {
      layout.buffers.push_back({.name="buffer"+std::to_string(i),.binding=i,
        .type=i==0 ? BufferBindingType_UniformBuffer : BufferBindingType_StorageBuffer,.visibility=BindingVisibility_Compute});
      group.buffers.push_back({.binding=i,.bufferView={.buffer=state_->buffers[i],
        .access=i>=4 && i!=8 && i!=10 ? AccessPattern::SHADER_READ_WRITE : AccessPattern::SHADER_READ,.offset=0,.size=renderGraph->getBufferSize(state_->buffers[i])}});
    }
    state_->layout=renderGraph->createBindingsLayout(BindingsLayoutInfo{.name=passName+".Layout",.groups={layout}});
    state_->groups=renderGraph->createBindingGroups(BindingGroupsInfo{.name=passName+".Groups",.layout=state_->layout,.groups={group}});
    state_->shader=renderGraph->createShader(ShaderInfo{.name=passName+".Shader",.src=os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-screen-first-shadow.spirv"),.layout=state_->layout,.type=ShaderType::SpirV});
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Compact",getIndex()+1,state_,false));
    auto view=[&](Buffer b){return BufferView{.buffer=b,.offset=0,.size=renderGraph->getBufferSize(b)};};
    VirtualGeometryHardwareRayTracePass::Inputs trace;
    trace.instances=view(inputs_.instances); trace.instanceCount=view(inputs_.instanceCount);
    trace.rays=view(state_->buffers[6]); trace.rayCount=view(state_->buffers[7]); trace.results=view(state_->buffers[8]); trace.buildPendingGeometry=false;
    trace_=renderGraph->registerPass<VirtualGeometryHardwareRayTracePass>(passName+".WorldTrace",getIndex()+2,backend_,trace);
    children_.push_back(trace_);
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Merge",getIndex()+3,state_,true));
  }
private:
  std::shared_ptr<Backend> backend_; Inputs inputs_; std::shared_ptr<State> state_; std::vector<rendering::Pass *> children_;
  VirtualGeometryHardwareRayTracePass *trace_=nullptr;
};
}
#endif
