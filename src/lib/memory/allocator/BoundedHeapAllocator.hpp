#pragma once

#include "o1heap/o1heap.h"

namespace lib
{
namespace allocator
{
template <typename T> class BoundedHeapAllocator 
{
public:
  BoundedHeapAllocator(void *buffer, size_t capacity)
  {
    this->capacity = capacity;
    this->buffer = buffer;
    this->instance = o1heapInit(this->buffer, capacity);
  }

  ~BoundedHeapAllocator()
  {
  }

  T *allocate(size_t n, void *hint = 0)
  {
    return (T *)o1heapAllocate(instance, sizeof(T) * n);
  }

  void deallocate(T *ptr, size_t n = 1)
  {
    return o1heapFree(instance, ptr);
  }

private:
  size_t capacity;
  void *buffer;
  O1HeapInstance *instance;
};
} // namespace allocator
} // namespace lib