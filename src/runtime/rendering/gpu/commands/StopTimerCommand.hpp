#pragma once

#include "Command.hpp"

namespace rendering
{

class StopTimerCommand final : public Command
{
public:
  StopTimerCommand(Timer valueTimer, uint32_t valueSampleIndex, PipelineStage valueStage);

  Timer timer;
  uint32_t sampleIndex = 0u;
  PipelineStage stage;

  std::shared_ptr<Command> clone() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
