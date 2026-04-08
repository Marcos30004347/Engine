#pragma once

#include "Command.hpp"

namespace rendering
{

class DrawIndirectCommand final : public Command
{
public:
  DrawIndirectCommand(BufferView valueBuffer, uint32_t valueOffset, uint32_t valueDrawCount, uint32_t valueStride);

  BufferView buffer;
  uint32_t offset;
  uint32_t drawCount;
  uint32_t stride;
  BufferView preparedBuffer;
  bool hasPreparedBuffer = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDrawCommand() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
