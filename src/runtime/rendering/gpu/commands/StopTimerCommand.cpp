#include "StopTimerCommand.hpp"
#include "CommandRunUtils.hpp"

namespace rendering
{

StopTimerCommand::StopTimerCommand(Timer valueTimer, uint32_t valueSampleIndex, PipelineStage valueStage) : timer(valueTimer), sampleIndex(valueSampleIndex), stage(valueStage)
{
}

std::shared_ptr<Command> StopTimerCommand::clone() const
{
  return std::make_shared<StopTimerCommand>(*this);
}

void StopTimerCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdStopTimer");
  context.rhi->cmdStopTimer(context.commandBuffer, timer, sampleIndex, stage);
  endTiming(context);
}

} // namespace rendering
