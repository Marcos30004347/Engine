#pragma once

#include "Command.hpp"

namespace rendering
{

class CopyBufferCommand final : public Command
{
public:
  CopyBufferCommand(BufferView source, BufferView destination);

  BufferView src;
  BufferView dst;
  BufferView preparedSrc;
  BufferView preparedDst;
  bool hasPreparedViews = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool triggersNodeSplit() const override;
  bool isTransferOnlyCommand() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
