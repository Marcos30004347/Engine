#pragma once
#include "fcontext/fcontext.h"
#include "os/Thread.hpp"
#include <atomic>
#include <functional>

// ================= Sanitizer detection =================

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ASYNC_HAS_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define ASYNC_HAS_TSAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define ASYNC_HAS_ASAN 1
#endif

#if defined(__SANITIZE_THREAD__)
#define ASYNC_HAS_TSAN 1
#endif

// ================= ASAN API =================

#if defined(ASYNC_HAS_ASAN)
extern "C"
{
  void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *stack_bottom, size_t stack_size);
  void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **old_stack_bottom, size_t *old_stack_size);
}
#endif

// ================= TSAN API =================

#if defined(ASYNC_HAS_TSAN)
extern "C"
{
  void *__tsan_get_current_fiber(void);
  void *__tsan_create_fiber(unsigned flags);
  void __tsan_destroy_fiber(void *fiber);
  void __tsan_switch_to_fiber(void *fiber, unsigned flags);
}
#endif

namespace async
{
namespace fiber
{

struct Fiber
{
  using Handler = void (*)(void *, Fiber *);
  bool terminated = false;
  Fiber *next = nullptr;
  Fiber *from = nullptr;

  fcontext_t ctx = nullptr;
  fcontext_stack_t stack{};
  size_t stack_size = 0;

  Handler handler = nullptr;
  void *userData = nullptr;
  bool isThreadFiber = false;

#if defined(ASYNC_HAS_ASAN)
  void *asan_fake_stack = nullptr;
  void *asan_stack_bottom = nullptr;
  size_t asan_stack_size = 0;
#endif

#if defined(ASYNC_HAS_TSAN)
  void *tsanFiber = nullptr;
#endif

  static thread_local Fiber *currentThreadFiber;

  Fiber();
  Fiber(Handler, void *userData, size_t stacksize, bool preFault = false);
  ~Fiber();

  void reset(Handler, void *userData);

  static void switchTo(Fiber *);
  static Fiber *current();
  static Fiber *currentThreadToFiber(Fiber *f);

  size_t getStackSize();

  static size_t getPageSize();
  static size_t getMinSize();
  static size_t getMaxSize();
  static size_t getDefaultSize();
};

} // namespace fiber
} // namespace async
