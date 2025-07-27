#pragma once

#include "Fiber.hpp"
#include "Fiberpool.hpp"

#include "lib/algorithm/string.hpp"
#include "lib/datastructure/ConcurrentPriorityQueue.hpp"
#include "lib/datastructure/ConcurrentQueue.hpp"
#include "lib/time/TimeSpan.hpp"
#include "os/Thread.hpp"

// #include "Promise.hpp"
#include "Job.hpp"

namespace jobsystem
{

struct JobAllocatorSettings
{
  size_t payloadSize;
  size_t capacity;
};

struct JobQueueSettings
{
  // uint64_t maxExecutionsBeforeReset = UINT64_MAX;
};

struct JobStackSettings
{
  size_t stackSize;
  size_t cacheSize;
};

struct JobEnqueueData
{
  uint64_t queueIndex = 0;
  uint64_t allocatorIndex = 0;
  uint64_t stackSize = 0;
};
struct JobSystemSettings
{
  size_t threadsCount;

  JobAllocatorSettings *jobAllocatorsSettings;
  size_t jobAllocatorSettingsCount;

  JobQueueSettings *jobQueueSettings;
  size_t jobQueueSettingsCount;

  JobStackSettings *jobStackSettings;
  size_t jobStackSettingsCount;
};

struct JobQueueInfo
{
  // std::atomic<uint64_t> dequeuesInCurrentTick;
  // uint64_t maxExecutionsBeforeReset;
};

class JobSystem
{
public:
  static void init(void (*entry)(), JobSystemSettings *settings);
  static void shutdown();

  template <typename F, typename... Args> static auto enqueue(JobEnqueueData *data, F &&f, Args &&...args);
  template <typename F, typename... Args> static auto enqueue(JobEnqueueData *data, std::result_of_t<F && (Args && ...)> *output, F &&f, Args &&...args);

  template <typename T> static T &wait(Promise<T> &promise);
  template <typename T> static T &wait(Promise<T> &&promise);

  static void wait(Promise<void> &promise);
  static void wait(Promise<void> &&promise);
  static void yield();
  static void stop();
  static void delay(lib::time::TimeSpan);

  // private:
  static void workerLoop();
  static void sleepAndWakeOnPromiseResolve(volatile Job *job);
  static void processYieldedJobs();

  template <typename F, typename... Args> static void dispatch(void *data, fiber::Fiber *self);
  static std::vector<os::Thread> workerThreads;

  static thread_local volatile Job *workerJob;
  static thread_local volatile Job *currentJob;
  static thread_local volatile Job *yieldedJob;
  static thread_local volatile Job *runningJob;
  static thread_local volatile Job *waitedJob;

  static thread_local uint64_t waitingTime;

  static uint64_t pendingQueueIndex;
  static std::vector<lib::ConcurrentQueue<volatile Job *> *> jobQueues;
  static std::vector<JobQueueInfo> jobQueuesInfo;
  static std::vector<JobAllocator *> jobAllocators;
  static std::atomic<bool> isRunning;
  static std::vector<fiber::FiberPool *> pools;
  static lib::ConcurrentPriorityQueue<volatile Job *, uint64_t> *waitingQueue;
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
  uint64_t poolIndex;

  ResultType *result;
};

template <typename F, typename... Args> void JobSystem::dispatch(void *data, fiber::Fiber *self)
{
  using Ret = std::invoke_result_t<F, Args...>;

  Job *job = static_cast<Job *>(data);

  assert(self == job->fiber);
  assert(currentJob == job);

  assert(job->refs.load() >= 1);

  size_t offset = calculateOffset<JobData<F, Args...>>();
  JobData<F, Args...> *jobData = (JobData<F, Args...> *)(reinterpret_cast<char *>(job) + offset);

  uint64_t poolIndex = jobData->poolIndex;

  // if (job->fiber != fiber::Fiber::current())
  // {
  //   // os::print("Thread %u inconsistent fiber start dispatch %p %p\n", os::Thread::getCurrentThreadId(), job->fiber, fiber::Fiber::current());
  //   *(int *)(0) = 3;
  // }

  if constexpr (std::is_void_v<Ret>)
  {
    std::apply(jobData->handler, jobData->payload);
  }
  else
  {
    *jobData->result = std::apply(jobData->handler, jobData->payload);
  }

  // if (job->fiber != fiber::Fiber::current())
  // {
  //   *(int *)(0) = 3;
  // }

  assert(job->refs.load() >= 1);

  job->lock();

  bool resolved = job->resolve();

  assert(resolved);

  volatile jobsystem::Job *waiter = job->waiter;
  job->waiter = nullptr;

  if (waiter)
  {
    jobQueues[pendingQueueIndex]->enqueue(waiter);
  }

  job->unlock();
}

template <typename F, typename... Args> auto JobSystem::enqueue(JobEnqueueData *data, F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;
  uint64_t i = 0;

  for (i = 0; i < pools.size(); i++)
  {
    if (pools[i]->getStackSize() >= data->stackSize)
    {
      break;
    }
  }

  if (i == pools.size())
  {
    throw std::runtime_error(formatString("No job pool supports required stack size of %u, create a new pool or try a different stack size", data->stackSize));
  }

  size_t retSize = 0;
  if constexpr (!std::is_void_v<Ret>)
  {
    retSize = sizeof(Ret);
  }

  if (sizeof(JobData<F, Args...>) + sizeof(Job) + retSize > jobAllocators[data->allocatorIndex]->getPayloadSize())
  {
    throw std::runtime_error(formatString("allocator can't support the payload size of %u", sizeof(JobData<F, Args...>) + retSize));
  }

  Job *job = jobAllocators[data->allocatorIndex]->allocate(&dispatch<F, Args...>, pools[i], i);

  if (job == nullptr)
  {
    throw std::runtime_error("error allocating job");
  }

  size_t offset = calculateOffset<JobData<F, Args...>>();
  void *buffer = (void *)(reinterpret_cast<char *>(job) + offset);

  JobData<F, Args...> *jobData = new (buffer) JobData<F, Args...>{std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...), i};

  if constexpr (!std::is_void_v<Ret>)
  {
    jobData->result = (Ret *)(reinterpret_cast<char *>(jobData) + sizeof(JobData<F, Args...>));
  }

  job->ref();
  job->ref();

  assert(job->refs.load() == 2);

  jobQueues[data->queueIndex]->enqueue(job);

  if constexpr (std::is_void_v<Ret>)
  {
    return Promise<Ret>(job);
  }
  else
  {
    return Promise<Ret>(job, jobData->result);
  }
}

template <typename F, typename... Args> auto JobSystem::enqueue(JobEnqueueData *data, std::result_of_t<F && (Args && ...)> *ret, F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;

  uint64_t i = 0;

  for (i = 0; i < pools.size(); i++)
  {
    if (pools[i]->getStackSize() >= data->stackSize)
    {
      break;
    }
  }

  if (i == pools.size())
  {
    throw std::runtime_error(formatString("No job pool supports required stack size of %u, create a new pool or try a different stack size", data->stackSize));
  }

  if (sizeof(JobData<F, Args...>) + sizeof(Job) > jobAllocators[data->allocatorIndex]->getPayloadSize())
  {
    throw std::runtime_error(formatString("allocator can't support the payload size of %u", sizeof(JobData<F, Args...>)));
  }

  Job *job = jobAllocators[data->allocatorIndex]->allocate(&dispatch<F, Args...>, pools[i], i);

  if (job == nullptr)
  {
    throw std::runtime_error("error allocating job");
  }

  size_t offset = calculateOffset<JobData<F, Args...>>();
  void *buffer = (void *)(reinterpret_cast<char *>(job) + offset);

  JobData<F, Args...> *jobData = new (buffer) JobData<F, Args...>{std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...), i};
  jobData->result = ret;

  job->ref();
  job->ref();

  assert(job->refs.load() == 2);

  jobQueues[data->queueIndex]->enqueue(job);

  if constexpr (std::is_void_v<Ret>)
  {
    return Promise<Ret>(job);
  }
  else
  {
    return Promise<Ret>(job, ret);
  }
}

template <typename T> inline T &JobSystem::wait(Promise<T> &promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

template <typename T> inline T &JobSystem::wait(Promise<T> &&promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
  return *(promise.data);
}

inline void JobSystem::wait(Promise<void> &promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
}

inline void JobSystem::wait(Promise<void> &&promise)
{
  sleepAndWakeOnPromiseResolve(promise.job);
}

} // namespace jobsystem