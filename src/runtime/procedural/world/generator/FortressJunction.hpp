#pragma once

#include "procedural/world/generator/AccessGeometry.hpp"
#include "procedural/world/generator/AngularJoiner.hpp"
#include "procedural/world/hierarchy/Generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A wall approaches a polygonal tower along an arbitrary centreline. The wall
// remains a parallelepiped. Any angular gap between its square end and the
// tower face is filled by explicit triangular wedges, never hidden by overlap.
struct FortressWallArm
{
  rewrite::Position farEnd{};
  double width = 1.2;
};

struct FortressJunctionBrief
{
  rewrite::Position centre{};
  uint32_t towerSides = 6;
  double towerRadius = 4.;
  // Match the phase of the footprint used to trim adjoining curtain walls.
  double towerRotation = 0.;
  double towerWallDepth = .55;
  double base = 0, height = 9.;
  uint32_t floors = 3;
  double stairWidth = 1.;
  double belfryHeight = 2.8;
  double roofHeight = 2.2;
  double roofOverhang = .45;
  bool sweptSpire = false; // Flared eaves and a concave, multi-stage crown.
  double belfryPierSize = .42;

  // Which way the town is. A tower is entered from INSIDE the circuit - that
  // is the whole point of a tower on a curtain wall - so the doorway is cut
  // in the face whose outward normal points this way. Left zero, the tower is
  // sealed, which is what it was.
  rewrite::Position inward{};
  double doorWidth = 1.1, doorHeight = 2.1;

  // Optional inhabited tower: complete floors with a central return stair.
  bool inhabitedFloors = false;
  // World directions with y equal to the requested threshold elevation.
  std::vector<rewrite::Position> elevatedDoors;
  std::vector<FortressWallArm> walls;
};

// One polygonal-stair corner is deliberately two horizontal triangles.  The
// walkable triangle turns between flights; the smaller closure triangle fills
// the remaining pocket up to the unmodified interior wall corner.
struct FortressStairTurn
{
  std::vector<rewrite::Position> walkable;
  std::vector<rewrite::Position> wallClosure;
};

struct FortressJunctionBlueprint
{
  std::vector<rewrite::Position> tower;
  std::vector<rewrite::Position> towerInner;
  // One ordinary full-depth wall prism per polygon face. Adjacent walls meet
  // at their unmodified polygon corner; stair joinery never changes the shell.
  std::vector<std::vector<rewrite::Position>> towerWallPlans;
  std::vector<FortressStairTurn> stairTurns;
  std::vector<std::vector<rewrite::Position>> walls;
  std::vector<std::vector<rewrite::Position>> joiners; // Triangles in plan.
  std::vector<uint32_t> joinerWall;                    // Wall each triangle adapts.
  std::vector<uint32_t> wallFace;
  access::Model towerAccess;
  std::vector<std::vector<rewrite::Position>> topDeckFaces;
  std::vector<std::vector<rewrite::Position>> belfryPiers;
  std::vector<std::vector<rewrite::Position>> roofFaces;
  std::vector<rewrite::Position> stairOpening;

  // The doorway: which tower face carries it, and the opening itself as a
  // quad on the inner face (jambs and head), for anything that wants to draw
  // or walk through it. UINT32_MAX when the tower has no door.
  uint32_t doorFace = UINT32_MAX;
  std::vector<rewrite::Position> doorway;

  // `along` is where the opening is centred on its face, as a fraction of
  // the face's length. A wall-walk door is centred where the curtain's axis
  // crosses the face, not on the face's middle: a twelve-sided tower meets
  // an axis-aligned wall on a face centre only when its footprint is turned
  // half a face, and a bevelled curtain meets it at a corner, so a door on
  // the face's middle landed half inside the parapet.
  struct Opening { uint32_t face; double bottom, top, width; double along = .5; };
  std::vector<Opening> openings;
  bool inhabitedFloors = false;
  double base = 0, height = 0;
  double belfryTop = 0, roofApex = 0;
};

enum class FortressPieceKind : uint8_t { Tower, StairTurn, Wall, Joiner, TopDeck, BelfryPier, Roof, StairFlight, Guard, FloorSlab };

struct FortressPiece
{
  FortressPieceKind kind = FortressPieceKind::Wall;
  std::vector<uint32_t> faces;
};

struct FortressContact
{
  uint32_t a = 0, b = 0;
  rewrite::Position at{};
};

struct FortressJunctionStructure
{
  std::vector<std::vector<rewrite::Position>> faces;
  std::vector<FortressPiece> pieces;
  std::vector<FortressContact> contacts;
};

bool planFortressJunction(const FortressJunctionBrief &, FortressJunctionBlueprint &, std::string *error = nullptr);
FortressJunctionStructure buildFortressJunctionStructure(const FortressJunctionBlueprint &);
void meshFortressJunction(const FortressJunctionStructure &, hierarchy::Solid &);

} // namespace procedural::world::generator
