#include "async/ThreadCache.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{

  bool started = false;

  size_t totalThreads = 128; // os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];
  async::ThreadCache<size_t> *storage = new async::ThreadCache<size_t>(totalThreads * 2);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          while (!started)
          {
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();

          double total_insert_ns = 0;
          double total_get_ns = 0;

          then = lib::time::TimeSpan::now();

          bool inserted = storage->set(os::Thread::getCurrentThreadId(), os::Thread::getCurrentThreadId());

          if (!inserted)
          {
            os::print(" Thread %u failed\n", os::Thread::getCurrentThreadId());
          }

          total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          then = lib::time::TimeSpan::now();

          size_t *x = storage->get(os::Thread::getCurrentThreadId());

          if (x == nullptr)
          {
            os::print(" Thread %u failed\n", os::Thread::getCurrentThreadId());
          }

          total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          assert(*x == os::Thread::getCurrentThreadId());

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_insert_ns / 1000);
          os::print("Thread %u average get time is %fns\n", os::Thread::getCurrentThreadId(), total_get_ns / 1000);
        });
  }

  started = true;

  for (size_t i = 0; i < totalThreads; i++)
  {
    if (threads[i].isRunning())
    {
      threads[i].join();
    }
  }

  delete storage;
}
int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();
  for (size_t i = 0; i < 1000; i++)
  {
    multiThreadTests();
  }
  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}