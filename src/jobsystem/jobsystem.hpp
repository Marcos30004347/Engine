#pragma once

#include <chrono>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "fiber/fiber.hpp"
#include "fiber/fiberpool.hpp"

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
  static void init(size_t numThreads = std::thread::hardware_concurrency());
  static void shutdown();

  template <typename F, typename... Args> static auto invoke(F &&f, Args &&...args);

  template <typename T> static T wait(Promise<T> promise);

  static void wait(Promise<void> promise);
  static void delay(uint64_t ms);
  static void yield();

private:
  static inline std::priority_queue<DelayedTask, std::vector<DelayedTask>, std::greater<>> delayed_tasks_;

  static inline std::thread timer_thread_;
  static void timer_loop();

  static void worker_loop();

  static inline std::vector<std::thread> workers_;
  static inline std::queue<std::function<void()>> tasks_;
  static inline std::mutex queue_mutex_;
  static inline std::condition_variable cv_;
  static inline std::atomic<bool> running_;
};

template <typename F, typename... Args> auto JobSystem::invoke(F &&f, Args &&...args)
{
  using Ret = std::invoke_result_t<F, Args...>;
  auto promise = std::make_shared<PromiseHandler<Ret>>();

  fiber::Fiber *fiber = fiber::FiberPool::acquire(
      [=]() mutable
      {
        if constexpr (std::is_void_v<Ret>)
        {
          f(std::forward<Args>(args)...);
          promise->set_value();
        }
        else
        {
          Ret result = f(std::forward<Args>(args)...);
          promise->set_value(result);
        }
      });

  std::lock_guard<std::mutex> lock(queue_mutex_);
  tasks_.emplace(
      [fiber]()
      {
        fiber->resume();
        fiber::FiberPool::release(fiber);
      });

  cv_.notify_one();
  return promise;
}

template <typename T> inline T JobSystem::wait(Promise<T> promise)
{
  return promise->get();
}

inline void JobSystem::wait(Promise<void> promise)
{
  promise->get();
}
} // namespace jobsystem