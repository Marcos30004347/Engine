#pragma once

#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// How a building's class is measured, shaped and laid out.
//
// The claim this phase rests on: **the subdivision is scale-free, the footprint
// and the circulation are not.** Cutting a floor into rooms works identically on
// twenty cells and six hundred. What breaks above cottage scale is the stencil
// generator - its three-by-three stencil caps how articulated a silhouette can
// be - and corridors, which emerge from the subdivision on a cottage and have
// to be planned on a manor.
//
// So a class changes three things and nothing else: the grid it is measured on,
// how big a room on it is, and whether its circulation is planned. It does NOT
// choose an outline.
//
// That was tried and it was wrong. A weighted list of shapes per class - this
// one is an L range, that one an H, the other a ring - produces exactly the
// buildings the list describes and nothing else, and at the sizes a real lot
// offers they come out as hairlines: a forty metre range two cells deep, an E
// whose three teeth are one room wide. The stencil generator already makes
// articulated outlines from the grid and the seed, and it makes them at any
// size; what it needed was not a shape table but a block size scaled to the
// class's band. A class says how big and how fine. The seed says what shape.

struct ClassProfile
{
  GridClass grid;

  // Above this many cells a floor's circulation is reserved before its rooms
  // are cut. Zero means always; a very large number means never.
  uint32_t circulationThresholdCells = 60;

  // The smallest a room may be across, and how wide the corridors are, both in
  // cells. See `SubdivisionSettings::minimumRoomCells` for why the first is a
  // hard floor rather than a preference.
  uint32_t minimumRoomCells = 1;
  uint32_t corridorCells = 1;

  // How many cells across the reserved stair is: one is a stairwell, two or
  // three is a tower. See `Stairwell::block`.
  uint32_t stairTowerCells = 1;

  // How many storeys the stair tower carries ABOVE the mass it serves.
  //
  // This is what makes it a tower rather than a wide stairwell. A castle's
  // stair is the structure you see from outside the walls, and it is taller
  // than what it connects - the climb keeps going past the top floor and comes
  // out under its own roof. Zero for everything that is not a castle.
  uint32_t stairTowerAbove = 0;

  // Towers at the corners, and how far above the eaves they carry.
  //
  // Not the stair: these are the drum towers a castle has at the angles of its
  // walls, and there are four of them on the reference plan. They are expressed
  // as ROOMS THAT ARE TALLER rather than as extra storeys, because that is what
  // they are - the room in the corner of the top floor keeps going up - and
  // because the per-room height already makes the wall round them taller and
  // the main roof die into them as a gable.
  uint32_t cornerTowers = 0;
  double cornerTowerStoreys = 2.;

  // Courts: how often this class has one at all, how much of its ground goes to
  // it, and how many it may have.
  //
  // A court is a HOLE IN A MASS and not the middle of a ring. That is the whole
  // correction the reference plans force (see `docs/reference-plans.md`): the
  // `Courtyard` archetype stamped a range-thick ring and read as an open
  // square, while a real castle is a solid block of ranges with courts cut out
  // of it, off-centre, of unequal size. So the court is not a shape the class
  // asks for - it is chosen from the cells the planner already grew.
  double courtChance = 0;
  double courtShare = .18;
  uint32_t courts = 1;

  // Yards: how often this class has one, and how much of its ground goes to
  // it. Settled from the standing rather than named per class, because a yard
  // is what a household's plot is FOR at the modest end - a craftsman's yard
  // is his workshop's other half - and a mansion's ground is laid out, not
  // worked.
  double yardChance = 0;
  double yardShare = .14;

  // A pen for the beasts and a shed against it (P27): a rural plot's, and
  // nobody else's.
  double penChance = 0;
  double shedChance = 0;

  // How often this class shows the street a single straight wall with the door
  // in the middle of it.
  //
  // A constraint on the front, not a shape of its own, and that distinction is
  // the whole lesson of the footprint table above: a class that names an
  // outline can only make that outline. Mirroring the plan was tried and has
  // the same fault in miniature - reflecting half a stencil yields a U, a T or
  // a plus and little else, and those recur until they read as templates. A
  // guildhall is formal because someone designed its front; a cottage grew, and
  // a formal cottage looks municipal. Zero for most classes, and zero means the
  // stream is not drawn from at all.
  double symmetryChance = 0;

  // Cells per room when the floor is cut. A hall in a manor is not a parlour
  // in a cottage, and one number is what says so.
  double averageRoomCells = 6;

  uint32_t minimumStoreys = 1;
  uint32_t maximumStoreys = 8;

  // What the household's standing is, where the class implies one.
  PlotStanding standing = PlotStanding::Modest;
};

const ClassProfile &profileFor(BuildingClass);

// Reached only when a CALLER has settled the shape - the `square` tag, or a
// compound wing whose rectangle the curtain wall dictates. No class asks for
// one.
//
// Returned unplaced: the caller masks it to the buildable ground and chooses
// where on the lot it stands, exactly as the polyomino path already does.
// A footprint of the given kind, as cells at the origin, fitting inside the
// given span. The span is the ground there actually is, not the class's tier:
// a castle's plan is drawn for the lot it stands on, and one drawn for a board
// twice the lot's size is one that will never be placed.
std::vector<std::pair<int32_t, int32_t>> archetype(FootprintKind, const GridClass &, uint32_t maximumRows, uint32_t maximumColumns, Stream &);

// The settings a building of this class is planned with.
//
// The class owns five things and the caller owns the rest: the board tier, the
// cell size, the size band, the shape, and how big a room is. Tolerances,
// stair geometry, glazing and the jetty envelope are the caller's, because
// they are properties of the world the building stands in rather than of what
// kind of building it is.
CellDwellingSettings classed(const CellDwellingSettings &, BuildingClass, Stream &);

} // namespace procedural::world::generator
