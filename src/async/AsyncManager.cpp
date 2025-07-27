#include "AsyncManager.hpp"

using namespace async;
using namespace async::fiber;

// static thread_local fiber::Fiber *workerFiber = nullptr;
thread_local Job *AsyncManager::workerJob = nullptr;
thread_local Job *AsyncManager::currentJob = nullptr;
thread_local Job *AsyncManager::yieldedJob = nullptr;
thread_local Job *AsyncManager::runningJob = nullptr;
thread_local Job *AsyncManager::waitedJob = nullptr;
thread_local uint64_t AsyncManager::waitingTime;

// static thread_local uint64_t tickCount;

std::vector<os::Thread> AsyncManager::workerThreads;
std::vector<lib::ConcurrentQueue<Job *> *> AsyncManager::jobQueues;
std::vector<JobAllocator *> AsyncManager::jobAllocators;
std::vector<FiberPool *> AsyncManager::pools;
uint64_t AsyncManager::pendingQueueIndex;
std::atomic<bool> AsyncManager::isRunning(false);
std::vector<JobQueueInfo> AsyncManager::jobQueuesInfo;
lib::ConcurrentPriorityQueue<Job *, uint64_t> *AsyncManager::waitingQueue = nullptr;

void AsyncManager::init(void (*entry)(), SystemSettings *settings)
{
  isRunning.store(false);

  Fiber::initializeSubSystems();

  for (size_t i = 0; i < settings->jobStackSettingsCount; i++)
  {
    pools.push_back(new FiberPool(settings->jobStackSettings[i].stackSize, settings->jobStackSettings[i].cacheSize, settings->threadsCount));
  }

  for (size_t i = 0; i < settings->jobAllocatorSettingsCount; i++)
  {
    jobAllocators.push_back(new JobAllocator(settings->jobAllocatorsSettings[i].payloadSize, settings->jobAllocatorsSettings[i].capacity + (i == 0 ? 1 : 0)));
  }

  for (size_t i = 0; i < settings->jobQueueSettingsCount; i++)
  {
    jobQueuesInfo.push_back(JobQueueInfo());
    jobQueues.push_back(new lib::ConcurrentQueue<Job *>());
  }

  pendingQueueIndex = jobQueues.size();
  jobQueues.push_back(new lib::ConcurrentQueue<Job *>());

  waitingQueue = new lib::ConcurrentPriorityQueue<Job *, uint64_t>();

  isRunning = true;

  AsyncEnqueueData data;

  data.allocatorIndex = 0;
  data.queueIndex = pendingQueueIndex;
  data.stackSize = settings->jobStackSettings[0].stackSize;

  for (size_t i = 0; i < settings->threadsCount - 1; ++i)
  {
    workerThreads.emplace_back(
        []()
        {
          for (size_t i = 0; i < pools.size(); i++)
          {
            pools[i]->initializeThread();
          }

          for (size_t i = 0; i < jobAllocators.size(); i++)
          {
            jobAllocators[i]->initializeThread();
          }

          workerLoop();

          for (size_t i = 0; i < pools.size(); i++)
          {
            pools[i]->deinitializeThread();
          }

          for (size_t i = 0; i < jobAllocators.size(); i++)
          {
            jobAllocators[i]->deinitializeThread();
          }
        });

    workerThreads.back().setAffinity(i % os::Thread::getHardwareConcurrency());
  }

  for (size_t i = 0; i < pools.size(); i++)
  {
    pools[i]->initializeThread();
  }

  for (size_t i = 0; i < jobAllocators.size(); i++)
  {
    jobAllocators[i]->initializeThread();
  }

  enqueue(&data, entry);
  workerLoop();

  for (size_t i = 0; i < pools.size(); i++)
  {
    pools[i]->deinitializeThread();
  }

  for (size_t i = 0; i < jobAllocators.size(); i++)
  {
    jobAllocators[i]->deinitializeThread();
  }

  for (uint32_t i = 0; i < settings->threadsCount - 1; ++i)
  {
    if (workerThreads[i].isRunning())
    {
      workerThreads[i].join();
    }
  }

  Fiber::deinitializeSubSystems();
}

void AsyncManager::stop()
{
  isRunning.store(false);
}

void AsyncManager::shutdown()
{
  workerThreads.clear();

  for (uint32_t i = 0; i < jobAllocators.size(); i++)
  {
    delete jobAllocators[i];
    jobAllocators[i] = nullptr;
  }

  jobAllocators.clear();

  for (uint32_t i = 0; i < jobQueues.size(); i++)
  {
    delete jobQueues[i];
    jobQueues[i] = nullptr;
  }

  jobQueues.clear();

  for (uint32_t i = 0; i < pools.size(); i++)
  {
    delete pools[i];
    pools[i] = nullptr;
  }
  pools.clear();
}

void AsyncManager::processYieldedJobs()
{
  if (runningJob)
  {
    assert(yieldedJob != nullptr);
    assert(yieldedJob == currentJob);

    runningJob->lock();

    if (runningJob->finished.load())
    {
      assert(yieldedJob != workerJob);
      jobQueues[pendingQueueIndex]->enqueue(yieldedJob);
    }
    else
    {
      runningJob->setWaiter(yieldedJob);
    }

    runningJob->unlock();

    runningJob = nullptr;
    yieldedJob = nullptr;
  }

  if (yieldedJob != nullptr)
  {
    assert(yieldedJob == currentJob);
    assert(yieldedJob != workerJob);

    jobQueues[pendingQueueIndex]->enqueue(yieldedJob);
  }

  yieldedJob = nullptr;

  if (waitedJob)
  {
    assert(waitingTime != UINT64_MAX);
    assert(waitedJob != workerJob);

    waitedJob->ref();
    waitingQueue->enqueue(waitedJob, waitingTime);
    waitingTime = UINT64_MAX;
  }

  waitedJob = nullptr;
}

void AsyncManager::workerLoop()
{
  workerJob = jobAllocators[0]->currentThreadToJob();
  workerJob->ref();

  while (AsyncManager::isRunning)
  {
    // uint64_t priority, dequeuedPriority;

    // Job *nextJob = nullptr;

    // if (waitingQueue->tryDequeue(nextJob, dequeuedPriority))
    // {
    //   if (dequeuedPriority > lib::time::TimeSpan::now().nanoseconds())
    //   {
    //     waitingQueue->enqueue(nextJob, dequeuedPriority);
    //   }
    //   else
    //   {
    //     nextJob->refInRuntime();
    //     nextJob->derefInQueue();

    //     currentJob = nextJob;

    //     nextJob->resume();

    //     assert(currentJob == nextJob);

    //     if (nextJob->finished.load())
    //     {
    //       pools[nextJob->fiberPoolIndex]->release(nextJob->fiber);
    //       nextJob->fiber = nullptr;
    //     }

    //     processYieldedJobs();

    //     nextJob->derefInRuntime();
    //   }
    // }
    // os::print("Thread %u wf=%p cf=%p, &cf=%p\n", os::Thread::getCurrentThreadId(), workerJob->fiber, fiber::Fiber::current(), 0);

    for (size_t i = 0; i < jobQueues.size(); i++)
    {
      currentJob = nullptr;

      while (jobQueues[i]->tryDequeue(currentJob))
      {
        assert(currentJob->fiber != nullptr);

        currentJob->resume();

        if (currentJob->finished.load())
        {
          pools[currentJob->fiberPoolIndex]->release((Fiber *)currentJob->fiber);
          currentJob->fiber = nullptr;
          currentJob->deref("finished job");
        }

        processYieldedJobs();
        currentJob = nullptr;
      }
    }
  }

  pools[0]->release((Fiber *)workerJob->fiber);
  // os::print("worker job ref = %u\n", workerJob->refs.load());
  workerJob->deref("worker");
}

void AsyncManager::yield()
{
  Job *curr = currentJob;
  yieldedJob = currentJob;
  workerJob->resume();
  currentJob = curr;
}

void AsyncManager::sleepAndWakeOnPromiseResolve(Job *job)
{
  if (job->finished.load())
  {
    return;
  }

  runningJob = job;
  yieldedJob = currentJob;

  assert(currentJob->fiber == Fiber::current());
  workerJob->resume();
}

void AsyncManager::delay(lib::time::TimeSpan span)
{
  waitingTime = lib::time::TimeSpan::now().nanoseconds() + span.nanoseconds();

  waitedJob = currentJob;

  workerJob->resume();
}