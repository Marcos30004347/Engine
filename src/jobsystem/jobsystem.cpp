#include "JobSystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

static thread_local fiber::Fiber *workerFiber = nullptr;
static thread_local std::shared_ptr<Job> workerJob = nullptr;
static thread_local std::shared_ptr<Job> currentJob = nullptr;

static thread_local std::shared_ptr<Job> yieldedJob = nullptr;
static thread_local std::shared_ptr<Job> runningJob = nullptr;

lib::Vector<std::thread> JobSystem::workerThreads;
// lib::ConcurrentQueue<std::shared_ptr<Job>> JobSystem::pendingJobs = lib::ConcurrentQueue<std::shared_ptr<Job>>();

JobQueue *JobSystem::pendingJobs = nullptr;
JobAllocator *JobSystem::jobsAllocator = nullptr;
std::atomic<bool> JobSystem::isRunning = false;

void JobSystem::init(void (*entry)(), JobSystemSettings settings)
{
  assert(settings.maxJobPayloadSize >= sizeof(Job) + 1 * sizeof(size_t));

  FiberPool::init();

  pendingJobs = new JobQueue(settings.maxJobsConcurrent);
  jobsAllocator = new JobAllocator(settings.maxJobPayloadSize, settings.maxJobsConcurrent);

  isRunning = true;
  enqueue(entry);

  for (size_t i = 0; i < settings.threadsCount; ++i)
  {
    workerThreads.emplaceBack(workerLoop);
  }

  workerLoop();
}

void JobSystem::stop()
{
  isRunning.store(false);
}

void JobSystem::shutdown()
{
  for (uint32_t i = 0; i < workerThreads.size(); i++)
  {
    workerThreads[i].join();
  }

  workerThreads.clear();

  delete jobsAllocator;
  delete pendingJobs;

  FiberPool::shutdown();
}

void JobSystem::workerLoop()
{
  workerFiber = fiber::Fiber::currentThreadToFiber();
  currentJob = jobsAllocator->allocate();

  new (currentJob.get()) Job(workerFiber);

  workerJob = currentJob;

  while (JobSystem::isRunning)
  {
    if (runningJob)
    {
      assert(yieldedJob != nullptr);

      if (!runningJob->addWaiter(yieldedJob))
      {
        pendingJobs->enqueue(yieldedJob);
      }

      yieldedJob = nullptr;
    }

    if (yieldedJob != nullptr)
    {
      pendingJobs->enqueue(yieldedJob);
    }

    yieldedJob = nullptr;

    std::shared_ptr<Job> job = pendingJobs->dequeue();

    if (job != nullptr)
    {
      currentJob = job;
      fiber::Fiber::switchTo(job->fiber);
    }
  }

  delete workerFiber;
}

void JobSystem::yield()
{
  yieldedJob = currentJob;
  fiber::Fiber::switchTo(workerFiber);
}

void JobSystem::sleepAndWakeOnPromiseResolve(std::shared_ptr<Job> job)
{
  runningJob = job;
  yieldedJob = currentJob;
  fiber::Fiber::switchTo(workerFiber);
}
