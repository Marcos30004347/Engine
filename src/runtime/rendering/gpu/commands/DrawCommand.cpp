#include "DrawCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

DrawCommand::DrawCommand(uint32_t valueVertexCount, uint32_t valueInstanceCount, uint32_t valueFirstVertex, uint32_t valueFirstInstance)
    : vertexCount(valueVertexCount), instanceCount(valueInstanceCount), firstVertex(valueFirstVertex), firstInstance(valueFirstInstance)
{
}

std::shared_ptr<Command> DrawCommand::clone() const
{
  return std::make_shared<DrawCommand>(*this);
}

Queue DrawCommand::queueHint() const
{
  return Queue::Graphics;
}

bool DrawCommand::isDrawCommand() const
{
  return true;
}

void DrawCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDraw");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] Draw vertices=" << vertexCount << " instances=" << instanceCount << " firstVertex=" << firstVertex << " firstInstance=" << firstInstance;
  logCommand(context, stream.str());

  context.rhi->cmdDraw(context.commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);

  endTiming(context);
}

} // namespace rendering
