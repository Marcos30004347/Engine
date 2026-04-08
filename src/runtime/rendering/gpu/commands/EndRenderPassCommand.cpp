#include "EndRenderPassCommand.hpp"
#include "CommandRunUtils.hpp"

namespace rendering
{

std::shared_ptr<Command> EndRenderPassCommand::clone() const
{
  return std::make_shared<EndRenderPassCommand>(*this);
}

Queue EndRenderPassCommand::queueHint() const
{
  return Queue::Graphics;
}

bool EndRenderPassCommand::triggersNodeSplit() const
{
  return true;
}

void EndRenderPassCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdEndRenderPass");
  logCommand(context, "[RenderGraph][Cmd] EndRenderPass");
  context.rhi->cmdEndRenderPass(context.commandBuffer);
  endTiming(context);
}

} // namespace rendering
