#include "DwellingStairs.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace procedural::world::generator {

StairSteps stepsFor(const double rise, const double comfortableRiser, const double available)
{
  StairSteps steps;

  if (!(rise > .05))
  {
    return steps;
  }

  steps.risers = std::max(2u, uint32_t(std::ceil(rise / std::max(comfortableRiser, .05))));
  steps.riser = rise / double(steps.risers);

  // A pace, less twice the rise of a step. Bounded below, because a riser high
  // enough to make the going negative is a ladder and the answer there is more
  // steps, not a tread you cannot put a foot on.
  steps.tread = std::max(.22, .63 - steps.riser * 2);

  // The going is measured over the treads, and the top step is the floor you
  // arrive on: a flight of N risers has N - 1 treads in front of it.
  steps.going = steps.tread * double(steps.risers - 1u);

  // Given less run than that, the *tread* gives - never the number of risers.
  //
  // Dropping risers to fit was the first answer and it is exactly backwards:
  // the climb is fixed, so fewer risers means a taller one, and a flight asked
  // to fit a short wall came back at two hundred and eighty millimetres a step,
  // which is a ladder. The number of steps is settled by the height and nothing
  // else; what a short run can change is how deep each one is, down to a tread
  // you can still put a foot on.
  if (available > 0 && steps.going > available && steps.risers > 1u)
  {
    steps.tread = std::max(.19, available / double(steps.risers - 1u));
    steps.going = steps.tread * double(steps.risers - 1u);
  }

  return steps;
}


bool planStairPath(const std::vector<StairLeg> &legs, const double base, const double top, const double width, const double riser, const double boards, const uint32_t lowerFloor, std::vector<rewrite::DwellingStair> &flights, std::vector<std::vector<Position>> &landings, const double landingDepth, const double available)
{
  if (legs.empty() || !(top + boards > base + .2))
  {
    return false;
  }

  // Every flight lands on the *surface*, not on the structure under it. A
  // storey's level is where its joists are and the boards are a thickness
  // above; a flight stopping at the first leaves a lip at its own top, and a
  // lip that size reads as a fault rather than a detail.
  const double climb = (top + boards - base) / double(legs.size());

  double height = base;
  std::vector<Position> ends;

  for (size_t i = 0; i < legs.size(); ++i)
  {
    const StairLeg &leg = legs[i];
    // With the run the caller has, not with none.
    //
    // This was called with no limit, so the going it produced could be longer
    // than the going the caller sized the room against - and a flight longer
    // than its budget runs out through the back wall of the room it is in.
    // The caller does the same sum; passing it in is what makes the two agree.
    const StairSteps steps = stepsFor(climb, riser, available);

    const auto corner = [&](const double along, const double across) {
      return Position{leg.from.x + leg.alongX * along + leg.acrossX * across, height, leg.from.z + leg.alongZ * along + leg.acrossZ * across};
    };

    rewrite::DwellingStair flight;
    flight.opening = {corner(0, 0), corner(0, width), corner(steps.going, width), corner(steps.going, 0)};
    // The two ends, as pieces of floor rather than as a fact about the flight.
    //
    // A flight with no landing is a set of steps you step onto from nowhere.
    // The lower one is the place you walk to in order to start climbing; the
    // upper one is the place you arrive, and it is the piece that must *not*
    // be cut out of the floor above - arriving into the hole the stair came up
    // through is the same fault as having no landing at all.
    // A square on each floor, and the flight joins an edge of one to an edge
    // of the other.
    //
    // Square rather than a strip the width of the flight, because a landing is
    // a place you turn round in: you arrive across it and leave along one of
    // its other edges, and a strip has only the edge you came in by. Centred
    // on the flight, so it reaches a little wider than the steps do on both
    // sides and there is somewhere to stand aside.
    const double deep = std::max({landingDepth, width, .6});
    const double spare = (deep - width) * .5;

    flight.lowerLanding = {corner(-deep, -spare), corner(-deep, width + spare), corner(0, width + spare), corner(0, -spare)};
    flight.upperLanding = {corner(steps.going, -spare), corner(steps.going, width + spare), corner(steps.going + deep, width + spare), corner(steps.going + deep, -spare)};
    flight.lowerFloor = lowerFloor;
    flight.rise = climb;
    flight.risers = steps.risers;
    flight.tread = steps.tread;
    flight.startRise = height - base;
    flight.width = width;
    flight.privateAccess = false;

    ends.push_back(corner(steps.going, 0));
    ends.push_back(corner(steps.going, width));

    flights.push_back(std::move(flight));

    height += climb;

    // The landing at the turn: a patch of floor spanning from where this leg
    // finished to where the next one starts. It has to touch both, because a
    // landing that touches neither is what two flights near each other look
    // like.
    if (i + 1 < legs.size())
    {
      const StairLeg &next = legs[i + 1];

      const Position a = ends[ends.size() - 2], c = ends.back();
      const Position d{next.from.x, height, next.from.z};
      const Position e{next.from.x + next.acrossX * width, height, next.from.z + next.acrossZ * width};

      landings.push_back({Position{a.x, height, a.z}, Position{c.x, height, c.z}, e});
      landings.push_back({Position{a.x, height, a.z}, e, d});
    }
  }

  return true;
}

} // namespace procedural::world::generator
