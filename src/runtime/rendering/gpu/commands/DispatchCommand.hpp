#pragma once

#include "Command.hpp"

namespace rendering
{

class DispatchCommand final : public Command
{
public:
  DispatchCommand(uint32_t valueX, uint32_t valueY, uint32_t valueZ);

  uint32_t x;
  uint32_t y;
  uint32_t z;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDispatchCommand() const override;
  bool triggersNodeSplit() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
