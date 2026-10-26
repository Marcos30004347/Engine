#pragma once

#include "procedural/geom2/CellGrid.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// Cutting a floor into rooms.
//
// The core of the whole approach, and the reason for doing it. A cut is not a
// halving of a rectangle: it is a wall seeded at a corner or a mid-wall point
// of the floor's OWN outline, run straight into the plan until it reaches the
// far side. That is what makes the rooms line up with the building on a
// footprint that is not a rectangle, and it is what a bounding-box subdivision
// cannot do however it is scored.

struct SubdivisionSettings
{
  // Cells. The subdivision stops when an area is about this big, with a
  // bell-shaped threshold so rooms vary rather than all landing on it.
  double averageRoomCells = 6;

  // The threshold spans `average` to `average * 2^chaos`.
  double roomSizeChaos = 1;

  // Stop only on rectangles. Watabou's static default, which his own house
  // builder overrides to false - so ours defaults the way his runs.
  bool regularRooms = false;

  // Where cuts are seeded. "Inner walls bisect the outer shape" against "inner
  // walls follow the outer shape", which is his own description of the two.
  bool preferCorners = false; // tag: mechanical
  bool preferWalls = false;   // tag: organic

  // Whether narrow cells are penalised or rewarded below the room size.
  // Rewarding them is how corridors are generated on purpose - clearing this
  // is what the "hallways" tag does.
  bool noNooks = true;

  // The smallest a room may be across, in cells, and it is a HARD floor rather
  // than a preference.
  //
  // Everything else about the subdivision is scored and sampled, which is what
  // makes the plans vary - but a scored preference cannot express "never", and
  // a room one cell wide is a never. It is a corridor with furniture in it: at
  // two and a bit metres you cannot put a bed across it, and on a blueprint it
  // reads as a passage that somebody has mislabelled.
  //
  // Measured with `narrow`, the same primitive the corridor detector uses: a
  // cell is narrow when it belongs to no two-by-two block of its own room, so
  // "no narrow cells" is exactly "two cells across everywhere, in both
  // directions". One means the rule is off, which is Watabou's own behaviour
  // and right for a cottage where a two-metre scullery is a scullery.
  uint32_t minimumRoomCells = 1;

  uint32_t maximumRooms = 96;
};

SubdivisionSettings subdivisionFromTags(const std::vector<std::string> &tags, double averageRoomCells);

struct CellRoomShape
{
  Board area;
  Chain contour;
};

// Cuts `contour` into rooms.
//
// Enumerate and sample, rather than Watabou's reject and retry. He picks one
// notch, scores the split, and rerolls the whole contour when the score loses
// its coin flip - with no bound on the rerolls, which is why a port of it needs
// an attempt budget and why a bad seed can hang. Scoring every candidate cut
// once and then drawing proportionally gives the same distribution, always
// terminates, and is cheaper: each candidate is scored once instead of one
// candidate being scored many times.
bool subdivide(const CellGrid &, const Chain &contour, const SubdivisionSettings &, Stream &, std::vector<CellRoomShape> &rooms, std::vector<Chain> &innerWalls, std::string *error = nullptr);

} // namespace procedural::world::generator
