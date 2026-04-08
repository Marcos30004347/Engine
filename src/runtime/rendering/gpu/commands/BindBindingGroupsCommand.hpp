#pragma once

#include "Command.hpp"
#include <vector>

namespace rendering
{

class BindBindingGroupsCommand final : public Command
{
public:
  BindBindingGroupsCommand(BindingGroups bindingGroups, const std::vector<uint32_t> &offsets);

  BindingGroups groups;
  std::vector<uint32_t> dynamicOffsets;
  BindingGroupsId preparedGroupsId = BindingGroupsId::Invalid;
  bool hasPreparedGroupsId = false;

  std::shared_ptr<Command> clone() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
