#pragma once

#include <new>
#include <stddef.h>

namespace lib
{
class SystemAllocator
{
public:
  static void init();
  static void shutdown();
  static void *alloc(size_t size);
  static void *alignedAlloc(size_t alignment, size_t size);
  static void free(void *ptr);
};

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