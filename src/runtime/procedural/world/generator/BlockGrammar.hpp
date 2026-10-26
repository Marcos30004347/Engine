#pragma once

#include "Ledger.hpp"
#include "PatchRegion.hpp"

#include <functional>
#include <string>
#include <vector>

namespace procedural::world::generator {

// Townscaper-style block grammar (B1-B3): an irregular quad grid inside one
// city block (jittered lattice, triangle/quad merge, midpoint subdivision,
// relaxation), then a bounded simplified wave-function collapse that tiles
// the quads as lots, alleys and courts, with alleys that climb a terrain step
// becoming stairs. Used for land allocation, stair alleys and exterior
// formations only; houses on the lots come from the dwelling planner.
enum class BlockTile : uint8_t
{
  Street, // Boundary cell on a street side; fixed.
  Alley,
  Stair,
  Lot,
  Court,
  Unresolved
};

struct BlockCell
{
  uint32_t id = 0;
  ConvexRing ring; // Quad (or triangle) in the block's local frame.
  Position centre;
  double elevation = 0;
  BlockTile tile = BlockTile::Unresolved;
  std::vector<uint32_t> neighbours;
  bool accessible = false; // Lot with an alley/street neighbour, or alley reached from a street.
};

struct BlockGrammarSettings
{
  double cellSpacingMetres = 7; // Lattice spacing before subdivision (lots end up around this size).
  uint32_t relaxationPasses = 3;
  double stairStepMetres = .6; // Height difference between adjacent alley cells that makes a stair.
  double lotWeight = .62, alleyWeight = .28, courtWeight = .10;
  uint32_t maximumRestarts = 16;
  uint64_t seed = 1;
};

struct BlockPlan
{
  uint64_t block = 0;
  std::vector<BlockCell> cells;
  uint32_t lots = 0, alleys = 0, stairs = 0, courts = 0, streets = 0, unresolved = 0, inaccessibleLots = 0, restarts = 0;
  bool solved = false;
};

// `streetSides` marks which boundary edges of `outline` face a street; the
// terrain callback supplies elevation in the same local frame.
bool planBlockGrammar(
    uint64_t block,
    const ConvexRing &outline,
    const std::vector<bool> &streetSides,
    const std::function<double(double, double)> &terrain,
    const BlockGrammarSettings &,
    BlockPlan &,
    Ledger &,
    std::string *error = nullptr);

} // namespace procedural::world::generator
