#include "datastructure/ConcurrentSkipListMap.hpp"
#include "memory/SystemMemoryManager.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"
#include <algorithm>
#include <assert.h>
#include <cstdlib>
#include <ctime>
#include <vector>

void basicTests()
{
  os::print("Running basic tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  // Test insert
  assert(map.insert(10, 100));
  assert(map.insert(20, 200));
  assert(map.insert(30, 300));
  assert(map.getSize() == 3);

  // Test duplicate insert
  assert(!map.insert(10, 150));
  assert(map.getSize() == 3);

  // Test find
  int value;
  assert(map.find(10, value) && value == 100);
  assert(map.find(20, value) && value == 200);
  assert(map.find(30, value) && value == 300);
  assert(!map.find(40, value));

  // Test remove
  assert(map.remove(20));
  assert(map.getSize() == 2);
  assert(!map.find(20, value));
  assert(!map.remove(20)); // Already removed

  // Test remaining elements
  assert(map.find(10, value) && value == 100);
  assert(map.find(30, value) && value == 300);

  os::print("Basic tests passed!\n");
}

void iteratorTests()
{
  os::print("Running iterator tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  // Insert elements
  for (int i = 0; i < 10; i++)
  {
    map.insert(i * 10, i * 100);
  }

  // Test iteration
  int count = 0;
  int lastKey = -1;

  for (auto e : map)
  {
    // printf("%u %u\n",e.key, e.value);
    assert(e.first > lastKey);
    assert(e.second == e.first * 10);
    lastKey = e.first;
    count++;
  }
  assert(count == 10);

  // Test iteration with modifications
  map.remove(30);
  map.remove(70);

  count = 0;
  
  for (auto e : map)
  {
    count++;
  }

  assert(count == 8);

  os::print("Iterator tests passed!\n");
}

void multiThreadInsertTests()
{
  os::print("Running multi-threaded insert tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];
  std::atomic<bool> started(false);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
            // Wait for all threads to be ready
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();
          double total_ns = 0;

          // Each thread inserts 1000 unique keys
          int base = i * 1000;

          for (int j = 0; j < 1000; j++)
          {
            then = lib::time::TimeSpan::now();
            bool inserted = map.insert(base + j, (base + j) * 10);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(inserted);
          }

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / 1000);
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  // Verify all elements are present
  assert(map.getSize() == totalThreads * 1000);

  int value;
  for (size_t i = 0; i < totalThreads; i++)
  {
    int base = i * 1000;
    for (int j = 0; j < 1000; j++)
    {
      assert(map.find(base + j, value));
      assert(value == (base + j) * 10);
    }
  }

  os::print("Multi-threaded insert tests passed!\n");
}

void multiThreadRemoveTests()
{
  os::print("Running multi-threaded remove tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  size_t elementsPerThread = 1000;

  // Pre-populate the map
  for (size_t i = 0; i < totalThreads * elementsPerThread; i++)
  {
    map.insert(i, i);
  }

  os::Thread threads[totalThreads];
  std::atomic<bool> started(false);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
            // Wait
          }

          lib::time::TimeSpan then = lib::time::TimeSpan::now();
          double total_ns = 0;

          // Each thread removes its own range
          int base = i * elementsPerThread;
          for (size_t j = 0; j < elementsPerThread; j++)
          {
            then = lib::time::TimeSpan::now();
            bool removed = map.remove(base + j);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(removed);
          }

          os::print("Thread %u average removal time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / elementsPerThread);
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  // Verify all elements are removed
  assert(map.isEmpty());

  os::print("Multi-threaded remove tests passed!\n");
}

void mixedOperationsTests()
{
  os::print("Running mixed operations tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];
  std::atomic<bool> started(false);
  std::atomic<size_t> totalInserts(0);
  std::atomic<size_t> totalRemoves(0);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
            // Wait
          }

          unsigned int seed = time(nullptr) + i;
          size_t inserts = 0;
          size_t removes = 0;

          for (int j = 0; j < 1000; j++)
          {
            int key = (rand_r(&seed) % 5000) + (i * 5000);
            int op = rand_r(&seed) % 3;

            if (op == 0)
            {
              // Insert
              if (map.insert(key, key * 10))
              {
                inserts++;
              }
            }
            else if (op == 1)
            {
              // Remove
              if (map.remove(key))
              {
                removes++;
              }
            }
            else
            {
              // Find
              int value;
              map.find(key, value);
            }
          }

          totalInserts.fetch_add(inserts);
          totalRemoves.fetch_add(removes);

          os::print("Thread %u: inserts=%zu, removes=%zu\n", os::Thread::getCurrentThreadId(), inserts, removes);
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  size_t expectedSize = totalInserts.load() - totalRemoves.load();
  size_t actualSize = map.getSize();

  os::print("Total inserts: %zu, Total removes: %zu\n", totalInserts.load(), totalRemoves.load());
  os::print("Expected size: %zu, Actual size: %zu\n", expectedSize, actualSize);

  assert(actualSize == expectedSize);

  os::print("Mixed operations tests passed!\n");
}

void concurrentIterationTests()
{
  os::print("Running concurrent iteration tests...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  // Pre-populate
  for (int i = 0; i < 1000; i++)
  {
    map.insert(i, i * 10);
  }

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];
  std::atomic<bool> started(false);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
            // Wait
          }

          if (i % 2 == 0)
          {
            // Iterator threads
            for (int iter = 0; iter < 100; iter++)
            {
              int count = 0;
              for (auto e : map)
              {
                count++;
              }
            }
          }
          else
          {
            // Modifier threads
            unsigned int seed = time(nullptr) + i;
            for (int j = 0; j < 500; j++)
            {
              int key = rand_r(&seed) % 1000;

              if (rand_r(&seed) % 2 == 0)
              {
                map.insert(key + 1000, key * 10);
              }
              else
              {
                map.remove(key);
              }
            }
          }
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  os::print("Concurrent iteration tests passed!\n");
}

void randomIteratorModificationTests()
{
  os::print("Running random iterator modification tests...\n");

  for (int test = 0; test < 10; test++)
  {
    lib::ConcurrentSkipListMap<int, int> map;

    // Pre-populate with 100 elements
    for (int i = 0; i < 100; i++)
    {
      map.insert(i, i * 10);
    }

    size_t totalThreads = os::Thread::getHardwareConcurrency();
    os::Thread threads[totalThreads];
    std::atomic<bool> started(false);

    for (size_t i = 0; i < totalThreads; i++)
    {
      threads[i] = os::Thread(
          [&, i]()
          {
            while (!started.load())
            {
              // Wait
            }

            unsigned int seed = time(nullptr) + i + test;

            // Iterate and randomly remove elements
            for (int iter = 0; iter < 50; iter++)
            {
              std::vector<int> keysToRemove;

              for (auto e : map)
              {
                // Randomly decide to remove this element
                if (rand_r(&seed) % 10 == 0)
                {
                  keysToRemove.push_back(e.first);
                }
              }

              // Remove the selected keys
              for (int key : keysToRemove)
              {
                map.remove(key);
              }

              // Insert some new random elements
              for (int j = 0; j < 5; j++)
              {
                int newKey = (rand_r(&seed) % 1000) + (i * 1000);
                map.insert(newKey, newKey * 10);
              }
            }
          });
    }

    started.store(true);

    for (size_t i = 0; i < totalThreads; i++)
    {
      threads[i].join();
    }
  }

  os::print("Random iterator modification tests passed!\n");
}

void stressTest()
{
  os::print("Running stress test...\n");

  lib::ConcurrentSkipListMap<int, int> map;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  os::Thread threads[totalThreads];
  std::atomic<bool> started(false);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
            // Wait
          }

          unsigned int seed = time(nullptr) + i;

          for (int j = 0; j < 10000; j++)
          {
            int key = rand_r(&seed) % 1000;
            int op = rand_r(&seed) % 10;

            if (op < 4)
            {
              // 40% insert
              map.insert(key, key * 10);
            }
            else if (op < 7)
            {
              // 30% remove
              map.remove(key);
            }
            else if (op < 9)
            {
              // 20% find
              int value;
              map.find(key, value);
            }
            else
            {
              // 10% iterate
              int count = 0;
              for (auto e : map)
              {
                count++;
                if (count > 100)
                  break; 
              }
            }
          }
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  os::print("Stress test passed! Final size: %zu\n", map.getSize());
}

int main()
{
  lib::memory::SystemMemoryManager::init();

  srand(time(nullptr));

  basicTests();
  iteratorTests();
  multiThreadInsertTests();
  multiThreadRemoveTests();
  mixedOperationsTests();
  concurrentIterationTests();
  randomIteratorModificationTests();

  // Run stress test multiple times
  for (int i = 0; i < 10; i++)
  {
    os::print("\n=== Stress test iteration %d ===\n", i + 1);
    stressTest();
  }

  os::print("\n=== All tests passed! ===\n");

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}