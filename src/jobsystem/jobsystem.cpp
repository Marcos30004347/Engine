#include "jobsystem.hpp"

using namespace jobsystem;
using namespace jobsystem::fiber;

void JobSystem::init(size_t numThreads)
{
    running_ = true;

    printf("Num threads = %u\n", numThreads);

    for (size_t i = 0; i < numThreads; ++i)
    {
        workers_.emplace_back(worker_loop);
    }
}

void JobSystem::shutdown()
{
    running_ = false;
    cv_.notify_all();

    for (auto &w : workers_)
    {
        if (w.joinable())
            w.join();
    }

    workers_.clear();
}

void JobSystem::worker_loop()
{
    while (running_)
    {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, []
                     { return !tasks_.empty() || !running_; });

            if (!running_ && tasks_.empty())
                return;

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}

void JobSystem::yield() {
    fiber::Fiber* current = fiber::Fiber::current();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.emplace([current]() {
            current->resume();
        });
    }

    cv_.notify_one();
    fiber::Fiber::yield();
}

void JobSystem::delay(uint64_t ms) {
    fiber::Fiber* current = Fiber::current();
    auto wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        delayed_tasks_.push({ wake, current });
    }

    cv_.notify_one();
    Fiber::yield(); 
}

void JobSystem::timer_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto now = std::chrono::steady_clock::now();
        std::vector<Fiber*> ready;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            while (!delayed_tasks_.empty() && delayed_tasks_.top().wake_time <= now) {
                ready.push_back(delayed_tasks_.top().fiber);
                delayed_tasks_.pop();
            }
        }

        if (!ready.empty()) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            for (Fiber* f : ready) {
                tasks_.emplace([f]() { f->resume(); });
            }
            cv_.notify_all();
        }
    }
}