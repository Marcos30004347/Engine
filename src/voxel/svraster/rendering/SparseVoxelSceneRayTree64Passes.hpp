#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{
namespace detail
{
struct alignas(16) SparseVoxelSceneRayDispatchGPU { uint32_t rayCount = 0u, reserved0 = 0u, reserved1 = 0u, reserved2 = 0u; };
static_assert(sizeof(SparseVoxelSceneRayDispatchGPU) == 16u);
struct alignas(16) SparseVoxelSceneHitMergeSettingsGPU
{
  uint32_t rayCount = 0u, reserved0 = 0u, reserved1 = 0u, reserved2 = 0u;
  std::array<float, 4u> sceneMinimum{};
  std::array<float, 4u> sceneMaximum{};
};
static_assert(sizeof(SparseVoxelSceneHitMergeSettingsGPU) == 48u);
inline ::rendering::BufferView sceneRayView(::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
{ return {.buffer=buffer, .offset=0u, .size=std::max<uint64_t>(size, sizeof(uint32_t)), .access=access}; }
}

class SparseVoxelSceneRayTree64PreparePass final : public ::rendering::Pass
{
public:
  struct Buffers { ::rendering::Buffer rays, screenHits, treeRays, treeState; };
  SparseVoxelSceneRayTree64PreparePass(Buffers buffers, uint32_t rayCapacity) : buffers_(buffers), rayCapacity_(std::max(1u, rayCapacity)) {}
  void update(uint32_t rayCount) { dispatch_.rayCount=std::min(rayCount, rayCapacity_); if (uniforms_.isValid()) renderGraph->bufferWrite(uniforms_,0u,sizeof(dispatch_),&dispatch_); }
  ~SparseVoxelSceneRayTree64PreparePass() override { destroy(); }
  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override { ensure(); renderGraph->bufferWrite(uniforms_,0u,sizeof(dispatch_),&dispatch_); cmd.cmdBindComputePipeline(pipeline_); cmd.cmdBindBindingGroups(groups_,nullptr,0u); cmd.cmdDispatch((rayCapacity_+63u)/64u,1u,1u); }
private:
  void destroy() { if(pipeline_.isValid())renderGraph->deleteComputePipeline(pipeline_); if(shader_.isValid())renderGraph->deleteShader(shader_); if(groups_.isValid())renderGraph->deleteBindingGroups(groups_); if(layout_.isValid())renderGraph->deleteBindingsLayout(layout_); if(uniforms_.isValid())renderGraph->deleteBuffer(uniforms_); }
  void ensure() {
    if(layout_.isValid())return; const uint64_t count=rayCapacity_;
    uniforms_=renderGraph->createBuffer({.name=passName+"_Dispatch",.size=sizeof(dispatch_),.usage=::rendering::BufferUsage::BufferUsage_Uniform|::rendering::BufferUsage::BufferUsage_Push,.frameLocal=true});
    layout_=renderGraph->createBindingsLayout({.name=passName+"_Layout",.groups={::rendering::BindingGroupLayout{.buffers={
      {.name="dispatch",.binding=0u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}, {.name="rays",.binding=1u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}, {.name="screenHits",.binding=2u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}, {.name="treeRays",.binding=3u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}, {.name="treeState",.binding=4u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}}}}});
    groups_=renderGraph->createBindingGroups({.layout=layout_,.name=passName+"_Groups",.groups={::rendering::GroupInfo{.name="group0",.buffers={
      {.binding=0u,.bufferView=detail::sceneRayView(uniforms_,sizeof(dispatch_),::rendering::AccessPattern::UNIFORM_READ)}, {.binding=1u,.bufferView=detail::sceneRayView(buffers_.rays,count*sizeof(SparseVoxelSceneRayGPU),::rendering::AccessPattern::SHADER_READ)}, {.binding=2u,.bufferView=detail::sceneRayView(buffers_.screenHits,count*sizeof(SparseVoxelSceneRayHitGPU),::rendering::AccessPattern::SHADER_READ)}, {.binding=3u,.bufferView=detail::sceneRayView(buffers_.treeRays,count*sizeof(SparseVoxelOccupancyDDARayGPU),::rendering::AccessPattern::SHADER_WRITE)}, {.binding=4u,.bufferView=detail::sceneRayView(buffers_.treeState,sizeof(SparseVoxelTree64StateGPU),::rendering::AccessPattern::SHADER_READ)}}}}});
    shader_=renderGraph->createShader({.name=passName+"_Shader",.layout=layout_,.src=os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-scene-tree64-prepare.spirv"),.type=::rendering::ShaderType::SpirV}); pipeline_=renderGraph->createComputePipeline({.entry="prepareSparseVoxelSceneTree64Rays",.layout=layout_,.name=passName+"_Pipeline",.shader=shader_});
  }
  Buffers buffers_{}; uint32_t rayCapacity_=1u; detail::SparseVoxelSceneRayDispatchGPU dispatch_{}; ::rendering::Buffer uniforms_{}; ::rendering::BindingsLayout layout_{}; ::rendering::BindingGroups groups_{}; ::rendering::Shader shader_{}; ::rendering::ComputePipeline pipeline_{};
};

class SparseVoxelSceneHitMergePass final : public ::rendering::Pass
{
public:
  struct Buffers { ::rendering::Buffer treeState, rays, screenHits, treeHits, canonicalToCompact, mergedHits, treeRays, activeRayIndices, activeRayState; };
  SparseVoxelSceneHitMergePass(
      Buffers buffers,
      uint32_t rayCapacity,
      const std::array<float, 3u> &sceneMinimum,
      const std::array<float, 3u> &sceneMaximum,
      const bool screenTraceEnabled = true,
      const bool confirmScreenHitsWithTree64 = false)
      : buffers_(buffers), rayCapacity_(std::max(1u,rayCapacity))
  {
    settings_.reserved0 = screenTraceEnabled ? 1u : 0u;
    settings_.reserved1 = confirmScreenHitsWithTree64 ? 1u : 0u;
    update(rayCapacity, sceneMinimum, sceneMaximum);
  }
  void update(
      uint32_t rayCount,
      const std::array<float, 3u> &sceneMinimum,
      const std::array<float, 3u> &sceneMaximum)
  {
    settings_.rayCount=std::min(rayCount,rayCapacity_);
    for (uint32_t axis=0u;axis<3u;++axis)
    {
      settings_.sceneMinimum[axis]=sceneMinimum[axis];
      settings_.sceneMaximum[axis]=sceneMaximum[axis];
    }
    if(uniforms_.isValid())renderGraph->bufferWrite(uniforms_,0u,sizeof(settings_),&settings_);
  }
  ~SparseVoxelSceneHitMergePass() override { destroy(); }
  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override { ensure(); renderGraph->bufferWrite(uniforms_,0u,sizeof(settings_),&settings_);cmd.cmdBindComputePipeline(pipeline_);cmd.cmdBindBindingGroups(groups_,nullptr,0u);cmd.cmdDispatchIndirect(buffers_.activeRayState,0u); }
private:
  void destroy(){if(pipeline_.isValid())renderGraph->deleteComputePipeline(pipeline_);if(shader_.isValid())renderGraph->deleteShader(shader_);if(groups_.isValid())renderGraph->deleteBindingGroups(groups_);if(layout_.isValid())renderGraph->deleteBindingsLayout(layout_);if(uniforms_.isValid())renderGraph->deleteBuffer(uniforms_);}
  void ensure(){if(layout_.isValid())return;const uint64_t count=rayCapacity_;uniforms_=renderGraph->createBuffer({.name=passName+"_Settings",.size=sizeof(settings_),.usage=::rendering::BufferUsage::BufferUsage_Uniform|::rendering::BufferUsage::BufferUsage_Push,.frameLocal=true});layout_=renderGraph->createBindingsLayout({.name=passName+"_Layout",.groups={::rendering::BindingGroupLayout{.buffers={
    {.name="dispatch",.binding=0u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="treeState",.binding=1u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="rays",.binding=2u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="screenHits",.binding=3u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="treeHits",.binding=4u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="canonicalToCompact",.binding=5u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="mergedHits",.binding=6u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="treeRays",.binding=7u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="activeRayIndices",.binding=8u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},{.name="activeRayState",.binding=9u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}}}}});groups_=renderGraph->createBindingGroups({.layout=layout_,.name=passName+"_Groups",.groups={::rendering::GroupInfo{.name="group0",.buffers={
    {.binding=0u,.bufferView=detail::sceneRayView(uniforms_,sizeof(settings_),::rendering::AccessPattern::UNIFORM_READ)},{.binding=1u,.bufferView=detail::sceneRayView(buffers_.treeState,sizeof(SparseVoxelTree64StateGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=2u,.bufferView=detail::sceneRayView(buffers_.rays,count*sizeof(SparseVoxelSceneRayGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=3u,.bufferView=detail::sceneRayView(buffers_.screenHits,(settings_.reserved0!=0u?count:1u)*sizeof(SparseVoxelSceneRayHitGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=4u,.bufferView=detail::sceneRayView(buffers_.treeHits,count*sizeof(SparseVoxelOccupancyDDAResultGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=5u,.bufferView=detail::sceneRayView(buffers_.canonicalToCompact,count*sizeof(uint32_t),::rendering::AccessPattern::SHADER_READ)},{.binding=6u,.bufferView=detail::sceneRayView(buffers_.mergedHits,count*sizeof(SparseVoxelSceneRayHitGPU),::rendering::AccessPattern::SHADER_WRITE)},{.binding=7u,.bufferView=detail::sceneRayView(buffers_.treeRays,count*sizeof(SparseVoxelOccupancyDDARayGPU),::rendering::AccessPattern::SHADER_READ)},{.binding=8u,.bufferView=detail::sceneRayView(buffers_.activeRayIndices,count*sizeof(uint32_t),::rendering::AccessPattern::SHADER_READ)},{.binding=9u,.bufferView=detail::sceneRayView(buffers_.activeRayState,16u,::rendering::AccessPattern::SHADER_READ)}}}}});shader_=renderGraph->createShader({.name=passName+"_Shader",.layout=layout_,.src=os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-scene-hit-merge.spirv"),.type=::rendering::ShaderType::SpirV});pipeline_=renderGraph->createComputePipeline({.entry="mergeSparseVoxelSceneHits",.layout=layout_,.name=passName+"_Pipeline",.shader=shader_});}
  Buffers buffers_{};uint32_t rayCapacity_=1u;detail::SparseVoxelSceneHitMergeSettingsGPU settings_{};::rendering::Buffer uniforms_{};::rendering::BindingsLayout layout_{};::rendering::BindingGroups groups_{};::rendering::Shader shader_{};::rendering::ComputePipeline pipeline_{};
};
} // namespace voxel::svraster::rendering
