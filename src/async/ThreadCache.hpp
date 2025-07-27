#pragma once
#include "lib/algorithm/random.hpp"
#include "lib/memory/allocator/SystemAllocator.hpp"

#include "os/Thread.hpp"
#include "os/print.hpp"

#include <assert.h>
#include <atomic> // For spinlock
#include <thread> // For std::this_thread::yield()

namespace async
{
template <typename T> class ThreadCache
{
public:
  ThreadCache(size_t reserve)
  {
    capacity = nextPowerOfTwo(reserve);
    capacityMinusOne = capacity - 1;

    data = static_cast<KeyVal *>(lib::memory::SystemMemoryManager::malloc(sizeof(KeyVal) * capacity));

    for (size_t i = 0; i < capacity; i++)
    {
      data[i].key.store(UINT32_MAX);
    }

    lock.clear(); // Initialize spinlock
  }

  ~ThreadCache()
  {
    if (data == nullptr)
    {
      return;
    }

    for (size_t i = 0; i < capacity; i++)
    {
      if (data[i].key.load() != UINT32_MAX)
      {
        data[i].get()->~T();
      }
    }

    lib::memory::SystemMemoryManager::free(data);
  }

  ThreadCache(const ThreadCache &) = delete;
  ThreadCache &operator=(const ThreadCache &) = delete;

  ThreadCache(ThreadCache &&other) noexcept : data(other.data), capacity(other.capacity), capacityMinusOne(other.capacityMinusOne)
  {
    other.data = nullptr;
    lock.clear(); // Reinitialize spinlock for the moved object
  }

  ThreadCache &operator=(ThreadCache &&other) noexcept
  {
    if (this != &other)
    {
      if (data)
      {
        lib::memory::SystemMemoryManager::free(data);
      }

      data = other.data;
      capacity = other.capacity;
      capacityMinusOne = other.capacityMinusOne;

      other.data = nullptr;
      lock.clear(); // Reinitialize spinlock
    }
    return *this;
  }

  template <typename... Args> bool set(uint32_t key, Args &&...args)
  {
    assert(key != UINT32_MAX);

    uint32_t index = hashInteger(key) & capacityMinusOne;

    // os::print("Thread %u inserting at %u, capacity %u\n", key, index, capacity);

    for (size_t i = index; i < capacity; i++)
    {
      uint32_t expected = UINT32_MAX;

      if (data[i].key.compare_exchange_strong(expected, key, std::memory_order_release, std::memory_order_relaxed))
      {
        // os::print("Thread %u inserted at %u\n", key, index, data[i].key.load());

        new (data[i].get()) T(std::forward<Args>(args)...);
        // unlockSpin(); // release lock
        return true;
      }

      // os::print("Thread %u inserting at %u, curr %u, what %u\n", key, index, data[i].key.load(), expected);
    }

    for (size_t i = 0; i < index; i++)
    {
      uint32_t expected = UINT32_MAX;

      if (data[i].key.compare_exchange_strong(expected, key, std::memory_order_release, std::memory_order_relaxed))
      {
        // os::print("Thread %u inserted at %u\n", key, index, data[i].key.load());

        new (data[i].get()) T(std::forward<Args>(args)...);
        // unlockSpin(); // release lock
        return true;
      }

      // os::print("Thread %u inserting at %u, curr %u, what %u\n", key, index, data[i].key.load(), expected);
    }

    // unlockSpin(); // release lock
    return false;
  }

  template <typename... Args> bool update(uint32_t key, Args &&...args)
  {
    assert(key != UINT32_MAX);

    // lockSpin(); // acquire lock

    uint32_t index = hashInteger(key) & capacityMinusOne;

    for (size_t i = index; i < capacity; i++)
    {
      if (data[i].key.load(std::memory_order_relaxed) == key)
      {
        new (data[i].get()) T(std::forward<Args>(args)...);
        return true;
      }
    }

    for (size_t i = 0; i < index; i++)
    {
      if (data[i].key.load(std::memory_order_relaxed) == key)
      {
        new (data[i].get()) T(std::forward<Args>(args)...);
        return true;
      }
    }

    // unlockSpin(); // release lock
    return false;
  }

  T *get(uint32_t key)
  {
    // os::print("getting %u\n", key);

    assert(key != UINT32_MAX);

    uint32_t index = hashInteger(key) & capacityMinusOne;

    for (size_t i = 0; i < capacity; i++)
    {
      if (data[index].key.load(std::memory_order_acquire) == key)
      {

        return data[index].get();

        // return &data[index].val;
      }

      index = (index + 1) & capacityMinusOne;
    }

    return nullptr;
  }

private:
  static size_t nextPowerOfTwo(uint32_t n)
  {
    if (n == 0)
      return 1;

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
  }

  inline size_t hashInteger(size_t h)
  {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
  }

  //   void lockSpin()
  //   {
  //     while (lock.test_and_set(std::memory_order_acquire))
  //     {
  //       //   std::this_thread::yield(); // Yield instead of spinning hard
  //     }
  //   }

  //   void unlockSpin()
  //   {
  //     lock.clear(std::memory_order_release);
  //   }

  struct KeyVal
  {
    std::atomic<uint32_t> key = UINT32_MAX;
    alignas(T) unsigned char storage[sizeof(T)];

    T *get()
    {
      return reinterpret_cast<T *>(&storage);
    }
  };

  size_t capacity;
  size_t capacityMinusOne;
  KeyVal *data;

  std::atomic_flag lock = ATOMIC_FLAG_INIT;
};
} // namespace async
