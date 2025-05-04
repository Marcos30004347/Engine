#include "fiberpool.hpp"
#include "jobsystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

lib::parallel::Queue<Fiber *> FiberPool::pool;

Fiber *FiberPool::acquire(Fiber::Handler func, void *data)
{
  Fiber *fiber;

  if (pool.dequeue(fiber))
  {
    fiber->reset(func, data);
    return fiber;
  }

  return new Fiber(func, data);
}

void FiberPool::release(Fiber *fiber)
{
  pool.enqueue(fiber);
}