#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator::access {

using rewrite::Position;

// The way into a building, as the simplest geometry that can carry the
// question.
//
// The previous two attempts at this built finished carpentry - treads, risers,
// balusters, posts - and then tried to reason about whether you could walk
// anywhere. Both failed the same way: every check agreed with the code that
// made it, and the renders stayed wrong. A stair with sixteen treads is not
// more checkable than a ramp; it is fifteen more things to get wrong before the
// question is even asked.
//
// So this layer is planes and nothing else. A place you can stand is a flat
// polygon. A way between two of them is a sloping quad. A door is a point that
// must stand on one. Whether you can get home is then a question about
// polygons, and it can be answered exactly rather than approximately.
//
// Treads come later, from this. A ramp with a foot edge, a head edge and a
// width has everything a stair builder needs and nothing it has to guess.

enum class SurfaceKind : uint8_t
{
  Ground,  // The dirt outside. Where a walk starts.
  Storey,  // The floor inside a building.
  Balcony, // A deck outside it, at a storey's level.
  Landing  // A place to turn, belonging to neither.
};

// A flat place you can stand, as a polygon at one height.
//
// Holes are not decoration: the stairwell a ramp comes up through is a hole,
// and a balcony that has one drawn as a solid rectangle is a balcony you cannot
// tell from a lid over a staircase.
struct Surface
{
  uint32_t id = 0;
  std::vector<Position> outer;               // Counter clockwise.
  std::vector<std::vector<Position>> holes;  // Clockwise, strictly inside.
  double height = 0;
  SurfaceKind kind = SurfaceKind::Ground;
  uint32_t storey = 0;
  std::string note;
};

// A sloping plane joining two surfaces: a stair, before it is a stair.
//
// Four corners. The two at the foot are at `from`'s height, the two at the head
// are at `to`'s height, and the quad between them is the whole of it. Which
// surfaces it joins is *claimed* here and checked against the geometry, never
// taken on trust - a ramp that says it reaches a balcony and stops a metre
// short is the fault this entire layer exists to catch.
struct Ramp
{
  uint32_t id = 0;
  Position footLeft, footRight;
  Position headLeft, headRight;
  uint32_t from = 0, to = 0;
  std::string note;
};

// A door, as a point you must be able to stand at.
struct Portal
{
  uint32_t id = 0;
  Position at;      // On the wall, at the storey's floor height.
  double into = 0;  // How far out from the wall the standing point is.
  double outX = 0, outZ = 1; // Which way is out.
  double width = 0.9;         // Clear wall opening, centred on `at`.
  double height = 2.1;        // Clear head height of the opening.
  uint32_t storey = 0;
  std::string note;
};

// The solid mass of a building: ground a surface may not be inside and a ramp
// may not pass through.
//
// A prism, because that is all it needs to be. The question asked of it is only
// ever "is this piece of walking geometry inside you", and a storey is a
// footprint between two heights.
struct Solid
{
  uint32_t id = 0;
  std::vector<Position> footprint; // Counter clockwise.
  double base = 0, top = 0;
  std::string note;
};

// A wall, or a rail: a line standing on a surface that you cannot walk through.
//
// One type for both, because the only question asked is whether it is in the
// way - and a rail across a doorway is as much in the way as a wall is. What
// they are made of is a later layer's business.
struct Barrier
{
  uint32_t id = 0;
  Position a, b;
  double base = 0, height = 1.0;
  bool wall = true; // False is a rail: waist high, and still impassable.

  // How thick it is. Carried rather than used: this layer only ever asks
  // whether something is in the way, and a line answers that. What extrudes it
  // wants one number and not a carpentry drawing - a quarter of a wall, one
  // rectangle, and whatever replaces it later can be as detailed as it likes.
  double thickness = .05;
  std::string note;
};

// A triangle of the model, for anything that has to draw or mesh it.
enum class FaceKind : uint8_t
{
  SurfaceFace,
  RampFace,
  BarrierFace,
  SolidFace,
  PortalFace
};

struct Face
{
  Position a, b, c;
  FaceKind kind = FaceKind::SurfaceFace;
  uint32_t of = 0; // The id of the thing it belongs to.
};

struct Model
{
  std::vector<Surface> surfaces;
  std::vector<Ramp> ramps;
  std::vector<Portal> portals;
  std::vector<Solid> solids;
  std::vector<Barrier> barriers;

  // How wide a person is, for every question about whether they fit.
  double clearWidth = 0.9;

  // How much headroom a surface has to leave over a ramp passing under it.
  double headroom = 2.0;
};

// What is wrong with a model.
//
// Typed rather than free text so a test can say which fault it expects, and so
// a deliberately broken model can prove that the check fires. A check nobody
// has ever seen fail is a check nobody has tested.
enum class FaultKind : uint8_t
{
  SurfaceDegenerate,   // Fewer than three corners, no area, or self crossing.
  SurfaceOverlaps,     // Two surfaces at one height standing on each other.
  RampFootOffSurface,  // The foot edge is not on the surface it claims.
  RampHeadOffSurface,  // ...nor the head.
  RampPierces,         // It passes through a surface that has no hole for it.
  RampTooSteep,        // Steeper than anyone can climb.
  PortalOffSurface,    // A door opening onto nothing.
  PortalPinched,       // A door whose floor is narrower than a person.
  PortalOnRamp,        // A door you step out of onto the ramp itself.
  SurfaceInSolid,      // A place to stand that is inside the building.
  RampInSolid,         // A ramp passing through the building's own mass.
  EntranceBlocked,     // A door or the end of a ramp with something across it.
  UnguardedEdge,       // An edge you would walk off, with nothing on it.
  NoWayThrough,        // Two things on one surface with no walkable path between.
  Unreachable          // A door no walk from the ground arrives at.
};

struct Fault
{
  FaultKind kind = FaultKind::SurfaceDegenerate;
  uint32_t a = 0, b = 0;
  Position where{};
  std::string what;
};

const char *nameOf(FaultKind);

// Everything that is wrong with the model, in one pass.
//
// Every answer comes from the polygons as they stand. Nothing here reads what
// a ramp *meant* to join or what a builder intended a surface to be - the
// connectivity is recomputed from the geometry, and a ramp's own `from` and
// `to` are treated as a claim to be checked rather than as information.
std::vector<Fault> validate(const Model &);

// Which surfaces can be walked between, as a graph over surface ids.
//
// Two surfaces are joined when they are at the same height and their polygons
// meet; a ramp joins the two it actually lands on. The walk that matters starts
// at every Ground surface.
std::vector<std::vector<uint32_t>> connections(const Model &);

// The surfaces a walk from the ground reaches.
std::vector<bool> reachable(const Model &);

// The walkable part of a surface: the surface eroded by half the clear width,
// which is exactly the set of points a person of that width can stand at.
//
// This is what makes "can you get from the stair to the door" answerable rather
// than approximable. A balcony with a stairwell cut out of it may still be
// walkable round the hole, or may not, and the difference is whether the eroded
// polygon stays in one piece - which is a question about polygons and has an
// exact answer.
bool walkableRegion(const Surface &, double clearWidth, std::vector<std::vector<Position>> &pieces);

// The model as triangles.
//
// Every piece of it is already a polygon, so this is a triangulation and not a
// mesher: a surface becomes a fan over its own outline with its holes carried,
// a ramp is its quad split in two, a barrier is a standing quad, a solid is its
// prism. Nothing is invented and nothing is smoothed - the point of the whole
// layer is that what you look at is what was checked.
bool triangulate(const Model &, std::vector<Face> &out, std::string *error = nullptr);

// A model written out for a renderer: surfaces, their holes, ramps, portals,
// the building's mass, the barriers, and the triangles of all of it.
std::string toJson(const Model &, const std::vector<Fault> &);

// Every place a person passes between a surface and something else: a door, and
// the two ends of every ramp.
//
// Named because the rule about them is one rule: an entrance is open. Nothing
// may stand across it - not a wall, not a rail, not the building - and the
// check is the same wherever the entrance came from.
// The short move you make going through it, not the opening itself.
//
// The opening was the first model - the doorway across the wall, the head edge
// of a ramp - and it cannot see what actually blocks a way in: a rail laid
// along the top of a ramp lies *on* that edge rather than across it, and a wall
// a stride in front of a door is parallel to the doorway and never touches it.
// What a barrier interrupts is the step you take, so the step is what is held
// here: out of the door onto the floor, off the top tread onto the surface.
struct Entrance
{
  Position a, b;   // From where you are to where you arrive.
  double height = 0;
  uint32_t of = 0; // The portal or ramp it belongs to.
  bool fromRamp = false;
};

std::vector<Entrance> entrances(const Model &);

// ---------------------------------------------------------------------------
// The one arrangement this is built to produce, for now.
// ---------------------------------------------------------------------------

// A building let by the floor, climbed from outside.
//
// One wall carries the whole thing: a balcony at every floor above the ground
// and a ramp between each pair, alternating direction so you arrive at one end
// of a balcony and leave from the other. The Z is not a style - it is what you
// get when the only ground available is one wall and each run has to be long
// enough to climb a storey.
struct StackBrief
{
  // The wall, as its two ends on the ground, with the building on the left.
  Position wallFrom, wallTo;

  uint32_t floors = 3;       // Storeys in the building, including the ground.
  double storeyHeight = 2.8;
  double ground = 0;

  // A ramp is W wide and a balcony is 2W deep, so the ramp takes the outer
  // half and the half against the wall is left to walk along.
  double rampWidth = 1.0;

  // How far a ramp may rise for each metre it runs. A stair is about 0.55;
  // above 0.9 nobody is climbing it.
  double maximumSlope = 0.62;

  double headroom = 2.0;
  double clearWidth = 0.9;
};

// Builds the model, and says why when it cannot.
bool planStack(const StackBrief &, Model &, std::string *error = nullptr);

// A stair winding up the inside of a tower.
//
// The other shape a stair takes, and a different structure rather than a taller
// version of the first: one flight to a side of the shaft, turning at every
// corner, with a landing at each turn and the well in the middle. Where the Z
// runs a wall's length and doubles back, this runs a square and keeps going.
struct TowerBrief
{
  Position centre;         // The middle of the shaft, on the ground.
  double side = 4.4;       // The inside of the shaft, wall to wall.
  uint32_t floors = 4;
  double storeyHeight = 2.8;
  double ground = 0;

  // The tread measured in from the wall. What is left in the middle is the
  // well, and it has to be left: a stair that fills the shaft has no well and
  // a shaft with no well has nowhere for the flights to wind around.
  double rampWidth = 1.1;

  double headroom = 2.0;
  double clearWidth = .8;

  // A door out of the shaft at every floor, on the landing.
  bool doorsAtLandings = true;
};

bool planTower(const TowerBrief &, Model &, std::string *error = nullptr);

} // namespace procedural::world::generator::access
