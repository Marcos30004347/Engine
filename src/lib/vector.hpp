#pragma once

#include <iostream>
#include <stdexcept>

#include "lib/allocator/SystemAllocator.hpp"

namespace lib
{
template <typename T, typename Allocator = lib::allocator::SystemAllocator<T>> class Vector
{
public:
  explicit Vector(const Allocator &alloc = Allocator()) : _data(nullptr), _size(0), _capacity(0), _reserved(false), _reserved_capacity(0), _allocator(alloc)
  {
  }

  ~Vector()
  {
    clear();
    _allocator.deallocate(_data, _capacity);
  }
  template <typename... Args> void emplaceBack(Args &&...args)
  {
    if (_size >= _capacity)
    {
      if (_reserved && _size >= _reserved_capacity)
      {
        _reserved = false;
      }

      if (!_reserved)
      {
        size_t new_capacity = (_capacity == 0) ? 1 : _capacity * 2;
        resize(new_capacity);
      }
      else
      {
        throw std::runtime_error("Exceeded reserved capacity");
      }
    }

    new (_data + _size) T(std::forward<Args>(args)...);
    ++_size;
  }
  void pushBack(const T &value)
  {
    if (_size >= _capacity)
    {
      if (_reserved && _size >= _reserved_capacity)
      {
        _reserved = false;
      }

      if (!_reserved)
      {
        size_t new_capacity = (_capacity == 0) ? 1 : _capacity * 2;
        resize(new_capacity);
      }
      else
      {
        throw std::runtime_error("Exceeded reserved capacity");
      }
    }

    new (_data + _size) T(value);
    ++_size;
  }

  void popBack()
  {
    if (_size == 0)
    {
      throw std::runtime_error("Pop from empty vector");
    }

    _data[_size - 1].~T();
    --_size;

    if (!_reserved && _size < _capacity / 4 && _capacity > 1)
    {
      resize(_capacity / 2);
    }
  }

  void reserve(size_t new_capacity)
  {
    if (new_capacity > _capacity)
    {
      resize(new_capacity);
    }
    _reserved = true;
    _reserved_capacity = new_capacity;
  }

  size_t size() const
  {
    return _size;
  }
  size_t capacity() const
  {
    return _capacity;
  }

  T &operator[](size_t index)
  {
    if (index >= _size)
      throw std::out_of_range("Index out of range");
    return _data[index];
  }

  const T &operator[](size_t index) const
  {
    if (index >= _size)
      throw std::out_of_range("Index out of range");
    return _data[index];
  }

  void clear()
  {
    for (size_t i = 0; i < _size; ++i)
    {
      _data[i].~T();
    }
    _size = 0;
  }

private:
  T *_data;
  size_t _size;
  size_t _capacity;
  bool _reserved;
  size_t _reserved_capacity;
  Allocator _allocator;

  void resize(size_t new_capacity)
  {
    T *new_data = _allocator.allocate(new_capacity);

    for (size_t i = 0; i < _size; ++i)
    {
      new (new_data + i) T(std::move(_data[i]));
      _data[i].~T();
    }

    if (_data)
    {
      _allocator.deallocate(_data, _capacity);
    }

    _data = new_data;
    _capacity = new_capacity;
  }
};
} // namespace lib
