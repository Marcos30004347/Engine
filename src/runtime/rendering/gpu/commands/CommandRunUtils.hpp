#pragma once

#include "Command.hpp"

namespace rendering
{

inline void beginTiming(CommandRunContext &context, const char *name)
{
  if (context.callbacks.beginTiming)
  {
    context.callbacks.beginTiming(name);
  }
}

inline void endTiming(CommandRunContext &context)
{
  if (context.callbacks.endTiming)
  {
    context.callbacks.endTiming();
  }
}

inline void logCommand(CommandRunContext &context, const std::string &message)
{
  if (context.callbacks.log)
  {
    context.callbacks.log(message);
  }
}

} // namespace rendering
