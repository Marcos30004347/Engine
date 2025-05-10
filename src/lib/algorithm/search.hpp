#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace lib
{
namespace algorithm
{
namespace search
{
template <typename T, typename Allocator> int binarySearch(const Vector<T, Allocator> &vec, const T &value)
{
  auto begin = vec.begin();
  auto end = vec.end();

  while (begin < end)
  {
    auto mid = begin + (end - begin) / 2;
    if (*mid == value)
    {
      return mid - vec.begin();
    }
    else if (*mid < value)
    {
      begin = mid + 1;
    }
    else
    {
      end = mid;
    }
  }
  return -1;
}
} // namespace search
} // namespace algorithm
} // namespace lib
