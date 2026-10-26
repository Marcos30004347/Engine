#pragma once

#include "voxel/svraster/SparseVoxelSceneManager.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include <cmath>

#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/PageBlasGate.hpp"
#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
namespace voxel::svraster
{
// Resident sparse-voxel models are installed once as immutable triangle BLASes.
// This provider covers authored leaf boxes; streamed VG pages use their separate
// page-install gate. Replacing topology requires recreating this provider.
class SparseVoxelHardwareRayTraceScene
{
public:
  using Backend = virtualgeometry::raytracing::VirtualGeometryMetalRayTraceBackend;
  SparseVoxelHardwareRayTraceScene(::rendering::RenderGraph &graph,
      const SparseVoxelSceneManager &scene, uint32_t rays, uint32_t frameSlots)
      : graph_(graph), topology_(scene.topologyRevision()), buildSlot_(frameSlots)
  {
    if (!scene.scene().voxelMaterialReferences.empty())
      throw std::invalid_argument("Hardware ReSTIR leaf-box geometry does not support procedural material geometry");
    Backend::Configuration config;
    config.metallibPath=VG_RT_METALLIB_PATH;
    config.maxInstances=std::max<uint32_t>(1u,scene.runtimeInstances().size());
    config.maxRays=std::max(1u,rays); config.frameSlots=frameSlots+1;
    backend_=std::make_shared<Backend>(config,graph.getRHI()->exportMetalDevice());
    if (!backend_->capabilities().available) return;
    instances_=create("restir.hardware.instances",uint64_t(config.maxInstances)*56,true);
    count_=create("restir.hardware.instanceCount",4,true);
    handles_.resize(scene.scene().models.size());
    const uint64_t owner=virtualgeometry::raytracing::allocateBlasOwner();
    for (uint32_t modelIndex=0;modelIndex<scene.scene().models.size();++modelIndex)
    {
      const auto &model=scene.scene().models[modelIndex];
      std::vector<float> vertices;
      std::vector<uint32_t> indices;
      constexpr uint32_t boxIndices[36]={0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,
                                        2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5};
      for (const auto &node:model.hierarchyNodes)
      {
        if (node.childCount) continue;
        if (node.voxelIndex>=model.centersLengths.size()) throw std::invalid_argument("Invalid hardware model leaf");
        const auto &box=model.centersLengths[node.voxelIndex];
        if (!std::isfinite(box.length) || box.length<=0) throw std::invalid_argument("Invalid hardware voxel extent");
        if (vertices.size()/3>UINT32_MAX-8u) throw std::length_error("Hardware model vertex capacity exceeded");
        const auto first=uint32_t(vertices.size()/3);
        for (uint32_t corner=0;corner<8;++corner)
          for (uint32_t axis=0;axis<3;++axis)
            vertices.push_back(box.center[axis]+((corner&(1u<<axis)) ? .5f : -.5f)*box.length);
        for (const auto index:boxIndices) indices.push_back(first+index);
      }
      if (indices.empty()) continue;
      auto vb=create("restir.hardware.modelVertices",vertices.size()*4,false);
      auto ib=create("restir.hardware.modelIndices",indices.size()*4,false);
      graph_.bufferWrite(vb,0,vertices.size()*4,vertices.data());
      graph_.bufferWrite(ib,0,indices.size()*4,indices.data());
      Backend::Geometry geometry;
      geometry.key={owner,topology_,modelIndex,2};
      geometry.vertexBuffer=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(vb));
      geometry.indexBuffer=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(ib));
      geometry.vertexStride=12; geometry.vertexCount=vertices.size()/3; geometry.triangleCount=indices.size()/3;
      handles_[modelIndex]=backend_->cacheGeometry(geometry);
      triangles_+=geometry.triangleCount;
    }
    update(scene);
  }
  ~SparseVoxelHardwareRayTraceScene()
  {
    // The backend owns pending native input reads. Drain it before releasing
    // immutable model buffers, including startup canceled before the first ray.
    backend_.reset();
    for (auto b:buffers_) graph_.deleteBuffer(b);
  }
  void update(const SparseVoxelSceneManager &scene)
  {
    if (scene.topologyRevision()!=topology_) throw std::runtime_error("Recreate hardware ReSTIR scene after topology changes");
    if (buildSerial_)
    {
      auto done=backend_->poll(buildSerial_);
      if (done.completed) {
        if (!done.succeeded) throw std::runtime_error(done.error);
        buildSerial_=0;
      }
    }
    if (!buildSerial_ && backend_->cacheStatistics().pendingBuilds)
    {
      Backend::Frame frame; frame.slot=buildSlot_; frame.work=Backend::Frame::Work::BuildOnly;
      buildSerial_=backend_->submit(frame);
    }
    std::vector<virtualgeometry::raytracing::RayTraceInstance> records;
    for (const auto &instance:scene.runtimeInstances())
    {
      const uint32_t handle=handles_.at(instance.gpu.blasIndex);
      if (!handle) continue; // Empty authored model.
      virtualgeometry::raytracing::RayTraceInstance record;
      // A pending install is an invalid scene, never an empty, fully lit scene.
      record.geometryHandle=backend_->geometryState(handle)==Backend::GeometryState::Ready ? handle : 0u;
      for (uint32_t column=0;column<4;++column)
        for (uint32_t row=0;row<3;++row)
          record.transform[column*3+row]=instance.gpu.modelToWorld[column*4+row];
      records.push_back(record);
    }
    uint32_t count=records.size();
    if (count>backend_->configuration().maxInstances) throw std::length_error("Hardware ReSTIR instance capacity exceeded");
    if (count) graph_.bufferWrite(instances_,0,records.size()*56,records.data());
    graph_.bufferWrite(count_,0,4,&count);
  }
  const std::shared_ptr<Backend> &backend() const { return backend_; }
  ::rendering::Buffer instances() const { return instances_; }
  ::rendering::Buffer instanceCount() const { return count_; }
  uint64_t triangleCount() const { return triangles_; }
private:
  ::rendering::Buffer create(const char *name,uint64_t bytes,bool frameLocal)
  {
    auto b=graph_.createBuffer({.name=std::string(name)+std::to_string(buffers_.size()),.size=bytes,
      .usage=::rendering::BufferUsage_Storage|::rendering::BufferUsage_Push,
      .frameLocal=frameLocal,.metalExportable=true});
    buffers_.push_back(b); return b;
  }
  ::rendering::RenderGraph &graph_;
  uint64_t topology_,buildSerial_=0,triangles_=0;
  uint32_t buildSlot_;
  std::shared_ptr<Backend> backend_;
  std::vector<uint32_t> handles_;
  std::vector<::rendering::Buffer> buffers_;
  ::rendering::Buffer instances_,count_;
};
}
#endif
