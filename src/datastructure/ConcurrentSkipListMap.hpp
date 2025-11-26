
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

// template <typename K, typename V> struct ConcurrentSkipListMapNode
// {
// private:
//   std::atomic<uint32_t> metadata;

// public:
//   std::vector<std::atomic<ConcurrentSkipListMapNode<K, V> *>> next;

//   K key;
//   V value;

//   ConcurrentSkipListMapNode(const K &k, const V &v, uint32_t level) : key(k), value(v), metadata(MARK_BIT), next(level + 1)
//   {
//     for (int i = 0; i <= level; ++i)
//     {
//       next[i].store(nullptr, std::memory_order_relaxed);
//     }
//   }

//   ConcurrentSkipListMapNode(int level) : metadata(MARK_BIT), next(level + 1)
//   {
//     for (int i = 0; i <= level; ++i)
//     {
//       next[i].store(nullptr, std::memory_order_relaxed);
//     }
//   }

//   bool ref()
//   {
//     uint32_t meta = 0;
//     uint32_t mark = 0;
//     uint32_t refs = 0;
//     uint32_t attempt = 0;

//     while (true)
//     {
//       meta = metadata.load(std::memory_order_acquire);
//       mark = meta & MARK_BIT;
//       refs = meta & REFCOUNT_MASK;

//       V min = std::numeric_limits<int>::min();
//       V max = std::numeric_limits<int>::max();

//       // if (meta == 0 && value != min && value != max)
//       // {
//       //   printf("[%u] FAILED ref v=%u, %u, l=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), level, suffix);

//       //   if (value >= 0 && value < 10000)
//       //   {
//       //     std::vector<std::pair<uint64_t, std::string>> out;

//       //     for (auto &s : debug[value]) // iterate your list
//       //     {
//       //       uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
//       //       out.emplace_back(ts, s);
//       //     }

//       //     std::sort(
//       //         out.begin(),
//       //         out.end(),
//       //         [](auto &a, auto &b)
//       //         {
//       //           return a.first < b.first;
//       //         });

//       //     for (auto &entry : out)
//       //     {
//       //       printf("%s", entry.second.c_str());
//       //     }
//       //   }

//       //   abort();
//       // }

//       if (meta == 0)
//       {
//         return false;
//       }

//       // uint32_t old = metadata.fetch_add(1, std::memory_order_acquire);

//       // if (value >= 0 && value < 10000)
//       // {
//       //   uint64_t ts = nowNs();
//       //   char buffer[1024];
//       //   std::snprintf(
//       //       buffer,
//       //       sizeof(buffer),
//       //       "%llu [%u] ref v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
//       //       (unsigned long long)ts,
//       //       os::Thread::getCurrentThreadId(),
//       //       value,
//       //       next.size(),
//       //       level,
//       //       mark ? 1 : 0,
//       //       (REFCOUNT_MASK & refs) + 1,
//       //       attempt++,
//       //       suffix);
//       //   debug[value].insert(std::string(buffer));
//       // }

//       // if (old == 0)
//       // {
//       //   printf("[%u] FAILED ref v=%u, %u, l=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), level, suffix);
//       //   if (value >= 0 && value < 10000)
//       //   {
//       //     std::vector<std::pair<uint64_t, std::string>> out;

//       //     for (auto &s : debug[value]) // iterate your list
//       //     {
//       //       uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
//       //       out.emplace_back(ts, s);
//       //     }

//       //     std::sort(
//       //         out.begin(),
//       //         out.end(),
//       //         [](auto &a, auto &b)
//       //         {
//       //           return a.first < b.first;
//       //         });

//       //     for (auto &entry : out)
//       //     {
//       //       printf("%s", entry.second.c_str());
//       //     }
//       //   }

//       //   abort();
//       // }

//       if (metadata.compare_exchange_strong(meta, mark | (refs + 1), std::memory_order_release, std::memory_order_acquire))
//       {
//         return true;
//       }
//     }

//     return false;
//   }

//   // void debugMessage(const char *suffix)
//   // {
//   //   uint32_t meta = 0;
//   //   uint32_t mark = 0;
//   //   uint32_t refs = 0;
//   //   uint32_t attempt = 0;
//   //   meta = metadata.load(std::memory_order_acquire);
//   //   mark = meta & MARK_BIT;
//   //   refs = meta & REFCOUNT_MASK;

//   //   if (value >= 0 && value < 10000)
//   //   {
//   //     uint64_t ts = nowNs();
//   //     char buffer[1024];
//   //     std::snprintf(buffer, sizeof(buffer), "%llu [%u] debug v=%u, refs=%u, %s\n", (unsigned long long)ts, os::Thread::getCurrentThreadId(), value, refs, suffix);
//   //     debug[value].insert(std::string(buffer));
//   //   }
//   // }

//   bool unref()
//   {
//     uint32_t meta = 0;
//     uint32_t mark = 0;
//     uint32_t refs = 0;
//     uint32_t attempt = 0;

//     while (true)
//     {
//       meta = metadata.load(std::memory_order_acquire);
//       mark = meta & MARK_BIT;
//       refs = meta & REFCOUNT_MASK;

//       if (meta == 0)
//       {
//         printf("[%u] FAILED unref v=%u, level=%u\n", os::Thread::getCurrentThreadId(), value, next.size());
//         return false;
//       }

//       // uint32_t old = metadata.fetch_sub(1, std::memory_order_acquire);

//       // if (value >= 0 && value < 10000)
//       // {
//       //   uint64_t ts = nowNs();
//       //   char buffer[1024];
//       //   std::snprintf(
//       //       buffer,
//       //       sizeof(buffer),
//       //       "%llu [%u] der v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
//       //       (unsigned long long)ts,
//       //       os::Thread::getCurrentThreadId(),
//       //       value,
//       //       next.size(),
//       //       level,
//       //       mark ? 1 : 0,
//       //       (REFCOUNT_MASK & refs) - 1,
//       //       attempt++,
//       //       suffix);
//       //   debug[value].insert(std::string(buffer));
//       // }

//       if (metadata.compare_exchange_strong(meta, mark | (refs - 1), std::memory_order_release, std::memory_order_acquire))
//       {
//         if ((refs - 1) == 0)
//         {
//           // for (auto &succ : next)
//           // {
//           //   succ.load(std::memory_order_acquire)->unref("deref destroy");
//           // }

//           return true;
//         }

//         return false;
//       }
//     }

//     return false;
//   }

//   bool isMarked()
//   {
//     return (metadata.load(std::memory_order_acquire) & MARK_BIT) == 0;
//   }
//   bool isInvalid()
//   {
//     return (metadata.load(std::memory_order_acquire)) == 0;
//   }

//   bool mark()
//   {
//     uint32_t meta;
//     uint32_t newMeta;
//     uint32_t refs;
//     uint32_t attempt = 0;

//     while (true)
//     {
//       meta = metadata.load(std::memory_order_acquire);

//       if ((meta & MARK_BIT) == 0)
//       {
//         return false;
//       }

//       refs = meta & REFCOUNT_MASK;

//       // if (value >= 0 && value < 10000)
//       // {
//       //   uint64_t ts = nowNs();
//       //   char buffer[1024];
//       //   std::snprintf(
//       //       buffer,
//       //       sizeof(buffer),
//       //       "%llu [%u] mark v=%u, level=%u, refs=%u, attempt=%u, %s\n",
//       //       (unsigned long long)ts,
//       //       os::Thread::getCurrentThreadId(),
//       //       value,
//       //       level,
//       //       refs,
//       //       attempt++,
//       //       suffix);
//       //   debug[value].insert(std::string(buffer));
//       // }

//       newMeta = refs;

//       if (metadata.compare_exchange_strong(meta, newMeta, std::memory_order_release, std::memory_order_acquire))
//       {
//         return true;
//       }
//     }
//   }
// };

#define CACHE_LINE_SIZE 128
#define ALIGNED_ATOMIC_PTR_ALIGNMENT 64
template <typename K, typename V, size_t MAX_LEVEL = 16> struct ConcurrentSkipListMapNode
{
private:
  using Node = ConcurrentSkipListMapNode<K, V, MAX_LEVEL>;

  alignas(CACHE_LINE_SIZE) std::atomic<uint32_t> refCount; // Own cache line
  alignas(CACHE_LINE_SIZE) std::atomic<bool> marked;       // Own cache line

  struct alignas(ALIGNED_ATOMIC_PTR_ALIGNMENT) AlignedAtomicPtr
  {
  private:
    std::atomic<Node *> internal_ptr;

  public:
    AlignedAtomicPtr() : internal_ptr(nullptr)
    {
    }

    AlignedAtomicPtr(Node *p) : internal_ptr(p)
    {
    }

    inline void store(Node *desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      internal_ptr.store(desired, order);
    }

    inline Node *load(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
      return internal_ptr.load(order);
    }

    inline Node *exchange(Node *desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return internal_ptr.exchange(desired, order);
    }

    inline bool compare_exchange_weak(Node *&expected, Node *desired, std::memory_order success, std::memory_order failure) noexcept
    {
      return internal_ptr.compare_exchange_weak(expected, desired, success, failure);
    }

    inline bool compare_exchange_weak(Node *&expected, Node *desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return internal_ptr.compare_exchange_weak(expected, desired, order);
    }

    inline bool compare_exchange_strong(Node *&expected, Node *desired, std::memory_order success, std::memory_order failure) noexcept
    {
      return internal_ptr.compare_exchange_strong(expected, desired, success, failure);
    }

    inline bool compare_exchange_strong(Node *&expected, Node *desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return internal_ptr.compare_exchange_strong(expected, desired, order);
    }

    inline operator Node *() const noexcept
    {
      return internal_ptr.load();
    }

    AlignedAtomicPtr(const AlignedAtomicPtr &) = delete;
    AlignedAtomicPtr &operator=(const AlignedAtomicPtr &) = delete;
  };

public:
  AlignedAtomicPtr next[MAX_LEVEL + 1];

  K key;
  V value;

  ConcurrentSkipListMapNode(const K &k, const V &v, uint32_t level) : key(k), value(v), refCount(0), marked(false)
  {
    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentSkipListMapNode(int level) : refCount(0), marked(false)
  {
    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  bool ref()
  {
    uint32_t old = refCount.fetch_add(1, std::memory_order_acquire);
    // printf("+ %p %u\n", this, old);
    if (old == 0 && marked.load(std::memory_order_release))
    {
      // Node was already freed, undo the increment
      refCount.fetch_sub(1, std::memory_order_release);
      return false;
    }

    return true;
  }

  bool unref()
  {
    uint32_t old = refCount.fetch_sub(1, std::memory_order_acquire);
    // printf("- %p %u\n", this, old);

    if (old == 0)
    {
      printf("[%u] FAILED unref v=%u\n", os::Thread::getCurrentThreadId(), value);
      return false;
    }

    return old == 1; // Returns true if this was the last reference
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
  using HazardPointerManager = HazardPointer<MAX_LEVEL + 1, ConcurrentSkipListMapNode<K, V, MAX_LEVEL>, Allocator>;

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

  void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec)
  {
    if (node == head || node == tail)
    {
      return;
    }
    if (node->unref())
    {
      // printf("freed %p\n", node);
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

  bool find(const K &key, Node **preds, Node **succs, HazardPointerRecord *predsRec, HazardPointerRecord *succRec)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    Node *curr = nullptr, *pred = nullptr;
    bool referenced = false;
    uint32_t retries = 0;
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
    rec->assign(pred, 2);

    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      curr = pred->next[level].load(std::memory_order_acquire);
      rec->assign(curr, 0);

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

        Node *succ = curr->next[level].load(std::memory_order_acquire);

        rec->assign(succ, 1);

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

          // curr->debugMessage("is marked");

          Node *expected = curr;

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
          removeLinksAndRetireNode(curr, rec);

          curr = succ;

          rec->assign(curr, 0);
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
          rec->assign(succ, 1);

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

        rec->assign(pred, 2);
        rec->assign(curr, 0);
      }

      predsRec->assign(pred, level);
      succRec->assign(curr, level);

      if (!pred->ref())
      {
        abort();
      }

      if (!curr->ref())
      {
        abort();
      }

      preds[level] = pred;
      succs[level] = curr;

      removeLinksAndRetireNode(curr, rec);
      rec->assign(pred, 2);
    }

    hazardAllocator.release(rec);

    bool result = curr != tail && curr->key == key;

    removeLinksAndRetireNode(pred, rec);

    return result;
  }

public:
  ConcurrentSkipListMap(V min = std::numeric_limits<int>::min(), V max = std::numeric_limits<int>::max()) : size_(0), hazardAllocator()
  {
    head = new Node(MAX_LEVEL);
    tail = new Node(MAX_LEVEL);

    head->ref();
    tail->ref();

    head->value = min;
    tail->value = max;
    head->key = min;
    tail->key = max;

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
    Node *current = head->next[0].load(std::memory_order_acquire);

    while (current != tail)
    {
      Node *next = current->next[0].load(std::memory_order_acquire);
      delete current;
      current = next;
    }

    delete head;
    delete tail;
  }

  class Iterator
  {
    template <typename A, typename B, size_t C, typename D> friend class ConcurrentSkipListMap;

  private:
    Node *current;
    Node *tail;
    HazardPointerRecord *rec;
    HazardPointerManager *hazardManager;

    void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec)
    {
      if (node->unref())
      {
        for (int i = 0; i <= MAX_LEVEL; i++)
        {
          Node *child = node->next[i].load(std::memory_order_acquire);
          if (child)
          {
            removeLinksAndRetireNode(child, rec);
          }
        }
        // printf("freed %u\n", node->value);
        rec->retire(node);
      }
    }

    void next()
    {
      while (current != tail)
      {
        assert(rec->get(0) == current);
        rec->assign(current, 2);
        Node *next = current->next[0].load(std::memory_order_acquire);
        rec->assign(next, 1);
        if (next != current->next[0].load(std::memory_order_acquire))
        {
          continue;
        }
        if (!next->ref())
        {
          continue;
        }
        removeLinksAndRetireNode(current, rec);
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
    Iterator(Node *start, Node *end, HazardPointerRecord *rec, HazardPointerManager *hazardManager, bool requiresUnmarked)
        : current(start), tail(end), rec(rec), hazardManager(hazardManager)
    {
      while (requiresUnmarked && current != tail && current->isMarked())
      {
        next();
      }
    }

    // Copy constructor
    Iterator(const Iterator &other) : current(other.current), tail(other.tail), rec(other.hazardManager->acquire()), hazardManager(other.hazardManager)
    {
      if (current && current->ref())
      {
        rec->assign(current, 0);
      }
    }
    // Copy assignment operator
    Iterator &operator=(const Iterator &other)
    {
      if (this != &other)
      {
        removeLinksAndRetireNode(current, rec);
        hazardManager->release(rec);

        // Copy from other
        current = other.current;
        tail = other.tail;
        hazardManager = other.hazardManager;
        rec = hazardManager->acquire();

        if (current && current->ref())
        {
          rec->assign(current, 0);
        }
      }
      return *this;
    }

    // Move constructor
    Iterator(Iterator &&other) noexcept : current(other.current), tail(other.tail), rec(other.rec), hazardManager(other.hazardManager)
    {
      other.rec = nullptr;
      other.current = nullptr;
    }

    Iterator &operator=(Iterator &&other) noexcept
    {
      if (this != &other)
      {
        if (rec)
        {
          removeLinksAndRetireNode(current, rec);
          hazardManager->release(rec);
        }

        current = other.current;
        tail = other.tail;
        rec = other.rec;
        hazardManager = std::move(other.hazardManager);

        other.rec = nullptr;
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
      if (rec)
      {
        removeLinksAndRetireNode(current, rec);
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

    Iterator operator++(int)
    {
      Iterator tmp = *this;
      next();
      return tmp;
    }

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

    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    while (true)
    {
    retry:
      if (find(key, preds, succs, predsRec, succsRec))
      {
        for (uint32_t i = 0; i <= MAX_LEVEL; i++)
        {
          if (preds[i] != nullptr)
          {
            removeLinksAndRetireNode(preds[i], predsRec);
            preds[i] = nullptr;
          }

          if (succs[i] != nullptr)
          {
            removeLinksAndRetireNode(succs[i], succsRec);
            succs[i] = nullptr;
          }
        }

        hazardAllocator.release(predsRec);
        hazardAllocator.release(succsRec);
        hazardAllocator.release(rec);

        return end();
      }

      Node *newNode = new Node(key, value, topLevel);
      // printf("new node = %p, %p\n", newNode, topLevel);
      newNode->ref();

      rec->assign(newNode, 0);

      for (int level = 0; level <= topLevel; ++level)
      {
        if (!succs[level]->ref())
        {
          delete newNode;
          goto retry;
        }

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
          removeLinksAndRetireNode(succs[level], succsRec);
        }

        delete newNode;
        goto retry;
      }

      removeLinksAndRetireNode(succ, succsRec);

      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          if (!newNode->ref())
          {
            // TODO: maybe just return false is enough
            abort();
          }

          Node *expected = succ;
          if (pred->next[level].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
          {
            removeLinksAndRetireNode(succ, succsRec);
            break;
          }

          removeLinksAndRetireNode(newNode, rec);
          find(key, preds, succs, predsRec, succsRec);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);

      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          removeLinksAndRetireNode(preds[i], predsRec);
        }

        if (succs[i] != nullptr)
        {
          removeLinksAndRetireNode(succs[i], succsRec);
        }
      }

      // removeLinksAndRetireNode(newNode, rec);

      hazardAllocator.release(predsRec);
      hazardAllocator.release(succsRec);

      return Iterator(newNode, tail, rec, &hazardAllocator, false);
    }
  }

  bool remove(const K &key)
  {
    Node *preds[MAX_LEVEL + 1] = {0};
    Node *succs[MAX_LEVEL + 1] = {0};
    Node *victim = nullptr;

    // TODO: we dont need this if we dont ref victim that is already referenced in succs[0]
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, predsRec, succsRec);
    bool ret = false;

    if (!found)
    {
      ret = false;
      goto result;
    }

    victim = succs[0];
    rec->assign(victim, 0);

    if (!victim->ref())
    {
      abort();
    }

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
        find(key, preds, succs, predsRec, succsRec);
        ret = true;
      }
      else
      {
        ret = false;
      }

      goto result;
    }

  result:
    if (ret)
    {
      removeLinksAndRetireNode(victim, rec);
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], predsRec);
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], succsRec);
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);
    hazardAllocator.release(rec);

    return ret;
  }

  Iterator at(const K &key)
  {
    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, predsRec, succsRec);

    if (!found)
    {
      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          removeLinksAndRetireNode(preds[i], predsRec);
        }

        if (succs[i] != nullptr)
        {
          removeLinksAndRetireNode(succs[i], succsRec);
        }
      }

      hazardAllocator.release(predsRec);
      hazardAllocator.release(succsRec);

      return end();
    }

    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    Iterator iter(head, tail, rec, &hazardAllocator, false);

    if (found && !succs[0]->isMarked())
    {
      rec->assign(succs[0], 0);
      iter.current = succs[0];

      if (!iter.current->ref())
      {
        abort();
      }
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], predsRec);
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], succsRec);
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);

    return iter;
  }

  int getSize() const
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

      if (!first->ref())
      {
        continue;
      }

      break;
    }

    return Iterator(first, tail, rec, &hazardAllocator, true);
  }

  Iterator end()
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    rec->assign(tail, 0);
    tail->ref();
    return Iterator(tail, tail, rec, &hazardAllocator, false);
  }

  Iterator operator[](const K &key)
  {
    while (true)
    {
      auto iter = at(key);

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
};

} // namespace lib