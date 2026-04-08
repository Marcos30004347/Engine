#include "CopyBufferCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

CopyBufferCommand::CopyBufferCommand(BufferView source, BufferView destination) : src(source), dst(destination)
{
}

std::shared_ptr<Command> CopyBufferCommand::clone() const
{
  return std::make_shared<CopyBufferCommand>(*this);
}

Queue CopyBufferCommand::queueHint() const
{
  return Queue::Transfer;
}

bool CopyBufferCommand::triggersNodeSplit() const
{
  return true;
}

bool CopyBufferCommand::isTransferOnlyCommand() const
{
  return true;
}

void CopyBufferCommand::prepare(CommandPrepareContext &context)
{
  preparedSrc = src;
  preparedDst = dst;
  hasPreparedViews = false;

  if (context.runtimeResources != nullptr)
  {
    preparedSrc = context.runtimeResources->resolveBufferView(src);
    preparedDst = context.runtimeResources->resolveBufferView(dst);
    hasPreparedViews = true;
  }
}

void CopyBufferCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdCopyBuffer");

  const BufferView &resolvedSrc = hasPreparedViews ? preparedSrc : src;
  const BufferView &resolvedDst = hasPreparedViews ? preparedDst : dst;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] CopyBuffer commandBuffer=" << static_cast<uint64_t>(context.commandBuffer) << " '" << resolvedSrc.buffer.name << "'[" << resolvedSrc.offset << "] -> '"
         << resolvedDst.buffer.name << "'[" << resolvedDst.offset << "] size=" << resolvedSrc.size;
  logCommand(context, stream.str());

  context.rhi->cmdCopyBuffer(context.commandBuffer, resolvedSrc, resolvedDst);

  endTiming(context);
}

} // namespace rendering
