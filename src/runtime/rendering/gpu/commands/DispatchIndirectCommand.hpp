#pragma once

#include "Command.hpp"

namespace rendering
{

class DispatchIndirectCommand final : public Command
{
public:
  DispatchIndirectCommand(
      Buffer valueIndirectBuffer, uint64_t valueOffset,
      std::shared_ptr<bool> enabled = nullptr);

  Buffer indirectBuffer;
  uint64_t offset;
  BufferId preparedIndirectBufferId = BufferId::Invalid;
  bool hasPreparedBufferId = false;
  bool preparedEnabled = true;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDispatchCommand() const override;
  bool triggersNodeSplit() const override;
  void prepare(CommandPrepareContext &context) override;
  bool isPreparedNoOp() const override;
  void run(CommandRunContext &context) const override;

private:
  std::shared_ptr<bool> enabled_;
};

} // namespace rendering
