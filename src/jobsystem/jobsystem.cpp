#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

// static thread_local fiber::Fiber *workerFiber = nullptr;
thread_local volatile Job *JobSystem::workerJob = nullptr;
thread_local volatile Job *JobSystem::currentJob = nullptr;
thread_local volatile Job *JobSystem::yieldedJob = nullptr;
thread_local volatile Job *JobSystem::runningJob = nullptr;
thread_local volatile Job *JobSystem::waitedJob = nullptr;
thread_local uint64_t JobSystem::waitingTime;

// static thread_local uint64_t tickCount;

std::vector<os::Thread> JobSystem::workerThreads;
std::vector<lib::ConcurrentQueue<volatile Job *> *> JobSystem::jobQueues;
std::vector<JobAllocator *> JobSystem::jobAllocators;
std::vector<FiberPool *> JobSystem::pools;
uint64_t JobSystem::pendingQueueIndex;
std::atomic<bool> JobSystem::isRunning(false);
std::vector<JobQueueInfo> JobSystem::jobQueuesInfo;
lib::ConcurrentPriorityQueue<volatile Job *, uint64_t> *JobSystem::waitingQueue = nullptr;

void JobSystem::init(void (*entry)(), JobSystemSettings *settings)
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
    jobQueues.push_back(new lib::ConcurrentQueue<volatile Job *>());
  }

  pendingQueueIndex = jobQueues.size();
  jobQueues.push_back(new lib::ConcurrentQueue<volatile Job *>());

  waitingQueue = new lib::ConcurrentPriorityQueue<volatile Job *, uint64_t>();

  isRunning = true;

  JobEnqueueData data;

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

void JobSystem::stop()
{
  isRunning.store(false);
}

void JobSystem::shutdown()
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

void JobSystem::processYieldedJobs()
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

void JobSystem::workerLoop()
{
  workerJob = jobAllocators[0]->currentThreadToJob();
  workerJob->ref();

  while (JobSystem::isRunning)
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

  workerJob->deref("worker");
}

void JobSystem::yield()
{
  volatile Job *curr = currentJob;
  yieldedJob = currentJob;
  workerJob->resume();
  currentJob = curr;
}

void JobSystem::sleepAndWakeOnPromiseResolve(volatile Job *job)
{
  if (job->finished.load())
  {
    return;
  }


  runningJob = job;
  yieldedJob = currentJob;

  // if (currentJob->fiber != Fiber::current())
  // {
  //   // os::print("Thread %u inconsistent sleep\n", os::Thread::getCurrentThreadId());
  //   *(int *)(0) = 3;
  // }

  assert(currentJob->fiber == Fiber::current());
  workerJob->resume();
}

void JobSystem::delay(lib::time::TimeSpan span)
{
  waitingTime = lib::time::TimeSpan::now().nanoseconds() + span.nanoseconds();

  waitedJob = currentJob;

  workerJob->resume();
}