#pragma once

#include "lib/algorithm/random.hpp"
#include "lib/datastructure/ConcurrentLinkedList.hpp"
#include "lib/datastructure/utils/HazardPointer.hpp"
#include "lib/memory/allocator/SystemAllocator.hpp"
#include "os/Thread.hpp"
#include "utils/HazardPointer.hpp"
#include <atomic>
#include <cstddef>

namespace lib
{
namespace detail
{
template <typename K> class ConcurrentQueue;
template <typename T> struct ConcurrentQueueNode
{
public:
  friend class ConcurrentQueue<T>;
  T value;
  std::atomic<ConcurrentQueueNode<T> *> next;

  ConcurrentQueueNode(const T &val) : value(val), next(nullptr)
  {
  }
  T &get()
  {
    return value;
  }
};

template <typename T, typename Allocator = memory::allocator::SystemAllocator<ConcurrentQueueNode<T>>> class ConcurrentQueueProducer
{
public:
  std::atomic<ConcurrentQueueNode<T> *> head;
  std::atomic<ConcurrentQueueNode<T> *> tail;
  std::atomic<int> size;

  // NOTE(marcos): maybe have a list of records and a ThreadLocalStorage to store thread records
  // so we dont need to achire them often. At destruction we iterate over the record list and release them.
  HazardPointer<2> hazardAllocator;

  Allocator allocator;

  ConcurrentQueueProducer(Allocator &allocator) : head(nullptr), tail(nullptr), allocator(allocator), size(0)
  {
    head = new ConcurrentQueueNode<T>(T());
    tail = head.load();
  }

  ConcurrentQueueProducer() : head(nullptr), tail(nullptr), size(0)
  {
    head = new ConcurrentQueueNode<T>(T());
    tail = head.load();
  }

  ~ConcurrentQueueProducer()
  {
    ConcurrentQueueNode<T> *curr = head.load();

    while (curr)
    {
      ConcurrentQueueNode<T> *next = curr->next.load();
      delete curr;
      curr = next;
    }
  }

  void enqueue(const T &value)
  {
    ConcurrentQueueNode<T> *newNode = new ConcurrentQueueNode<T>(value);
    ConcurrentQueueNode<T> *oldHead = nullptr;
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();
    void *null = nullptr;

    while (true)
    {
      ConcurrentQueueNode<T> *currentTail = tail.load();

      rec->assign(currentTail, 0);

      if (tail.load() != currentTail)
      {
        continue;
      }

      ConcurrentQueueNode<T> *next = currentTail->next.load();

      if (tail.load() != currentTail)
      {
        continue;
      }

      if (next != nullptr)
      {
        tail.compare_exchange_strong(currentTail, next);
        continue;
      }

      ConcurrentQueueNode<T> *null = nullptr;

      if (currentTail->next.compare_exchange_strong(null, newNode))
      {
        break;
      }
    }

    size.fetch_add(1);

    rec->assign(null, 0);

    hazardAllocator.release(rec);
  }

  bool tryDequeue(T &value)
  {
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();

    void *null = nullptr;

    while (true)
    {
      ConcurrentQueueNode<T> *h = head.load();

      rec->assign(h, 0);

      if (head.load() != h)
      {
        continue;
      }

      ConcurrentQueueNode<T> *t = tail.load();
      ConcurrentQueueNode<T> *next = h->next.load();

      rec->assign(next, 1);

      if (head.load() != h)
      {
        continue;
      }

      if (next == nullptr)
      {
        return false;
      }

      if (h == t)
      {
        tail.compare_exchange_strong(t, next);
        continue;
      }

      value = next->get();

      if (head.compare_exchange_strong(h, next))
      {
        break;
      }
    }

    rec->retire<ConcurrentQueueNode<T>, Allocator>(allocator, 0);
    rec->assign(null, 1);

    hazardAllocator.release(rec);

    return true;
  }

  bool tryPop(T &value)
  {
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();
    void *nil = nullptr;

    while (true)
    {
      ConcurrentQueueNode<T> *oldHead = head.load();

      if (!oldHead)
      {
        rec->assign(nil, 0);

        hazardAllocator.release(rec);
        return false;
      }

      rec->assign(oldHead, 0);

      if (head.load() != oldHead)
      {
        continue;
      }

      ConcurrentQueueNode<T> *newHead = oldHead->next.load();

      if (head.compare_exchange_strong(oldHead, newHead))
      {
        value = oldHead->value;

        rec->retire<ConcurrentQueueNode<T>, Allocator>(allocator, 0);
        rec->assign(nil, 1);
        hazardAllocator.release(rec);
        size.fetch_sub(1);
        return true;
      }
    }

    return false;
  }
};
} // namespace detail

template <typename T> class ConcurrentQueue
{
  size_t concurrencyLevel;

public:
  ConcurrentQueue() : time(random(os::Thread::getCurrentThreadId())), threadLists(), localLists()
  {
    concurrencyLevel = os::Thread::getHardwareConcurrency();
  }

  ~ConcurrentQueue()
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *node = threadLists.head.load(std::memory_order_acquire);
    while (node)
    {
      detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *next = node->next.load(std::memory_order_acquire);
      delete node->get();
      node = next;
    }
  }

  void enqueue(T value)
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *local = nullptr;

    if (!localLists.get(local))
    {
      // NOTE: never delete local directly, it will be cleaned up on destruction.
      detail::ConcurrentQueueProducer<T> *producer = new detail::ConcurrentQueueProducer<T>();
      local = threadLists.insert(producer);
      localLists.set(local);
    }

    assert(local != nullptr);

    local->get()->enqueue(value);
  }

  bool tryDequeue(T &value)
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *local = nullptr;

    localLists.get(local);

    if (local == nullptr)
    {
      local = threadLists.head.load(std::memory_order_acquire);
    }

    if (local == nullptr)
    {
      return false;
    }

    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *node = local;
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *start = local;

    for (size_t i = 0; i < (time % concurrencyLevel); i++)
    {
      node = node->next.load(std::memory_order_relaxed);
      if (node == nullptr)
      {
        node = threadLists.head.load(std::memory_order_acquire);
      }
    }

    start = node;

    const size_t candidatesMax = 3;
    size_t listsCount = 0;
    detail::ConcurrentQueueProducer<T> *lists[candidatesMax];

    bool looping = false;

    size_t iters = 0;

    for (size_t iter = 0; iter < 2 && listsCount < candidatesMax; iter++)
    {
      while (node && listsCount < candidatesMax)
      {
        iters++;
        if (looping && node == start)
        {
          break;
        }

        auto size = node->get()->size.load();

        if (size > 0)
        {
          lists[listsCount++] = node->get();
        }

        node = node->next.load(std::memory_order_relaxed);
      }

      if (node == nullptr)
      {
        looping = true;
        node = threadLists.head.load(std::memory_order_relaxed);
      }
    }

    if (listsCount == 0)
    {
      return false;
    }

    time++;

    for (size_t i = 0; i < listsCount; i++)
    {
      if (lists[i]->tryDequeue(value))
      {
        return true;
      }
    }

    return false;
  }

private:
  ThreadLocalStorage<detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *> localLists;
  detail::ConcurrentLinkedList<detail::ConcurrentQueueProducer<T> *> threadLists;
  size_t time;
};

} // namespace lib
