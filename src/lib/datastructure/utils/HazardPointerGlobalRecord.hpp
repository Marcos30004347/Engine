
#include "lib/algorithm/search.hpp"
#include "lib/algorithm/sort.hpp"
#include "lib/datastructure/Vector.hpp"
#include "lib/memory/allocator/SystemAllocator.hpp"

#include <atomic>

namespace lib
{

thread_local static lib::Vector<void *> rlist;
const static int R = 16;

template <typename T> class HazardPointerRecord
{
 
  HazardPointerRecord<T> *next;

  std::atomic<int> isActive;

  static std::atomic<HazardPointerRecord<T> *> head;
  static std::atomic<int> listLen;

  void *pointer;

  template <typename Allocator> static void scan(HazardPointerRecord<T> *h, Allocator &allocator)
  {
    lib::Vector<void *> hp;

    while (h)
    {
      if (h->pointer != nullptr)
      {
        hp.pushBack(h->pointer);
      }

      h = h->next;
    }

    lib::algorithm::sort::quickSort(hp.buffer(), hp.size());

    for (size_t i = 0; i < rlist.size();)
    {
      size_t index = lib::algorithm::search::binarySearch(hp.buffer(), rlist[i], hp.size());

      if (index == hp.size()) // not found
      {
        allocator.deallocate((T *)rlist[i]);

        if (index != rlist.size() - 1)
        {
          rlist[i] = rlist[rlist.size() - 1];
          rlist[rlist.size() - 1] = nullptr;
        }

        rlist.popBack();
      }
      else
      {
        i++;
      }
    }
  }

public:
  static HazardPointerRecord<T> *acquire()
  {
    HazardPointerRecord<T> *p = head;

    for (; p; p = p->next)
    {
      int expected = 0;

      if (p->isActive || !p->isActive.compare_exchange_weak(expected, 1, std::memory_order_release, std::memory_order_relaxed))
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

    p = new HazardPointerRecord<T>();

    p->isActive = 1;
    p->pointer = NULL;

    HazardPointerRecord<T> *old = head;

    do
    {
      old = head;
      p->next = old;
    } while (!head.compare_exchange_weak(old, p, std::memory_order_release, std::memory_order_relaxed));

    return p;
  }

  template <typename Allocator> static void release(HazardPointerRecord<T> *rec, Allocator &allocator)
  {
    rec->pointer = NULL;

    if (rlist.size() >= R)
    {
      scan(head, allocator);
    }

    rec->isActive = false;
  }

  inline void assign(std::atomic<T *> &ref)
  {
    T *ptr = ref;
    do
    {
      ptr = ref;
      pointer = ptr;
    } while (ref != ptr);
  }

  inline void assign(T *&ref)
  {
    T *ptr = ref;
    do
    {
      ptr = ref;
      pointer = ptr;
    } while (ref != ptr);
  }

  template <typename Allocator> static void retire(T *ptr, Allocator &allocator)
  {
    rlist.pushBack((void *)ptr);

    if (rlist.size() >= R)
    {
      scan(head, allocator);
    }
  }
};

template <typename T> std::atomic<HazardPointerRecord<T> *> HazardPointerRecord<T>::head = nullptr;
template <typename T> std::atomic<int> HazardPointerRecord<T>::listLen = 0;

} // namespace lib