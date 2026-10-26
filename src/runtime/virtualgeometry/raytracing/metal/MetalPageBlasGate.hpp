#pragma once
#include "virtualgeometry/raytracing/PageBlasGate.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
#include <map>
#include <tuple>
#include <stdexcept>

namespace virtualgeometry::raytracing
{
// Renderer-thread service. Reserve buildSlot exclusively for this installer;
// trace passes use the other slots of the SAME backend, hence the same handles.
class MetalPageBlasGate final : public PageBlasGate
{
  using Backend = VirtualGeometryMetalRayTraceBackend;
  using Key = std::tuple<uint64_t, uint32_t, uint64_t, uint64_t>;
  struct Page { uint64_t identity; std::vector<uint32_t> handles; uint32_t expectedClusters = 0; };
  std::shared_ptr<Backend> backend_;
  std::map<Key, Page> pages_;
  uint32_t buildSlot_;
  uint64_t serial_ = 0;
  uint32_t preparationsRemaining_ = UINT32_MAX;
  static Key key(PageBlasKey k) { return {k.owner,k.page,k.generation,k.revision}; }
public:
  MetalPageBlasGate(std::shared_ptr<Backend> backend, uint32_t buildSlot)
      : backend_(std::move(backend)), buildSlot_(buildSlot)
  {
    if (!backend_ || !backend_->capabilities().available)
      throw std::runtime_error("Page BLAS installation requires an available Metal backend");
    preparationsRemaining_ = backend_->configuration().maxBlasPreparationsPerFrame;
  }
  const std::shared_ptr<Backend> &backend() const { return backend_; }
  ~MetalPageBlasGate() override
  {
    for (const auto &[k,page] : pages_)
      for (uint32_t i=0;i<page.handles.size();++i)
        backend_->evictGeometry({page.identity,std::get<3>(k),i,1});
  }
  State prepareEncoded(PageBlasKey k, const uint32_t *words, uint64_t count,
      uint32_t header, float scale) override
  {
    if (!k.owner || !k.generation || !k.revision || !words || !count)
      throw std::runtime_error("Invalid page BLAS install identity/input");
    auto it=pages_.find(key(k));
    if (it==pages_.end()) {
      Page page{allocateBlasOwner(),{}};
      try { page.handles=backend_->cacheEncodedPage(page.identity,k.revision,words,count,header,scale); }
      catch (const Backend::BudgetExceeded &) { return State::Pending; }
      it=pages_.emplace(key(k),std::move(page)).first;
    }
    return state(k,words[0]);
  }
  State prepareDecoded(PageBlasKey k, const std::vector<ClusterGeometry> &clusters,
      void *event, uint64_t value, bool producerCompleted = false) override
  {
    if (!k.owner || !k.generation || !k.revision || clusters.empty() || clusters.size()>MAX_GROUPS_PER_PAGE * GroupSize || (!producerCompleted && (!event || !value)))
      throw std::runtime_error("Invalid generated BLAS manifest/producer event");
    auto it=pages_.find(key(k));
    if (it==pages_.end()) it=pages_.emplace(key(k),Page{allocateBlasOwner(),{}}).first;
    auto &page=it->second;
    page.expectedClusters = uint32_t(clusters.size());
    try {
      while (page.handles.size()<clusters.size() && preparationsRemaining_>0) {
        const uint32_t i=uint32_t(page.handles.size());
        const auto &c=clusters[i]; Backend::Geometry geometry;
        geometry.key={page.identity,k.revision,i,1};
        geometry.vertexBuffer=c.vertices; geometry.indexBuffer=c.indices;
        geometry.vertexOffset=c.vertexOffset; geometry.indexOffset=c.indexOffset;
        geometry.vertexStride=c.vertexStride; geometry.vertexCount=c.vertexCount;
        geometry.triangleCount=c.triangleCount; geometry.readyEvent=event; geometry.readyValue=value;
        page.handles.push_back(backend_->cacheGeometry(geometry));
        --preparationsRemaining_;
      }
    } catch (const Backend::BudgetExceeded &) {
      for (uint32_t i=0;i<page.handles.size();++i) backend_->evictGeometry({page.identity,k.revision,i,1});
      pages_.erase(it);
      return State::Pending;
    } catch (...) {
      for (uint32_t i=0;i<page.handles.size();++i) backend_->evictGeometry({page.identity,k.revision,i,1});
      pages_.erase(it);
      throw;
    }
    if (page.handles.size()<clusters.size()) return State::Pending;
    return state(k,uint32_t(clusters.size()));
  }
  State state(PageBlasKey k, uint32_t clusters) override
  {
    const auto it=pages_.find(key(k));
    if (it==pages_.end() || !clusters || it->second.handles.size()>clusters) return State::Missing;
    if (it->second.handles.size()<clusters) return State::Pending;
    switch (backend_->geometryStates(it->second.handles)) {
      case Backend::GeometryState::Missing: return State::Missing;
      case Backend::GeometryState::Failed: return State::Failed;
      case Backend::GeometryState::Pending: return State::Pending;
      case Backend::GeometryState::Ready: return State::Ready;
    }
    return State::Failed;
  }
  std::vector<uint32_t> handles(PageBlasKey k) override
  {
    const auto it=pages_.find(key(k));
    if (it==pages_.end() || state(k,it->second.expectedClusters ? it->second.expectedClusters : uint32_t(it->second.handles.size()))!=State::Ready) return {};
    return it->second.handles;
  }
  void pump() override
  {
    preparationsRemaining_ = backend_->configuration().maxBlasPreparationsPerFrame;
    if (serial_) {
      const auto completion=backend_->poll(serial_);
      if (!completion.completed) return;
      if (!completion.succeeded) throw std::runtime_error("Page BLAS build failed: "+completion.error);
      serial_=0;
    }
    if (!backend_->cacheStatistics().pendingBuilds) return;
    Backend::Frame frame; frame.slot=buildSlot_; frame.work=Backend::Frame::Work::BuildOnly;
    serial_=backend_->submit(frame);
  }
  void retireOtherVersions(PageBlasKey keep) override
  {
    for (auto it=pages_.begin();it!=pages_.end();) {
      if (std::get<0>(it->first)!=keep.owner || std::get<1>(it->first)!=keep.page ||
          it->first==key(keep)) { ++it; continue; }
      for (uint32_t i=0;i<it->second.handles.size();++i)
        backend_->evictGeometry({it->second.identity,std::get<3>(it->first),i,1});
      it=pages_.erase(it);
    }
  }
  bool releasePage(PageBlasKey k) override
  {
    auto it=pages_.find(key(k));
    if (it==pages_.end()) return true;
    bool retired=true;
    for (uint32_t i=0;i<it->second.handles.size();++i)
      retired &= backend_->tryEvictGeometryAfterInputReads({it->second.identity,k.revision,i,1});
    if (!retired) return false;
    pages_.erase(it);
    return true;
  }
  void drainInputReads() override
  {
    for (const auto &[k,page]:pages_)
      for (auto handle:page.handles) backend_->drainGeometryInputs(handle);
  }
  void releaseOwner(uint64_t owner) override
  {
    for (auto it=pages_.begin();it!=pages_.end();) {
      if (std::get<0>(it->first)!=owner) { ++it; continue; }
      for (uint32_t i=0;i<it->second.handles.size();++i)
        backend_->evictGeometry({it->second.identity,std::get<3>(it->first),i,1});
      it=pages_.erase(it);
    }
  }
};
}
