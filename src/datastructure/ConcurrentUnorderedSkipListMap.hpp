#pragma once

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace lib
{

template <typename K, typename V> class ConcurrentUnorderedSkipListMap
{
private:
  static constexpr int MAX_LEVEL = 16;

  struct Node
  {
    size_t hash;
    K key;
    V value;
    std::atomic<bool> marked;
    std::vector<std::atomic<Node *>> next;

    // data node
    Node(size_t h, const K &k, const V &v, int level) : hash(h), key(k), value(v), marked(false), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
        next[i].store(nullptr, std::memory_order_relaxed);
    }

    // sentinel node (head/tail)
    Node(size_t h, int level) : hash(h), key(), value(), marked(false), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
        next[i].store(nullptr, std::memory_order_relaxed);
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

  int randomLevel()
  {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::bernoulli_distribution coin(0.5);
    int lvl = 0;
    while (lvl < MAX_LEVEL && coin(gen))
      ++lvl;
    return lvl;
  }

  // Compare (hash, key) pairs. Return -1 if a < b, 0 if equal, +1 if a > b
  int cmp(size_t h1, const K &k1, size_t h2, const K &k2) const
  {
    if (h1 < h2)
      return -1;
    if (h1 > h2)
      return 1;
    // hashes equal -> tie-break via key
    if (k1 < k2)
      return -1;
    if (k2 < k1)
      return 1;
    return 0;
  }

  // find preds and succs for a given (hash, key)
  bool findByHashKey(size_t h, const K &key, Node **preds, Node **succs)
  {
    bool snip;
    Node *pred = nullptr;
    Node *curr = nullptr;
    Node *succ = nullptr;

  retry:
    pred = head;
    for (int level = MAX_LEVEL; level >= 0; --level)
    {
      curr = pred->next[level].load(std::memory_order_acquire);
      while (true)
      {
        if (curr == tail)
          break;
        succ = curr->next[level].load(std::memory_order_acquire);

        // physically remove marked nodes
        while (curr->marked.load(std::memory_order_acquire))
        {
          snip = pred->next[level].compare_exchange_strong(curr, succ, std::memory_order_release, std::memory_order_acquire);
          if (!snip)
            goto retry;
          curr = succ;
          if (curr == tail)
            break;
          succ = curr->next[level].load(std::memory_order_acquire);
        }

        if (curr == tail)
          break;

        int c = cmp(curr->hash, curr->key, h, key);
        if (c >= 0) // curr >= (h,key)
        {
          break;
        }

        pred = curr;
        curr = succ;
      }

      preds[level] = pred;
      succs[level] = curr;
    }

    // found if curr is not tail and equal (hash and key)
    return (curr != tail && curr->hash == h && !(curr->key < key) && !(key < curr->key));
  }

public:
  ConcurrentUnorderedSkipListMap() : size_(0)
  {
    head = new Node(0, MAX_LEVEL);                                  // smallest hash sentinel
    tail = new Node(std::numeric_limits<size_t>::max(), MAX_LEVEL); // largest hash sentinel
    for (int i = 0; i <= MAX_LEVEL; ++i)
      head->next[i].store(tail, std::memory_order_relaxed);
  }

  ~ConcurrentUnorderedSkipListMap()
  {
    Node *curr = head;
    while (curr != nullptr)
    {
      Node *next = curr->next[0].load(std::memory_order_acquire);
      delete curr;
      curr = next;
    }
  }

  bool insert(const K &key, const V &value)
  {
    size_t h = hasher(key);
    int topLevel = randomLevel();
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    while (true)
    {
      bool found = findByHashKey(h, key, preds, succs);
      if (found)
      {
        return false; // Key already exists
      }

      Node *newNode = new Node(h, key, value, topLevel);

      for (int level = 0; level <= topLevel; ++level)
      {
        Node *succ = succs[level];
        newNode->next[level].store(succ, std::memory_order_relaxed);
      }

      Node *pred = preds[0];
      Node *succ = succs[0];
      if (!pred->next[0].compare_exchange_strong(succ, newNode, std::memory_order_release, std::memory_order_acquire))
      {
        delete newNode;
        continue; // retry
      }

      // link higher levels
      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];
          if (pred->next[level].compare_exchange_strong(succ, newNode, std::memory_order_release, std::memory_order_acquire))
            break;
          // refresh preds & succs and try again
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
    Node *victim = nullptr;

    bool found = findByHashKey(h, key, preds, succs);
    if (!found)
      return false;

    victim = succs[0];

    // mark higher-level pointers (logical removal)
    for (int level = victim->topLevel(); level >= 1; --level)
    {
      Node *succ;
      do
      {
        succ = victim->next[level].load(std::memory_order_acquire);
        if (victim->marked.load(std::memory_order_acquire))
          return false; // already marked by someone else
      } while (!victim->next[level].compare_exchange_strong(succ, succ, std::memory_order_release, std::memory_order_acquire));
    }

    // mark level 0 atomically
    Node *succ0 = victim->next[0].load(std::memory_order_acquire);
    while (true)
    {
      bool marked = victim->marked.load(std::memory_order_acquire);
      if (marked)
        return false;
      if (victim->marked.compare_exchange_strong(marked, true, std::memory_order_release, std::memory_order_acquire))
        break;
    }

    // help unlink by calling find to physically remove it
    findByHashKey(h, key, preds, succs);
    size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  bool find(const K &key, V &value)
  {
    size_t h = hasher(key);
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = findByHashKey(h, key, preds, succs);
    if (found && !succs[0]->marked.load(std::memory_order_acquire))
    {
      value = succs[0]->value;
      return true;
    }
    return false;
  }

  V *getReference(const K &key)
  {
    size_t h = hasher(key);
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = findByHashKey(h, key, preds, succs);
    if (found && !succs[0]->marked.load(std::memory_order_acquire))
    {
      return &(succs[0]->value);
    }
    return nullptr;
  }

  int getSize() const
  {
    return size_.load(std::memory_order_relaxed);
  }
  bool isEmpty() const
  {
    return getSize() == 0;
  }

  // forward iterator over live (unmarked) nodes in hash order
  class Iterator
  {
  private:
    Node *current;
    Node *tail;

    void advance_to_next_live()
    {
      while (current != tail && current->marked.load(std::memory_order_acquire))
        current = current->next[0].load(std::memory_order_acquire);
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<const K &, V &>;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type *;
    using reference = value_type;

    Iterator(Node *start, Node *end) : current(start), tail(end)
    {
      advance_to_next_live();
    }

    bool hasNext() const
    {
      return current != tail;
    }

    void next()
    {
      if (current != tail)
      {
        current = current->next[0].load(std::memory_order_acquire);
        advance_to_next_live();
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
    Node *first = head->next[0].load(std::memory_order_acquire);
    return Iterator(first, tail);
  }

  Iterator end()
  {
    return Iterator(tail, tail);
  }
};

} // namespace lib
