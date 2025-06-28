#pragma once

#include "lib/memory/SystemMemoryManager.hpp"

namespace lib
{
namespace memory
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
    return (T *)lib::memory::SystemMemoryManager::malloc(sizeof(T) * n, hint);
  }
  T *allocateAlligned(size_t n, size_t alignment, void *hint = 0)
  {
    return (T *)lib::memory::SystemMemoryManager::allignedMalloc(sizeof(T) * n, alignment, hint);
  }
  void deallocate(T *ptr, size_t n)
  {
    return lib::memory::SystemMemoryManager::free(ptr);
  }

  void deallocate(T *ptr)
  {
    return lib::memory::SystemMemoryManager::free(ptr);
  }

private:
};
} // namespace allocator
} // namespace memory
} // namespace lib