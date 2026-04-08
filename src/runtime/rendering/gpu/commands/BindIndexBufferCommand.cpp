#include "BindIndexBufferCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

BindIndexBufferCommand::BindIndexBufferCommand(BufferView valueBuffer, Type valueType) : buffer(valueBuffer), type(valueType)
{
}

std::shared_ptr<Command> BindIndexBufferCommand::clone() const
{
  return std::make_shared<BindIndexBufferCommand>(*this);
}

Queue BindIndexBufferCommand::queueHint() const
{
  return Queue::Graphics;
}

void BindIndexBufferCommand::prepare(CommandPrepareContext &context)
{
  preparedBuffer = buffer;
  hasPreparedBuffer = false;

  if (context.runtimeResources != nullptr)
  {
    preparedBuffer = context.runtimeResources->resolveBufferView(buffer);
    hasPreparedBuffer = true;
  }
}

void BindIndexBufferCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBindIndexBuffer");

  const BufferView &resolvedBuffer = hasPreparedBuffer ? preparedBuffer : buffer;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindIndexBuffer '" << resolvedBuffer.buffer.name << "' offset=" << resolvedBuffer.offset << " type=" << static_cast<uint32_t>(type);
  logCommand(context, stream.str());

  context.rhi->cmdBindIndexBuffer(context.commandBuffer, resolvedBuffer, type);

  endTiming(context);
}

} // namespace rendering
