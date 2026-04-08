#include "datastructure/ConcurrentPriorityQueue.hpp"
#include "os/Thread.hpp"

#include "algorithm/random.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"
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

          for (size_t j = 0; j < 1000; j++)
          {
            // os::print("Thread %i enqueuing %i\n", i, (i + 1) * 1000 + j);
            then = lib::time::TimeSpan::now();
            bool enqueued = pq->enqueue((i + 1) * 1000 + j, (i + 1) * 1000 + j);
            
            assert(enqueued);

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

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            while (!pq->dequeue(x))
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

          os::print("Thread %u average insertion time is %lluns\n", i, (size_t)total_insert_ns / 1000);
          os::print("Thread %u average get time is %lluns\n", i, (size_t)total_get_ns / 1000);
          lib::memory::SystemMemoryManager::finializeThread();
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }
  delete pq;
}

int main()
{
  lib::memory::SystemMemoryManager::init();

  for (size_t i = 0; i < 10; i++)
  {
    multiThreadTests();
  }

  lib::memory::SystemMemoryManager::shutdown();
}