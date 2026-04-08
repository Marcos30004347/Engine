#pragma once

#include "Command.hpp"

namespace rendering
{

class DrawCommand final : public Command
{
public:
  DrawCommand(uint32_t valueVertexCount, uint32_t valueInstanceCount, uint32_t valueFirstVertex, uint32_t valueFirstInstance);

  uint32_t vertexCount;
  uint32_t instanceCount;
  uint32_t firstVertex;
  uint32_t firstInstance;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool isDrawCommand() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
