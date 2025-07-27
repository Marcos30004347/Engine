#pragma once

#include "HazardPointer.hpp"
#include "algorithm/random.hpp"
#include "memory/allocator/SystemAllocator.hpp"
#include "os/Thread.hpp"
#include <atomic>
#include <cstddef>

namespace lib
{
namespace detail
{
template <typename K> class ConcurrentStack;
template <typename T> struct ConcurrentStackNode
{
public:
  friend class ConcurrentStack<T>;
  T value;
  std::atomic<ConcurrentStackNode<T> *> next;

  ConcurrentStackNode(const T &val) : value(val), next(nullptr)
  {
  }
  T &get()
  {
    return value;
  }
};

template <typename T, typename Allocator = memory::allocator::SystemAllocator<ConcurrentStackNode<T>>> class ConcurrentStackProducer
{
public:
  std::atomic<ConcurrentStackNode<T> *> head;
  std::atomic<int> size;
  // NOTE(marcos): maybe have a list of records and a ThreadLocalStorage to store thread records
  // so we dont need to achire them often. At destruction we iterate over the record list and release them.
  using HazardPointerManager = HazardPointer<2, ConcurrentStackNode<T>, Allocator>;
  using HazardPointerRecord = typename HazardPointerManager::Record;
  HazardPointerManager hazardAllocator;

  Allocator allocator;

  ConcurrentStackProducer(Allocator &allocator) : head(nullptr), allocator(allocator), size(0)
  {
  }
  ConcurrentStackProducer() : head(nullptr), allocator(), size(0)
  {
  }

  ~ConcurrentStackProducer()
  {
    ConcurrentStackNode<T> *curr = head.load();

    while (curr)
    {
      ConcurrentStackNode<T> *next = curr->next.load();
      delete curr;
      curr = next;
    }
  }

  ConcurrentStackNode<T> *push(const T &value)
  {
    ConcurrentStackNode<T> *newNode = new ConcurrentStackNode<T>(value);
    ConcurrentStackNode<T> *oldHead = nullptr;

    do
    {
      oldHead = head.load();
      newNode->next.store(oldHead);
    } while (!head.compare_exchange_weak(oldHead, newNode));

    size.fetch_add(1);

    return newNode;
  }

  bool tryPop(T &value)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    while (true)
    {
      ConcurrentStackNode<T> *oldHead = head.load();

      if (!oldHead)
      {
        hazardAllocator.release(rec);
        return false;
      }

      rec->assign(oldHead, 0);

      if (head.load() != oldHead)
      {
        continue;
      }

      ConcurrentStackNode<T> *newHead = oldHead->next.load();

      if (head.compare_exchange_strong(oldHead, newHead))
      {
        value = oldHead->value;
        rec->retire(oldHead);
        hazardAllocator.release(rec);
        size.fetch_sub(1);
        return true;
      }
    }
  }
};
} // namespace detail

template <typename T> class ConcurrentStack
{
  size_t concurrencyLevel;

public:
  ConcurrentStack() : time(0), threadLists(), localLists()
  {
    concurrencyLevel = os::Thread::getHardwareConcurrency();
  }

  ~ConcurrentStack()
  {
    detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *node = threadLists.head.load(std::memory_order_acquire);
    while (node)
    {
      detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *next = node->next.load(std::memory_order_acquire);
      delete node->get();
      node = next;
    }
  }

  void push(T value)
  {
    detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *local = nullptr;

    if (!localLists.get(local))
    {
      // NOTE: never delete local directly, it will be cleaned up on destruction.
      detail::ConcurrentStackProducer<T> *producer = new detail::ConcurrentStackProducer<T>();
      local = threadLists.push(producer);
      localLists.set(local);
    }

    assert(local != nullptr);

    local->get()->push(value);
  }

  bool tryPop(T &value)
  {
    detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *local = nullptr;

    localLists.get(local);

    if (local == nullptr)
    {
      local = threadLists.head.load(std::memory_order_acquire);
    }

    if (local == nullptr)
    {
      return false;
    }

    detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *node = local;
    detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *start = local;

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
    detail::ConcurrentStackProducer<T> *lists[candidatesMax];

    bool looping = false;

    for (size_t iter = 0; iter < 2 && listsCount < candidatesMax; iter++)
    {
      while (node && listsCount < candidatesMax)
      {
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

      if (node == nullptr && listsCount < candidatesMax)
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
      if (lists[i % listsCount]->tryPop(value))
      {
        return true;
      }
    }

    return false;
  }

private:
  ThreadLocalStorage<detail::ConcurrentStackNode<detail::ConcurrentStackProducer<T> *> *> localLists;
  detail::ConcurrentStackProducer<detail::ConcurrentStackProducer<T> *> threadLists;
  size_t time;
};

} // namespace lib
