#pragma once

#include "Command.hpp"

namespace rendering
{

class BindComputePipelineCommand final : public Command
{
public:
  explicit BindComputePipelineCommand(
      ComputePipeline value, std::shared_ptr<bool> enabled = nullptr);

  ComputePipeline pipeline;
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
