#include "lib/datastructure/ConcurrentPriorityQueue.hpp"
#include "os/Thread.hpp"

#include "lib/algorithm/random.hpp"
#include "lib/memory/SystemMemoryManager.hpp"
#include "lib/time/TimeSpan.hpp"
#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::ConcurrentPriorityQueue<int, size_t> *pq = new lib::ConcurrentPriorityQueue<int, size_t>();

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  std::atomic<size_t> started(0);
  std::atomic<size_t> dequeing(0);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [i, &started, &dequeing, &pq, totalThreads]()
        {
          lib::memory::SystemMemoryManager::initializeThread();
          started.fetch_add(1);
          while (started.load() < totalThreads)
          {
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();
          double total_insert_ns = 0;
          double total_get_ns = 0;

          for (size_t j = 0; j < 100; j++)
          {
            // os::print("Thread %i enqueuing %i\n", i, (i + 1) * 1000 + j);
            then = lib::time::TimeSpan::now();
            assert(pq->enqueue((i + 1) * 1000 + j, (i + 1) * 1000 + j));
            total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          dequeing.fetch_add(1);
          // pq->printTree(i);

          // pq->printTree(i);

          while (dequeing.load() < totalThreads)
          {
          }

          // os::print("Thread %i dequeing\n", i);
          int x;
          size_t p;
          int prev = -1;

          for (size_t j = 0; j < 100; j++)
          {
            then = lib::time::TimeSpan::now();
            while (!pq->tryDequeue(x, p))
            {
            }

            if (x <= prev)
            {
              os::print("Thread %u dequeued %i, prev = %i, at iter %i\n", i, x, prev, j);
              assert(false);
            }

            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            prev = x;
          }

          os::print("Thread %u average insertion time is %lluns\n", i, (size_t)total_insert_ns / 100);
          os::print("Thread %u average get time is %lluns\n", i, (size_t)total_get_ns / 100);
          lib::memory::SystemMemoryManager::finializeThread();
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }
  delete pq;
}
void multiThreadGCTests()
{
  lib::memory::allocator::SystemAllocator<int> allocator;
  lib::ConcurrentTimestampGarbageCollector<int, lib::memory::allocator::SystemAllocator<int>> gc(allocator);

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  int *values[1000];
  std::atomic<bool> wasFreed[1000];

  for (size_t i = 0; i < 1000; i++)
  {
    values[i] = allocator.allocate(1);
    wasFreed[i].store(false);
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [i, &gc, totalThreads, &values, &allocator, &wasFreed]()
        {
          double total_insert_ns = 0;
          double total_get_ns = 0;
          double total_collect_ns = 0;

          lib::time::TimeSpan then = lib::time::TimeSpan::now();

          for (size_t j = 0; j < 1000; j++)
          {
            // os::print("Thread %u oppening context\n", os::Thread::getCurrentThreadId());

            then = lib::time::TimeSpan::now();
            gc.openThreadContext();
            total_insert_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            int *v = values[j];

            if (!wasFreed[j].load())
            {
              int x = 0;

              // os::print("Thread %u using j = %u, %p\n", os::Thread::getCurrentThreadId(), j, v);

              size_t rand = lib::random(os::Thread::getCurrentThreadId() + j);

              for (size_t k = 0; k < rand % 1000; k++)
              {
                x += *v;
              }

              bool expected = false;

              if (wasFreed[j].compare_exchange_strong(expected, true))
              {
                // os::print("Thread %u collecting j = %u\n", os::Thread::getCurrentThreadId(), j);

                int **buff = new int *[1];
                buff[0] = v;
                gc.free(buff, 1);
              }
            }

            then = lib::time::TimeSpan::now();
            gc.collect();
            total_collect_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            
            then = lib::time::TimeSpan::now();
            gc.closeThreadContext();
            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %lluns\n", os::Thread::getCurrentThreadId(), (size_t)total_insert_ns / 1000);
          os::print("Thread %u average get time is %lluns\n", os::Thread::getCurrentThreadId(), (size_t)total_get_ns / 1000);
          os::print("Thread %u average collect time is %lluns\n", os::Thread::getCurrentThreadId(), (size_t)total_collect_ns / 1000);
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }
}
int main()
{
  lib::memory::SystemMemoryManager::init();

  multiThreadGCTests();

  for (size_t i = 0; i < 100; i++)
  {
    multiThreadTests();
  }

  lib::memory::SystemMemoryManager::shutdown();
}