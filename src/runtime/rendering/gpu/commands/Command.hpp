#pragma once

#include "rendering/gpu/RHI.hpp"
#include <functional>
#include <memory>
#include <string>

namespace rendering
{

class Command;
class RenderGraphRuntimeResourcesManager;

struct CommandRunCallbacks
{
  std::function<void(const char *opName)> beginTiming;
  std::function<void()> endTiming;
  std::function<void(const std::string &message)> log;
};

struct CommandRunContext
{
  RHI *rhi = nullptr;
  CommandBuffer commandBuffer = {};
  RenderGraphRuntimeResourcesManager *runtimeResources = nullptr;
  CommandRunCallbacks callbacks;
};

struct CommandPrepareContext
{
  RenderGraphRuntimeResourcesManager *runtimeResources = nullptr;
};

class Command
{
public:
  virtual ~Command() = default;

  virtual std::shared_ptr<Command> clone() const = 0;

  virtual Queue queueHint() const
  {
    return Queue::None;
  }

  virtual bool triggersNodeSplit() const
  {
    return false;
  }

  virtual bool isDrawCommand() const
  {
    return false;
  }

  virtual bool isDispatchCommand() const
  {
    return false;
  }

  virtual bool isTransferOnlyCommand() const
  {
    return false;
  }

  virtual void prepare(CommandPrepareContext &context)
  {
    (void)context;
  }

  virtual void run(CommandRunContext &context) const = 0;
};

using CommandPtr = std::shared_ptr<Command>;

} // namespace rendering
