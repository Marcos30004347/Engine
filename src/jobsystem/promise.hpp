#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "lib/parallel/vector.hpp"

namespace jobsystem
{

class PromiseHandler
{
public:
  PromiseHandler() : ready(false), spinLock(false)
  {
  }
  bool is_ready() const
  {
    return ready.load();
  }

  void lock()
  {
    bool expected = false;
    while (spinLock.compare_exchange_weak(expected, true))
    {
    }
  }

  void unlock()
  {
    bool expected = true;
    while (spinLock.compare_exchange_weak(expected, false))
    {
    }
  }

  bool addToWatchGroup(fiber::Fiber *f)
  {
    if (ready.load())
    {
      return false;
    }

    waiters.push_back(f);
    return true;
  }

  void foreachWatcher(void (*callback)(fiber::Fiber *))
  {
    for (uint32_t i = 0; i < waiters.size(); i++)
    {
      callback(waiters[i]);
    }
  }

  std::atomic<bool> ready;
  std::atomic<bool> spinLock;
  lib::parallel::Vector<fiber::Fiber *> waiters;
};

template <typename T> class PromiseContainer
{
  friend class JobSystem;

public:
  PromiseContainer()
  {
  }
  void set_value(const T &value)
  {
    handler.lock();
    value_ = value;
    handler.ready = true;
    handler.unlock();
  }
  T get()
  {
    return value_;
  }

private:
  T value_;
  PromiseHandler handler;
};

template <> class PromiseContainer<void>
{
  friend class JobSystem;

public:
  PromiseContainer()
  {
  }

  void set_value()
  {
    handler.lock();
    handler.ready = true;
    handler.unlock();
  }

private:
  PromiseHandler handler;
};

template <typename T> using Promise = std::shared_ptr<PromiseContainer<T>>;
} // namespace jobsystem