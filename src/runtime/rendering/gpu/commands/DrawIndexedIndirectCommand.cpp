#include "DrawIndexedIndirectCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

DrawIndexedIndirectCommand::DrawIndexedIndirectCommand(BufferView valueBuffer, uint32_t valueOffset, uint32_t valueDrawCount, uint32_t valueStride)
    : buffer(valueBuffer), offset(valueOffset), drawCount(valueDrawCount), stride(valueStride)
{
}

std::shared_ptr<Command> DrawIndexedIndirectCommand::clone() const
{
  return std::make_shared<DrawIndexedIndirectCommand>(*this);
}

Queue DrawIndexedIndirectCommand::queueHint() const
{
  return Queue::Graphics;
}

bool DrawIndexedIndirectCommand::isDrawCommand() const
{
  return true;
}

void DrawIndexedIndirectCommand::prepare(CommandPrepareContext &context)
{
  preparedBuffer = buffer;
  hasPreparedBuffer = false;

  if (context.runtimeResources != nullptr)
  {
    preparedBuffer = context.runtimeResources->resolveBufferView(buffer);
    hasPreparedBuffer = true;
  }
}

void DrawIndexedIndirectCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDrawIndexedIndirect");

  const BufferView &resolvedBuffer = hasPreparedBuffer ? preparedBuffer : buffer;
  BufferView drawBuffer = resolvedBuffer;
  drawBuffer.offset = offset;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] DrawIndexedIndirect buffer='" << resolvedBuffer.buffer.name << "' offset=" << offset << " count=" << drawCount << " stride=" << stride;
  logCommand(context, stream.str());

  context.rhi->cmdDrawIndexedIndirect(context.commandBuffer, drawBuffer, drawCount, stride);

  endTiming(context);
}

} // namespace rendering
