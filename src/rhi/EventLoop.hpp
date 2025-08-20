#pragma once

#include "datastructure/ConcurrentQueue.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace rhi
{

enum class FenceStatus
{
  PENDING = 0,
  FINISHED = 1,
  ERROR = 2
};

enum class ExecutionState : uint32_t
{
  PENDING = 0,
  COMPLETED = 1,
  CANCELLED = 2
};

template <typename Fence> class EventLoop;
template <typename Fence> class AsyncEvent
{
private:
  struct ExecutionEntry
  {
    using CompletionCallback = std::function<void(Fence &)>;

    Fence fence{};
    CompletionCallback callback;
    std::atomic<ExecutionState> state{ExecutionState::PENDING};
    std::atomic<FenceStatus> finalStatus{FenceStatus::PENDING};

    ExecutionEntry(Fence fence, CompletionCallback callback) : fence(std::move(fence)), callback(std::move(callback))
    {
    }

    ExecutionEntry(const ExecutionEntry &) = delete;
    ExecutionEntry &operator=(const ExecutionEntry &) = delete;
  };

  std::shared_ptr<ExecutionEntry> entry_;
  friend class EventLoop<Fence>;

  explicit AsyncEvent(std::shared_ptr<ExecutionEntry> entry) : entry_(std::move(entry))
  {
  }

public:
  AsyncEvent() = default;
  AsyncEvent(const AsyncEvent &) = default;
  AsyncEvent(AsyncEvent &&) = default;
  AsyncEvent &operator=(const AsyncEvent &) = default;
  AsyncEvent &operator=(AsyncEvent &&) = default;

  bool isValid() const
  {
    return entry_ != nullptr;
  }

  ExecutionState getState() const
  {
    if (!entry_)
      return ExecutionState::CANCELLED;
    return entry_->state.load(std::memory_order_acquire);
  }

  FenceStatus getFinalStatus() const
  {
    if (!entry_)
      return FenceStatus::ERROR;
    return entry_->finalStatus.load(std::memory_order_acquire);
  }

  FenceStatus checkStatus() const
  {
    if (!entry_)
      return FenceStatus::ERROR;

    ExecutionState state = entry_->state.load(std::memory_order_acquire);
    if (state == ExecutionState::COMPLETED)
    {
      return entry_->finalStatus.load(std::memory_order_acquire);
    }
    else if (state == ExecutionState::PENDING)
    {
      return FenceStatus::PENDING;
    }
    else // CANCELLED
    {
      return FenceStatus::ERROR;
    }
  }

  void cancel()
  {
    if (!entry_)
      return;

    ExecutionState expected = ExecutionState::PENDING;
    entry_->state.compare_exchange_strong(expected, ExecutionState::CANCELLED, std::memory_order_acq_rel);
  }

  template <typename EventLoopPtr> FenceStatus wait(EventLoopPtr &eventLoop) const
  {
    if (!entry_)
      return FenceStatus::ERROR;

    while (entry_->state.load(std::memory_order_acquire) == ExecutionState::PENDING)
    {
      eventLoop.tick();
    }

    return checkStatus();
  }

  const Fence *getFence() const
  {
    if (!entry_)
      return nullptr;
    return &entry_->fence;
  }
};

template <typename Fence> class EventLoop
{
public:
  using CompletionCallback = std::function<void(Fence &)>;

private:
  using ExecutionEntry = typename AsyncEvent<Fence>::ExecutionEntry;

  std::function<FenceStatus(Fence &)> getStatusFunc_;
  lib::ConcurrentQueue<std::shared_ptr<ExecutionEntry>> pendingQueue_;
  lib::ConcurrentQueue<std::shared_ptr<ExecutionEntry>> processingQueue_;

public:
  explicit EventLoop(std::function<FenceStatus(Fence &)> getStatusFunc) : getStatusFunc_(std::move(getStatusFunc))
  {
  }

  ~EventLoop()
  {
  }

  AsyncEvent<Fence> submit(Fence fence, CompletionCallback callback = nullptr)
  {
    auto entry = std::make_shared<ExecutionEntry>(std::move(fence), std::move(callback));

    pendingQueue_.enqueue(entry);
    return AsyncEvent<Fence>(entry);
  }

  void tick()
  {
    std::shared_ptr<ExecutionEntry> entry;
    while (processingQueue_.tryDequeue(entry))
    {
      if (!entry)
        continue;

      ExecutionState currentState = entry->state.load(std::memory_order_acquire);

      if (currentState != ExecutionState::PENDING)
      {
        continue;
      }

      FenceStatus status = getStatusFunc_(entry->fence);

      if (status != FenceStatus::PENDING)
      {
        ExecutionState expected = ExecutionState::PENDING;
        if (entry->state.compare_exchange_strong(expected, ExecutionState::COMPLETED, std::memory_order_acq_rel))
        {
          entry->finalStatus.store(status, std::memory_order_release);

          if (entry->callback)
          {
            entry->callback(entry->fence);
          }
        }
      }
      else
      {
        processingQueue_.enqueue(entry);
      }
    }

    while (pendingQueue_.tryDequeue(entry))
    {
      if (!entry)
        continue;

      ExecutionState currentState = entry->state.load(std::memory_order_acquire);

      if (currentState == ExecutionState::PENDING)
      {
        FenceStatus status = getStatusFunc_(entry->fence);

        if (status != FenceStatus::PENDING)
        {
          ExecutionState expected = ExecutionState::PENDING;
          if (entry->state.compare_exchange_strong(expected, ExecutionState::COMPLETED, std::memory_order_acq_rel))
          {
            entry->finalStatus.store(status, std::memory_order_release);

            if (entry->callback)
            {
              entry->callback(entry->fence);
            }
          }
        }
        else
        {
          processingQueue_.enqueue(entry);
        }
      }
    }
  }
};

} // namespace rhi