#pragma once
#include "virtualshadowmap/VirtualShadowMapManager.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareRayTracePass.hpp"
#include "os/File.hpp"
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
namespace virtualgeometry::gpgpu {
// Opaque hard-light page producer for a complete AS-ready scene cut. The final
// depth-only page blit preserves the existing sampled depth-atlas ABI; no caster
// rasterization, raster culling, or hierarchical page bounds are executed.
class VirtualShadowMapRayTraceDepthPass final : public rendering::Pass {
  using Backend=raytracing::VirtualGeometryMetalRayTraceBackend;
  struct State {
    VirtualShadowMapManager *manager=nullptr;
    std::vector<rendering::Buffer> buffers;
    rendering::BindingsLayout layout;
    rendering::BindingGroups groups;
    rendering::Shader compute,publish;
    uint32_t capacity=0;
  };
  class Stage final : public rendering::Pass {
    std::shared_ptr<State> state_; uint32_t stage_;
    std::vector<rendering::ComputePipeline> pipelines_;
    rendering::GraphicsPipeline graphics_;
  public:
    Stage(std::shared_ptr<State> state,uint32_t stage):state_(std::move(state)),stage_(stage) {}
    ~Stage() override {
      for (auto p:pipelines_) renderGraph->deleteComputePipeline(p);
      if (stage_==2) renderGraph->deleteGraphicsPipeline(graphics_);
    }
    void recordCommandBuffer(rendering::CommandRecorder &cmd) override {
      using namespace rendering;
      auto kernel=[&](const char *entry,bool indirect) {
        auto p=renderGraph->createComputePipeline(ComputePipelineInfo{.entry=entry,.layout=state_->layout,.name=passName+entry,.shader=state_->compute});
        pipelines_.push_back(p); cmd.cmdBindComputePipeline(p); cmd.cmdBindBindingGroups(state_->groups,nullptr,0);
        if (indirect) cmd.cmdDispatchIndirect(state_->buffers[13],0);
        else cmd.cmdDispatch(stage_==3 ? (state_->capacity+63)/64 : 1,1,1);
      };
      if (stage_==0) {
        kernel("schedule_pages",false);
        cmd.cmdMemoryBarrier(PipelineStage::COMPUTE_SHADER,PipelineStage::ALL_COMMANDS,
            AccessPattern::SHADER_WRITE,AccessPattern::SHADER_READ_WRITE|AccessPattern::INDIRECT_COMMAND_READ);
        kernel("generate_rays",true);
      } else if (stage_==1) kernel("resolve_hits",true);
      else if (stage_==3) kernel("finish_pages",false);
      else {
        GraphicsPipelineInfo p{}; p.name=passName+".DepthPageBlit"; p.layout=state_->layout;
        p.vertexStage.vertexShader=state_->publish; p.vertexStage.shaderEntry="publish_vs";
        p.vertexStage.cullType=CullMode::None; p.vertexStage.primitiveType=PrimitiveType_Triangles;
        p.fragmentStage.fragmentShader=state_->publish; p.fragmentStage.shaderEntry="publish_fs";
        p.fragmentStage.depthAttatchment={.enabled=true,.format=Format_Depth32Float,
            .loadOp=LoadOp::LoadOp_Load,.storeOp=StoreOp::StoreOp_Store,
            .depthTestEnabled=true,.depthWriteEnabled=true,.comparison=ComparisonOp::ComparisonOp_Always};
        graphics_=renderGraph->createGraphicsPipeline(p);
        const auto size=state_->manager->getSettings().physicalAtlasResolution;
        RenderPassInfo renderPass{}; renderPass.name=passName+".Publish";
        renderPass.scissor=Rect2D(0,0,size,size); renderPass.viewport=Viewport(size,size);
        renderPass.depthStencilAttachment={.enabled=true,.name=passName+".Atlas",
            .view={.texture=state_->manager->getShadowAtlasTexture(),.access=AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
                .layout=ResourceLayout::DEPTH_STENCIL_ATTACHMENT,.flags=ImageAspectFlags::Depth,
                .baseArrayLayer=0,.baseMipLevel=0,.layerCount=1,.levelCount=1}};
        cmd.cmdBindGraphicsPipeline(graphics_); cmd.cmdBeginRenderPass(renderPass);
        cmd.cmdBindBindingGroups(state_->groups,nullptr,0); cmd.cmdDraw(6,state_->capacity); cmd.cmdEndRenderPass();
      }
    }
  };
public:
  struct Settings { uint32_t rayBudget=1024*1024; };
  VirtualShadowMapRayTraceDepthPass(VirtualShadowMapManager &manager,std::shared_ptr<Backend> backend,
      rendering::Buffer instances,rendering::Buffer instanceCount,Settings settings)
      : manager_(manager),backend_(std::move(backend)),instances_(instances),instanceCount_(instanceCount),settings_(settings) {
    if (!backend_ || !backend_->capabilities().available) throw std::invalid_argument("RT depth pages require Metal ray tracing");
  }
  ~VirtualShadowMapRayTraceDepthPass() override {
    for (auto *pass:children_) delete pass;
    if (!state_) return;
    for (uint32_t i:{0u,7u,8u,9u,10u,11u,12u,13u}) renderGraph->deleteBuffer(state_->buffers[i]);
    renderGraph->deleteBindingGroups(state_->groups); renderGraph->deleteBindingsLayout(state_->layout);
    renderGraph->deleteShader(state_->compute); renderGraph->deleteShader(state_->publish);
  }
  const rendering::Buffer &getUniformBuffer() const { return state_->buffers[0]; }
  const rendering::Buffer &getRayCountBuffer() const { return state_->buffers[8]; }
  const rendering::Buffer &getDepthValuesBuffer() const { return state_->buffers[11]; }
  const rendering::Buffer &getPageJobsBuffer() const { return state_->buffers[7]; }
  void setRayBudget(uint32_t rays) { settings_.rayBudget=rays; if (state_) uploadUniforms(); }
  void recordCommandBuffer(rendering::CommandRecorder &) override {
    using namespace rendering;
    state_=std::make_shared<State>(); state_->manager=&manager_;
    const auto &limits=backend_->configuration();
    const uint64_t texels=uint64_t(manager_.getPhysicalPageResolution())*manager_.getPhysicalPageResolution();
    state_->capacity=uint32_t(limits.maxRays/texels);
    if (!state_->capacity || !instances_.isValid() || !instanceCount_.isValid() ||
        renderGraph->getBufferSize(instances_)!=uint64_t(limits.maxInstances)*sizeof(raytracing::RayTraceInstance))
      throw std::invalid_argument("RT depth page buffers must match native capacities and fit a complete page");
    auto create=[&](const char *name,uint64_t size,BufferUsage usage,bool native=false) {
      return createFrameLocalBuffer(BufferInfo{.name=passName+name,.size=size,.usage=usage,.metalExportable=native});
    };
    state_->buffers={create(".Uniforms",32,BufferUsage_Uniform|BufferUsage_Push),manager_.getAllocatorCountersBuffer(),
        manager_.getFutureAllocationRequestsBuffer(),manager_.getAllocationRequestsBuffer(),manager_.getVirtualPageTableBuffer(),
        manager_.getCascadeMatricesBuffer(),manager_.getCascadeStatesBuffer(),
        create(".Jobs",uint64_t(state_->capacity)*8,BufferUsage_Storage|BufferUsage_CopySrc),
        create(".RayCount",4,BufferUsage_Storage|BufferUsage_CopySrc,true),
        create(".Rays",uint64_t(limits.maxRays)*sizeof(raytracing::ShadowRay),BufferUsage_Storage,true),
        create(".Hits",uint64_t(limits.maxRays)*sizeof(raytracing::ClosestHitResult),BufferUsage_Storage,true),
        create(".DepthValues",uint64_t(limits.maxRays)*4,BufferUsage_Storage|BufferUsage_CopySrc),
        create(".Valid",uint64_t(state_->capacity)*4,BufferUsage_Storage),
        create(".Dispatch",12,BufferUsage_Storage|BufferUsage_Indirect)};
    uploadUniforms();
    BindingGroupLayout layout; GroupInfo group; group.name=passName+".Bindings";
    for (uint32_t i=0;i<state_->buffers.size();++i) {
      const bool graphics=i==0 || i==7 || i==8 || i==11 || i==12;
      layout.buffers.push_back({.name="buffer"+std::to_string(i),.binding=i,
          .type=i==0 ? BufferBindingType_UniformBuffer : BufferBindingType_StorageBuffer,
          .visibility=graphics ? BindingVisibility_Compute|BindingVisibility_Vertex|BindingVisibility_Fragment : BindingVisibility_Compute});
      const bool write=i==4 || i==7 || i==8 || i==9 || i==11 || i==12 || i==13;
      group.buffers.push_back({.binding=i,.bufferView={.buffer=state_->buffers[i],
          .access=write ? AccessPattern::SHADER_READ_WRITE : AccessPattern::SHADER_READ,
          .offset=0,.size=renderGraph->getBufferSize(state_->buffers[i])}});
    }
    state_->layout=renderGraph->createBindingsLayout(BindingsLayoutInfo{.name=passName+".Layout",.groups={layout}});
    state_->groups=renderGraph->createBindingGroups(BindingGroupsInfo{.layout=state_->layout,.name=passName+".Groups",.groups={group}});
    auto shader=[&](const char *name){ return renderGraph->createShader(ShaderInfo{.name=passName+name,.layout=state_->layout,
        .src=os::io::readRelativeFile(std::string("assets/shaders/spirv/vsm-raytrace-")+name+".spirv"),.type=ShaderType::SpirV}); };
    state_->compute=shader("depth"); state_->publish=shader("publish");
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Generate",getIndex()+1,state_,0u));
    VirtualGeometryHardwareRayTracePass::Inputs inputs;
    auto view=[&](Buffer b){return BufferView{.buffer=b,.offset=0,.size=renderGraph->getBufferSize(b)};};
    inputs.instances=view(instances_); inputs.instanceCount=view(instanceCount_);
    inputs.rays=view(state_->buffers[9]); inputs.rayCount=view(state_->buffers[8]); inputs.results=view(state_->buffers[10]);
    inputs.query=Backend::Frame::Query::ClosestHit; inputs.buildPendingGeometry=false;
    children_.push_back(renderGraph->registerPass<VirtualGeometryHardwareRayTracePass>(passName+".Trace",getIndex()+2,backend_,inputs));
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Resolve",getIndex()+3,state_,1u));
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Publish",getIndex()+4,state_,2u));
    children_.push_back(renderGraph->registerPass<Stage>(passName+".Finish",getIndex()+5,state_,3u));
  }
private:
  void uploadUniforms() {
    uint32_t data[8]={manager_.getVirtualPageTableResolution(),manager_.getPhysicalPageResolution(),
        manager_.getSettings().physicalAtlasResolution,state_->capacity,manager_.getSettings().reverseZ ? 1u : 0u,
        std::min(settings_.rayBudget,backend_->configuration().maxRays),0,0};
    renderGraph->bufferWrite(state_->buffers[0],0,sizeof(data),data);
  }
  VirtualShadowMapManager &manager_; std::shared_ptr<Backend> backend_;
  rendering::Buffer instances_,instanceCount_; Settings settings_;
  std::shared_ptr<State> state_; std::vector<rendering::Pass *> children_;
};
}
#endif
