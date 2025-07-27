#pragma once

#include <algorithm>
#include <cstddef>
#include <iostream>

namespace lib
{
namespace algorithm
{
namespace sort
{

// Helper function to partition the array
template <typename T> int partition(T *arr, int low, int high)
{
  // Choose the last element as the pivot
  T pivot = arr[high];
  // Index of smaller element
  int i = low - 1;

  for (int j = low; j < high; ++j)
  {
    // If current element is smaller than or equal to pivot
    if (arr[j] <= pivot)
    {
      ++i;                       // Increment index of smaller element
      std::swap(arr[i], arr[j]); // Swap current element with the element at i
    }
  }

  // Swap the pivot element with the element at i + 1
  std::swap(arr[i + 1], arr[high]);
  return i + 1; // Return the partitioning index
}

// Main QuickSort function
template <typename T> void quickSort(T *arr, int low, int high)
{
  // Base case: if low is less than high, there's at least two elements to sort
  if (low < high)
  {
    // pi is partitioning index, arr[pi] is now at right place
    int pi = partition(arr, low, high);

    // Recursively sort elements before partition and after partition
    quickSort(arr, low, pi - 1);
    quickSort(arr, pi + 1, high);
  }
}

// Overload for convenience: allows calling with a pointer and size
template <typename T> void quickSort(T *arr, size_t size)
{
  if (size == 0)
  {
    return; // Nothing to sort if the array is empty
  }
  quickSort(arr, 0, static_cast<int>(size - 1));
}

} // namespace sort
} // namespace algorithm
} // namespace lib