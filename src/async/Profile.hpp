// #define ASYNC_MANAGER_LOG_TIMES

#ifdef ASYNC_MANAGER_LOG_TIMES

#pragma once

#include "os/print.hpp"
#include "time/TimeSpan.hpp"
#include <atomic>

namespace async
{
namespace profiling
{
struct Counter
{
  std::atomic<uint64_t> total_ns{0};
  std::atomic<uint64_t> count{0};

  void add(lib::time::TimeSpan t)
  {
    total_ns.fetch_add((uint64_t)t.nanoseconds(), std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
  }

  double averageNanoseconds() const
  {
    uint64_t c = count.load();
    return c ? double(total_ns.load()) / double(c) : 0.0;
  }
};

struct Stats
{
  Counter switchFiber;
  Counter jobExecution;
  Counter enqueue;
  Counter workerLoop;
};

extern Stats gStats;

struct ScopedTimer
{
  Counter &counter;
  lib::time::Timer timer;

  ScopedTimer(Counter &c) : counter(c)
  {
    timer.start();
  }

  ~ScopedTimer()
  {
    counter.add(timer.end());
  }
};

inline void report()
{
  os::print("\n=== AsyncManager Average Timing Report ===\n");
  os::print("context return : %.2f ns, total: %u\n", gStats.switchFiber.averageNanoseconds(), gStats.switchFiber.count.load());
  os::print("execution      : %.2f ns, total: %u\n", gStats.jobExecution.averageNanoseconds(), gStats.jobExecution.count.load());
  os::print("enqueue        : %.2f ns, total: %u\n", gStats.enqueue.averageNanoseconds(), gStats.enqueue.count.load());
  os::print("workerLoop     : %.2f ns, total: %u\n", gStats.workerLoop.averageNanoseconds(), gStats.workerLoop.count.load());
  os::print("=================================\n");
}

} // namespace profiling
} // namespace async
#endif
