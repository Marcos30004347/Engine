#include "Fiber.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

static thread_local Fiber *currentFiber = nullptr;

static void fiber_entry(fcontext_transfer_t t)
{
  Fiber *self = (Fiber *)t.data;
  self->from->ctx = t.ctx;

  currentFiber = self;

  self->started = true;
  self->run();
  self->finished = true;

  // threadSafePrintf("jump entry %p %p, %p %p\n", self, self->from);
  // threadSafePrintf("finished resume %p\n", self);

  fcontext_transfer_t r = jump_fcontext(self->from->ctx, self);

  abort();

  // Fiber* self = (Fiber*)t.data;
  // currentFiber = self;
  // self->from->ctx = r.ctx;
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
  stack = create_fcontext_stack(1024 * 1024);
  ctx = NULL;
  stack_size = 1024 * 1024;
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

  finished = false;
  started = false;
  from = nullptr;
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
}

void Fiber::run()
{
  handler(userData, this);
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
  fcontext_t old_ctx = currentFiber->ctx;

  Fiber *curr = currentFiber;
  other->from = currentFiber;

  // threadSafePrintf("jump resume %p\n", other);

  fcontext_transfer_t r = jump_fcontext(other->ctx, other);

  // threadSafePrintf("jump return %p\n", curr);

  Fiber *self = (Fiber *)r.data;

  self->from->ctx = r.ctx;

  // threadSafePrintf("return %p %p\n",  curr, currentFiber);

  currentFiber = curr;
}
