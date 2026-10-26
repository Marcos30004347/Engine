#pragma once

#include "ConcurrentEpochGarbageCollector.hpp"
#include "MarkedAtomicPointer.hpp"
#include "algorithm/random.hpp"
#include "datastructure/HazardPointer.hpp"
#include "memory/allocator/SystemAllocator.hpp"

#include "os/Thread.hpp"
#include <atomic>
#include <cstddef>

namespace lib
{
#define CONCURRENT_LINKED_LIST_CACHE_SIZE 8
template <typename K> class ConcurrentShardedList;
template <typename T> class ConcurrentLinkedList;

template <typename T> struct ConcurrentListNode
{
public:
  friend class ConcurrentShardedList<T>;
  T value;
  MarkedAtomicPointer<ConcurrentListNode<T>> next;

  ConcurrentListNode(T &val) : value(val), next()
  {
    next.store(nullptr);
  }

  template <typename U = T, std::enable_if_t<std::is_default_constructible_v<U>, int> = 0> ConcurrentListNode() : next()
  {
    next.store(nullptr);
  }

  ~ConcurrentListNode()
  {
  }

  T &get()
  {
    return value;
  }
};

template <typename T> class ConcurrentLinkedListIterator
{
  friend class ConcurrentLinkedList<T>;

public:
  using Node = ConcurrentListNode<T>;
  using Guard = typename ConcurrentEpochGarbageCollector<Node, CONCURRENT_LINKED_LIST_CACHE_SIZE>::EpochGuard;

  ConcurrentLinkedListIterator(Node *node, Guard &guard) : curr(node), guard(guard)
  {
  }

  ConcurrentLinkedListIterator(const ConcurrentLinkedListIterator &other) : curr(other.curr), guard(other.guard)
  {
  }

  // Copy assignment operator
  ConcurrentLinkedListIterator &operator=(const ConcurrentLinkedListIterator &other)
  {
    if (this != &other)
    {
      curr = other.curr;
      guard = other.guard;
    }
    return *this;
  }

  // Move constructor
  ConcurrentLinkedListIterator(ConcurrentLinkedListIterator &&other) noexcept : curr(other.curr), guard(other.guard)
  {
    other.curr = nullptr;
    other.guard.clear();
  }

  ConcurrentLinkedListIterator &operator=(ConcurrentLinkedListIterator &&other) noexcept
  {
    if (this != &other)
    {
      curr = other.curr;
      guard = other.guard;

      other.guard.clear();
      other.curr = nullptr;
    }
    return *this;
  }

  ConcurrentLinkedListIterator &operator=(const T &val)
  {
    curr->value = val;
    return *this;
  }

  ~ConcurrentLinkedListIterator()
  {
    guard.clear();
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
    {
      return *this;
    }

    Node *next;

    while (true)
    {
      next = curr->next.load(std::memory_order_acquire);

      if (curr->next.load(std::memory_order_acquire) != next)
      {
        continue;
      }

      curr = next;
      return *this;
    }
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

  T &value()
  {
    return curr->value;
  }

  const T &value() const
  {
    return curr->value;
  }

  T *operator->()
  {
    return &curr->value;
  }

private:
  Node *curr;
  Guard guard;
};

// ConcurrentLinkedList
template <typename T> class ConcurrentLinkedList
{
public:
  using iterator = ConcurrentLinkedListIterator<T>;
  using const_iterator = ConcurrentLinkedListIterator<T>;
  using Node = ConcurrentListNode<T>;
  using Guard = typename ConcurrentEpochGarbageCollector<Node, CONCURRENT_LINKED_LIST_CACHE_SIZE>::EpochGuard;

  ConcurrentListNode<T> *root;
  std::atomic<int> size;

  ConcurrentEpochGarbageCollector<Node> garbageCollector;

  template <typename U = T, std::enable_if_t<std::is_default_constructible_v<U>, int> = 0> ConcurrentLinkedList() : root(nullptr), size(0)
  {
    auto scope = garbageCollector.openEpochGuard();
    root = garbageCollector.allocateUnitialized(scope);
    root->next.store(nullptr);
  }

  template <typename... Args, std::enable_if_t<std::is_constructible_v<T, Args...>, int> = 0> explicit ConcurrentLinkedList(Args &&...args) : root(nullptr), size(0)
  {
    auto scope = garbageCollector.openEpochGuard();
    root = garbageCollector.allocate(scope, std::forward<Args>(args)...);
    root->next.store(nullptr);
  }

  ~ConcurrentLinkedList()
  {
    auto scope = garbageCollector.openEpochGuard();
    clear();
    scope.retire(root);
  }

  iterator pushFront(T &value)
  {
    auto scope = garbageCollector.openEpochGuard();

    ConcurrentListNode<T> *newNode = garbageCollector.allocate(scope, value);
    ConcurrentListNode<T> *oldHead = nullptr;

    bool isMarked = false;

    do
    {
      oldHead = root->next.read(isMarked);
      assert(!isMarked);
      newNode->next.store(oldHead);
    } while (!root->next.compare_exchange_strong(oldHead, newNode, std::memory_order_release, std::memory_order_acquire));

    size.fetch_add(1);

    return iterator(newNode, scope);
  }

  template <typename... Args, std::enable_if_t<std::is_constructible_v<T, Args...>, int> = 0> iterator emplaceFront(Args &&...args)
  {
    auto scope = garbageCollector.openEpochGuard();

    ConcurrentListNode<T> *newNode = garbageCollector.allocate(scope, std::forward<Args>(args)...);

    ConcurrentListNode<T> *oldHead = nullptr;
    bool isMarked = false;

    do
    {
      oldHead = root->next.read(isMarked);
      assert(!isMarked);
      newNode->next.store(oldHead);
    } while (!root->next.compare_exchange_strong(oldHead, newNode, std::memory_order_release, std::memory_order_acquire));

    size.fetch_add(1, std::memory_order_relaxed);

    return iterator(newNode, scope);
  }

  void clear()
  {
    auto scope = garbageCollector.openEpochGuard();
    bool isMarked = false;

    while (size.load() > 0)
    {
      ConcurrentListNode<T> *oldHead = root->next.read(isMarked);

      assert(!isMarked);

      if (!oldHead)
      {
        return;
      }

      ConcurrentListNode<T> *newHead = oldHead->next.load();

      if (root->next.compare_exchange_strong(oldHead, newHead, std::memory_order_release, std::memory_order_acquire))
      {
        if (oldHead->next.attemptMark(newHead, true))
        {
          scope.retire(oldHead);
          size.fetch_sub(1);
        }
      }
    }
  }

  bool tryRemove(const T &value)
  {
    auto scope = garbageCollector.openEpochGuard();

  retry:
    ConcurrentListNode<T> *prev = root;
    bool isMarked = false;
    ConcurrentListNode<T> *curr = prev->next.read(isMarked);

    while (curr)
    {
      ConcurrentListNode<T> *next = curr->next.read(isMarked);

      if (isMarked)
      {
        ConcurrentListNode<T> *expected = curr;

        if (!prev->next.compare_exchange_strong(expected, next, std::memory_order_release, std::memory_order_acquire))
        {
          goto retry;
        }

        scope.retire(curr);

        curr = next;

        if (curr)
        {
          next = curr->next.read(isMarked);
        }

        continue;
      }

      if (curr->value == value)
      {
        if (!curr->next.attemptMark(next, true))
        {
          goto retry;
        }

        ConcurrentListNode<T> *expected = curr;

        if (prev->next.compare_exchange_strong(expected, next, std::memory_order_release, std::memory_order_acquire))
        {
          scope.retire(curr);
        }

        size.fetch_sub(1);
        return true;
      }

      prev = curr;
      curr = next;
    }

    return false;
  }

  bool popFront(T &value)
  {
    auto scope = garbageCollector.openEpochGuard();

    while (true)
    {
      bool isMarked = false;
      ConcurrentListNode<T> *oldHead = root->next.read(isMarked);

      assert(!isMarked);

      if (!oldHead)
      {
        return false;
      }

      ConcurrentListNode<T> *newHead = oldHead->next.read(isMarked);

      while (isMarked && newHead)
      {
        newHead = newHead->next.read(isMarked);
      }

      if (root->next.compare_exchange_strong(oldHead, newHead, std::memory_order_release, std::memory_order_acquire))
      {
        if (oldHead->next.attemptMark(newHead, true))
        {
          scope.retire(oldHead);
          value = std::move(oldHead->value);
          size.fetch_sub(1);

          return true;
        }
      }
    }

    return false;
  }

  iterator begin()
  {
    auto scope = garbageCollector.openEpochGuard();
    bool isMarked = false;

    while (true)
    {
      Node *curr = root->next.read(isMarked);

      if (!curr)
      {
        return end();
      }

      return iterator(curr, scope);
    }
  }

  uint64_t length()
  {
    return size.load();
  }

  iterator end()
  {
    auto scope = garbageCollector.openEpochGuard();
    return iterator(nullptr, scope);
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
  }

  void pushFront(T value)
  {
    ConcurrentLinkedList<T> *local = nullptr;

    if (!localLists.get(local))
    {
      ConcurrentLinkedList<T> *producer = new ConcurrentLinkedList<T>();

      local = threadLists.pushFront(producer).value();
      localLists.set(local);
    }

    assert(local != nullptr);
    local->pushFront(value);
  }

  bool popFront(T &value)
  {
    ConcurrentLinkedList<T> *local = nullptr;

    localLists.get(local);

    if (local != nullptr && local->popFront(value))
    {
      return true;
    }

    for (auto &l : threadLists)
    {
      if (l->popFront(value))
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
  ThreadLocalStorage<ConcurrentLinkedList<T> *> localLists;
  ConcurrentLinkedList<ConcurrentLinkedList<T> *> threadLists;
  size_t time;
};

} // namespace lib