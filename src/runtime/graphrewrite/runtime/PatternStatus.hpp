#pragma once

#include <cstdint>

namespace graphrewrite::runtime {

enum class PatternState : uint32_t
{
  Running = 0u,
  Complete = 1u,
  Overflowed = 2u,
};

// Mirrors the WGSL struct in graph-publish.wgsl. The CPU reads this back each
// frame and enqueues a rewrite for every Complete pattern.
struct PatternStatus
{
  uint32_t state = 0u;
  uint32_t rows = 0u;
  uint32_t cols = 0u;

  // Bumped once per rewrite step. The readback lags the GPU by a frame or two,
  // so without a generation a status left over from an earlier step is
  // indistinguishable from this step's result.
  uint32_t generation = 0u;

  uint32_t resultOffset = 0u;
  uint32_t pad0 = 0u;
  uint32_t pad1 = 0u;
  uint32_t pad2 = 0u;

  bool isComplete() const
  {
    return state == static_cast<uint32_t>(PatternState::Complete);
  }

  bool isFresh(const uint32_t currentGeneration) const
  {
    return generation == currentGeneration;
  }
};

static_assert(sizeof(PatternStatus) == 32u, "PatternStatus must match its WGSL layout");

struct PublishParams
{
  uint32_t patternId = 0u;
  uint32_t rows = 0u;
  uint32_t cols = 0u;
  uint32_t generation = 0u;
  uint32_t resultOffset = 0u;
  uint32_t overflowed = 0u;

  // A pass records once, at registration, so the dispatch set is fixed. This is
  // how a frame decides which of those dispatches actually publish.
  uint32_t enabled = 0u;
  uint32_t pad0 = 0u;
};

static_assert(sizeof(PublishParams) == 32u, "PublishParams must match its WGSL layout");

} // namespace graphrewrite::runtime
