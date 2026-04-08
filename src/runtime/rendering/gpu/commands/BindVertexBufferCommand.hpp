#pragma once

#include "Command.hpp"

namespace rendering
{

class BindVertexBufferCommand final : public Command
{
public:
  BindVertexBufferCommand(uint32_t valueSlot, BufferView valueBuffer);

  uint32_t slot;
  BufferView buffer;
  BufferView preparedBuffer;
  bool hasPreparedBuffer = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
