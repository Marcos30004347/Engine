#include "async/Job.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"

static async::Job *mainJob = nullptr;
static async::Job *funcJob = nullptr;

void handler0(void *data, async::fiber::Fiber *fiber)
{
  mainJob->resume();
  mainJob->resume();
}

int multithreadTests()
{
  uint32_t invocations = 1000;

  async::JobAllocator *allocator = new async::JobAllocator(2 * 1024 * 1024, invocations, invocations);

  allocator->initializeThread();

  mainJob = async::Job::currentThreadToJob();

  for (uint32_t i = 0; i < invocations; i++)
  {
    funcJob = allocator->allocate(handler0);
    funcJob->resume();
    funcJob->resume();
    allocator->deallocate(funcJob);
  }

  delete mainJob;
  allocator->deinitializeThread();

  delete allocator;

  return 0;
}

int main()
{
  multithreadTests();
  return 0;
}