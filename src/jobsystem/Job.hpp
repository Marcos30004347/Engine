#pragma once

#include <memory>
#include <utility>

#include "Fiberpool.hpp"
#include "lib/datastructure/ConcurrentQueue.hpp"

namespace jobsystem
{

class Job;
class JobAllocator
{
public:
  JobAllocator(size_t maxPayloadSize, size_t capacity);
  ~JobAllocator();

  Job *allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolIndex);
  Job *currentThreadToJob();
  uint64_t getPayloadSize();
  void deallocate(Job *);

private:
  uint64_t payloadSize;
  lib::memory::allocator::SystemAllocator<char> allocator;
  lib::ConcurrentQueue<void *> freeList;
};

class Job
{
  friend class JobSystem;
  template <typename T> friend class Promise;

  static std::atomic<uint32_t> allocations;
  static std::atomic<uint32_t> deallocations;

private:
  std::atomic<uint32_t> refsInQueues;
  std::atomic<uint32_t> refsInPromises;
  std::atomic<uint32_t> refsInRuntime;
  std::atomic<uint32_t> refsInJobs;

  Job *waiter;
  fiber::Fiber *fiber;

  uint32_t fiberPoolIndex;

  std::atomic<bool> finished;
  std::atomic_flag spinLock = ATOMIC_FLAG_INIT;
  JobAllocator *allocator;
  inline void checkForDeallocation()
  {
    if (refsInQueues.load() == 0 && refsInPromises.load() == 0 && refsInRuntime.load() == 0 && refsInJobs.load() == 0)
    {
      deallocations.fetch_add(1);
      // os::print("deallocations at %p = %u, allocs = %u\n", this, deallocations.load(), allocations.load());
      assert(deallocations.load() <= allocations.load());
      allocator->deallocate(this);
    }
  }

public:
  Job(fiber::Fiber *fiber, uint32_t poolId, JobAllocator *allocator)
      : finished(false), fiber(fiber), waiter(nullptr), fiberPoolIndex(poolId), refsInQueues(0), refsInPromises(0), refsInRuntime(0), refsInJobs(0), allocator(allocator)
  {
    allocations.fetch_add(1);
    assert(deallocations.load() <= allocations.load());
    // os::print("allocations at %p = %u\n", this, allocations.load());
  }
  ~Job()
  {
  }

  uint32_t refInPromise()
  {
    return refsInPromises.fetch_add(1);
  }

  uint32_t refInQueue()
  {
    return refsInQueues.fetch_add(1);
  }

  uint32_t refInRuntime()
  {
    return refsInRuntime.fetch_add(1);
  }

  uint32_t refInJob()
  {
    return refsInJobs.fetch_add(1);
  }

  uint32_t derefInPromise()
  {
    uint32_t old = refsInPromises.fetch_sub(1);
    assert(old > 0);
    checkForDeallocation();
    return old;
  }

  uint32_t derefInQueue()
  {
    uint32_t old = refsInQueues.fetch_sub(1);
    assert(old > 0);
    checkForDeallocation();
    return old;
  }

  uint32_t derefInRuntime()
  {
    uint32_t old = refsInRuntime.fetch_sub(1);
    assert(old > 0);
    checkForDeallocation();
    return old;
  }

  uint32_t derefInJob()
  {
    uint32_t old = refsInJobs.fetch_sub(1);
    assert(old > 0);
    checkForDeallocation();
    return old;
  }

  // void run()
  // {
  //   fiber->run();
  // }

  void resume()
  {
    assert(fiber != nullptr);
    fiber::Fiber::switchTo(fiber);
  }

  bool resolve()
  {

    bool expected = false;
    bool old = finished.compare_exchange_strong(expected, true, std::memory_order_acquire);

    // w = waiter;
    // waiter = nullptr;

    // uint32_t oldRefs = refInWaiters.fetch_add(1);
    // assert(oldRefs == 0);

    return old;
  }

  void lock()
  {
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }
  }
  void unlock()
  {
    spinLock.clear(std::memory_order_release);
  }

  void setWaiter(Job *&job)
  {
    job->refInJob();
    // assert(waiter == nullptr);
    waiter = job;
    assert(job->refInJob() == 0);
    // uint32_t oldRefs = refInWaiters.fetch_add(1);
    // assert(oldRefs == 0);
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
    job->refInPromise();
  }

public:
  Promise() : job(nullptr), data(nullptr)
  {
  }

  ~Promise()
  {
    if (job)
    {
      assert(job->refsInPromises.load() == 1);
      job->derefInPromise();
      assert(job->refsInPromises.load() == 0);
    }
  }

  Promise(const Promise &) = delete;
  Promise &operator=(const Promise &) = delete;

  Promise(Promise &&other) noexcept : job(std::move(other.job)), data(std::move(other.data))
  {
    other.job = nullptr;
    other.data = nullptr;

    assert(job->refsInPromises.load() == 1);
  }

  Promise &operator=(Promise &&other) noexcept
  {
    if (this != &other)
    {
      assert(other.job->refsInPromises.load() == 1);
      job = std::move(other.job);
      other.job = nullptr;
      data = std::move(other.data);
      other.data = nullptr;
    }

    assert(job->refsInPromises.load() == 1);
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
    job->refInPromise();
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
      assert(job->refsInPromises.load() == 1);
      job->derefInPromise(); // refInPromises.fetch_sub(1);
      assert(job->refsInPromises.load() == 0);
    }
  }

  Promise(const Promise &) = default;
  Promise &operator=(const Promise &) = default;

  Promise(Promise &&other) noexcept : job(std::move(other.job))
  {
    other.job = nullptr;
    assert(job->refsInPromises.load() == 1);
  }

  Promise &operator=(Promise &&other) noexcept
  {
    // os::print("assigning void promise rf = %u\n", other.job->refInPromises.load());
    if (this != &other)
    {
      assert(other.job->refsInPromises.load() == 1);
      job = std::move(other.job);
      other.job = nullptr;
    }

    assert(job->refsInPromises.load() == 1);

    return *this;
  }
};

} // namespace jobsystem
