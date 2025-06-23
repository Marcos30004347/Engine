#include "lib/datastructure/ConcurrentQueue.hpp"

#include "os/Thread.hpp"

#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::detail::ConcurrentQueueProducer<int> *queue = new lib::detail::ConcurrentQueueProducer<int>();

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
            queue->enqueue(j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / 1000);

          total_ns = 0;

          int x = -1;
          for (size_t j = 0; j < 1000; j++)
          {
            bool removed;

            for (size_t attempt = 0; attempt < totalThreads * 10000; attempt++)
            {
              then = lib::time::TimeSpan::now();
              removed = queue->tryDequeue(x);
              total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

              if (removed)
              {
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

  delete queue;
}

void concurrentListMultithreadTests()
{
  lib::ConcurrentQueue<int> queue;

  os::print("Inserting 0\n");
  queue.enqueue(0);
  os::print("Inserting 1\n");
  queue.enqueue(1);
  os::print("Inserting 2\n");
  queue.enqueue(2);

  int x;
  queue.tryDequeue(x);
  os::print("Popped %i\n", x);

  queue.tryDequeue(x);
  os::print("Popped %i\n", x);

  queue.tryDequeue(x);
  os::print("Popped %i\n", x);

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
            queue.enqueue(j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %fns\n", i, total_ns / 1000);

          total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();

            while (!queue.tryDequeue(x))
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
  return;
  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&]()
        {
          double total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            queue.enqueue(j);
          }
        });
  }

  for (size_t i = 0; i < totalThreads * 1000; i++)
  {
    int x;
    assert(queue.tryDequeue(x));
  }
}

int main()
{
  lib::memory::SystemMemoryManager::init();

  multiThreadTests();
  concurrentListMultithreadTests();

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}