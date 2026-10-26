#pragma once

#include <memory>
#include <utility>

#include "datastructure/MarkedAtomicPointer.hpp"

#include "Fiber.hpp"
#include "time/TimeSpan.hpp"
#include <assert.h>

#include "os/print.hpp"

namespace async
{

namespace detail
{
class AsyncManager;
}

class Job;

struct JobFreeNode
{
  JobFreeNode *next;
  fiber::Fiber *fiber;
};

class JobAllocator
{
public:
  JobAllocator(size_t stackSize, size_t initialCapacity, size_t maxLocal);
  ~JobAllocator();

  Job *allocate(fiber::Fiber::Handler handler);
  void deallocate(Job *job);

  void initializeThread();
  void deinitializeThread();

private:
  size_t stackSize;
  size_t initialCapacity;
  size_t maxLocal;

  static thread_local Job *localHead;
  static thread_local size_t localCount;
};

struct JobDataBase
{
  void (*invoke)(JobDataBase *);
};

template <typename F, typename R> struct JobDataValue final : JobDataBase
{
  F fn;
  R result;

  explicit JobDataValue(F &&f) : fn(std::forward<F>(f))
  {
    invoke = &invokeImpl;
  }

  static void invokeImpl(JobDataBase *base)
  {
    auto *self = static_cast<JobDataValue *>(base);
    self->result = self->fn();
  }
};

template <typename F> struct JobDataVoid final : JobDataBase
{
  F fn;

  explicit JobDataVoid(F &&f) : fn(std::forward<F>(f))
  {
    invoke = &invokeImpl;
  }

  static void invokeImpl(JobDataBase *base)
  {
    auto *self = static_cast<JobDataVoid *>(base);
    self->fn();
  }
};

struct Job
{
  static thread_local Job *currentJob;

  std::atomic<uint64_t> refs;
  lib::MarkedAtomicPointer<Job> waiter;

  Job *nextFree;

  fiber::Fiber fiber;

  JobAllocator *allocator = nullptr;
  JobDataBase *jobData = nullptr;

  Job *waiting;
  Job *manager;

  bool yielding;

  alignas(std::max_align_t) uint8_t payload[256];

  Job(JobAllocator *a, fiber::Fiber::Handler handler, uint64_t stackSize) : allocator(a), nextFree(nullptr), refs(0), fiber(handler, this, stackSize, false)
  {
  }

  void reset(fiber::Fiber::Handler handler)
  {
    // finished.store(false, std::memory_order_relaxed);
    waiter.store(nullptr);
    jobData = nullptr;
    nextFree = nullptr;

    waiting = nullptr;
    manager = nullptr;
    yielding = false;

    fiber.reset(handler, this);
  }

  static Job *currentThreadToJob();

  // void lock()
  // {
  //   while (spinLock.test_and_set(std::memory_order_acquire))
  //   {
  //   }
  // }
  // void unlock()
  // {
  //   spinLock.clear(std::memory_order_release);
  // }
  void ref(uint32_t c = 1, const char *debug = nullptr)
  {
    // os::print("refing %p %u - %s\n", this, refs.load() + c, debug);
    refs.fetch_add(c, std::memory_order_relaxed);
  }
  
  void deref(uint32_t c = 1, const char *debug = nullptr)
  {
    auto old = refs.fetch_sub(c, std::memory_order_acq_rel);
    assert(old != 0);
    if (old == 1)
    {
      if (allocator == nullptr)
      {
        delete this;
      }
      else
      {
        allocator->deallocate(this);
      }
    }
  }

  fiber::Fiber *getFiber()
  {
    return &fiber;
  }

  bool setWaiter(Job *job)
  {
    bool isMarked = false;

    Job *curr = nullptr;
    do
    {
      curr = waiter.read(isMarked);

      assert(curr == nullptr && "Can't have multiple waiters");

      if (isMarked)
      {
        return false;
      }

    } while (!waiter.compare_exchange_strong(curr, job, std::memory_order_acquire, std::memory_order_release));

    return true;
  }

  void resume()
  {
    assert(fiber::Fiber::current() == &Job::currentJob->fiber);

    Job *old = Job::currentJob;
    Job::currentJob = this;
    fiber::Fiber::switchTo(&fiber);
    Job::currentJob = old;

    assert(fiber::Fiber::current() == &Job::currentJob->fiber);
  }

  bool resolve()
  {
    while (true)
    {
      bool isMarked = false;
      auto w = waiter.read(isMarked);

      assert(!isMarked && "Should not be marked");

      if (waiter.attemptMark(w, true))
      {
        return true;
      }
    }
    return true;
  }

  bool isFinished()
  {
    bool isMarked = false;
    waiter.read(isMarked);
    return isMarked;
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
  friend class detail::AsyncManager;
  friend void wait(Promise<T> &);
  friend void wait(Promise<T> &&);
  // private:

public:
  Job *job;
  T *data;

  Promise(Job *job, T *data) : job(job), data(data)
  {
  }

public:
  Promise() : job(nullptr), data(nullptr)
  {
  }

  ~Promise()
  {
    if (job)
    {
      // assert(job->refs.load() >= 1);
      //  os::print("promise %p derefing job %p (%u)\n", this, job, job->refs.load());
      job->deref(1, "promise");
    }
  }

  Promise(const Promise &) = delete;
  Promise &operator=(const Promise &) = delete;

  Promise(Promise &&other) noexcept : job(std::move(other.job)), data(std::move(other.data))
  {
    other.job = nullptr;
    other.data = nullptr;
  }

  Promise &operator=(Promise &&other) noexcept
  {
    if (this != &other)
    {
      this->~Promise();

      job = std::move(other.job);
      data = std::move(other.data);

      other.job = nullptr;
      other.data = nullptr;
    }

    assert(job->refs.load() >= 1);
    return *this;
  }
};

template <> class Promise<void>
{
  friend class detail::AsyncManager;
  friend void wait(Promise<void> &);
  friend void wait(Promise<void> &&);

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
      // assert(job->refs.load() >= 1);
      job->deref(1, "promise"); // refInPromises.fetch_sub(1);
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
    if (this != &other)
    {
      this->~Promise();

      assert(other.job->refs.load() >= 1);
      job = std::move(other.job);
      other.job = nullptr;
    }

    assert(job->refs.load() >= 1);

    return *this;
  }
};

} // namespace async
