#include "MemoryBarrierCommand.hpp"
#include "CommandRunUtils.hpp"

namespace rendering
{

MemoryBarrierCommand::MemoryBarrierCommand(
    const PipelineStage inSourceStage,
    const PipelineStage inDestinationStage,
    const AccessPattern inSourceAccess,
    const AccessPattern inDestinationAccess,
    std::shared_ptr<bool> enabled)
    : sourceStage(inSourceStage), destinationStage(inDestinationStage),
      sourceAccess(inSourceAccess), destinationAccess(inDestinationAccess),
      enabled_(std::move(enabled))
{
}

std::shared_ptr<Command> MemoryBarrierCommand::clone() const
{
  return std::make_shared<MemoryBarrierCommand>(*this);
}

Queue MemoryBarrierCommand::queueHint() const
{
  return Queue::Compute;
}

void MemoryBarrierCommand::prepare(CommandPrepareContext &)
{
  preparedEnabled = enabled_ == nullptr || *enabled_;
}

bool MemoryBarrierCommand::isPreparedNoOp() const
{
  return enabled_ != nullptr && !preparedEnabled;
}

void MemoryBarrierCommand::run(CommandRunContext &context) const
{
  if (!preparedEnabled)
    return;
  beginTiming(context, "cmdMemoryBarrier");
  context.rhi->cmdMemoryBarrier(
      context.commandBuffer, sourceStage, destinationStage,
      sourceAccess, destinationAccess);
  endTiming(context);
}

} // namespace rendering
