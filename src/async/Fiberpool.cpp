#include "Fiberpool.hpp"
#include "AsyncManager.hpp"

#include "lib/time/TimeSpan.hpp"

using namespace async;
using namespace async::fiber;

FiberPool::FiberPool(uint64_t stackSize, uint64_t threadCacheSize, uint64_t maxThreads)
    : stackSize(stackSize), threadCacheSize(threadCacheSize), maxThreads(maxThreads), cache(os::Thread::getHardwareConcurrency())
{
}

void emptyHandler(void *, Fiber *)
{
}

void FiberPool::initializeThread()
{
  bool inserted = cache.set(os::Thread::getCurrentThreadId(), threadCacheSize);
  
  if (!inserted)
  {
    abort();
  }

  assert(inserted);

  Stack<async::fiber::Fiber *> *local = cache.get(os::Thread::getCurrentThreadId());
  
  if (!local)
  {
    abort();
  }

  assert(local != nullptr);

  for (size_t i = 0; i < threadCacheSize; i++)
  {
    local->push(new Fiber(emptyHandler, nullptr, stackSize));
  }
}

void FiberPool::deinitializeThread()
{
  Stack<async::fiber::Fiber *> *local = cache.get(os::Thread::getCurrentThreadId());

  Fiber *curr = nullptr;

  while (local->pop(curr))
  {
    delete curr;
  }
}

FiberPool::~FiberPool()
{
}

Fiber *FiberPool::acquire(Fiber::Handler func, void *data)
{
  Stack<async::fiber::Fiber *> *local = cache.get(os::Thread::getCurrentThreadId());

  assert(local != nullptr);

  Fiber *fib = nullptr;

  if (local->pop(fib))
  {
    fib->reset(func, data);
  }
  else
  {
    fib = new Fiber(func, data, stackSize);
  }

  return fib;
}

void FiberPool::release(Fiber *fiber)
{
  Stack<async::fiber::Fiber *> *local = cache.get(os::Thread::getCurrentThreadId());

  assert(local != nullptr);

  if (!local->push(fiber))
  {
    delete fiber;
  }
}