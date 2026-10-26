#include "AsyncManager.hpp"
#include "os/hqos.hpp"

using namespace async;
using namespace async::fiber;
using namespace async::detail;

std::vector<os::Thread> AsyncManager::workerThreads;
lib::ConcurrentShardedQueue<Job *> AsyncManager::jobQueue;
JobAllocator *AsyncManager::jobAllocator;
uint64_t AsyncManager::pendingQueueIndex;
std::atomic<bool> AsyncManager::isRunning(false);
std::vector<JobQueueInfo> AsyncManager::jobQueuesInfo;

void AsyncManager::init(void (*entry)(), SystemSettings settings)
{
  jobAllocator = new JobAllocator(settings.stackSize, settings.jobsCapacity + 1, settings.jobsCapacity + 1);

  auto workerJob = Job::currentThreadToJob();
  workerJob->ref();

  jobAllocator->initializeThread();

  isRunning = true;
  std::atomic<uint64_t> initializing(0);

  auto threadInitialization = [&settings, &initializing]()
  {
    jobAllocator->initializeThread();

    // Add elements to queue cache cache
    Job *n;
    for (uint32_t i = 0; i < settings.jobsCapacity; i++)
    {
      jobQueue.enqueue(nullptr);
    }
    for (uint32_t i = 0; i < settings.jobsCapacity; i++)
    {
      while (!jobQueue.dequeue(n))
      {
      }
      assert(n == nullptr);
    }
    initializing.fetch_add(1);
    while (initializing.load() != settings.threadsCount)
    {
    }
  };

  for (size_t i = 0; i < settings.threadsCount - 1; ++i)
  {
    workerThreads.emplace_back(
        [&threadInitialization]()
        {
          os::hqos::setHighQos();
          threadInitialization();

          auto workerJob = Job::currentThreadToJob();
          workerJob->ref();

          auto j = jobAllocator->allocate(
              [](void *data, fiber::Fiber *self)
              {
                workerLoop();
              });

          j->manager = workerJob;
          j->resume();

          workerJob->deref();
          jobAllocator->deinitializeThread();
        });

    workerThreads.back().setAffinity(i % os::Thread::getHardwareConcurrency());
  }
  os::hqos::setHighQos();
  threadInitialization();

  enqueue(entry);

  auto j = jobAllocator->allocate(
      [](void *data, fiber::Fiber *self)
      {
        workerLoop();
      });

  j->ref();

  j->manager = workerJob;
  j->resume();

  for (uint32_t i = 0; i < settings.threadsCount - 1; ++i)
  {
    if (workerThreads[i].isRunning())
    {
      workerThreads[i].join();
    }
  }

  j->deref();
  workerJob->deref();
  jobAllocator->deinitializeThread();

  delete jobAllocator;
}

void AsyncManager::stop()
{
  isRunning.store(false);
}

void AsyncManager::shutdown()
{
  for (auto &t : workerThreads)
  {
    t.join();
  }

  // for (uint32_t i = 0; i < jobAllocators.size(); i++)
  // {
  //   delete jobAllocators[i];
  //   jobAllocators[i] = nullptr;
  // }

  // jobAllocator.clear();

  // for (uint32_t i = 0; i < jobQueues.size(); i++)
  // {
  //   delete jobQueues[i];
  //   jobQueues[i] = nullptr;
  // }

  // jobQueues.clear();

  // for (uint32_t i = 0; i < pools.size(); i++)
  // {
  //   delete pools[i];
  //   pools[i] = nullptr;
  // }
  // pools.clear();

#ifdef ASYNC_MANAGER_LOG_TIMES
  async::profiling::report();
#endif
}

void AsyncManager::processYieldedJobs()
{
}

void AsyncManager::workerLoop()
{
  auto workerJob = Job::currentJob; // jobAllocator->currentThreadToJob();
  auto threadJob = workerJob->manager;

  // os::print("%u worker start %p %p\n", os::Thread::getCurrentThreadId(), workerJob, &workerJob->fiber);

  while (AsyncManager::isRunning)
  {
#ifdef ASYNC_MANAGER_LOG_TIMES
    async::profiling::ScopedTimer timer(async::profiling::gStats.workerLoop);
#endif
    Job *job = nullptr;

    if (jobQueue.dequeue(job))
    {
      assert(job != nullptr);

      assert(Fiber::current() == &workerJob->fiber);

      job->manager = workerJob;

      job->resume();

      job->manager = nullptr;

      assert(Fiber::current() == &workerJob->fiber);

      if (job->waiting != nullptr)
      {
        Job *waiting = job->waiting;
        job->waiting = nullptr;

        if (!waiting->setWaiter(job))
        {
          assert(job != nullptr);
          jobQueue.enqueue(job);
        }
      }
      else if (job->yielding)
      {
        job->yielding = false;

        assert(job != workerJob);
        assert(job != nullptr);
        jobQueue.enqueue(job);
      }
      else
      {
        // os::print("%u processed yield jobs\n", os::Thread::getCurrentThreadId());

        if (job->isFinished())
        {
          thread_local bool isMarked = false;

          async::Job *waiter = job->waiter.read(isMarked);

          if (waiter)
          {
            assert(waiter != nullptr);
            // os::print("%u enqueueing waiter %p %p\n", os::Thread::getCurrentThreadId(), waiter, &waiter->fiber);
            jobQueue.enqueue(waiter);
          }

          job->deref(1, "finished");
        }
      }
    }
  }

  threadJob->resume();
}

void AsyncManager::yield()
{
  Job::currentJob->yielding = true;
  Job::currentJob->manager->resume();
}

void AsyncManager::sleepAndWakeOnPromiseResolve(Job *job)
{
  assert(Job::currentJob != Job::currentJob->manager);

  Job::currentJob->waiting = job;

  job->ref();
  Job::currentJob->manager->resume();
  job->deref();

  // Job::currentJob->waiting = nullptr;

  assert(&Job::currentJob->fiber == Fiber::current());
}

void AsyncManager::fiberEntry(void *data, fiber::Fiber *)
{
#ifdef ASYNC_MANAGER_LOG_TIMES
  async::profiling::ScopedTimer t(async::profiling::gStats.jobExecution);
#endif

  auto *job = static_cast<Job *>(data);
  auto *jobData = job->jobData;

  jobData->invoke(jobData);

  job->resolve();
}
