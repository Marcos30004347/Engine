
#include "HazardPointer.hpp"
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <random>

#include "AtomicLock.hpp"
#include "ConcurrentLinkedList.hpp"
#include <chrono>
#include <limits>

namespace lib
{

// TODO: add a cache for deleted nodes

#define MARK_BIT_INDEX 31
#define MARK_BIT (1U << MARK_BIT_INDEX)
#define REFCOUNT_MASK ~MARK_BIT

#define CACHE_LINE_SIZE 8
#define ALIGNED_ATOMIC_PTR_ALIGNMENT 8

template <typename K, typename V, size_t MAX_LEVEL = 16> struct ConcurrentSkipListMapNode
{
public:
  using Node = ConcurrentSkipListMapNode<K, V, MAX_LEVEL>;

  alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> refCount; // Own cache line
  alignas(CACHE_LINE_SIZE) std::atomic<bool> marked;       // Own cache line

private:
  struct alignas(ALIGNED_ATOMIC_PTR_ALIGNMENT) MarkedAtomicPtr
  {
  private:
    std::atomic<uintptr_t> internal_ptr;

    static constexpr uintptr_t MARK_BIT = 0x1;
    static constexpr uintptr_t PTR_MASK = ~MARK_BIT;

    static inline uintptr_t compose(Node *ptr, bool mark) noexcept
    {
      return reinterpret_cast<uintptr_t>(ptr) | (mark ? MARK_BIT : 0);
    }

  public:
    MarkedAtomicPtr() : internal_ptr(0)
    {
    }

    MarkedAtomicPtr(Node *p, bool mark = false) : internal_ptr(compose(p, mark))
    {
    }

    inline Node *getReference(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      uintptr_t value = internal_ptr.load(order);
      return reinterpret_cast<Node *>(value & PTR_MASK);
    }

    inline bool isMarked(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      uintptr_t value = internal_ptr.load(order);
      return (value & MARK_BIT) != 0;
    }

    inline std::pair<Node *, bool> get(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      uintptr_t value = internal_ptr.load(order);
      return {reinterpret_cast<Node *>(value & PTR_MASK), (value & MARK_BIT) != 0};
    }

    inline bool tryMark(Node *expected_ptr, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      uintptr_t expected = compose(expected_ptr, false);
      uintptr_t desired = compose(expected_ptr, true);
      return internal_ptr.compare_exchange_strong(expected, desired, order);
    }

    inline void store(Node *desired, bool mark = false, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      internal_ptr.store(compose(desired, mark), order);
    }

    inline Node *load(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      return getReference(order);
    }

    inline uintptr_t exchange(Node *desired, bool mark = false, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return internal_ptr.exchange(compose(desired, mark), order);
    }

    inline bool compare_exchange_weak(Node *&expected, Node *desired, bool expected_mark, bool desired_mark, std::memory_order success, std::memory_order failure) noexcept
    {
      uintptr_t exp = compose(expected, expected_mark);
      uintptr_t des = compose(desired, desired_mark);
      bool result = internal_ptr.compare_exchange_weak(exp, des, success, failure);
      expected = reinterpret_cast<Node *>(exp & PTR_MASK);
      return result;
    }

    inline bool compare_exchange_weak(Node *&expected, Node *desired, bool expected_mark, bool desired_mark, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return compare_exchange_weak(expected, desired, expected_mark, desired_mark, order, order);
    }

    inline bool compare_exchange_strong(Node *&expected, Node *desired, bool expected_mark, bool desired_mark, std::memory_order success, std::memory_order failure) noexcept
    {
      uintptr_t exp = compose(expected, expected_mark);
      uintptr_t des = compose(desired, desired_mark);
      bool result = internal_ptr.compare_exchange_strong(exp, des, success, failure);
      expected = reinterpret_cast<Node *>(exp & PTR_MASK);
      return result;
    }

    inline bool compare_exchange_strong(Node *&expected, Node *desired, bool expected_mark, bool desired_mark, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return compare_exchange_strong(expected, desired, expected_mark, desired_mark, order, order);
    }

    inline operator Node *() const noexcept
    {
      return getReference();
    }

    MarkedAtomicPtr(const MarkedAtomicPtr &) = delete;
    MarkedAtomicPtr &operator=(const MarkedAtomicPtr &) = delete;
  };

  MarkedAtomicPtr next[MAX_LEVEL + 1];

public:
  void *h;
  void *t;

  uint32_t level;
  K key;
  V value;

  ConcurrentSkipListMapNode(const K &k, const V &v, uint32_t level) : key(k), value(v), refCount(0), marked(false), level(level)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentSkipListMapNode(int level) : refCount(0), marked(false), level(level)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentListNode<K, V, MAX_LEVEL> *get(uint32_t level)
  {
    return next[level].getReference();
  }

  ConcurrentListNode<K, V, MAX_LEVEL> *get(uint32_t level, bool &marked)
  {
    auto p = next[level].get();
    marked = p.second;
    return p.first;
  }

  bool ref()
  {
    while (true)
    {
      uint32_t degree = refCount.load(std::memory_order_acquire);

      if (degree == 0 && marked.load(std::memory_order_acquire))
      {
        return false;
      }

      if (refCount.compare_exchange_strong(degree, degree + 1, std::memory_order_release, std::memory_order_acquire))
      {
        return true;
      }
    }

    return false;
  }

  bool unref()
  {
    while (true)
    {
      uint32_t degree = refCount.load(std::memory_order_acquire);

      if (degree <= 0 && marked.load(std::memory_order_acquire))
      {
        return false;
      }

      uint32_t expected = degree;

      if (refCount.compare_exchange_strong(expected, degree - 1, std::memory_order_release, std::memory_order_acquire))
      {
        return expected == 1 && marked.load(std::memory_order_acquire);
      }
    }

    return false;

    // uint32_t old = refCount.fetch_sub(1, std::memory_order_acquire);
    // if (this != h && this != t)
    //   // printf("- %p %u\n", this, old - 1);

    //   if (old == 0)
    //   {
    //     //        printf("[%u] FAILED unref ptr=%p\n", os::Thread::getCurrentThreadId(), this);
    //     return false;
    //   }

    // return old == 1; // Returns true if this was the last reference
  }

  bool isMarked()
  {
    return marked.load(std::memory_order_acquire);
  }

  bool mark()
  {
    bool expected = false;
    return marked.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire);
  }
};

template <typename K, typename V, size_t MAX_LEVEL = 16, typename Allocator = memory::allocator::SystemAllocator<ConcurrentSkipListMapNode<K, V, MAX_LEVEL>>>
class ConcurrentSkipListMap
{
  using HazardPointerManager = HazardPointer<2 * (MAX_LEVEL + 1) + 6, ConcurrentSkipListMapNode<K, V, MAX_LEVEL>, Allocator>;

  using HazardPointerRecord = typename HazardPointerManager::Record;
  using Node = ConcurrentSkipListMapNode<K, V, MAX_LEVEL>;

  HazardPointerManager hazardAllocator;
  Allocator allocator;

private:
  Node *head;
  Node *tail;
  std::atomic<int> size_;

  int randomLevel()
  {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::geometric_distribution<int> dist(0.5);
    return std::min(dist(gen), (int)MAX_LEVEL);
  }
  // int randomLevel()
  // {
  //   static thread_local uint64_t state = std::chrono::steady_clock::now().time_since_epoch().count();

  //   // Wyrand - fastest quality PRNG
  //   state += 0xa0761d6478bd642f;
  //   uint64_t result = state;
  //   result ^= result >> 32;
  //   result *= 0xe7037ed1a0b428db;
  //   result ^= result >> 32;

  //   int level = __builtin_ctzll(~result);
  //   return level % (MAX_LEVEL + 1);
  // }

  void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec)
  {
    if (node == head || node == tail)
    {
      return;
    }

    if (node->unref())
    {
      for (int i = 0; i <= MAX_LEVEL; i++)
      {
        Node *child = node->next[i].load(std::memory_order_acquire);

        if (child != nullptr)
        {
          removeLinksAndRetireNode(child, rec);
        }
      }

      rec->retire(node);
    }
  }

  bool find(const K &key, Node **preds, Node **succs, HazardPointerRecord *rec)
  {
    // printf("find internal\n");
    // HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    constexpr uint32_t recStart = 3;

    Node *curr = nullptr, *pred = nullptr;

  retry:
    pred = nullptr;
    curr = nullptr;

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], rec);
        preds[i] = nullptr;
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], rec);
        succs[i] = nullptr;
      }
    }

    pred = head;
    pred->ref();

    rec->assign(pred, recStart + 2);

    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      curr = pred->next[level].load(std::memory_order_relaxed);
      rec->assign(curr, recStart + 0);

      if (curr != pred->next[level].load(std::memory_order_acquire))
      {
        removeLinksAndRetireNode(pred, rec);
        goto retry;
      }

      if (!curr->ref())
      {
        removeLinksAndRetireNode(pred, rec);
        goto retry;
      }

      while (true)
      {
        if (curr == tail)
        {
          break;
        }

        Node *succ = curr->next[level].load(std::memory_order_relaxed);
        rec->assign(succ, recStart + 1);

        if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
        {
          removeLinksAndRetireNode(pred, rec);
          removeLinksAndRetireNode(curr, rec);
          goto retry;
        }

        if (!succ->ref())
        {
          removeLinksAndRetireNode(pred, rec);
          removeLinksAndRetireNode(curr, rec);
          goto retry;
        }

        while (curr->isMarked())
        {
          assert(succ != curr);

          Node *expected = curr;
          assert(succ == rec->get(recStart + 1));

          if (!succ->ref())
          {
            removeLinksAndRetireNode(pred, rec);
            removeLinksAndRetireNode(curr, rec);
            removeLinksAndRetireNode(succ, rec);
            goto retry;
          }

          if (!pred->next[level].compare_exchange_strong(expected, succ, std::memory_order_release, std::memory_order_acquire))
          {
            removeLinksAndRetireNode(pred, rec);
            removeLinksAndRetireNode(curr, rec);
            removeLinksAndRetireNode(succ, rec);
            removeLinksAndRetireNode(succ, rec);
            goto retry;
          }

          removeLinksAndRetireNode(curr, rec);

          curr = succ;

          rec->assign(curr, recStart + 0);
          if (!curr || pred->next[level].load(std::memory_order_acquire) != curr)
          {
            removeLinksAndRetireNode(pred, rec);
            removeLinksAndRetireNode(curr, rec);
            goto retry;
          }

          succ = nullptr;

          if (curr == tail)
          {
            break;
          }

          succ = curr->next[level].load(std::memory_order_acquire);
          rec->assign(succ, recStart + 1);
          if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
          {
            removeLinksAndRetireNode(pred, rec);
            removeLinksAndRetireNode(curr, rec);
            goto retry;
          }

          if (!succ->ref())
          {
            removeLinksAndRetireNode(pred, rec);
            removeLinksAndRetireNode(curr, rec);
            goto retry;
          }
        }

        if (curr == tail || curr->key >= key)
        {
          if (succ)
          {
            removeLinksAndRetireNode(succ, rec);
          }
          break;
        }

        removeLinksAndRetireNode(pred, rec);
        pred = curr;

        assert(curr != succ);
        curr = succ;

        rec->assign(pred, recStart + 2);
        rec->assign(curr, recStart + 0);
      }

      rec->assign(pred, 6 + level);
      rec->assign(curr, 6 + MAX_LEVEL + 1 + level);

      if (!pred->ref())
      {
        goto retry;
      }

      preds[level] = pred;

      if (!curr->ref())
      {
        goto retry;
      }

      succs[level] = curr;

      removeLinksAndRetireNode(curr, rec);
      rec->assign(pred, recStart + 2);
    }

    // hazardAllocator.release(rec);

    bool result = curr != tail && curr->key == key;
    removeLinksAndRetireNode(pred, rec);
    return result;
  }

public:
  ConcurrentSkipListMap() : size_(0), hazardAllocator()
  {
    head = new Node(MAX_LEVEL);
    tail = new Node(MAX_LEVEL);
    head->h = head;
    head->t = tail;
    tail->h = head;
    tail->t = tail;
    head->ref();
    tail->ref();

    head->key = std::numeric_limits<K>::min();
    tail->key = std::numeric_limits<K>::max();

    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      head->next[i].store(tail, std::memory_order_relaxed);

      if (!tail->ref())
      {
        abort();
      }
    }
  }

  ~ConcurrentSkipListMap()
  {
    clear();

    auto record = hazardAllocator.acquire(allocator);

    record->retire(head);
    record->retire(tail);

    hazardAllocator.release(record);
  }

  bool checkDegree(Node *node, std::unordered_set<Node *> &visited)
  {
    if (visited.count(node))
    {
      return true;
    }

    visited.insert(node);

    if (node == tail)
    {
      return true;
    }

    for (uint32_t i = 0; i <= node->level; i++)
    {
      if ((node == head && node->next[i].load(std::memory_order_acquire)) || node != head)
      {
        if (!checkDegree(node->next[i].load(std::memory_order_acquire), visited))
        {
          return false;
        }
      }
    }

    if (node == head || node == tail)
    {
      return true;
    }

    if (node->refCount.load() != node->level + 1)
    {
      if constexpr (std::is_integral<V>::value)
      {
        printf("node %p (%u) have degree %u and level %u\n", node, node->value, node->refCount.load(), node->level + 1);
      }
    }

    return node->refCount.load() == node->level + 1;
  }

  bool checkNodesDegrees()
  {
    std::unordered_set<Node *> visited;
    return checkDegree(head, visited);
  }

  class Iterator
  {
    template <typename A, typename B, size_t C, typename D> friend class ConcurrentSkipListMap;

  private:
    Node *current;
    Node *tail;
    HazardPointerRecord *rec;
    HazardPointerManager *hazardManager;
    Allocator *allocator;

    // void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec)
    // {
    //   if (node->unref())
    //   {
    //     for (int i = 0; i <= MAX_LEVEL; i++)
    //     {
    //       Node *child = node->next[i].load(std::memory_order_acquire);
    //       if (child)
    //       {
    //         removeLinksAndRetireNode(child, rec);
    //       }
    //     }

    //     // printf("freed %p\n", node);

    //     rec->retire(node);
    //   }
    // }

    void next()
    {
      while (current != tail)
      {
        assert(rec->get(0) == current);

        //        rec->assign(current, 2);

        Node *next = current->next[0].load(std::memory_order_acquire);

        rec->assign(next, 1);

        if (next != current->next[0].load(std::memory_order_acquire))
        {
          continue;
        }

        rec->assign(next, 0);
        current = next;

        if (current->isMarked())
        {
          continue;
        }

        break;
      };
    }

  public:
    Iterator(Node *start, Node *end, HazardPointerRecord *rec, HazardPointerManager *hazardManager, Allocator *allocator, bool requiresUnmarked)
        : current(start), tail(end), rec(rec), hazardManager(hazardManager)
    {
      while (requiresUnmarked && current != tail && current->isMarked())
      {
        next();
      }
    }

    // Copy constructor
    Iterator(const Iterator &other) : current(other.current), tail(other.tail), allocator(other.allocator), hazardManager(other.hazardManager)
    {
      if (other.hazardManager)
      {
        if (other.allocator == nullptr)
        {
          *(int *)0 = 4;
        }
        rec = other.hazardManager->acquire(*(other.allocator));
      }

      if (current == tail)
      {
        return;
      }

      if (current)
      {
        rec->assign(current, 0);
      }
    }

    // Copy assignment operator
    Iterator &operator=(const Iterator &other)
    {
      if (this != &other)
      {
        if (rec)
        {
          // removeLinksAndRetireNode(current, rec);
          hazardManager->release(rec);
        }

        // Copy from other
        current = other.current;
        tail = other.tail;
        hazardManager = other.hazardManager;
        allocator = other.allocator;

        if (hazardManager)
        {
          rec = hazardManager->acquire();
          if (current)
          {
            rec->assign(current, 0);
          }
        }
      }
      return *this;
    }

    // Move constructor
    Iterator(Iterator &&other) noexcept : current(other.current), tail(other.tail), rec(other.rec), hazardManager(other.hazardManager)
    {
      other.rec = nullptr;
      other.current = nullptr;
      other.allocator = nullptr;
      other.hazardManager = nullptr;
    }

    Iterator &operator=(Iterator &&other) noexcept
    {
      if (this != &other)
      {
        // if (rec)
        // {
        //   removeLinksAndRetireNode(current, rec);
        //   hazardManager->release(rec);
        // }

        current = other.current;
        tail = other.tail;
        rec = other.rec;
        hazardManager = std::move(other.hazardManager);
        allocator = other.allocator;

        other.rec = nullptr;
        other.current = nullptr;
        other.allocator = nullptr;
        other.hazardManager = nullptr;
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
      if (rec)
      {
        // removeLinksAndRetireNode(current, rec);
        hazardManager->release(rec);
      }
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
    int topLevel = randomLevel();
    // printf("insert\n");
    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    while (true)
    {
    retry:
      if (find(key, preds, succs, rec))
      {
        for (uint32_t i = 0; i <= MAX_LEVEL; i++)
        {
          if (preds[i] != nullptr)
          {
            removeLinksAndRetireNode(preds[i], rec);
            preds[i] = nullptr;
          }

          if (succs[i] != nullptr)
          {
            removeLinksAndRetireNode(succs[i], rec);
            succs[i] = nullptr;
          }
        }

        hazardAllocator.release(rec);

        return end();
      }
      // printf("creating node\n");
      Node *newNode = new Node(key, value, topLevel);

      newNode->h = head;
      newNode->t = tail;
      newNode->ref();

      rec->assign(newNode, 0);

      for (int level = 0; level <= topLevel; ++level)
      {
        if (!succs[level]->ref())
        {
          for (int l = 0; l < level; ++l)
          {
            removeLinksAndRetireNode(succs[level], rec);
          }

          delete newNode;
          goto retry;
        }

        assert(succs[level] != nullptr);

        newNode->next[level].store(succs[level], std::memory_order_relaxed);
      }

      Node *pred = preds[0];
      Node *succ = succs[0];

      newNode->ref();

      Node *expected = succ;
      if (!pred->next[0].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
      {
        for (int level = 0; level <= topLevel; ++level)
        {
          assert(succs[level] != nullptr);
          removeLinksAndRetireNode(succs[level], rec);
        }

        delete newNode;
        goto retry;
      }

      removeLinksAndRetireNode(succ, rec);

      for (int level = 1; level <= topLevel; ++level)
      {
        if (!newNode->ref())
        {
          // TODO: maybe just return false is enough
          abort();
        }

        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          Node *expected = succ;
          if (pred->next[level].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
          {
            removeLinksAndRetireNode(succ, rec);
            break;
          }

          find(key, preds, succs, rec);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);

      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          removeLinksAndRetireNode(preds[i], rec);
        }

        if (succs[i] != nullptr)
        {
          removeLinksAndRetireNode(succs[i], rec);
        }
      }

      removeLinksAndRetireNode(newNode, rec);

      return Iterator(newNode, tail, rec, &hazardAllocator, &allocator, false);
    }
  }

  Iterator remove(const K &key)
  {
    // printf("remove\n");
    Node *preds[MAX_LEVEL + 1] = {0};
    Node *succs[MAX_LEVEL + 1] = {0};
    Node *victim = nullptr;

    // TODO: we dont need this if we dont ref victim that is already referenced in succs[0]
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, rec);
    bool ret = false;

    if (!found)
    {
      ret = false;
      goto result;
    }

    victim = succs[0];
    rec->assign(victim, 0);

    // if (!victim->ref())
    // {
    //   abort();
    // }

    if (victim->isMarked())
    {
      ret = false;
      goto result;
    }

    // uint32_t x = victim->metadata.load(std::memory_order_acquire);
    // assert((x & REFCOUNT_MASK) >= 2);

    while (!victim->isMarked())
    {
      if (victim->mark())
      {
        size_.fetch_sub(1, std::memory_order_relaxed);
        find(key, preds, succs, rec);
        ret = true;
      }
      else
      {
        ret = false;
      }

      goto result;
    }

  result:

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], rec);
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], rec);
      }
    }

    if (ret)
    {
      rec->assign(victim, 0);
      return Iterator(victim, tail, rec, &hazardAllocator, &allocator, false);
    }

    hazardAllocator.release(rec);

    return end();
  }

  Iterator find(const K &key)
  {
    // printf("find\n");
    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, rec);

    if (!found)
    {
      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          removeLinksAndRetireNode(preds[i], rec);
        }

        if (succs[i] != nullptr)
        {
          removeLinksAndRetireNode(succs[i], rec);
        }
      }

      hazardAllocator.release(rec);

      return end();
    }

    // succs[0]->ref();
    rec->assign(succs[0], 0);

    Iterator iter(succs[0], tail, rec, &hazardAllocator, &allocator, false);

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], rec);
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], rec);
      }
    }

    return iter;
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
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    Node *first = nullptr;

    while (true)
    {
      first = head->next[0].load(std::memory_order_acquire);

      rec->assign(first, 0);

      if (first != head->next[0].load(std::memory_order_acquire))
      {
        continue;
      }

      break;
    }

    return Iterator(first, tail, rec, &hazardAllocator, &allocator, true);
  }

  Iterator end()
  {
    //     printf("end\n");
    // HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    // rec->assign(tail, 0);
    // tail->ref();
    return Iterator(tail, tail, nullptr, nullptr, nullptr, false);
  }

  void clear()
  {
    for (auto it = begin(); it != end(); ++it)
    {
      remove(it.key());
    }
  }

  Iterator operator[](const K &key)
  {
    // printf("[]\n");
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
};

} // namespace lib