#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

#include <concurrentqueue/concurrentqueue.h>

namespace lib
{
namespace parallel
{

template <typename T> class Queue
{
private:
  moodycamel::ConcurrentQueue<T> queue;

public:
  Queue() = default;

  void enqueue(const T &value)
  {
    queue.enqueue(value);
  }

  bool dequeue(T &value)
  {
    return queue.try_dequeue(value);
  }

  bool empty() const
  {
    return queue.is_empty();
  }
};
} // namespace parallel
} // namespace lib