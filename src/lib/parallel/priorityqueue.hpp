#pragma once

#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace lib
{
namespace parallel
{

namespace epoch_manager
{
constexpr int MAX_THREADS = 64;
std::atomic<uint64_t> global_epoch{0};
std::atomic<uint64_t> thread_epochs[MAX_THREADS];
std::atomic<int> thread_count{0};
std::mutex registration_mutex;

thread_local int thread_id = -1;

uint64_t get_current_epoch()
{
  return global_epoch.load(std::memory_order_relaxed);
}

uint64_t get_min_active_epoch()
{
  uint64_t min_epoch = std::numeric_limits<uint64_t>::max();
  for (int i = 0; i < thread_count.load(std::memory_order_relaxed); ++i)
  {
    min_epoch = std::min(min_epoch, thread_epochs[i].load(std::memory_order_acquire));
  }
  return min_epoch;
}

void enter_epoch()
{
  if (thread_id == -1)
  {
    std::lock_guard<std::mutex> lock(registration_mutex);
    thread_id = thread_count.fetch_add(1, std::memory_order_relaxed);
    assert(thread_id < MAX_THREADS);
  }
  thread_epochs[thread_id].store(global_epoch.load(std::memory_order_relaxed), std::memory_order_release);
}

void advance_epoch()
{
  global_epoch.fetch_add(1, std::memory_order_release);
}
} // namespace epoch_manager

template <typename Key, typename Value, int MaxLevels = 32> class PriorityQueue
{
private:
  struct Node
  {
    Key key;
    Value value;
    int level;

    std::atomic<Node *> next[MaxLevels];
    std::atomic<bool> inserting;

    Node(int lvl) : level(lvl), inserting(true)
    {
      for (int i = 0; i < MaxLevels; ++i)
      {
        next[i].store(nullptr, std::memory_order_relaxed);
      }
    }

    ~Node()
    {
    }
  };

  Node *head;
  Node *tail;

  thread_local static std::mt19937 rng;

  static thread_local std::vector<std::vector<Node *>> garbage_lists;
  static thread_local uint64_t last_reclamation_epoch;

  static int random_level()
  {
    uint32_t r = rng();
    r |= 1;
    return __builtin_ctz(r) + 1;
  }

  static Node *get_unmarked(Node *ptr)
  {
    return reinterpret_cast<Node *>(reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(1));
  }

  static Node *mark(Node *ptr)
  {
    return reinterpret_cast<Node *>(reinterpret_cast<uintptr_t>(ptr) | uintptr_t(1));
  }

  static bool is_marked(Node *ptr)
  {
    return reinterpret_cast<uintptr_t>(ptr) & uintptr_t(1);
  }

  void retire_node(Node *node)
  {
    uint64_t current_epoch = epoch_manager::get_current_epoch();
    if (garbage_lists.size() <= current_epoch)
    {
      garbage_lists.resize(current_epoch + 1);
    }
    garbage_lists[current_epoch].push_back(node);
    reclaim_nodes();
  }

  void reclaim_nodes()
  {
    uint64_t min_active_epoch = epoch_manager::get_min_active_epoch();
    
    for (uint64_t epoch = last_reclamation_epoch; epoch < min_active_epoch && epoch < garbage_lists.size(); ++epoch)
    {
      for (Node *node : garbage_lists[epoch])
      {
        delete node;
      }

      garbage_lists[epoch].clear();
    }
    last_reclamation_epoch = std::max(last_reclamation_epoch, min_active_epoch);
  }

private:
  Node *locate_preds(Key k, Node *preds[], Node *succs[])
  {
    Node *x = head;
    Node *del = nullptr;

    for (int i = MaxLevels - 1; i >= 0; --i)
    {
      Node *x_next = x->next[i].load(std::memory_order_acquire);
      bool d = is_marked(x_next);
      x_next = get_unmarked(x_next);

      while (x_next->key < k || is_marked(x_next->next[0].load()) || (i == 0 && d))
      {
        if (i == 0 && d)
          del = x_next;
        x = x_next;
        x_next = x->next[i].load(std::memory_order_acquire);
        d = is_marked(x_next);
        x_next = get_unmarked(x_next);
      }
      preds[i] = x;
      succs[i] = x_next;
    }
    return del;
  }

public:
  PriorityQueue()
  {
    head = new Node(MaxLevels);
    tail = new Node(MaxLevels);
    Key minKey = std::numeric_limits<Key>::lowest();
    Key maxKey = std::numeric_limits<Key>::max();
    head->key = minKey;
    tail->key = maxKey;

    for (int i = 0; i < MaxLevels; ++i)
    {
      head->next[i].store(tail, std::memory_order_relaxed);
    }
  }

  ~PriorityQueue()
  {
    for (const auto &epoch_list : garbage_lists)
    {
      for (Node *node : epoch_list)
      {
        delete node;
      }
    }
    Node *node = get_unmarked(head);
    while (node)
    {
      Node *next_ptr = node->next[0].load(std::memory_order_relaxed);
      Node *next = get_unmarked(next_ptr);
      delete node;
      if (is_marked(next_ptr))
      {
        break;
      }
      node = next;
    }
  }

public:
  void insert(Key k, Value v)
  {
    epoch_manager::enter_epoch();
    assert(k > std::numeric_limits<Key>::lowest() && k < std::numeric_limits<Key>::max());

    Node *preds[MaxLevels];
    Node *succs[MaxLevels];
    Node *new_node = nullptr;

    while (true)
    {
      Node *del = locate_preds(k, preds, succs);

      if (succs[0]->key == k && !is_marked(preds[0]->next[0].load()) && preds[0]->next[0].load() == succs[0])
      {
        if (new_node)
        {
          delete new_node;
        }
        return;
      }

      if (!new_node)
      {
        int lvl = random_level();
        new_node = new Node(lvl);
        new_node->key = k;
        new_node->value = v;
      }

      new_node->next[0].store(succs[0], std::memory_order_relaxed);

      if (!preds[0]->next[0].compare_exchange_weak(succs[0], new_node, std::memory_order_release, std::memory_order_relaxed))
      {
        continue;
      }
      bool success = false;

      for (int i = 1; i < new_node->level && !success; ++i)
      {
        while (true)
        {
          if (is_marked(new_node->next[0].load()) || is_marked(succs[i]->next[0].load()) || del == succs[i])
          {
            success = true;
            break;
          }

          new_node->next[i].store(succs[i], std::memory_order_relaxed);

          if (preds[i]->next[i].compare_exchange_weak(succs[i], new_node, std::memory_order_release, std::memory_order_relaxed))
          {
            break;
          }

          del = locate_preds(k, preds, succs);
          if (succs[0] != new_node)
          {
            success = true;
            break;
          }
        }
      }

      new_node->inserting.store(false, std::memory_order_release);
      return;
    }
  }

  bool deletemin(Value &result)
  {
    epoch_manager::enter_epoch();
    Node *x = head;
    Node *newhead = nullptr;

    while (true)
    {
      Node *nxt = x->next[0].load(std::memory_order_acquire);

      if (get_unmarked(nxt) == tail)
      {
        return false;
      }

      if (!newhead && x->inserting.load(std::memory_order_acquire))
      {
        newhead = x;
      }

      if (is_marked(nxt))
      {
        x = get_unmarked(nxt);
        continue;
      }

      Node *expected = nxt;
      if (x->next[0].compare_exchange_strong(expected, mark(expected), std::memory_order_acq_rel, std::memory_order_relaxed))
      {
        result = get_unmarked(expected)->value;
        retire_node(get_unmarked(expected));
        return true;
      }
      else
      {
        x = get_unmarked(expected);
      }
    }
  }
};

template <typename Key, typename Value, int MaxLevels> thread_local std::mt19937 PriorityQueue<Key, Value, MaxLevels>::rng(std::random_device{}());
template <typename Key, typename Value, int MaxLevels>
thread_local std::vector<std::vector<typename PriorityQueue<Key, Value, MaxLevels>::Node *>> PriorityQueue<Key, Value, MaxLevels>::garbage_lists;
template <typename Key, typename Value, int MaxLevels> thread_local uint64_t PriorityQueue<Key, Value, MaxLevels>::last_reclamation_epoch = 0;

} // namespace parallel
} // namespace lib