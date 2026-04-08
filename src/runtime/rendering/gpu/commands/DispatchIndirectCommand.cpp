#include "DispatchIndirectCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

DispatchIndirectCommand::DispatchIndirectCommand(Buffer valueIndirectBuffer, uint64_t valueOffset) : indirectBuffer(valueIndirectBuffer), offset(valueOffset)
{
}

std::shared_ptr<Command> DispatchIndirectCommand::clone() const
{
  return std::make_shared<DispatchIndirectCommand>(*this);
}

Queue DispatchIndirectCommand::queueHint() const
{
  return Queue::Compute;
}

bool DispatchIndirectCommand::isDispatchCommand() const
{
  return true;
}

bool DispatchIndirectCommand::triggersNodeSplit() const
{
  return true;
}

void DispatchIndirectCommand::prepare(CommandPrepareContext &context)
{
  preparedIndirectBufferId = BufferId::Invalid;
  hasPreparedBufferId = false;

  if (context.runtimeResources != nullptr)
  {
    preparedIndirectBufferId = context.runtimeResources->resolveBuffer(indirectBuffer);
    hasPreparedBufferId = true;
  }
}

void DispatchIndirectCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDispatchIndirect");

  const BufferId resolvedIndirectBufferId = hasPreparedBufferId ? preparedIndirectBufferId : BufferId::Invalid;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] DispatchIndirect commandBuffer=" << static_cast<uint64_t>(context.commandBuffer) << " offset=" << offset;
  logCommand(context, stream.str());

  BufferView resolvedIndirectBuffer = {
    .buffer = indirectBuffer,
    .resourceId = resolvedIndirectBufferId,
    .offset = offset,
  };
  context.rhi->cmdDispatchIndirect(context.commandBuffer, resolvedIndirectBuffer);

  endTiming(context);
}

} // namespace rendering
