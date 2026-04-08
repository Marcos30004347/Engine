#include "BeginRenderPassCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

BeginRenderPassCommand::BeginRenderPassCommand(const RenderPassInfo &value) : info(value)
{
}

std::shared_ptr<Command> BeginRenderPassCommand::clone() const
{
  return std::make_shared<BeginRenderPassCommand>(*this);
}

Queue BeginRenderPassCommand::queueHint() const
{
  return Queue::Graphics;
}

void BeginRenderPassCommand::prepare(CommandPrepareContext &context)
{
  preparedInfo = info;
  hasPreparedInfo = false;

  if (context.runtimeResources != nullptr)
  {
    preparedInfo = context.runtimeResources->resolveRenderPassInfo(info);
    hasPreparedInfo = true;
  }
}

void BeginRenderPassCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBeginRenderPass");

  const RenderPassInfo &resolvedInfo = hasPreparedInfo ? preparedInfo : info;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BeginRenderPass '" << resolvedInfo.name << "'";
  logCommand(context, stream.str());

  context.rhi->cmdBeginRenderPass(context.commandBuffer, resolvedInfo);

  endTiming(context);
}

} // namespace rendering
