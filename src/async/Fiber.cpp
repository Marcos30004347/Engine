
// void Fiber::switchTo(Fiber *other)
// {
//   //os::print("%u switchTo %p %p %p started\n", os::Thread::getCurrentThreadId(), other, other->ctx, Fiber::current());

//   assert(other);

// #ifdef ASYNC_MANAGER_LOG_TIMES
//   async::profiling::ScopedTimer timer(async::profiling::gStats.switchFiber);
// #endif

//   Fiber *self = Fiber::current();
//   other->from = self;

// #if defined(ASYNC_HAS_ASAN)
//   void **asan_slot = asanThreadState;

//   bool did_asan_switch = false;

//   if (other->stack.sptr)
//   {
//     void *stack_bottom = static_cast<char *>(other->stack.sptr) - other->stack.ssize;

//     __sanitizer_start_switch_fiber(asan_slot, stack_bottom, other->stack.ssize);

//     did_asan_switch = true;
//   }
// #endif

// #if defined(ASYNC_HAS_TSAN)
//   // Notify TSAN about the fiber switch
//   // Only switch if the target fiber has a valid TSAN context
//   // flags = 0 means normal switch (not terminating)
//   if (other && other->tsanFiber)
//   {
//     __tsan_switch_to_fiber(other->tsanFiber, 0);
//   }
// #endif

//   os::print("%u switchTo to=%p curr=%p\n", os::Thread::getCurrentThreadId(), (void *)other, Fiber::current());

//   currentThreadFiber = other;

//   fcontext_transfer_t r = jump_fcontext(other->ctx, (void *)other);

//   Fiber *returned = static_cast<Fiber *>(r.data);

//   assert(self == returned);

//   os::print("%u returned %p from %p\n", os::Thread::getCurrentThreadId(), returned, returned->from);

// #if defined(ASYNC_HAS_ASAN)
//   if (did_asan_switch)
//   {
//     const void *old_stack = nullptr;
//     size_t old_stack_size = 0;

//     __sanitizer_finish_switch_fiber(*asan_slot, &old_stack, &old_stack_size);
//   }
// #endif

// #if defined(ASYNC_HAS_TSAN)
//   // Switch back to the returned fiber's TSAN context
//   // Only switch if we have a valid context
//   if (returned && returned->tsanFiber)
//   {
//     __tsan_switch_to_fiber(returned->tsanFiber, 0);
//   }
// #endif
//   returned->from->ctx = r.ctx;
//   currentThreadFiber = returned;
// }

#include "Fiber.hpp"
#include "os/print.hpp"
#include <cassert>
#include <sys/resource.h>
#include <unistd.h>

namespace async
{
namespace fiber
{

thread_local Fiber *Fiber::currentThreadFiber = nullptr;

// ================= Utilities =================

size_t Fiber::getPageSize()
{
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

size_t Fiber::getMinSize()
{
#if defined(ASYNC_HAS_ASAN) || defined(ASYNC_HAS_TSAN)
  return 256 * 1024;
#else
  return MINSIGSTKSZ;
#endif
}

size_t Fiber::getMaxSize()
{
  rlimit limit{};
  getrlimit(RLIMIT_STACK, &limit);
  return static_cast<size_t>(limit.rlim_max);
}

size_t Fiber::getDefaultSize()
{
#if defined(ASYNC_HAS_ASAN) || defined(ASYNC_HAS_TSAN)
  return 256 * 1024;
#else
  return SIGSTKSZ;
#endif
}

// ================= Fiber entry =================

static void fiber_entry(fcontext_transfer_t t)
{
#if defined(ASYNC_HAS_ASAN)
  __sanitizer_finish_switch_fiber(
      Fiber::currentThreadFiber->asan_fake_stack, (const void **)&Fiber::currentThreadFiber->asan_stack_bottom, &Fiber::currentThreadFiber->asan_stack_size);
  // os::print(
  //     "%u %p <<< %p, at entry, stack=%p, %u\n",
  //     os::Thread::getCurrentThreadId(),
  //     Fiber::currentThreadFiber,
  //     Fiber::currentThreadFiber->from,
  //     Fiber::currentThreadFiber->asan_fake_stack,
  //     Fiber::currentThreadFiber->asan_stack_size);

#endif
  Fiber *self = static_cast<Fiber *>(t.data);
  self->from->ctx = t.ctx;

  assert(Fiber::currentThreadFiber == self);
  assert(!self->isThreadFiber);

  self->handler(self->userData, self);
  self->terminated = true;

  Fiber::switchTo(self->from);
  assert(false && "Fiber already terminated");
}

// ================= Fiber lifecycle =================

Fiber::Fiber() = default;

Fiber::Fiber(Handler h, void *ud, size_t ssize, bool prefault) : handler(h), userData(ud)
{
  size_t minSize = getMinSize(); //2 * getPageSize();
  size_t allocSize = ssize < minSize ? minSize : ssize;
  
  terminated = false;

  stack = create_fcontext_stack(allocSize);
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);

  stack_size = stack.ssize;
  isThreadFiber = false;

#if defined(ASYNC_HAS_ASAN)
  asan_fake_stack = nullptr;
  asan_stack_bottom = static_cast<void *>(static_cast<char *>(stack.sptr) - stack.ssize);
  asan_stack_size = stack.ssize;
#endif

#if defined(ASYNC_HAS_TSAN)
  tsanFiber = __tsan_create_fiber(0);
#endif
}

Fiber::~Fiber()
{
  if (stack.sptr)
  {
    destroy_fcontext_stack(&stack);
  }

#if defined(ASYNC_HAS_TSAN)
  if (tsanFiber)
  {
    __tsan_destroy_fiber(tsanFiber);
    tsanFiber = nullptr;
  }
#endif
}

void Fiber::reset(Handler h, void *ud)
{

  handler = h;
  userData = ud;
  from = nullptr;
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
  terminated = false;
  isThreadFiber = false;
#if defined(ASYNC_HAS_ASAN)
  asan_fake_stack = nullptr;
  asan_stack_bottom = static_cast<void *>(static_cast<char *>(stack.sptr) - stack.ssize);
  asan_stack_size = stack.ssize;
#endif
#if defined(ASYNC_HAS_TSAN)
  if (tsanFiber)
    __tsan_destroy_fiber(tsanFiber);
  tsanFiber = __tsan_create_fiber(0);
#endif
}

// ================= Thread ↔ Fiber =================

Fiber *Fiber::current()
{
  assert(currentThreadFiber);
  return currentThreadFiber;
}

static void thread_capture_entry(fcontext_transfer_t t)
{
  Fiber *threadFiber = static_cast<Fiber *>(t.data);

  // Save thread context
  threadFiber->ctx = t.ctx;

  // Jump back to the caller
  jump_fcontext(t.ctx, threadFiber);
}

Fiber *Fiber::currentThreadToFiber(Fiber *f)
{
  assert(f);
  //assert(!currentThreadFiber && "Thread already converted to fiber");

  f->terminated = false;
  f->isThreadFiber = true;
  f->from = nullptr;

#if defined(ASYNC_HAS_ASAN)
  f->asan_fake_stack = nullptr;
  f->asan_stack_bottom = nullptr;
  f->asan_stack_size = 0;
#endif

#if defined(ASYNC_HAS_TSAN)
  f->tsanFiber = __tsan_get_current_fiber();
#endif

  constexpr size_t trampStackSize = 64 * 1024;
  fcontext_stack_t trampStack = create_fcontext_stack(trampStackSize);

  fcontext_t trampCtx = make_fcontext(trampStack.sptr, trampStack.ssize, thread_capture_entry);

  currentThreadFiber = f;
  fcontext_transfer_t t = jump_fcontext(trampCtx, f);
  f->ctx = t.ctx;

  destroy_fcontext_stack(&trampStack);

  // os::print("> %u thread captured as fiber %p\n", os::Thread::getCurrentThreadId(), f);

  return f;
}

size_t Fiber::getStackSize()
{
  return stack_size;
}

void Fiber::switchTo(Fiber *to)
{

#if defined(ASYNC_HAS_ASAN)
  // if (Fiber::current()->isThreadFiber)
  // {
  //   __sanitizer_start_switch_fiber(nullptr, Fiber::current()->asan_stack_bottom, Fiber::current()->asan_stack_size);
  // }
  // else
  // {
    __sanitizer_start_switch_fiber(&Fiber::current()->asan_fake_stack, Fiber::current()->asan_stack_bottom, Fiber::current()->asan_stack_size);
  //}

  // os::print("%u %p >>> %p, stack=%p, %u\n", os::Thread::getCurrentThreadId(), Fiber::current(), to, Fiber::current()->asan_fake_stack, Fiber::current()->asan_stack_size);
#endif

#if defined(ASYNC_HAS_TSAN)
  __tsan_switch_to_fiber(to->tsanFiber, 0);
#endif
  assert(to);
  Fiber *self = Fiber::current();
  to->from = self;

  currentThreadFiber = to;
  // os::print("jump %p -> %p %p\n", self, to, to->ctx);
  fcontext_transfer_t t = jump_fcontext(to->ctx, to);

#if defined(ASYNC_HAS_ASAN)
  __sanitizer_finish_switch_fiber(Fiber::current()->asan_fake_stack, (const void **)&Fiber::current()->asan_stack_bottom, &Fiber::current()->asan_stack_size);
  // os::print("%u %p <<< %p, stack=%p\n", os::Thread::getCurrentThreadId(), Fiber::current(), Fiber::current()->from, Fiber::current()->asan_fake_stack);
#endif

  Fiber *returned = static_cast<Fiber *>(t.data);
  assert(returned == Fiber::current());
  returned->from->ctx = t.ctx;

#if defined(ASYNC_HAS_TSAN)
  __tsan_switch_to_fiber(returned->tsanFiber, 0);
#endif
}

} // namespace fiber
} // namespace async
