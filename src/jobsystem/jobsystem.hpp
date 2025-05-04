#pragma once
#include <thread>

#include "fiber.hpp"
#include "fiberpool.hpp"

#include "lib/parallel/priorityqueue.hpp"
#include "lib/parallel/queue.hpp"
#include "lib/time.hpp"
#include "lib/vector.hpp"

#include "promise.hpp"

namespace jobsystem
{

class JobSystem
{
public:
  static void init(void (*entry)(), size_t numThreads = std::thread::hardware_concurrency());
  static void shutdown();

  template <typename F, typename... Args> static auto enqueue(F &&f, Args &&...args);
  template <typename T> static T wait(Promise<T> promise);

  static void wait(Promise<void> promise);
  static void yield();
  static void stop();
  static void delay(lib::TimeSpan span);

private:
  static void workerLoop();
  static void sleepAndWakeOnPromiseResolve(PromiseHandler *data);
  static void wakeUpFiber(fiber::Fiber *);
  template <typename F, typename... Args> static void jobDispatcher(void *data, fiber::Fiber *self);

  static lib::Vector<std::thread> workerThreads;
  static lib::parallel::Queue<fiber::Fiber *> pendingFibers;
  static lib::parallel::PriorityQueue<fiber::Fiber *, double> waitingFibers;

  static std::atomic<bool> isRunning;
};

template <typename F, typename... Args> struct JobData
{
  F f;
  std::tuple<std::decay_t<Args>...> args;
  std::shared_ptr<PromiseContainer<std::invoke_result_t<F, Args...>>> promise;
};

template <typename F, typename... Args> void JobSystem::jobDispatcher(void *data, fiber::Fiber *self)
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

  job->promise->handler.foreachWatcher(JobSystem::wakeUpFiber);

  delete job;

  fiber::FiberPool::release(self);
}

template <typename F, typename... Args> auto JobSystem::enqueue(F &&f, Args &&...args)
{
  // TODO: add a job pool or special allocator
  using Ret = std::invoke_result_t<F, Args...>;
  using Job = JobData<std::decay_t<F>, std::decay_t<Args>...>;

  auto promise = std::make_shared<PromiseContainer<Ret>>();

  Job *job = new Job{std::forward<F>(f), std::make_tuple(std::forward<Args>(args)...), promise};

  fiber::Fiber *fiber = fiber::FiberPool::acquire(&jobDispatcher<F, Args...>, static_cast<void *>(job));

  pendingFibers.enqueue(fiber);

  return promise;
}

template <typename T> inline T JobSystem::wait(Promise<T> promise)
{
  sleepAndWakeOnPromiseResolve(&promise->handler);
  return promise->get();
}

inline void JobSystem::wait(Promise<void> promise)
{
  sleepAndWakeOnPromiseResolve(&promise->handler);
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

} // namespace jobsystem