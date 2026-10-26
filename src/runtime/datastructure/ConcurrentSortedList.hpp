#include "datastructure/HazardPointer.hpp"
#include "time/TimeSpan.hpp"
#include <atomic>

namespace lib
{

template <typename K> class ConcurrentSortedList
{
  struct Node
  {
  public:
    K data;

    std::atomic<Node *> next;

    Node(K data) : data(data), next(nullptr)
    {
    }
  };

  class ConcurrentNodeAllocator
  {
    std::atomic<Node *> head;
    std::atomic<int> size;

  public:
    ConcurrentNodeAllocator() : head(nullptr), size(0)
    {
    }

    ~ConcurrentNodeAllocator()
    {
      Node *curr = head.load();
      while (curr)
      {
        Node *next = curr->next.load();
        lib::memory::SystemMemoryManager::free(curr);
        curr = next;
      }
    }

    void insert(Node *newNode)
    {
      Node *oldHead = nullptr;
      do
      {
        oldHead = head.load();
        newNode->next.store(oldHead);
      } while (!head.compare_exchange_weak(oldHead, newNode));
      size.fetch_add(1);
    }

    bool tryPop(Node *&value)
    {

      while (true)
      {
        Node *oldHead = head.load();

        if (!oldHead)
        {
          return false;
        }

        if (head.load() != oldHead)
          continue;

        Node *newHead = oldHead->next.load();

        if (head.compare_exchange_strong(oldHead, newHead))
        {
          value = oldHead;
          size.fetch_sub(1);
          return true;
        }
      }
    }

    Node *allocate()
    {
      Node *node = nullptr;
      while (size.load())
      {
        if (tryPop(node))
        {
          return node;
        }
      }

      return (Node *)lib::memory::SystemMemoryManager::malloc(sizeof(Node));
    }

    void deallocate(Node *ptr)
    {
      const size_t CACHE_SIZE = 0;

      if (size.load() < CACHE_SIZE)
      {
        insert(ptr);
        return;
      }

      return lib::memory::SystemMemoryManager::free(ptr);
    }
  };

  ConcurrentNodeAllocator nodeAllocator;

  using HazardPointerManager = HazardPointer<3, Node, ConcurrentNodeAllocator>;
  using HazardPointerRecord = typename HazardPointerManager::Record;

  HazardPointerManager hazardAllocator;

  std::atomic<Node *> free;
  std::atomic<uint64_t> freeSize;

  std::atomic<Node *> head;

  std::atomic<uint64_t> size;

  bool find(K key, HazardPointerRecord *rec, Node *&curr, std::atomic<Node *> *&prev, Node *&next, std::atomic<Node *> *head)
  {
    bool found = false;

    while (true)
    {
      prev = head;
      if (prev == nullptr)
      {
        return false;
      }

      curr = prev->load();

      if (curr == nullptr)
      {
        break;
      }

      while (curr != nullptr)
      {
        rec->assign(curr, 0);

        if (prev->load() != curr)
        {
          break;
        }

        next = curr->next.load();

        if ((uintptr_t)next & 1)
        {
          Node *nextPtr = (Node *)((uintptr_t)next - 1);

          if (!prev->compare_exchange_strong(curr, nextPtr))
          {
            break;
          }

          rec->retire(curr);
          curr = nextPtr;
        }
        else
        {
          K& ckey = curr->data;

          if (prev->load() != curr)
          {
            break;
          }

          if (ckey >= key)
          {
            return ckey == key;
          }

          prev = &curr->next;

          rec->assign((Node *)rec->get(0), 2);
          rec->assign((Node *)rec->get(1), 0);
          rec->assign((Node *)rec->get(2), 1);

          curr = next;
        }
      }

      if (curr == nullptr)
      {
        break;
      }
    }

    return false;
  }

public:
  ConcurrentSortedList() : head(nullptr), nodeAllocator(), size(0)
  {
  }

  ~ConcurrentSortedList()
  {
    auto h = head.load();

    while (h)
    {
      auto tmp = h->next.load();
      nodeAllocator.deallocate(h);
      h = tmp;
    }
  }

  bool min(K &out)
  {
    auto *rec = hazardAllocator.acquire(nodeAllocator);

    do
    {
      auto curr = head.load();

      if (curr == nullptr)
      {
        hazardAllocator.release(rec);
        return false;
      }
      rec->assign(curr, 0);
      if (head.load() != curr)
      {
        continue;
      }

      out = curr->data;
      hazardAllocator.release(rec);
      return true;
    } while (true);
    hazardAllocator.release(rec);

    return false;
  }

  bool insert(K data)
  {
    auto *rec = hazardAllocator.acquire(nodeAllocator);

    Node *buff = nodeAllocator.allocate();
    Node *newNode = new (buff) Node(data);

    Node *curr = nullptr;
    Node *next = nullptr;

    std::atomic<Node *> *prev = nullptr;

    int iter = 0;

    while (true)
    {

      Node *curr = nullptr;
      Node *next = nullptr;

      std::atomic<Node *> *prev = nullptr;

      if (find(data, rec, curr, prev, next, &head))
      {
        rec->retire(newNode);
        hazardAllocator.release(rec);
        return false;
      }

      if (curr && curr->data <= data)
      {
        continue;
      }

      newNode->next.store(curr);

      if (prev->compare_exchange_strong(curr, newNode))
      {
        size.fetch_add(1);
        hazardAllocator.release(rec);
        return true;
      }
    }
  }

  bool remove(K data)
  {
    HazardPointerRecord *rec = hazardAllocator.acquire(nodeAllocator);

    while (true)
    {
      Node *curr = nullptr;
      Node *next = nullptr;

      std::atomic<Node *> *prev = nullptr;

      if (!find(data, rec, curr, prev, next, &head))
      {
        return false;
      }

      if (!curr->next.compare_exchange_strong(next, (Node *)((uintptr_t)next + 1)))
      {
        continue;
      }

      data = curr->data;

      if (prev->compare_exchange_strong(curr, next))
      {
        rec->retire(curr);
      }
      else
      {
        find(data, rec, curr, prev, next, &head);
      }

      size.fetch_sub(1);
      hazardAllocator.release(rec);
      return true;
    }
  }

  uint64_t length()
  {
    return size.load();
  }
};

} // namespace lib