#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include <algorithm>
#include <array>

namespace voxel::svraster::rendering
{
class SparseVoxelSceneRayCompactionPass final : public ::rendering::Pass
{
public:
  enum class Stage { Reset, Compact };
  struct Buffers { ::rendering::Buffer rays, screenHits, compactRays, compactToCanonical, canonicalToCompact, treeDispatch, indirectArgs, treeState, activeRayIndices, activeRayState; };
  // `resumeAfterScreenTrace` lets the world ray skip the span the screen tracer
  // already examined, when that trace ended by exhausting its near-field budget
  // *on screen*. It is off by default and deliberately so: the depth buffer
  // holds only what is on screen and facing the camera, so a span that looked
  // clear there can still contain an occluder the world tracer would have
  // found. Inside a sealed room most of the enclosing geometry is off screen,
  // and skipping it lets sky and analytical lights leak in. A ray that left the
  // screen is never resumed for exactly that reason.
  SparseVoxelSceneRayCompactionPass(
      Stage stage, Buffers buffers, uint32_t capacity,
      bool screenTraceEnabled = true, bool resumeAfterScreenTrace = false,
      bool traceScreenHitsForConfirmation = false)
      : stage_(stage), buffers_(buffers),
        capacity_(std::max(1u, capacity)),
        screenTraceEnabled_(screenTraceEnabled),
        resumeAfterScreenTrace_(resumeAfterScreenTrace),
        traceScreenHitsForConfirmation_(traceScreenHitsForConfirmation)
  {
  }
  ~SparseVoxelSceneRayCompactionPass() override { if(pipeline_.isValid())renderGraph->deleteComputePipeline(pipeline_);if(shader_.isValid())renderGraph->deleteShader(shader_);if(groups_.isValid())renderGraph->deleteBindingGroups(groups_);if(layout_.isValid())renderGraph->deleteBindingsLayout(layout_);if(uniforms_.isValid())renderGraph->deleteBuffer(uniforms_); }
  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override { ensure(); renderGraph->bufferWrite(uniforms_,0u,sizeof(settings_),settings_.data());cmd.cmdBindComputePipeline(pipeline_);cmd.cmdBindBindingGroups(groups_,nullptr,0u);if(stage_==Stage::Reset)cmd.cmdDispatch(1u,1u,1u);else cmd.cmdDispatchIndirect(buffers_.activeRayState,0u); }
private:
  static ::rendering::BufferView v(::rendering::Buffer b,uint64_t n,::rendering::AccessPattern a){return {.buffer=b,.offset=0u,.size=std::max<uint64_t>(n,4u),.access=a};}
  // The settings buffer must NOT be frameLocal: it is uploaded once from
  // recordCommandBuffer(), which only runs while the first frame slot is
  // current. A frameLocal buffer would leave every other slot's instance
  // zeroed, so denseRayCount reads 0 on those frames, compaction emits no
  // world rays, and the trace consumes stale results on all but one of the
  // frames in flight. The content is a compile-time constant, so one shared
  // instance is race-free.
  void ensure(){if(layout_.isValid())return;const uint64_t n=capacity_;uniforms_=renderGraph->createBuffer({.name=passName+"_Settings",.size=32u,.usage=::rendering::BufferUsage::BufferUsage_Uniform|::rendering::BufferUsage::BufferUsage_Push,.frameLocal=false});settings_={capacity_,capacity_,screenTraceEnabled_?1u:0u,resumeAfterScreenTrace_?1u:0u,traceScreenHitsForConfirmation_?1u:0u,0u,0u,0u};layout_=renderGraph->createBindingsLayout({.name=passName+"_Layout",.groups={::rendering::BindingGroupLayout{.buffers={
 {.name="settings",.binding=0u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="rays",.binding=1u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="screenHits",.binding=2u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="compactRays",.binding=3u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="compactToCanonical",.binding=4u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="canonicalToCompact",.binding=5u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="treeDispatch",.binding=6u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="indirectArgs",.binding=7u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="treeState",.binding=8u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="activeRayIndices",.binding=9u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="activeRayState",.binding=10u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}}}}});groups_=renderGraph->createBindingGroups({.layout=layout_,.name=passName+"_Groups",.groups={::rendering::GroupInfo{.name="group0",.buffers={
 {.binding=0u,.bufferView=v(uniforms_,32u,::rendering::AccessPattern::UNIFORM_READ)},{.binding=1u,.bufferView=v(buffers_.rays,n*sizeof(SparseVoxelSceneRayGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=2u,.bufferView=v(buffers_.screenHits,(screenTraceEnabled_?n:1u)*sizeof(SparseVoxelSceneRayHitGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=3u,.bufferView=v(buffers_.compactRays,n*sizeof(SparseVoxelOccupancyDDARayGPU),::rendering::AccessPattern::SHADER_WRITE)},{.binding=4u,.bufferView=v(buffers_.compactToCanonical,n*sizeof(uint32_t),::rendering::AccessPattern::SHADER_WRITE)},{.binding=5u,.bufferView=v(buffers_.canonicalToCompact,n*sizeof(uint32_t),::rendering::AccessPattern::SHADER_WRITE)},{.binding=6u,.bufferView=v(buffers_.treeDispatch,sizeof(SparseVoxelTree64TraceDispatchGPU),::rendering::AccessPattern::SHADER_READ|::rendering::AccessPattern::SHADER_WRITE)},{.binding=7u,.bufferView=v(buffers_.indirectArgs,12u,::rendering::AccessPattern::SHADER_READ|::rendering::AccessPattern::SHADER_WRITE)},{.binding=8u,.bufferView=v(buffers_.treeState,sizeof(SparseVoxelTree64StateGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=9u,.bufferView=v(buffers_.activeRayIndices,n*sizeof(uint32_t),::rendering::AccessPattern::SHADER_READ)},{.binding=10u,.bufferView=v(buffers_.activeRayState,16u,::rendering::AccessPattern::SHADER_READ)}}}}});shader_=renderGraph->createShader({.name=passName+"_Shader",.layout=layout_,.src=os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-scene-ray-compact.spirv"),.type=::rendering::ShaderType::SpirV});pipeline_=renderGraph->createComputePipeline({.entry=stage_==Stage::Reset?"resetSparseVoxelSceneRayCompaction":"compactSparseVoxelSceneRays",.layout=layout_,.name=passName+"_Pipeline",.shader=shader_});}
  Stage stage_;Buffers buffers_{};uint32_t capacity_=1u;bool screenTraceEnabled_=true;bool resumeAfterScreenTrace_=false;bool traceScreenHitsForConfirmation_=false;std::array<uint32_t,8> settings_{};::rendering::Buffer uniforms_{};::rendering::BindingsLayout layout_{};::rendering::BindingGroups groups_{};::rendering::Shader shader_{};::rendering::ComputePipeline pipeline_{};
};
} // namespace voxel::svraster::rendering
