#pragma once

#include <new>
#include <stddef.h>

namespace lib
{
namespace memory
{

class SystemMemoryManager
{
public:
  static void init();
  static void shutdown();
  static void *malloc(size_t size, void *hint = 0);
  static void *alignedAlloc(size_t alignment, size_t size);
  static void free(void *ptr);

  template <typename T> T *allocate(size_t n, void *hint = 0)
  {
    return (T *)SystemMemoryManager::malloc(n * sizeof(T));
  }

  template <typename T> void deallocate(T *ptr, size_t n)
  {
    return SystemMemoryManager::free(ptr);
  }
};
} // namespace memory

} // namespace lib

void *operator new(std::size_t size) noexcept(false);
void *operator new[](std::size_t size) noexcept(false);
void *operator new(std::size_t size, const std::nothrow_t &tag) noexcept;
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept;

void operator delete(void *p) noexcept;
void operator delete[](void *p) noexcept;

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)
void operator delete(void *p, std::size_t size) noexcept;
void operator delete[](void *p, std::size_t size) noexcept;
#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))
void operator delete(void *p, std::align_val_t align) noexcept;
void operator delete[](void *p, std::align_val_t align) noexcept;
void operator delete(void *p, std::size_t size, std::align_val_t align) noexcept;
void operator delete[](void *p, std::size_t size, std::align_val_t align) noexcept;
void *operator new(std::size_t size, std::align_val_t align) noexcept(false);
void *operator new[](std::size_t size, std::align_val_t align) noexcept(false);
void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept;
void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept;
#endif
