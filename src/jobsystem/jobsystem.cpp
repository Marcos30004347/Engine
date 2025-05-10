#include "jobsystem.hpp"
#include <assert.h>
#include <iostream>

using namespace jobsystem;
using namespace jobsystem::fiber;

static thread_local fiber::Fiber *workerFiber = nullptr;
static thread_local fiber::Fiber *yieldedFiber = nullptr;

static thread_local fiber::Fiber *forgetedFiber = nullptr;
static thread_local PromiseHandler *promiseData = nullptr;

static thread_local fiber::Fiber *waitedFiber = nullptr;
static thread_local double waitedFiberNs;

lib::Vector<std::thread> JobSystem::workerThreads;
lib::parallel::Queue<fiber::Fiber *> JobSystem::pendingFibers = lib::parallel::Queue<fiber::Fiber *>();
lib::parallel::PriorityQueue<fiber::Fiber *, double> JobSystem::waitingFibers = lib::parallel::PriorityQueue<fiber::Fiber *, double>();
std::atomic<bool> JobSystem::isRunning = false;

void JobSystem::init(void (*entry)(), size_t numThreads)
{
  FiberPool::init();
  
  isRunning = true;
  enqueue(entry);

  for (size_t i = 0; i < numThreads; ++i)
  {
    workerThreads.emplaceBack(workerLoop);
  }

  workerLoop();

}

void JobSystem::stop()
{
  isRunning.store(false);
}

void JobSystem::shutdown()
{
  for (uint32_t i = 0; i < workerThreads.size(); i++)
  {
    workerThreads[i].join();
  }

  workerThreads.clear();
  FiberPool::shutdown();
}

void JobSystem::workerLoop()
{
  workerFiber = fiber::Fiber::currentThreadToFiber();

  while (JobSystem::isRunning)
  {
    assert(fiber::Fiber::current() == workerFiber);

    if (yieldedFiber != nullptr)
    {
      pendingFibers.enqueue(yieldedFiber);
    }

    yieldedFiber = nullptr;

    if (forgetedFiber)
    {
      assert(promiseData);

      promiseData->lock();

      if (!promiseData->enqueueWaiter(forgetedFiber))
      {
        pendingFibers.enqueue(forgetedFiber);
      }

      promiseData->unlock();
    }

    forgetedFiber = nullptr;
    promiseData = nullptr;

    if (waitedFiber)
    {
      waitingFibers.push(waitedFiber, waitedFiberNs);
    }

    waitedFiber = nullptr;
    waitedFiberNs = 0;

    lib::TimeSpan now = lib::TimeSpan::now();

    Fiber *fib = nullptr;

    double priority = 0.0f;

    if (waitingFibers.try_pop(fib, priority))
    {
      if (priority <= now.nanoseconds())
      {
        fiber::Fiber::switchTo(fib);
      }
      else
      {
        waitingFibers.push(fib, priority);
      }
    }

    if (fib == nullptr && pendingFibers.dequeue(fib))
    {
      fiber::Fiber::switchTo(fib);
    }
  }

  delete workerFiber;
}

void JobSystem::yield()
{
  Fiber *f = Fiber::current();

  assert(yieldedFiber == nullptr);

  yieldedFiber = f;

  fiber::Fiber::switchTo(workerFiber);

  assert(fiber::Fiber::current() == f);
}

void JobSystem::sleepAndWakeOnPromiseResolve(PromiseHandler *data)
{
  Fiber *f = Fiber::current();

  forgetedFiber = f;
  promiseData = data;

  fiber::Fiber::switchTo(workerFiber);

  assert(fiber::Fiber::current() == f);
}

void JobSystem::wakeUpFiber(fiber::Fiber *f)
{
  pendingFibers.enqueue(f);
}

void JobSystem::delay(lib::TimeSpan span)
{
  lib::TimeSpan wakeAt = lib::TimeSpan::now() + span;

  waitedFiber = fiber::Fiber::current();
  waitedFiberNs = wakeAt.nanoseconds();

  fiber::Fiber::switchTo(workerFiber);
}
