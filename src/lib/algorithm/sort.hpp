#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace lib
{
namespace algorithm
{
namespace sort
{
template <typename T, typename Allocator> int partition(Vector<T, Allocator> &vec, int low, int high)
{
  T pivot = vec[high];
  int i = low - 1;

  for (int j = low; j < high; ++j)
  {
    if (vec[j] <= pivot)
    {
      ++i;
      std::swap(vec[i], vec[j]);
    }
  }

  std::swap(vec[i + 1], vec[high]);
  return i + 1;
}

template <typename T, typename Allocator> void quickSort(Vector<T, Allocator> &vec, int low, int high)
{
  if (low < high)
  {
    int pi = partition(vec, low, high);

    quickSort(vec, low, pi - 1);
    quickSort(vec, pi + 1, high);
  }
}

} // namespace sort
} // namespace algorithm
} // namespace lib
