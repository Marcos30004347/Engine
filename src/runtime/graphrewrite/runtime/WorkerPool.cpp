#include "graphrewrite/runtime/WorkerPool.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

uint32_t WorkerPool::hardwareWorkers()
{
  const uint32_t reported = std::thread::hardware_concurrency();

  // Leave the calling thread a core: it takes a chunk too.
  return reported <= 1u ? 0u : reported - 1u;
}

WorkerPool::WorkerPool(const uint32_t workers)
{
  threads.reserve(workers);

  for (uint32_t i = 0u; i < workers; ++i)
  {
    threads.emplace_back([this] { workerLoop(); });
  }
}

WorkerPool::~WorkerPool()
{
  {
    std::lock_guard<std::mutex> lock(mutex);

    stopping = true;
    ++generation;
  }

  wake.notify_all();

  for (std::thread &thread : threads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }
}

uint32_t WorkerPool::chunkCountFor(const uint32_t count) const
{
  if (count == 0u)
  {
    return 0u;
  }

  if (threads.empty())
  {
    return 1u;
  }

  // A few chunks per worker so a slow chunk does not strand the rest, but not so
  // many that the bookkeeping outweighs the work.
  const uint32_t target = (static_cast<uint32_t>(threads.size()) + 1u) * 4u;

  return std::max(1u, std::min(target, count));
}

void WorkerPool::workerLoop()
{
  uint64_t seen = 0u;

  while (true)
  {
    const std::function<void(uint32_t, uint32_t, uint32_t)> *body = nullptr;
    uint32_t count = 0u;
    uint32_t chunks = 0u;

    {
      std::unique_lock<std::mutex> lock(mutex);

      wake.wait(lock, [this, seen] { return stopping || generation != seen; });

      if (stopping)
      {
        return;
      }

      seen = generation;
      body = currentBody;
      count = currentCount;
      chunks = currentChunks;
    }

    if (body == nullptr || chunks == 0u)
    {
      continue;
    }

    while (true)
    {
      const uint32_t chunk = nextChunk.fetch_add(1u);

      if (chunk >= chunks)
      {
        break;
      }

      const uint32_t begin = static_cast<uint32_t>((static_cast<uint64_t>(count) * chunk) / chunks);
      const uint32_t end = static_cast<uint32_t>((static_cast<uint64_t>(count) * (chunk + 1u)) / chunks);

      (*body)(chunk, begin, end);

      if (finishedChunks.fetch_add(1u) + 1u == chunks)
      {
        std::lock_guard<std::mutex> lock(mutex);

        done.notify_all();
      }
    }
  }
}

void WorkerPool::parallelFor(const uint32_t count, const uint32_t chunks,
                             const std::function<void(uint32_t, uint32_t, uint32_t)> &body)
{
  if (count == 0u || chunks == 0u)
  {
    return;
  }

  if (threads.empty() || chunks == 1u)
  {
    for (uint32_t chunk = 0u; chunk < chunks; ++chunk)
    {
      const uint32_t begin = static_cast<uint32_t>((static_cast<uint64_t>(count) * chunk) / chunks);
      const uint32_t end = static_cast<uint32_t>((static_cast<uint64_t>(count) * (chunk + 1u)) / chunks);

      body(chunk, begin, end);
    }

    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);

    currentBody = &body;
    currentCount = count;
    currentChunks = chunks;

    nextChunk.store(0u);
    finishedChunks.store(0u);

    ++generation;
  }

  wake.notify_all();

  // The calling thread takes chunks too rather than blocking idle.
  while (true)
  {
    const uint32_t chunk = nextChunk.fetch_add(1u);

    if (chunk >= chunks)
    {
      break;
    }

    const uint32_t begin = static_cast<uint32_t>((static_cast<uint64_t>(count) * chunk) / chunks);
    const uint32_t end = static_cast<uint32_t>((static_cast<uint64_t>(count) * (chunk + 1u)) / chunks);

    body(chunk, begin, end);

    if (finishedChunks.fetch_add(1u) + 1u == chunks)
    {
      std::lock_guard<std::mutex> lock(mutex);

      done.notify_all();
    }
  }

  std::unique_lock<std::mutex> lock(mutex);

  done.wait(lock, [this, chunks] { return finishedChunks.load() >= chunks; });

  currentBody = nullptr;
}

} // namespace graphrewrite::runtime
