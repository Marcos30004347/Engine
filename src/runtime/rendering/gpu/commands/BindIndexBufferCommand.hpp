#pragma once

#include "Command.hpp"

namespace rendering
{

class BindIndexBufferCommand final : public Command
{
public:
  BindIndexBufferCommand(BufferView valueBuffer, Type valueType);

  BufferView buffer;
  Type type;
  BufferView preparedBuffer;
  bool hasPreparedBuffer = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
