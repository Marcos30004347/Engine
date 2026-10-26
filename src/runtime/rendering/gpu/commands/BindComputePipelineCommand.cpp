#include "BindComputePipelineCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

BindComputePipelineCommand::BindComputePipelineCommand(
    ComputePipeline value, std::shared_ptr<bool> enabled)
    : pipeline(value), enabled_(std::move(enabled))
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

void BindComputePipelineCommand::prepare(CommandPrepareContext &)
{
  preparedEnabled = enabled_ == nullptr || *enabled_;
}

bool BindComputePipelineCommand::isPreparedNoOp() const
{
  return enabled_ != nullptr && !preparedEnabled;
}

void BindComputePipelineCommand::run(CommandRunContext &context) const
{
  if (!preparedEnabled)
    return;
  beginTiming(context, "cmdBindComputePipeline");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindComputePipeline '" << pipeline.name << "'";
  logCommand(context, stream.str());

  context.rhi->cmdBindComputePipeline(context.commandBuffer, pipeline);

  endTiming(context);
}

} // namespace rendering
