#include "fiberpool.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

std::queue<Fiber *> FiberPool::pool;
std::mutex FiberPool::pool_mutex;

Fiber *FiberPool::acquire(std::function<void()> fn)
{
  std::lock_guard<std::mutex> l(pool_mutex);

  if (!pool.empty())
  {
    Fiber *fiber = pool.front();
    pool.pop();
    fiber->reset(std::move(fn));
    return fiber;
  }

  return new Fiber(std::move(fn));
}

void FiberPool::release(Fiber *fiber)
{
  std::lock_guard<std::mutex> l(pool_mutex);
  pool.push(fiber);
}