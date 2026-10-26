#pragma once

// When each edge type is next needed.
//
// For instruction i and edge type t, the smallest index >= i at which some
// instruction reads t, or `never` if none does. This is the next-use distance
// that makes the runtime's page eviction Belady-optimal for a straight-line
// programme: when the page pool is full, evict the page whose type is needed
// furthest away.

#include "graphrewrite/compiler/Program.hpp"

namespace graphrewrite::compiler {

struct UsageValues
{
  static constexpr uint32_t never = ~uint32_t(0);

  uint32_t instructionCount = 0u;
  uint32_t edgeTypeCount = 0u;

  std::vector<uint32_t> nextUse; // [instruction * edgeTypeCount + type]

  uint32_t at(const uint32_t instruction, const EdgeTypeId type) const
  {
    return nextUse[static_cast<size_t>(instruction) * edgeTypeCount + type];
  }
};

UsageValues computeUsageValues(const CompiledProgram &program);

std::string dumpUsageValues(const UsageValues &usage, const CompiledProgram &program);

} // namespace graphrewrite::compiler
