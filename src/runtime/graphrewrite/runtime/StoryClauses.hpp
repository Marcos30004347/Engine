#pragma once

// The clause handlers the story programme uses - week arithmetic, ages, the
// church booking, the seeded roll - registered on an ExpressionHost. Both the
// CPU story runner and the GPU one call this, so the story reads the same
// whichever device matched it.

#include "graphrewrite/runtime/ExpressionHost.hpp"

#include <random>
#include <set>
#include <utility>

namespace graphrewrite::runtime {

// `booked` is the (place, week) table `availableAt` keeps; `rolls` is the
// generator `roll(lo, hi)` draws from. Both must outlive the host.
void registerStoryClauses(ExpressionHost &host, uint32_t dayBase, std::set<std::pair<uint32_t, uint32_t>> &booked,
                          std::mt19937 &rolls);

} // namespace graphrewrite::runtime
