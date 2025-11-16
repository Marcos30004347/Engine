
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <random>

namespace lib
{
template <typename K, typename V> class ConcurrentSkipListMap
{
private:
  static constexpr int MAX_LEVEL = 16;

  struct Node
  {
    K key;
    V value;
    std::atomic<bool> marked;
    std::vector<std::atomic<Node *>> next;

    Node(const K &k, const V &v, int level) : key(k), value(v), marked(false), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
      {
        next[i].store(nullptr, std::memory_order_relaxed);
      }
    }

    Node(int level) : marked(false), next(level + 1)
    {
      for (int i = 0; i <= level; ++i)
      {
        next[i].store(nullptr, std::memory_order_relaxed);
      }
    }
  };

  Node *head;
  Node *tail;
  std::atomic<int> size_;

  int randomLevel()
  {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::geometric_distribution<int> dist(0.5);
    return std::min(dist(gen), MAX_LEVEL);
  }

  bool find(const K &key, Node **preds, Node **succs)
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

        if (curr == tail || curr->key >= key)
        {
          break;
        }

        pred = curr;
        curr = succ;
      }

      preds[level] = pred;
      succs[level] = curr;
    }

    return curr != tail && curr->key == key;
  }

public:
  ConcurrentSkipListMap() : size_(0)
  {
    head = new Node(MAX_LEVEL);
    tail = new Node(MAX_LEVEL);

    for (int i = 0; i <= MAX_LEVEL; ++i)
    {
      head->next[i].store(tail, std::memory_order_relaxed);
    }
  }

  ~ConcurrentSkipListMap()
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
    int topLevel = randomLevel();
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    while (true)
    {
      bool found = find(key, preds, succs);
      if (found)
      {
        return false; // Key already exists
      }

      Node *newNode = new Node(key, value, topLevel);

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
        continue;
      }

      for (int level = 1; level <= topLevel; ++level)
      {
        while (true)
        {
          pred = preds[level];
          succ = succs[level];

          if (pred->next[level].compare_exchange_strong(succ, newNode, std::memory_order_release, std::memory_order_acquire))
          {
            break;
          }
          find(key, preds, succs);
        }
      }

      size_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  bool remove(const K &key)
  {
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];
    Node *victim = nullptr;

    bool found = find(key, preds, succs);
    if (!found)
    {
      return false;
    }

    victim = succs[0];

    for (int level = victim->next.size() - 1; level >= 1; --level)
    {
      Node *succ;
      do
      {
        succ = victim->next[level].load(std::memory_order_acquire);
        if (victim->marked.load(std::memory_order_acquire))
        {
          return false;
        }
      } while (!victim->next[level].compare_exchange_strong(succ, succ, std::memory_order_release, std::memory_order_acquire));
    }

    Node *succ = victim->next[0].load(std::memory_order_acquire);
    while (true)
    {
      bool marked = victim->marked.load(std::memory_order_acquire);
      if (marked)
      {
        return false;
      }
      if (victim->marked.compare_exchange_strong(marked, true, std::memory_order_release, std::memory_order_acquire))
      {
        break;
      }
    }

    find(key, preds, succs);
    size_.fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  bool find(const K &key, V &value)
  {
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = find(key, preds, succs);
    if (found && !succs[0]->marked.load(std::memory_order_acquire))
    {
      value = succs[0]->value;
      return true;
    }
    return false;
  }

  V *getReference(const K &key)
  {
    Node *preds[MAX_LEVEL + 1];
    Node *succs[MAX_LEVEL + 1];

    bool found = find(key, preds, succs);
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
    return size_.load(std::memory_order_relaxed) == 0;
  }

  class Iterator
  {
  private:
    Node *current;
    Node *tail;

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<const K &, V &>;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type *;
    using reference = value_type;

    Iterator(Node *start, Node *end) : current(start), tail(end)
    {
      while (current != tail && current->marked.load(std::memory_order_acquire))
      {
        current = current->next[0].load(std::memory_order_acquire);
      }
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
          current = current->next[0].load(std::memory_order_acquire);
        } while (current != tail && current->marked.load(std::memory_order_acquire));
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
    Node *first = head->next[0].load(std::memory_order_acquire);
    return Iterator(first, tail);
  }

  Iterator end()
  {
    return Iterator(tail, tail);
  }
};

} // namespace lib