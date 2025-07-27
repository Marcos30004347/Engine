#include "async/AsyncManager.hpp"
#include "lib/time/TimeSpan.hpp"

async::Job *mainJob;
async::Job *funcJob;

lib::time::TimeSpan prev;
lib::time::TimeSpan totalTime;

int counter = 0;

int add1(int i)
{
  return i + 1;
}

int add3(int i)
{
  async::AsyncEnqueueData data;
  data.allocatorIndex = 0;
  data.queueIndex = 0;
  data.stackSize = async::AsyncEnqueueData::getMinStackSize();

  async::Promise<int> promise = async::AsyncManager::enqueue(&data, add1, i);
  return async::AsyncManager::wait(promise) + 2;
}

void entry()
{
  size_t count = 128;
  async::Promise<int> promises[count];

  for (size_t iter = 0; iter < 1000; iter++)
  {
    async::AsyncEnqueueData data;

    data.allocatorIndex = 0;
    data.queueIndex = 0;
    data.stackSize = async::AsyncEnqueueData::getMinStackSize();

    for (int i = 0; i < count; i++)
    {
      promises[i] = async::AsyncManager::enqueue(&data, add3, i);
    }

    for (int i = 0; i < count; i++)
    {
      int &v = async::AsyncManager::wait(promises[i]);
      assert(v == i + 3);
    }
  }

  async::AsyncManager::stop();
}

int main()
{
  // async::JobAllocator *allocator = new async::JobAllocator(sizeof(uint8_t) * 4096, 4096);

  async::SystemSettings settings;

  async::AsyncAllocatorSettings allocators[1];
  async::AsyncQueueSettings queues[1];
  async::AsyncStackSettings stacks[1];

  allocators[0].capacity = 256;
  allocators[0].payloadSize = sizeof(uint32_t) + 64;

  stacks[0].stackSize = async::AsyncEnqueueData::getMinStackSize();
  stacks[0].cacheSize = 256;

  settings.threadsCount = os::Thread::getHardwareConcurrency();

  settings.jobAllocatorsSettings = allocators;
  settings.jobAllocatorSettingsCount = 1;

  settings.jobQueueSettings = queues;
  settings.jobQueueSettingsCount = 1;

  settings.jobStackSettings = stacks;
  settings.jobStackSettingsCount = 1;

  async::AsyncManager::init(entry, &settings);
  async::AsyncManager::shutdown();

  return 0;
}