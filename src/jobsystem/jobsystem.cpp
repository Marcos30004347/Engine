#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

// static thread_local fiber::Fiber *workerFiber = nullptr;
thread_local std::shared_ptr<Job> JobSystem::workerJob = nullptr;
thread_local std::shared_ptr<Job> JobSystem::currentJob = nullptr;
thread_local std::shared_ptr<Job> JobSystem::yieldedJob = nullptr;
thread_local std::shared_ptr<Job> JobSystem::runningJob = nullptr;
thread_local std::shared_ptr<Job> JobSystem::waitedJob = nullptr;
thread_local uint64_t JobSystem::waitingTime;

// static thread_local uint64_t tickCount;

std::vector<std::thread> JobSystem::workerThreads;
std::vector<lib::ConcurrentQueue<std::shared_ptr<Job>> *> JobSystem::jobQueues;
std::vector<JobAllocator *> JobSystem::jobAllocators;
std::vector<FiberPool *> JobSystem::pools;
uint64_t JobSystem::pendingQueueIndex;
std::atomic<bool> JobSystem::isRunning(false);
std::vector<JobQueueInfo> JobSystem::jobQueuesInfo;
lib::ConcurrentPriorityQueue<std::shared_ptr<Job>, uint64_t> *JobSystem::waitingQueue = nullptr;

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

    jobQueues.push_back(new lib::ConcurrentQueue<std::shared_ptr<Job>>());
  }

  pendingQueueIndex = jobQueues.size();
  jobQueues.push_back(new lib::ConcurrentQueue<std::shared_ptr<Job>>());

  waitingQueue = new lib::ConcurrentPriorityQueue<std::shared_ptr<Job>, uint64_t>();

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

    if (!runningJob->setWaiter(yieldedJob))
    {
      os::print("enqueuing %p because cant wait %p\n", yieldedJob.get(), runningJob.get());
      assert(yieldedJob.get() != workerJob.get());
      jobQueues[pendingQueueIndex]->enqueue(yieldedJob);
    }

    runningJob = nullptr;
    yieldedJob = nullptr;
  }

  if (yieldedJob != nullptr)
  {
    assert(yieldedJob.get() != workerJob.get());
    jobQueues[pendingQueueIndex]->enqueue(yieldedJob);
  }

  yieldedJob = nullptr;

  if (waitedJob)
  {
    assert(waitingTime != UINT64_MAX);

    assert(waitedJob.get() != workerJob.get());

    waitingQueue->enqueue(waitedJob, waitingTime);
    waitingTime = UINT64_MAX;
  }

  waitedJob = nullptr;
}

void JobSystem::workerLoop()
{
  workerJob = jobAllocators[0]->currentThreadToJob();

  while (JobSystem::isRunning)
  {
    // uint64_t priority, dequeuedPriority;

    // if (waitingQueue->tryPeek(priority))
    // {
    //   if (priority <= lib::time::TimeSpan::now().nanoseconds())
    //   {
    //     waitingQueue->dequeue(currentJob, dequeuedPriority);
    //     if (dequeuedPriority > lib::time::TimeSpan::now().nanoseconds())
    //     {
    //       waitingQueue->enqueue(currentJob, dequeuedPriority);
    //       currentJob = nullptr;
    //     }
    //     else
    //     {
    //       currentJob->resume();
    //       processYieldedJobs();
    //       currentJob = nullptr;
    //     }
    //   }
    // }

    for (size_t i = 0; i < jobQueues.size(); i++)
    {
      std::shared_ptr<Job> nextJob = nullptr;
      while (jobQueues[i]->tryDequeue(nextJob))
      {
        // os::print("p = %p\n", currentJob.get());
        assert(nextJob.use_count() >= 1);
        assert(nextJob.get()->fiber != nullptr);

        os::print("thread %u executing %p, refconut = %u\n", os::Thread::getCurrentThreadId(), nextJob.get(), nextJob.use_count());

        currentJob = nextJob;
        nextJob->resume();

        if (nextJob.get()->finished.load())
        {
          pools[nextJob.get()->fiberPoolIndex]->release(nextJob.get()->fiber);
          nextJob.get()->fiber = nullptr;
        }

        processYieldedJobs();

        assert(workerJob.use_count() == 1);
        assert(nextJob.use_count() >= 2);
      }
    }

    currentJob = nullptr;
  }

  // yieldedJob = nullptr;
  // runningJob = nullptr;
  // waitedJob = nullptr;
  workerJob = nullptr;
  // delete workerFiber;
}

void JobSystem::yield()
{
  std::shared_ptr<Job> curr = currentJob;

  yieldedJob = currentJob;
  workerJob->resume();
  // fiber::Fiber::switchTo(workerFiber);
  currentJob = curr;
}

void JobSystem::sleepAndWakeOnPromiseResolve(std::shared_ptr<Job> &job)
{
  std::shared_ptr<Job> curr = currentJob;
  // currentJob->setWaiter();
  runningJob = job;
  yieldedJob = currentJob;
  workerJob->resume();
  currentJob = curr;
}

void JobSystem::delay(lib::time::TimeSpan span)
{
  std::shared_ptr<Job> curr = currentJob;

  waitingTime = lib::time::TimeSpan::now().nanoseconds() + span.nanoseconds();
  waitedJob = currentJob;
  workerJob->resume();
  currentJob = curr;
}