#pragma once

#include "Fiber.hpp"
#include "Profile.hpp"
#include "algorithm/string.hpp"
#include "datastructure/ConcurrentPriorityQueue.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "os/Thread.hpp"
#include "time/TimeSpan.hpp"

// #include "Promise.hpp"
#include "Job.hpp"

namespace async
{

struct AsyncStackSettings
{
  size_t stackSize;
  size_t cacheSize;
};

inline static size_t getMinStackSize()
{
  return fiber::Fiber::getMinSize();
}

struct SystemSettings
{
  size_t threadsCount;

  uint64_t jobsCapacity;
  uint64_t stackSize;
};

struct JobQueueInfo
{
};

namespace detail
{

class AsyncManager
{
public:
  static void init(void (*entry)(), SystemSettings settings);
  static void shutdown();

  template <typename F, typename... Args> static auto enqueue(F &&f, Args &&...args);
  template <typename F, typename... Args> static auto enqueue(std::result_of_t<F && (Args && ...)> *output, F &&f, Args &&...args);
  static void fiberEntry(void *data, fiber::Fiber *);

  template <typename T> static T &wait(Promise<T> &promise);
  template <typename T> static T &wait(Promise<T> &&promise);

  static void wait(Promise<void> &promise);
  static void wait(Promise<void> &&promise);
  static void yield();
  static void stop();
  // static void delay(lib::time::TimeSpan);

  // private:
  static void workerLoop();
  static void sleepAndWakeOnPromiseResolve(Job *job);
  static void processYieldedJobs();

  template <typename F, typename... Args> static void dispatch(void *data, fiber::Fiber *self);
  static std::vector<os::Thread> workerThreads;

  //static thread_local Job *workerJob;
  // static thread_local Job *currentJob;
  // static thread_local Job *yieldedJob;
  // static thread_local Job *runningJob;
  // static thread_local Job *waitedJob;
  //static thread_local uint64_t waitingTime;

  static uint64_t pendingQueueIndex;
  static lib::ConcurrentShardedQueue<Job *> jobQueue;
  static std::vector<JobQueueInfo> jobQueuesInfo;
  static JobAllocator *jobAllocator;
  static std::atomic<bool> isRunning;
  // static lib::ConcurrentPriorityQueue<Job *, uint64_t> *waitingQueue;
};

template <typename F, typename... Args> auto AsyncManager::enqueue(F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;

#ifdef ASYNC_MANAGER_LOG_TIMES
  async::profiling::ScopedTimer timer(async::profiling::gStats.enqueue);
#endif

  auto bound = [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Ret
  {
    return std::apply(fn, tup);
  };

  Job *job = jobAllocator->allocate(&AsyncManager::fiberEntry);

  // One ref for promise another for runtime queue
  job->ref(2, "allocating");

  assert(job->refs.load() == 2);

  if constexpr (std::is_void_v<Ret>)
  {
    using JD = JobDataVoid<decltype(bound)>;
    auto *jd = new (job->payload) JD(std::move(bound));
    job->jobData = jd;
    //os::print("%u enqueueing %p %p\n", os::Thread::getCurrentThreadId(), job, &job->fiber);
    jobQueue.enqueue(job);
    return Promise<void>(job);
  }
  else
  {
    using JD = JobDataValue<decltype(bound), Ret>;
    auto *jd = new (job->payload) JD(std::move(bound));
    job->jobData = jd;
    //os::print("%u enqueueing %p %p\n", os::Thread::getCurrentThreadId(), job, &job->fiber);
    jobQueue.enqueue(job);
    return Promise<Ret>(job, &jd->result);
  }
}

template <typename T> inline T &AsyncManager::wait(Promise<T> &promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

template <typename T> inline T &AsyncManager::wait(Promise<T> &&promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

inline void AsyncManager::wait(Promise<void> &promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
}

inline void AsyncManager::wait(Promise<void> &&promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
}

} // namespace detail

} // namespace async