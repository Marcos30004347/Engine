#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// The roofs, one per storey, and the chimneys through them.
//
// A storey's roof covers what the storey ABOVE it does not: the leftover when
// the setback took a wing away. The walls of that leftover which stand against
// the taller mass are gables - the surface dies into them - and the rest are
// eaves, where it comes down and stops. That is one rule and it settles every
// hip and gable in the building without a style ever being consulted.
//
// The construction itself is `geom2::roofFromSkeleton`, unchanged. It is the
// straight skeleton as the L-infinity distance to the eaves, exact on any plan
// whose every reflex corner is a right angle - and a polyomino outline is
// exactly that, by construction. Watabou's own skeleton deadlocks on these
// inputs and he recovers by jittering every vertex and rebuilding until it
// completes; ours is built for them, and `no_jitter_needed` is the test that
// keeps that true.

struct CellRoofs
{
  std::vector<rewrite::DwellingRoofFace> faces;
  std::vector<rewrite::DwellingPillar> chimneys;

  // The parapet a fortified roof stands behind: a wall above the eaves line,
  // around the edge of the deck.
  //
  // A wall rather than a roof face, which is the whole point. A castle range is
  // not a house with a flat roof on it - it is a wall that continues past the
  // floor it carries - and drawing the parapet as roof geometry loses the
  // merlon line, the wall thickness and the fact that you can stand behind it.
  std::vector<rewrite::DwellingWall> parapets;

  // Where each storey's ridge peaks, for anything that needs the height.
  std::vector<double> summitHeight;
};

bool planRoofs(const CellHouse &, const CellDwellingSettings &, Stream &, CellRoofs &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
