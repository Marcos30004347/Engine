#pragma once

#include "Command.hpp"

#include <string>

namespace rendering
{

// A named region in the recorded command stream.
//
// This exists for external GPU profilers. The engine's own timestamp
// instrumentation measures a pass by writing timestamps around it, which on a
// translation layer bills any stall before the pass to the pass itself; a
// capture tool reading debug regions attributes the same work to the encoder
// that actually ran it. Backends without label support ignore these.
class DebugLabelCommand final : public Command
{
public:
  DebugLabelCommand(std::string valueLabel, bool valueBegin);

  std::string label;
  bool begin = true;

  std::shared_ptr<Command> clone() const override;
  void run(CommandRunContext &context) const override;
};

} // namespace rendering
