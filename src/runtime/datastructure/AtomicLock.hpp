#pragma once
#include <atomic>

namespace lib
{

class AtomicLock
{
  std::atomic<bool> flag; // false = unlocked, true = locked

public:
  AtomicLock() : flag(false)
  {
  }
  AtomicLock(const AtomicLock &) = delete;
  AtomicLock &operator=(const AtomicLock &) = delete;

  inline void lock() noexcept
  {
    bool expected = false;
    while (!flag.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
    {
      expected = false; 

#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#endif
    }
  }

  inline bool tryLock() noexcept
  {
    bool expected = false;
    return flag.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  inline void unlock() noexcept
  {
    flag.store(false, std::memory_order_release);
  }

  inline bool isLocked() const noexcept
  {
    return flag.load(std::memory_order_relaxed);
  }
};

} // namespace lib
