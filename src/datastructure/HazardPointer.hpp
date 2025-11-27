#pragma once

#include "algorithm/search.hpp"
#include "algorithm/sort.hpp"
#include "datastructure/ThreadLocalStorage.hpp"
#include "memory/allocator/SystemAllocator.hpp"

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

    std::atomic<uint32_t> refs;

    HazardPointer *manager;
    Record *next;

    std::atomic<bool> isActive;
    void *pointers[K];

    std::vector<void *> retiredList;
    Allocator &allocator;

    Record(HazardPointer *manager, Allocator &allocator) : allocator(allocator), retiredList(), isActive(false), next(nullptr), manager(manager), refs(0)
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
        allocator.deallocate((T *)retiredList[i]);
      }

      retiredList.clear();
    }

    void helpScan()
    {
      // Accumulate retired nodes from inactive records
      for (Record *hprec = manager->head.load(); hprec != nullptr; hprec = hprec->next)
      {
        bool expected = false;

        if (!hprec->isActive.compare_exchange_strong(expected, true))
        {
          continue;
        }

        // hprec->retiredLock.lock();

        while (hprec->retiredList.size() > 0)
        {
          void *node = hprec->retiredList[hprec->retiredList.size() - 1];

          hprec->retiredList.pop_back();
          retiredList.push_back(node);

          if (retiredList.size() > R)
          {
            scan(manager->head.load());
          }
        }
        // hprec->retiredLock.unlock();

        expected = true;

        bool exchanged = hprec->isActive.compare_exchange_strong(expected, false);
        assert(exchanged);
      }
    }

    void scan(Record *h)
    {
      std::vector<void *> hp;
      size_t i = 0;

      while (h)
      {
        for (size_t k = 0; k < K; k++)
        {
          if (h->pointers[k] != nullptr && h->isActive.load())
          {
            hp.push_back(h->pointers[k]);
          }
        }

        h = h->next;
      }

      lib::algorithm::sort::quickSort(hp.data(), hp.size());

      for (size_t i = 0; i < retiredList.size();)
      {
        size_t index = lib::algorithm::search::binarySearch(hp.data(), retiredList[i], hp.size());

        if (index == hp.size()) // not found
        {
         // printf("deallocating %p\n", retiredList[i]);
          allocator.deallocate((T *)retiredList[i]);

          if (i != retiredList.size() - 1)
          {
            retiredList[i] = retiredList[retiredList.size() - 1];
            retiredList[retiredList.size() - 1] = nullptr;
          }

          retiredList.pop_back();
        }
        else
        {
          i++;
        }
      }
      //retiredLock.unlock();
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

    void retire(void *ptr)
    {
      retiredList.push_back(ptr);
      bool needScan = retiredList.size() >= R;

      if (needScan)
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
      bool expected = false;

      if (!p->isActive.compare_exchange_strong(expected, true))
      {
        continue;
      }

      uint32_t refs = p->refs.fetch_add(1);
      assert(refs == 0);

      return p;
    }

    int oldLen = listLen.load();
    do
    {
      oldLen = listLen.load();
    } while (!listLen.compare_exchange_weak(oldLen, oldLen + 1, std::memory_order_release, std::memory_order_relaxed));

    p = new Record(this, allocator);

    p->isActive.store(true);

    uint32_t refs = p->refs.fetch_add(1);

    assert(refs == 0);

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

    assert(p->refs.load() == 1);

    return p;
  }

  void release(Record *rec)
  {
    for (size_t i = 0; i < K; i++)
    {
      rec->pointers[i] = nullptr;
    }

    rec->refs.fetch_sub(1);

    assert(rec->refs.load() == 0);
    assert(rec->isActive.load());

    bool expected = true;
    bool exchanged = rec->isActive.compare_exchange_strong(expected, false);
    assert(exchanged);
  }

private:
  std::atomic<Record *> head;
  std::atomic<int> listLen;
};

} // namespace lib