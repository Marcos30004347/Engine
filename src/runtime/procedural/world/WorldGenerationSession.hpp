#pragma once
#include "WorldGeneratorDirector.hpp"
namespace procedural::world::rewrite
{
struct GenerationSession
{
  uint64_t revision = 0, seed = 0;
  GenerationSnapshot inputs, products;
  std::vector<GenerationDescriptor> descriptors;
};
struct GenerationUpdateReport
{
  std::vector<std::pair<std::string, uint64_t>> executed, reused;
  std::vector<GenerationKey> removed;
};
// Inputs carry monotone revision tokens. Changed values MUST advance their token;
// opaque payload equality is intentionally not inferred. Callback-captured state
// changes require an explicit dirty invocation or descriptor version change.
// Replaces producer-owned output sets atomically, including retired generators.
// Shared external inputs are never removed merely because a producer retires.
bool updateGenerationSession(
    const std::vector<GenerationTask> &,
    const GenerationSnapshot &inputs,
    uint64_t worldSeed,
    uint64_t expectedRevision,
    const std::vector<std::pair<std::string, uint64_t>> &dirtyInvocations,
    GenerationSession &,
    GenerationUpdateReport &,
    const std::function<bool()> &cancelled = {},
    std::string *error = nullptr,
    uint32_t maximumWorkers = 1);
} // namespace procedural::world::rewrite
