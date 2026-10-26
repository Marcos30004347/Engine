#include "datastructure/ConcurrentQueue.hpp"

#include "os/Thread.hpp"

#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void singleThreadTimingAndOrderTest()
{
  os::print("Running single-thread FIFO order + timing test...\n");

  lib::ConcurrentQueue<int> queue;

  constexpr size_t N = 100000;

  lib::time::TimeSpan then;
  double enqueue_total_ns = 0.0;
  double dequeue_total_ns = 0.0;

  for (size_t i = 0; i < N; ++i)
  {
    then = lib::time::TimeSpan::now();
    queue.enqueue(static_cast<int>(i));
    enqueue_total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
  }

  os::print("Single-thread average enqueue time: %f ns\n", enqueue_total_ns / N);

  for (size_t expected = 0; expected < N; ++expected)
  {
    int value = -1;

    then = lib::time::TimeSpan::now();
    bool ok = queue.dequeue(value);
    dequeue_total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

    assert(ok);
    assert(value == static_cast<int>(expected));
  }

  os::print("Single-thread average dequeue time: %f ns\n", dequeue_total_ns / N);

  int dummy;
  bool empty = queue.dequeue(dummy);
  assert(!empty);

  os::print("Single-thread FIFO order + timing test passed.\n");
}

void multiThreadTests()
{
  lib::ConcurrentQueue<int> *queue = new lib::ConcurrentQueue<int>();

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
              removed = queue->dequeue(x);
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
  lib::ConcurrentShardedQueue<int> *queue = new lib::ConcurrentShardedQueue<int>();

  queue->enqueue(0);
  queue->enqueue(1);
  queue->enqueue(2);

  int x;
  queue->dequeue(x);
  queue->dequeue(x);
  queue->dequeue(x);

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
            queue->enqueue(j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average insertion time is %fns\n", i, total_ns / 1000);

          total_ns = 0;

          for (size_t j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();

            while (!queue->dequeue(x))
            {
            }

            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
          }

          os::print("Thread %u average removal time is %fns\n", i, total_ns / 1000);
        });
  }

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }
 
  delete queue;
}

int main()
{
  lib::memory::SystemMemoryManager::init();

  singleThreadTimingAndOrderTest();
  printf(" Multi thread tests\n");
  multiThreadTests();

  for (uint32_t i = 0; i < 10; i++)
  {
    printf(" concurrentListMultithreadTests\n");

    concurrentListMultithreadTests();
  }

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}