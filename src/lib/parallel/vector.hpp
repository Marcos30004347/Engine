#pragma once
#include <condition_variable>
#include <mutex>
#include <vector>

namespace lib
{
namespace parallel
{

template <typename T> class Vector
{
private:
  std::vector<T> vector;
  mutable std::mutex mtx;
  std::condition_variable cv;

public:
  Vector() = default;

  void push_back(const T &value)
  {
    std::lock_guard<std::mutex> lock(mtx);
    vector.push_back(value);
    cv.notify_one();
  }

  template <typename... Args> void emplace_back(Args &&...args)
  {
    std::lock_guard<std::mutex> lock(mtx);
    vector.emplace_back(std::forward<Args>(args)...);
    cv.notify_one();
  }

  bool pop_back(T &value)
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (vector.empty())
      return false;
    value = vector.back();
    vector.pop_back();
    return true;
  }

  bool empty() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.empty();
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.size();
  }

  std::vector<T> snapshot() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector;
  }

  auto begin() const
  {
    return snapshot().begin();
  }
  auto end() const
  {
    return snapshot().end();
  }
  void resize(size_t new_size)
  {
    std::lock_guard<std::mutex> lock(mtx);
    vector.resize(new_size);
  }

  void resize(size_t new_size, const T &value)
  {
    std::lock_guard<std::mutex> lock(mtx);
    vector.resize(new_size, value);
  }

  T &front()
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.front();
  }

  const T &front() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.front();
  }

  T &back()
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.back();
  }

  const T &back() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector.back();
  }

  T &operator[](size_t index)
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector[index];
  }

  const T &operator[](size_t index) const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return vector[index];
  }
};

} // namespace parallel
} // namespace lib
