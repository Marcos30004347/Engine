#include "Job.hpp"
#include <assert.h>

#include "os/print.hpp"
namespace async
{

std::atomic<uint32_t> Job::allocations(0);
std::atomic<uint32_t> Job::deallocations(0);

JobAllocator::JobAllocator(size_t maxPayloadSize, size_t reserve) : cache(os::Thread::getHardwareConcurrency()), allocator(), payloadSize(maxPayloadSize), cacheSize(reserve)
{
  assert(reserve >= 1);
  // assert(maxPayloadSize >= sizeof(Job) + 1 * sizeof(size_t));
}

void JobAllocator::initializeThread()
{
  bool inserted = cache.set(os::Thread::getCurrentThreadId(), cacheSize);

  assert(inserted);

  Stack<void *> *local = cache.get(os::Thread::getCurrentThreadId());

  assert(local != nullptr);

  for (size_t i = 0; i < cacheSize; i++)
  {
    void *ptr = (void *)allocator.allocate(payloadSize + sizeof(Job));

    bool pushed = local->push(ptr);
    assert(pushed);
  }
}

void JobAllocator::deinitializeThread()
{
  Stack<void *> *local = cache.get(os::Thread::getCurrentThreadId());

  void *curr = nullptr;

  while (local->pop(curr))
  {
    allocator.deallocate((char *)curr);
  }
}

JobAllocator::~JobAllocator()
{
}

Job *JobAllocator::allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolId)
{
  void *free = nullptr;

  Stack<void *> *local = cache.get(os::Thread::getCurrentThreadId());
  // os::print("job pool achiring %u\n", local->size());

  if (!local->pop(free))
  {
    free = (void *)allocator.allocate(payloadSize + sizeof(Job));
    assert(free != nullptr);
  }

  fiber::Fiber *fiber = nullptr;

  // lib::time::TimeSpan prev1 = lib::time::TimeSpan::now();
  fiber = pool->acquire(handler, free);
  // os::print("achire time = %f\n", (lib::time::TimeSpan::now() - prev1).nanoseconds());

  // lib::time::TimeSpan prev2 = lib::time::TimeSpan::now();
  Job *j = new (free) Job(fiber, poolId, this);
  // os::print("allocated job = %p\n", j);
  return j;
}

Job *JobAllocator::currentThreadToJob()
{
  void *free = nullptr;

  Stack<void *> *local = cache.get(os::Thread::getCurrentThreadId());

  if (!local->pop(free))
  {
    free = (void *)allocator.allocate(payloadSize + sizeof(Job));

    assert(free != nullptr);
  }

  fiber::Fiber *fiber = fiber::Fiber::currentThreadToFiber();
  // os::print("Thread %u fib = %p\n", os::Thread::getCurrentThreadId(), fiber);

  return new (free) Job(fiber, -1, this);
}

uint64_t JobAllocator::getPayloadSize()
{
  return payloadSize;
}

void JobAllocator::deallocate(Job *job)
{
  Stack<void *> *local = cache.get(os::Thread::getCurrentThreadId());
  // uint32_t s = local->size();
  // os::print("Thread %u job pool deallocating %u %p\n", os::Thread::getCurrentThreadId(), 0, job);

  if (!local->push((void *)job))
  {
    allocator.deallocate((char *)job);
  }
  // // os::print("job pool deallocated %u\n", s);
}

} // namespace async