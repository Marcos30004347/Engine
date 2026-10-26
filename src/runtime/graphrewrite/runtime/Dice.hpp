#pragma once

// The rewrite dice, shared by the CPU and GPU runners.
//
// This MUST stay identical to dice_roll/row_fires in graph-rewrite.wgsl. The
// benchmark asserts the two runners produce the same matches, and a probabilistic
// rewrite only keeps that property if both roll the same number for the same row.
//
// It is a hash, not a sequence: a stateful RNG would have to be advanced in the
// same order on both sides, and the GPU rolls every row concurrently, so there is
// no order to agree on. Hashing (seed, row) instead makes the roll a pure
// function of where the row is, which is reproducible on either device and stable
// regardless of how the work is partitioned.

#include <cstdint>

namespace graphrewrite::runtime::dice {

// Folds the ROUND into the seed. A rewrite round - a tick of the story, one
// applyRewrites on the GPU - re-matches the same graph and hands the applier
// the same rows at the same indices. Without this every row rolled the same
// number every round: a couple whose row rolled low had a child EVERY tick, and
// one whose row rolled high never did.
inline uint32_t roundSeed(const uint32_t runSeed, const uint32_t round)
{
  uint32_t h = runSeed ^ (round * 0x9e3779b9u + 0x7f4a7c15u);

  h ^= h >> 16u;
  h *= 0x7feb352du;
  h ^= h >> 15u;

  return h;
}

// Folds the rule into the seed so two rules matching the same row do not roll
// the same number.
inline uint32_t seedFor(const uint32_t runSeed, const uint32_t rule)
{
  uint32_t h = runSeed ^ (rule * 0x7feb352du);

  h ^= h >> 15u;
  h *= 0x846ca68bu;
  h ^= h >> 16u;

  return h;
}

inline uint32_t roll(const uint32_t seed, const uint32_t row)
{
  uint32_t h = seed ^ (row * 0x9e3779b9u);

  h ^= h >> 16u;
  h *= 0x85ebca6bu;
  h ^= h >> 13u;
  h *= 0xc2b2ae35u;
  h ^= h >> 16u;

  return h;
}

// The scale of a chance: `maybe N` in the language is N/10, but the runtime
// works in thousandths so that something outside the programme - a population
// pressure, a season - can scale a rule's chance with resolution to spare. A
// death rate of 1 in 200 is expressible here and not on a scale of ten.
constexpr uint32_t permille = 1000u;

// `chance` is 0..1000. 1000 (which is how an ALWAYS rule is encoded) skips the
// roll so an always-rule costs nothing.
inline bool fires(const uint32_t seed, const uint32_t row, const uint32_t chance)
{
  if (chance >= permille)
  {
    return true;
  }

  if (chance == 0u)
  {
    return false;
  }

  return (roll(seed, row) % permille) < chance;
}

} // namespace graphrewrite::runtime::dice
