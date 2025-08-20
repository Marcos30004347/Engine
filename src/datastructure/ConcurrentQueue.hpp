#pragma once

#include "algorithm/random.hpp"
#include "datastructure/ConcurrentLinkedList.hpp"
#include "datastructure/HazardPointer.hpp"
#include "memory/allocator/SystemAllocator.hpp"
#include "os/Thread.hpp"
#include <atomic>
#include <cstddef>

#include <type_traits>

// #include "async/Fiber.hpp"
// #include "async/AsyncManager.hpp"

template <typename T> struct is_shared_ptr : std::false_type
{
};

template <typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type
{
};

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

  using HazardPointerManager = HazardPointer<2, ConcurrentQueueNode<T>, Allocator>;
  using HazardPointerRecord = typename HazardPointerManager::Record;

  HazardPointerManager hazardAllocator;

  Allocator allocator;

  ConcurrentQueueProducer(Allocator &allocator) : head(nullptr), tail(nullptr), allocator(allocator), size(0), hazardAllocator(this->allocator)
  {
    head = new ConcurrentQueueNode<T>(T());
    tail = head.load();
  }

  ConcurrentQueueProducer() : head(nullptr), tail(nullptr), size(0), hazardAllocator()
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
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    ConcurrentQueueNode<T> *currentTail = nullptr;

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
        // if constexpr (is_shared_ptr<T>::value)
        // {
        //   os::print("enqueued %p in %p, %p\n", value.get(), newNode);
        // }
        break;
      }
    }

    tail.compare_exchange_strong(currentTail, newNode);

    size.fetch_add(1);

    hazardAllocator.release(rec);
  }

  bool tryDequeue(T &value)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    ConcurrentQueueNode<T> *h;

    while (true)
    {
      h = head.load();

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
        hazardAllocator.release(rec);

        return false;
      }

      if (h == t)
      {
        tail.compare_exchange_strong(t, next);
        continue;
      }

      value = std::move(next->get());

      // if constexpr (is_shared_ptr<T>::value)
      // {
      //   os::print("> got shared %p, from %p, %p -> %p\n", value.get(), this, h, next);
      // }

      if (head.compare_exchange_strong(h, next))
      {
        break;
      }
    }

    rec->retire(h);
    hazardAllocator.release(rec);
    size.fetch_sub(1);
    return true;
  }

  bool tryPop(T &value)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
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
        value = std::move(oldHead->value);

        rec->retire(oldHead);
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
  std::atomic<size_t> approximateQueueSize;

public:
  ConcurrentQueue() : time(random(os::Thread::getCurrentThreadId())), threadLists(), localLists(), approximateQueueSize(0)
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
  size_t getApproximateSize()
  {
    return approximateQueueSize.load();
  }
  void enqueue(T value)
  {
    // if constexpr (is_shared_ptr<T>::value)
    // {
    //   os::print("queue enqueuing %p\n", value.get());
    // }

    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *local = nullptr;

    if (!localLists.get(local))
    {
      // NOTE: never delete local directly, it will be cleaned up on destruction.
      detail::ConcurrentQueueProducer<T> *producer = new detail::ConcurrentQueueProducer<T>();
      local = threadLists.insert(producer);
      localLists.set(local);
    }

    assert(local != nullptr && local->get() != nullptr);

    approximateQueueSize.fetch_add(1);
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

    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *start = local;

    bool looping = false;

    for (size_t iter = 0; iter < 2; iter++)
    {
      while (local)
      {
        assert(local->get());

        if (looping && local == start)
        {
          break;
        }

        auto size = local->get()->size.load();

        if (size > 0)
        {
          if (local->get()->tryDequeue(value))
          {
            approximateQueueSize.fetch_sub(1);
            return true;
          }
        }

        local = local->next.load(std::memory_order_relaxed);
      }

      if (local == nullptr)
      {
        looping = true;
        local = threadLists.head.load(std::memory_order_relaxed);
      }
      // os::print("Thread %u wf=%p cf=%p, inside queue part 7\n", os::Thread::getCurrentThreadId(), async::AsyncManager::workerJob, async::fiber::Fiber::currentFiber);
    }

    // if (listsCount == 0)
    // {
    //   return false;
    // }

    // time++;

    // for (size_t i = 0; i < listsCount; i++)
    // {
    //   if (lists[i]->tryDequeue(value))
    //   {
    //     // if constexpr (std::is_pointer<T>::value)
    //     // {
    //     //   os::print("got %p\n", value);
    //     // }
    //     // if constexpr (is_shared_ptr<T>::value)
    //     // {
    //     //   os::print("got shared %p\n", value.get());
    //     // }
    //     return true;
    //   }
    // }

    return false;
  }

private:
  ThreadLocalStorage<detail::ConcurrentSingleLinkedListNode<detail::ConcurrentQueueProducer<T> *> *> localLists;
  detail::ConcurrentLinkedList<detail::ConcurrentQueueProducer<T> *> threadLists;
  size_t time;
};

} // namespace lib
