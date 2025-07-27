#include "Fiber.hpp"

#include "os/Thread.hpp"
#include "os/print.hpp"

#include <cassert>

namespace async
{
namespace fiber
{

#if _WIN32
size_t Fiber::getPageSize()
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}

size_t Fiber::getMinSize()
{
  return MINSIGSTKSZ;
}

size_t Fiber::getMaxSize()
{
  return 1 * 1024 * 1024 * 1024; /* 1GB */
}

size_t Fiber::getDefaultSize()
{
  return 131072; // 128kb
}
#endif

#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
#include <unistd.h>
#define _HAVE_POSIX 1
#endif

#if defined(_HAVE_POSIX)

size_t Fiber::getPageSize()
{
  return (size_t)sysconf(_SC_PAGESIZE);
}

size_t Fiber::getMinSize()
{
  return MINSIGSTKSZ;
}

size_t Fiber::getMaxSize()
{
  struct rlimit limit;
  getrlimit(RLIMIT_STACK, &limit);
  return (size_t)limit.rlim_max;
}

size_t Fiber::getDefaultSize()
{
  return SIGSTKSZ;
}
#endif

ThreadCache<Fiber *> *Fiber::currentThreadFiber = nullptr;
// thread_local  Fiber *Fiber::current() = nullptr;

void Fiber::initializeSubSystems(size_t threads)
{
  currentThreadFiber = new ThreadCache<Fiber *>(threads);
}

void Fiber::deinitializeSubSystems()
{
  delete currentThreadFiber;
}

static void fiber_entry(fcontext_transfer_t t)
{
  Fiber *self = (Fiber *)t.data;

  self->from->ctx = t.ctx;

  Fiber::currentThreadFiber->update(os::Thread::getCurrentThreadId(), self);

  self->started = true;

  self->handler(self->userData, const_cast<Fiber *>(self));
  self->finished = true;

  Fiber::switchTo(self->from);
}

Fiber *Fiber::current()
{
  Fiber *curr = *currentThreadFiber->get(os::Thread::getCurrentThreadId());
  if (!curr)
  {
    abort();
  }
  assert(curr);
  return curr;
}

Fiber *Fiber::currentThreadToFiber()
{
  Fiber *f = new Fiber();

  currentThreadFiber->set(os::Thread::getCurrentThreadId(), f);

  // os::print("Thread %u setting current fiber to %p\n", os::Thread::getCurrentThreadId(), Fiber::current());

  // os::print("Thread %u fiber is %p\n", os::Thread::getCurrentThreadId(), f);

  return f;
}

Fiber::Fiber()
{
  size_t pageSize = getPageSize();
  stack = create_fcontext_stack(pageSize * 2);
  ctx = NULL;
  stack_size = 4096 * 2;
}

static void pre_fault_stack(void *stack_top, size_t size)
{
  size_t page = Fiber::getPageSize();

  if (size <= page)
  {
    return;
  }

  char *bottom = (char *)stack_top - size;
  char *p = bottom + page;

  size_t usable = size - page;

  for (size_t off = 0; off < usable; off += page)
  {
    p[off] = 0;
  }
}

Fiber::Fiber(Handler handler, void *userData, size_t ssize)
{
  this->handler = handler;
  this->userData = userData;

  stack = create_fcontext_stack(ssize < 2 * getPageSize() ? 2 * getPageSize() : ssize);
  pre_fault_stack(stack.sptr, stack.ssize);

  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
  stack_size = ssize;
}

size_t Fiber::getStackSize()
{
  return stack_size;
}

Fiber::~Fiber()
{
  destroy_fcontext_stack(&stack);
}

void Fiber::reset(Handler handler, void *userData)
{
  this->handler = handler;
  this->userData = userData;

  finished = false;
  started = false;
  from = nullptr;

  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
}

void Fiber::switchTo(Fiber *other)
{
  assert(other);

  Fiber *curr = Fiber::current();
  other->from = curr;

  currentThreadFiber->update(os::Thread::getCurrentThreadId(), other);

  fcontext_transfer_t r = jump_fcontext(other->ctx, (void *)other);

  Fiber *self = (Fiber *)r.data;
  self->from->ctx = r.ctx;

  currentThreadFiber->update(os::Thread::getCurrentThreadId(), self);
}

} // namespace fiber
} // namespace async