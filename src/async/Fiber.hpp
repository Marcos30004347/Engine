#pragma once

#include "fcontext/fcontext.h"
#include <atomic>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "ThreadCache.hpp"

namespace async
{
namespace fiber
{
struct Fiber
{
  typedef void (*Handler)(void *, Fiber *);

  Fiber *from;
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
  static void switchTo(Fiber *);

  static Fiber *current();
  static Fiber *currentThreadToFiber();
  size_t getStackSize();

  // static thread_local  Fiber *currentFiber;
  static void initializeSubSystems(size_t threads = 2 * os::Thread::getHardwareConcurrency());
  static void deinitializeSubSystems();
  static ThreadCache<Fiber *> *currentThreadFiber;

  static size_t getPageSize();
  static size_t getMinSize();
  static size_t getMaxSize();
  static size_t getDefaultSize();

private:
  Fiber();
};

} // namespace fiber
} // namespace async