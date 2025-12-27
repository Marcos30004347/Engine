#include "datastructure/ConcurrentSortedList.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"

#include <assert.h>
#include <atomic>

void concurrentSortedListMultithreadInsertTest()
{
  constexpr size_t NUM_INSERTS = 2000;

  lib::ConcurrentSortedList<int> *list = new lib::ConcurrentSortedList<int>();

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  std::atomic<bool> started(false);

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t] = os::Thread(
        [&, t]()
        {
          while (!started)
          {
          } // sync

          lib::time::TimeSpan then;
          double total_ns = 0;

          for (size_t i = 0; i < NUM_INSERTS; i++)
          {
            uint64_t key = (uint64_t)(t * NUM_INSERTS + i);

            then = lib::time::TimeSpan::now();
            bool ok = list->insert(key);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            assert(ok);
          }

          os::print("Thread %u average insertion = %f ns\n", t, total_ns / NUM_INSERTS);
        });
  }

  started = true;

  for (size_t t = 0; t < totalThreads; t++)
    threads[t].join();

  uint64_t expectedLength = NUM_INSERTS * totalThreads;

  assert(list->length() == expectedLength);

  os::print("Insertion test successful. Final list length = %llu\n", expectedLength);

  delete list;
}

void concurrentSortedListMultithreadRemoveTest()
{
  constexpr size_t NUM_ELEMENTS = 2000;

  lib::ConcurrentSortedList<int> *list = new lib::ConcurrentSortedList<int>();

  // Pre-insert all values
  for (size_t i = 0; i < NUM_ELEMENTS; i++)
    list->insert(i);

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  std::atomic<bool> started(false);

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t] = os::Thread(
        [&, t]()
        {
          while (!started)
          {
          }

          lib::time::TimeSpan then;
          double total_ns = 0;

          for (size_t i = t; i < NUM_ELEMENTS; i += totalThreads)
          {
            bool ok = false;

            then = lib::time::TimeSpan::now();
            ok = list->remove(i);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();

            assert(ok);
          }

          os::print("Thread %u average removal = %f ns\n", t, total_ns);
        });
  }

  started = true;

  for (size_t t = 0; t < totalThreads; t++)
    threads[t].join();

  assert(list->length() == 0);

  os::print("Removal test successful. Final list length = 0\n");

  delete list;
}

void concurrentSortedListMinTest()
{
  lib::ConcurrentSortedList<int> *list = new lib::ConcurrentSortedList<int>();

  list->insert(10);
  list->insert(5);
  list->insert(7);

  int x;
  bool ok = list->min(x);

  assert(ok);
  assert(x == 5);

  os::print("Min test successful. Value = %d\n", x);

  delete list;
}

void concurrentSortedListRandomRepeatedTest(size_t N)
{
  lib::ConcurrentSortedList<int> *list = new lib::ConcurrentSortedList<int>();

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];

  std::atomic<bool> started(false);

  for (size_t t = 0; t < totalThreads; t++)
  {
    threads[t] = os::Thread(
        [&, t]()
        {
          std::mt19937_64 rng((uint64_t)(os::Thread::getCurrentThreadId() * 1234567 + 987654321));

          double insertTotalNs = 0;
          double removeTotalNs = 0;

          while (!started)
          {
          } // sync start

          for (size_t i = 0; i < N; i++)
          {
            uint64_t key = rng() % 1'000'000'000;

            lib::time::TimeSpan then = lib::time::TimeSpan::now();
            bool okInsert = list->insert(key);

            insertTotalNs += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(okInsert);

            then = lib::time::TimeSpan::now();

            bool okRemove = list->remove(key);
            removeTotalNs += (lib::time::TimeSpan::now() - then).nanoseconds();

            assert(okRemove);
          }

          os::print("Thread %u average insert = %f ns, average remove = %f ns (N=%u)\n", t, insertTotalNs / N, removeTotalNs / N, (unsigned)N);
        });
  }

  started = true;

  for (size_t t = 0; t < totalThreads; t++)
    threads[t].join();

  assert(list->length() == 0);
  os::print("List is empty after all operations.\n");

  delete list;
}

int main()
{
  lib::memory::SystemMemoryManager::init();

  os::print("\n==== ConcurrentSortedList Insert Test ====\n");
  concurrentSortedListMultithreadInsertTest();

  os::print("\n==== ConcurrentSortedList Remove Test ====\n");
  concurrentSortedListMultithreadRemoveTest();

  os::print("\n==== ConcurrentSortedList Min Test ====\n");
  concurrentSortedListMinTest();

  os::print("\n==== ConcurrentSortedList Random Insert+Remove Test ====\n");
  concurrentSortedListRandomRepeatedTest(1024);

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}
