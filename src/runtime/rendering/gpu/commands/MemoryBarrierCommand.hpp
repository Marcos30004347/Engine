#pragma once

#include "Command.hpp"

namespace rendering
{

class MemoryBarrierCommand final : public Command
{
public:
  MemoryBarrierCommand(
      PipelineStage sourceStage,
      PipelineStage destinationStage,
      AccessPattern sourceAccess,
      AccessPattern destinationAccess,
      std::shared_ptr<bool> enabled = nullptr);

  PipelineStage sourceStage;
  PipelineStage destinationStage;
  AccessPattern sourceAccess;
  AccessPattern destinationAccess;
  bool preparedEnabled = true;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void prepare(CommandPrepareContext &context) override;
  bool isPreparedNoOp() const override;
  void run(CommandRunContext &context) const override;

private:
  std::shared_ptr<bool> enabled_;
};

} // namespace rendering
