#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>
namespace lib
{
template <typename T> struct MarkedAtomicPointer
{
  static constexpr uintptr_t MARK_BITS = 3;
  static constexpr uintptr_t MARK_MASK = (uintptr_t{1} << MARK_BITS) - 1;
  static constexpr uintptr_t PTR_MASK = ~MARK_MASK;

private:
  std::atomic<uintptr_t> internal{0};

  static uintptr_t compose(T *ptr, uintptr_t mark) noexcept
  {
    return (reinterpret_cast<uintptr_t>(ptr) & PTR_MASK) | (mark & MARK_MASK);
  }

public:
  MarkedAtomicPointer() = default;

  MarkedAtomicPointer(T *ptr, uintptr_t mark = 0) noexcept : internal(compose(ptr, mark))
  {
    static_assert(alignof(T) >= (1 << MARK_BITS), "T must be aligned to at least 8 bytes to use 3 mark bits.");
  }

  T *getReference(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    return reinterpret_cast<T *>(internal.load(order) & PTR_MASK);
  }

  uintptr_t getMark(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    return internal.load(order) & MARK_MASK;
  }

  void getMark(int &mark_out, std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    mark_out = static_cast<int>(internal.load(order) & MARK_MASK);
  }

  int getMarkInt(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    return static_cast<int>(internal.load(order) & MARK_MASK);
  }

  T *read(uintptr_t &mark, std::memory_order order = std::memory_order_seq_cst)
  {
    uintptr_t v = internal.load(order);
    mark = v & MARK_MASK;
    return reinterpret_cast<T *>(v & PTR_MASK);
  }

  T *read(bool &mark, std::memory_order order = std::memory_order_seq_cst)
  {
    uintptr_t v = internal.load(order);
    mark = (v & MARK_MASK) != 0;
    return reinterpret_cast<T *>(v & PTR_MASK);
  }

  T *load(std::memory_order order = std::memory_order_seq_cst)
  {
    uintptr_t v = internal.load(order);
    return reinterpret_cast<T *>(v & PTR_MASK);
  }

  std::pair<T *, uintptr_t> get(std::memory_order order = std::memory_order_seq_cst) const noexcept
  {
    uintptr_t v = internal.load(order);
    return {reinterpret_cast<T *>(v & PTR_MASK), v & MARK_MASK};
  }

  bool compare_exchange(T *&expected_ptr, uintptr_t &expected_mark, T *desired_ptr, int desired_mark, std::memory_order success, std::memory_order failure) noexcept
  {
    uintptr_t expected = compose(expected_ptr, static_cast<uintptr_t>(expected_mark));
    uintptr_t desired = compose(desired_ptr, static_cast<uintptr_t>(desired_mark));

    bool ok = internal.compare_exchange_strong(expected, desired, success, failure);

    if (!ok)
    {
      expected_ptr = reinterpret_cast<T *>(expected & PTR_MASK);
      expected_mark = static_cast<uintptr_t>(expected & MARK_MASK);
    }

    return ok;
  }

  bool compare_exchange_strong(T *&expected, T *desired, std::memory_order success, std::memory_order failure) noexcept
  {
    return internal.compare_exchange_strong((uintptr_t &)expected, (uintptr_t &)desired, success, failure);
  }

  void store(T *ptr, uintptr_t mark, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    internal.store(compose(ptr, mark), order);
  }

  void store(T *ptr, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    internal.store((uintptr_t)ptr, order);
  }

  inline bool attemptMark(T *expected_ptr, uintptr_t new_mark, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    uintptr_t current = compose(expected_ptr, false);
    uintptr_t desired = compose(expected_ptr, new_mark);
    return internal.compare_exchange_strong(current, desired, order, order);
  }

  inline T *mark(uintptr_t new_mark, std::memory_order order = std::memory_order_seq_cst) noexcept
  {
    return reinterpret_cast<T *>(internal.fetch_or(new_mark, order));
  }
};
} // namespace lib