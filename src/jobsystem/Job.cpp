#include "Job.hpp"
#include <assert.h>

namespace jobsystem
{

std::atomic<uint32_t> Job::allocations(0);
std::atomic<uint32_t> Job::deallocations(0);

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

Job *JobAllocator::allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolId)
{
  void *free = nullptr;

  if (!freeList.tryDequeue(free))
  {
    return nullptr;
  }

  fiber::Fiber *fiber = nullptr;
  fiber = pool->acquire(handler, free);
  // os::print("job = %p, fiber = %p\n", free, fiber);

  return new (free) Job(fiber, poolId, this);
}

Job *JobAllocator::currentThreadToJob()
{
  void *free = nullptr;

  freeList.tryDequeue(free);
  if (free == nullptr)
  {
    return nullptr;
  }

  fiber::Fiber *fiber = fiber::Fiber::currentThreadToFiber();

  return new (free) Job(fiber, -1, this);
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