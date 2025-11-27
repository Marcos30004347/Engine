#include "datastructure/ConcurrentMap.hpp"
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

  lib::ConcurrentMap<int, int> map;
  // Test insert
  assert(map.insert(10, 100) != map.end());
  assert(map.insert(20, 200) != map.end());
  assert(map.insert(30, 300) != map.end());
  // assert(map.size() == 3);
  // assert(map.insert(10, 150) == map.end());

  // assert(map.find(10) != map.end() && map.find(10).value() == 100);
  // assert(map.find(20) != map.end() && map.find(20).value() == 200);
  // assert(map.find(30) != map.end() && map.find(30).value() == 300);
  // assert(map.find(40) == map.end());

  // assert(map[10] == 100);

  // map[5] = 5;

  // assert(map[5] == 5);

  // // Test remove
  assert(map.remove(20));
  assert(map.find(20) == map.end());
  //  assert(!map.remove(20));

  // assert(map.remove(30));
  // assert(map.find(30) == map.end());
  // assert(!map.remove(30));

  os::print("Basic tests passed!\n");

  // map.verifyReferenceCounts();
}

void iteratorTests()
{
  os::print("Running iterator tests...\n");

  lib::ConcurrentMap<int, int> map;

  // Insert elements
  for (int i = 1; i <= 1000; i++)
  {
    map.insert(i * 10, i * 100);
  }

  // Test iteration
  int count = 0;
  int lastKey = -1;

  for (auto e : map)
  {
    assert(e.first > lastKey);
    assert(e.second == e.first * 10);
    lastKey = e.first;
    count++;
  }

  assert(count == 1000);

  // // Test iteration with modifications
  // map.remove(30);
  // map.remove(70);

  for (int i = 1; i <= 1000; i++)
  {
    map.remove(i * 10);
  }

  // count = 0;

  // for (auto e : map)
  // {
  //   count++;
  // }

  // assert(count == 1000 - 2);

  os::print("Iterator tests passed!\n");
}

void multiThreadInsertTests()
{
  os::print("Running multi-threaded insert tests...\n");

  lib::ConcurrentMap<int, int> map;

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

          for (int j = 0; j < 10; j++)
          {
            then = lib::time::TimeSpan::now();
            auto iter = map.insert(base + j, (base + j) * 10);
            total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
            assert(iter != map.end());
          }

          os::print("Thread %u average insertion time is %fns\n", os::Thread::getCurrentThreadId(), total_ns / 1000);
        });
  }

  started.store(true);

  for (size_t i = 0; i < totalThreads; i++)
  {
    threads[i].join();
  }

  // map.verifyReferenceCounts();

  // Verify all elements are present
  // assert(map.size() == totalThreads * 1000);

  // int value;
  // for (size_t i = 0; i < totalThreads; i++)
  // {
  //   int base = i * 1000;
  //   for (int j = 0; j < 1000; j++)
  //   {
  //     assert(map.find(base + j, value));
  //     assert(value == (base + j) * 10);
  //   }
  // }

  // map.verifyReferenceCounts();

  os::print("Multi-threaded insert tests passed!\n");
}

void multiThreadRemoveTests()
{
  // os::print("Running multi-threaded remove tests...\n");

  lib::ConcurrentMap<int, int> map;

  size_t totalThreads = os::Thread::getHardwareConcurrency();
  size_t elementsPerThread = 10;

  // Pre-populate the map
  for (size_t i = 0; i < totalThreads * elementsPerThread; i++)
  {
    // printf("inserting %u\n", i);
    auto iter = map.insert(i, i);
    assert(iter != map.end());
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

          double total_ns = 0;

          // Each thread removes its own range
          int base = i * elementsPerThread;
          for (size_t j = 0; j < elementsPerThread; j++)
          {
            auto then = lib::time::TimeSpan::now();
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
}

void mixedOperationsTests()
{
  os::print("Running mixed operations tests...\n");

  lib::ConcurrentMap<int, int> map;

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
              if (map.insert(key, key * 10) != map.end())
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
              auto iter = map.find(key);
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
  size_t actualSize = map.size();

  os::print("Total inserts: %zu, Total removes: %zu\n", totalInserts.load(), totalRemoves.load());
  os::print("Expected size: %zu, Actual size: %zu\n", expectedSize, actualSize);

  assert(actualSize == expectedSize);

  os::print("Mixed operations tests passed!\n");
}

void concurrentIterationTests()
{
  os::print("Running concurrent iteration tests...\n");

  lib::ConcurrentMap<int, int> map;

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
    lib::ConcurrentMap<int, int> map;

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

  lib::ConcurrentMap<int, int> map;

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
              auto iter = map.find(key);
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

  os::print("Stress test passed! Final size: %zu\n", map.size());
}

void benchmarkInsertST(int numOperations)
{
  lib::ConcurrentMap<int, int> map;

  // Sequential insert
  lib::time::TimeSpan start = lib::time::TimeSpan::now();
  for (int i = 0; i < numOperations; i++)
  {
    map.insert(i, i * 10);
  }
  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  double avgNs = elapsed.nanoseconds() / numOperations;
  double opsPerSec = numOperations / elapsed.seconds();

  os::print("  Sequential: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numOperations, elapsed.milliseconds(), avgNs, opsPerSec);

  assert(map.size() == numOperations);

  // Clear
  for (int i = 0; i < numOperations; i++)
  {
    map.remove(i);
  }

  // Random insert
  unsigned int seed = time(nullptr);
  std::vector<int> randomKeys(numOperations);
  for (int i = 0; i < numOperations; i++)
  {
    randomKeys[i] = rand_r(&seed);
  }

  start = lib::time::TimeSpan::now();
  for (int i = 0; i < numOperations; i++)
  {
    map.insert(randomKeys[i], randomKeys[i] * 10);
  }
  elapsed = lib::time::TimeSpan::now() - start;

  avgNs = elapsed.nanoseconds() / numOperations;
  opsPerSec = numOperations / elapsed.seconds();

  os::print("  Random:     %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numOperations, elapsed.milliseconds(), avgNs, opsPerSec);
}

void benchmarkAtST(int numElements)
{
  lib::ConcurrentMap<int, int> map;

  // Pre-populate
  for (int i = 0; i < numElements; i++)
  {
    map.insert(i, i * 10);
  }

  // Sequential lookup (100% hit)
  lib::time::TimeSpan start = lib::time::TimeSpan::now();
  int hitCount = 0;
  for (int i = 0; i < numElements; i++)
  {
    auto iter = map.find(i);
    if (iter != map.end())
      hitCount++;
  }
  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  double avgNs = elapsed.nanoseconds() / numElements;
  double opsPerSec = numElements / elapsed.seconds();

  os::print("  Seq(100%%):  %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numElements, elapsed.milliseconds(), avgNs, opsPerSec);

  // Random lookup (100% hit)
  unsigned int seed = time(nullptr);
  std::vector<int> randomKeys(numElements);
  for (int i = 0; i < numElements; i++)
  {
    randomKeys[i] = rand_r(&seed) % numElements;
  }

  start = lib::time::TimeSpan::now();
  hitCount = 0;
  for (int i = 0; i < numElements; i++)
  {
    auto iter = map.find(randomKeys[i]);
    if (iter != map.end())
      hitCount++;
  }
  elapsed = lib::time::TimeSpan::now() - start;

  avgNs = elapsed.nanoseconds() / numElements;
  opsPerSec = numElements / elapsed.seconds();

  os::print("  Rnd(100%%):  %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numElements, elapsed.milliseconds(), avgNs, opsPerSec);

  // Random lookup (50% hit)
  for (int i = 0; i < numElements; i++)
  {
    randomKeys[i] = rand_r(&seed) % (numElements * 2);
  }

  start = lib::time::TimeSpan::now();
  hitCount = 0;
  for (int i = 0; i < numElements; i++)
  {
    auto iter = map.find(randomKeys[i]);
    if (iter != map.end())
      hitCount++;
  }
  elapsed = lib::time::TimeSpan::now() - start;

  avgNs = elapsed.nanoseconds() / numElements;
  opsPerSec = numElements / elapsed.seconds();

  os::print("  Rnd(~50%%):  %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec | Hits:%d\n", numElements, elapsed.milliseconds(), avgNs, opsPerSec, hitCount);
}

void benchmarkRemoveST(int numOperations)
{
  // Sequential remove
  lib::ConcurrentMap<int, int> map;
  for (int i = 0; i < numOperations; i++)
  {
    map.insert(i, i * 10);
  }

  lib::time::TimeSpan start = lib::time::TimeSpan::now();
  for (int i = 0; i < numOperations; i++)
  {
    map.remove(i);
  }
  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  double avgNs = elapsed.nanoseconds() / numOperations;
  double opsPerSec = numOperations / elapsed.seconds();

  os::print("  Sequential: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numOperations, elapsed.milliseconds(), avgNs, opsPerSec);
  assert(map.isEmpty());

  // Random remove
  unsigned int seed = time(nullptr);
  std::vector<int> randomKeys(numOperations);
  for (int i = 0; i < numOperations; i++)
  {
    randomKeys[i] = rand_r(&seed);
    map.insert(randomKeys[i], randomKeys[i] * 10);
  }

  start = lib::time::TimeSpan::now();
  for (int i = 0; i < numOperations; i++)
  {
    map.remove(randomKeys[i]);
  }
  elapsed = lib::time::TimeSpan::now() - start;

  avgNs = elapsed.nanoseconds() / numOperations;
  opsPerSec = numOperations / elapsed.seconds();

  os::print("  Random:     %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec\n", numOperations, elapsed.milliseconds(), avgNs, opsPerSec);
}

void singleThreadBenchmarks()
{
  os::print("\n");
  os::print("================================================================================\n");
  os::print("                        SINGLE-THREADED BENCHMARKS\n");
  os::print("================================================================================\n");

  int sizes[] = {100, 1000, 10000, 100000};

  // INSERT
  os::print("\n--- INSERT BENCHMARK ---\n");
  for (int size : sizes)
  {
    os::print("\n[%d elements]\n", size);
    benchmarkInsertST(size);
  }

  // LOOKUP
  os::print("\n\n--- LOOKUP (at) BENCHMARK ---\n");
  for (int size : sizes)
  {
    os::print("\n[%d elements]\n", size);
    benchmarkAtST(size);
  }

  // REMOVE
  os::print("\n\n--- REMOVE BENCHMARK ---\n");
  for (int size : sizes)
  {
    os::print("\n[%d elements]\n", size);
    benchmarkRemoveST(size);
  }
}

// ============================================================================
// MULTI-THREADED BENCHMARKS
// ============================================================================

void benchmarkInsertMT(int opsPerThread, int numThreads)
{
  lib::ConcurrentMap<int, int> map;
  os::Thread threads[numThreads];
  std::atomic<bool> started(false);
  std::atomic<size_t> successfulInserts(0);

  lib::time::TimeSpan start = lib::time::TimeSpan::now();

  for (int i = 0; i < numThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
          }

          unsigned int seed = time(nullptr) + i;
          size_t inserted = 0;
          int base = i * opsPerThread;

          for (int j = 0; j < opsPerThread; j++)
          {
            if (map.insert(base + j, (base + j) * 10) != map.end())
            {
              inserted++;
            }
          }

          successfulInserts.fetch_add(inserted);
        });
  }

  started.store(true);

  for (int i = 0; i < numThreads; i++)
  {
    threads[i].join();
  }

  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  int totalOps = opsPerThread * numThreads;
  double avgNs = elapsed.nanoseconds() / totalOps;
  double opsPerSec = totalOps / elapsed.seconds();
  double scalability = opsPerSec / numThreads;

  os::print(
      "  %2d threads: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec | %8.0f ops/sec/thread\n", numThreads, totalOps, elapsed.milliseconds(), avgNs, opsPerSec, scalability);
}

void benchmarkAtMT(int mapSize, int lookupsPerThread, int numThreads)
{
  lib::ConcurrentMap<int, int> map;

  // Pre-populate
  for (int i = 0; i < mapSize; i++)
  {
    map.insert(i, i * 10);
  }

  os::Thread threads[numThreads];
  std::atomic<bool> started(false);
  std::atomic<size_t> totalHits(0);

  lib::time::TimeSpan start = lib::time::TimeSpan::now();

  for (int i = 0; i < numThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
          }

          unsigned int seed = time(nullptr) + i;
          size_t hits = 0;

          for (int j = 0; j < lookupsPerThread; j++)
          {
            int key = rand_r(&seed) % mapSize;
            auto iter = map.find(key);
            if (iter != map.end())
              hits++;
          }

          totalHits.fetch_add(hits);
        });
  }

  started.store(true);

  for (int i = 0; i < numThreads; i++)
  {
    threads[i].join();
  }

  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  int totalOps = lookupsPerThread * numThreads;
  double avgNs = elapsed.nanoseconds() / totalOps;
  double opsPerSec = totalOps / elapsed.seconds();
  double scalability = opsPerSec / numThreads;

  os::print(
      "  %2d threads: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec | %8.0f ops/sec/thread\n", numThreads, totalOps, elapsed.milliseconds(), avgNs, opsPerSec, scalability);
}

void benchmarkRemoveMT(int mapSize, int numThreads)
{
  lib::ConcurrentMap<int, int> map;

  // Pre-populate
  for (int i = 0; i < mapSize; i++)
  {
    map.insert(i, i * 10);
  }

  int opsPerThread = mapSize / numThreads;
  os::Thread threads[numThreads];
  std::atomic<bool> started(false);
  std::atomic<size_t> successfulRemoves(0);

  lib::time::TimeSpan start = lib::time::TimeSpan::now();

  for (int i = 0; i < numThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
          }

          size_t removed = 0;
          int base = i * opsPerThread;

          for (int j = 0; j < opsPerThread; j++)
          {
            if (map.remove(base + j))
              removed++;
          }

          successfulRemoves.fetch_add(removed);
        });
  }

  started.store(true);

  for (int i = 0; i < numThreads; i++)
  {
    threads[i].join();
  }

  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  int totalOps = opsPerThread * numThreads;
  double avgNs = elapsed.nanoseconds() / totalOps;
  double opsPerSec = totalOps / elapsed.seconds();
  double scalability = opsPerSec / numThreads;

  os::print(
      "  %2d threads: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec | %8.0f ops/sec/thread\n", numThreads, totalOps, elapsed.milliseconds(), avgNs, opsPerSec, scalability);
}

void benchmarkMixedMT(int mapSize, int opsPerThread, int numThreads)
{
  lib::ConcurrentMap<int, int> map;

  // Pre-populate 50%
  for (int i = 0; i < mapSize / 2; i++)
  {
    map.insert(i, i * 10);
  }

  os::Thread threads[numThreads];
  std::atomic<bool> started(false);
  std::atomic<size_t> inserts(0), removes(0), lookups(0);

  lib::time::TimeSpan start = lib::time::TimeSpan::now();

  for (int i = 0; i < numThreads; i++)
  {
    threads[i] = os::Thread(
        [&, i]()
        {
          while (!started.load())
          {
          }

          unsigned int seed = time(nullptr) + i;
          size_t localInserts = 0, localRemoves = 0, localLookups = 0;

          for (int j = 0; j < opsPerThread; j++)
          {
            int key = (rand_r(&seed) % mapSize) + (i * mapSize);
            int op = rand_r(&seed) % 10;

            if (op < 4)
            {
              // 40% insert
              if (map.insert(key, key * 10) != map.end())
                localInserts++;
            }
            else if (op < 7)
            {
              // 30% remove
              if (map.remove(key))
                localRemoves++;
            }
            else
            {
              // 30% lookup
              auto iter = map.find(key);
              if (iter != map.end())
                localLookups++;
            }
          }

          inserts.fetch_add(localInserts);
          removes.fetch_add(localRemoves);
          lookups.fetch_add(localLookups);
        });
  }

  started.store(true);

  for (int i = 0; i < numThreads; i++)
  {
    threads[i].join();
  }

  lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;

  int totalOps = opsPerThread * numThreads;
  double avgNs = elapsed.nanoseconds() / totalOps;
  double opsPerSec = totalOps / elapsed.seconds();
  double scalability = opsPerSec / numThreads;

  os::print(
      "  %2d threads: %7d ops | %8.3f ms | %8.2f ns/op | %10.0f ops/sec | %8.0f ops/sec/thread | I:%zu R:%zu L:%zu\n",
      numThreads,
      totalOps,
      elapsed.milliseconds(),
      avgNs,
      opsPerSec,
      scalability,
      inserts.load(),
      removes.load(),
      lookups.load());
}

void multiThreadBenchmarks()
{
  size_t maxThreads = os::Thread::getHardwareConcurrency();

  os::print("\n");
  os::print("================================================================================\n");
  os::print("                        MULTI-THREADED BENCHMARKS\n");
  os::print("================================================================================\n");
  os::print("Hardware Concurrency: %zu threads\n", maxThreads);

  // Test with different thread counts
  std::vector<int> threadCounts;
  threadCounts.push_back(1);
  threadCounts.push_back(2);
  threadCounts.push_back(4);
  if (maxThreads >= 8)
    threadCounts.push_back(8);
  if (maxThreads >= 16)
    threadCounts.push_back(16);
  if (maxThreads > 16)
    threadCounts.push_back(maxThreads);

  // INSERT SCALING
  os::print("\n--- INSERT SCALING (10k ops/thread) ---\n");
  os::print("\n[Thread scaling with 10,000 inserts per thread]\n");
  for (int tc : threadCounts)
  {
    benchmarkInsertMT(10000, tc);
  }

  // LOOKUP SCALING
  os::print("\n\n--- LOOKUP SCALING (map size: 100k, 10k lookups/thread) ---\n");
  os::print("\n[Thread scaling with 100,000 element map]\n");
  for (int tc : threadCounts)
  {
    benchmarkAtMT(100000, 10000, tc);
  }

  // REMOVE SCALING
  os::print("\n\n--- REMOVE SCALING (100k total elements) ---\n");
  os::print("\n[Thread scaling removing 100,000 total elements]\n");
  for (int tc : threadCounts)
  {
    benchmarkRemoveMT(100000, tc);
  }

  // MIXED WORKLOAD SCALING
  os::print("\n\n--- MIXED WORKLOAD (40%% insert, 30%% remove, 30%% lookup) ---\n");
  os::print("\n[Thread scaling with 10,000 ops/thread, map size 50k]\n");
  for (int tc : threadCounts)
  {
    benchmarkMixedMT(50000, 10000, tc);
  }
}

// ============================================================================
// CONTENTION BENCHMARKS
// ============================================================================

void benchmarkContention()
{
  size_t numThreads = os::Thread::getHardwareConcurrency();

  os::print("\n");
  os::print("================================================================================\n");
  os::print("                        CONTENTION ANALYSIS\n");
  os::print("================================================================================\n");
  os::print("Using %zu threads\n", numThreads);

  // Low contention: large key space
  os::print("\n--- LOW CONTENTION (key space: 1M) ---\n");
  {
    lib::ConcurrentMap<int, int> map;
    os::Thread threads[numThreads];
    std::atomic<bool> started(false);

    lib::time::TimeSpan start = lib::time::TimeSpan::now();

    for (size_t i = 0; i < numThreads; i++)
    {
      threads[i] = os::Thread(
          [&, i]()
          {
            while (!started.load())
            {
            }
            unsigned int seed = time(nullptr) + i;

            for (int j = 0; j < 10000; j++)
            {
              int key = rand_r(&seed) % 1000000; // Large key space
              int op = rand_r(&seed) % 3;

              if (op == 0)
                map.insert(key, key);
              else if (op == 1)
                map.remove(key);
              else
                map.find(key);
            }
          });
    }

    started.store(true);
    for (size_t i = 0; i < numThreads; i++)
      threads[i].join();

    lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;
    int totalOps = 10000 * numThreads;
    os::print("  Total: %d ops in %.3f ms | %.0f ops/sec\n", totalOps, elapsed.milliseconds(), totalOps / elapsed.seconds());
  }

  // Medium contention
  os::print("\n--- MEDIUM CONTENTION (key space: 10k) ---\n");
  {
    lib::ConcurrentMap<int, int> map;
    os::Thread threads[numThreads];
    std::atomic<bool> started(false);

    lib::time::TimeSpan start = lib::time::TimeSpan::now();

    for (size_t i = 0; i < numThreads; i++)
    {
      threads[i] = os::Thread(
          [&, i]()
          {
            while (!started.load())
            {
            }
            unsigned int seed = time(nullptr) + i;

            for (int j = 0; j < 10000; j++)
            {
              int key = rand_r(&seed) % 10000; // Medium key space
              int op = rand_r(&seed) % 3;

              if (op == 0)
                map.insert(key, key);
              else if (op == 1)
                map.remove(key);
              else
                map.find(key);
            }
          });
    }

    started.store(true);
    for (size_t i = 0; i < numThreads; i++)
      threads[i].join();

    lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;
    int totalOps = 10000 * numThreads;
    os::print("  Total: %d ops in %.3f ms | %.0f ops/sec\n", totalOps, elapsed.milliseconds(), totalOps / elapsed.seconds());
  }

  // High contention
  os::print("\n--- HIGH CONTENTION (key space: 100) ---\n");
  {
    lib::ConcurrentMap<int, int> map;
    os::Thread threads[numThreads];
    std::atomic<bool> started(false);

    lib::time::TimeSpan start = lib::time::TimeSpan::now();

    for (size_t i = 0; i < numThreads; i++)
    {
      threads[i] = os::Thread(
          [&, i]()
          {
            while (!started.load())
            {
            }
            unsigned int seed = time(nullptr) + i;

            for (int j = 0; j < 10000; j++)
            {
              int key = rand_r(&seed) % 100; // Small key space = high contention
              int op = rand_r(&seed) % 3;

              if (op == 0)
                map.insert(key, key);
              else if (op == 1)
                map.remove(key);
              else
                map.find(key);
            }
          });
    }

    started.store(true);
    for (size_t i = 0; i < numThreads; i++)
      threads[i].join();

    lib::time::TimeSpan elapsed = lib::time::TimeSpan::now() - start;
    int totalOps = 10000 * numThreads;
    os::print("  Total: %d ops in %.3f ms | %.0f ops/sec\n", totalOps, elapsed.milliseconds(), totalOps / elapsed.seconds());
  }
}

// ============================================================================
// MAIN BENCHMARK RUNNER
// ============================================================================

void runAllBenchmarks()
{
  os::print("\n");
  os::print("################################################################################\n");
  os::print("##                                                                            ##\n");
  os::print("##              CONCURRENT SKIP LIST - PERFORMANCE BENCHMARKS                ##\n");
  os::print("##                                                                            ##\n");
  os::print("################################################################################\n");

  singleThreadBenchmarks();
  multiThreadBenchmarks();
  benchmarkContention();

  os::print("\n");
  os::print("################################################################################\n");
  os::print("##                        BENCHMARKS COMPLETED                                ##\n");
  os::print("################################################################################\n");
  os::print("\n");
}
int main()
{
  lib::memory::SystemMemoryManager::init();

  for (int i = 0; i < 10; i++)
  {
    srand(time(nullptr));
    basicTests();
    iteratorTests();
    multiThreadInsertTests();
    multiThreadRemoveTests();
    mixedOperationsTests();
    concurrentIterationTests();
    randomIteratorModificationTests();
  }

  // Run stress test multiple times
  for (int i = 0; i < 10; i++)
  {
    os::print("\n=== Stress test iteration %d ===\n", i + 1);
    stressTest();
  }

  runAllBenchmarks();
  os::print("\n=== All tests passed! ===\n");

  lib::memory::SystemMemoryManager::shutdown();
  return 0;
}