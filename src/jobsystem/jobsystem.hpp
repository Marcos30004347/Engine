#pragma once
#include "core/print.hpp"

#include <chrono>
#include <mutex>
#include <queue>
#include <stdio.h>
#include <thread>
#include <vector>

#include "fiber.hpp"
#include "fiberpool.hpp"

#include "lib/parallel/linearqueue.hpp"
#include "promise.hpp"

namespace jobsystem
{

struct DelayedTask
{
  std::chrono::steady_clock::time_point wake_time;
  fiber::Fiber *fiber;

  bool operator>(const DelayedTask &other) const
  {
    return wake_time > other.wake_time;
  }
};

class JobSystem
{
public:
  static void init(size_t numThreads, void (*entry)());
  static void shutdown();

  template <typename F, typename... Args> static auto enqueue(F &&f, Args &&...args);
  template <typename T> static T wait(Promise<T> promise);

  static void wait(Promise<void> promise);
  static void yield();
  static void stop();

private:
  static void timerLoop();
  static void workerLoop();

  static std::vector<std::thread> workers_;
  static lib::parallel::LinearQueue<fiber::Fiber *, 4096> tasks_;
  static std::atomic<bool> running_;
};

template <typename F, typename... Args> struct JobData
{
  F f;
  std::tuple<std::decay_t<Args>...> args;
  std::shared_ptr<PromiseHandler<std::invoke_result_t<F, Args...>>> promise;
};

template <typename F, typename... Args> static void job_dispatcher(void *data, fiber::Fiber *self)
{
  using Ret = std::invoke_result_t<F, Args...>;
  using Job = JobData<std::decay_t<F>, std::decay_t<Args>...>;

  Job *job = static_cast<Job *>(data);

  if constexpr (std::is_void_v<Ret>)
  {
    std::apply(job->f, job->args);
    job->promise->set_value();
  }
  else
  {
    Ret result = std::apply(job->f, job->args);
    job->promise->set_value(result);
  }

  delete job;

  fiber::FiberPool::release(self);
}

template <typename F, typename... Args> auto JobSystem::enqueue(F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;
  using Job = JobData<std::decay_t<F>, std::decay_t<Args>...>;

  auto promise = std::make_shared<PromiseHandler<Ret>>();

  Job *job = new Job{std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...), promise};

  fiber::Fiber *fiber = fiber::FiberPool::acquire(&job_dispatcher<F, Args...>, static_cast<void *>(job));

  tasks_.push(fiber);

  return promise;
}

// template <typename F, typename... Args> Promise<void> JobSystem::enqueueGroup(F &&f, size_t size, size_t group_size, Args *...arrays)
// {
//   auto promise = std::make_shared<PromiseHandler<void>>();

//   struct Context
//   {
//     F func;
//     size_t size;
//     size_t group_size;
//     std::tuple<Args *...> arrays;
//   };

//   auto context = std::make_shared<Context>(Context{std::forward<F>(f), size, group_size, std::tuple<Args *...>(arrays...)});

//   auto fiber = fiber::FiberPool::acquire(
//       [context, promise]() mutable
//       {
//         dispatchGroup(*context, 0, context->size);
//         promise->set_value();
//       });

//   while (!tasks_.push(fiber))
//   {
//   }
//   safe_print("pushed %p\n", fiber);

//   return promise;
// }

// template <typename Context> void JobSystem::dispatchGroup(Context &context, size_t start, size_t end)
// {
//   if (end - start <= context.group_size)
//   {
//     std::apply(
//         [&](auto *...arrays)
//         {
//           context.func(start, context.size, context.group_size, arrays...);
//         },
//         context.arrays);
//     return;
//   }

//   size_t mid = start + (end - start) / 2;

//   auto left = enqueue(
//       [=, &context]()
//       {
//         dispatchGroup(context, start, mid);
//       });

//   auto right = enqueue(
//       [=, &context]()
//       {
//         dispatchGroup(context, mid, end);
//       });

//   wait(left);
//   wait(right);
//   safe_print("      dispatch end\n");
// }

template <typename T> inline T JobSystem::wait(Promise<T> promise)
{
  while (!promise->is_ready())
  {
    yield();
  }

  return promise->get();
}

inline void JobSystem::wait(Promise<void> promise)
{
  while (!promise->is_ready())
  {
    yield();
  }

  promise->get();
}

} // namespace jobsystem