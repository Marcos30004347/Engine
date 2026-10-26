#pragma once

// Wraps any recorded command behind a flag read when the frame is prepared.
//
// A pass records its commands once, at registration. A pass that is in the
// graph but has nothing to do this frame - a rewrite pass on a match frame,
// a match pass on a rewrite frame - used to run every one of its dispatches
// as a no-op kernel, which at a dozen dispatches with barriers is most of a
// millisecond of encoding per frame. Wrapped, the commands are prepared no-ops
// and the graph skips them.

#include "rendering/gpu/commands/Command.hpp"

#include <memory>
#include <utility>

namespace rendering {
namespace gpgpu {

class ConditionalCommand final : public Command
{
public:
  ConditionalCommand(std::shared_ptr<Command> inner, std::shared_ptr<bool> enabled) : inner_(std::move(inner)), enabled_(std::move(enabled))
  {
  }

  std::shared_ptr<Command> clone() const override
  {
    return std::make_shared<ConditionalCommand>(inner_->clone(), enabled_);
  }

  Queue queueHint() const override
  {
    return inner_->queueHint();
  }

  bool triggersNodeSplit() const override
  {
    return inner_->triggersNodeSplit();
  }

  bool isDrawCommand() const override
  {
    return inner_->isDrawCommand();
  }

  bool isDispatchCommand() const override
  {
    return inner_->isDispatchCommand();
  }

  bool isTransferOnlyCommand() const override
  {
    return inner_->isTransferOnlyCommand();
  }

  bool copyBufferViews(BufferView &source, BufferView &destination) const override
  {
    return inner_->copyBufferViews(source, destination);
  }

  void externalBufferViews(std::vector<BufferView> &views) const override
  {
    inner_->externalBufferViews(views);
  }

  void externalTextureViews(std::vector<TextureView> &views) const override
  {
    inner_->externalTextureViews(views);
  }

  void prepare(CommandPrepareContext &context) override
  {
    preparedEnabled_ = enabled_ == nullptr || *enabled_;
    inner_->prepare(context);
  }

  bool isPreparedNoOp() const override
  {
    return !preparedEnabled_ || inner_->isPreparedNoOp();
  }

  void run(CommandRunContext &context) const override
  {
    if (preparedEnabled_)
    {
      inner_->run(context);
    }
  }

private:
  std::shared_ptr<Command> inner_;
  std::shared_ptr<bool> enabled_;
  bool preparedEnabled_ = true;
};

// Wraps every DISPATCH recorded since `first` in the sequence being recorded.
// Binds and barriers stay as they are: the graph validates a sequence by the
// concrete type of its bind commands, and a bind with no dispatch after it
// costs nothing.
inline void guardRecordedCommands(::rendering::CommandRecorder &commandBuffer, const size_t first, const std::shared_ptr<bool> &enabled)
{
  auto &commands = commandBuffer.recorded.back().commands;

  for (size_t i = first; i < commands.size(); ++i)
  {
    if (commands[i]->isDispatchCommand())
    {
      commands[i] = std::make_shared<ConditionalCommand>(commands[i], enabled);
    }
  }
}

} // namespace gpgpu
} // namespace rendering
