#pragma once

#include <iostream>
#include <stdexcept>
#include <utility>

#include "lib/memory/allocator/SystemAllocator.hpp"

namespace lib
{
template <typename T, typename Allocator = lib::memory::allocator::SystemAllocator<T>> class Vector
{
public:
  explicit Vector(const Allocator &alloc = Allocator()) : data(nullptr), count(0), totalCapacity(0), isReserved(false), reservedLimit(0), memoryAllocator(alloc)
  {
  }

  ~Vector()
  {
    clear();
    if (data)
    {
      memoryAllocator.deallocate(data, totalCapacity);
    }
  }

  Vector(const Vector &other) : data(nullptr), count(0), totalCapacity(0), isReserved(false), reservedLimit(0), memoryAllocator(other.memoryAllocator)
  {
    reserve(other.count);
    for (size_t i = 0; i < other.count; ++i)
    {
      new (data + i) T(other.data[i]);
    }
    count = other.count;
    isReserved = other.isReserved;
    reservedLimit = other.reservedLimit;
  }

  Vector &operator=(const Vector &other)
  {
    if (this != &other)
    {
      clear();
      if (data)
      {
        memoryAllocator.deallocate(data, totalCapacity);
        data = nullptr;
        totalCapacity = 0;
      }

      memoryAllocator = other.memoryAllocator;

      // Reserve and copy elements
      reserve(other.count);
      for (size_t i = 0; i < other.count; ++i)
      {
        new (data + i) T(other.data[i]);
      }
      count = other.count;
      isReserved = other.isReserved;
      reservedLimit = other.reservedLimit;
    }
    return *this;
  }

  Vector(Vector &&other) noexcept
      : data(other.data), count(other.count), totalCapacity(other.totalCapacity), isReserved(other.isReserved), reservedLimit(other.reservedLimit),
        memoryAllocator(std::move(other.memoryAllocator))
  {
    other.data = nullptr;
    other.count = 0;
    other.totalCapacity = 0;
    other.isReserved = false;
    other.reservedLimit = 0;
  }

  Vector &operator=(Vector &&other) noexcept
  {
    if (this != &other)
    {
      clear();
      if (data)
      {
        memoryAllocator.deallocate(data, totalCapacity);
      }

      data = other.data;
      count = other.count;
      totalCapacity = other.totalCapacity;
      isReserved = other.isReserved;
      reservedLimit = other.reservedLimit;
      memoryAllocator = std::move(other.memoryAllocator);

      other.data = nullptr;
      other.count = 0;
      other.totalCapacity = 0;
      other.isReserved = false;
      other.reservedLimit = 0;
    }
    return *this;
  }

  T *buffer()
  {
    return data;
  }

  T &front()
  {
    if (count == 0)
    {
      throw std::out_of_range("Vector is empty, cannot access front element.");
    }
    return data[0];
  }

  const T &front() const
  {
    if (count == 0)
    {
      throw std::out_of_range("Vector is empty, cannot access front element.");
    }
    return data[0];
  }

  T &back()
  {
    if (count == 0)
    {
      throw std::out_of_range("Vector is empty, cannot access back element.");
    }
    return data[count - 1];
  }

  const T &back() const
  {
    if (count == 0)
    {
      throw std::out_of_range("Vector is empty, cannot access back element.");
    }
    return data[count - 1];
  }

  template <typename... Args> void emplaceBack(Args &&...args)
  {
    if (count >= totalCapacity)
    {
      if (isReserved && count >= reservedLimit)
      {
        isReserved = false;
      }

      if (!isReserved)
      {
        size_t newCapacity = (totalCapacity == 0) ? 1 : totalCapacity * 2;
        if (newCapacity <= count)
        {
          newCapacity = count + 1;
        }
        resize(newCapacity);
      }
      else
      {
        throw std::runtime_error("Exceeded reserved capacity");
      }
    }

    new (data + count) T(std::forward<Args>(args)...);
    ++count;
  }

  void pushBack(const T &value)
  {
    emplaceBack(value);
  }

  void popBack()
  {
    if (count == 0)
    {
      throw std::runtime_error("Pop from empty vector");
    }

    data[count - 1].~T();
    --count;

    if (!isReserved && totalCapacity > 1 && count < totalCapacity / 4)
    {
      size_t newCapacity = totalCapacity / 2;
      if (newCapacity < count)
      {
        newCapacity = count;
      }
      if (newCapacity < 1 && count == 0)
      {
        newCapacity = 0;
      }
      else if (newCapacity < 1)
      {
        newCapacity = 1;
      }
      resize(newCapacity);
    }
  }

  void reserve(size_t newCapacity)
  {
    if (newCapacity > totalCapacity)
    {
      resize(newCapacity);
    }
    isReserved = true;
    reservedLimit = newCapacity;
  }

  size_t size() const
  {
    return count;
  }

  size_t capacity() const
  {
    return totalCapacity;
  }

  T &operator[](size_t index)
  {
    if (index >= count)
    {
      throw std::out_of_range("Index out of range");
    }
    return data[index];
  }

  const T &operator[](size_t index) const
  {
    if (index >= count)
    {
      throw std::out_of_range("Index out of range");
    }
    return data[index];
  }

  void clear()
  {
    for (size_t i = 0; i < count; ++i)
    {
      data[i].~T();
    }
    count = 0;
  }

private:
  T *data;
  size_t count;
  size_t totalCapacity;
  bool isReserved;
  size_t reservedLimit;
  Allocator memoryAllocator;

  void resize(size_t newCapacity)
  {
    if (newCapacity > 0 && static_cast<size_t>(-1) / sizeof(T) < newCapacity)
    {
      throw std::bad_alloc();
    }

    T *newData = nullptr;
    if (newCapacity > 0)
    {
      newData = memoryAllocator.allocate(newCapacity);
    }

    size_t elementsToMove = std::min(count, newCapacity);
    for (size_t i = 0; i < elementsToMove; ++i)
    {
      new (newData + i) T(std::move(data[i]));
      data[i].~T();
    }

    if (data)
    {
      memoryAllocator.deallocate(data, totalCapacity);
    }

    data = newData;
    totalCapacity = newCapacity;
    count = elementsToMove;
  }
};
} // namespace lib