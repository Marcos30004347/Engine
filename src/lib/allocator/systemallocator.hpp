#pragma once

#include "lib/memory/SystemMemoryManager.hpp"

namespace lib
{
namespace allocator
{
template <typename T> class SystemAllocator
{
public:
  SystemAllocator()
  {
  }

  ~SystemAllocator()
  {
  }

  T *allocate(size_t n, void *hint = 0)
  {
    return (T *)lib::memory::SystemMemoryManager::malloc(sizeof(T) * n);
  }

  void deallocate(T *ptr, size_t n)
  {
    return lib::memory::SystemMemoryManager::free(ptr);
  }

private:
};
} // namespace allocator
} // namespace lib