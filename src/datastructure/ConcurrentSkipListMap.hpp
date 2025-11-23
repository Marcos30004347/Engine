
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

#define MARK_BIT_INDEX 31
#define MARK_BIT (1U << MARK_BIT_INDEX)
#define REFCOUNT_MASK ~MARK_BIT

ConcurrentLinkedList<std::string> debug[10000];
std::atomic<uint64_t> timestamp;

void clearDebug()
{
  for (int i = 0; i < 10000; i++)
  {
    debug[i].clear();
  }
}

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

  // TODO remove
  int level;
  uint64_t freedAt;

  ConcurrentSkipListMapNode(const K &k, const V &v, int level) : key(k), value(v), metadata(MARK_BIT), next(level + 1), level(level)
  {
    freedAt = (uint64_t)-1;
    for (int i = 0; i <= level; ++i)
    {
      next[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  ConcurrentSkipListMapNode(int level) : metadata(MARK_BIT), next(level + 1), level(level)
  {
    freedAt = (uint64_t)-1;
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

      if (value >= 0 && value < 10000)
      {
        uint64_t ts = nowNs();
        char buffer[1024];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu [%u] ref v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
            (unsigned long long)ts,
            os::Thread::getCurrentThreadId(),
            value,
            next.size(),
            level,
            mark ? 1 : 0,
            (REFCOUNT_MASK & old) + 1,
            attempt++,
            suffix);
        debug[value].insert(std::string(buffer));
      }

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

  void debugMessage(const char *suffix)
  {
    uint32_t meta = 0;
    uint32_t mark = 0;
    uint32_t refs = 0;
    uint32_t attempt = 0;
    meta = metadata.load(std::memory_order_acquire);
    mark = meta & MARK_BIT;
    refs = meta & REFCOUNT_MASK;

    if (value >= 0 && value < 10000)
    {
      uint64_t ts = nowNs();
      char buffer[1024];
      std::snprintf(buffer, sizeof(buffer), "%llu [%u] debug v=%u, refs=%u, %s\n", (unsigned long long)ts, os::Thread::getCurrentThreadId(), value, refs, suffix);
      debug[value].insert(std::string(buffer));
    }
  }

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
        printf("[%u] FAILED unref v=%u, %u, l=%u, %s\n", os::Thread::getCurrentThreadId(), value, next.size(), level, suffix);
        if (value >= 0 && value < 10000)
        {
          std::vector<std::pair<uint64_t, std::string>> out;

          for (auto &s : debug[value]) // iterate your list
          {
            uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
            out.emplace_back(ts, s);
          }

          std::sort(
              out.begin(),
              out.end(),
              [](auto &a, auto &b)
              {
                return a.first < b.first;
              });

          for (auto &entry : out)
          {
            printf("%s", entry.second.c_str());
          }
        }

        return false;
        // abort();
      }

      // uint32_t old = metadata.fetch_sub(1, std::memory_order_acquire);

      if (value >= 0 && value < 10000)
      {
        uint64_t ts = nowNs();
        char buffer[1024];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu [%u] der v=%u, %u, l=%u, alive=%u, refs=%u, attempt=%u, %s\n",
            (unsigned long long)ts,
            os::Thread::getCurrentThreadId(),
            value,
            next.size(),
            level,
            mark ? 1 : 0,
            (REFCOUNT_MASK & old) - 1,
            attempt++,
            suffix);
        debug[value].insert(std::string(buffer));
      }

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

      if (value >= 0 && value < 10000)
      {
        uint64_t ts = nowNs();
        char buffer[1024];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu [%u] mark v=%u, level=%u, refs=%u, attempt=%u, %s\n",
            (unsigned long long)ts,
            os::Thread::getCurrentThreadId(),
            value,
            level,
            refs,
            attempt++,
            suffix);
        debug[value].insert(std::string(buffer));
      }

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

  void removeLinks(Node *node)
  {
    for (auto &c : node->next)
    {
      Node *node = c.load(std::memory_order_acquire);
      bool shouldDelete = node->unref("unref remove links");
      if (shouldDelete)
      {
        removeLinks(shouldDelete);
      }
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
        preds[i]->unref("find retry pred");
        preds[i] = nullptr;
      }

      if (succs[i] != nullptr)
      {
        succs[i]->unref("find retry succ");
        succs[i] = nullptr;
      }
    }

    pred = head;
    
    rec->assign(pred, 2);
    
    if(!pred->ref("pred ref")) {
      goto retry;
    }

    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      assert(pred->freedAt == (uint64_t)-1);

      curr = pred->next[level].load(std::memory_order_acquire);
      rec->assign(curr, 0);
      
      if (curr != pred->next[level].load(std::memory_order_acquire))
      {
        pred->unref("find goto retry pred");
        goto retry;
      }

      if(!curr->ref("curr ref")) {
        pred->unref("unref curr ref failed");
        goto retry;
      }

      assert(curr->key > pred->key);

      while (true)
      {
        if (curr == tail)
        {
          break;
        }

        Node *succ = curr->next[level].load(std::memory_order_acquire);

        if (succ == curr)
        {
          pred->unref("> equal");
          curr->unref("> equal");
          goto retry;
        }

        rec->assign(succ, 1);
        if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
        {
          pred->unref("succ fail pred");
          curr->unref("succ fail curr");
          goto retry;
        }
        referenced = succ->ref("succ ref");
        if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
        {
          if (referenced)
          {
            succ->unref("succ unref");
          }
          pred->unref("succ fail pred");
          curr->unref("succ fail curr");
          goto retry;
        }

        while (curr->isMarked())
        {
          assert(succ != curr);
          assert(succ->key > curr->key);

          curr->debugMessage("is marked");

          Node *expected = curr;
          referenced = succ->ref("ref linking while removing");

          if (!referenced)
          {
            succ->unref("ref linking while removing");
            pred->unref("> retry succ replace pred");
            curr->unref("> retry succ replace curr");
            goto retry;
          }

          if (!pred->next[level].compare_exchange_strong(expected, succ, std::memory_order_release, std::memory_order_acquire))
          {
            succ->unref("ref linking while removing");
            pred->unref("> retry succ replace pred");
            curr->unref("> retry succ replace curr");
            succ->unref("> retry succ replace succ");
            goto retry;
          }
          else
          {
            // Node *expected = succ;

            // if (curr->next[level].compare_exchange_strong(expected, tail, std::memory_order_release, std::memory_order_acquire))
            // {
            //   succ->unref("unref from curr");
            // }
            // else
            // {
            //   pred->unref("> retry succ replace pred");
            //   curr->unref("> retry succ replace curr");
            //   succ->unref("> retry succ replace succ");
            //   printf("retry9");
            //   goto retry;
            // }

            // std::snprintf(buffer, sizeof(buffer), "debug pred =%p, v=%u, linking to v=%u %p %p unlink\0", pred, pred->value, succ->value, head, tail);
            // pred->debugMessage(buffer);

            // std::snprintf(buffer, sizeof(buffer), "debug succ =%p, v=%u, linking to pred v=%u %p %p unlink\0", succ, succ->value, pred->value, head, tail);
            // succ->debugMessage(buffer);
          }

          char buffer[1024];
          std::snprintf(buffer, sizeof(buffer), ">> unref curr is marked remove link from pred, pred =%p, v=%u %p %p unlink", pred, pred->value, head, tail);
          curr->unref(buffer);

          // assert(pred->next[level].load() != curr);
          // if(level == 0) {
          //   abort();
          // }
          // curr->next[level].store(tail);
          // succ->unref("unref from curr");

          curr->unref(">> unref curr reassign to succ");

          // printf("v=%u refs=%u\n", curr->value, curr->refs());

          // if(curr->refs() == 0) {
          //   printf(" AAAAAA\n");
          // }

          curr = succ;

          succ->debugMessage("assign to curr");
          rec->assign(curr, 0);

          if (!curr || pred->next[level].load(std::memory_order_acquire) != curr)
          {
            pred->unref(">>> retry find failed assign pred 1");
            curr->unref(">>> retry find failed assign curr 1");
            goto retry;
          }
          succ = nullptr;

          // referenced = curr->ref(">> ref find referenced curr");
          // if (!curr || pred->next[level].load(std::memory_order_acquire) != curr)
          // {
          //   pred->unref(">>>> retry find failed assign pred 2");
          //   succ->unref(">>>> retry find failed assign curr 2");

          //   if (referenced)
          //   {
          //     curr->unref(">>>> retry find failed assign curr 2");
          //   }
          //   goto retry;
          // }

          // succ->unref(">> succ while reassign");

          if (curr == tail)
          {
            break;
          }

          succ = curr->next[level].load(std::memory_order_acquire);
          rec->assign(succ, 1);
          if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
          {
            pred->unref(">>>>> retry find failed succ assign pred");
            curr->unref(">>>>> retry fund failed succ assign curr");
            goto retry;
          }
          referenced = succ->ref(">> succ ref reassign");
          if (!succ || succ != curr->next[level].load(std::memory_order_acquire))
          {
            pred->unref(">>>>>> retry find failed succ assign pred");
            curr->unref(">>>>>> retry fund failed succ assign curr");
            if (referenced)
            {
              succ->unref(">>>>>> retry succ ref reassign");
            }
            goto retry;
          }
        }

        if (curr == tail || curr->key >= key)
        {
          // if (succ)
          // {
          if (succ)
          {
            succ->unref(">>> succ unred end");
          }
          // }
          break;
        }

        pred->unref("pred unref default");
        pred = curr;
        //        referenced = pred->ref("ref assiign pred referenced");
        assert(curr != succ);
        //        curr->unref("unref curr reassign to succ");
        curr = succ;

        assert(referenced);

        rec->assign(pred, 2);
        rec->assign(curr, 0);
      }

      predsRec->assign(pred, level);
      succRec->assign(curr, level);
      // printf("%p %p %p\n", head, tail, pred);
      referenced = pred->ref("find pred ref save list");
      assert(referenced);

      referenced = curr->ref("find curr ref save list");
      assert(referenced);

      preds[level] = pred;
      succs[level] = curr;

      curr->unref("unref curr at end of loop");
      rec->assign(pred, 2);
    }

    hazardAllocator.release(rec);

    bool result = curr != tail && curr->key == key;

    pred->unref("find end unred");
    // curr->unref("find end curr");

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
      tail->ref("ref tail link");
      char buffer[1024];
      std::snprintf(buffer, sizeof(buffer), "debug pred =%p, v=%u, linking to v=%u %p %p unlink\0", head, head->value, tail->value, head, tail);
      tail->debugMessage(buffer);
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
            preds[i]->unref("insert retry pred");
            if (preds[i]->isInvalid())
            {
              preds[i]->freedAt = timestamp.fetch_add(1);
            }
            preds[i] = nullptr;
          }

          if (succs[i] != nullptr)
          {
            succs[i]->unref("insert retry succ");
            if (succs[i]->isInvalid())
            {
              succs[i]->freedAt = timestamp.fetch_add(1);
            }
            succs[i] = nullptr;
          }
        }

        hazardAllocator.release(predsRec);
        hazardAllocator.release(succsRec);

        return false;
      }

      Node *newNode = new Node(key, value, topLevel);

      newNode->ref("ref new node");

      for (int level = 0; level <= topLevel; ++level)
      {
        succs[level]->ref("ref succ new node first");
        newNode->next[level].store(succs[level], std::memory_order_relaxed);

        // char buffer[1024];
        // std::snprintf(buffer, sizeof(buffer), "debug newNode =%p, v=%u, linking to v=%u %p %p unlink\0", newNode, newNode->value, succs[level]->value, head, tail);
        // newNode->debugMessage(buffer);
      }

      Node *pred = preds[0];
      Node *succ = succs[0];

      newNode->ref("ref new node before linking as prev succ");

      Node *expected = succ;
      if (!pred->next[0].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
      {
        for (int level = 0; level <= topLevel; ++level)
        {
          succs[level]->unref("insert not insert delete succ");
          if (succs[level]->isInvalid())
          {
            succs[level]->freedAt = timestamp.fetch_add(1);
          }
        }

        delete newNode;
        goto retry;
      }
      succ->unref("succ unref first");

      // char buffer[1024];
      // std::snprintf(buffer, sizeof(buffer), "debug pred =%p, v=%u, linking to v=%u %p %p unlink\0", pred, pred->value, newNode->value, head, tail);
      // newNode->debugMessage(buffer);

      if (succ->isInvalid())
      {
        succ->freedAt = timestamp.fetch_add(1);
      }

      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          Node *expected = succ;

          newNode->ref("new node link succ");
          if (pred->next[level].compare_exchange_strong(expected, newNode, std::memory_order_release, std::memory_order_acquire))
          {
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer), "debug maybe a find pred=%p, v=%u, linking to new node=%u %p %p unlink\0", pred, pred->value, newNode->value, head, tail);
            newNode->debugMessage(buffer);

            succ->unref("succ unref second");
            break;
          }

          newNode->unref("new node link succ");
          find(key, preds, succs, predsRec, succsRec);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);

      for (uint32_t i = 0; i <= MAX_LEVEL; i++)
      {
        if (preds[i] != nullptr)
        {
          preds[i]->unref("insert end pred");
        }

        if (succs[i] != nullptr)
        {
          succs[i]->unref("insert end succ");
        }
      }

      hazardAllocator.release(predsRec);
      hazardAllocator.release(succsRec);

      newNode->unref("new node unref");
      // printf("[%u] inserted v=%u, %u\n", os::Thread::getCurrentThreadId(), value, topLevel);

      return true;
    }
  }

  bool remove(const K &key)
  {
    Node *preds[MAX_LEVEL + 1] = {0};
    Node *succs[MAX_LEVEL + 1] = {0};
    Node *victim = nullptr;

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
    victim->debugMessage("is victim");
    victim->ref("ref victim");

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
      // printf("[%u] remove %u, refs %u\n", os::Thread::getCurrentThreadId(), victim->value, victim->refs());

      victim->unref("deref victim");
    }
    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        preds[i]->unref("remove list result pred");
      }

      if (succs[i] != nullptr)
      {
        succs[i]->unref("remove list result succ");
      }
    }
    if (ret)
    {
      // printf("[%u] remove %u, refs %u\n", os::Thread::getCurrentThreadId(), victim->value, victim->refs());

      if (victim->value == 200 && victim->refs() == 2)
      {
        std::vector<std::pair<uint64_t, std::string>> out;

        for (auto &s : debug[victim->value]) // iterate your list
        {
          uint64_t ts = std::strtoull(s.c_str(), nullptr, 10);
          out.emplace_back(ts, s);
        }

        std::sort(
            out.begin(),
            out.end(),
            [](auto &a, auto &b)
            {
              return a.first < b.first;
            });

        for (auto &entry : out)
        {
          printf("%s", entry.second.c_str());
        }
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);

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
        preds[i]->unref("find unref pred");
        if (preds[i]->isInvalid())
        {
          preds[i]->freedAt = timestamp.fetch_add(1);
        }
        preds[i] = nullptr;
      }

      if (succs[i] != nullptr)
      {
        succs[i]->unref("find unref succ");
        if (succs[i]->isInvalid())
        {
          succs[i]->freedAt = timestamp.fetch_add(1);
        }
        succs[i] = nullptr;
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);

    return result;
  }

  // TODO: unsafe, consider keeping reference
  V *getReference(const K &key)
  {
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    HazardPointerRecord *predsRec = hazardAllocator.acquire(allocator);
    HazardPointerRecord *succsRec = hazardAllocator.acquire(allocator);

    bool found = find(key, preds, succs, predsRec, succsRec);

    V *result = nullptr;

    if (found && !succs[0]->marked.load(std::memory_order_acquire))
    {
      result = &(succs[0]->value);
    }

    for (uint32_t i = 0; i <= MAX_LEVEL; i++)
    {
      if (preds[i] != nullptr)
      {
        preds[i]->unref("get ref pred");
        if (preds[i]->isInvalid())
        {
          preds[i]->freedAt = timestamp.fetch_add(1);
        }
        preds[i] = nullptr;
      }

      if (succs[i] != nullptr)
      {
        succs[i]->unref("get ref succ");
        if (succs[i]->isInvalid())
        {
          succs[i]->freedAt = timestamp.fetch_add(1);
        }
        succs[i] = nullptr;
      }
    }

    hazardAllocator.release(predsRec);
    hazardAllocator.release(succsRec);
    return result;
  }

  int getSize() const
  {
    return size_.load(std::memory_order_relaxed);
  }

  bool isEmpty() const
  {
    return size_.load(std::memory_order_relaxed) == 0;
  }

  // void verifyReferenceCounts()
  // {
  //   Node *current = head;

  //   printf("=== Reference Count Verification ===\n");

  //   while (current)
  //   {
  //     uint32_t metadata = current->metadata.load(std::memory_order_acquire);
  //     uint32_t refCount = metadata & REFCOUNT_MASK;
  //     uint32_t expectedRefs = current->next.size();
  //     bool marked = (metadata & MARK_BIT) == 0;
  //     if (!(current == head || current == tail))
  //     {
  //       printf(
  //           "Node %p: key=%d, next.size()=%zu, refCount=%u, marked=%d, match=%s\n",
  //           current,
  //           (current == head || current == tail) ? -1 : current->key,
  //           current->next.size(),
  //           refCount,
  //           marked,
  //           (refCount == expectedRefs + ((current == tail || current == head) ? 1 : 0)) ? "YES" : "NO");

  //       if (refCount != expectedRefs)
  //       {
  //         printf("  ERROR: Reference count mismatch! Expected %zu, got %u\n", expectedRefs, refCount);
  //       }
  //     }

  //     if (current == tail)
  //     {
  //       break;
  //     }

  //     Node *next = current->next[0].load(std::memory_order_acquire);

  //     if (next != current->next[0].load(std::memory_order_acquire))
  //     {
  //       continue;
  //     }

  //     current = next;
  //   }

  //   printf("=== End Verification ===\n");
  // }

  class Iterator
  {
  private:
    Node *current;
    Node *tail;

    HazardPointerRecord *rec;
    HazardPointerManager &hazardManager;

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<const K &, V &>;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type *;
    using reference = value_type;

    Iterator(Node *start, Node *end, HazardPointerRecord *rec, HazardPointerManager &hazardManager) : current(start), tail(end), rec(rec), hazardManager(hazardManager)
    {
      while (current != tail && current->isMarked())
      {
        Node *prev = current;
        rec->assign(prev, 1);
        current = current->next[0].load(std::memory_order_acquire);
        rec->assign(current, 0);

        if (prev->next[0].load(std::memory_order_acquire) != current)
        {
          current = prev;
          rec->assign(current, 0);
          continue;
        }
      }
    }

    ~Iterator()
    {
      hazardManager.release(rec);
    }

    bool hasNext() const
    {
      return current != tail;
    }

    void next()
    {
      if (current != tail)
      {
        do
        {
          Node *prev = current;
          rec->assign(prev, 1);
          current = current->next[0].load(std::memory_order_acquire);

          if (!current)
          {
            current = tail;
            continue;
          }

          rec->assign(current, 0);

          if (prev->next[0].load(std::memory_order_acquire) != current)
          {
            current = prev;
            rec->assign(current, 0);
            continue;
          }

        } while (current != tail && current->isMarked());
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

    // Standard iterator interface for range-based for loops
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

    reference operator*()
    {
      return reference(current->key, current->value);
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

  Iterator begin()
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    Node *first = nullptr;

    while (true)
    {
      first = head->next[0].load(std::memory_order_acquire);

      rec->assign(first, 0);

      if (first == head->next[0].load(std::memory_order_acquire))
      {
        break;
      }
    }

    return Iterator(first, tail, rec, hazardAllocator);
  }

  Iterator end()
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(allocator);
    rec->assign(tail, 0);
    return Iterator(tail, tail, rec, hazardAllocator);
  }
};

} // namespace lib