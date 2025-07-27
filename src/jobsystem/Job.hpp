#pragma once

#include <memory>
#include <utility>

#include "Fiberpool.hpp"
// #include "lib/datastructure/ConcurrentQueue.hpp"

#include "lib/time/TimeSpan.hpp"

#include "Stack.hpp"
#include "ThreadCache.hpp"
namespace jobsystem
{

class Job;
class JobAllocator
{
public:
  JobAllocator(size_t maxPayloadSize, size_t capacity);
  ~JobAllocator();

  Job *allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolIndex);

  volatile Job *currentThreadToJob();
  uint64_t getPayloadSize();
  void deallocate(volatile Job *);
  void initializeThread();
  void deinitializeThread();

private:
  uint64_t payloadSize;
  lib::memory::allocator::SystemAllocator<char> allocator;
  uint64_t cacheSize;
  ThreadCache<Stack<void *>> cache;
  // lib::ConcurrentQueue<void *> freeList;
};

class Job
{
  friend class JobSystem;
  template <typename T> friend class Promise;

  static std::atomic<uint32_t> allocations;
  static std::atomic<uint32_t> deallocations;

public:
  // std::atomic<uint32_t> refsInQueues;
  // std::atomic<uint32_t> refsInPromises;
  // std::atomic<uint32_t> refsInRuntime;
  // std::atomic<uint32_t> refsInJobs;

  std::atomic<uint32_t> refs;

  volatile Job *waiter;
  volatile fiber::Fiber *fiber;

  uint32_t fiberPoolIndex;

  std::atomic<bool> finished;
  std::atomic_flag spinLock = ATOMIC_FLAG_INIT;
  JobAllocator *allocator;

public:
  Job(volatile fiber::Fiber *fiber, uint32_t poolId, JobAllocator *allocator)
      : finished(false), fiber(fiber), waiter(nullptr), fiberPoolIndex(poolId), refs(0), allocator(allocator)
  {
    // allocations.fetch_add(1);

    assert(deallocations.load() <= allocations.load());
    // os::print("allocations at %p = %u\n", this, allocations.load());
  }
  ~Job()
  {
  }

  void ref() volatile
  {
    refs.fetch_add(1);
  }

  void deref(const char *place) volatile
  {
    // os::print("derefing job %p (%u) - %s\n", this, refs.load(), place != nullptr ? place : "");
    if (refs.fetch_sub(1) == 1)
    {
      // os::print("deallocating %p\n", this);

      deallocations.fetch_add(1);

      uint32_t deallocs = deallocations.load();
      uint32_t allocs = allocations.load();

      // if (deallocs > allocs)
      // {
      //   os::print("deallocations at %p = %u, allocs = %u\n", this, deallocs, allocs);
      //   assert(deallocs <= allocs);
      // }
      // os::print("%p deallocated by %i - %u %u %u %u = %u\n", this, src, refsInQueues.load(), refsInPromises.load(), refsInRuntime.load(), refsInJobs.load(), refs.load());

      allocator->deallocate(this);
    }
  }

  void resume() volatile
  {
    assert(fiber != nullptr);
    fiber::Fiber::switchTo(fiber);
  }

  bool resolve() volatile
  {
    bool expected = false;
    bool old = finished.compare_exchange_strong(expected, true, std::memory_order_acquire);
    // w = waiter;
    // waiter = nullptr;

    // uint32_t oldRefs = refInWaiters.fetch_add(1);
    // assert(oldRefs == 0);

    return old;
  }

  void lock() volatile
  {
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }
    // os::print("Thread %u locking %p %p\n", os::Thread::getCurrentThreadId(), this, fiber);
  }

  void unlock() volatile
  {
    // os::print("Thread %u unlocking %p %p\n", os::Thread::getCurrentThreadId(), this, fiber);

    spinLock.clear(std::memory_order_release);
  }

  volatile fiber::Fiber *getFiber() volatile
  {
    return fiber;
  }

  void setWaiter(volatile Job *job) volatile
  {
    waiter = job;
    // os::print("thread %u waiting refs = %u - fiber = %p\n", os::Thread::getCurrentThreadId(), job->refs.load(), fiber);
  }
};

template <typename T> size_t calculateOffset()
{
  size_t j_size = sizeof(Job);
  size_t alignment = alignof(T);
  return (j_size + alignment - 1) / alignment * alignment;
}

template <typename T> class Promise
{
  friend class JobSystem;

  // private:

public:
  Job *job;
  T *data;

  Promise(Job *job, T *data) : job(job), data(data)
  {
    // os::print("promise constructor %p assigning job %p (%u)\n", this, job, job->refs.load());
  }

public:
  Promise() : job(nullptr), data(nullptr)
  {
  }

  ~Promise()
  {
    if (job)
    {
      assert(job->refs.load() >= 1);
      // os::print("promise %p derefing job %p (%u)\n", this, job, job->refs.load());
      job->deref("promise destructor");
    }
  }

  Promise(const Promise &) = delete;
  Promise &operator=(const Promise &) = delete;

  Promise(Promise &&other) noexcept : job(std::move(other.job)), data(std::move(other.data))
  {
    // os::print("promise %p copy assigning job %p (%u)\n", this, job, job->refs.load());

    other.job = nullptr;
    other.data = nullptr;
  }

  Promise &operator=(Promise &&other) noexcept
  {
    if (this != &other)
    {
      job = std::move(other.job);
      data = std::move(other.data);

      other.job = nullptr;
      other.data = nullptr;

      // os::print("promise %p move assigning job %p (%u)\n", this, job, job->refs.load());
    }

    assert(job->refs.load() >= 1);
    return *this;
  }
};

template <> class Promise<void>
{
  friend class JobSystem;

private:
  Job *job;
  /*
    bool resolve()
    {
      return job->resolve();
    }
  */
  Promise(Job *j) : job(j)
  {
    // os::print("creating this %p and increasing promise refs %u\n", this, job->refInPromises.load());
  }

public:
  Promise() : job(nullptr)
  {
  }

  ~Promise()
  {
    if (job)
    {
      assert(job->refs.load() >= 1);
      job->deref("promise destructor 2"); // refInPromises.fetch_sub(1);
      // assert(job->refsInPromises.load() == 0);
    }
  }

  Promise(const Promise &) = default;
  Promise &operator=(const Promise &) = default;

  Promise(Promise &&other) noexcept : job(std::move(other.job))
  {
    other.job = nullptr;
    assert(job->refs.load() >= 1);
  }

  Promise &operator=(Promise &&other) noexcept
  {
    // os::print("assigning void promise rf = %u\n", other.job->refInPromises.load());
    if (this != &other)
    {
      assert(other.job->refs.load() >= 1);
      job = std::move(other.job);
      other.job = nullptr;
    }

    assert(job->refs.load() >= 1);

    return *this;
  }
};

} // namespace jobsystem
