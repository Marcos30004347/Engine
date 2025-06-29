#include "lib/datastructure/ConcurrentPriorityQueue.hpp"
#include "os/Thread.hpp"

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

          for (size_t j = 0; j < 1000; j++)
          {
            //os::print("Thread %i enqueuing %i\n", i, (i + 1) * 1000 + j);
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

          os::print("Thread %i dequeing\n", i);
          int x;
          int prev = -1;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            assert(pq->tryDequeue(x));

            if (x <= prev)
            {
              os::print("Thread %u dequeued %i, prev = %i, at iter %i\n", i, x, prev, j);
              assert(false);
            }

            total_get_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            prev = x;
          }

          os::print("Thread %u average insertion time is %fns\n", i, total_insert_ns / 1000);
          os::print("Thread %u average get time is %fns\n", i, total_get_ns / 1000);
          lib::memory::SystemMemoryManager::finializeThread();
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
  // lib::ConcurrentPriorityQueue<int, size_t> pq;
  // lib::time::TimeSpan then = lib::time::TimeSpan::now();
  // double total_ns = 0;

  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(1, 1));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(1, 1));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(2, 11));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
  // /*
  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(3, 1));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
  // */
  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(4, 2));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(5, 3));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

  // then = lib::time::TimeSpan::now();
  // assert(pq.enqueue(6, 10));
  // total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

  // os::print("Thread %u average insertion time is %fns\n", 0, total_ns / 5);

  // int x;
  // size_t p;

  // for (size_t i = 0; i < 6; i++)
  // {
  //   assert(pq.tryPeek(p));
  //   assert(pq.tryDequeue(x));
  //   os::print("%i %llu\n", x, p);
  // }
  for (size_t i = 0; i < 100000; i++)
  {
    multiThreadTests();
  }
  lib::memory::SystemMemoryManager::shutdown();
  /*
  assert(pq.tryDequeue(x));
  os::print("%i\n", x);
  */
}