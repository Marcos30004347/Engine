#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world::generator {

using rewrite::Position;

// How a flight is stepped: how many, how high, how deep, and how far it runs.
//
// One rule, in one place. There were three - `ceil(rise / .18)` in a room,
// `ceil(rise / towerRiser)` in a tower, and the same again halved on an
// outside stair - and each was a different answer to the same question, so a
// building could carry three stairs no two of which you could walk up the same
// way.
//
// The rule is the builder's own: twice the riser plus the going is a pace,
// about six hundred and thirty millimetres. Fix the number of risers from the
// climb, take the riser from that, and the going follows - which is why a tall
// storey gets *more* steps rather than steeper ones, and why a flight given a
// shorter run than it needs comes back saying so rather than quietly steepening
// until it fits.
struct StairSteps
{
  uint32_t risers = 2;
  double riser = .18, tread = .27, going = .54;
};

StairSteps stepsFor(double rise, double comfortableRiser, double available);

// One leg of a stair's path: where it starts, the way it runs, and the way its
// treads lie across it.
//
// The across direction is given rather than derived. Deriving it is one cross
// product and it was the single commonest mistake in this whole area - a sign
// the wrong way round puts a flight on the far side of the wall it is supposed
// to hug, and it looks plausible until you notice the building is on the other
// side of it. Said out loud by the caller, who knows which way is out, it
// cannot be got wrong silently.
struct StairLeg
{
  Position from;
  double alongX = 1, alongZ = 0;
  double acrossX = 0, acrossZ = 1;
};

// A stair from one floor to another, as a path of legs with landings at the
// turns.
//
// This exists because of what went wrong without it. The path, the landings and
// the two ends were written out separately at each place a stair was made -
// three of them - and every single correction this area needed was about a
// *joint*: which frame two pieces shared, which end a rail opened, which level
// the boards sat at, whether the landing touched both flights. None was about
// stairs. So the joints are computed once, here, and a caller says only where
// its legs run.
//
// The rise is split evenly between the legs, because a turn halfway up is a
// turn halfway up: a flight that climbs most of the storey and a second that
// climbs the rest is two different stairs joined at a landing you trip on.
bool planStairPath(const std::vector<StairLeg> &legs, double base, double top, double width, double riser, double boards, uint32_t lowerFloor, std::vector<rewrite::DwellingStair> &flights, std::vector<std::vector<Position>> &landings, double landingDepth, double available);

} // namespace procedural::world::generator
