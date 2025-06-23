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
JobQueue::JobQueue(size_t capacity) : capacity(capacity), head(0), tail(0)
{
  jobs = new std::shared_ptr<Job>[capacity];
  for (size_t i = 0; i < capacity; ++i)
  {
    jobs[i] = nullptr;
  }
}

JobQueue::~JobQueue()
{
  delete[] jobs;
}

bool JobQueue::enqueue(std::shared_ptr<Job> job)
{
  size_t pos;

  while (true)
  {
    pos = tail.load(std::memory_order_relaxed);

    size_t next = (pos + 1) % capacity;

    if (next == head.load(std::memory_order_acquire))
    {
      return false;
    }

    if (tail.compare_exchange_weak(pos, next, std::memory_order_acq_rel))
    {
      break;
    }
  }

  while (jobs[pos] != nullptr)
  {
  }

  jobs[pos] = job;

  return true;
}

std::shared_ptr<Job> JobQueue::dequeue()
{
  size_t pos;

  while (true)
  {
    pos = head.load(std::memory_order_relaxed);
    if (pos == tail.load(std::memory_order_acquire))
    {
      return nullptr;
    }

    if (head.compare_exchange_weak(pos, (pos + 1) % capacity, std::memory_order_acq_rel))
    {
      break;
    }
  }

  std::shared_ptr<Job> job = jobs[pos];

  jobs[pos] = nullptr;

  return job;
}

JobAllocator::JobAllocator(size_t maxPayloadSize, size_t capacity)
{
  assert(capacity >= 1);
  assert(maxPayloadSize >= sizeof(Job) + 1 * sizeof(size_t));

  jobsBuffer = new char[maxPayloadSize * capacity];

  freelist = (JobAllocatorFreeList *)jobsBuffer;

  for (size_t i = 0; i < maxPayloadSize * capacity; i += maxPayloadSize)
  {
    JobAllocatorFreeList *ptr = reinterpret_cast<JobAllocatorFreeList *>((char*)jobsBuffer + i);
    ptr->next = (JobAllocatorFreeList *)(i + maxPayloadSize);
  }

  JobAllocatorFreeList *last = (JobAllocatorFreeList *)(jobsBuffer[maxPayloadSize * (capacity - 1)]);
  last->next = nullptr;
}
JobAllocator::~JobAllocator() {
    delete[] jobsBuffer;
}

std::shared_ptr<Job> JobAllocator::allocate()
{
  JobAllocatorFreeList *free;

  do
  {
    free = freelist.load();

    if (free == nullptr)
    {
      return nullptr;
    }

  } while (freelist.compare_exchange_strong(free, free->next));

  return std::shared_ptr<Job>((Job *)(index), [this](Job* j) { deallocate(j); });
}

void JobAllocator::deallocate(Job *job)
{

  JobAllocatorFreeList *free;
  JobAllocatorFreeList *next = (JobAllocatorFreeList *)job;

  do
  {
    free = freelist.load();

    if (freelist == nullptr)
    {
      next->next = nullptr;

      if (freelist.compare_exchange_strong(free, next))
      {
        break;
      }
    }

    next->next = free;
  } while (freelist.compare_exchange_strong(free, next));
}

} // namespace jobsystem