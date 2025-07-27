#pragma once

#include "fcontext/fcontext.h"
#include <atomic>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "ThreadCache.hpp"


namespace jobsystem
{
namespace fiber
{
struct Fiber
{
  typedef void (*Handler)(void *, Fiber *);

  volatile Fiber *from;
  size_t stack_size;

  fcontext_t ctx = nullptr;
  fcontext_stack_t stack{};

  Handler handler;

  void *userData = nullptr;
  std::atomic<bool> finished{false};
  std::atomic<bool> started{false};
  std::atomic<bool> stalled{false};

  Fiber(Handler, void *userData, size_t stacksize);
  ~Fiber();

  void reset(Handler, void *userData);
  static void switchTo(volatile Fiber *);

  static volatile Fiber *current();
  static volatile Fiber *currentThreadToFiber();
  size_t getStackSize();

  // static thread_local volatile Fiber *currentFiber;
  static void initializeSubSystems(size_t threads = 2 * os::Thread::getHardwareConcurrency());
  static void deinitializeSubSystems();
  static ThreadCache<volatile Fiber *> *currentThreadFiber;

private:
  Fiber();
};

} // namespace fiber
} // namespace jobsystem