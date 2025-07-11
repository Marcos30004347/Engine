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
  FiberPool(uint64_t stackSize);
  ~FiberPool();
  Fiber *acquire(Fiber::Handler, void *userData);
  void release(Fiber *fiber);
  
  inline uint64_t getStackSize()
  {
    return stackSize;
  }

private:
  uint64_t stackSize;
  lib::ConcurrentQueue<Fiber *> pool;
};

} // namespace fiber
} // namespace jobsystem