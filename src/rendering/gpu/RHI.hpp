#pragma once

#include "EventLoop.hpp"
#include "Types.hpp"

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

  // 3. Templated Constructor (The Magic)
  // This constructor accepts ANY future type from any RHI (Vulkan, DX12, etc)
  template <typename T> GPUFuture(T &&future) : impl_(std::make_shared<FutureModel<std::decay_t<T>>>(std::forward<T>(future)))
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

  // Helper to get raw pointer for RHI internals (Requires knowing the type)
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

  virtual void bufferMapRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, void **ptr) = 0;
  virtual void bufferUnmap(const Buffer &buffer) = 0;

  virtual void bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data) = 0;

  virtual const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) = 0;
  virtual void destroySwapChain(SwapChain) = 0;

  virtual const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle, uint32_t imageIndex) = 0;

  virtual void beginCommandBuffer(CommandBuffer) = 0;
  virtual void endCommandBuffer(CommandBuffer) = 0;
  virtual void cmdCopyBuffer(CommandBuffer cmdBuffer, Buffer src, Buffer dst, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) = 0;
  virtual void cmdBeginRenderPass(CommandBuffer, const RenderPassInfo &) = 0;
  virtual void cmdEndRenderPass(CommandBuffer) = 0;

  virtual void cmdBindBindingGroups(CommandBuffer cmdBuffer, BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) = 0;
  virtual void cmdBindGraphicsPipeline(CommandBuffer, GraphicsPipeline) = 0;
  virtual void cmdBindComputePipeline(CommandBuffer, ComputePipeline) = 0;
  virtual void cmdBindVertexBuffer(CommandBuffer, uint32_t slot, Buffer, uint64_t offset) = 0;
  virtual void cmdBindIndexBuffer(CommandBuffer, Buffer, Type type, uint64_t offset) = 0;

  virtual void cmdDraw(CommandBuffer, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndexed(CommandBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
  virtual void cmdDrawIndexedIndirect(CommandBuffer, Buffer indirectBuffer, size_t offset, uint32_t drawCount, uint32_t stride) = 0;

  virtual void cmdDispatch(CommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
  virtual void cmdImageBarrier(
      CommandBuffer cmd,
      Texture image,
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
      uint32_t src_queue_family,
      uint32_t dst_queue_family) = 0;
  virtual void cmdBufferBarrier(
      CommandBuffer cmd,
      Buffer b,
      PipelineStage src_stage,
      PipelineStage dst_stage,
      AccessPattern src_access,
      AccessPattern dst_access,
      uint32_t offset,
      uint32_t size,
      uint32_t src_queue_family,
      uint32_t dst_queue_family) = 0;
  virtual void cmdMemoryBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;
  virtual void cmdPipelineBarrier(CommandBuffer cmd, PipelineStage src_stage, PipelineStage dst_stage, AccessPattern src_access, AccessPattern dst_access) = 0;
  virtual GPUFuture submit(Queue queue, CommandBuffer *commandBuffers, uint32_t count, GPUFuture *wait, uint32_t waitCount) = 0;
  virtual void waitIdle() = 0;
  virtual void blockUntil(GPUFuture &) = 0;
  virtual bool isCompleted(GPUFuture &) = 0;
};

} // namespace rendering