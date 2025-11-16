#include "async/Job.hpp"
#include "time/TimeSpan.hpp"

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

static inline void* get_stack_pointer() {
    void* sp;
#if defined(__x86_64__) || defined(_M_X64)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(__i386__) || defined(_M_IX86)
    asm volatile("mov %%esp, %0" : "=r"(sp));
#elif defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(sp));
#else
    // Fallback: frame address is often close to SP
    sp = __builtin_frame_address(0);
#endif
    return sp;
}

void handler0(void *data, async::fiber::Fiber *fiber)
{
  invocations += 1;

  auto now = lib::time::TimeSpan::now();

  invocationTimes += (now - prev).nanoseconds();

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

  uint32_t invocations = 256;
  size_t totalThreads = os::Thread::getHardwareConcurrency();

  async::fiber::FiberPool *fiberpool = new async::fiber::FiberPool(async::fiber::Fiber::getDefaultSize(), 1, totalThreads);
  async::JobAllocator *allocator = new async::JobAllocator(sizeof(uint8_t) * 1024, totalThreads);

  std::atomic<uint32_t> started(0);

  std::atomic<size_t> insertedFinished(0);

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
          funcJob = nullptr;

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