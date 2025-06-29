#include "SystemMemoryManager.hpp"
#include "os/print.hpp"

#include <atomic>
#include <rpmalloc/rpmalloc.h>

using namespace lib;
using namespace memory;

void SystemMemoryManager::init()
{
  if (rpmalloc_initialize(nullptr))
  {
    abort();
  }
}

void SystemMemoryManager::shutdown()
{
  return rpmalloc_finalize();
}

void SystemMemoryManager::initializeThread()
{
  if (!rpmalloc_is_thread_initialized())
  {
    rpmalloc_thread_initialize();
  }
}

void SystemMemoryManager::finializeThread()
{
  rpmalloc_thread_finalize();
}

void *SystemMemoryManager::malloc(size_t size, void *hint)
{
  return rpmalloc(size);
}

void *SystemMemoryManager::allignedMalloc(size_t size, size_t alignment, void *hint)
{
  return rpaligned_alloc(alignment, size);
}
void SystemMemoryManager::free(void *ptr)
{
  return rpfree(ptr);
}

void *SystemMemoryManager::alignedAlloc(size_t alignment, size_t size)
{
  return rpaligned_alloc(alignment, size);
}

void *operator new(std::size_t size) noexcept(false)
{
  if (size == 0)
  {
    size = 1;
  }

  void *ptr = lib::memory::SystemMemoryManager::malloc(size);

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
  void *ptr = lib::memory::SystemMemoryManager::malloc(size);
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
  return lib::memory::SystemMemoryManager::malloc(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  if (size == 0)
  {
    size = 1;
  }
  return lib::memory::SystemMemoryManager::malloc(size);
}

void operator delete(void *p) noexcept
{
  lib::memory::SystemMemoryManager::free(p);
}

void operator delete[](void *p) noexcept
{
  lib::memory::SystemMemoryManager::free(p);
}

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)
void operator delete(void *p, std::size_t size) noexcept
{
  (void)sizeof(size);
  lib::memory::SystemMemoryManager::free(p);
}

void operator delete[](void *p, std::size_t size) noexcept
{
  (void)sizeof(size);
  lib::memory::SystemMemoryManager::free(p);
}
#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))
void operator delete(void *p, std::align_val_t align) noexcept
{
  (void)sizeof(align);
  lib::memory::SystemMemoryManager::free(p);
}

void operator delete[](void *p, std::align_val_t align) noexcept
{
  (void)sizeof(align);
  lib::memory::SystemMemoryManager::free(p);
}

void operator delete(void *p, std::size_t size, std::align_val_t align) noexcept
{
  (void)sizeof(size);
  (void)sizeof(align);
  lib::memory::SystemMemoryManager::free(p);
}

void operator delete[](void *p, std::size_t size, std::align_val_t align) noexcept
{
  (void)sizeof(size);
  (void)sizeof(align);
  lib::memory::SystemMemoryManager::free(p);
}

void *operator new(std::size_t size, std::align_val_t align) noexcept(false)
{
  return lib::memory::SystemMemoryManager::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new[](std::size_t size, std::align_val_t align) noexcept(false)
{
  return lib::memory::SystemMemoryManager::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  return lib::memory::SystemMemoryManager::alignedAlloc(static_cast<size_t>(align), size);
}

void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &tag) noexcept
{
  (void)sizeof(tag);
  return lib::memory::SystemMemoryManager::alignedAlloc(static_cast<size_t>(align), size);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
  lib::memory::SystemMemoryManager::free(ptr);
}
#endif