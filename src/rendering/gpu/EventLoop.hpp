#pragma once

#include "datastructure/ConcurrentQueue.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace rendering
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
public:
  struct ExecutionEntry
  {
    using CompletionCallback = std::function<void(Fence &)>;

    Fence fence{};
    CompletionCallback callback;
    std::atomic<ExecutionState> state{ExecutionState::PENDING};
    std::atomic<FenceStatus> finalStatus{FenceStatus::PENDING};

    ExecutionEntry(Fence f, CompletionCallback cb) : fence(std::move(f)), callback(std::move(cb))
    {
    }

    // Non-copyable/movable to ensure pointer stability
    ExecutionEntry(const ExecutionEntry &) = delete;
    ExecutionEntry &operator=(const ExecutionEntry &) = delete;
  };

  bool isDone() const
  {
    if (!entry_)
      return true; 

    ExecutionState state = entry_->state.load(std::memory_order_acquire);
    return state != ExecutionState::PENDING;
  }

private:
  std::shared_ptr<ExecutionEntry> entry_;
  friend class EventLoop<Fence>;

public:
  explicit AsyncEvent(std::shared_ptr<ExecutionEntry> entry) : entry_(std::move(entry))
  {
  }
  AsyncEvent() = default;

  bool isValid() const
  {
    return entry_ != nullptr;
  }

  FenceStatus checkStatus() const
  {
    if (!entry_)
      return FenceStatus::ERROR;

    // Fast path: check atomic state first
    ExecutionState state = entry_->state.load(std::memory_order_acquire);
    if (state == ExecutionState::COMPLETED)
    {
      return entry_->finalStatus.load(std::memory_order_acquire);
    }
    return (state == ExecutionState::CANCELLED) ? FenceStatus::ERROR : FenceStatus::PENDING;
  }

  const Fence *getFence() const
  {
    return entry_ ? &entry_->fence : nullptr;
  }
};

template <typename Fence> class EventLoop
{
public:
  using CompletionCallback = std::function<void(Fence &)>;
  using EntryType = typename AsyncEvent<Fence>::ExecutionEntry;

private:
  std::function<FenceStatus(Fence &)> getStatusFunc_;

  lib::ConcurrentQueue<std::shared_ptr<EntryType>> pendingQueue_;

  std::vector<std::shared_ptr<EntryType>> activeTasks_;

public:
  explicit EventLoop(std::function<FenceStatus(Fence &)> getStatusFunc) : getStatusFunc_(std::move(getStatusFunc))
  {
    activeTasks_.reserve(64); // Pre-allocate to avoid immediate resize
  }

  AsyncEvent<Fence> submit(Fence fence, CompletionCallback callback = nullptr)
  {
    auto entry = std::make_shared<EntryType>(std::move(fence), std::move(callback));

    pendingQueue_.enqueue(entry);

    return AsyncEvent<Fence>(entry);
  }

  void tick()
  {
    std::shared_ptr<EntryType> newEntry;
    while (pendingQueue_.dequeue(newEntry))
    {
      if (newEntry)
      {
        activeTasks_.push_back(std::move(newEntry));
      }
    }

    if (activeTasks_.empty())
      return;

    auto it = activeTasks_.begin();
    while (it != activeTasks_.end())
    {
      auto &entry = *it;
      ExecutionState currentState = entry->state.load(std::memory_order_acquire);

      if (currentState != ExecutionState::PENDING)
      {
        *it = std::move(activeTasks_.back());
        activeTasks_.pop_back();
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

        *it = std::move(activeTasks_.back());
        activeTasks_.pop_back();
      }
      else
      {
        ++it;
      }
    }
  }

  void blockUntil(AsyncEvent<Fence> *event)
  {
    if (!event->isValid())
      return;

    while (event->checkStatus() == FenceStatus::PENDING)
    {
      tick();
    }
  }
};

} // namespace rendering