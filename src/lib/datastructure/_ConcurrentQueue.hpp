#pragma once

#include <concurrentqueue/concurrentqueue.h>

namespace lib
{
template <typename T> class ConcurrentQueue
{
private:
  moodycamel::ConcurrentQueue<T> queue;

public:
  ConcurrentQueue() = default;
  ~ConcurrentQueue() = default;

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

} // namespace lib