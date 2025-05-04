#include <iostream>
#include <stdexcept>
namespace lib
{
template <typename T> class Vector
{
public:
  Vector() : _size(0), _capacity(0), _data(nullptr)
  {
  }

  ~Vector()
  {
    clear();
    operator delete(_data);
  }

  void push_back(const T &value)
  {
    ensure_capacity();
    new (_data + _size) T(value);
    ++_size;
  }

  void push_back(T &&value)
  {
    ensure_capacity();
    new (_data + _size) T(std::move(value));
    ++_size;
  }

  template<typename... Args>
  void emplace_back(Args&&... args) {
      ensure_capacity();
      new (_data + _size) T(std::forward<Args>(args)...);
      ++_size;
  }

  T &operator[](size_t i)
  {
    if (i >= _size)
      throw std::out_of_range("Index out of bounds");
    return _data[i];
  }

  const T &operator[](size_t i) const
  {
    if (i >= _size)
      throw std::out_of_range("Index out of bounds");
    return _data[i];
  }

  size_t size() const
  {
    return _size;
  }

  void clear()
  {
    for (size_t i = 0; i < _size; ++i)
      _data[i].~T();
    _size = 0;
  }

private:
  void ensure_capacity()
  {
    if (_size == _capacity)
    {
      size_t new_capacity = _capacity == 0 ? 1 : _capacity * 2;
      T *new_data = static_cast<T *>(operator new(sizeof(T) * new_capacity));
      for (size_t i = 0; i < _size; ++i)
        new (new_data + i) T(std::move(_data[i]));
      clear();
      operator delete(_data);
      _data = new_data;
      _capacity = new_capacity;
    }
  }

  size_t _size;
  size_t _capacity;
  T *_data;
};
} // namespace lib