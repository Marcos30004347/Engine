#include "async/async.hpp"
#include "os/print.hpp"
#include "time/TimeSpan.hpp"
#include <cassert>
#include <thread>
#include <vector>

static const size_t JOB_COUNT = 32;
static const size_t ITERATIONS = 1000;

int add1(int i)
{
  return i + 1;
}

void entry()
{
  os::print("--- AsyncManager Benchmark ---\n");
  os::print("Workload: %zu iterations of %zu concurrent jobs\n", ITERATIONS, JOB_COUNT);

  lib::time::TimeSpan start = lib::time::TimeSpan::now();

  for (size_t iter = 0; iter < ITERATIONS; iter++)
  {
    //os::print("iteration=%u\n", iter);

    async::Promise<int> promises[JOB_COUNT];

    for (int i = 0; i < JOB_COUNT; i++)
    {
      //os::print("%u enqueuing %u\n", os::Thread::getCurrentThreadId(), i);
      promises[i] = async::enqueue(add1, i);
    }

    for (int i = 0; i < JOB_COUNT; i++)
    {
      int &v = async::wait(promises[i]);
      (void)v;
    }
  }

  lib::time::TimeSpan end = lib::time::TimeSpan::now();

  double total_ns = (double)(end - start).nanoseconds();
  double total_ms = total_ns / 1000000.0;
  double avg_ns_per_job = total_ns / (ITERATIONS * JOB_COUNT);

  os::print("Total execution time: %.2f ms\n", total_ms);
  os::print("Average time per job (Overhead + Exec): %.2f ns\n", avg_ns_per_job);
  os::print("--------------------------------\n\n");

  async::stop();
}

// void threadWorker(int input, int *result)
// {
//   *result = add1(input);
// }

// void measureStdThread()
// {
//   os::print("--- std::thread Benchmark ---\n");
//   os::print("Workload: %zu iterations of %zu concurrent threads\n", ITERATIONS, JOB_COUNT);

//   std::vector<int> results(JOB_COUNT);
//   std::vector<std::thread> threads;
//   threads.reserve(JOB_COUNT);

//   lib::time::TimeSpan start = lib::time::TimeSpan::now();

//   for (size_t iter = 0; iter < ITERATIONS; ++iter)
//   {
//     for (size_t i = 0; i < JOB_COUNT; ++i)
//     {
//       threads.emplace_back(threadWorker, (int)i, &results[i]);
//     }

//     for (size_t i = 0; i < JOB_COUNT; ++i)
//     {
//       if (threads[i].joinable())
//       {
//         threads[i].join();
//       }
//     }

//     threads.clear();
//   }

//   lib::time::TimeSpan end = lib::time::TimeSpan::now();

//   double total_ns = (double)(end - start).nanoseconds();
//   double total_ms = total_ns / 1000000.0;
//   double avg_ns_per_job = total_ns / (ITERATIONS * JOB_COUNT);

//   os::print("Total execution time: %.2f ms\n", total_ms);
//   os::print("Average time per job (Syscall + Exec): %.2f ns\n", avg_ns_per_job);
//   os::print("--------------------------------\n");
// }

int main()
{
  for (uint32_t i = 0; i < 1000; i++)
  {
    async::SystemSettings settings;

    // Capacity must accommodate the concurrent burst (JOB_COUNT)
    settings.jobsCapacity = JOB_COUNT * 2;
    settings.stackSize = 1024 * 1024; // async::getMinStackSize();
    settings.threadsCount = 2;        // os::Thread::getHardwareConcurrency();

    os::print("Initializing AsyncManager...\n");
    lib::time::TimeSpan initStart = lib::time::TimeSpan::now();

    async::init(entry, settings);

    lib::time::TimeSpan initEnd = lib::time::TimeSpan::now();
    double init_ms = (initEnd - initStart).nanoseconds() / 1000000.0;
    os::print("AsyncManager Initialization overhead: %.2f ms\n\n", init_ms);

    async::shutdown();
  }

  // measureStdThread();

  return 0;
}