#pragma once

#include "lib/datastructure/HazardPointer.hpp"
#include "lib/time/TimeSpan.hpp"
#include <atomic>

namespace lib
{

template <typename T, typename Allocator = memory::allocator::SystemAllocator<T>> class ConcurrentTimestampGarbageCollector
{
  template <typename K> class ConcurrentSortedLinkedList
  {
  public:
    struct Node
    {
    public:
      uint64_t key;
      K data;
      std::atomic<Node *> next;

      Node(uint64_t key, K data) : key(key), data(data), next(nullptr)
      {
      }
    };

    memory::allocator::SystemAllocator<Node> nodeAllocator;
    using HazardPointerManager = HazardPointer<3, Node, memory::allocator::SystemAllocator<Node>>;
    using HazardPointerRecord = typename HazardPointerManager::Record;

    HazardPointerManager hazardAllocator;

    std::atomic<Node *> head;

    std::atomic<uint64_t> size;

    ConcurrentSortedLinkedList() : head(nullptr), nodeAllocator(), size(0)
    {
    }

    ~ConcurrentSortedLinkedList()
    {
      auto h = head.load();

      while (h)
      {
        auto tmp = h->next.load();
        nodeAllocator.deallocate(h);
        h = tmp;
      }
    }

    bool front(K &out)
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

        // os::print("Thread %u curr = %u\n", os::Thread::getCurrentThreadId(), curr->key);

        out = curr->data;
        hazardAllocator.release(rec);
        return true;
      } while (true);
      hazardAllocator.release(rec);

      return false;
    }

    bool insert(uint64_t key, K data)
    {
      auto *rec = hazardAllocator.acquire(nodeAllocator);

      Node *buff = nodeAllocator.allocate(1);
      Node *newNode = new (buff) Node(key, data);

      Node *curr = nullptr;
      Node *next = nullptr;

      std::atomic<Node *> *prev = nullptr;

      int iter = 0;

      while (true)
      {

        Node *curr = nullptr;
        Node *next = nullptr;

        std::atomic<Node *> *prev = nullptr;

        if (find(key, rec, curr, prev, next, &head))
        {
          rec->retire(newNode);
          hazardAllocator.release(rec);
          return false;
        }

        // Node *p = prev->load();

        // if (curr)
        // {
        //   if ((uintptr_t)p & 1L)
        //   {
        //     p = (Node *)((uintptr_t)p - 1L);
        //   }
        // }

        if (curr && curr->key <= key)
        {
          //   os::print("iter = %u, %u %u\n", iter++, curr->key, key);
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

    bool get(uint64_t key, K &data)
    {
      HazardPointerRecord *rec = hazardAllocator.acquire(nodeAllocator);

      Node *buff = nodeAllocator.allocate(1);
      Node *newNode = new (buff) Node(key, data);

      Node *curr = nullptr;
      Node *next = nullptr;

      std::atomic<Node *> *prev = nullptr;

      int iter = 0;

      while (true)
      {

        Node *curr = nullptr;
        Node *next = nullptr;

        std::atomic<Node *> *prev = nullptr;

        if (find(key, rec, curr, prev, next, &head))
        {
          data = curr->data;
          rec->retire(newNode);
          hazardAllocator.release(rec);
          return true;
        }
        else
        {
          hazardAllocator.release(rec);
          return false;
        }
      }
    }
    bool find(uint64_t key, HazardPointerRecord *rec, Node *&curr, std::atomic<Node *> *&prev, Node *&next, std::atomic<Node *> *head)
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

        // os::print("Thread %u curr = %u, key = %u\n", os::Thread::getCurrentThreadId(), curr->key, key);

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
            size_t ckey = curr->key;

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

    bool remove(uint64_t key, K &data)
    {
      HazardPointerRecord *rec = hazardAllocator.acquire(nodeAllocator);

      while (true)
      {
        Node *curr = nullptr;
        Node *next = nullptr;

        std::atomic<Node *> *prev = nullptr;

        if (!find(key, rec, curr, prev, next, &head))
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
          find(key, rec, curr, prev, next, &head);
        }

        size.fetch_sub(1);
        hazardAllocator.release(rec);
        return true;
      }
    }

    bool min(uint64_t (*map)(K &), K &out)
    {
      uint64_t min = UINT64_MAX;
      HazardPointerRecord *rec = hazardAllocator.acquire(nodeAllocator);

      bool result = false;

      while (true)
      {
        auto prev = &head;
        Node *curr = prev->load();

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

          auto next = curr->next.load();

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
            uint64_t value = map(curr->data);

            if (value < min)
            {
              out = curr->data;
              min = value;
              result = true;
            }

            if (prev->load() != curr)
            {
              break;
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

      hazardAllocator.release(rec);
      return result;
    }

    uint64_t length()
    {
      return size.load();
    }
  };

public:
  // std::atomic<ConcurrentTimestampGarbageCollectorNode *> head;
  // std::atomic<ConcurrentTimestampGarbageCollectorNode *> tail;

  struct GarbageRecord
  {
    T **garbage;
    size_t size;
    uint64_t timestamp;
  };

  struct ThreadRecord
  {
    uint64_t threadId;
    uint64_t timestamp;
    ~ThreadRecord()
    {
    }
  };

  ConcurrentSortedLinkedList<ThreadRecord> activeThreads;
  ConcurrentSortedLinkedList<GarbageRecord> garbageRecords;

  std::atomic<uint64_t> minActiveTimestamp;
  std::atomic<uint64_t> timestamp;
  Allocator &allocator;

  ConcurrentTimestampGarbageCollector(Allocator &allocator) : allocator(allocator), garbageRecords(), activeThreads(), timestamp(0)
  {
  }

  ~ConcurrentTimestampGarbageCollector()
  {
    uint64_t threadTimestamp = minActiveTimestamp.load();

    GarbageRecord garbage;

    while (garbageRecords.front(garbage))
    {
      if (garbageRecords.remove(garbage.timestamp, garbage))
      {
        for (size_t i = 0; i < garbage.size; i++)
        {
          allocator.deallocate(garbage.garbage[i]);
        }

        delete[] garbage.garbage;
      }
    }
  }

  void openThreadContext()
  {
    ThreadRecord record;
    record.threadId = os::Thread::getCurrentThreadId();
    record.timestamp = timestamp.fetch_add(1);

    // os::print("Thread %u with timestamp %u started\n", os::Thread::getCurrentThreadId(), record.timestamp);
    if (!activeThreads.insert(record.threadId, record))
    {
      os::print("failed to start thread %u\n", os::Thread::getCurrentThreadId());
      assert(false);
    }
  }

  void closeThreadContext()
  {
    ThreadRecord record;

    if (!activeThreads.remove(os::Thread::getCurrentThreadId(), record))
    {
      os::print("failed to stop thread %u\n", os::Thread::getCurrentThreadId());
      assert(false);
    }

    uint64_t oldVal = minActiveTimestamp.load(std::memory_order_relaxed);

    while (oldVal > record.timestamp && !minActiveTimestamp.compare_exchange_weak(oldVal, record.timestamp, std::memory_order_release, std::memory_order_relaxed))
    {
    }
  }

  void free(T **garbage, size_t size)
  {
    GarbageRecord record;

    record.garbage = garbage;
    record.size = size;
    record.timestamp = timestamp.fetch_add(1) + 1;

    if (!garbageRecords.insert(record.timestamp, record))
    {
      os::print("failed to push garbage on thread %u\n", os::Thread::getCurrentThreadId());
      assert(false);
    }
  }

  static uint64_t getTimestamp(ThreadRecord &record)
  {
    return record.timestamp;
  }
  // void (*gc)(T *, uint64_t tts, uint64_t gts) = nullptr
  void collect()
  {
    uint64_t threadTimestamp = minActiveTimestamp.load();

    GarbageRecord garbage;

    while (garbageRecords.front(garbage))
    {
      // os::print("Thread %u collect garbage = %u, timestamp = %u\n", os::Thread::getCurrentThreadId(), garbage.timestamp, timestamp);

      if (garbage.timestamp >= minActiveTimestamp.load())
      {
        break;
      }

      if (garbageRecords.remove(garbage.timestamp, garbage))
      {
        // os::print("Thread %u removed hehe\n", os::Thread::getCurrentThreadId(), garbage.timestamp, timestamp);

        for (size_t i = 0; i < garbage.size; i++)
        {
          // if (gc)
          // {
          //   gc(garbage.garbage[i], threadTimestamp, garbage.timestamp);
          // }
          // else
          // {
          // os::print(
          //     "Thread %u, garbage timestamp = %u, current timestamp = %u, freeing %p\n",
          //     os::Thread::getCurrentThreadId(),
          //     garbage.timestamp,
          //     threadTimestamp,
          //     garbage.garbage[i]);
          allocator.deallocate(garbage.garbage[i]);
          // }
        }

        delete[] garbage.garbage;
      }
    }

    return;
  }
};
} // namespace lib