#include "Fiber.hpp"

#include "os/print.hpp"
#include <cassert>

using namespace jobsystem;
using namespace jobsystem::fiber;

static thread_local Fiber *currentFiber = nullptr;

static void fiber_entry(fcontext_transfer_t t)
{
  Fiber *self = (Fiber *)t.data;

  // if (!self || !self->from)
  // {
  //   std::cerr << "Invalid fiber or from pointer" << std::endl;
  //   std::abort();
  // }

  self->from->ctx = t.ctx;

  currentFiber = self;

  self->started = true;
  self->handler(self->userData, self);
  self->finished = true;

  fcontext_transfer_t r = jump_fcontext(self->from->ctx, self);
}

Fiber *Fiber::current()
{
  return currentFiber;
}

Fiber *Fiber::currentThreadToFiber()
{
  Fiber *f = new Fiber();
  currentFiber = f;
  return f;
}

Fiber::Fiber()
{
  stack.ssize = 0; // create_fcontext_stack(1024 * 1024);
  stack.sptr = NULL;
  ctx = NULL;
  stack_size = 0;
}

Fiber::Fiber(Handler handler, void *userData, size_t ssize)
{
  this->handler = handler;
  this->userData = userData;

  stack = create_fcontext_stack(ssize);
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

  os::print("resetting %p\n", this);

  finished = false;
  started = false;
  from = nullptr;
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
}

// fcontext_transfer_t yieldHandler(fcontext_transfer_t t)
// {
//   Fiber* self = (Fiber*)t.data;
//   // self->from->ctx = t.ctx;
//   safe_print("yield resume %p %p\n", self, self->from);

//   return {t.ctx, t.data};
// }

void Fiber::switchTo(Fiber *other)
{
  os::print("switching to %p\n", other);

  
  Fiber *curr = currentFiber;
  other->from = currentFiber;

  fcontext_transfer_t r = jump_fcontext(other->ctx, other);

  Fiber *self = (Fiber *)r.data;

  self->from->ctx = r.ctx;

  currentFiber = curr;
}
