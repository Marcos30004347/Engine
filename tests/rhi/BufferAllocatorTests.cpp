// #include "memory/SystemMemoryManager.hpp"
// #include "os/Logger.hpp"
// #include "os/Thread.hpp"
// #include "os/print.hpp"
// #include "rhi/BufferAllocator.hpp"
// #include "rhi/Device.hpp"
// #include "time/TimeSpan.hpp"

// #include <assert.h>
// #include <atomic>
// #include <random>
// #include <vector>

// using namespace rhi;

// void bufferAllocatorSingleThreadTest()
// {
//   const uint64_t totalSize = 1024ull * 1024ull;
//   BufferAllocator allocator(nullptr, 0, totalSize);

//   std::mt19937 rng(1234);
//   std::uniform_int_distribution<size_t> sizeDist(16, 1024);
//   std::uniform_int_distribution<size_t> alignDist(1, 5);
//   std::uniform_int_distribution<int> freeOrNot(0, 1);

//   double allocTimeTotal = 0;
//   double freeTimeTotal = 0;

//   for (size_t j = 0; j < 20000; j++)
//   {
//     BufferViewInfo info;
//     size_t size = sizeDist(rng);
//     size_t alignment = 1ull << alignDist(rng);

//     lib::time::TimeSpan then = lib::time::TimeSpan::now();
//     BufferAllocateStatus status = allocator.allocate(size, alignment, info);
//     allocTimeTotal += (lib::time::TimeSpan::now() - then).nanoseconds();

//     if (status == BufferAllocateStatus_Ok)
//     {
//       then = lib::time::TimeSpan::now();
//       allocator.free(info);
//       freeTimeTotal += (lib::time::TimeSpan::now() - then).nanoseconds();
//     }
//     else if (status == BufferAllocateStatus_OutOfMemory)
//     {
//       os::Logger::logf("Single-thread ran out of memory on iteration %zu", j);
//     }
//     else
//     {
//       assert(false && "Unexpected allocation error");
//     }
//   }

//   os::Logger::logf("Single-thread average allocation time: %.2f ns, average free time: %.2f ns", allocTimeTotal / 20000.0, freeTimeTotal / 20000.0);

//   assert(allocator.getUsedSize() == 0);

//   os::Logger::logf(
//       "Single-thread BufferAllocator test passed. Free blocks ~%zu, Used=%llu, Free=%llu",
//       allocator.getApproximateFreeBlockCount(),
//       (unsigned long long)allocator.getUsedSize(),
//       (unsigned long long)allocator.getFreeSize());
// }

// void bufferAllocatorMultithreadTest()
// {
//   const uint64_t totalSize = 1024ull * 1024ull;
//   BufferAllocator allocator(nullptr, 0, totalSize);

//   size_t totalThreads = os::Thread::getHardwareConcurrency();
//   os::Thread threads[totalThreads];
//   std::atomic<bool> started(false);

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i] = os::Thread(
//         [&, i]()
//         {
//           std::mt19937 rng((unsigned)(i + 1234));
//           std::uniform_int_distribution<size_t> sizeDist(16, 1024);
//           std::uniform_int_distribution<size_t> alignDist(1, 5);
//           std::uniform_int_distribution<int> freeOrNot(0, 1);

//           while (!started.load())
//           {
//           } // spin until start

//           double allocTimeTotal = 0;
//           double freeTimeTotal = 0;

//           for (size_t j = 0; j < 20000; j++)
//           {
//             BufferViewInfo info;
//             size_t size = sizeDist(rng);
//             size_t alignment = 1ull << alignDist(rng);

//             lib::time::TimeSpan then = lib::time::TimeSpan::now();
//             BufferAllocateStatus status = allocator.allocate(size, alignment, info);
//             allocTimeTotal += (lib::time::TimeSpan::now() - then).nanoseconds();

//             if (status == BufferAllocateStatus_Ok)
//             {
//               then = lib::time::TimeSpan::now();
//               allocator.free(info);
//               freeTimeTotal += (lib::time::TimeSpan::now() - then).nanoseconds();
//             }
//             else if (status == BufferAllocateStatus_OutOfMemory)
//             {
//               os::Logger::logf("Thread %u ran out of memory on iteration %zu", i, j);
//             }
//             else
//             {
//               assert(false && "Unexpected allocation error");
//             }
//           }

//           os::Logger::logf("Thread %u average allocation time: %.2f ns, average free time: %.2f ns", i, allocTimeTotal / 20000.0, freeTimeTotal / 20000.0);
//         });
//   }

//   started = true;

//   for (size_t i = 0; i < totalThreads; i++)
//   {
//     threads[i].join();
//   }

//   assert(allocator.getUsedSize() == 0);

//   os::Logger::logf(
//       "Multithread BufferAllocator test passed. Free blocks ~%zu, Used=%llu, Free=%llu",
//       allocator.getApproximateFreeBlockCount(),
//       (unsigned long long)allocator.getUsedSize(),
//       (unsigned long long)allocator.getFreeSize());
// }

// int main()
// {
//   os::Logger::start();
//   lib::memory::SystemMemoryManager::init();

//   os::Logger::logf("=== Running single-thread test ===");
//   bufferAllocatorSingleThreadTest();

//   os::Logger::logf("=== Running multithread test ===");
//   bufferAllocatorMultithreadTest();

//   lib::memory::SystemMemoryManager::shutdown();
//   os::Logger::shutdown();
//   return 0;
// }
int main() {
    return 0;
}