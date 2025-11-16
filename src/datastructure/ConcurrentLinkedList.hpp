#pragma once

#include "algorithm/random.hpp"
#include "datastructure/HazardPointer.hpp"
#include "memory/allocator/SystemAllocator.hpp"
#include "os/Thread.hpp"
#include <atomic>
#include <cstddef>

namespace lib
{

template <typename K> class ConcurrentShardedList;
template <typename T, typename Allocator> class ConcurrentLinkedList;

template <typename T> struct ConcurrentListNode
{
public:
  friend class ConcurrentShardedList<T>;
  T value;
  std::atomic<ConcurrentListNode<T> *> next;

  ConcurrentListNode(const T &val) : value(val), next(nullptr)
  {
  }
  ~ConcurrentListNode()
  {
  }

  T &get()
  {
    return value;
  }
};

template <typename T, typename Allocator> class ConcurrentLinkedListIterator
{
  friend class ConcurrentLinkedList<T, Allocator>;

public:
  using Node = ConcurrentListNode<T>;
  using HazardPointerManager = HazardPointer<2, Node, Allocator>;
  using HazardPointerRecord = typename HazardPointerManager::Record;

  ConcurrentLinkedListIterator(Node *node, HazardPointerRecord *rec, HazardPointerManager &hazardAllocator, Allocator &allocator)
      : curr(node), prev(nullptr), rec(rec), hazardAllocator(&hazardAllocator), allocator(&allocator)
  {
  }

  ~ConcurrentLinkedListIterator()
  {
    if (rec)
    {
      hazardAllocator->release(rec);
      rec = nullptr;
    }
  }

  ConcurrentLinkedListIterator(const ConcurrentLinkedListIterator &other) : curr(other.curr), prev(other.prev), hazardAllocator(other.hazardAllocator), allocator(other.allocator)
  {
    if (other.rec && curr)
    {
      rec = hazardAllocator->acquire(*allocator);
      rec->assign(curr, 0);
    }
    else
    {
      rec = nullptr;
    }
  }

  ConcurrentLinkedListIterator(ConcurrentLinkedListIterator &&other) noexcept
      : curr(other.curr), prev(other.prev), rec(other.rec), hazardAllocator(other.hazardAllocator), allocator(other.allocator)
  {
    other.rec = nullptr;
    other.curr = nullptr;
    other.prev = nullptr;
  }

  ConcurrentLinkedListIterator &operator=(const ConcurrentLinkedListIterator &other)
  {
    if (this != &other)
    {
      if (rec)
      {
        hazardAllocator->release(rec);
      }

      curr = other.curr;
      prev = other.prev;
      hazardAllocator = other.hazardAllocator;
      allocator = other.allocator;

      if (other.rec && curr)
      {
        rec = hazardAllocator->acquire(*allocator);
        rec->assign(curr, 0);
      }
      else
      {
        rec = nullptr;
      }
    }
    return *this;
  }

  ConcurrentLinkedListIterator &operator=(ConcurrentLinkedListIterator &&other) noexcept
  {
    if (this != &other)
    {
      if (rec)
      {
        hazardAllocator->release(rec);
      }

      curr = other.curr;
      prev = other.prev;
      rec = other.rec;
      hazardAllocator = other.hazardAllocator;
      allocator = other.allocator;

      other.rec = nullptr;
      other.curr = nullptr;
      other.prev = nullptr;
    }
    return *this;
  }

  T &operator*() const
  {
    return curr->value;
  }

  T *operator->() const
  {
    return &(curr->value);
  }

  ConcurrentLinkedListIterator &operator++()
  {
    if (!curr)
      return *this;

    HazardPointerRecord *nrec = hazardAllocator->acquire(*allocator);

    while (true)
    {
      Node *next = curr->next.load(std::memory_order_acquire);

      if (next)
        nrec->assign(next, 0);

      if (curr->next.load(std::memory_order_acquire) == next)
      {
        // Update prev and curr
        prev = curr;
        hazardAllocator->release(rec);
        rec = nrec;
        curr = next;
        break;
      }
    }

    return *this;
  }

  ConcurrentLinkedListIterator operator++(int)
  {
    ConcurrentLinkedListIterator tmp(*this);
    ++(*this);
    return tmp;
  }

  bool operator==(const ConcurrentLinkedListIterator &other) const
  {
    return curr == other.curr;
  }

  bool operator!=(const ConcurrentLinkedListIterator &other) const
  {
    return !(*this == other);
  }

private:
  Node *curr;
  Node *prev; // Track previous node for O(1) erase during iteration
  HazardPointerRecord *rec;
  HazardPointerManager *hazardAllocator;
  Allocator *allocator;
};

// ConcurrentLinkedList
template <typename T, typename Allocator = memory::allocator::SystemAllocator<ConcurrentListNode<T>>> class ConcurrentLinkedList
{
public:
  using iterator = ConcurrentLinkedListIterator<T, Allocator>;
  using const_iterator = ConcurrentLinkedListIterator<T, Allocator>;

  std::atomic<ConcurrentListNode<T> *> head;
  std::atomic<int> size;
  using HazardPointerManager = HazardPointer<2, ConcurrentListNode<T>, Allocator>;
  using HazardPointerRecord = typename HazardPointerManager::Record;
  HazardPointerManager hazardAllocator;
  Allocator allocator;

  ConcurrentLinkedList(Allocator &allocator) : head(nullptr), allocator(allocator), size(0)
  {
  }
  ConcurrentLinkedList() : head(nullptr), allocator(), size(0)
  {
  }

  ~ConcurrentLinkedList()
  {
    ConcurrentListNode<T> *curr = head.load();
    while (curr)
    {
      ConcurrentListNode<T> *next = curr->next.load();
      delete curr;
      curr = next;
    }
  }

  ConcurrentListNode<T> *insert(const T &value)
  {
    ConcurrentListNode<T> *newNode = new ConcurrentListNode<T>(value);
    ConcurrentListNode<T> *oldHead = nullptr;
    do
    {
      oldHead = head.load();
      newNode->next.store(oldHead);
    } while (!head.compare_exchange_weak(oldHead, newNode));
    size.fetch_add(1);
    return newNode;
  }

  iterator insertAndGetIterator(const T &value)
  {
    ConcurrentListNode<T> *newNode = new ConcurrentListNode<T>(value);
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    ConcurrentListNode<T> *oldHead = nullptr;
    do
    {
      oldHead = head.load();
      newNode->next.store(oldHead);
    } while (!head.compare_exchange_weak(oldHead, newNode));

    rec->assign(newNode, 0);
    size.fetch_add(1);

    iterator it(newNode, rec, hazardAllocator, allocator);
    it.prev = nullptr; // New head has no predecessor
    return it;
  }

  // O(1) erase if iterator has valid prev pointer (e.g., obtained during iteration)
  // Returns iterator to next element, or end() if erase failed
  iterator erase(iterator &it)
  {
    if (!it.curr)
    {
      return end();
    }

    ConcurrentListNode<T> *toDelete = it.curr;
    ConcurrentListNode<T> *next = toDelete->next.load(std::memory_order_acquire);

    // Try to unlink the node
    bool success = false;
    if (it.prev)
    {
      // We have prev pointer - try O(1) deletion
      success = it.prev->next.compare_exchange_strong(toDelete, next);
    }
    else
    {
      // No prev pointer, must be head (or iterator not from iteration)
      success = head.compare_exchange_strong(toDelete, next);
    }

    if (success)
    {
      // Successfully unlinked
      HazardPointerRecord *oldRec = it.rec;

      // Acquire new hazard pointer for next node
      HazardPointerRecord *newRec = nullptr;
      if (next)
      {
        newRec = hazardAllocator.acquire(allocator);
        newRec->assign(next, 0);
      }

      // Retire the deleted node
      if (oldRec)
      {
        oldRec->retire(toDelete);
        hazardAllocator.release(oldRec);
      }

      size.fetch_sub(1);

      // Update the iterator to point to next
      it.curr = next;
      it.rec = newRec;
      // it.prev stays the same

      return it;
    }
    else
    {
      // CAS failed - node was already modified/deleted by another thread
      // Return end() to indicate failure
      return end();
    }
  }

  void clear()
  {
    using Node = ConcurrentListNode<T>;

    Node *oldHead = head.exchange(nullptr, std::memory_order_acq_rel);
    size.store(0, std::memory_order_release);

    if (!oldHead)
      return;

    Node *curr = oldHead;
    while (curr)
    {
      Node *next = curr->next.load(std::memory_order_relaxed);

      HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
      rec->retire(curr);
      hazardAllocator.release(rec);

      curr = next;
    }
  }

  bool tryRemove(const T &value)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    while (true)
    {
      ConcurrentListNode<T> *prev = nullptr;
      ConcurrentListNode<T> *curr = head.load();

      while (curr)
      {
        rec->assign(curr, 0);

        if (head.load() != curr && !prev)
        {
          break;
        }

        ConcurrentListNode<T> *next = curr->next.load();
        rec->assign(next, 1);

        if (next != curr->next.load())
        {
          break;
        }

        if (curr->value == value)
        {
          if (prev)
          {
            if (!prev->next.compare_exchange_strong(curr, next))
              break;
          }
          else
          {
            if (!head.compare_exchange_strong(curr, next))
              break;
          }
          rec->retire(curr);
          hazardAllocator.release(rec);
          size.fetch_sub(1);
          return true;
        }
        prev = curr;
        curr = next;
      }
      hazardAllocator.release(rec);
      return false;
    }
  }

  bool tryPop(T &value)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    while (true)
    {
      ConcurrentListNode<T> *oldHead = head.load();
      if (!oldHead)
      {
        hazardAllocator.release(rec);
        return false;
      }
      rec->assign(oldHead, 0);
      if (head.load() != oldHead)
        continue;
      ConcurrentListNode<T> *newHead = oldHead->next.load();
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

  iterator begin()
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    using Node = ConcurrentListNode<T>;
    while (true)
    {
      Node *curr = head.load(std::memory_order_acquire);

      if (!curr)
      {
        hazardAllocator.release(rec);
        return end();
      }

      rec->assign(curr, 0);

      if (head.load(std::memory_order_acquire) == curr)
      {
        iterator it(curr, rec, hazardAllocator, allocator);
        it.prev = nullptr; // First element has no predecessor
        return it;
      }
    }
  }

  uint64_t length()
  {
    return size.load();
  }

  iterator end()
  {
    return iterator(nullptr, nullptr, hazardAllocator, allocator);
  }

  const_iterator begin() const
  {
    return const_cast<ConcurrentLinkedList *>(this)->begin();
  }

  const_iterator end() const
  {
    return const_cast<ConcurrentLinkedList *>(this)->end();
  }
};

template <typename T> class ConcurrentShardedList
{
  size_t concurrencyLevel;

public:
  ConcurrentShardedList() : time(0), threadLists(), localLists()
  {
    concurrencyLevel = os::Thread::getHardwareConcurrency();
  }

  ~ConcurrentShardedList()
  {
    ConcurrentListNode<ConcurrentLinkedList<T> *> *node = threadLists.head.load(std::memory_order_acquire);
    while (node)
    {
      ConcurrentListNode<ConcurrentLinkedList<T> *> *next = node->next.load(std::memory_order_acquire);
      delete node->get();
      node = next;
    }
  }

  void insert(T value)
  {
    ConcurrentListNode<ConcurrentLinkedList<T> *> *local = nullptr;

    if (!localLists.get(local))
    {
      ConcurrentLinkedList<T> *producer = new ConcurrentLinkedList<T>();
      local = threadLists.insert(producer);
      localLists.set(local);
    }

    assert(local != nullptr);
    local->get()->insert(value);
  }

  bool tryPop(T &value)
  {
    ConcurrentListNode<ConcurrentLinkedList<T> *> *local = nullptr;
    localLists.get(local);

    if (local == nullptr)
    {
      local = threadLists.head.load(std::memory_order_acquire);
    }

    if (local == nullptr)
    {
      return false;
    }

    ConcurrentListNode<ConcurrentLinkedList<T> *> *node = local;
    ConcurrentListNode<ConcurrentLinkedList<T> *> *start = local;

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
    ConcurrentLinkedList<T> *lists[candidatesMax];

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

  void clear()
  {
    auto it = threadLists.begin();
    while (it != threadLists.end())
    {
      ConcurrentLinkedList<T> *lst = *it;
      lst->clear();
      ++it;
    }

    threadLists.clear();
    localLists.clear();

    time = 0;
  }

private:
  ThreadLocalStorage<ConcurrentListNode<ConcurrentLinkedList<T> *> *> localLists;
  ConcurrentLinkedList<ConcurrentLinkedList<T> *> threadLists;
  size_t time;
};

} // namespace lib