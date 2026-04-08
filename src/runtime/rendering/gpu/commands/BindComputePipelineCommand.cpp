#include "BindComputePipelineCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

BindComputePipelineCommand::BindComputePipelineCommand(ComputePipeline value) : pipeline(value)
{
}

std::shared_ptr<Command> BindComputePipelineCommand::clone() const
{
  return std::make_shared<BindComputePipelineCommand>(*this);
}

Queue BindComputePipelineCommand::queueHint() const
{
  return Queue::Compute;
}

void BindComputePipelineCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBindComputePipeline");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindComputePipeline '" << pipeline.name << "'";
  logCommand(context, stream.str());

  context.rhi->cmdBindComputePipeline(context.commandBuffer, pipeline);

  endTiming(context);
}

} // namespace rendering
