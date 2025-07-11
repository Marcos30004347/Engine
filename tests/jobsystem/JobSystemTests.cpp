#include "jobsystem/JobSystem.hpp"
#include "lib/time/TimeSpan.hpp"

jobsystem::Job *mainJob;
jobsystem::Job *funcJob;

lib::time::TimeSpan prev;
lib::time::TimeSpan totalTime;

int counter = 0;

int add3(int i)
{
  return i + 3;
}

void entry()
{
  jobsystem::JobEnqueueData data;

  data.allocatorIndex = 0;
  data.queueIndex = 0;
  data.stackSize = 1024 * 1024;

  size_t count = 16;
  jobsystem::Promise<int> promises[count];

  for (int i = 0; i < count; i++)
  {
    os::print("enqueing add3:\n");
    promises[i] = jobsystem::JobSystem::enqueue(&data, add3, i);
  }

  for (int i = 0; i < count; i++)
  {
    int &x = jobsystem::JobSystem::wait(promises[i]);
    // os::print("count = %u, %u\n", i, promises[i].job.use_count());

    // assert(x == i + 3);
  }

  jobsystem::JobSystem::stop();
}

int main()
{
  jobsystem::fiber::FiberPool *pool = new jobsystem::fiber::FiberPool(1024 * 1024);
  jobsystem::JobAllocator *allocator = new jobsystem::JobAllocator(sizeof(uint8_t) * 256, 4096);

  jobsystem::JobSystemSettings settings;

  jobsystem::JobAllocatorSettings allocators[1];
  jobsystem::JobQueueSettings queues[1];
  jobsystem::JobStackSettings stacks[1];

  allocators[0].capacity = 4096;
  allocators[0].payloadSize = sizeof(uint8_t) * 256;

  stacks[0].stackSize = 1024 * 1024;

  settings.threadsCount = 1; // os::Thread::getHardwareConcurrency();

  settings.jobAllocatorsSettings = allocators;
  settings.jobAllocatorSettingsCount = 1;

  settings.jobQueueSettings = queues;
  settings.jobQueueSettingsCount = 1;

  settings.jobStackSettings = stacks;
  settings.jobStackSettingsCount = 1;

  jobsystem::JobSystem::init(entry, &settings);
  os::print("here\n");
  jobsystem::JobSystem::shutdown();

  return 0;
}