#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// Furniture, and the light over it.
//
// Not a sprinkle. Watabou's `Clutter` spawns loose shapes inside a cell and
// then SLIDES each cluster bodily along one direction until it touches
// something - and the thing it slides away from is the room's own circulation.
// That is why his rooms look furnished rather than decorated: everything ends
// up against a wall, in the part of the room nobody walks through, and nothing
// ever comes to rest across a doorway.

// A piece, before it becomes a footprint.
struct ClutterPiece
{
  double x = 0, z = 0;      // Centre, metres.
  double halfLong = 0;      // Along its own axis.
  double halfShort = 0;
  double angle = 0;         // Radians, from the room's frame.
  bool round = false;
  uint32_t cluster = 0;
};

// The seven scalars a room type hands the solver, in Watabou's own order.
struct ClutterRule
{
  double size = 0;      // Zero draws a bell-shaped default.
  // How firmly the cluster is pressed against what it lands on.
  //
  // One slides it all the way; nought leaves it where it spawned. Watabou
  // writes this as `min(rnd / (1 - press), 1)`, so at one the division by zero
  // pins it and at nought the branch short-circuits - which reads backwards
  // until you notice his bedroom asks for one and comes out hard against a
  // wall.
  double press = .6;
  uint32_t pieces = 3;  // Most per cluster.
  double shrink = .8;   // How fast the pieces in a cluster fall away.
  double align = .7;    // How often a piece stays square to the room.
  double round = .8;    // How often it is a rectangle rather than a disc.
  double density = .4;  // Clusters per cell of floor.
};

ClutterRule clutterFor(rewrite::RoomKind);

// Where a light hangs.
struct RoomLight
{
  double x = 0, z = 0;
  double radius = 0;
  bool onWall = false;
};

// Fills one room.
//
// `abris` - the things a piece can come to rest against - is the room's wall
// polyline PLUS a spur half a cell inward at every opening. The spur is the
// whole reason nothing ends up in a doorway, and it costs two points.
bool furnishRoom(const CellGrid &, const CellHouse &, const CellFloor &, uint32_t room, const ClutterRule &, Stream &, std::vector<ClutterPiece> &out, double outerWallMetres = .20, double innerWallMetres = .10);

// Where the light goes.
//
// Two rules, and which applies is a property of the room rather than a choice.
// A basement or a corridor with more than one door gets a sconce at its darkest
// corner - the one furthest, in inverse square, from every window, door and the
// front door. Everything else gets a fixture at the point deepest inside the
// room, which is the summit of the room's own straight skeleton and is a far
// better answer than a centroid on any plan that is not convex.
bool lightRoom(const CellGrid &, const CellHouse &, const CellFloor &, uint32_t room, RoomLight &out);

// Every room of every floor.
bool furnishHouse(const CellHouse &, const CellDwellingSettings &, Stream &, std::vector<rewrite::DwellingFurniture> &furniture, std::vector<RoomLight> &lights, std::string *error = nullptr);

} // namespace procedural::world::generator
