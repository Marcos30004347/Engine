#include "Fiberpool.hpp"
#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

lib::ConcurrentQueue<Fiber *> FiberPool::pool;

void FiberPool::init()
{
}

void FiberPool::shutdown()
{
}

Fiber *FiberPool::acquire(Fiber::Handler func, void *data, size_t stack_size)
{
  Fiber *fiber;

  if (pool.tryDequeue(fiber))
  {
    fiber->reset(func, data);
    return fiber;
  }

  return new Fiber(func, data, stack_size);
}

void FiberPool::release(Fiber *fiber)
{
  pool.enqueue(fiber);
}