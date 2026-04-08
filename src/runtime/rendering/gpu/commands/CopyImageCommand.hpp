#pragma once

#include "Command.hpp"

namespace rendering
{

class CopyImageCommand final : public Command
{
public:
  CopyImageCommand(TextureView source, TextureView destination);

  TextureView src;
  TextureView dst;
  TextureView preparedSrc;
  TextureView preparedDst;
  bool hasPreparedViews = false;

  std::shared_ptr<Command> clone() const override;
  Queue queueHint() const override;
  bool triggersNodeSplit() const override;
  bool isTransferOnlyCommand() const override;
  void prepare(CommandPrepareContext &context) override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
