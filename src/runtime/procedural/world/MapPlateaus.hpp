#pragma once

#include "procedural/world/MapGraph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

struct PlateauSettings
{
  // Vertical rung of the global ladder. Zero takes the contour interval, which
  // is the point: a plateau's brink is then *on* a contour, so it is already a
  // constraint, the terrace shelves already align, and the terrace graph gets
  // integer levels for free.
  double ladderIntervalMetres = 0.0;

  // How far a block stands above the ground it interrupts, in rungs. One rung
  // at a 20 m ladder is a bank; three is a cliff you cannot climb.
  uint32_t minimumRungs = 1u;
  uint32_t maximumRungs = 3u;

  // Target area of one block and the share of the land to cover. Blocks grow
  // until the share is met, so this is really "how much of this world is mesa".
  double targetBlockAreaSquareMetres = 6.0e6;
  double coverageFraction = 0.16;
  uint32_t minimumBlockCells = 3u;

  // A block may not contain a cell whose surroundings fall away by more than
  // this: flattening a mountainside produces a wall, not a mesa.
  double maximumLocalReliefMetres = 260.0;

  uint64_t seed = 0u;
};

// One flattened block. `riserMetres` is the drop from the block's own surface
// to the ground immediately outside it - the height of the cliff.
struct PlateauBlock
{
  uint32_t id = kInvalidNode;
  std::vector<uint32_t> cells;
  double elevationMetres = 0.0;
  int32_t rung = 0;
  double riserMetres = 0.0;
  double areaSquareMetres = 0.0;
};

struct PlateauField
{
  std::vector<PlateauBlock> blocks;
  // Block id per node, or kInvalidNode. Indexed by node id.
  std::vector<uint32_t> cellBlock;
  double ladderIntervalMetres = 0.0;
  uint32_t flattenedCells = 0u;
  double meanRiserMetres = 0.0;
};

// Stage 5B: pick connected sets of land cells and flatten each to one rung of
// a global elevation ladder.
//
// Runs *before* the definitive drainage pass, and that is the whole scheduling
// argument: flatten after the rivers and they run uphill across the block;
// flatten before and the drainage that follows routes water around it and drops
// it off the brink, where the existing waterfall detector marks the knickpoint
// for free.
//
// Writes `intent.elevationMetres` and `type` on every flattened cell.
bool buildPlateaus(
    const PlateauSettings &settings,
    MapGraph &graph,
    PlateauField &field,
    std::string *error = nullptr);

} // namespace procedural::world
