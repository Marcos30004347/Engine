#pragma once

#include "rendering/gpu/RHI.hpp"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace rendering
{

class RenderGraph;

using CommandBuffersByQueue = std::array<std::vector<CommandBuffer>, Queue::QueuesCount>;

struct RenderGraphRuntimeCallbacks
{
  std::function<void(const char *opName)> beginRHITiming;
  std::function<void()> endRHITiming;
  std::function<void(const char *scopeName)> beginScopedTiming;
  std::function<void()> endScopedTiming;
  std::function<void(const std::string &message)> logBarrier;
};

} // namespace rendering
