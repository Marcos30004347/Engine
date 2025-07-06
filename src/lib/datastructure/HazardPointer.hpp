#pragma once

#include "lib/algorithm/search.hpp"
#include "lib/algorithm/sort.hpp"
#include "lib/datastructure/ThreadLocalStorage.hpp"
#include "lib/datastructure/Vector.hpp"
#include "lib/memory/allocator/SystemAllocator.hpp"

#include <atomic>
#include <unordered_set>

namespace lib
{

template <size_t K, typename T, typename Allocator = memory::allocator::SystemAllocator<T>> class HazardPointer
{
public:
  class Record
  {
    const int R = 16;
    friend class HazardPointer;

    HazardPointer *manager;
    Record *next;

    std::atomic_flag isActive;
    void *pointers[K];

    Vector<void *> retiredList;
    Allocator &allocator;

    Record(HazardPointer *manager, Allocator &allocator) : allocator(allocator), retiredList(), isActive(false), next(nullptr), manager(manager)
    {
      for (size_t i = 0; i < K; i++)
      {
        pointers[i] = nullptr;
      }
    }

    ~Record()
    {
      for (size_t i = 0; i < retiredList.size(); i++)
      {
        allocator.deallocate((T*)retiredList[i]);
      }
    }

    void helpScan()
    {
      // Accumulate retired nodes from inactive records
      for (Record *hprec = manager->head.load(); hprec != nullptr; hprec = hprec->next)
      {
        if (hprec->isActive.test())
        {
          continue;
        }

        if (hprec->isActive.test_and_set(std::memory_order_acquire))
        {
          continue;
        }

        while (hprec->retiredList.size() > 0)
        {
          void *node = hprec->retiredList[hprec->retiredList.size() - 1];
          hprec->retiredList.popBack();

          retiredList.pushBack(node);

          if (retiredList.size() > R)
          {
            scan(manager->head.load());
          }
        }

        hprec->isActive.clear();
      }
    }

    void scan(Record *h)
    {
      lib::Vector<void *> hp;
      size_t i = 0;

      while (h)
      {
        for (size_t k = 0; k < K; k++)
        {
          if (h->pointers[k] != nullptr && h->isActive.test())
          {
            hp.pushBack(h->pointers[k]);
          }
        }

        h = h->next;
      }

      lib::algorithm::sort::quickSort(hp.buffer(), hp.size());

      for (size_t i = 0; i < retiredList.size();)
      {
        size_t index = lib::algorithm::search::binarySearch(hp.buffer(), retiredList[i], hp.size());

        if (index == hp.size()) // not found
        {
          // os::print("deallocating %p\n", retiredList[i]);

          allocator.deallocate((T *)retiredList[i]);

          if (i != retiredList.size() - 1)
          {
            retiredList[i] = retiredList[retiredList.size() - 1];
            retiredList[retiredList.size() - 1] = nullptr;
          }

          retiredList.popBack();
        }
        else
        {
          i++;
        }
      }
    }

  public:
    inline void assign(T *ref, uint32_t index = 0)
    {
      // os::print("assigning %p\n", ref);
      pointers[index] = (void *)ref;
    }

    inline void unassign(uint32_t index = 0)
    {
      pointers[index] = nullptr;
    }
    inline void *get(uint32_t index)
    {
      return pointers[index];
    }
    // void retire(uint32_t index)
    // {
    //   retiredList.pushBack((void *)pointers[index]);

    //   // os::print("retiring %p\n", pointers[index]);

    //   pointers[index] = nullptr;

    //   if (retiredList.size() >= R)
    //   {
    //     scan(manager->head);
    //     helpScan();
    //   }
    // }

    void retire(void *ptr)
    {
      retiredList.pushBack(ptr);

      // os::print("retiring %p\n", ptr);

      if (retiredList.size() >= R)
      {
        scan(manager->head);
        helpScan();
      }
    }
  };

  HazardPointer() : head(nullptr), listLen(0)
  {
  }

  ~HazardPointer()
  {
    Record *curr = head.load();
    while (curr)
    {
      // assert(!curr->isActive.test());
      Record *tmp = curr->next;

      delete curr;
      curr = tmp;
    }
  }

  Record *acquire(Allocator &allocator)
  {
    Record *p = head;

    for (; p; p = p->next)
    {
      int expected = 0;

      if (p->isActive.test(std::memory_order_acquire) || p->isActive.test_and_set(std::memory_order_acquire))
      {
        continue;
      }

      return p;
    }

    int oldLen = listLen.load();
    do
    {
      oldLen = listLen.load();
    } while (!listLen.compare_exchange_weak(oldLen, oldLen + 1, std::memory_order_release, std::memory_order_relaxed));

    p = new Record(this, allocator);

    assert(!p->isActive.test_and_set(std::memory_order_acquire));

    for (size_t i = 0; i < K; i++)
    {
      p->pointers[i] = NULL;
    }

    Record *old = head;

    do
    {
      old = head;
      p->next = old;
    } while (!head.compare_exchange_weak(old, p, std::memory_order_release, std::memory_order_relaxed));

    return p;
  }

  void release(Record *rec)
  {
    for (size_t i = 0; i < K; i++)
    {
      rec->pointers[i] = nullptr;
    }

    assert(rec->isActive.test());
    rec->isActive.clear();
  }

private:
  std::atomic<Record *> head;
  std::atomic<int> listLen;
};

} // namespace lib