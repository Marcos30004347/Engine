#pragma once

#include "Fiber.hpp"

#include "lib/datastructure/ConcurrentQueue.hpp"
#include "lib/datastructure/ThreadLocalStorage.hpp"

#include "ThreadCache.hpp"
#include "Stack.hpp"

namespace async
{
namespace fiber
{
class FiberPool
{
public:
  FiberPool(uint64_t stackSize, uint64_t threadCacheSize, uint64_t maxThreads);

  void initializeThread();
  void deinitializeThread();

  ~FiberPool();

  Fiber *acquire(Fiber::Handler, void *userData);

  void release(Fiber *fiber);

  inline uint64_t getStackSize()
  {
    return stackSize;
  }

private:
  uint64_t threadCacheSize;
  uint64_t stackSize;
  uint64_t maxThreads;

  ThreadCache<Stack<Fiber *>> cache;
};

} // namespace fiber
} // namespace async