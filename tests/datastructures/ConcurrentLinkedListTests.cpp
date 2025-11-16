#include "datastructure/ConcurrentLinkedList.hpp"
#include "os/Thread.hpp"

#include "memory/SystemMemoryManager.hpp"
#include "time/TimeSpan.hpp"

#include "os/print.hpp"
#include <assert.h>

void multiThreadTests()
{
  lib::ConcurrentLinkedList<int> *list = new lib::ConcurrentLinkedList<int>();

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
            bool removed = false;

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
  lib::ConcurrentShardedList<int> list;

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

          os::print("Thread %u average removal time is %fns\n", i, total_ns / 1000);
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

void concurrentListIterationTests()
{
  os::print("Running concurrent iteration test...\n");

  const size_t totalThreads = os::Thread::getHardwareConcurrency();
  const size_t insertsPerThread = 2000;

  lib::ConcurrentLinkedList<int> list;

  std::atomic<bool> startInsert(false);
  std::atomic<size_t> finishedInsertCount(0);

  os::Thread threads[totalThreads];

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t] = os::Thread(
        [&, t]()
        {
          while (!startInsert.load())
          {
          }

          for (size_t i = 0; i < insertsPerThread; i++)
          {
            int value = (int)(t * insertsPerThread + i);
            list.insert(value);
          }

          finishedInsertCount.fetch_add(1);

          while (finishedInsertCount.load() != totalThreads)
          {
          }

          size_t localCount = 0;
          long long sum = 0;

          for (auto &v : list)
          {
            localCount++;
            sum += v;
          }

          os::print("Thread %u iteration saw count=%zu sum=%lld\n", os::Thread::getCurrentThreadId(), localCount, sum);
        });
  }

  startInsert = true;

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t].join();
  }

  os::print("Concurrent iteration test finished.\n");
}

void concurrentListIterationWithInPlaceRemovalTests()
{
  os::print("Running concurrent iteration with in-place removal test...\n");

  const size_t totalThreads = os::Thread::getHardwareConcurrency();
  const size_t insertsPerThread = 4000;
  const size_t totalInserts = totalThreads * insertsPerThread;

  lib::ConcurrentLinkedList<int> list;

  // -----------------------
  // Insert phase
  // -----------------------
  std::atomic<bool> startInsert(false);
  std::atomic<size_t> finishedInsertCount(0);

  os::Thread insertThreads[totalThreads];

  for (size_t t = 0; t < totalThreads; t++)
  {
    insertThreads[t] = os::Thread(
        [&, t]()
        {
          while (!startInsert.load())
          {
          }

          for (size_t i = 0; i < insertsPerThread; i++)
          {
            int v = (int)(t * insertsPerThread + i);
            list.insert(v);
          }

          finishedInsertCount.fetch_add(1);
        });
  }

  startInsert = true;

  for (size_t t = 0; t < totalThreads; t++)
    insertThreads[t].join();

  os::print("Inserted %zu elements\n", totalInserts);

  // -----------------------
  // Iterate + Random Remove
  // -----------------------
  std::atomic<bool> startIter(false);
  std::atomic<size_t> finishedIter(0);
  std::atomic<size_t> totalRemovals(0);

  os::Thread iterThreads[totalThreads];

  for (size_t t = 0; t < totalThreads; t++)
  {
    iterThreads[t] = os::Thread(
        [&, t]()
        {
          std::mt19937_64 rng((uint64_t(os::Thread::getCurrentThreadId()) << 32) ^ t);
          std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

          while (!startIter.load())
          {
          }

          size_t localCount = 0;
          size_t localRemoved = 0;

          for (auto &e : list)
          {
            localCount++;

            // 20% chance to try to remove this element
            if ((dist(rng) & 0xFFFF) < 0x3333)
            {
              int x = e; // copy the value
              if (list.tryRemove(x))
              {
                localRemoved++;
              }
            }
          }

          totalRemovals.fetch_add(localRemoved);

          os::print("Thread %u iterated=%zu removed=%zu\n",
                    os::Thread::getCurrentThreadId(),
                    localCount, localRemoved);

          finishedIter.fetch_add(1);
        });
  }

  startIter = true;

  for (size_t t = 0; t < totalThreads; t++)
    iterThreads[t].join();

  os::print("Total removals = %zu\n", totalRemovals.load());

  size_t remain = 0;
  for (auto &v : list)
    remain++;

  os::print("Remaining elements after concurrent iteration+remove: %zu\n", remain);

  os::print("Test complete.\n\n");
}

int main()
{
  lib::time::TimeSpan then = lib::time::TimeSpan::now();
  lib::memory::SystemMemoryManager::init();

  concurrentListMultithreadTests();
  concurrentListIterationTests();
  concurrentListIterationWithInPlaceRemovalTests();

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}