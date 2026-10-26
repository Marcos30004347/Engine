#pragma once
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/commands/Command.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"

#include <memory>
#include <utility>

namespace rendering
{
namespace gpgpu
{

class ConditionalCopyBufferCommand final : public Command
{
public:
  ConditionalCopyBufferCommand(
      BufferView source,
      BufferView destination,
      std::shared_ptr<bool> enabled)
      : src(source), dst(destination), enabled_(std::move(enabled))
  {
  }

  std::shared_ptr<Command> clone() const override
  {
    return std::make_shared<ConditionalCopyBufferCommand>(*this);
  }

  Queue queueHint() const override
  {
    return Queue::Transfer;
  }

  bool triggersNodeSplit() const override
  {
    return true;
  }

  bool isTransferOnlyCommand() const override
  {
    return true;
  }

  bool copyBufferViews(BufferView &source, BufferView &destination) const override
  {
    source = src;
    destination = dst;
    return true;
  }

  void prepare(CommandPrepareContext &context) override
  {
    preparedEnabled = enabled_ == nullptr || *enabled_;
    preparedSrc = src;
    preparedDst = dst;
    hasPreparedViews = false;
    if (context.runtimeResources != nullptr)
    {
      preparedSrc = context.runtimeResources->resolveBufferView(src);
      preparedDst = context.runtimeResources->resolveBufferView(dst);
      hasPreparedViews = true;
    }
  }

  void run(CommandRunContext &context) const override
  {
    if (!preparedEnabled)
      return;
    const BufferView &resolvedSrc = hasPreparedViews ? preparedSrc : src;
    const BufferView &resolvedDst = hasPreparedViews ? preparedDst : dst;
    context.rhi->cmdCopyBuffer(context.commandBuffer, resolvedSrc, resolvedDst);
  }

  bool isPreparedNoOp() const override
  {
    return enabled_ != nullptr && !preparedEnabled;
  }

private:
  BufferView src;
  BufferView dst;
  BufferView preparedSrc;
  BufferView preparedDst;
  bool hasPreparedViews = false;
  bool preparedEnabled = true;
  std::shared_ptr<bool> enabled_;
};

class CopyBufferPass : public Pass
{
  rendering::BufferView bufferViewA;
  rendering::BufferView bufferViewB;
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);

public:


  CopyBufferPass(const Buffer &bufferA, uint64_t offsetA, uint64_t sizeA, const Buffer &bufferB, uint64_t offsetB, uint64_t sizeB)
  {
    bufferViewA = BufferView{
      .buffer = bufferA,
      .offset = offsetA,
      .size = sizeA,
      .access = AccessPattern::TRANSFER_READ,
    };
    bufferViewB = BufferView{
      .buffer = bufferB,
      .offset = offsetB,
      .size = sizeB,
      .access = AccessPattern::TRANSFER_WRITE,
    };
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

  bool enabled() const
  {
    return *enabled_;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    commandBuffer.recorded.back().commands.push_back(
        std::make_shared<ConditionalCopyBufferCommand>(
            bufferViewA, bufferViewB, enabled_));
  }
};

} // namespace gpgpu
} // namespace rendering
