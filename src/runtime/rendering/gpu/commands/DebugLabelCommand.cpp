#include "DebugLabelCommand.hpp"
#include "CommandRunUtils.hpp"

namespace rendering
{

DebugLabelCommand::DebugLabelCommand(std::string valueLabel, const bool valueBegin)
    : label(std::move(valueLabel)), begin(valueBegin)
{
}

std::shared_ptr<Command> DebugLabelCommand::clone() const
{
  return std::make_shared<DebugLabelCommand>(*this);
}

void DebugLabelCommand::run(CommandRunContext &context) const
{
  beginTiming(context, begin ? "cmdBeginDebugLabel" : "cmdEndDebugLabel");
  if (begin)
    context.rhi->cmdBeginDebugLabel(context.commandBuffer, label.c_str());
  else
    context.rhi->cmdEndDebugLabel(context.commandBuffer);
  endTiming(context);
}

} // namespace rendering
