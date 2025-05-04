#include "systemallocator.hpp"
#include <rpmalloc/rpmalloc.h>

#include <cstdlib>

using namespace lib;

void SystemAllocator::init()
{
  if (rpmalloc_initialize(nullptr))
  {
    abort();
  }
}

void SystemAllocator::shutdown()
{
  return rpmalloc_finalize();
}

void *SystemAllocator::alloc(size_t size)
{
  return rpmalloc(size);
}

void SystemAllocator::free(void *ptr)
{
  return rpfree(ptr);
}

void *SystemAllocator::alignedAlloc(size_t alignment, size_t size)
{
  return rpaligned_alloc(alignment, size);
}

void *operator new(std::size_t size) noexcept(false)
{
  if (size == 0)
  {
    size = 1;
  }
  void *ptr = lib::SystemAllocator::alloc(size);
  if (!ptr)
  {
    throw std::bad_alloc();
  }
  return ptr;
}

void *operator new[](std::size_t size) noexcept(false)
{
  if (size == 0)
  {
    size = 1;
  }
  void *ptr = lib::SystemAllocator::alloc(size);
  if (!ptr)
  {
    throw std::bad_alloc();
  }
  return ptr;
}

void *operator new(std::size_t size, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  if (size == 0)
  {
    size = 1;
  }
  return lib::SystemAllocator::alloc(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  if (size == 0)
  {
    size = 1;
  }
  return lib::SystemAllocator::alloc(size);
}

void operator delete(void *p) noexcept
{
  lib::SystemAllocator::free(p);
}

void operator delete[](void *p) noexcept
{
  lib::SystemAllocator::free(p);
}

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)
void operator delete(void *p, std::size_t size) noexcept
{
  (void)sizeof(size);
  lib::SystemAllocator::free(p);
}

void operator delete[](void *p, std::size_t size) noexcept
{
  (void)sizeof(size);
  lib::SystemAllocator::free(p);
}
#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))
void operator delete(void *p, std::align_val_t align) noexcept
{
  (void)sizeof(align);
  lib::SystemAllocator::free(p);
}

void operator delete[](void *p, std::align_val_t align) noexcept
{
  (void)sizeof(align);
  lib::SystemAllocator::free(p);
}

void operator delete(void *p, std::size_t size, std::align_val_t align) noexcept
{
  (void)sizeof(size);
  (void)sizeof(align);
  lib::SystemAllocator::free(p);
}

void operator delete[](void *p, std::size_t size, std::align_val_t align) noexcept
{
  (void)sizeof(size);
  (void)sizeof(align);
  lib::SystemAllocator::free(p);
}

void *operator new(std::size_t size, std::align_val_t align) noexcept(false)
{
  return lib::SystemAllocator::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new[](std::size_t size, std::align_val_t align) noexcept(false)
{
  return lib::SystemAllocator::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  return lib::SystemAllocator::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  return lib::SystemAllocator::alignedAlloc(static_cast<size_t>(align), size);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
  lib::SystemAllocator::free(ptr);
}
#endif