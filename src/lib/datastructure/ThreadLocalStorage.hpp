#pragma once

#include "lib/datastructure/ConcurrentLookupTable.hpp"
#include "os/Thread.hpp"

namespace lib
{
template <typename T, size_t reservedThreads = 32> class ThreadLocalStorage
{
  ConcurrentLookupTable<T> lookupTable;

public:
  ThreadLocalStorage() : lookupTable(reservedThreads)
  {
  }

  void set(T val)
  {
    T old;

    if (lookupTable.get(os::Thread::getCurrentThreadId(), old))
    {
      return assert(lookupTable.update(os::Thread::getCurrentThreadId(), val));
    }

    assert(lookupTable.insert(os::Thread::getCurrentThreadId(), val));
  }

  void set(T val, T &old)
  {
    if (lookupTable.get(os::Thread::getCurrentThreadId(), old))
    {
      return assert(lookupTable.update(os::Thread::getCurrentThreadId(), val));
    }

    assert(lookupTable.insert(os::Thread::getCurrentThreadId(), val));
  }

  bool get(T &val)
  {
    return lookupTable.get(os::Thread::getCurrentThreadId(), val);
  }
};
} // namespace lib