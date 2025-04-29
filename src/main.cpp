#include "jobsystem/jobsystem.hpp"
#include "rhi/rhi.hpp"
#include "window/window.hpp"
#include <cstdio>
#include "lib/parallel/priorityqueue.hpp"

using namespace window;
using namespace jobsystem;

#include "fcontext/fcontext.h"
#define ARRAY_SIZE 16 * 3
int a[ARRAY_SIZE];

void group_example(uint32_t index, uint32_t n, uint32_t group_size, int *a)
{
  // std::lock_guard<std::mutex> lock(print_mutex); // Lock during the entire printing
  // for(uint32_t i = 0; i < group_size; i++) {
  //     if(index + i >= n) break;
  //     printf("%i \n", a[index + i]);
  // }
  threadSafePrintf("AAAA\n");
}
void enqueue_two()
{
  auto p0 = JobSystem::enqueue(group_example, 0, ARRAY_SIZE, 16, a);
  auto p1 = JobSystem::enqueue(group_example, 0, ARRAY_SIZE, 16, a);

  JobSystem::wait(p0);
  JobSystem::wait(p1);
  // std::lock_guard<std::mutex> lock(print_mutex); // Lock during the entire printing
  // for(uint32_t i = 0; i < group_size; i++) {
  //     if(index + i >= n) break;
  //     printf("%i \n", a[index + i]);
  // }
  printf("over\n");
}

void entry()
{
  threadSafePrintf("aaaaa\n");
  for (uint32_t i = 0; i < ARRAY_SIZE; i++)
  {
    a[i] = i;
  }
  threadSafePrintf("bbbb\n");

  //   auto p = JobSystem::enqueueGroup(group_example, ARRAY_SIZE, 16, a);
  auto p = JobSystem::enqueue(enqueue_two);
  //   Window *appWindow = createWindow(WindowBackend::WindowBackend_SDL3, WindowSurfaceType::WindowSurface_Vulkan, "Engine");
  threadSafePrintf("cccc\n");

  JobSystem::wait(p);
  threadSafePrintf("ddddd\n");

  JobSystem::stop();
}

// void child_fn0(fcontext_transfer_t arg) {
//     printf("aaa\n");
//     fcontext_t parent = arg.ctx;
//     fcontext_transfer_t t0 =jump_fcontext(parent, NULL);
//     printf("bbbb\n");
//     fcontext_transfer_t t1 =jump_fcontext(t0.ctx, NULL);
//     printf("cccc\n");

// }
fiber::Fiber *fib0, *fib1, *fib2, *fib3;

void f0(void*, fiber::Fiber*) {
  threadSafePrintf("fib3 A\n");
  fiber::Fiber::switchTo(fib2);
  threadSafePrintf("fib3 B\n");
  fiber::Fiber::switchTo(fib2);
}
void f1(void*, fiber::Fiber*) {
  threadSafePrintf("fib0 A\n");
  fiber::Fiber::switchTo(fib1);
  threadSafePrintf("fib0 B\n");
  fiber::Fiber::switchTo(fib3);
  threadSafePrintf("fib0 C\n");
  fiber::Fiber::switchTo(fib2);
}
void f2(void*, fiber::Fiber*) {
  threadSafePrintf("fib1 A\n");
  fiber::Fiber::switchTo(fib0);
  threadSafePrintf("fib1 B\n");
  fiber::Fiber::switchTo(fib2);
  threadSafePrintf("fib1 C\n");
  fiber::Fiber::switchTo(fib2);
}
int main()
{

  lib::parallel::PriorityQueue<int, int> pq;

  pq.insert(2, 1);
  pq.insert(9, 2);
  pq.insert(8, 3);
  pq.insert(7, 4);
  pq.insert(6, -1);

  int x;
  pq.deletemin(x);
  printf("%i\n", x);
  pq.deletemin(x);
  printf("%i\n", x);
  pq.deletemin(x);
  printf("%i\n", x);
  pq.deletemin(x);
  printf("%i\n", x);
  pq.deletemin(x);
  printf("%i\n", x);
  fib3 = fiber::FiberPool::acquire(f0, nullptr);
  fib0 = fiber::FiberPool::acquire(f1, nullptr);
  fib1 = fiber::FiberPool::acquire(f2, nullptr);

  fib2 = fiber::Fiber::currentThreadToFiber();

  fiber::Fiber::switchTo(fib1);
  fiber::Fiber::switchTo(fib0);
  fiber::Fiber::switchTo(fib0);
  fiber::Fiber::switchTo(fib3);

  delete fib0;
  delete fib1;
  delete fib2;
  //   fib0->resume();
  //   printf("finished\n");

  JobSystem::init(5, entry);
  //   rhi::DeviceRequiredLimits limits;

  //   limits.minimumComputeSharedMemory = 0;
  //   limits.minimumComputeWorkGroupInvocations = 0;
  //   limits.minimumMemory = 1024 * 1024;

  //   std::vector<rhi::DeviceFeatures> features = {
  //     rhi::DeviceFeatures::DeviceFeatures_Graphics,
  //     rhi::DeviceFeatures::DeviceFeatures_Compute,
  //   };

  //   rhi::Device *device = rhi::Device::create(rhi::DeviceBackend_Vulkan_1_2, limits, features);

  //   rhi::SurfaceHandle surface = device->addWindowForDrawing(appWindow);

  //   device->init();

  //   while (!appWindow->shouldClose())
  //   {
  //     appWindow->update();
  //   }

  //   delete device;
  JobSystem::shutdown();
  return 0;
}

// #include "lib/parallel/priorityqueue.hpp" // Assuming your PriorityQueue is in priorityqueue.hpp
// #include <algorithm>
// #include <chrono>
// #include <iostream>
// #include <numeric> // std::iota
// #include <random>
// #include <thread>
// #include <vector>

// using namespace lib::parallel;

// // Function to generate a vector of unique random keys
// std::vector<int> generate_random_keys(int num_keys)
// {
//   std::vector<int> keys(num_keys);
//   std::iota(keys.begin(), keys.end(), 0); // Fill with 0, 1, 2, ..., num_keys-1
//   std::random_device rd;
//   std::mt19937 g(rd());
//   std::shuffle(keys.begin(), keys.end(), g); // Shuffle the keys
//   return keys;
// }

// // Function for a thread to push elements into the priority queue
// void push_thread_func(PriorityQueue<int, int> &pq, const std::vector<int> &keys, int thread_id, int num_threads)
// {
//   int num_elements_per_thread = keys.size() / num_threads;
//   int start_index = thread_id * num_elements_per_thread;
//   int end_index = (thread_id == num_threads - 1) ? keys.size() : start_index + num_elements_per_thread;

//   for (int i = start_index; i < end_index; ++i)
//   {
//     pq.insert(keys[i], keys[i]); // Insert key as both key and value
//   }
// }

// // Function for a thread to pop elements from the priority queue
// void pop_thread_func(PriorityQueue<int, int> &pq, int num_elements_to_pop)
// {
//   for (int i = 0; i < num_elements_to_pop; ++i)
//   {
//     int value;
//     if (pq.deletemin(value))
//     {
//         std::cout << "Popped: " << value << " from thread " << std::this_thread::get_id() << std::endl; //Removed cout to reduce noise
//     }
//     else
//     {
//       // std::cout << "Pop failed" << std::endl; //Removed cout to reduce noise
//     }
//   }
// }

// // Test function to spawn threads and test concurrent push and pop
// void test_concurrent_priority_queue(int num_threads, int num_elements_to_push, int num_elements_to_pop)
// {
//   PriorityQueue<int, int> pq;
//   std::vector<std::thread> push_threads;
//   std::vector<std::thread> pop_threads;

//   // 1. Prepare data for pushing (unique random keys)
//   std::vector<int> keys = generate_random_keys(num_elements_to_push);

//   // 2. Spawn push threads
//   for (int i = 0; i < num_threads; ++i)
//   {
//     push_threads.emplace_back(push_thread_func, std::ref(pq), std::ref(keys), i, num_threads);
//   }

//   // 3. Spawn pop threads
//   for (int i = 0; i < num_threads; ++i)
//   {
//     pop_threads.emplace_back(pop_thread_func, std::ref(pq), num_elements_to_pop / num_threads);
//   }

//   // 4. Wait for all threads to finish
//   for (auto &thread : push_threads)
//   {
//     thread.join();
//   }

//   for (auto &thread : pop_threads)
//   {
//     thread.join();
//   }

//   // 5. (Optional) Verify the queue is empty after all pops
//   int remaining_elements = 0;
//   int val;
  
//   while (pq.deletemin(val)) {
//     remaining_elements++;
//   }
  
//   std::cout << "Remaining elements in queue: " << remaining_elements << std::endl;
  
//   if (remaining_elements != 0)
//   {
//     std::cout << "FAILED: Queue is not empty after all pops" << std::endl;
//   }
//   else
//   {
//     std::cout << "PASSED: Queue is empty after all pops" << std::endl;
//   }
// }

// int main()
// {
//   // Example usage:
//   int num_threads = 4;
//   int num_elements_to_push = 10000;
//   int num_elements_to_pop = 10000;
//   test_concurrent_priority_queue(num_threads, num_elements_to_push, num_elements_to_pop);

//   return 0;
// }
