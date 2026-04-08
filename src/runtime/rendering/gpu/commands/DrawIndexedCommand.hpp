#pragma once

#include "Command.hpp"

namespace rendering
{

class DrawIndexedCommand final : public Command
{
public:
  DrawIndexedCommand(uint32_t valueIndexCount, uint32_t valueInstanceCount, uint32_t valueFirstIndex, int32_t valueVertexOffset, uint32_t valueFirstInstance);

  uint32_t indexCount;
  uint32_t instanceCount;
  uint32_t firstIndex;
  int32_t vertexOffset;
  uint32_t firstInstance;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDrawCommand() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
