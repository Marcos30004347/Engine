#pragma once

#include <memory>
#include <utility>

#include "Fiber.hpp"
#include "lib/datastructure/ConcurrentVector.hpp"

namespace jobsystem
{

class Job
{
  friend class JobSystem;

private:
  lib::ConcurrentVector<std::shared_ptr<Job>> waiters;
  fiber::Fiber *fiber;

  std::atomic<bool> executed;
  std::atomic_flag spinLock = ATOMIC_FLAG_INIT;

public:
  Job(fiber::Fiber *fiber) : executed(false), fiber(fiber), waiters()
  {
  }

  bool resolve()
  {
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }

    bool expected = false;
    bool old = executed.compare_exchange_strong(expected, true, std::memory_order_acquire);

    spinLock.clear(std::memory_order_release);

    return old;
  }

  bool addWaiter(std::shared_ptr<Job> job)
  {
    while (spinLock.test_and_set(std::memory_order_acquire))
    {
    }

    if (executed.load(std::memory_order_acquire))
    {
      return false;
    }

    waiters.pushBack(job);
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

private:
  std::shared_ptr<Job> job;

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

  bool wait(std::shared_ptr<Job> &p)
  {
    return job->addWaiter(p);
  }

  Promise(std::shared_ptr<Job> job) : job(job)
  {
  }

public:
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
  Promise(std::shared_ptr<Job> &job) : job(job)
  {
  }

public:
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

class JobQueue
{
public:
  std::shared_ptr<Job> dequeue();
  bool enqueue(std::shared_ptr<Job> ptr);
  JobQueue(size_t capacity);
  ~JobQueue();

private:
  size_t capacity;
  std::shared_ptr<Job> *jobs;
  std::atomic<size_t> head;
  std::atomic<size_t> tail;
};

struct JobAllocatorFreeList
{
  JobAllocatorFreeList *next;
};

class JobAllocator
{
public:
  JobAllocator(size_t maxPayloadSize, size_t capacity);
  ~JobAllocator();

  std::shared_ptr<Job> allocate();

private:
  void deallocate(Job *);
  std::atomic<JobAllocatorFreeList *> freelist;
  char *jobsBuffer;
};

} // namespace jobsystem
