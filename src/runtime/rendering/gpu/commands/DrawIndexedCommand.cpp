#include "DrawIndexedCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

DrawIndexedCommand::DrawIndexedCommand(uint32_t valueIndexCount, uint32_t valueInstanceCount, uint32_t valueFirstIndex, int32_t valueVertexOffset, uint32_t valueFirstInstance)
    : indexCount(valueIndexCount), instanceCount(valueInstanceCount), firstIndex(valueFirstIndex), vertexOffset(valueVertexOffset), firstInstance(valueFirstInstance)
{
}

std::shared_ptr<Command> DrawIndexedCommand::clone() const
{
  return std::make_shared<DrawIndexedCommand>(*this);
}

Queue DrawIndexedCommand::queueHint() const
{
  return Queue::Graphics;
}

bool DrawIndexedCommand::isDrawCommand() const
{
  return true;
}

void DrawIndexedCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDrawIndexed");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] DrawIndexed indices=" << indexCount << " instances=" << instanceCount << " firstIndex=" << firstIndex << " vertexOffset=" << vertexOffset << " firstInstance=" << firstInstance;
  logCommand(context, stream.str());

  context.rhi->cmdDrawIndexed(context.commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);

  endTiming(context);
}

} // namespace rendering
