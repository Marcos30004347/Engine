#include "StartTimerCommand.hpp"
#include "CommandRunUtils.hpp"

namespace rendering
{

StartTimerCommand::StartTimerCommand(Timer valueTimer, uint32_t valueSampleIndex, PipelineStage valueStage) : timer(valueTimer), sampleIndex(valueSampleIndex), stage(valueStage)
{
}

std::shared_ptr<Command> StartTimerCommand::clone() const
{
  return std::make_shared<StartTimerCommand>(*this);
}

void StartTimerCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdStartTimer");
  context.rhi->cmdStartTimer(context.commandBuffer, timer, sampleIndex, stage);
  endTiming(context);
}

} // namespace rendering
