#pragma once

#include "Command.hpp"
#include <vector>

namespace rendering
{

class BindBindingGroupsCommand final : public Command
{
public:
  BindBindingGroupsCommand(
      BindingGroups bindingGroups, const std::vector<uint32_t> &offsets,
      std::shared_ptr<bool> enabled = nullptr);

  BindingGroups groups;
  std::vector<uint32_t> dynamicOffsets;
  BindingGroupsId preparedGroupsId = BindingGroupsId::Invalid;
  bool hasPreparedGroupsId = false;
  bool preparedEnabled = true;

  std::shared_ptr<Command> clone() const override;
  void prepare(CommandPrepareContext &context) override;
  bool isPreparedNoOp() const override;
  void run(CommandRunContext &context) const override;

private:
  std::shared_ptr<bool> enabled_;
};

} // namespace rendering
