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
  for (size_t iter = 0; iter < 1000; iter++)
  {
    jobsystem::JobEnqueueData data;

    data.allocatorIndex = 0;
    data.queueIndex = 0;
    data.stackSize = 4096;

    size_t count = 128;
    jobsystem::Promise<int> promises[count];

    // os::print("Thread %u Start enqueing\n", os::Thread::getCurrentThreadId());
    
    for (int i = 0; i < count; i++)
    {
      promises[i] = jobsystem::JobSystem::enqueue(&data, add3, i);
    }
    
    // os::print("Thread %u Start waiting\n", os::Thread::getCurrentThreadId());

    for (int i = 0; i < count; i++)
    {
      jobsystem::JobSystem::wait(promises[i]);
    }

    // os::print("Thread %u finished iteration\n", os::Thread::getCurrentThreadId());
  }

  jobsystem::JobSystem::stop();
}

int main()
{
  // jobsystem::JobAllocator *allocator = new jobsystem::JobAllocator(sizeof(uint8_t) * 4096, 4096);

  jobsystem::JobSystemSettings settings;

  jobsystem::JobAllocatorSettings allocators[1];
  jobsystem::JobQueueSettings queues[1];
  jobsystem::JobStackSettings stacks[1];

  allocators[0].capacity = 4096;
  allocators[0].payloadSize = sizeof(uint8_t) * 256;

  stacks[0].stackSize = 4096;
  stacks[0].cacheSize = 128;

  settings.threadsCount = os::Thread::getHardwareConcurrency();

  settings.jobAllocatorsSettings = allocators;
  settings.jobAllocatorSettingsCount = 1;

  settings.jobQueueSettings = queues;
  settings.jobQueueSettingsCount = 1;

  settings.jobStackSettings = stacks;
  settings.jobStackSettingsCount = 1;

  jobsystem::JobSystem::init(entry, &settings);
  os::print("shuting down...\n");
  jobsystem::JobSystem::shutdown();

  return 0;
}