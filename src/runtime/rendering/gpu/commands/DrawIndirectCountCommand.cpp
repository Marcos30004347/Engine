#include "DrawIndirectCountCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

DrawIndirectCountCommand::DrawIndirectCountCommand(
    BufferView valueIndirectBuffer,
    size_t valueOffset,
    BufferView valueCountBuffer,
    size_t valueCountOffset,
    uint32_t valueMaxDrawCount,
    uint32_t valueStride)
    : indirectBuffer(valueIndirectBuffer), offset(valueOffset), countBuffer(valueCountBuffer), countOffset(valueCountOffset), maxDrawCount(valueMaxDrawCount), stride(valueStride)
{
}

std::shared_ptr<Command> DrawIndirectCountCommand::clone() const
{
  return std::make_shared<DrawIndirectCountCommand>(*this);
}

Queue DrawIndirectCountCommand::queueHint() const
{
  return Queue::Graphics;
}

bool DrawIndirectCountCommand::isDrawCommand() const
{
  return true;
}

void DrawIndirectCountCommand::prepare(CommandPrepareContext &context)
{
  preparedIndirectBuffer = indirectBuffer;
  preparedCountBuffer = countBuffer;
  hasPreparedBuffers = false;

  if (context.runtimeResources != nullptr)
  {
    preparedIndirectBuffer = context.runtimeResources->resolveBufferView(indirectBuffer);
    preparedCountBuffer = context.runtimeResources->resolveBufferView(countBuffer);
    hasPreparedBuffers = true;
  }
}

void DrawIndirectCountCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDrawIndirectCount");

  const BufferView &resolvedIndirectBuffer = hasPreparedBuffers ? preparedIndirectBuffer : indirectBuffer;
  const BufferView &resolvedCountBuffer = hasPreparedBuffers ? preparedCountBuffer : countBuffer;
  BufferView drawIndirectBuffer = resolvedIndirectBuffer;
  BufferView drawCountBuffer = resolvedCountBuffer;
  drawIndirectBuffer.offset = offset;
  drawCountBuffer.offset = countOffset;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] DrawIndirectCount indirectBuffer='" << resolvedIndirectBuffer.buffer.name << "' offset=" << offset << " countBuffer='" << resolvedCountBuffer.buffer.name
         << "' countOffset=" << countOffset << " maxDraw=" << maxDrawCount << " stride=" << stride;
  logCommand(context, stream.str());

  context.rhi->cmdDrawIndirectCount(context.commandBuffer, drawIndirectBuffer, drawCountBuffer, maxDrawCount, stride);

  endTiming(context);
}

} // namespace rendering
