#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.hpp"
#include <atomic>

namespace virtualgeometry::gpgpu
{
// Native frame segment for an already valid AS-ready scene cut and GPU ray list.
// It declares graph dependencies and uses the shared-event handoff; it does not
// synchronously read a count, wait for a frame, or own the input scene buffers.
class VirtualGeometryHardwareRayTracePass final : public rendering::Pass
{
public:
  using Backend = raytracing::VirtualGeometryMetalRayTraceBackend;
  struct Inputs
  {
    rendering::BufferView instances, instanceCount, rays, rayCount, results;
    rendering::BufferView casterBounds, lightVolumes, lightVolumeCount;
    // Decoded geometry buffers used by pending one-time builds. They must be
    // declared even though the native cache, rather than the trace kernel,
    // binds them; this orders page decode/upload before the shared ready event.
    std::vector<rendering::BufferView> geometryBuildInputs;
    Backend::Frame::Query query = Backend::Frame::Query::AnyHit;
    // Disable when PageBlasGate owns installation, so tracing cannot consume
    // a second build quota or wait on unrelated pending generated pages.
    bool buildPendingGeometry = true;
  };
  VirtualGeometryHardwareRayTracePass(std::shared_ptr<Backend> backend, Inputs inputs)
      : backend_(std::move(backend)), inputs_(std::move(inputs))
  {
    if (!backend_ || !backend_->capabilities().available)
      throw std::invalid_argument("Hardware ray trace pass requires an available Metal backend");
    serials_=std::make_shared<std::vector<std::atomic<uint64_t>>>(backend_->configuration().frameSlots);
    for (const auto *view : {&inputs_.instances,&inputs_.instanceCount,&inputs_.rays,&inputs_.rayCount,&inputs_.results})
      if (!view->buffer.isValid() || view->size == 0 || view->offset != 0)
        throw std::invalid_argument("Hardware ray trace pass requires complete buffers at offset zero");
    const bool lightCull=inputs_.casterBounds.buffer.isValid() || inputs_.lightVolumes.buffer.isValid() || inputs_.lightVolumeCount.buffer.isValid();
    if (lightCull)
      for (const auto *view : {&inputs_.casterBounds,&inputs_.lightVolumes,&inputs_.lightVolumeCount})
        if (!view->buffer.isValid() || view->size == 0 || view->offset != 0)
          throw std::invalid_argument("Hardware ray trace light culling requires all three complete buffers");
  }

  // Read after the graph frame completes, before reusing this slot. This uses
  // Metal command-buffer timestamps and existing shared statistics, without
  // adding GPU queries, readback copies, or a synchronous wait.
  Backend::Completion completion(uint32_t frameSlot)
  {
    const auto serial=serials_->at(frameSlot).load(std::memory_order_acquire);
    return serial ? backend_->poll(serial) : Backend::Completion{};
  }

  void recordCommandBuffer(rendering::CommandRecorder &recorder) override
  {
    std::vector<rendering::BufferView> views;
    for (auto view : {inputs_.instances,inputs_.instanceCount,inputs_.rays,inputs_.rayCount,
                      inputs_.casterBounds,inputs_.lightVolumes,inputs_.lightVolumeCount})
      if (view.buffer.isValid()) { view.access=rendering::AccessPattern::SHADER_READ; views.push_back(view); }
    auto output=inputs_.results; output.access=rendering::AccessPattern::SHADER_WRITE; views.push_back(output);
    for (auto view : inputs_.geometryBuildInputs)
    { view.access=rendering::AccessPattern::SHADER_READ; views.push_back(view); }
    recorder.recorded.back().commands.push_back(std::make_shared<NativeCommand>(std::move(views),[this](rendering::CommandRunContext &context) {
      Backend::Frame frame;
      frame.query=inputs_.query;
      if (!inputs_.buildPendingGeometry) frame.work=Backend::Frame::Work::TlasAndTrace;
      auto native=[&](const rendering::BufferView &view) -> void * {
        if (!view.buffer.isValid()) return nullptr;
        return renderGraph->getRHI()->exportMetalBuffer(renderGraph->resolveBufferId(view.buffer));
      };
      frame.instances=native(inputs_.instances); frame.instanceCount=native(inputs_.instanceCount);
      frame.rays=native(inputs_.rays); frame.rayCount=native(inputs_.rayCount); frame.results=native(inputs_.results);
      frame.casterBounds=native(inputs_.casterBounds); frame.lightVolumes=native(inputs_.lightVolumes);
      frame.lightVolumeCount=native(inputs_.lightVolumeCount); frame.slot=getCurrentFrameIndex();
      context.rhi->cmdMetalCompute(context.commandBuffer,[backend=backend_,serials=serials_,frame](void *event,uint64_t ready,uint64_t done) mutable {
        frame.sharedEvent=event; frame.readyValue=ready; frame.doneValue=done;
        serials->at(frame.slot).store(backend->submit(frame),std::memory_order_release);
      });
    }));
  }

private:
  class NativeCommand final : public rendering::Command
  {
    std::vector<rendering::BufferView> views_;
    std::function<void(rendering::CommandRunContext &)> encode_;
  public:
    NativeCommand(std::vector<rendering::BufferView> views, decltype(encode_) encode)
        : views_(std::move(views)), encode_(std::move(encode)) {}
    std::shared_ptr<rendering::Command> clone() const override { return std::make_shared<NativeCommand>(*this); }
    rendering::Queue queueHint() const override { return rendering::Queue::Compute; }
    bool triggersNodeSplit() const override { return true; }
    void externalBufferViews(std::vector<rendering::BufferView> &views) const override { views.insert(views.end(),views_.begin(),views_.end()); }
    void run(rendering::CommandRunContext &context) const override { encode_(context); }
  };
  std::shared_ptr<Backend> backend_;
  Inputs inputs_;
  std::shared_ptr<std::vector<std::atomic<uint64_t>>> serials_;
};
}
#endif
