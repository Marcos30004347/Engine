#pragma once

#include "memory/allocator/SystemAllocator.hpp"

namespace async
{
template <typename T> class Stack
{
public:
  Stack(uint32_t capacity) : capacity(capacity), head(0)
  {
    data = (T *)lib::memory::SystemMemoryManager::malloc(sizeof(T) * capacity);
  }

  ~Stack()
  {
    lib::memory::SystemMemoryManager::free(data);
  }

  template <typename U> inline bool push(U &&value)
  {
    if (head >= capacity)
    {
      return false;
    }

    new (&data[head++]) T(std::forward<U>(value));

    return true;
  }

  inline bool pop(T &out)
  {
    if (head == 0)
    {
      return false;
    }

    out = std::move(data[--head]);
    return true;
  }

  inline uint32_t size()
  {
    return head;
  }

private:
  T *data;
  uint32_t capacity;
  uint32_t head;
};
} // namespace async