#include "BindBindingGroupsCommand.hpp"
#include "CommandRunUtils.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <sstream>

namespace rendering
{

BindBindingGroupsCommand::BindBindingGroupsCommand(BindingGroups bindingGroups, const std::vector<uint32_t> &offsets) : groups(bindingGroups), dynamicOffsets(offsets)
{
}

std::shared_ptr<Command> BindBindingGroupsCommand::clone() const
{
  return std::make_shared<BindBindingGroupsCommand>(*this);
}

void BindBindingGroupsCommand::prepare(CommandPrepareContext &context)
{
  preparedGroupsId = BindingGroupsId::Invalid;
  hasPreparedGroupsId = false;

  if (context.runtimeResources != nullptr)
  {
    preparedGroupsId = context.runtimeResources->resolveBindingGroups(groups);
    hasPreparedGroupsId = true;
  }
}

void BindBindingGroupsCommand::run(CommandRunContext &context) const
{
  beginTiming(context, "cmdBindBindingGroups");

  const BindingGroupsId resolvedGroupsId = hasPreparedGroupsId ? preparedGroupsId : BindingGroupsId::Invalid;

  std::stringstream stream;
  stream << "[RenderGraph][Cmd] BindBindingGroups '" << groups.name << "' dynamicOffsets=" << dynamicOffsets.size();
  logCommand(context, stream.str());

  context.rhi->cmdBindBindingGroups(context.commandBuffer, resolvedGroupsId, const_cast<uint32_t *>(dynamicOffsets.data()), dynamicOffsets.size());

  endTiming(context);
}

} // namespace rendering
