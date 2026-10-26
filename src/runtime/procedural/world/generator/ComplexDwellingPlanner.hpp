#pragma once

#include <cstdint>

#include "procedural/world/WorldDwellingPlan.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// A dwelling whose interior is the shape of its footprint.
//
// `planDwelling` takes a rectangle and nothing else: it works in a local frame
// where the wing is W by D and every room is a Rect in it. That is the right
// model for a wing of a compound and the wrong one for a house, because a
// house's footprint is whatever the lot left - a trapezoid from a Voronoi
// block, an L where two streets meet, a U around a yard. Planned as a
// rectangle, the rooms inside a building do not line up with the building, and
// the render shows walls crossing the outline.
//
// So this plans on the polygon. The subdivision is the one the town's lots use
// - halve the area, judge the shape, keep two fixed axes - because a room and a
// lot are the same problem at different scales, and that rule is already known
// to produce squarish pieces on an awkward outline rather than wedges.
//
// What it does not do is any of the programme reasoning `planDwelling` carries:
// no reserved hall, no stair core, no jettying. Rooms come out of the shape,
// purposes are fitted to the rooms afterwards, and the stair stands in a corner
// of the largest one. That is the ordering Watabou's Dwellings uses and the
// opposite of the rectangular planner's.
// Which parts of a mass the roof rises to, rather than falls from.
//
// A hip and a gable are not two qualities of a roof; they are two answers to
// the same question, asked once per wall: does the surface come down to this
// wall and stop, or does the wall carry on up and the surface die into it?
// That is a design decision and it is taken here, before any geometry exists,
// rather than read back off the shape afterwards.
//
// One decision is not a choice at all, though, and it is taken whatever the
// style says: where a wing joins a body, or where two masses stand against
// each other, the roof gables. Hipping there would run the surface down into
// the join from both sides and leave a gutter in the middle of the building
// with nowhere to fall - which is what a street of these looked like, and
// what "the middle, where two roofs meet, should be gabled" is about. The
// style decides the *free* ends: the ones standing in open air.
enum class RoofStyle : uint8_t
{
  // Falls on every free side. The ends of a range are hipped, so an extruded
  // wing reads as a hipped stub with a gable only where it joins.
  Hipped,

  // The body hips, each wing gables at its own end. The English cross-gable:
  // a main range under a hipped roof with gabled cross-wings, which is what
  // most of a town looks like.
  CrossGabled,

  // Every free end gables. A terrace, where a range runs between party walls
  // and its roof stops flat against both.
  Gabled,
};

struct ComplexDwellingSettings
{
  // Target floor area of a room, in square metres. Rooms come out log-uniform
  // about it, as lots do.
  double roomAreaMetres = 16;

  // No cut may leave a piece smaller than this.
  double minimumRoomMetres = 6;

  // ...nor narrower than this, which is what a room needs to be usable.
  double minimumWidthMetres = 2.1;

  // A piece longer than this ratio keeps splitting whatever its area, so the
  // sides of a room come out as equal as the footprint allows.
  double squareEnoughRatio = .58;

  double wallThickness = .2;

  // Width of a corridor, and the smallest footprint that gets one.
  //
  // Carved from the entrance before any room is cut, so the rooms are what is
  // left either side of it and every one of them has a wall on it. Placing
  // doors afterwards on whatever walls two rooms happen to share is what makes
  // a plan you walk through a bedroom to cross; a corridor is the thing that
  // stops it, and it has to exist before the rooms do.
  double corridorWidthMetres = 1.4;
  double corridorFromAreaMetres = 55;

  // A corridor turns and runs across the building as well, once the footprint
  // is deep enough for one arm not to reach everything.
  double corridorBranchFromAreaMetres = 130;

  // The narrowest a corridor may be anywhere along it.
  //
  // Clipping a band to a building leaves whatever the outline leaves - a
  // triangular stub in a corner, a pinch where the strip crosses a sloping
  // wall at a shallow angle - and both are corridor by area with no room to
  // pass. Width is the question, and area cannot answer it.
  double minimumWalkableMetres = .9;

  // How wide a part of the plan has to be to carry another storey.
  //
  // Watabou's mansion generator states the rule outright: "the number of
  // storeys at any particular place of a building is defined by its ground
  // plan. One-cell wide pieces usually end up being 1 storey tall." A mansion
  // is not a prism - its wings are lower than its body and its towers higher -
  // and a plan that gives every part the same height is a block of flats
  // whatever its outline.
  //
  // A room carries a storey above it when the building is at least this wide
  // there. Measured on the *plan*, not on the room: a narrow wing is narrow
  // however its rooms are cut.
  double storeyWidthMetres = 7.5;

  // ...and a part this wide carries every storey the brief asks for.
  double fullHeightWidthMetres = 13;

  // How long a rectangular block has to be before it is cut into two wings.
  //
  // A wing is a block with one roof over it. Left whole, a long building gets
  // one ridge running the length of it, which is a barn; cut, it gets the
  // several ridges a house of that size actually has.
  double wingLengthMetres = 16;

  // How far a roof oversails the wall it covers.
  //
  // A roof flush with its walls is a box with a lid. The overhang is what
  // throws rain clear of the wall, and from above it is what makes a building
  // read as roofed rather than as a floor plan extruded.
  double eavesOverhangMetres = .45;

  // Which ends the roof rises to. See RoofStyle: a join is gabled whatever
  // this says, and this decides the ends standing in open air.
  RoofStyle roofStyle = RoofStyle::CrossGabled;

  // How deep a composed wing may be, across its ridge.
  //
  // A wing is one room deep either side of nothing, or either side of a
  // corridor, so its depth is bounded by what a building of that age can span
  // and not by the plot. Left to grow with the plot, a large lot produces one
  // fat block rather than a building with wings.
  double minimumWingDepthMetres = 5.5;
  double maximumWingDepthMetres = 10.0;
};

// The massing of a building: wings, their storeys, their roof clusters and the
// surfaces of the roof over each.
//
// Exposed on its own because a building with no interior - one of the town's
// ordinary houses, which is a footprint and a height - has the same exterior
// as a planned one and no reason to go through the planner to get it.
//
// `outline` is the ground, counter clockwise; its y is ignored.
void massBuilding(const std::vector<rewrite::Position> &outline, double foundation, double floorRise, uint32_t maximumStoreys, const ComplexDwellingSettings &, std::vector<rewrite::DwellingWing> &, std::vector<rewrite::DwellingRoofFace> &, std::vector<rewrite::DwellingStorey> * = nullptr, std::vector<rewrite::DwellingPillar> * = nullptr, double jettyMetres = 0, double arcadeDepthMetres = 0, uint32_t minimumStoreys = 1);

// What a composed building is asked to be.
struct MassingBrief
{
  std::vector<rewrite::Position> plot; // The ground it may stand on, counter clockwise.

  // Ground inside the outline that is not available: the court a ring lot
  // wraps around. Without it the composer sees a solid plot and builds a wing
  // across the court, and the outline it returns carries the court as a hole
  // that the next layer silently drops.
  std::vector<std::vector<rewrite::Position>> holes;
  uint64_t seed = 0;
  double coverage = .5; // Share of the plot to build over.
  double foundation = 0, floorRise = 2.9;
  uint32_t minimumStoreys = 1, maximumStoreys = 1;

  // The direction the first wing runs. Zero means the plot's longest edge.
  //
  // A house runs along its street, not along whatever edge of its lot happens
  // to be longest - and on a deep narrow lot those are at right angles.
  double axisX = 0, axisZ = 0;

  // A point the first wing is placed nearest to: the street. Without one it
  // lands anywhere on the plot, and a house in the middle of its lot is a
  // villa, and a street of villas is not a town.
  bool anchored = false;
  double anchorX = 0, anchorZ = 0;

  // How far each floor above the ground oversails the one below it.
  //
  // A jetty. Timber framing carries the upper floor's joists out past the
  // wall below, so a town house is wider at the top than at the bottom and
  // leans over its street. It is also the cheapest way for a building to stop
  // being a prism: without it every storey plate has the same outline as the
  // one under it, and the only variety left is which wings stop early.
  double jettyMetres = 0;

  // How far the ground floor is set back from the floor above it, on the
  // street front, with posts carrying the overhang.
  //
  // An arcade, or an undercroft: the pavement runs under the building. Where
  // a jetty pushes the upper floors out by a hand's breadth, this pulls the
  // ground floor in by a stride, and what is underneath is open ground.
  double arcadeDepthMetres = 0;
};

// The massing of a building composed on a plot rather than carved out of a
// footprint it was handed.
//
// One wing is laid down and further wings are pushed out from the sides of
// those already standing, each kept only if it stays on the plot and clear of
// the rest. The shape that results is not chosen from a list: an L, a T, a U
// and a courtyard are all outcomes of the same rule, and so are the forms
// with no name.
void massComposedBuilding(const MassingBrief &, const ComplexDwellingSettings &, std::vector<rewrite::DwellingWing> &, std::vector<rewrite::DwellingRoofFace> &, std::vector<rewrite::DwellingStorey> * = nullptr, std::vector<rewrite::DwellingPillar> * = nullptr);

// The short form: no axis, no anchor, one storey minimum.
void massComposedBuilding(const std::vector<rewrite::Position> &plot, uint64_t seed, double coverage, double foundation, double floorRise, uint32_t maximumStoreys, const ComplexDwellingSettings &, std::vector<rewrite::DwellingWing> &, std::vector<rewrite::DwellingRoofFace> &, std::vector<rewrite::DwellingStorey> * = nullptr);

// Plans every floor of `brief` on its own footprint. The shell may have any
// number of vertices and need not be convex; it must be simple and counter
// clockwise.
bool planComplexDwelling(const rewrite::DwellingBrief &brief, const ComplexDwellingSettings &, rewrite::DwellingPlan &, std::string *error = nullptr);

} // namespace procedural::world::generator
