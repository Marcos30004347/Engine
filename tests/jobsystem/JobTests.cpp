#include "jobsystem/Job.hpp"
#include "lib/time/TimeSpan.hpp"

std::shared_ptr<jobsystem::Job> mainJob;
std::shared_ptr<jobsystem::Job> funcJob;

lib::time::TimeSpan prev;
lib::time::TimeSpan totalTime;

int counter = 0;

void handler0(void *data, jobsystem::fiber::Fiber *fiber)
{
  totalTime = lib::time::TimeSpan::now() - prev;
  os::print("invocation time = %f\n", totalTime.nanoseconds());

  assert(counter++ == 1);
  prev = lib::time::TimeSpan::now();
  mainJob->resume();
  assert(counter++ == 3);
  mainJob->resume();
}

int main()
{
  jobsystem::fiber::FiberPool *pool = new jobsystem::fiber::FiberPool(1024 * 1024);
  jobsystem::JobAllocator *allocator = new jobsystem::JobAllocator(sizeof(uint8_t) * 256, 4096);

  mainJob = allocator->currentThreadToJob();
  funcJob = allocator->allocate(handler0, pool, 0);

  counter = 1;

  prev = lib::time::TimeSpan::now();
  funcJob->resume();
  totalTime = lib::time::TimeSpan::now() - prev;

  os::print("resume time = %f\n", totalTime.nanoseconds());

  assert(counter++ == 2);
  funcJob->resume();
  assert(counter++ == 4);

  funcJob = nullptr;
  mainJob = nullptr;

  return 0;
}