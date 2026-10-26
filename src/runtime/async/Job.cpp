#include "Job.hpp"
#include <assert.h>

#include "os/print.hpp"
namespace async
{
thread_local Job *JobAllocator::localHead = nullptr;
thread_local size_t JobAllocator::localCount = 0;
thread_local Job *Job::currentJob = nullptr;

JobAllocator::JobAllocator(size_t stackSize, size_t initialCapacity, size_t maxLocal) : stackSize(stackSize), initialCapacity(initialCapacity), maxLocal(maxLocal)
{
}

JobAllocator::~JobAllocator()
{
#ifndef NDEBUG
  assert(localHead == nullptr);
  assert(localCount == 0);
#endif
}

void JobAllocator::initializeThread()
{
  while (localCount < initialCapacity)
  {
    Job *job = new Job(this, nullptr, stackSize);
    job->nextFree = localHead;
    localHead = job;
    localCount++;
  }
}

void JobAllocator::deinitializeThread()
{
  while (localHead)
  {
    Job *job = localHead;
    localHead = job->nextFree;
    delete job;
  }

  localCount = 0;
}

Job *JobAllocator::allocate(fiber::Fiber::Handler handler)
{
  Job *job = nullptr;

  if (localHead)
  {
    job = localHead;
    localHead = job->nextFree;
    localCount--;
    job->reset(handler);
  }
  else
  {
    job = new Job(this, handler, stackSize);
  }

  return job;
}

void JobAllocator::deallocate(Job *job)
{
  if (localCount < maxLocal)
  {
    //      os::print("deallocating using cache, local count %u, max %u\n", localCount, maxLocal);

    job->nextFree = localHead;
    localHead = job;
    localCount++;
    // os::print("local count %u\n", localCount);
  }
  else
  {
    //          os::print("deallocating using heap, local count %u, max %u\n", localCount, maxLocal);

    // pool.release(job->fiber);
    delete job;
  }
}

Job *Job::currentThreadToJob()
{
  auto job = new Job(nullptr, nullptr, 0);

  fiber::Fiber::currentThreadToFiber(&job->fiber);

  Job::currentJob = job;

  assert(fiber::Fiber::current() == &Job::currentJob->fiber);

  return job;
}
} // namespace async