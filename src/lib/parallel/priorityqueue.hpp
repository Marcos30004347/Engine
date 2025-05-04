#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

namespace lib
{
namespace parallel
{
template <typename T, typename P = int> class PriorityQueue
{
public:
  struct Element
  {
    T value;
    P priority;

    bool operator<(const Element &other) const
    {
      return priority > other.priority;
    }
  };

  void push(const T &value, P priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace(Element{value, priority});
    cond_var_.notify_one();
  }

  void push(T &&value, P priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.emplace(Element{std::move(value), priority});
    cond_var_.notify_one();
  }

  T wait_and_pop()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_var_.wait(
        lock,
        [&]
        {
          return !queue_.empty();
        });

    T val = std::move(queue_.top().value);
    queue_.pop();
    return val;
  }
  bool try_pop(T &val)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;

    val = std::move(queue_.top().value);
    queue_.pop();
    return true;
  }

  bool try_pop(T &val, P &priority)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
      return false;

    val = std::move(queue_.top().value);
    priority = queue_.top().priority;

    queue_.pop();
    return true;
  }

  bool empty() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  std::size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cond_var_;
  std::priority_queue<Element> queue_;
};
} // namespace parallel
} // namespace lib