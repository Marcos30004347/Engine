#include "async/Job.hpp"
#include "lib/time/TimeSpan.hpp"

static thread_local async::Job *mainJob = nullptr;
static thread_local async::Job *funcJob = nullptr;

static thread_local int counter = 0;
static thread_local lib::time::TimeSpan prev;

static thread_local double allocationTimes = 0;
static thread_local double resumeTimes = 0;
static thread_local double invocationTimes = 0;

static thread_local double allocations = 0;
static thread_local double invocations = 0;
static thread_local double resumes = 0;

void handler0(void *data, async::fiber::Fiber *fiber)
{
  invocations += 1;
  invocationTimes += (lib::time::TimeSpan::now() - prev).nanoseconds();

  assert(counter++ == 1);

  prev = lib::time::TimeSpan::now();
  mainJob->resume();

  assert(counter++ == 3);

  resumeTimes += (lib::time::TimeSpan::now() - prev).nanoseconds();
  resumes += 1;
  prev = lib::time::TimeSpan::now();

  mainJob->resume();
}

int multithreadTests()
{
  async::fiber::Fiber::initializeSubSystems();

  uint32_t invocations = 1000;

  async::fiber::FiberPool *fiberpool = new async::fiber::FiberPool(32768, invocations, os::Thread::getHardwareConcurrency());
  async::JobAllocator *allocator = new async::JobAllocator(sizeof(uint8_t) * 256, os::Thread::getHardwareConcurrency() * invocations);

  std::atomic<uint32_t> started(0);

  std::atomic<size_t> insertedFinished(0);

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          fiberpool->initializeThread();
          allocator->initializeThread();

          started.fetch_add(1);

          while (started.load() < totalThreads)
          {
          }

          mainJob = allocator->currentThreadToJob();

          for (uint32_t i = 0; i < invocations; i++)
          {
            prev = lib::time::TimeSpan::now();

            funcJob = allocator->allocate(handler0, fiberpool, 0);

            allocationTimes += (lib::time::TimeSpan::now() - prev).nanoseconds();
            allocations += 1;
            counter = 1;

            prev = lib::time::TimeSpan::now();

            funcJob->resume();

            resumeTimes += (lib::time::TimeSpan::now() - prev).nanoseconds();
            resumes += 1;
            prev = lib::time::TimeSpan::now();

            assert(counter++ == 2);
            funcJob->resume();

            resumeTimes += (lib::time::TimeSpan::now() - prev).nanoseconds();
            resumes += 1;

            assert(counter++ == 4);
          
            fiberpool->release((async::fiber::Fiber *)funcJob->getFiber());
            allocator->deallocate(funcJob);
          }

          fiberpool->release((async::fiber::Fiber *)mainJob->getFiber());
          allocator->deallocate(mainJob);

          mainJob = nullptr;

          allocator->deinitializeThread();
          fiberpool->deinitializeThread();

          os::print("Thread %u average allocation time is %fns\n", os::Thread::getCurrentThreadId(), allocationTimes / allocations);
          os::print("Thread %u average invocation time is %fns\n", os::Thread::getCurrentThreadId(), invocationTimes / invocations);
          os::print("Thread %u average resume time is %fns\n", os::Thread::getCurrentThreadId(), resumeTimes / resumes);
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }
  delete allocator;
  delete fiberpool;
  async::fiber::Fiber::deinitializeSubSystems();

  return 0;
}

int main()
{
  for (size_t i = 0; i < 100; i++)
  {
    multithreadTests();
  }
  return 0;
}