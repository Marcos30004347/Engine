#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

// static thread_local fiber::Fiber *workerFiber = nullptr;
thread_local Job *JobSystem::workerJob = nullptr;
thread_local Job *JobSystem::currentJob = nullptr;
thread_local Job *JobSystem::yieldedJob = nullptr;
thread_local Job *JobSystem::runningJob = nullptr;
thread_local Job *JobSystem::waitedJob = nullptr;
thread_local uint64_t JobSystem::waitingTime;

// static thread_local uint64_t tickCount;

std::vector<std::thread> JobSystem::workerThreads;
std::vector<lib::ConcurrentQueue<Job *> *> JobSystem::jobQueues;
std::vector<JobAllocator *> JobSystem::jobAllocators;
std::vector<FiberPool *> JobSystem::pools;
uint64_t JobSystem::pendingQueueIndex;
std::atomic<bool> JobSystem::isRunning(false);
std::vector<JobQueueInfo> JobSystem::jobQueuesInfo;
lib::ConcurrentPriorityQueue<Job *, uint64_t> *JobSystem::waitingQueue = nullptr;

void JobSystem::init(void (*entry)(), JobSystemSettings *settings)
{
  isRunning.store(false);

  for (size_t i = 0; i < settings->jobStackSettingsCount; i++)
  {
    pools.push_back(new FiberPool(settings->jobStackSettings[i].stackSize));
  }

  for (size_t i = 0; i < settings->jobAllocatorSettingsCount; i++)
  {
    jobAllocators.push_back(new JobAllocator(settings->jobAllocatorsSettings[i].payloadSize, settings->jobAllocatorsSettings[i].capacity + (i == 0 ? 1 : 0)));
  }

  for (size_t i = 0; i < settings->jobQueueSettingsCount; i++)
  {
    jobQueuesInfo.push_back(JobQueueInfo());

    // jobQueuesInfo[jobQueuesInfo.size() - 1].dequeuesInCurrentTick.store(0);
    // jobQueuesInfo[jobQueuesInfo.size() - 1].maxExecutionsBeforeReset = settings->jobQueueSettings[i].maxExecutionsBeforeReset;

    jobQueues.push_back(new lib::ConcurrentQueue<Job *>());
  }

  pendingQueueIndex = jobQueues.size();
  jobQueues.push_back(new lib::ConcurrentQueue<Job *>());

  waitingQueue = new lib::ConcurrentPriorityQueue<Job *, uint64_t>();

  isRunning = true;

  JobEnqueueData data;

  data.allocatorIndex = 0;
  data.queueIndex = pendingQueueIndex;
  data.stackSize = 1024 * 1024;

  os::print("enqueuing entry:\n");
  enqueue(&data, entry);
  os::print("enqueued entry\n");

  for (size_t i = 0; i < settings->threadsCount; ++i)
  {
    workerThreads.emplace_back(workerLoop);
  }

  // ticks.store(0);

  // tick();

  workerLoop();

  os::print("joining\n");

  for (uint32_t i = 0; i < settings->threadsCount; ++i)
  {
    if (workerThreads[i].joinable())
    {
      workerThreads[i].join();
    }
  }

  os::print("all joined\n");
}

// void JobSystem::tick()
// {
//   ticks.fetch_add(1);
// }

void JobSystem::stop()
{
  isRunning.store(false);
}

void JobSystem::shutdown()
{
  os::print("shutting down\n");

  // assert(currentJob == nullptr);
  // assert(workerJob == nullptr);
  // assert(yieldedJob == nullptr);
  // assert(runningJob == nullptr);

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
    runningJob->lock();

    if (runningJob->finished.load())
    {
      assert(yieldedJob != workerJob);

      os::print("enqueuing %p because cant wait %p\n", yieldedJob, runningJob);

      yieldedJob->refInQueue();
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
    assert(yieldedJob != workerJob);
    yieldedJob->refInQueue();
    jobQueues[pendingQueueIndex]->enqueue(yieldedJob);
  }

  yieldedJob = nullptr;

  if (waitedJob)
  {
    assert(waitingTime != UINT64_MAX);
    assert(waitedJob != workerJob);

    waitedJob->refInQueue();
    waitingQueue->enqueue(waitedJob, waitingTime);
    waitingTime = UINT64_MAX;
  }

  waitedJob = nullptr;
}

void JobSystem::workerLoop()
{
  workerJob = jobAllocators[0]->currentThreadToJob();
  workerJob->refInRuntime();

  while (JobSystem::isRunning)
  {
    uint64_t priority, dequeuedPriority;

    Job *nextJob = nullptr;

    if (waitingQueue->tryDequeue(nextJob, dequeuedPriority))
    {
      if (dequeuedPriority > lib::time::TimeSpan::now().nanoseconds())
      {
        waitingQueue->enqueue(nextJob, dequeuedPriority);
      }
      else
      {
        nextJob->refInRuntime();
        nextJob->derefInQueue();

        currentJob = nextJob;

        nextJob->resume();

        assert(currentJob == nextJob);

        if (nextJob->finished.load())
        {
          pools[nextJob->fiberPoolIndex]->release(nextJob->fiber);
          nextJob->fiber = nullptr;
        }

        processYieldedJobs();

        nextJob->derefInRuntime();
      }
    }

    for (size_t i = 0; i < jobQueues.size(); i++)
    {
      Job *nextJob = nullptr;

      while (jobQueues[i]->tryDequeue(nextJob))
      {
        nextJob->refInRuntime();
        nextJob->derefInQueue();

        assert(nextJob->fiber != nullptr);

        os::print(
            "thread %u executing %p, p=%u q=%u r=%u\n",
            os::Thread::getCurrentThreadId(),
            nextJob,
            nextJob->refsInPromises.load(),
            nextJob->refsInQueues.load(),
            nextJob->refsInRuntime.load());

        currentJob = nextJob;
        nextJob->resume();

        assert(currentJob == nextJob);

        if (nextJob->finished.load())
        {
          pools[nextJob->fiberPoolIndex]->release(nextJob->fiber);
          nextJob->fiber = nullptr;
        }

        processYieldedJobs();

        os::print("worker runtime refs of %p p=%u q=%u r=%u\n", nextJob, nextJob->refsInPromises.load(), nextJob->refsInQueues.load(), nextJob->refsInRuntime.load());

        currentJob->derefInRuntime();
      }
    }
  }

  workerJob->derefInRuntime();
}

void JobSystem::yield()
{
  Job *curr = currentJob;
  yieldedJob = currentJob;
  workerJob->resume();
  currentJob = curr;
}

void JobSystem::sleepAndWakeOnPromiseResolve(Job *&job)
{
  if (job->finished.load())
  {
    return;
  }
  // uint32_t jobCurrent = job->refInRuntime.load();

  // do {
  // } while();

  runningJob = job;
  yieldedJob = currentJob;
  workerJob->resume();
}

void JobSystem::delay(lib::time::TimeSpan span)
{
  waitingTime = lib::time::TimeSpan::now().nanoseconds() + span.nanoseconds();

  waitedJob = currentJob;

  workerJob->resume();
}