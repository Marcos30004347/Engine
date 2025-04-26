#include "fiber.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

static thread_local Fiber *current_fiber = nullptr;

Fiber *Fiber::current()
{
  return current_fiber;
}

static void fiber_entry(fcontext_transfer_t t)
{
  Fiber *self = (Fiber *)t.data;
  current_fiber = self;
  self->scheduler_ctx = t.ctx;
  self->run();
  self->finished = true;
  jump_fcontext(self->scheduler_ctx, self);
}

fcontext_transfer_t Fiber::yield_entry(fcontext_transfer_t t)
{
  Fiber *self = (Fiber *)t.data;
  self->scheduler_ctx = t.ctx;
  return {self->ctx, self};
}

Fiber::Fiber(std::function<void()> fn_) : fn(std::move(fn_))
{
  stack = create_fcontext_stack(256);
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
}

Fiber::~Fiber()
{
  destroy_fcontext_stack(&stack);
}

void Fiber::reset(std::function<void()> new_fn)
{
  fn = std::move(new_fn);
  finished = false;
  ctx = make_fcontext(stack.sptr, stack.ssize, fiber_entry);
}

void Fiber::run()
{
  fn();
}

void Fiber::resume()
{
  jump_fcontext(ctx, this);
}

void Fiber::yield()
{
  if (current_fiber)
  {
    ontop_fcontext(current_fiber->scheduler_ctx, current_fiber, yield_entry);
  }
}