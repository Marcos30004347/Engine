#pragma once

// A small persistent worker pool for the CPU matcher.
//
// Persistent because the unit of work is one instruction over a few thousand
// rows: spawning threads per instruction costs more than the work itself.
//
// `parallelFor` hands each worker a contiguous CHUNK rather than interleaving
// rows, and results are concatenated in chunk order. That is what keeps the
// output identical to the serial run whatever the worker count and whatever
// order the threads happen to finish in - a matcher whose answer depends on
// scheduling is not something you can check against a reference.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace graphrewrite::runtime {

class WorkerPool
{
public:
  // `workers` is the number of ADDITIONAL threads; 0 runs everything on the
  // calling thread.
  explicit WorkerPool(uint32_t workers);
  ~WorkerPool();

  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  uint32_t workerCount() const
  {
    return static_cast<uint32_t>(threads.size());
  }

  // Splits [0, count) into `chunks` contiguous ranges and runs `body(chunk,
  // begin, end)` for each. Returns once every chunk has finished.
  void parallelFor(uint32_t count, uint32_t chunks, const std::function<void(uint32_t, uint32_t, uint32_t)> &body);

  // How many chunks to use for a given amount of work: enough to keep every
  // worker busy, but never so many that the per-chunk overhead dominates.
  uint32_t chunkCountFor(uint32_t count) const;

  static uint32_t hardwareWorkers();

private:
  void workerLoop();

  std::vector<std::thread> threads;

  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable done;

  const std::function<void(uint32_t, uint32_t, uint32_t)> *currentBody = nullptr;
  uint32_t currentCount = 0u;
  uint32_t currentChunks = 0u;
  std::atomic<uint32_t> nextChunk{0u};
  std::atomic<uint32_t> finishedChunks{0u};
  uint64_t generation = 0u;
  bool stopping = false;
};

} // namespace graphrewrite::runtime
