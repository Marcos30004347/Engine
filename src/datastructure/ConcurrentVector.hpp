#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

#include "memory/SystemMemoryManager.hpp"
#include "memory/allocator/SystemAllocator.hpp"

#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>


namespace lib
{

template <typename T, typename Allocator = lib::memory::allocator::SystemAllocator<T>> class ConcurrentVector
{
private:
  std::atomic<T *> dataArray;
  std::atomic<size_t> currentSize;
  std::atomic<size_t> currentCapacity;
  std::atomic_flag resizeLock = ATOMIC_FLAG_INIT;

  static constexpr size_t kInitialCapacity = 8;

  Allocator allocator;

  template <typename... Args> void constructElement(T *ptr, Args &&...args)
  {
    new (ptr) T(std::forward<Args>(args)...);
  }

  void destroyElement(T *ptr)
  {
    ptr->~T();
  }

  void destroyElements(T *ptr, size_t count)
  {
    for (size_t i = 0; i < count; ++i)
    {
      destroyElement(ptr + i);
    }
  }

  void deallocateMemory(T *ptr, size_t capacity)
  {
    if (ptr)
    {
      allocator.deallocate(ptr, capacity);
    }
  }

  void resizeInternal(size_t oldSize, std::size_t oldCapacity)
  {
    while (resizeLock.test_and_set(std::memory_order_acquire))
    {
    }

    size_t actualCurrentCapacity = currentCapacity.load(std::memory_order_relaxed);
    size_t actualCurrentSize = currentSize.load(std::memory_order_relaxed);

    if (actualCurrentCapacity >= actualCurrentSize + 1)
    {
      resizeLock.clear(std::memory_order_release);
      return;
    }

    size_t newCapacity = (actualCurrentCapacity == 0) ? kInitialCapacity : actualCurrentCapacity * 2;

    T *oldData = dataArray.load(std::memory_order_relaxed);
    T *newData = nullptr;

    try
    {
      newData = allocator.allocate(newCapacity);

      if (actualCurrentSize > 0 && oldData)
      {
        std::uninitialized_copy(oldData, oldData + actualCurrentSize, newData);
      }
    }
    catch (...)
    {
      if (newData)
      {
        allocator.deallocate(newData, newCapacity);
      }
      resizeLock.clear(std::memory_order_release);
      throw;
    }

    dataArray.store(newData, std::memory_order_release);
    currentCapacity.store(newCapacity, std::memory_order_release);

    if (oldData)
    {
      destroyElements(oldData, actualCurrentSize);
      deallocateMemory(oldData, actualCurrentCapacity);
    }

    resizeLock.clear(std::memory_order_release);
  }

public:
  explicit ConcurrentVector(const Allocator &alloc = Allocator()) : dataArray(nullptr), currentSize(0), currentCapacity(0), allocator(alloc)
  {
  }

  ~ConcurrentVector()
  {
    T *currentData = dataArray.load(std::memory_order_acquire);
    size_t currentVecSize = currentSize.load(std::memory_order_acquire);
    size_t currentVecCapacity = currentCapacity.load(std::memory_order_acquire);

    destroyElements(currentData, currentVecSize);
    deallocateMemory(currentData, currentVecCapacity);
  }

  ConcurrentVector(const ConcurrentVector &) = delete;
  ConcurrentVector &operator=(const ConcurrentVector &) = delete;

  void pushBack(const T &value)
  {
    size_t currentVecSize;
    size_t currentVecCapacity;
    T *currentVecData;

    while (true)
    {
      currentVecSize = currentSize.load(std::memory_order_acquire);
      currentVecCapacity = currentCapacity.load(std::memory_order_acquire);
      currentVecData = dataArray.load(std::memory_order_acquire);

      if (currentVecSize >= currentVecCapacity)
      {
        resizeInternal(currentVecSize, currentVecCapacity);
        continue;
      }

      if (currentSize.compare_exchange_weak(currentVecSize, currentVecSize + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
      {
        currentVecData = dataArray.load(std::memory_order_acquire);
        if (currentVecData == nullptr)
        {
          throw std::runtime_error("Internal error: dataArray pointer is null.");
        }

        constructElement(currentVecData + currentVecSize, value);
        return;
      }
    }
  }

  T &at(size_t index)
  {
    size_t currentVecSize = currentSize.load(std::memory_order_acquire);
    T *currentVecData = dataArray.load(std::memory_order_acquire);

    if (index >= currentVecSize)
    {
      throw std::out_of_range("Index out of bounds in ConcurrentVector::at");
    }
    return currentVecData[index];
  }

  const T &at(size_t index) const
  {
    size_t currentVecSize = currentSize.load(std::memory_order_acquire);
    T *currentVecData = dataArray.load(std::memory_order_acquire);

    if (index >= currentVecSize)
    {
      throw std::out_of_range("Index out of bounds in ConcurrentVector::at (const)");
    }

    return currentVecData[index];
  }

  T &operator[](size_t index)
  {
    return at(index);
  }

  const T &operator[](size_t index) const
  {
    return at(index);
  }

  size_t size() const
  {
    return currentSize.load(std::memory_order_acquire);
  }

  size_t capacity() const
  {
    return currentCapacity.load(std::memory_order_acquire);
  }

  bool empty() const
  {
    return currentSize.load(std::memory_order_acquire) == 0;
  }
};

} // namespace lib