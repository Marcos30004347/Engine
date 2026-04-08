#pragma once

#include "Command.hpp"

namespace rendering
{

class EndRenderPassCommand final : public Command
{
public:
  EndRenderPassCommand() = default;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool triggersNodeSplit() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
