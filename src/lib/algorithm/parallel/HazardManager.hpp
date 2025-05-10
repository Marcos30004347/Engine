#pragma once
#include "lib/Vector.hpp"
#include "lib/allocator/SystemAllocator.hpp"

#include "lib/algorithm/search.hpp"
#include "lib/algorithm/sort.hpp"

#include <atomic>

namespace lib
{
namespace algorithm
{
namespace parallel
{
template <typename T, size_t K> class HazardManager
{
  template <typename H> using HPRecAllocator = lib::allocator::SystemAllocator<H>;
  template <typename H> using RListAllocator = lib::allocator::SystemAllocator<H>;

private:
  struct HPRec
  {
    T *HP[K];
    HPRec *next;
    std::atomic<bool> active;
    lib::Vector<T *, RListAllocator<T *>> rlist;
    size_t rcount;
  };

  thread_local HPRec *myHPRec = nullptr;
  thread_local lib::Vector<T *, RListAllocator<T *>> plist;

  std::atomic<size_t> H;
  std::atomic<HPRec *> headHPRec;

  HPRecAllocator<T *> allocator;

  size_t R()
  {
    size_t h = H.load();
    return h + (h > 10 ? 10 : h);
  }

  void helpScan()
  {
    for (HPRec *hprec = headHPRec; hprec != nullptr; hprec = hprec->next)
    {
      if (hprec->active.load())
      {
        continue;
      }
      if (!hprec->active.compare_and_swap_strong(false, true))
      {
        continue;
      }

      while (hprec->rcount > 0)
      {
        T *node = hprec->rlist.pop();
        hprec->rcount -= 1;

        myHPRec->rlist.push(node);
        myHPRec->rcount += 1;

        HPRec *head = headHPRec;

        if (myHPRec->rcount >= R())
        {
          scan(head);
        }
      }
    }
  }

  void scan(HPRec *head)
  {
    plist.reserve(H.load() * K);

    HPRec *hprec = head;

    while (hprec != nullptr)
    {
      for (uint32_t i = 0; i < K; i++)
      {
        T *hptr = hprec->HP[i];
        if (hptr != nullptr)
        {
          plist.pushBack(hptr);
        }
      }

      hprec = hprec->next;
    }

    lib::algorithm::sort::quickSort(plist, 0, plist.size());

    for (int32_t i = 0; i < myHPRec->rlist.size(); i++)
    {
      if (lib::algorithm::search::binarySearch(plist, myHPRec->rlist[i]) == -1)
      {
        prepareForReuse(myHPRec->rlist[i]);

        myHPRec->rlist[i] = myHPRec->rlist[myHPRec->rlist[i].size() - 1];
        myHPRec->rlist[i].pop_back();
        i -= 1;
      }
    }

    plist.clear();
  }

public:
  HazardManager()
  {
    allocator = HPRecAllocator<T *>();
    headHPRec = nullptr;
    H = 0;
  }

  HazardManager(HPRecAllocator<T *> &alloc)
  {
    allocator = alloc;
    headHPRec = nullptr;
    H = 0;
  }

  ~HazardManager()
  {
    HPRec *curr = headHPRec.load();

    while (curr)
    {
      HPRec *tmp = curr;
      curr = curr->next;
      delete tmp;
    }
  }

  void retireHPRecForThisThread()
  {
    for (size_t i = 0; i < K; i++)
    {
      myHPRec->HP[i] = nullptr;
    }

    myHPRec->active = false;
  }

  void allocateHPRecForThisThread()
  {
    myHPRec = nullptr;

    for (HPRec *rec = headHPRec; rec != nullptr; rec = rec->next)
    {
      if (rec->active)
      {
        continue;
      }

      if (!rec->active.compare_and_swap_strong(false, true))
      {
        continue;
      }

      myHPRec = rec;
      return;
    }

    size_t oldCount = 0;

    do
    {
      oldCount = H.load();
    } while (!H.compare_exchange_strong(oldCount, oldCount + K));

    // TODO: use special allocator
    myHPRec = allocator.allocate(1);
    myHPRec->active = true;
    myHPRec->rcount = 0;

    myHPRec->rlist = lib::Vector<T *, RListAllocator<T *>>();

    for (size_t i = 0; i < K; i++)
    {
      myHPRec->HP[i] = nullptr;
    }

    HPRec *oldHead = nullptr;

    do
    {
      oldHead = headHPRec.load();
      myHPRec->next = oldHead;
    } while (!headHPRec.compare_exchange_strong(oldHead, myHPRec));
  }

  void retirePtr(T *ptr)
  {
    myHPRec->rlist.push_back(ptr);
    myHPRec->rcount += 1;

    HPRec *head = headHPRec;

    if (myHPRec->rcount >= R())
    {
      scan(head);
      helpScan();
    }
  }

  bool tryAchirePtrAndCompare(T *&v, std::atomic<T *> x, uint32_t index)
  {
    do
    {
      v = x.load();
      myHPRec->HP[index] = x.load();
    } while (x.load() != v);
  }
};
} // namespace parallel
} // namespace algorithm
} // namespace lib