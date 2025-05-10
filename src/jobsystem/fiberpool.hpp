#pragma once

#include "fiber.hpp"
#include "lib/parallel/queue.hpp"

namespace jobsystem
{
namespace fiber
{
class FiberPool
{
public:
  static void init();
  static void shutdown();
  static Fiber *acquire(Fiber::Handler, void *userData, size_t stack_size);
  static void release(Fiber *fiber);

private:
  static lib::parallel::Queue<Fiber *> pool[64];
  static std::atomic<uint32_t> poolSize[64];
};

} // namespace fiber
} // namespace jobsystem