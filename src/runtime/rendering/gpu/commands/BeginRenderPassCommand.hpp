#pragma once

#include "Command.hpp"

namespace rendering
{

class BeginRenderPassCommand final : public Command
{
public:
  explicit BeginRenderPassCommand(const RenderPassInfo &value);

  RenderPassInfo info;
  RenderPassInfo preparedInfo;
  bool hasPreparedInfo = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
