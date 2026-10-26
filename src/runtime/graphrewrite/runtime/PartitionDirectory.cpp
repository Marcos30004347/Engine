#include "graphrewrite/runtime/PartitionDirectory.hpp"

namespace graphrewrite::runtime {

PageRun directoryRange(const PartitionDirectory &directory, const uint32_t key)
{
  PageRun run;

  const int64_t count = static_cast<int64_t>(directory.size());

  if (count == 0)
  {
    return run; // first > last, i.e. empty
  }

  // First page whose maxKey >= key.
  int64_t first = count;
  int64_t low = 0;
  int64_t high = count - 1;

  while (low <= high)
  {
    const int64_t mid = low + (high - low) / 2;

    if (directory.maxKey[static_cast<size_t>(mid)] >= key)
    {
      first = mid;
      high = mid - 1;
    }
    else
    {
      low = mid + 1;
    }
  }

  // Last page whose minKey <= key.
  int64_t last = -1;

  low = 0;
  high = count - 1;

  while (low <= high)
  {
    const int64_t mid = low + (high - low) / 2;

    if (directory.minKey[static_cast<size_t>(mid)] <= key)
    {
      last = mid;
      low = mid + 1;
    }
    else
    {
      high = mid - 1;
    }
  }

  if (first > last)
  {
    return run; // no page can hold this key
  }

  run.first = static_cast<uint32_t>(first);
  run.last = static_cast<uint32_t>(last);

  return run;
}

} // namespace graphrewrite::runtime
