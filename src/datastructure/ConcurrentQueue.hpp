#pragma once

#include "ConcurrentEpochGarbageCollector.hpp"
#include "algorithm/random.hpp"
#include "datastructure/ConcurrentLinkedList.hpp"

// #include "memory/allocator/SystemAllocator.hpp"
#include "os/Thread.hpp"
#include <atomic>
#include <cstddef>

#include <type_traits>

namespace lib
{

template <typename T, uint64_t CacheSize = 128> class ConcurrentQueue
{
private:
  struct Node
  {
    std::atomic<Node *> next;

    T value;

    template <typename U = T, std::enable_if_t<std::is_default_constructible_v<U>, int> = 0> Node() : next(nullptr)
    {
    }

    Node(const T &v) : next(nullptr), value(v)
    {
    }
  };

  ConcurrentEpochGarbageCollector<Node, CacheSize> garbageCollector;

  std::atomic<Node *> head;
  std::atomic<Node *> tail;
  std::atomic<uint32_t> size;

public:
  template <typename U = T, typename = std::enable_if_t<std::is_default_constructible_v<U>>> ConcurrentQueue() : size(0)
  {
    auto scope = garbageCollector.openEpochGuard();
    Node *dummy = garbageCollector.allocateUnitialized(scope);
    dummy->next.store(nullptr);
    head.store(dummy, std::memory_order_relaxed);
    tail.store(dummy, std::memory_order_relaxed);
  }

  template <typename Arg1, typename... Args, std::enable_if_t<std::is_constructible_v<T, Arg1, Args...>, int> = 0> explicit ConcurrentQueue(Arg1 &&arg1, Args &&...args) : size(0)
  {
    auto scope = garbageCollector.openEpochGuard();
    Node *dummy = garbageCollector.allocate(scope, std::forward<Arg1>(arg1), std::forward<Args>(args)...);
    dummy->next.store(nullptr);
    head.store(dummy, std::memory_order_relaxed);
    tail.store(dummy, std::memory_order_relaxed);
  }

  ~ConcurrentQueue()
  {
    clear();
  }

  void clear()
  {
    auto scope = garbageCollector.openEpochGuard();

    while (head.load())
    {
      Node *first = head.load(std::memory_order_acquire);
      Node *last = tail.load(std::memory_order_acquire);
      Node *next = first->next.load(std::memory_order_acquire);

      if (first == head.load(std::memory_order_acquire))
      {
        if (first == last)
        {
          if (next == nullptr)
          {
            scope.retire(first);
            return;
          }

          tail.compare_exchange_weak(last, next, std::memory_order_release, std::memory_order_relaxed);
        }
        else
        {
          if (head.compare_exchange_weak(first, next, std::memory_order_release, std::memory_order_relaxed))
          {
            scope.retire(first);
            size.fetch_sub(1);
          }
        }
      }
    }
  }

  void enqueue(const T &value)
  {
    auto scope = garbageCollector.openEpochGuard();
    Node *newNode = garbageCollector.allocate(scope, value);

    while (true)
    {
      Node *last = tail.load(std::memory_order_acquire);
      Node *next = last->next.load(std::memory_order_acquire);

      if (last == tail.load(std::memory_order_acquire))
      {
        if (next == nullptr)
        {
          if (last->next.compare_exchange_weak(next, newNode, std::memory_order_release, std::memory_order_relaxed))
          {
            tail.compare_exchange_weak(last, newNode, std::memory_order_release, std::memory_order_relaxed);
            size.fetch_add(1);
            return;
          }
        }
        else
        {
          tail.compare_exchange_weak(last, next, std::memory_order_release, std::memory_order_relaxed);
        }
      }
    }
  }

  bool dequeue(T &out)
  {
    auto scope = garbageCollector.openEpochGuard();
    while (true)
    {
      Node *first = head.load(std::memory_order_acquire);
      Node *last = tail.load(std::memory_order_acquire);
      Node *next = first->next.load(std::memory_order_acquire);

      if (first == head.load(std::memory_order_acquire))
      {
        if (first == last)
        {
          if (next == nullptr)
          {
            return false;
          }

          tail.compare_exchange_weak(last, next, std::memory_order_release, std::memory_order_relaxed);
        }
        else
        {
          if (head.compare_exchange_weak(first, next, std::memory_order_release, std::memory_order_relaxed))
          {
            out = std::move(next->value);
            scope.retire(first);
            size.fetch_sub(1);
            return true;
          }
        }
      }
    }
  }

  bool empty() const
  {
    Node *first = head.load(std::memory_order_acquire);
    Node *next = first->next.load(std::memory_order_acquire);
    return next == nullptr;
  }

  uint32_t length()
  {
    return size.load();
  }
};

template <typename T, uint64_t CacheSize = 128> class ConcurrentShardedQueue
{
public:
  ConcurrentShardedQueue() : threadLists(), localLists()
  {
  }

  ~ConcurrentShardedQueue()
  {
  }

  void enqueue(T value)
  {
    ConcurrentQueue<T, CacheSize> *local = nullptr;

    if (!localLists.get(local))
    {
      auto iter = threadLists.emplaceFront();
      localLists.set(&(iter.value()));
      local = &iter.value();
    }

    assert(local != nullptr);
    local->enqueue(value);
  }

  bool dequeue(T &value)
  {
    ConcurrentQueue<T, CacheSize> *local = nullptr;

    localLists.get(local);

    if (local != nullptr && local->dequeue(value))
    {
      return true;
    }

    for (auto &list : threadLists)
    {
      auto size = list.length();

      if (size > 0)
      {
        if (list.dequeue(value))
        {
          return true;
        }
      }
    }

    return false;
  }

private:
  ConcurrentLinkedList<ConcurrentQueue<T, CacheSize>> threadLists;
  ThreadLocalStorage<ConcurrentQueue<T, CacheSize> *> localLists;
};

} // namespace lib
