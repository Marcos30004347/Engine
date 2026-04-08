#include "DrawIndirectCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

DrawIndirectCommand::DrawIndirectCommand(BufferView valueBuffer, uint32_t valueOffset, uint32_t valueDrawCount, uint32_t valueStride)
    : buffer(valueBuffer), offset(valueOffset), drawCount(valueDrawCount), stride(valueStride)
{
}

std::shared_ptr<Command> DrawIndirectCommand::clone() const
{
  return std::make_shared<DrawIndirectCommand>(*this);
}

Queue DrawIndirectCommand::queueHint() const
{
  return Queue::Graphics;
}

bool DrawIndirectCommand::isDrawCommand() const
{
  return true;
}

void DrawIndirectCommand::prepare(CommandPrepareContext &context)
{
  preparedBuffer = buffer;
  hasPreparedBuffer = false;

  if (context.runtimeResources != nullptr)
  {
    preparedBuffer = context.runtimeResources->resolveBufferView(buffer);
    hasPreparedBuffer = true;
  }
}

void DrawIndirectCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDrawIndirect");

  const BufferView &resolvedBuffer = hasPreparedBuffer ? preparedBuffer : buffer;
  BufferView drawBuffer = resolvedBuffer;
  drawBuffer.offset = offset;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] DrawIndirect buffer='" << resolvedBuffer.buffer.name << "' offset=" << offset << " count=" << drawCount << " stride=" << stride;
  logCommand(context, stream.str());

  context.rhi->cmdDrawIndirect(context.commandBuffer, drawBuffer, drawCount, stride);

  endTiming(context);
}

} // namespace rendering
