#pragma once

#include "Command.hpp"
#include <cstddef>

namespace rendering
{

class DrawIndirectCountCommand final : public Command
{
public:
  DrawIndirectCountCommand(BufferView valueIndirectBuffer, size_t valueOffset, BufferView valueCountBuffer, size_t valueCountOffset, uint32_t valueMaxDrawCount, uint32_t valueStride);

  BufferView indirectBuffer;
  size_t offset;
  BufferView countBuffer;
  size_t countOffset;
  uint32_t maxDrawCount;
  uint32_t stride;
  BufferView preparedIndirectBuffer;
  BufferView preparedCountBuffer;
  bool hasPreparedBuffers = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDrawCommand() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
