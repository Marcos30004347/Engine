#include "jobsystem.hpp"
#include <assert.h>
#include <iostream>

using namespace jobsystem;
using namespace jobsystem::fiber;

// thread_local fiber::Fiber *JobSystem::currentFiber = nullptr;
static thread_local fiber::Fiber *workerFiber = nullptr;
static thread_local fiber::Fiber *yieldedFiber = nullptr;

std::vector<std::thread> JobSystem::workers_;
lib::parallel::LinearQueue<fiber::Fiber *, 4096> JobSystem::tasks_ = lib::parallel::LinearQueue<fiber::Fiber *, 4096>();
std::atomic<bool> JobSystem::running_ = false;

void JobSystem::init(size_t numThreads, void (*entry)())
{
  running_ = true;
  enqueue(entry);

  for (size_t i = 0; i < numThreads; ++i)
  {
    workers_.emplace_back(workerLoop);
  }

  workerLoop();
}

void JobSystem::stop()
{
  running_.store(false);
}

void JobSystem::shutdown()
{
  assert(tasks_.size() == 0);

  for (auto &w : workers_)
  {
    w.join();
  }

  workers_.clear();
}

void JobSystem::workerLoop()
{
  workerFiber = fiber::Fiber::currentThreadToFiber();

  while (JobSystem::running_)
  {
    while (yieldedFiber != nullptr && !tasks_.push(yieldedFiber))
    {
    }

    yieldedFiber = nullptr;
    Fiber *fib = nullptr;

    assert(fiber::Fiber::current() == workerFiber);

    if (tasks_.pop(fib))
    {
      fiber::Fiber::switchTo(fib);
    }
  }

  workerFiber = nullptr;
}

void JobSystem::yield()
{

  Fiber *f = Fiber::current();

  assert(yieldedFiber == nullptr);

  yieldedFiber = f;

  fiber::Fiber::switchTo(workerFiber);

  assert(fiber::Fiber::current() == f);
}

