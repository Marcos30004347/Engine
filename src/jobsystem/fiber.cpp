#include "Fiber.hpp"

#include "os/Thread.hpp"
#include "os/print.hpp"

#include <cassert>

namespace jobsystem
{
namespace fiber
{

#if _WIN32
static size_t getPageSize()
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (size_t)si.dwPageSize;
}
#endif

#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
#include <unistd.h>
#define _HAVE_POSIX 1
#endif

#if defined(_HAVE_POSIX)
static size_t getPageSize()
{
  return (size_t)sysconf(_SC_PAGESIZE);
}

static size_t getMinSize()
{
  return MINSIGSTKSZ;
}

static size_t getMaxSize()
{
  struct rlimit limit;
  getrlimit(RLIMIT_STACK, &limit);

  return (size_t)limit.rlim_max;
}

#endif

ThreadCache<volatile Fiber *> *Fiber::currentThreadFiber = nullptr;
// thread_local volatile Fiber *Fiber::current() = nullptr;

void Fiber::initializeSubSystems(size_t threads)
{
  currentThreadFiber = new ThreadCache<volatile Fiber *>(threads);
}

void Fiber::deinitializeSubSystems()
{
  delete currentThreadFiber;
}

static void fiber_entry(fcontext_transfer_t t)
{
  volatile Fiber *self = (volatile Fiber *)t.data;

  self->from->ctx = t.ctx;

  Fiber::currentThreadFiber->update(os::Thread::getCurrentThreadId(), self);

  self->started = true;

  self->handler(self->userData, const_cast<Fiber *>(self));
  self->finished = true;

  Fiber::switchTo(self->from);
}

volatile Fiber *Fiber::current()
{
  volatile Fiber *curr = *currentThreadFiber->get(os::Thread::getCurrentThreadId());
  if (!curr)
  {
    abort();
  }
  assert(curr);
  return curr;
}

volatile Fiber *Fiber::currentThreadToFiber()
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
  if (size <= getPageSize())
  {
    return;
  }

  size_t page = getPageSize();
  volatile char *bottom = (char *)stack_top - size;
  volatile char *p = bottom + page;
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

void Fiber::switchTo(volatile Fiber *other)
{
  assert(other);

  volatile Fiber *curr = Fiber::current();
  other->from = curr;

  currentThreadFiber->update(os::Thread::getCurrentThreadId(), other);
 
  fcontext_transfer_t r = jump_fcontext(other->ctx, (void *)other);

  Fiber *self = (Fiber *)r.data;
  self->from->ctx = r.ctx;

  currentThreadFiber->update(os::Thread::getCurrentThreadId(), self);
}

} // namespace fiber
} // namespace jobsystem