#pragma once

#include "Command.hpp"

namespace rendering
{

class BindComputePipelineCommand final : public Command
{
public:
  explicit BindComputePipelineCommand(ComputePipeline value);

  ComputePipeline pipeline;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
