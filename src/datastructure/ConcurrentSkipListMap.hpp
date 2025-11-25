
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

// ConcurrentLinkedList<std::string> debug[10000];
// std::atomic<uint64_t> timestamp;

// void clearDebug()
// {
//   for (int i = 0; i < 10000; i++)
//   {
//     debug[i].clear();
//   }
// }

inline uint64_t nowNs()
{
  return std::chrono::steady_clock::now().time_since_epoch().count();
}
template <typename K, typename V> struct ConcurrentSkipListMapNode
{
private:
  std::atomic<uint32_t> metadata;

public:
  std::vector<std::atomic<ConcurrentSkipListMapNode<K, V> *>> next;

  K key;
  V value;

  ConcurrentSkipListMapNode(const K &k, const V &v, uint32_t level) : key(k), value(v), metadata(MARK_BIT), next(level + 1)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentSkipListMapNode(int level) : metadata(MARK_BIT), next(level + 1)
  {
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  bool ref(const char *suffix)
  {
    uint32_t meta = 0;
    uint32_t mark = 0;
    uint32_t refs = 0;
    uint32_t attempt = 0;

    while (true)
    {
      meta = metadata.load(std::memory_order_acquire);
      mark = meta & MARK_BIT;
      refs = meta & REFCOUNT_MASK;

      V min = std::numeric_limits<int>::min();
      V max = std::numeric_limits<int>::max();

      // if (meta == 0 && value != min && value != max)
      // {
      //   printf("[%u] FAILED ref v=%u, %u, l=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), level, suffix);

      //   if (value >= 0 && value < 10000)
      //   {
      //     std::vector<std::pair<uint64_t, std::string>> out;

      //     for (auto &s : debug[value]) // iterate your list
      //     {
      //       uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
      //       out.emplace_back(ts, s);
      //     }

      //     std::sort(
      //         out.begin(),
      //         out.end(),
      //         [](auto &a, auto &b)
      //         {
      //           return a.first < b.first;
      //         });

      //     for (auto &entry : out)
      //     {
      //       printf("%s", entry.second.c_str());
      //     }
      //   }

      //   abort();
      // }

      if (meta == 0)
      {
        return false;
      }

      // uint32_t old = metadata.fetch_add(1, std::memory_order_acquire);

      // if (value >= 0 && value < 10000)
      // {
      //   uint64_t ts = nowNs();
      //   char buffer[1024];
      //   std::snprintf(
      //       buffer,
      //       sizeof(buffer),
      //       "%llu [%u] ref v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
      //       (unsigned long long)ts,
      //       os::Thread::getCurrentThreadId(),
      //       value,
      //       next.size(),
      //       level,
      //       mark ? 1 : 0,
      //       (REFCOUNT_MASK & refs) + 1,
      //       attempt++,
      //       suffix);
      //   debug[value].insert(std::string(buffer));
      // }

      // if (old == 0)
      // {
      //   printf("[%u] FAILED ref v=%u, %u, l=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), level, suffix);
      //   if (value >= 0 && value < 10000)
      //   {
      //     std::vector<std::pair<uint64_t, std::string>> out;

      //     for (auto &s : debug[value]) // iterate your list
      //     {
      //       uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
      //       out.emplace_back(ts, s);
      //     }

      //     std::sort(
      //         out.begin(),
      //         out.end(),
      //         [](auto &a, auto &b)
      //         {
      //           return a.first < b.first;
      //         });

      //     for (auto &entry : out)
      //     {
      //       printf("%s", entry.second.c_str());
      //     }
      //   }

      //   abort();
      // }

      if (metadata.compare_exchange_strong(meta, mark | (refs + 1), std::memory_order_release, std::memory_order_acquire))
      {
        return true;
      }
    }

    return false;
  }

  uint32_t refs()
  {
    return metadata.load(std::memory_order_acquire) & REFCOUNT_MASK;
  }

  // void debugMessage(const char *suffix)
  // {
  //   uint32_t meta = 0;
  //   uint32_t mark = 0;
  //   uint32_t refs = 0;
  //   uint32_t attempt = 0;
  //   meta = metadata.load(std::memory_order_acquire);
  //   mark = meta & MARK_BIT;
  //   refs = meta & REFCOUNT_MASK;

  //   if (value >= 0 && value < 10000)
  //   {
  //     uint64_t ts = nowNs();
  //     char buffer[1024];
  //     std::snprintf(buffer, sizeof(buffer), "%llu [%u] debug v=%u, refs=%u, %s\n", (unsigned long long)ts, os::Thread::getCurrentThreadId(), value, refs, suffix);
  //     debug[value].insert(std::string(buffer));
  //   }
  // }

  bool unref(const char *suffix, bool force = false)
  {
    uint32_t meta = 0;
    uint32_t mark = 0;
    uint32_t refs = 0;
    uint32_t attempt = 0;

    while (true)
    {
      meta = metadata.load(std::memory_order_acquire);
      mark = meta & MARK_BIT;
      refs = meta & REFCOUNT_MASK;

      if (meta == 0)
      {
        printf("[%u] FAILED unref v=%u, level=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), suffix);
        // if (value >= 0 && value < 10000)
        // {
        //   std::vector<std::pair<uint64_t, std::string>> out;

        //   for (auto &s : debug[value]) // iterate your list
        //   {
        //     uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
        //     out.emplace_back(ts, s);
        //   }

        //   std::sort(
        //       out.begin(),
        //       out.end(),
        //       [](auto &a, auto &b)
        //       {
        //         return a.first < b.first;
        //       });

        //   for (auto &entry : out)
        //   {
        //     printf("%s", entry.second.c_str());
        //   }
        // }

        return false;
        // abort();
      }

      // uint32_t old = metadata.fetch_sub(1, std::memory_order_acquire);

      // if (value >= 0 && value < 10000)
      // {
      //   uint64_t ts = nowNs();
      //   char buffer[1024];
      //   std::snprintf(
      //       buffer,
      //       sizeof(buffer),
      //       "%llu [%u] der v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
      //       (unsigned long long)ts,
      //       os::Thread::getCurrentThreadId(),
      //       value,
      //       next.size(),
      //       level,
      //       mark ? 1 : 0,
      //       (REFCOUNT_MASK & refs) - 1,
      //       attempt++,
      //       suffix);
      //   debug[value].insert(std::string(buffer));
      // }

      if (metadata.compare_exchange_strong(meta, mark | (refs - 1), std::memory_order_release, std::memory_order_acquire))
      {
        if ((refs - 1) == 0)
        {
          // for (auto &succ : next)
          // {
          //   succ.load(std::memory_order_acquire)->unref("deref destroy");
          // }

          return true;
        }

        return false;
      }
    }

    return false;
  }

  bool isMarked()
  {
    return (metadata.load(std::memory_order_acquire) & MARK_BIT) == 0;
  }
  bool isInvalid()
  {
    return (metadata.load(std::memory_order_acquire)) == 0;
  }

  bool mark(const char *suffix)
  {
    uint32_t meta;
    uint32_t newMeta;
    uint32_t refs;
    uint32_t attempt = 0;

    while (true)
    {
      meta = metadata.load(std::memory_order_acquire);

      if ((meta & MARK_BIT) == 0)
      {
        return false;
      }

      refs = meta & REFCOUNT_MASK;

      // if (value >= 0 && value < 10000)
      // {
      //   uint64_t ts = nowNs();
      //   char buffer[1024];
      //   std::snprintf(
      //       buffer,
      //       sizeof(buffer),
      //       "%llu [%u] mark v=%u, level=%u, refs=%u, attempt=%u, %s\n",
      //       (unsigned long long)ts,
      //       os::Thread::getCurrentThreadId(),
      //       value,
      //       level,
      //       refs,
      //       attempt++,
      //       suffix);
      //   debug[value].insert(std::string(buffer));
      // }

      newMeta = refs;

      if (metadata.compare_exchange_strong(meta, newMeta, std::memory_order_release, std::memory_order_acquire))
      {
        return true;
      }
    }
  }
};

inline void ensure(bool v)
{
  assert(v);
}

template <typename K, typename V, typename Allocator = memory::allocator::SystemAllocator<ConcurrentSkipListMapNode<K, V>>> class ConcurrentSkipListMap
{
  static constexpr int MAX_LEVEL = 16;

  using HazardPointerManager = HazardPointer<MAX_LEVEL + 1, ConcurrentSkipListMapNode<K, V>, Allocator>;

  using HazardPointerRecord = typename HazardPointerManager::Record;
  using Node = ConcurrentSkipListMapNode<K, V>;

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
    return std::min(dist(gen), MAX_LEVEL);
  }

  void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec, const char *suffix)
  {
    if (node == head || node == tail)
    {
      return;
    }

    if (node->unref(suffix))
    {
      for (int i = 0; i < node->next.size(); i++)
      {
        Node *child = node->next[i].load(std::memory_order_acquire);
        removeLinksAndRetireNode(child, rec, suffix);
      }

      rec->retire(node);
    }
  }

  bool find(const K &key, Node **preds, Node **succs, HazardPointerRecord *predsRec, HazardPointerRecord *succRec)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);

    Node *curr = nullptr, *pred = nullptr;
    bool referenced = false;
  retry:
    pred = nullptr;
    curr = nullptr;

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], rec, "find retry pred");
        preds[i] = nullptr;
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], rec, "find retry pred");
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
        removeLinksAndRetireNode(pred, rec, "find goto retry pred");
        goto retry;
      }

      if (!curr->ref("curr ref"))
      {
        removeLinksAndRetireNode(pred, rec, "unref curr ref failed");
        goto retry;
      }

      if (curr->key < pred->key)
      {
        printf(">>>> %u %u\n", curr->key, pred->key);
        abort();
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
          removeLinksAndRetireNode(pred, rec, "succ fail pred");
          removeLinksAndRetireNode(curr, rec, "succ fail curr");
          goto retry;
        }

        if (!succ->ref("succ ref"))
        {
          removeLinksAndRetireNode(pred, rec, "succ fail ref pred");
          removeLinksAndRetireNode(curr, rec, "succ fail ref curr");
          goto retry;
        }

        while (curr->isMarked())
        {
          assert(succ != curr);

          if (succ->key < curr->key)
          {
            printf(">> %u %u\n", succ->key, curr->key);
            abort();
          }

          // curr->debugMessage("is marked");

          Node *expected = curr;

          if (!succ->ref("ref linking while removing"))
          {
            removeLinksAndRetireNode(pred, rec, "succ ref fail pred");
            removeLinksAndRetireNode(curr, rec, "succ ref fail curr");
            removeLinksAndRetireNode(succ, rec, "succ ref fail succ");
            goto retry;
          }

          if (!pred->next[level].compare_exchange_strong(expected, succ, std::memory_order_release, std::memory_order_acquire))
          {
            removeLinksAndRetireNode(pred, rec, "succ link fail pred");
            removeLinksAndRetireNode(curr, rec, "succ link fail curr");
            removeLinksAndRetireNode(succ, rec, "succ link fail succ0");
            removeLinksAndRetireNode(succ, rec, "succ link fail succ1");
            goto retry;
          }

          char buffer[1024];
          std::snprintf(buffer, sizeof(buffer), ">> unref curr is marked remove link from pred, pred =%p, v=%u %p %p unlink", pred, pred->value, head, tail);

          removeLinksAndRetireNode(curr, rec, buffer);
          removeLinksAndRetireNode(curr, rec, ">> unref curr reassign to succ");

          curr = succ;

          // succ->debugMessage("assign to curr");
          rec->assign(curr, 0);
          if (!curr || pred->next[level].load(std::memory_order_acquire) != curr)
          {
            removeLinksAndRetireNode(pred, rec, ">>> retry find failed assign pred 1");
            removeLinksAndRetireNode(curr, rec, ">>> retry find failed assign curr 1");
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
            removeLinksAndRetireNode(pred, rec, ">>>>> retry find failed succ assign pred");
            removeLinksAndRetireNode(curr, rec, ">>>>> retry fund failed succ assign curr");
            goto retry;
          }

          if (!succ->ref(">> succ ref reassign"))
          {
            removeLinksAndRetireNode(pred, rec, ">>>>>> retry find failed succ assign pred");
            removeLinksAndRetireNode(curr, rec, ">>>>>> retry fund failed succ assign curr");
            goto retry;
          }
        }

        if (curr == tail || curr->key >= key)
        {
          if (succ)
          {
            removeLinksAndRetireNode(succ, rec, ">>> succ unred end");
          }
          break;
        }

        removeLinksAndRetireNode(pred, rec, "pred unref default");
        pred = curr;

        assert(curr != succ);
        curr = succ;

        rec->assign(pred, 2);
        rec->assign(curr, 0);
      }

      predsRec->assign(pred, level);
      succRec->assign(curr, level);
      // printf("%p %p %p\n", head, tail, pred);

      if (!pred->ref("find pred ref save list"))
      {
        abort();
      }

      if (!curr->ref("find curr ref save list"))
      {
        abort();
      }

      preds[level] = pred;
      succs[level] = curr;

      removeLinksAndRetireNode(curr, rec, "unref curr at end of loop");
      rec->assign(pred, 2);
    }

    hazardAllocator.release(rec);

    bool result = curr != tail && curr->key == key;

    removeLinksAndRetireNode(pred, rec, "find end unred");

    return result;
  }

public:
  ConcurrentSkipListMap(V min = std::numeric_limits<int>::min(), V max = std::numeric_limits<int>::max()) : size_(0), hazardAllocator()
  {
    head = new Node(MAX_LEVEL);
    tail = new Node(MAX_LEVEL);

    head->ref("ref head");
    tail->ref("ref tail");

    head->value = min;
    tail->value = max;
    head->key = min;
    tail->key = max;

    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      head->next[i].store(tail, std::memory_order_relaxed);

      if (!tail->ref("ref tail link"))
      {
        abort();
      }

      // char buffer[1024];
      // std::snprintf(buffer, sizeof(buffer), "debug pred =%p, v=%u, linking to v=%u %p %p unlink\0", head, head->value, tail->value, head, tail);
      // tail->debugMessage(buffer);
    }
  }

  ~ConcurrentSkipListMap()
  {
    // TODO
  }

  bool insert(const K &key, const V &value)
  {
    int topLevel = randomLevel();

    if (value == 200)
    {
      topLevel = 0;
    }

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
            removeLinksAndRetireNode(preds[i], predsRec, "insert retry pred");
            preds[i] = nullptr;
          }

          if (succs[i] != nullptr)
          {
            removeLinksAndRetireNode(succs[i], succsRec, "insert retry succ");
            succs[i] = nullptr;
          }
        }

        hazardAllocator.release(predsRec);
        hazardAllocator.release(succsRec);
        hazardAllocator.release(rec);

        return false;
      }

      Node *newNode = new Node(key, value, topLevel);
      newNode->ref("ref new node");

      rec->assign(newNode, 0);

      for (int level = 0; level <= topLevel; ++level)
      {
        if (!succs[level]->ref("ref succ new node first"))
        {
          delete newNode;
          goto retry;
        }

        newNode->next[level].store(succs[level], std::memory_order_relaxed);
      }

      Node *pred = preds[0];
      Node *succ = succs[0];

      newNode->ref("ref new node before linking as prev succ");

      Node *expected = succ;
      if (!pred->next[0].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
      {
        for (int level = 0; level <= topLevel; ++level)
        {
          removeLinksAndRetireNode(succs[level], succsRec, "insert not insert delete succ");
        }

        delete newNode;
        goto retry;
      }

      removeLinksAndRetireNode(succ, succsRec, "succ unref first");

      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          if (!newNode->ref("new node link succ"))
          {
            // TODO: maybe just return false is enough
            abort();
          }

          Node *expected = succ;
          if (pred->next[level].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
          {
            removeLinksAndRetireNode(succ, succsRec, "succ unref second");
            break;
          }

          removeLinksAndRetireNode(newNode, rec, "new node link succ");
          find(key, preds, succs, predsRec, succsRec);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);

      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          removeLinksAndRetireNode(preds[i], predsRec, "insert end pred");
        }

        if (succs[i] != nullptr)
        {
          removeLinksAndRetireNode(succs[i], succsRec, "insert end succ");
        }
      }

      removeLinksAndRetireNode(newNode, rec, "new node unref");

      hazardAllocator.release(predsRec);
      hazardAllocator.release(succsRec);
      hazardAllocator.release(rec);

      // printf("[%u] inserted v=%u, %u\n", os::Thread::getCurrentThreadId(), value, topLevel);

      return true;
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

    if (!victim->ref("ref victim"))
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
      if (victim->mark("mark deletetion"))
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
      removeLinksAndRetireNode(victim, rec, "deref victim");
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], predsRec, "remove list result pred");
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], succsRec, "remove list result succ");
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);
    hazardAllocator.release(rec);

    return ret;
  }

  // TODO: unsafe, consider keeping reference
  bool find(const K &key, V &value)
  {
    Node *preds[MAX_LEVEL + 1] = {};
    Node *succs[MAX_LEVEL + 1] = {};

    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, predsRec, succsRec);

    bool result = false;

    if (found && !succs[0]->isMarked())
    {
      value = succs[0]->value;
      result = true;
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], predsRec, "find unref pred");
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], succsRec, "find unref succ");
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);

    return result;
  }

  class Iterator
  {
  private:
    Node *current;
    Node *tail;

    HazardPointerRecord *rec;
    HazardPointerManager &hazardManager;

  public:
    Iterator(Node *start, Node *end, HazardPointerRecord *rec, HazardPointerManager &hazardManager) : current(start), tail(end), rec(rec), hazardManager(hazardManager)
    {
      while (current != tail && current->isMarked())
      {
        next();
      }
    }

    ~Iterator()
    {
      removeLinksAndRetireNode(current, rec, "iterator destructor");
      hazardManager.release(rec);
    }

    void removeLinksAndRetireNode(Node *node, HazardPointerRecord *rec, const char *suffix)
    {
      if (node->unref(suffix))
      {
        for (int i = 0; i < node->next.size(); i++)
        {
          Node *child = node->next[i].load(std::memory_order_acquire);
          removeLinksAndRetireNode(child, rec, suffix);
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

        if (!next->ref("ref next iterator"))
        {
          continue;
        }

        removeLinksAndRetireNode(current, rec, "iterator");

        rec->assign(next, 0);
        current = next;

        if (current->isMarked())
        {
          continue;
        }

        break;
      };
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
  };

  Iterator at(const K &key)
  {
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, predsRec, succsRec);

    Iterator iter(head, tail, rec, hazardAllocator);

    if (found && !succs[0]->marked.load(std::memory_order_acquire))
    {
      rec->assign(succs[0], 0);
      iter->current = succs[0];

      if (!iter->current->ref("ref iter"))
      {
        abort();
      }
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        removeLinksAndRetireNode(preds[i], predsRec, "get ref pred");
      }

      if (succs[i] != nullptr)
      {
        removeLinksAndRetireNode(succs[i], succsRec, "get ref succ");
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

      if (!first->ref("ref iterator"))
      {
        continue;
      }

      break;
    }

    return Iterator(first, tail, rec, hazardAllocator);
  }

  Iterator end()
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    rec->assign(tail, 0);
    tail->ref("ref tail end");
    return Iterator(tail, tail, rec, hazardAllocator);
  }
};

} // namespace lib