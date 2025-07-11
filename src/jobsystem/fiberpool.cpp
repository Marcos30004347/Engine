#include "Fiberpool.hpp"
#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

FiberPool::FiberPool(uint64_t stackSize) : stackSize(stackSize), pool()
{
}

FiberPool::~FiberPool()
{
  Fiber *fiber = nullptr;

  while (pool.tryDequeue(fiber))
  {
    delete fiber;
  }
}

Fiber *FiberPool::acquire(Fiber::Handler func, void *data)
{
  Fiber *fiber;

  if (pool.tryDequeue(fiber))
  {
    fiber->reset(func, data);
    return fiber;
  }

  return new Fiber(func, data, stackSize);
}

void FiberPool::release(Fiber *fiber)
{
  pool.enqueue(fiber);
}