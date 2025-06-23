#pragma once

#include "Fiber.hpp"

#include "lib/datastructure/ConcurrentQueue.hpp"

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
  static lib::ConcurrentQueue<Fiber *> pool;
};

} // namespace fiber
} // namespace jobsystem