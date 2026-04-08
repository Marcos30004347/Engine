#include "BindGraphicsPipelineCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

BindGraphicsPipelineCommand::BindGraphicsPipelineCommand(GraphicsPipeline value) : pipeline(value)
{
}

std::shared_ptr<Command> BindGraphicsPipelineCommand::clone() const
{
  return std::make_shared<BindGraphicsPipelineCommand>(*this);
}

Queue BindGraphicsPipelineCommand::queueHint() const
{
  return Queue::Graphics;
}

void BindGraphicsPipelineCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBindGraphicsPipeline");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindGraphicsPipeline '" << pipeline.name << "'";
  logCommand(context, stream.str());

  context.rhi->cmdBindGraphicsPipeline(context.commandBuffer, pipeline);

  endTiming(context);
}

} // namespace rendering
