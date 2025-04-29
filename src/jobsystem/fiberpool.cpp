#include "fiberpool.hpp"
#include "jobsystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

std::queue<Fiber *> FiberPool::pool;
std::mutex FiberPool::pool_mutex;

Fiber *FiberPool::acquire(Fiber::Handler func, void* data)
{
  std::lock_guard<std::mutex> l(pool_mutex);

  if (!pool.empty())
  {
    Fiber *fiber = pool.front();
    pool.pop();
    fiber->reset(func, data);
    return fiber;
  }

  return new Fiber(func, data);
}

void FiberPool::release(Fiber *fiber)
{
  std::lock_guard<std::mutex> l(pool_mutex);
  pool.push(fiber);
}