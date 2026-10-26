#pragma once

#include "Command.hpp"

namespace rendering
{

class BindGraphicsPipelineCommand final : public Command
{
public:
  explicit BindGraphicsPipelineCommand(GraphicsPipeline value);

  GraphicsPipeline pipeline;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
