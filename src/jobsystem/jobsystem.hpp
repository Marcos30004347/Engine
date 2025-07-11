#pragma once
#include <thread>

#include "Fiber.hpp"
#include "Fiberpool.hpp"

#include "lib/algorithm/string.hpp"
#include "lib/datastructure/ConcurrentPriorityQueue.hpp"
#include "lib/datastructure/ConcurrentQueue.hpp"
#include "lib/datastructure/Vector.hpp"
#include "lib/time/TimeSpan.hpp"

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

  template <typename T> static T &wait(Promise<T> &promise);
  template <typename T> static T &wait(Promise<T> &&promise);

  static void wait(Promise<void> &promise);
  static void wait(Promise<void> &&promise);
  static void yield();
  static void stop();
  static void delay(lib::time::TimeSpan);

private:
  static void workerLoop();
  static void sleepAndWakeOnPromiseResolve(std::shared_ptr<Job> &job);
  static void processYieldedJobs();

  template <typename F, typename... Args> static void dispatch(void *data, fiber::Fiber *self);
  static std::vector<std::thread> workerThreads;
  // static lib::ConcurrentQueue<std::shared_ptr<Job>> pendingJobs;

  static thread_local std::shared_ptr<Job> workerJob;
  static thread_local std::shared_ptr<Job> currentJob;
  static thread_local std::shared_ptr<Job> yieldedJob;
  static thread_local std::shared_ptr<Job> runningJob;
  static thread_local std::shared_ptr<Job> waitedJob;
  static thread_local uint64_t waitingTime;

  static uint64_t pendingQueueIndex;
  static std::vector<lib::ConcurrentQueue<std::shared_ptr<Job>> *> jobQueues;
  static std::vector<JobQueueInfo> jobQueuesInfo;
  static std::vector<JobAllocator *> jobAllocators;
  static std::atomic<bool> isRunning;
  static std::vector<fiber::FiberPool *> pools;
  static lib::ConcurrentPriorityQueue<std::shared_ptr<Job>, uint64_t> *waitingQueue;
};
struct EmptyResultTag
{
};
template <typename Function, typename... Args> struct JobData
{
  using Return = std::invoke_result_t<Function, Args...>;
  using ResultType = std::conditional_t<std::is_void_v<Return>, struct EmptyResultTag, Return>;

  std::shared_ptr<Job> job;

  Function handler;
  std::tuple<std::decay_t<Args>...> payload;
  uint64_t poolIndex;

  ResultType result;
  // std::shared_ptr<Job> job;
};

template <typename F, typename... Args> void JobSystem::dispatch(void *data, fiber::Fiber *self)
{
  using Ret = std::invoke_result_t<F, Args...>;

  Job *job = static_cast<Job *>(data);

  assert(self == job->fiber);

  size_t offset = calculateOffset<JobData<F, Args...>>();
  JobData<F, Args...> *jobData = (JobData<F, Args...> *)(reinterpret_cast<char *>(job) + offset);

  uint64_t poolIndex = jobData->poolIndex;

  if constexpr (std::is_void_v<Ret>)
  {
    std::apply(jobData->handler, jobData->payload);
  }
  else
  {
    jobData->result = std::apply(jobData->handler, jobData->payload);
  }

  std::shared_ptr<jobsystem::Job> waiter;

  assert(jobData->job->resolve(waiter));

  if (waiter.get())
  {
    os::print("waiting enqueuing %p\n", waiter.get());
    jobQueues[pendingQueueIndex]->enqueue(waiter);
  }

  // job->fiber = nullptr;
  // pools[poolIndex]->release(self);
  // jobData->job->fiber = nullptr;
  os::print("thread %u finished %p, refconut = %u\n", os::Thread::getCurrentThreadId(), job, jobData->job.use_count());

  assert(currentJob.get() == jobData->job.get());

  jobData->job = nullptr;

  assert(currentJob.use_count() >= 1);
}

template <typename F, typename... Args> auto JobSystem::enqueue(JobEnqueueData *data, F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;

  // fiber::Fiber *fiber = fiber::FiberPool::acquire(&dispatch<F, Args...>, data, 1024 * 1024 * 2);
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

  std::shared_ptr<Job> job = jobAllocators[data->allocatorIndex]->allocate(&dispatch<F, Args...>, pools[i], i);

  if (job.get() == nullptr)
  {
    throw std::runtime_error("error allocating job");
  }

  size_t offset = calculateOffset<JobData<F, Args...>>();
  void *buffer = (void *)(reinterpret_cast<char *>(job.get()) + offset);

  JobData<F, Args...> *jobData = new (buffer) JobData<F, Args...>{job, std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...), i};

  // auto sharedJob = std::shared_ptr<jobsystem::Job>(
  //     job,
  //     [allocatorIndex](jobsystem::Job *j)
  //     {
  //       jobAllocators[allocatorIndex]->deallocate(j);
  //     });

  Promise<Ret> p;

  if constexpr (std::is_void_v<Ret>)
  {
    p = Promise<Ret>(std::move(job));
  }
  else
  {
    p = Promise<Ret>(std::move(job), &jobData->result);
  }

  // os::print("enqueuing %p\n", job.get());
  os::print("enqueuing %p, %p\n", job.get(), jobData->job.get());

  jobQueues[data->queueIndex]->enqueue(job);

  return p;
}

template <typename T> inline T &JobSystem::wait(Promise<T> &promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return promise->get();
  }
  */

  sleepAndWakeOnPromiseResolve(promise.job);

  return *(promise.data);
}

template <typename T> inline T &JobSystem::wait(Promise<T> &&promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return promise->get();
  }
  */

  sleepAndWakeOnPromiseResolve(promise.job);

  return *(promise.data);
}

inline void JobSystem::wait(Promise<void> &promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return;
  }
  */
  os::print("waiting %p\n", promise.job.get());

  assert(promise.job.use_count() >= 1);
  std::shared_ptr<Job> &other = promise.job;
  sleepAndWakeOnPromiseResolve(promise.job);
}

inline void JobSystem::wait(Promise<void> &&promise)
{
  /*
  if (!promise->job.addWaiter(fiber::Fiber::current()))
  {
    return;
  }
  */
  os::print("waiting %p\n", promise.job.get());

  assert(promise.job.use_count() >= 1);
  std::shared_ptr<Job> &other = promise.job;
  sleepAndWakeOnPromiseResolve(promise.job);
}

} // namespace jobsystem