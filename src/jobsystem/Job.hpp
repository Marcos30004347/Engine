#pragma once

#include <memory>
#include <utility>

#include "Fiberpool.hpp"
#include "lib/datastructure/ConcurrentQueue.hpp"

namespace jobsystem
{

class Job
{
  friend class JobSystem;

private:
  std::shared_ptr<Job> waiter;
  fiber::Fiber *fiber;
  
  uint32_t fiberPoolIndex;

  std::atomic<bool> finished;
  std::atomic_flag spinLock = ATOMIC_FLAG_INIT;

public:
  Job(fiber::Fiber *fiber, uint32_t poolId) : finished(false), fiber(fiber), waiter(nullptr), fiberPoolIndex(poolId)
  {
  }
  ~Job()
  {
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

  bool resolve(std::shared_ptr<Job> &w)
  {
    // TODO: likelly dont need to return bool
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }

    bool expected = false;
    bool old = finished.compare_exchange_strong(expected, true, std::memory_order_acquire);

    spinLock.clear(std::memory_order_release);

    w = std::move(waiter);
    waiter = nullptr;

    return old;
  }

  bool setWaiter(std::shared_ptr<Job> &job)
  {
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }

    if (finished.load(std::memory_order_acquire))
    {
      return false;
    }

    waiter = std::move(job);
    spinLock.clear(std::memory_order_release);
    return true;
  }
};

template <typename T> size_t calculateOffset()
{
  size_t j_size = sizeof(Job);
  size_t alignment = alignof(T);
  return (j_size + alignment - 1) / alignment * alignment;
}
/*
template <typename T> std::shared_ptr<Job> allocateJob(fiber::Fiber *f)
{
  size_t offset = calculateOffset<T>();
  size_t total_bytes = offset + sizeof(T);

  char *buffer = new char[total_bytes];

  Job *j_obj = new (buffer) Job(f);

  return std::shared_ptr<T>(
      j_obj,
      [buffer, offset](Job *ptrToJob)
      {
        T *ptr_to_t = reinterpret_cast<T *>(reinterpret_cast<char *>(ptrToJob) + offset);

        if (ptr_to_t)
        {
          ptr_to_t->~T();
        }
        if (ptrToJob)
        {
          ptrToJob->~Job();
        }

        delete[] buffer;
      });
}
template <> std::shared_ptr<Job> allocateJob<void>(fiber::Fiber *f);
*/
/*
template <typename T> T *getJobPayload(const std::shared_ptr<Job> &ptr)
{
  if (!ptr)
  {
    return nullptr;
  }
  return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(ptr.get()) + calculateOffset<T>());
}
*/
template <typename T> class Promise
{
  friend class JobSystem;

  // private:

public:
  std::shared_ptr<Job> job;
  T *data;
  /*
  bool resolve(T &value)
  {
    *getJobPayload(job) = std::move(value);
    return job->resolve();
  }
*/
  /*
    T &get()
    {
      return *getJobPayload(job);
    }
  */

  // bool wait(std::shared_ptr<Job> &p)
  // {
  //   return job->setWaiter(p);
  // }

  Promise(std::shared_ptr<Job> &&job, T *data) : job(job), data(data)
  {
  }

public:
  Promise() : job(nullptr), data(nullptr)
  {
  }

  Promise(const Promise &) = delete;
  Promise &operator=(const Promise &) = delete;

  Promise(Promise &&other) noexcept : job(std::move(other.job)), data(std::move(other.data))
  {
  }

  Promise &operator=(Promise &&other) noexcept
  {
    if (this != &other)
    {
      job = std::move(other.job);
      data = std::move(other.data);
    }

    return *this;
  }
};

template <> class Promise<void>
{
  friend class JobSystem;

private:
  std::shared_ptr<Job> job;
  /*
    bool resolve()
    {
      return job->resolve();
    }
  */
  Promise(std::shared_ptr<Job> &&job) : job(job)
  {
  }

public:
  Promise() : job(nullptr)
  {
  }

  Promise(const Promise &) = default;
  Promise &operator=(const Promise &) = default;

  Promise(Promise &&other) noexcept : job(std::move(other.job))
  {
  }

  Promise &operator=(Promise &&other) noexcept
  {
    if (this != &other)
    {
      job = std::move(other.job);
    }
    return *this;
  }
};

class JobAllocator
{
public:
  JobAllocator(size_t maxPayloadSize, size_t capacity);
  ~JobAllocator();

  std::shared_ptr<Job> allocate(fiber::Fiber::Handler handler, fiber::FiberPool *pool, uint32_t poolIndex);
  std::shared_ptr<Job> currentThreadToJob();
  uint64_t getPayloadSize();

private:
  void deallocate(Job *);

  uint64_t payloadSize;
  lib::memory::allocator::SystemAllocator<char> allocator;
  lib::ConcurrentQueue<void *> freeList;
};

} // namespace jobsystem
