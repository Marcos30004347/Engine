#pragma once
#include <thread>

#include "Fiber.hpp"
#include "Fiberpool.hpp"

#include "lib/datastructure/ConcurrentPriorityQueue.hpp"
#include "lib/datastructure/ConcurrentQueue.hpp"
#include "lib/datastructure/Vector.hpp"
#include "lib/memory/allocator/BoundedHeapAllocator.hpp"
#include "lib/time/TimeSpan.hpp"
// #include "Promise.hpp"
#include "Job.hpp"

namespace jobsystem
{
struct JobSystemSettings
{
  size_t threadsCount;
  size_t maxJobsConcurrent;
  size_t maxJobPayloadSize;
  size_t jobsBufferSize;
};

class JobSystem
{
public:
  static void init(void (*entry)(), JobSystemSettings settings);
  static void shutdown();

  template <typename F, typename... Args> static auto enqueue(F &&f, Args &&...args);

  template <typename T> static T &wait(Promise<T> promise);
  static void wait(Promise<void> promise);
  static void yield();
  static void stop();

private:
  static void workerLoop();
  static void sleepAndWakeOnPromiseResolve(std::shared_ptr<Job> job);
  template <typename F, typename... Args> static void dispatch(void *data, fiber::Fiber *self);
  static lib::Vector<std::thread> workerThreads;
  // static lib::ConcurrentQueue<std::shared_ptr<Job>> pendingJobs;

  static JobQueue *pendingJobs;
  static JobAllocator *jobsAllocator;

  static std::atomic<bool> isRunning;
};
struct EmptyResultTag
{
};
template <typename Function, typename... Args> struct JobData
{
  using Return = std::invoke_result_t<Function, Args...>;
  using ResultType = std::conditional_t<std::is_void_v<Return>, struct EmptyResultTag, Return>;

  Function handler;
  std::tuple<std::decay_t<Args>...> payload;
  ResultType result;

  // std::shared_ptr<Job> job;
};

template <typename F, typename... Args> void JobSystem::dispatch(void *data, fiber::Fiber *self)
{
  using Ret = std::invoke_result_t<F, Args...>;
  using Job = JobData<std::decay_t<F>, std::decay_t<Args>...>;

  std::shared_ptr<Job> job = *static_cast<std::shared_ptr<Job> *>(data);

  size_t offset = calculateOffset<JobData<F, Args...>>();
  JobData<F, Args...> *jobData = (JobData<F, Args...> *)(reinterpret_cast<char *>(job.get()) + offset);

  if constexpr (std::is_void_v<Ret>)
  {
    std::apply(job->handler, job->payload);
  }
  else
  {
    job->resolve();
    jobData->result = std::apply(job->handler, job->payload);
  }

  fiber::FiberPool::release(self);
}

template <typename F, typename... Args> auto JobSystem::enqueue(F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;

  std::shared_ptr<Job> jobStorage = jobsAllocator->allocate();
  void *data = (void *)(&jobStorage);

  fiber::Fiber *fiber = fiber::FiberPool::acquire(&dispatch<F, Args...>, data, 1024 * 1024 * 2);

  Job *job = new (jobStorage.get()) Job(fiber);

  size_t offset = calculateOffset<JobData<F, Args...>>();
  void *buffer = (void *)(reinterpret_cast<char *>(jobStorage.get()) + offset);

  JobData<F, Args...> *jobData = new (buffer) JobData<F, Args...>{

    // jobData->job = jobStorage;
    std::forward<F>(f),
    std::make_tuple(std::forward<Args>(args)...),

  };
  pendingJobs->enqueue(jobStorage);

  return Promise<Ret>(jobStorage);
}

template <typename T> inline T &JobSystem::wait(Promise<T> promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return promise->get();
  }
  */

  sleepAndWakeOnPromiseResolve(promise.job);

  return promise->get();
}

inline void JobSystem::wait(Promise<void> promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return;
  }
  */
  sleepAndWakeOnPromiseResolve(promise.job);
}

} // namespace jobsystem