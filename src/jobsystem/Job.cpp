#include "Job.hpp"
#include <assert.h>

namespace jobsystem
{
/*
template <> std::shared_ptr<Job> allocateJob<void>(fiber::Fiber *f)
{
return std::make_shared<Job>(f);
}
*/

JobAllocator::JobAllocator(size_t maxPayloadSize, size_t capacity) : freeList(), allocator(), payloadSize(maxPayloadSize)
{
  assert(capacity >= 1);
  assert(maxPayloadSize >= sizeof(Job) + 1 * sizeof(size_t));

  for (size_t i = 0; i < capacity; i++)
  {
    freeList.enqueue((void *)allocator.allocate(maxPayloadSize + sizeof(Job)));
  }
}

JobAllocator::~JobAllocator()
{
  void *data = nullptr;

  while (freeList.tryDequeue(data))
  {
    allocator.deallocate((char *)data);
  }
}

std::shared_ptr<Job> JobAllocator::allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolId)
{
  void *free = nullptr;

  if (!freeList.tryDequeue(free))
  {
    return nullptr;
  }

  fiber::Fiber *fiber = nullptr;
  fiber = pool->acquire(handler, free);
os::print("job = %p, fiber = %p\n", free, fiber);

  return std::shared_ptr<Job>(
      new (free) Job(fiber, poolId),
      [this](Job *j)
      {
        os::print("deallocating %p\n", j);
        // pool->release(fiber);
        j->~Job();
        this->deallocate(j);
      });
}

std::shared_ptr<Job> JobAllocator::currentThreadToJob()
{
  void *free = nullptr;

  freeList.tryDequeue(free);
  if (free == nullptr)
  {
    return nullptr;
  }

  fiber::Fiber *fiber = fiber::Fiber::currentThreadToFiber();

  return std::shared_ptr<Job>(
      new (free) Job(fiber, -1),
      [this](Job *j)
      {
        os::print("deallocating thread %p\n", j);
        j->~Job();
        this->deallocate(j);
      });
}

uint64_t JobAllocator::getPayloadSize()
{
  return payloadSize;
}

void JobAllocator::deallocate(Job *job)
{
  freeList.enqueue((void *)job);
}

} // namespace jobsystem