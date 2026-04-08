#include "CopyImageCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

CopyImageCommand::CopyImageCommand(TextureView source, TextureView destination) : src(source), dst(destination)
{
}

std::shared_ptr<Command> CopyImageCommand::clone() const
{
  return std::make_shared<CopyImageCommand>(*this);
}

Queue CopyImageCommand::queueHint() const
{
  return Queue::Transfer;
}

bool CopyImageCommand::triggersNodeSplit() const
{
  return true;
}

bool CopyImageCommand::isTransferOnlyCommand() const
{
  return true;
}

void CopyImageCommand::prepare(CommandPrepareContext &context)
{
  preparedSrc = src;
  preparedDst = dst;
  hasPreparedViews = false;

  if (context.runtimeResources != nullptr)
  {
    preparedSrc = context.runtimeResources->resolveTextureView(src);
    preparedDst = context.runtimeResources->resolveTextureView(dst);
    hasPreparedViews = true;
  }
}

void CopyImageCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdCopyImage");

  const TextureView &resolvedSrc = hasPreparedViews ? preparedSrc : src;
  const TextureView &resolvedDst = hasPreparedViews ? preparedDst : dst;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] CopyImage commandBuffer=" << static_cast<uint64_t>(context.commandBuffer) << " '" << resolvedSrc.texture.name << "' -> '" << resolvedDst.texture.name << "'";
  logCommand(context, stream.str());

  context.rhi->cmdCopyImage(context.commandBuffer, resolvedSrc, resolvedDst);

  endTiming(context);
}

} // namespace rendering
