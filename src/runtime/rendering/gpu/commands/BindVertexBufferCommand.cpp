#include "BindVertexBufferCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

BindVertexBufferCommand::BindVertexBufferCommand(uint32_t valueSlot, BufferView valueBuffer) : slot(valueSlot), buffer(valueBuffer)
{
}

std::shared_ptr<Command> BindVertexBufferCommand::clone() const
{
  return std::make_shared<BindVertexBufferCommand>(*this);
}

Queue BindVertexBufferCommand::queueHint() const
{
  return Queue::Graphics;
}

void BindVertexBufferCommand::prepare(CommandPrepareContext &context)
{
  preparedBuffer = buffer;
  hasPreparedBuffer = false;

  if (context.runtimeResources != nullptr)
  {
    preparedBuffer = context.runtimeResources->resolveBufferView(buffer);
    hasPreparedBuffer = true;
  }
}

void BindVertexBufferCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBindVertexBuffer");

  const BufferView &resolvedBuffer = hasPreparedBuffer ? preparedBuffer : buffer;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindVertexBuffer slot=" << slot << " '" << resolvedBuffer.buffer.name << "' offset=" << resolvedBuffer.offset;
  logCommand(context, stream.str());

  context.rhi->cmdBindVertexBuffer(context.commandBuffer, slot, resolvedBuffer);

  endTiming(context);
}

} // namespace rendering
