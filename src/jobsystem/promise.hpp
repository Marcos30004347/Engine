#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace jobsystem
{
template <typename T> class PromiseHandler
{
public:
  PromiseHandler() : ready_(false)
  {
  }

  void set_value(const T &value)
  {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      value_ = value;
      ready_ = true;
    }
    cv_.notify_all();
  }

  T get()
  {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(
        lock,
        [&]
        {
          return ready_.load();
        });
    return value_;
  }

  bool is_ready() const
  {
    return ready_.load();
  }

private:
  T value_;
  std::atomic<bool> ready_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
};

template <> class PromiseHandler<void>
{
public:
  PromiseHandler() : ready_(false)
  {
  }

  void set_value()
  {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      ready_ = true;
    }
    cv_.notify_all();
  }

  void get()
  {
    std::unique_lock<std::mutex> lock(mtx_);
    
    cv_.wait(
        lock,
        [&]
        {
          return ready_.load();
        });
  }

  bool is_ready() const
  {
    return ready_.load();
  }

private:
  std::atomic<bool> ready_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
};

template <typename T> using Promise = std::shared_ptr<PromiseHandler<T>>;
} // namespace jobsystem