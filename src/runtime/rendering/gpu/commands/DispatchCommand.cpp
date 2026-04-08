#include "DispatchCommand.hpp"
#include "CommandRunUtils.hpp"
#include <sstream>

namespace rendering
{

DispatchCommand::DispatchCommand(uint32_t valueX, uint32_t valueY, uint32_t valueZ) : x(valueX), y(valueY), z(valueZ)
{
}

std::shared_ptr<Command> DispatchCommand::clone() const
{
  return std::make_shared<DispatchCommand>(*this);
}

Queue DispatchCommand::queueHint() const
{
  return Queue::Compute;
}

bool DispatchCommand::isDispatchCommand() const
{
  return true;
}

bool DispatchCommand::triggersNodeSplit() const
{
  return true;
}

void DispatchCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdDispatch");

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] Dispatch commandBuffer=" << static_cast<uint64_t>(context.commandBuffer) << " (" << x << ", " << y << ", " << z << ")";
  logCommand(context, stream.str());

  context.rhi->cmdDispatch(context.commandBuffer, x, y, z);

  endTiming(context);
}

} // namespace rendering
