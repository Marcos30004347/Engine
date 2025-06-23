#include "lib/datastructure/ConcurrentLinkedList.hpp"
#include "os/Thread.hpp"

#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

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

  lib::detail::ConcurrentLinkedList<int> *list = new lib::detail::ConcurrentLinkedList<int>();

  then = lib::time::TimeSpan::now();
  list->insert(0);
  os::print("Inserting 0 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  list->insert(1);
  os::print("Inserting 1 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  list->insert(2);
  os::print("Inserting 2 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  list->tryRemove(2);
  os::print("Removing 2 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  list->tryRemove(0);
  os::print("Removing 0 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  then = lib::time::TimeSpan::now();
  list->tryRemove(1);
  os::print("Removing 1 in %fns\n", (lib::time::TimeSpan::now() - then).nanoseconds());

  list->insert(1);
  list->insert(3);
  list->insert(4);
  list->insert(6);

  int value = 0;
  size_t iters = 0;

  printf("\n");
  while (list->tryPop(value))
  {
    os::print("Removing %i...\n", value);
    iters += 1;
  }

  assert(iters == 4);

  delete list;

  // multiThreadTests();
  concurrentListMultithreadTests();

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}