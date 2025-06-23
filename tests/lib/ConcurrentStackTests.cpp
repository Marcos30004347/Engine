#include "lib/datastructure/ConcurrentStack.hpp"
#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::ConcurrentStack<int> stack = lib::ConcurrentStack<int>();

  std::atomic<int> can_push(0);
  std::atomic<int> can_pop(0);

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          lib::memory::SystemMemoryManager::initializeThread();

          can_push.fetch_add(1);
          while (can_push.load() != totalThreads)
          {
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();

          double total_insert_ns = 0;
          double total_get_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            stack.push(j);
            total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          can_pop.fetch_add(1);
          while (can_pop.load() != totalThreads)
          {
          }

          for (size_t j = 0; j < 1000; j++)
          {
            int x;
            then = lib::time::TimeSpan::now();
            assert(stack.tryPop(x));
            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::threadSafePrintf("Thread %u average push time is %fns\n", os::Thread::getCurrentThreadId(), total_insert_ns / 1000);
          os::threadSafePrintf("Thread %u average pop time is %fns\n", os::Thread::getCurrentThreadId(), total_get_ns / 1000);

          lib::memory::SystemMemoryManager::finializeThread();
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    if (threads[i].isRunning())
    {
      threads[i].join();
    }
  }
}

int main()
{
  lib::memory::SystemMemoryManager::init();
  lib::memory::SystemMemoryManager::initializeThread();

  multiThreadTests();
  
  lib::memory::SystemMemoryManager::finializeThread();
  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}