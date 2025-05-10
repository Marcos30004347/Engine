#pragma once

#include "fcontext/fcontext.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace jobsystem
{
namespace fiber
{
struct Fiber
{
  typedef void (*Handler)(void*, Fiber*);

  Fiber* from;
  size_t stack_size;
  
  fcontext_t ctx = nullptr;
  fcontext_stack_t stack{};

  Handler handler;

  void* userData = nullptr;
  std::atomic<bool> finished{false};
  std::atomic<bool> started{false};
  std::atomic<bool> stalled{false};

  Fiber(Handler, void* userData, size_t stacksize);
  ~Fiber();

  void reset(Handler,  void* userData);
  void run();
  static void switchTo(Fiber*);

  static Fiber *current();
  static Fiber *currentThreadToFiber();
  size_t getStackSize();

private:
  Fiber();
  static fcontext_transfer_t yield_entry(fcontext_transfer_t t);
};

} // namespace fiber
} // namespace jobsystem