
#include "HazardPointer.hpp"
#include "algorithm/bit.hpp"
#include <atomic>
#include <bit>
#include <functional>
#include <limits>
#include <memory>
#include <random>

#include "AtomicLock.hpp"
#include "ConcurrentEpochGarbageCollector.hpp"
#include "ConcurrentLinkedList.hpp"
#include "ConcurrentSortedList.hpp"
#include "MarkedAtomicPointer.hpp"
#include <chrono>
#include <limits>
#include <map>
namespace lib
{

// TODO: add a cache for deleted nodes

// #define MARK_BIT_INDEX 31
// #define MARK_BIT (1U << MARK_BIT_INDEX)
// #define REFCOUNT_MASK ~MARK_BIT

#define CONCURRENT_EGC_CACHE_SIZE 16

template <typename K, typename V, size_t MAX_LEVEL = 16> struct ConcurrentSkipListMapNode
{
private:

public:
  using Node = ConcurrentSkipListMapNode<K, V, MAX_LEVEL>;
  using GC = ConcurrentEpochGarbageCollector<Node, CONCURRENT_EGC_CACHE_SIZE>;

  std::atomic<uint32_t> refCount; // Own cache line
  MarkedAtomicPointer<Node> next[MAX_LEVEL + 1];

  char pad[256 - sizeof(bool) - sizeof(uint32_t) - sizeof(MarkedAtomicPointer<Node>) * (MAX_LEVEL + 1)];

  uint32_t level;
  K key;
  V value;
  const char *debug[MAX_LEVEL + 1];

  // std::atomic<bool> freed;

  uint32_t refAdd(uint32_t value)
  {
    uint32_t expected = 0;
    do
    {
      expected = refCount.load();
      if (expected == 0)
      {
        return 0;
      }

    } while (!refCount.compare_exchange_weak(expected, expected + value, std::memory_order_release, std::memory_order_acquire));
    // if (key != 2147483647)
    //   os::print(" add %u refs %u\n", key, expected + value);

    return expected;
  }

  uint32_t refSub(uint32_t value, typename GC::EpochGuard &scope)
  {
    uint32_t expected = 0;
    do
    {
      expected = refCount.load();
      if (expected == 0)
      {
        int *t = 0;
        *t = 3;
      }
      assert(expected > 0);
      assert(expected >= value);

    } while (!refCount.compare_exchange_weak(expected, expected - value, std::memory_order_release, std::memory_order_acquire));

    if (expected - value == 0)
    {
      // os::print(" retiring %u\n", key);

      for (int i = 0; i <= level; i++)
      {
        next[i].load()->refSub(1, scope);
      }

      scope.retire(this);
    }

    // if (key != 2147483647)
    //   os::print(" sub %u refs %u\n", key, expected - value);

    return expected;
  }

  ConcurrentSkipListMapNode(const K &k, const V &v, uint32_t level) : key(k), value(v), refCount(1), level(level)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ~ConcurrentSkipListMapNode()
  {
    // freed.store(true);
    //  auto allocation = reinterpret_cast<typename GC::Allocation *>(reinterpret_cast<char *>(this) - offsetof(typename
    //  GC::Allocation, data)); uint64_t epoch = gc.minimumEpoch();
  }

  ConcurrentSkipListMapNode(int level) : refCount(1), level(level)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentSkipListMapNode<K, V, MAX_LEVEL> *get(uint32_t level, Node *prev = nullptr, uint32_t l = 0)
  {
    // if (freed.load())
    // {
    //   auto allocation = reinterpret_cast<typename GC::Allocation *>(
    //       reinterpret_cast<char *>(this) - offsetof(typename GC::Allocation, data));
    //   os::print(">>> [%u] was freed %u, %p, prev=%u, %u\n", os::Thread::getCurrentThreadId(), key, allocation, prev != nullptr ? prev->key : -1, l);
    //   int *t = 0;
    //   *t = 3;
    // }
    // assert(!freed.load());
    return next[level].getReference();
  }

  ConcurrentSkipListMapNode<K, V, MAX_LEVEL> *get(uint32_t level, bool &marked, Node *prev = nullptr, uint32_t l = 0)
  {
    // if (freed.load())
    // {
    //   auto allocation = reinterpret_cast<typename GC::Allocation *>(
    //       reinterpret_cast<char *>(this) - offsetof(typename GC::Allocation, data));
    //   os::print(">>> [%u] was freed %u, %p, prev=%u, %u\n", os::Thread::getCurrentThreadId(), key, allocation, prev != nullptr ? prev->key : -1, l);
    //   int *t = 0;
    //   *t = 3;
    //   // exit(1);
    // }
    auto p = next[level].get();
    marked = p.second;
    return p.first;
  }

  // inline uint32_t ref(int increment, bool &marked)
  // {
  //   while (true)
  //   {
  //     next[level].read(marked);

  //     uint32_t old = refCount.load();

  //     if (marked && old == 0)
  //     {
  //       if (key != 2147483647)
  //         os::print("ref failed, was deleted %u %u %i %u, %s\n", key, level, increment, old, debug.c_str());
  //       debug = "";
  //       return 0;
  //     }

  //     if (refCount.compare_exchange_strong(old, (increment >= 0) ? old + 1 : old - 1, std::memory_order_release, std::memory_order_acquire))
  //     {
  //       if (key != 2147483647)
  //         os::print("ref %u %u %i %u %s\n", key, level, increment, (increment >= 0) ? old + 1 : old - 1, debug.c_str());
  //       debug = "";
  //       return old;
  //     }
  //   }

  //   return 0;
  // }

  bool setNext(uint32_t level, ConcurrentSkipListMapNode<K, V, MAX_LEVEL> *expected, ConcurrentSkipListMapNode<K, V, MAX_LEVEL> *to, typename GC::EpochGuard &scope)
  {
    if (next[level].compare_exchange_strong(expected, to, std::memory_order_release, std::memory_order_acquire))
    {
      // to->debug[level] = debug;
      return true;
    }
    return false;
  }
};

template <typename K, typename V, size_t MAX_LEVEL = 16> class ConcurrentSkipListMap
{
private:
  using Node = ConcurrentSkipListMapNode<K, V, MAX_LEVEL>;
  using GC = ConcurrentEpochGarbageCollector<Node, CONCURRENT_EGC_CACHE_SIZE>;

  GC epochGarbageCollector;

  Node *head;
  Node *tail;

  std::atomic<uint64_t> size_;

  int randomLevel()
  {
    static thread_local std::mt19937 generator(std::random_device{}());
    uint32_t r = generator();
    r |= (1U << (MAX_LEVEL - 1));
    int level = countrZero(r) + 1;
    return level;
  }

  // int randomLevel() {
  //     static thread_local uint64_t rng = 0x853245 ^ os::Thread::getCurrentThreadId();

  //     // Xorshift algorithm
  //     rng ^= rng << 13;
  //     rng ^= rng >> 7;
  //     rng ^= rng << 17;

  //     // Geometric distribution simulation
  //     // "countr_zero" counts trailing zeros.
  //     // If the bottom n bits are zero, level is n+1.
  //     // Ensure we don't exceed MAX_LEVEL.
  //     if ((rng & 0xFFFF) != 0) return 1; // 99% chance to return fast

  //     // Fallback for higher levels
  //     uint32_t r = (uint32_t)rng;
  //     int level = 1;
  //     while (((r >>= 1) & 1) != 0) level++;
  //     return std::min((int)MAX_LEVEL, level);
  // }

  template <bool NeedSuccs = true> Node *find(const K &key, Node **preds, Node **succs, typename GC::EpochGuard &scope)
  {
    Node *curr = nullptr, *pred = nullptr;
    bool isMarked = false;

  retry:
    pred = nullptr;
    curr = nullptr;

    pred = head;

    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      curr = pred->get(level);

      while (curr != tail)
      {
        auto succ = curr->get(level, isMarked, pred, level); // curr->next[level].load(std::memory_order_relaxed);

        while (isMarked)
        {
          if (!succ->refAdd(1))
          {
            goto retry;
          }

          if (!pred->setNext(level, curr, succ, scope))
          {
            succ->refSub(1, scope);
            goto retry;
          }

          curr->refSub(1, scope);
          // succ->refSub(1, scope);

          curr = succ;
          succ = curr->get(level, isMarked, pred, level);
        }

        if (curr->key >= key)
        {
          break;
        }

        pred = curr;
        curr = succ;
      }

      if constexpr (NeedSuccs)
      {
        preds[level] = pred;
        succs[level] = curr;
      }
    }

    return curr->key == key ? curr : nullptr;
  }
  void clearInternal(typename GC::EpochGuard &scope)
  {
    bool marked = false;
    Node *prev = head;
    Node *curr = prev->get(0, marked);

    while (true)
    {

      if (curr == tail)
      {
        break;
      }
      Node *next = curr->get(0, marked);

      // os::print("curr = %u, %u\n", curr->key, marked);

      if (!marked)
      {
        remove(curr->key);
      }

      curr = next;
    }
  }

public:
  ConcurrentSkipListMap() : size_(0), epochGarbageCollector()
  {
    auto scope = epochGarbageCollector.openEpochGuard();

    head = epochGarbageCollector.allocate(scope, MAX_LEVEL);
    tail = epochGarbageCollector.allocate(scope, MAX_LEVEL);

    head->key = 0; // std::numeric_limits<K>::min();
    tail->key = std::numeric_limits<K>::max();

    head->refAdd(MAX_LEVEL);
    tail->refAdd(MAX_LEVEL);

    bool marked = false;
    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      bool result = head->setNext(i, nullptr, tail, scope);
      assert(result);
    }
  }

  ~ConcurrentSkipListMap()
  {
    auto scope = epochGarbageCollector.openEpochGuard();

    clearInternal(scope);

    scope.retire(head);
    scope.retire(tail);
  }

  class Iterator
  {
    template <typename A, typename B, size_t C> friend class ConcurrentSkipListMap;

  private:
    Node *current;
    Node *tail;
    typename GC::EpochGuard scope;

    void next()
    {
      while (current != tail)
      {
        //        rec->assign(current, 2);

        Node *next = current->get(0); // next[0].load(std::memory_order_acquire);

        bool isMarked = false;
        if (next != current->get(0, isMarked)) // next[0].load(std::memory_order_acquire))
        {
          continue;
        }

        current = next;

        if (isMarked)
        {
          continue;
        }

        break;
      };
    }

  public:
    Iterator(Node *start, Node *end, typename GC::EpochGuard &scope, bool requiresUnmarked) : current(start), tail(end), scope(scope)
    {
      bool isMarked = false;
      current->get(0, isMarked);

      while (requiresUnmarked && current != tail && isMarked)
      {
        next();
        current->get(0, isMarked);
      }
    }

    // Copy constructor
    Iterator(const Iterator &other) : current(other.current), tail(other.tail), scope(other.scope)
    {
    }

    // Copy assignment operator
    Iterator &operator=(const Iterator &other)
    {
      if (this != &other)
      {
        current = other.current;
        tail = other.tail;
        scope = other.scope;
      }
      return *this;
    }

    // Move constructor
    Iterator(Iterator &&other) noexcept : current(other.current), tail(other.tail), scope(other.scope)
    {
      other.current = nullptr;
      other.scope.clear();
    }

    Iterator &operator=(Iterator &&other) noexcept
    {
      if (this != &other)
      {
        current = other.current;
        tail = other.tail;
        scope = other.scope;

        other.scope.clear();
        other.current = nullptr;
      }
      return *this;
    }

    Iterator &operator=(const V &val)
    {
      current->value = val;
      return *this;
    }

    ~Iterator()
    {
      scope.clear();
    }

    const K &key() const
    {
      return current->key;
    }

    V &value()
    {
      return current->value;
    }

    const V &value() const
    {
      return current->value;
    }

    operator V &()
    {
      return current->value;
    }

    operator const V &() const
    {
      return current->value;
    }

    Iterator &operator++()
    {
      next();
      return *this;
    }

    // Iterator operator++(int)
    // {
    //   Iterator tmp = *this;
    //   next();
    //   return tmp;
    // }

    std::pair<const K &, V &> operator*()
    {
      return std::pair<const K &, V &>(current->key, current->value);
    }

    bool operator==(const Iterator &other) const
    {
      return current == other.current;
    }

    bool operator!=(const Iterator &other) const
    {
      return current != other.current;
    }

    V *operator->()
    {
      return &current->value;
    }

    const V *operator->() const
    {
      return &current->value;
    }
  };

  Iterator insert(const K &key, const V &value)
  {
    uint64_t refs;
    auto scope = epochGarbageCollector.openEpochGuard();
    // os::print("inserting = %u\n", key);

    int topLevel = randomLevel();

    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    Node *newNode = epochGarbageCollector.allocate(scope, key, value, topLevel);

    while (true)
    {
    retry:
      newNode->refCount.store(topLevel + 2);

      if (find(key, preds, succs, scope))
      {
        return end();
      }

      bool marked = false;

      for (int level = 0; level <= topLevel; ++level)
      {
        if (!succs[level]->refAdd(1))
        {
          for (int k = 0; k < level; k++)
          {
            succs[k]->refSub(1, scope);
          }

          goto retry;
        }
      }

      for (int level = 0; level <= topLevel; ++level)
      {
        newNode->next[level].store(succs[level]); //(level, nullptr, succs[level], scope, "insert/newNode linking");
      }

      Node *pred = preds[0];
      Node *succ = succs[0];

      Node *expected = succ;

      if (!pred->setNext(0, expected, newNode, scope))
      {
        for (int k = 0; k <= topLevel; k++)
        {
          succs[k]->refSub(1, scope);
        }

        goto retry;
      }

      succ->refSub(1, scope);

      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          if (pred->setNext(level, succ, newNode, scope))
          {
            succ->refSub(1, scope);
            break;
          }

          find(key, preds, succs, scope);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);

      newNode->refSub(1, scope);

      return Iterator(newNode, tail, scope, false);
    }
  }

  bool remove(const K &key)
  {
    auto scope = epochGarbageCollector.openEpochGuard();

    Node *preds[MAX_LEVEL + 1] = {0};
    Node *succs[MAX_LEVEL + 1] = {0};

    Node *toRemove = nullptr;
    bool ret = false;

    while (true)
    {
      toRemove = find(key, preds, succs, scope);

      if (toRemove == nullptr)
      {
        return false;
      }

      for (int level = toRemove->level; level >= 1; level--)
      {
        bool marked = false;

        Node *succ = toRemove->get(level, marked);

        while (!marked)
        {
          toRemove->setNext(level, succ, (Node *)((uintptr_t)succ | 1ull), scope);
          succ = toRemove->get(level, marked);
        }
      }

      bool marked = false;
      auto succ = toRemove->get(0, marked);

      while (true)
      {
        bool markedIt = toRemove->setNext(0, succ, (Node *)((uintptr_t)succ | 1ull), scope);
        succ = toRemove->get(0, marked);

        if (markedIt)
        {
          find(key, preds, succs, scope);

// #define CHECK_DELETED_NODE
#ifdef CHECK_DELETED_NODE
          for (int k = 0; k < 10; k++)
          {
            struct data
            {
              Node *node;
              Node *prev;
              uint32_t level;
            };

            auto pending = std::queue<data>();
            pending.push({head, head});

            std::map<std::tuple<Node *, Node *, uint32_t>, int> visited;

            while (pending.size())
            {
              auto curr = pending.front();

              visited[std::tuple<Node *, Node *, uint32_t>(curr.node, curr.prev, curr.level)] = 1;

              pending.pop();

              if (!(curr.node == head || curr.node->key > curr.prev->key))
              {
                exit(1);
              }

              assert(curr.node == head || curr.prev != curr.node);

              if (curr.node == tail)
              {
                for (int l = 0; l <= MAX_LEVEL; l++)
                {
                  assert(curr.node->get(l) == nullptr);
                }
              }

              for (uint32_t l = 0; l <= curr.node->level; l++)
              {
                auto next = curr.node->get(l);
                if (next == toRemove && toRemove->refCount.load() == 0)
                {
                  for (int i = 0; i <= MAX_LEVEL; i++)
                  {
                    if (preds[i] != nullptr)
                    {
                      os::print("[%u] level=%u is %p %u\n", os::Thread::getCurrentThreadId(), i, preds[i], preds[i]->key);
                    }
                  }

                  os::print(
                      ">>>> [%u] %u prev=(%p,%u), curr=(%p,%u), next=(%p,%u), (%p,%p),level=%u\n",
                      os::Thread::getCurrentThreadId(),
                      key,
                      curr.prev,
                      curr.prev->key,
                      curr.node,
                      curr.node->key,
                      next,
                      next->key,
                      head,
                      tail,
                      curr.level);
                  exit(1);
                }

                if (next && !visited.count(std::tuple<Node *, Node *, uint32_t>(curr.node, curr.prev, curr.level)))
                {
                  pending.push(data{.node = next, .prev = curr.node, .level = l});
                }
              }
            }
          }
#endif
          size_.fetch_sub(1);
          return true;
        }
        else if (marked)
        {
          return false;
        }
      }
    }

    return false;
  }

  Iterator find(const K &key)
  {
    auto scope = epochGarbageCollector.openEpochGuard();

    Node *found = find<false>(key, nullptr, nullptr, scope);

    if (!found)
    {
      return end();
    }

    return Iterator(found, tail, scope, false);
  }

  int size() const
  {
    return size_.load(std::memory_order_relaxed);
  }

  bool isEmpty() const
  {
    return size_.load(std::memory_order_relaxed) == 0;
  }

  Iterator begin()
  {
    auto scope = epochGarbageCollector.openEpochGuard();
    Node *first = nullptr;

    while (true)
    {
      first = head->get(0);      // next[0].load(std::memory_order_acquire);
      if (first != head->get(0)) // next[0].load(std::memory_order_acquire))
      {
        continue;
      }

      break;
    }

    return Iterator(first, tail, scope, true);
  }

  Iterator end()
  {
    auto scope = epochGarbageCollector.openEpochGuard();
    return Iterator(tail, tail, scope, false);
  }

  Iterator operator[](const K &key)
  {
    while (true)
    {
      auto iter = find(key);

      if (iter.current == tail)
      {
        auto iter2 = insert(key, V());

        if (iter2.current == tail)
        {
          continue;
        }
        return iter2;
      }

      return iter;
    }
  }

  bool contains(const K &key)
  {
    return find(key) != end();
  }

  void clear()
  {
    auto scope = epochGarbageCollector.openEpochGuard();
    clearInternal(scope);
  }
};

} // namespace lib