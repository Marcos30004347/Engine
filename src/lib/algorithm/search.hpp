#pragma once

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "lib/datastructure/Vector.hpp"

namespace lib
{
namespace algorithm
{
namespace search
{

template <typename T> size_t binarySearch(const T *array, const T &value, size_t len)
{
  long long int begin = 0;
  long long int end = len - 1;

  while (begin <= end)
  {
    auto mid = begin + (end - begin) / 2;

    if (array[mid] == value)
    {
      return mid;
    }
    else if (array[mid] < value)
    {
      begin = mid + 1;
    }
    else
    {
      end = mid - 1;
    }
  }

  return len;
}

template <typename T> size_t linearSearch(const T *array, const T &value, size_t len)
{
  for (size_t i = 0; i < len; i++)
  {
    if (array[i] == value)
    {
      return i;
    }
  }

  return len;
}
} // namespace search
} // namespace algorithm
} // namespace lib
