#include "datastructure/ConcurrentLinkedList.hpp"
#include "os/Thread.hpp"

#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::detail::ConcurrentLinkedList<int> *list = new lib::detail::ConcurrentLinkedList<int>();

  bool started = false;

  std::atomic<size_t> insertedFinished(0);

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          while (!started)
          {
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();
          double total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            list->insert(j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / 1000);

          insertedFinished.fetch_add(1);
          while (insertedFinished.load() != totalThreads)
          {
          }

          total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            bool removed;

            for (size_t attempt = 0; attempt < totalThreads * 10000; attempt++)
            {
              then = lib::time::TimeSpan::now();

              int x = j;
              removed = list->tryRemove(x);

              total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

              if (removed)
              {
                assert(x == j);
                break;
              }
            }
            assert(removed);
          }

          os::print("Thread %u average removal time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / 1000);
        });
  }
  started = true;
  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  delete list;
}

void concurrentListMultithreadTests()
{
  lib::ConcurrentList<int> list;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          lib::time::TimeSpan then = lib::time::TimeSpan::now();

          double total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            list.insert(j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %fns\n", i, total_ns / 1000);

          total_ns = 0;
          int x;
          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();

            while (!list.tryPop(x))
            {
            }

            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average removal time is %fns\n", i);
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          for (size_t j = 0; j < 1000; j++)
          {
            list.insert(j);
          }
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  for (size_t i = 0; i < totalThreads * 1000; i++)
  {
    int x;
    assert(list.tryPop(x));
  }
}

int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();

  // multiThreadTests();
  concurrentListMultithreadTests();

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}