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
  static Fiber *acquire(Fiber::Handler, void* userData);
  static void release(Fiber *fiber);
private:
  static lib::parallel::Queue<Fiber *> pool;
};

} // namespace fiber
} // namespace jobsystem