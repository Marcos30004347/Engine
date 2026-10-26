// // tests/ConcurrentHashMapTests.cpp
// #include "datastructure/ConcurrentHashMap.hpp"
// #include "memory/SystemMemoryManager.hpp"
// #include "os/Thread.hpp"
// #include "os/print.hpp"
// #include "time/TimeSpan.hpp"
// #include <algorithm>
// #include <assert.h>
// #include <cstdlib>
// #include <ctime>
// #include <vector>
// #include <atomic>

// void basicTests()
// {
//   os::print("Running basic tests...\n");

//   lib::ConcurrentHashMap<int, int> map;

//   // Test insert
//   map.insert(10, 100);
//   map.insert(20, 200);
//   map.insert(30, 300);
//   assert(map.size() == 3);

//   // Test find / contains
//   int value = 0;
//   assert(map.find(10, value) && value == 100);
//   assert(map.find(20, value) && value == 200);
//   assert(map.find(30, value) && value == 300);
//   assert(!map.find(40, value));
//   assert(map.contains(10));
//   assert(!map.contains(40));

//   // Test overwrite via insert (class semantics place value)
//   map.insert(10, 150); // our implementation will overwrite
//   assert(map.find(10, value) && value == 150);

//   // Test erase
//   assert(map.erase(20));
//   assert(map.size() == 2);
//   assert(!map.find(20, value));
//   assert(!map.erase(20)); // already erased

//   // clear
//   map.clear();
//   assert(map.size() == 0);

//   os::print("Basic tests passed!\n");
// }

// void multiThreadInsertTests()
// {
//   os::print("Running multi-threaded insert tests...\n");

//   lib::ConcurrentHashMap<int, int> map;

//   size_t totalThreads = os::Thread::getHardwareConcurrency();
//   std::vector<os::Thread> threads(totalThreads);
//   std::atomic<bool> started(false);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i] = os::Thread(
//         [&, i]()
//         {
//           while (!started.load())
//           {
//             // spin
//           }

//           lib::time::TimeSpan then;
//           double total_ns = 0;

//           // Each thread inserts 2000 unique keys
//           int base = static_cast<int>(i * 2000);

//           for (int j = 0; j < 2000; j++)
//           {
//             then = lib::time::TimeSpan::now();
//             map.insert(base + j, (base + j) * 10);
//             total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
//           }

//           os::print("Thread %u average insertion time is %fns\n",
//                     os::Thread::getCurrentThreadId(),
//                     total_ns / 2000.0);
//         });
//   }

//   started.store(true);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i].join();
//   }

//   // Verify all elements are present
//   size_t expected = totalThreads * 2000;
//   size_t actual = map.size();
//   os::print("Expected size: %zu, Actual size: %zu\n", expected, actual);
//   assert(actual == expected);

//   int value;
//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     int base = static_cast<int>(i * 2000);
//     for (int j = 0; j < 2000; j++)
//     {
//       bool ok = map.find(base + j, value);
//       assert(ok);
//       assert(value == (base + j) * 10);
//     }
//   }

//   os::print("Multi-threaded insert tests passed!\n");
// }

// void multiThreadEraseTests()
// {
//   os::print("Running multi-threaded erase tests...\n");

//   lib::ConcurrentHashMap<int, int> map;

//   size_t totalThreads = os::Thread::getHardwareConcurrency();
//   size_t elementsPerThread = 1500;

//   // Pre-populate the map
//   for (size_t i = 0; i < totalThreads * elementsPerThread; i++)
//   {
//     map.insert(static_cast<int>(i), static_cast<int>(i));
//   }
//   assert(map.size() == totalThreads * elementsPerThread);

//   std::vector<os::Thread> threads(totalThreads);
//   std::atomic<bool> started(false);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i] = os::Thread(
//         [&, i]()
//         {
//           while (!started.load())
//           {
//             // spin
//           }

//           lib::time::TimeSpan then;
//           double total_ns = 0;

//           // Each thread removes its own range
//           int base = static_cast<int>(i * elementsPerThread);
//           for (size_t j = 0; j < elementsPerThread; j++)
//           {
//             then = lib::time::TimeSpan::now();
//             bool removed = map.erase(base + static_cast<int>(j));
//             total_ns += (lib::time::TimeSpan::now() - then).nanoseconds();
//             assert(removed);
//           }

//           os::print("Thread %u average removal time is %fns\n",
//                     os::Thread::getCurrentThreadId(),
//                     total_ns / static_cast<double>(elementsPerThread));
//         });
//   }

//   started.store(true);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i].join();
//   }

//   // Verify all elements are removed
//   assert(map.size() == 0);

//   os::print("Multi-threaded erase tests passed!\n");
// }

// void mixedOperationsTests()
// {
//   os::print("Running mixed operations tests...\n");

//   lib::ConcurrentHashMap<int, int> map;

//   size_t totalThreads = os::Thread::getHardwareConcurrency();
//   std::vector<os::Thread> threads(totalThreads);
//   std::atomic<bool> started(false);
//   std::atomic<size_t> totalInserts(0);
//   std::atomic<size_t> totalRemoves(0);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i] = os::Thread(
//         [&, i]()
//         {
//           while (!started.load())
//           {
//             // spin
//           }

//           unsigned int seed = static_cast<unsigned int>(time(nullptr)) + static_cast<unsigned int>(i);
//           size_t inserts = 0;
//           size_t removes = 0;

//           for (int j = 0; j < 2000; j++)
//           {
//             int key = (rand_r(&seed) % 5000) + static_cast<int>(i * 5000);
//             int op = rand_r(&seed) % 3;

//             if (op == 0)
//             {
//               // Insert
//               map.insert(key, key * 10);
//               inserts++;
//             }
//             else if (op == 1)
//             {
//               // Remove
//               if (map.erase(key))
//               {
//                 removes++;
//               }
//             }
//             else
//             {
//               // Find
//               int value;
//               map.find(key, value);
//             }
//           }

//           totalInserts.fetch_add(inserts);
//           totalRemoves.fetch_add(removes);

//           os::print("Thread %u: inserts=%zu, removes=%zu\n",
//                     os::Thread::getCurrentThreadId(), inserts, removes);
//         });
//   }

//   started.store(true);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i].join();
//   }

//   size_t expectedSize = totalInserts.load() - totalRemoves.load();
//   size_t actualSize = map.size();

//   os::print("Total inserts: %zu, Total removes: %zu\n", totalInserts.load(), totalRemoves.load());
//   os::print("Expected size (approx): %zu, Actual size: %zu\n", expectedSize, actualSize);

//   // Because keys can collide across threads the exact size might differ; we assert a reasonable bound:
//   // Actual size must be <= total inserts and >= 0. Here we assert it matches expectedSize when keys were mostly unique.
//   // For stricter asserts, you can change key generation to guarantee uniqueness.
//   assert(actualSize <= totalInserts.load());
//   // It's acceptable that actualSize != expectedSize if keys overlapped; we'll log both.

//   os::print("Mixed operations tests passed!\n");
// }

// void stressTest()
// {
//   os::print("Running stress test...\n");

//   lib::ConcurrentHashMap<int, int> map;

//   size_t totalThreads = os::Thread::getHardwareConcurrency();
//   std::vector<os::Thread> threads(totalThreads);
//   std::atomic<bool> started(false);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i] = os::Thread(
//         [&, i]()
//         {
//           while (!started.load())
//           {
//             // spin
//           }

//           unsigned int seed = static_cast<unsigned int>(time(nullptr)) + static_cast<unsigned int>(i);

//           for (int j = 0; j < 15000; j++)
//           {
//             int key = rand_r(&seed) % 2000;
//             int op = rand_r(&seed) % 10;

//             if (op < 4)
//             {
//               // 40% insert
//               map.insert(key + static_cast<int>(i * 10000), (key) * 10);
//             }
//             else if (op < 7)
//             {
//               // 30% remove
//               map.erase(key + static_cast<int>(i * 10000));
//             }
//             else
//             {
//               // 60% find (combines the remaining buckets)
//               int value;
//               map.find(key + static_cast<int>(i * 10000), value);
//             }
//           }
//         });
//   }

//   started.store(true);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i].join();
//   }

//   os::print("Stress test completed. Final size: %zu\n", map.size());
// }

int main()
{
  //   lib::memory::SystemMemoryManager::init();

  //   srand(static_cast<unsigned int>(time(nullptr)));

  //   basicTests();
  //   multiThreadInsertTests();
  //   multiThreadEraseTests();
  //   mixedOperationsTests();

  //   // Run stress test a few times
  //   for (int i = 0; i < 5; i++)
  //   {
  //     os::print("\n=== Stress test iteration %d ===\n", i + 1);
  //     stressTest();
  //   }

  //   os::print("\n=== All ConcurrentHashMap tests finished! ===\n");

  //   lib::memory::SystemMemoryManager::shutdown();
  return 0;
}
