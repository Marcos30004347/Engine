#pragma once

#include "lib/algorithm/random.hpp"
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
template <typename K> class ConcurrentList;
template <typename T> struct ConcurrentSingleLinkedListNode
{
public:
  friend class ConcurrentList<T>;
  T value;
  std::atomic<ConcurrentSingleLinkedListNode<T> *> next;

  ConcurrentSingleLinkedListNode(const T &val) : value(val), next(nullptr)
  {
  }
  T &get()
  {
    return value;
  }
};

template <typename T, typename Allocator = memory::allocator::SystemAllocator<ConcurrentSingleLinkedListNode<T>>> class ConcurrentLinkedList
{
public:
  std::atomic<ConcurrentSingleLinkedListNode<T> *> head;
  std::atomic<int> size;

  // NOTE(marcos): maybe have a list of records and a ThreadLocalStorage to store thread records
  // so we dont need to achire them often. At destruction we iterate over the record list and release them.
  HazardPointer<2> hazardAllocator;

  Allocator allocator;

  ConcurrentLinkedList(Allocator &allocator) : head(nullptr), allocator(allocator), size(0)
  {
  }
  ConcurrentLinkedList() : head(nullptr), allocator(), size(0)
  {
  }

  ~ConcurrentLinkedList()
  {
    ConcurrentSingleLinkedListNode<T> *curr = head.load();

    while (curr)
    {
      ConcurrentSingleLinkedListNode<T> *next = curr->next.load();
      delete curr;
      curr = next;
    }
  }

  ConcurrentSingleLinkedListNode<T> *insert(const T &value)
  {
    ConcurrentSingleLinkedListNode<T> *newNode = new ConcurrentSingleLinkedListNode<T>(value);
    ConcurrentSingleLinkedListNode<T> *oldHead = nullptr;

    do
    {
      oldHead = head.load();
      newNode->next.store(oldHead);
    } while (!head.compare_exchange_weak(oldHead, newNode));

    size.fetch_add(1);

    return newNode;
  }

  bool find(const T &value)
  {
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();

    ConcurrentSingleLinkedListNode<T> *curr = head.load();

    while (curr)
    {
      rec->assign(curr, 0);

      if (curr->value == value)
      {
        void *nil = nullptr;
        rec->assign(nil, 0);

        hazardAllocator.release(rec);
        return true;
      }

      curr = curr->next.load();
    }

    void *nil = nullptr;
    rec->assign(nil, 0);
    hazardAllocator.release(rec);

    return false;
  }

  bool tryRemove(const T &value)
  {
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();

    void *nil = nullptr;

    while (true)
    {
      ConcurrentSingleLinkedListNode<T> *prev = nullptr;
      ConcurrentSingleLinkedListNode<T> *curr = head.load();

      while (curr)
      {
        rec->assign(curr, 0);

        ConcurrentSingleLinkedListNode<T> *next = curr->next.load();

        rec->assign(next, 1);

        if (curr->value == value)
        {
          if (prev)
          {
            if (!prev->next.compare_exchange_strong(curr, next))
            {
              break;
            }
          }
          else
          {
            if (!head.compare_exchange_strong(curr, next))
            {
              break;
            }
          }

          rec->retire<ConcurrentSingleLinkedListNode<T>, Allocator>(allocator, 0);
          rec->assign(nil, 1);
          hazardAllocator.release(rec);

          size.fetch_sub(1);
          return true;
        }

        prev = curr;
        curr = next;
      }

      rec->assign(nil, 0);
      rec->assign(nil, 1);
      hazardAllocator.release(rec);

      return false;
    }
  }

  bool tryPop(T &value)
  {
    HazardPointer<2>::Record *rec = hazardAllocator.acquire();

    while (true)
    {
      ConcurrentSingleLinkedListNode<T> *oldHead = head.load();

      if (!oldHead)
      {
        void *nil = nullptr;
        rec->assign(nil, 0);

        hazardAllocator.release(rec);
        return false;
      }

      rec->assign(oldHead, 0);

      if (head.load() != oldHead)
      {
        continue;
      }

      ConcurrentSingleLinkedListNode<T> *newHead = oldHead->next.load();

      if (head.compare_exchange_strong(oldHead, newHead))
      {
        value = oldHead->value;
        rec->retire<ConcurrentSingleLinkedListNode<T>, Allocator>(allocator, 0);
        hazardAllocator.release(rec);
        size.fetch_sub(1);
        return true;
      }
    }
  }
};
} // namespace detail

template <typename T> class ConcurrentList
{
  size_t concurrencyLevel;

public:
  ConcurrentList() : time(0), threadLists(), localLists()
  {
    concurrencyLevel = os::Thread::getHardwareConcurrency();
  }

  ~ConcurrentList()
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *node = threadLists.head.load(std::memory_order_acquire);
    while (node)
    {
      detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *next = node->next.load(std::memory_order_acquire);
      delete node->get();
      node = next;
    }
  }

  void insert(T value)
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *local = nullptr;

    if (!localLists.get(local))
    {
      // NOTE: never delete local directly, it will be cleaned up on destruction.

      detail::ConcurrentLinkedList<T> *producer = new detail::ConcurrentLinkedList<T>();
      local = threadLists.insert(producer);
      localLists.set(local);
    }

    assert(local != nullptr);

    local->get()->insert(value);
  }

  bool tryPop(T &value)
  {
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *local = nullptr;
    localLists.get(local);

    if (local == nullptr)
    {
      local = threadLists.head.load(std::memory_order_acquire);
    }

    if (local == nullptr)
    {
      return false;
    }

    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *node = local;
    detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *start = local;

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
    detail::ConcurrentLinkedList<T> *lists[candidatesMax];

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
      if (lists[i]->tryPop(value))
      {
        return true;
      }
    }

    return false;
  }

private:
  ThreadLocalStorage<detail::ConcurrentSingleLinkedListNode<detail::ConcurrentLinkedList<T> *> *> localLists;
  detail::ConcurrentLinkedList<detail::ConcurrentLinkedList<T> *> threadLists;
  size_t time;
};

} // namespace lib
