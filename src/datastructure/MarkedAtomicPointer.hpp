#pragma once
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <random>

#define ALIGNED_ATOMIC_PTR_ALIGNMENT 8

namespace lib
{
template <typename T> struct alignas(ALIGNED_ATOMIC_PTR_ALIGNMENT) MarkedAtomicPointer
{
private:
  std::atomic<T *> internal_ptr;
  static constexpr uintptr_t MARK_BIT = 0x1;
  static constexpr uintptr_t PTR_MASK = ~MARK_BIT;
  static inline uintptr_t compose(T *ptr, bool mark) noexcept
  {
    return reinterpret_cast<uintptr_t>(ptr) | (mark ? MARK_BIT : 0);
  }

public:
  MarkedAtomicPointer() : internal_ptr(0)
  {
  }
  MarkedAtomicPointer(T *p, bool mark = false) : internal_ptr(compose(p, mark))
  {
  }
  inline T *getReference(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    T *value = internal_ptr.load(order);
    return reinterpret_cast<T *>((uintptr_t)value & PTR_MASK);
  }
  inline bool isMarked(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    T *value = internal_ptr.load(order);
    return ((uintptr_t)value & MARK_BIT) != 0;
  }
  inline std::pair<T *, bool> get(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    T *value = internal_ptr.load(order);
    return {reinterpret_cast<T *>((uintptr_t)value & PTR_MASK), ((uintptr_t)value & MARK_BIT) != 0};
  }
  inline void store(T *desired, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    internal_ptr.store(desired, order);
  }
  inline T *load(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    return getReference(order);
  }
  inline bool compare_exchange_strong(T *&expected, T *desired, std::memory_order success, std::memory_order failure) noexcept
  {
    bool result = internal_ptr.compare_exchange_strong(expected, desired, success, failure);
    return result;
  }
  inline operator T *() const noexcept
  {
    return getReference();
  }

  inline void setMark(bool mark, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    T *current = internal_ptr.load(std::memory_order_relaxed);
    T *ptr = reinterpret_cast<T *>((uintptr_t)current & PTR_MASK);
    internal_ptr.store(reinterpret_cast<T *>(compose(ptr, mark)), order);
  }

  inline bool attemptMark(T *expected_ptr, bool new_mark, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    T *current = reinterpret_cast<T *>(compose(expected_ptr, false));
    T *desired = reinterpret_cast<T *>(compose(expected_ptr, new_mark));
    return internal_ptr.compare_exchange_strong(current, desired, order, order);
  }

  inline T *read(bool &is_marked, std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    T *value = internal_ptr.load(order);
    is_marked = ((uintptr_t)value & MARK_BIT) != 0;
    return reinterpret_cast<T *>((uintptr_t)value & PTR_MASK);
  }

  MarkedAtomicPointer(const MarkedAtomicPointer &) = delete;
  MarkedAtomicPointer &operator=(const MarkedAtomicPointer &) = delete;
};
} // namespace lib