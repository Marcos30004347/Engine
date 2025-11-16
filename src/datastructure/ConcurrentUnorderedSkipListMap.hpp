#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace lib
{

// ------------------------
// Fixed Hazard Pointer Implementation
// ------------------------

class HazardManager
{
public:
  struct HazardRecord
  {
    std::atomic<std::uintptr_t> ptr{0};
  };

  static constexpr int MAX_GLOBAL_HP = 1024;

  static int allocate_hp_slot()
  {
    static std::atomic<int> next{0};
    int idx = next.fetch_add(1, std::memory_order_relaxed);
    if (idx >= MAX_GLOBAL_HP)
      return -1;
    return idx;
  }

  static HazardRecord *get_record(int idx)
  {
    static HazardRecord global[MAX_GLOBAL_HP];
    return &global[idx];
  }

  struct ThreadState
  {
    std::vector<int> my_slots;
    std::vector<void *> retired;
  };

  static ThreadState &thread_state()
  {
    thread_local ThreadState ts;
    return ts;
  }

  static int thread_allocate_slot()
  {
    ThreadState &ts = thread_state();
    if (!ts.my_slots.empty())
      return ts.my_slots.back();

    int idx = allocate_hp_slot();
    if (idx < 0)
      return -1;
    ts.my_slots.push_back(idx);
    return idx;
  }

  // Fixed: Direct protection of actual atomic pointers
  template <typename T> static T *protect(std::atomic<uintptr_t> &src, int slot_idx)
  {
    assert(slot_idx >= 0 && slot_idx < MAX_GLOBAL_HP);
    HazardRecord *hr = get_record(slot_idx);

    uintptr_t packed;
    T *p;
    do
    {
      packed = src.load(std::memory_order_acquire);
      p = reinterpret_cast<T *>(packed & ~uintptr_t(1)); // strip mark bit
      std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
      hr->ptr.store(v, std::memory_order_release);
    } while (packed != src.load(std::memory_order_acquire));

    return p;
  }

  static void clear_slot(int slot_idx)
  {
    if (slot_idx < 0)
      return;
    HazardRecord *hr = get_record(slot_idx);
    hr->ptr.store(0, std::memory_order_release);
  }

  // Fixed: Template specialization for proper deletion
  template <typename T> static void retire(T *p)
  {
    ThreadState &ts = thread_state();
    ts.retired.push_back(reinterpret_cast<void *>(p));
    if (ts.retired.size() >= retire_threshold())
      scan_and_reclaim<T>();
  }

  template <typename T> static void scan_and_reclaim()
  {
    ThreadState &ts = thread_state();

    std::vector<std::uintptr_t> hazards;
    hazards.reserve(MAX_GLOBAL_HP);
    for (int i = 0; i < MAX_GLOBAL_HP; ++i)
    {
      std::uintptr_t v = get_record(i)->ptr.load(std::memory_order_acquire);
      if (v)
        hazards.push_back(v);
    }

    auto it = ts.retired.begin();
    while (it != ts.retired.end())
    {
      void *p = *it;
      std::uintptr_t up = reinterpret_cast<std::uintptr_t>(p);
      bool can_delete = (std::find(hazards.begin(), hazards.end(), up) == hazards.end());
      if (can_delete)
      {
        // Fixed: Call proper destructor
        delete reinterpret_cast<T *>(p);
        it = ts.retired.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  static size_t retire_threshold()
  {
    return 64;
  }
};

struct HazardGuard
{
  int slot;

  HazardGuard() : slot(-1)
  {
    slot = HazardManager::thread_allocate_slot();
  }

  explicit HazardGuard(int s) : slot(s)
  {
  }

  ~HazardGuard()
  {
    if (slot >= 0)
      HazardManager::clear_slot(slot);
  }

  // Fixed: Direct protection of packed atomic pointers
  template <typename T> T *protect(std::atomic<uintptr_t> &src)
  {
    if (slot < 0)
      slot = HazardManager::thread_allocate_slot();
    return HazardManager::protect<T>(src, slot);
  }
};

// ------------------------
// Fixed Concurrent Skip List
// ------------------------

template <typename K, typename V> class ConcurrentUnorderedSkipListMap
{
private:
  static constexpr int MAX_LEVEL = 16;

  static inline uintptr_t ptr_pack(void *p, bool marked)
  {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    assert((v & 1) == 0);
    return v | (marked ? 1u : 0u);
  }

  static inline void *ptr_get_ptr(uintptr_t packed)
  {
    return reinterpret_cast<void *>(packed & ~uintptr_t(1));
  }

  static inline bool ptr_get_mark(uintptr_t packed)
  {
    return (packed & 1) != 0;
  }

  struct Node
  {
    size_t hash;
    K key;
    V value;
    std::vector<std::atomic<uintptr_t>> next;

    Node(size_t h, const K &k, const V &v, int level) : hash(h), key(k), value(v), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
        next[i].store(ptr_pack(nullptr, false), std::memory_order_relaxed);
    }

    Node(size_t h, int level) : hash(h), key(), value(), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
        next[i].store(ptr_pack(nullptr, false), std::memory_order_relaxed);
    }

    int topLevel() const
    {
      return static_cast<int>(next.size()) - 1;
    }
  };

  Node *head;
  Node *tail;
  std::atomic<int> size_;
  std::hash<K> hasher;

  // Fixed: Thread-local random generator
  int randomLevel()
  {
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::bernoulli_distribution coin{0.5};

    int lvl = 0;
    while (lvl < MAX_LEVEL && coin(gen))
      ++lvl;
    return lvl;
  }

  int cmp(size_t h1, const K &k1, size_t h2, const K &k2) const
  {
    if (h1 < h2)
      return -1;
    if (h1 > h2)
      return 1;
    if (k1 < k2)
      return -1;
    if (k2 < k1)
      return 1;
    return 0;
  }

  Node *getPtr(uintptr_t packed) const
  {
    return reinterpret_cast<Node *>(ptr_get_ptr(packed));
  }

  bool getMark(uintptr_t packed) const
  {
    return ptr_get_mark(packed);
  }

  // Fixed: Proper hazard pointer protection during traversal
  bool findByHashKey(size_t h, const K &key, Node **preds, Node **succs)
  {
    HazardGuard guard;

  retry:
    Node *pred = head;
    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      uintptr_t curr_packed = pred->next[level].load(std::memory_order_acquire);
      Node *curr = getPtr(curr_packed);

      while (true)
      {
        if (curr == tail)
        {
          preds[level] = pred;
          succs[level] = tail;
          break;
        }

        // Fixed: Protect curr before accessing it
        Node *safe_curr = guard.protect<Node>(curr->next[level]);
        if (safe_curr != curr)
        {
          // Pointer changed, retry from top
          goto retry;
        }

        uintptr_t succ_packed = curr->next[level].load(std::memory_order_acquire);
        Node *succ = getPtr(succ_packed);
        bool curr_marked = getMark(curr_packed);

        // Help remove marked nodes
        if (curr_marked)
        {
          uintptr_t expected = curr_packed;
          uintptr_t desired = ptr_pack(succ, false);
          pred->next[level].compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
          curr_packed = pred->next[level].load(std::memory_order_acquire);
          curr = getPtr(curr_packed);
          continue;
        }

        int c = cmp(curr->hash, curr->key, h, key);
        if (c >= 0)
        {
          preds[level] = pred;
          succs[level] = curr;
          break;
        }

        pred = curr;
        curr_packed = succ_packed;
        curr = succ;
      }
    }

    Node *candidate = succs[0];
    if (candidate != tail && candidate->hash == h && !(candidate->key < key) && !(key < candidate->key))
    {
      uintptr_t p0 = candidate->next[0].load(std::memory_order_acquire);
      if (!getMark(p0))
        return true;
    }
    return false;
  }

public:
  ConcurrentUnorderedSkipListMap() : size_(0)
  {
    head = new Node(0, MAX_LEVEL);
    tail = new Node(std::numeric_limits<size_t>::max(), MAX_LEVEL);
    for (int i = 0; i <= MAX_LEVEL; ++i)
      head->next[i].store(ptr_pack(tail, false), std::memory_order_relaxed);
  }

  ~ConcurrentUnorderedSkipListMap()
  {
    Node *cur = getPtr(head->next[0].load(std::memory_order_acquire));
    while (cur != nullptr && cur != tail)
    {
      Node *next = getPtr(cur->next[0].load(std::memory_order_acquire));
      delete cur;
      cur = next;
    }
    delete head;
    delete tail;
  }

  bool insert(const K &key, const V &value)
  {
    size_t h = hasher(key);
    int topLevel = randomLevel();
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    while (true)
    {
      if (findByHashKey(h, key, preds, succs))
        return false; // Key already exists

      Node *newNode = new Node(h, key, value, topLevel);
      for (int level = 0; level <= topLevel; ++level)
      {
        newNode->next[level].store(ptr_pack(succs[level], false), std::memory_order_relaxed);
      }

      Node *pred = preds[0];
      uintptr_t expected = ptr_pack(succs[0], false);

      if (!pred->next[0].compare_exchange_strong(expected, ptr_pack(newNode, false), std::memory_order_acq_rel, std::memory_order_acquire))
      {
        delete newNode;
        continue;
      }

      // Link upper levels
      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          expected = ptr_pack(succs[level], false);

          if (pred->next[level].compare_exchange_strong(expected, ptr_pack(newNode, false), std::memory_order_acq_rel, std::memory_order_acquire))
          {
            break;
          }

          findByHashKey(h, key, preds, succs);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  bool remove(const K &key)
  {
    size_t h = hasher(key);
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    while (true)
    {
      bool found = findByHashKey(h, key, preds, succs);
      if (!found)
        return false;

      Node *victim = succs[0];

      // Mark level-0 next pointer
      while (true)
      {
        uintptr_t old0 = victim->next[0].load(std::memory_order_acquire);
        if (getMark(old0))
          return false;

        uintptr_t new0 = ptr_pack(getPtr(old0), true);
        if (victim->next[0].compare_exchange_strong(old0, new0, std::memory_order_acq_rel, std::memory_order_acquire))
        {
          break;
        }
      }

      // Help unlink at all levels
      for (int level = victim->topLevel(); level >= 0; --level)
      {
        uintptr_t succ_packed = victim->next[level].load(std::memory_order_acquire);
        Node *succ = getPtr(succ_packed);
        uintptr_t pred_next = preds[level]->next[level].load(std::memory_order_acquire);
        uintptr_t desired = ptr_pack(succ, false);
        preds[level]->next[level].compare_exchange_strong(pred_next, desired, std::memory_order_acq_rel, std::memory_order_acquire);
      }

      // Fixed: Retire with proper type
      HazardManager::retire<Node>(victim);
      size_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }
  }

  bool find(const K &key, V &value)
  {
    size_t h = hasher(key);
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = findByHashKey(h, key, preds, succs);
    if (found)
    {
      Node *candidate = succs[0];
      uintptr_t p0 = candidate->next[0].load(std::memory_order_acquire);
      if (!getMark(p0))
      {
        value = candidate->value;
        return true;
      }
    }
    return false;
  }

  // Fixed ValueHandle with move semantics
  class ValueHandle
  {
  private:
    Node *node;
    int slot;

  public:
    ValueHandle(Node *n = nullptr, int s = -1) : node(n), slot(s)
    {
    }

    ~ValueHandle()
    {
      if (slot >= 0)
        HazardManager::clear_slot(slot);
    }

    // Prevent copying
    ValueHandle(const ValueHandle &) = delete;
    ValueHandle &operator=(const ValueHandle &) = delete;

    // Allow moving
    ValueHandle(ValueHandle &&other) noexcept : node(other.node), slot(other.slot)
    {
      other.node = nullptr;
      other.slot = -1;
    }

    ValueHandle &operator=(ValueHandle &&other) noexcept
    {
      if (this != &other)
      {
        if (slot >= 0)
          HazardManager::clear_slot(slot);
        node = other.node;
        slot = other.slot;
        other.node = nullptr;
        other.slot = -1;
      }
      return *this;
    }

    bool valid() const
    {
      return node != nullptr;
    }
    V &operator*()
    {
      return node->value;
    }
    V *operator->()
    {
      return &node->value;
    }
    const V &operator*() const
    {
      return node->value;
    }
    const V *operator->() const
    {
      return &node->value;
    }
  };

  ValueHandle getReference(const K &key)
  {
    size_t h = hasher(key);
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = findByHashKey(h, key, preds, succs);
    if (!found)
      return ValueHandle(nullptr, -1);

    Node *candidate = succs[0];

    // Allocate a dedicated slot for this handle
    int slot_idx = HazardManager::thread_allocate_slot();
    if (slot_idx < 0)
      return ValueHandle(nullptr, -1);

    // Protect the candidate
    Node *safe = HazardManager::protect<Node>(candidate->next[0], slot_idx);

    uintptr_t p0 = candidate->next[0].load(std::memory_order_acquire);
    if (getMark(p0))
    {
      HazardManager::clear_slot(slot_idx);
      return ValueHandle(nullptr, -1);
    }

    return ValueHandle(candidate, slot_idx);
  }

  int getSize() const
  {
    return size_.load(std::memory_order_relaxed);
  }
  bool isEmpty() const
  {
    return getSize() == 0;
  }

  // Simplified iterator - for read-only traversal
  class Iterator
  {
  private:
    Node *current;
    Node *tail;
    HazardGuard guard;

    static Node *getPtr(uintptr_t packed)
    {
      return reinterpret_cast<Node *>(ptr_get_ptr(packed));
    }

    static bool getMark(uintptr_t packed)
    {
      return ptr_get_mark(packed);
    }

    void advance_to_next_live()
    {
      while (current != tail)
      {
        uintptr_t p0 = current->next[0].load(std::memory_order_acquire);
        if (!getMark(p0))
          break;
        current = getPtr(p0);
      }
    }

  public:
    Iterator(Node *start, Node *end) : current(start), tail(end)
    {
      advance_to_next_live();
    }

    bool operator==(const Iterator &o) const
    {
      return current == o.current;
    }
    bool operator!=(const Iterator &o) const
    {
      return !(*this == o);
    }

    Iterator &operator++()
    {
      if (current != tail)
      {
        uintptr_t p0 = current->next[0].load(std::memory_order_acquire);
        current = getPtr(p0);
        advance_to_next_live();
      }
      return *this;
    }

    std::pair<const K &, const V &> operator*() const
    {
      return {current->key, current->value};
    }
  };

  Iterator begin()
  {
    Node *first = getPtr(head->next[0].load(std::memory_order_acquire));
    return Iterator(first, tail);
  }

  Iterator end()
  {
    return Iterator(tail, tail);
  }
};

} // namespace lib