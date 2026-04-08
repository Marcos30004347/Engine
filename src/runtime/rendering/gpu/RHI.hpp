#pragma once

#include "EventLoop.hpp"
#include "Types.hpp"
#include <type_traits>

namespace rendering
{

class GPUFuture
{
private:
  // 1. The abstract concept
  struct FutureConcept
  {
    virtual ~FutureConcept() = default;
    virtual bool valid() const = 0;
    virtual FenceStatus status() const = 0;
    virtual void wait() const = 0; // Optional blocking wait
  };

  // 2. The concrete model (Templated)
  template <typename T> struct FutureModel : FutureConcept
  {
    T internalFuture; // e.g., AsyncEvent<VulkanAsyncHandler>

    FutureModel(T f) : internalFuture(std::move(f))
    {
    }

    bool valid() const override
    {
      return internalFuture.isValid();
    }

    rendering::FenceStatus status() const override
    {
      return internalFuture.checkStatus();
    }

    void wait() const override
    {
      // If you need blocking wait logic, you can implement it here
      // using the backend's specific wait mechanism
    }
  };

  std::shared_ptr<FutureConcept> impl_;

public:
  GPUFuture() = default;
  GPUFuture(const GPUFuture &) = default;
  GPUFuture(GPUFuture &&) noexcept = default;
  GPUFuture &operator=(const GPUFuture &) = default;
  GPUFuture &operator=(GPUFuture &&) noexcept = default;

  template <typename T, typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, GPUFuture>>> GPUFuture(T &&future)
      : impl_(std::make_shared<FutureModel<std::decay_t<T>>>(std::forward<T>(future)))
  {
  }

  bool valid() const
  {
    return impl_ && impl_->valid();
  }

  rendering::FenceStatus checkStatus() const
  {
    return impl_ ? impl_->status() : rendering::FenceStatus::ERROR;
  }

  template <typename T> T *getIf() const
  {
    if (auto model = dynamic_cast<FutureModel<T> *>(impl_.get()))
    {
      return &model->internalFuture;
    }
    return nullptr;
  }
};
class RHI
{
protected:
  DeviceFeatures features;
  DeviceProperties properties;
  DeviceRequiredLimits requiredLimits;

public:
  virtual ~RHI() = default;

  inline DeviceProperties GetProperties()
  {
    return properties;
  }

  inline DeviceFeatures getFeatures()
  {
    return features;
  }

  virtual void bufferRead(BufferId bufferId, const uint64_t offset, const uint64_t size, std::function<void(const void *)>) = 0;
  virtual void bufferWrite(BufferId bufferId, const uint64_t offset, const uint64_t size, void *data) = 0;

  virtual const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) = 0;
  virtual void destroySwapChain(SwapChain) = 0;
  virtual Format getSwapChainFormat(SwapChain handle) = 0;
  virtual const uint32_t getSwapChainImagesCount(SwapChain swapChainHandle) = 0;
  virtual const uint32_t getSwapChainImagesWidth(SwapChain swapChainHandle) = 0;
  virtual const uint32_t getSwapChainImagesHeight(SwapChain swapChainHandle) = 0;

  virtual const TextureView getSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex) = 0;
  virtual const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle) = 0;

  virtual std::vector<CommandBuffer> allocateCommandBuffers(Queue queue, uint32_t count) = 0;
  virtual void releaseCommandBuffer(std::vector<CommandBuffer> &buffers) = 0;

  virtual void beginCommandBuffer(CommandBuffer, bool oneTimeSubmit = true) = 0;
  virtual void endCommandBuffer(CommandBuffer) = 0;
  virtual void cmdCopyBuffer(CommandBuffer cmdBuffer, const BufferView &src, const BufferView &dst) = 0;
  virtual void cmdCopyImage(CommandBuffer cmdBuffer, const TextureView &src, const TextureView &dst) = 0;
  virtual void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) = 0;
  virtual void cmdEndRenderPass(CommandBuffer) = 0;

  virtual void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroupsId groupsId, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) = 0;
  virtual void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) = 0;
  virtual void cmdBindComputePipeline(CommandBuffer, ComputePipeline) = 0;
  virtual void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, const BufferView &) = 0;
  virtual void cmdBindIndexBuffer(CommandBuffer, const BufferView &, Type type) = 0;

  virtual void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndirect(CommandBuffer, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride) = 0;
  virtual void cmdDrawIndirectCount(CommandBuffer handle, const BufferView &indirectBuffer, const BufferView &countBuffer, uint32_t maxDrawCount, uint32_t stride) = 0;
  virtual void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndexedIndirect(CommandBuffer, const BufferView &indirectBuffer, uint32_t drawCount, uint32_t stride) = 0;
  virtual void cmdResetTimer(CommandBuffer, const Timer &) = 0;
  virtual void cmdStartTimer(CommandBuffer, const Timer &, uint32_t sampleIndex, PipelineStage stage) = 0;
  virtual void cmdStopTimer(CommandBuffer, const Timer &, uint32_t sampleIndex, PipelineStage stage) = 0;
  virtual void cmdResolveTimersToBuffer(CommandBuffer, const Timer *timers, uint32_t count, BufferId destinationBuffer, uint64_t destinationOffset) = 0;
  virtual void cmdDispatchIndirect(CommandBuffer commandBuffer, const BufferView &indirectBuffer) = 0;
  virtual void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
  virtual void cmdImageBarrier(
      CommandBuffer cmd,
      TextureId image,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      ResourceLayout old_layout,
      ResourceLayout new_layout,
      ImageAspectFlags aspect_mask,
      uint32_t base_mip_level,
      uint32_t level_count,
      uint32_t base_array_layer,
      uint32_t layer_count,
      Queue src_queue_family,
      Queue dst_queue_family) = 0;
  virtual void cmdBufferBarrier(
      CommandBuffer cmd,
      BufferId b,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      uint32_t offset,
      uint32_t size,
      Queue src_queue_family,
      Queue dst_queue_family) = 0;
  virtual void cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;
  virtual void cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;
  virtual GPUFuture submit(Queue queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait, uint32_t waitCount, bool releaseCommandBuffersOnCompletion = true) = 0;
  virtual void present(SwapChain &swapChain, TextureId textureId, ResourceLayout currentLayout) = 0;
  virtual void waitIdle() = 0;
  virtual void blockUntil(GPUFuture &) = 0;
  virtual bool isCompleted(GPUFuture &) = 0;
  virtual void flushDeferredDeletions() {}
  virtual void reserveTimerCapacity(uint32_t) {}

  virtual const Timer createTimer(const TimerInfo) = 0;
  virtual void deleteTimer(const Timer &timer) = 0;
  virtual double readTimer(const Timer &timer) = 0;
  virtual double getTimestampPeriodNs() const
  {
    return 1.0;
  }
  virtual void readTimers(const Timer *timers, uint32_t count, double *outValues)
  {
    if (outValues == nullptr)
    {
      return;
    }

    for (uint32_t timerIndex = 0u; timerIndex < count; ++timerIndex)
    {
      outValues[timerIndex] = readTimer(timers[timerIndex]);
    }
  }
  virtual BufferId createBuffer(const BufferInfo &info) = 0;
  virtual TextureId createTexture(const TextureInfo &info) = 0;
  virtual SamplerId createSampler(const SamplerInfo &info) = 0;
  virtual BindingsLayoutId createBindingsLayout(const BindingsLayoutInfo &info) = 0;
  virtual BindingGroupsId createBindingGroups(const BindingGroupsInfo &info) = 0;
  virtual GraphicsPipelineId createGraphicsPipeline(const GraphicsPipelineInfo &info) = 0;
  virtual ComputePipelineId createComputePipeline(const ComputePipelineInfo &info) = 0;
  virtual ShaderId createShader(const ShaderInfo data) = 0;
  virtual void deleteShader(ShaderId handle) = 0;
  virtual void deleteBuffer(BufferId resourceId) = 0;
  virtual void deleteTexture(TextureId resourceId) = 0;
  virtual void deleteSampler(SamplerId resourceId) = 0;
  virtual void deleteBindingsLayout(BindingsLayoutId resourceId) = 0;
  virtual void deleteBindingGroups(BindingGroupsId resourceId) = 0;
  virtual void deleteGraphicsPipeline(GraphicsPipelineId resourceId) = 0;
  virtual void deleteComputePipeline(ComputePipelineId resourceId) = 0;
};

} // namespace rendering
