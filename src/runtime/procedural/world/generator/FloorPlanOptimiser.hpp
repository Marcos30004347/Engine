#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "procedural/world/generator/ArchitecturalProgram.hpp"

namespace procedural::world::generator {

// Merrell, Schkufza and Koltun, §5: turning an architectural programme into a
// floor plan by optimising over the space of building layouts.
//
// This is the paper's algorithm and not a placement heuristic. The difference
// is the representation: a room here is a *rectilinear polygon*, which is what
// lets a wall slide, split and snap, and what makes the result read as a plan
// rather than as a diagram. The power-diagram placer in ProgramDwellingPlanner
// cannot express a rectilinear room at all, which is why it could never get
// there by tuning.
//
//   "A floor plan x is defined as a set of n rooms. Each room is a rectilinear
//    polygon enclosed by wall segments. A maximal contiguous set of collinear
//    wall segments is said to form a wall."
//
// The state is carried as a grid of cells with a room label each, on a grid
// whose lines are the wall coordinates and which is refined as walls split.
// Every room is then a union of cells and so is rectilinear by construction,
// the partition is exact by construction, and a wall slide is the insertion of
// one grid line plus a relabelling of the cells the wall swept over. Rooms as
// explicit polygons would be closer to the paper's words and would need a
// polygon repair after every move; this is the same thing with the bookkeeping
// done once.
//
// The search is the paper's: propose, accept by Metropolis, anneal.
//
//   f(x) = exp(-B C(x)),  a(x*|x) = min(1, exp(B (C(x) - C(x*))))
//
// starting "from a canonical high-cost configuration, in which equally-sized
// rectangular rooms are packed on a grid on each floor".

// The four terms of C(x), reported separately because the paper's Figure 8 is
// an ablation of exactly these and because a single number cannot say which
// part of a layout is wrong.
struct LayoutCost
{
  double accessibility = 0;  // Ca: missing connections, and anything sealed in.
  double dimensions = 0;     // Cd: -log-likelihood of each area and aspect.
  double floors = 0;         // Cf: share of floor area standing over nothing.
  double shape = 0;          // Cs: convexity defect, room groups, outline.

  double total = 0;

  // Floor area standing off the ground the building was given, in square
  // metres. Not part of the total, because it is not a cost: it is a hard
  // constraint, and a move that increases it is refused however good the rest
  // of the layout it buys.
  //
  // The paper can leave this to a cost term because its building outline is
  // fixed by the user and its rooms only ever partition it. Ours decides its
  // own outline, and a town lot is bounded by the street on one side and the
  // neighbour's wall on the other: a plan that pays a fine and stands there
  // anyway is still standing in the street.
  double outsideGround = 0;
};

struct FloorPlanSettings
{
  // The weights of the four terms. The paper gives them names and not values,
  // so these are ours: chosen so that one missing door costs about as much as
  // one room at the wrong end of its area distribution, which is the balance
  // Figure 8 implies.
  double accessibilityWeight = 12.;
  double dimensionWeight = 1.;
  double floorWeight = 90.;
  double shapeWeight = 1.;

  // Inside the shape term: individual rooms, open-walled groups, and the
  // irregularity of each floor's outline. The paper's kr, kg and ko.
  double roomShapeWeight = .6;
  double groupShapeWeight = .4;
  double outlineWeight = .25;

  // How hard the plan is held to the total floor area the programme drew.
  //
  // A departure from the paper, and one its data would not have needed. Cd
  // scores each room against its own distribution, and distributions fitted
  // to a few dozen designs are broad: a parlour is about as likely at six
  // square metres as at twelve, so nothing in the four terms objects when the
  // whole house comes out at half the size it asked for. The network draws a
  // total area as well, and holding the plan to it is not a new idea imported
  // from outside - it is one of the programme's own variables, scored the way
  // the rooms are.
  //
  // As the squared log ratio, so halving and doubling cost the same, and so
  // the programme's own areas - which sum to the total by construction - pay
  // nothing. Six puts a doubling at about the price of a missing door.
  double totalAreaWeight = 25.;

  // "We empirically set eps = 1 ft. and sigma = 2 eps."
  double snapMetres = 0.3048;
  double slideSigmaMetres = 0.6096;

  // The narrowest wall a door will go in. The paper: two rooms must "share a
  // sufficiently long wall segment to accommodate a door".
  double doorWidthMetres = .85;

  // No room may be squeezed below this, whatever the cost says.
  double minimumRoomMetres = 1.6;

  // ...nor be narrower than this anywhere along it.
  //
  // Area is not width. A passage of four square metres can be six metres long
  // and two thirds of a metre wide, which is not a passage - it is a gap
  // between two walls that nobody can walk down, and the aspect term will not
  // catch it because the network's own passages are long and thin too.
  //
  // Measured as twice the area over the perimeter, which is the *harmonic*
  // width and not the narrowest: a one-metre passage four metres long scores
  // 0.8 by it, and a square room scores half its side. So the number here is
  // below the width it is enforcing - .72 refuses anything under about eight
  // hundred millimetres, which is a door's width and the point below which a
  // passage stops being one.
  double minimumWidthMetres = .72;

  // ...and a passage has to be wider than that.
  //
  // The rule above is the width below which a *room* is not a room. A corridor
  // is different: it is only ever the width it is, and two people passing in a
  // mansion's first-floor passage need more than a doorway between them. Eight
  // tenths by the harmonic measure is about a metre across, which is what the
  // book's own plans give a principal passage.
  double minimumPassageMetres = .82;

  // ...and a room somebody lives, works or worships in has to be wider again.
  //
  // The two rules above are floors below which a thing stops being what it is
  // called: eight hundred millimetres is a doorway, a metre is a corridor. They
  // say nothing about whether a room is any good, and the optimiser will
  // cheerfully hand a nave or a bedroom a two-metre ribbon down the side of the
  // plan if that is where the area fits - which is what a church came out as,
  // a long thin strip beside a long thin strip.
  //
  // By the harmonic measure 1.25 refuses anything under about two and a half
  // metres across for a long room, and about two and a half metres square for a
  // square one. That is the width below which furniture and a person cannot
  // both be in the room. Service rooms - a larder, a privy, a closet - are held
  // to the general floor instead, because small is what they are.
  double minimumHabitableMetres = 1.0;

  // The paper illustrates a hundred thousand iterations, taking thirty-five
  // seconds on a 2010 i7. Ours takes about two and a half seconds for the
  // same count, because the state is the arrangement of the walls rather than
  // a polygon set - a hundred and twenty cells to measure, not a thousand.
  uint32_t iterations = 100000;

  // Beta, annealed. "Although we have found the algorithm to perform well with
  // a set value of beta, annealing beta over time facilitates a more rapid
  // exploration of the space."
  double betaStart = .35;
  double betaEnd = 9.;

  // How often the move is a room swap rather than a wall slide. The swap is
  // the global move: "this generally results in a drastic change in the
  // objective function value".
  double swapChance = .06;

  // ...and how often the move is "put this floor's stair over the one below".
  // Only ever proposed on a floor that has one and whose neighbour below has
  // one, so on a single-storey plan it costs nothing.
  double stackChance = .04;

  // Which way the street lies, as a direction. Zero means the plan may sit
  // anywhere on its ground.
  //
  // A house is not put down in the middle of its plot with a margin all round;
  // it stands on the street, and what is left over is behind it. Nothing in
  // the four cost terms says so - the paper's building outline is given, so
  // the question never arises - and a plan free to choose its own outline
  // will centre itself, which reads as a villa in its grounds however good the
  // rooms are. So the start is laid against the frontage.
  double streetX = 0, streetZ = 0;

  // Cells narrower than this are not worth a grid line; it is the grid's own
  // resolution and it bounds how fine a split can be.
  double gridEpsilonMetres = .02;
};

// One room, placed.
struct LayoutRoom
{
  uint32_t programmeRoom = 0;
  RoomUse use = RoomUse::Kitchen;
  int32_t floor = 0;

  // The rectilinear outline, counter clockwise, metres. Holes are not produced
  // - a room that would enclose another is a cost, not a shape.
  std::vector<std::pair<double, double>> outline;

  double areaMetres = 0;
  double aspect = 1;
};

// What kind of opening joins two rooms.
//
// §6: "Different types of passageways are used between different types of
// rooms. Ordinary doors are placed in private rooms such as bedrooms. French
// doors with windows are placed in semi-private rooms such as studies or
// patios. Wide passageways are placed between public rooms."
//
// The network already says whether the programme wanted a door or an open
// wall; this says what the opening *is*, which depends on what the rooms are
// for and not on what the programme asked.
enum class PassageType : uint8_t
{
  Door,        // An ordinary door: anything touching a private room.
  FrenchDoor,  // Glazed, between semi-private rooms.
  WidePassage, // No door at all, between public rooms.
};

const char *name(PassageType);

struct LayoutDoor
{
  uint32_t a = 0, b = 0;  // Indices into FloorPlan::rooms.
  bool door = true;       // What the programme asked for; false is an open wall.
  PassageType passage = PassageType::Door;
  double x = 0, z = 0;
  double wallLengthMetres = 0;
};

// The way in.
//
// §6 places the front door by turning one of the entrance room's windows into
// a door: "the entrance to the building is placed in the room designated as
// the entrance". A building without one is a set of rooms, and everything
// downstream that asks how a person gets in - the town's paths, the
// occupants, the camera - has no answer.
struct LayoutEntrance
{
  uint32_t room = 0;  // Index into FloorPlan::rooms.
  double x = 0, z = 0;

  // The outward normal of the wall it sits in, so a step, a porch or a path
  // knows which way to face.
  double normalX = 0, normalZ = 0;
  double wallLengthMetres = 0;
};

struct FloorPlan
{
  Programme programme;

  std::vector<LayoutRoom> rooms;
  std::vector<LayoutDoor> doors;
  std::vector<LayoutEntrance> entrances;

  // The outline of each floor, by level, counter clockwise.
  std::vector<std::pair<int32_t, std::vector<std::pair<double, double>>>> outlines;

  // ...and the courts inside them: the inner loops of a floor that wraps round
  // open ground. A guild hall and a temple both do, and a storey plate without
  // them has no wall around its court and a roof straight across it.
  std::vector<std::pair<int32_t, std::vector<std::pair<double, double>>>> courts;

  LayoutCost cost;
  LayoutCost startCost;

  uint32_t proposed = 0, accepted = 0;

  // How many distinct wall coordinates the layout ended up carrying, summed
  // over floors. The paper's state is a set of rectilinear polygons and this
  // is the size of the arrangement encoding them, so it should be close to the
  // number of walls actually in the plan - not a fine mesh.
  uint32_t gridLines = 0, gridCells = 0;

  // What the optimisation was asked for and what it delivered, in the terms
  // the plan's acceptance is stated in.
  uint32_t doorsAsked = 0, doorsDelivered = 0;

  // Mean |achieved - sampled| / sampled. Reported, but read the next field
  // first: the paper is explicit that the sampled value is *not* the target,
  // and a draw from a distribution is not near its own mode, so this number
  // overstates the error by however wide the distribution is.
  double areaError = 0;

  // The dimension cost of the programme's own sampled areas and aspects,
  // against the same distributions. This is the number that matters: a layout
  // whose Cd is at or below it is at least as likely as the programme it came
  // from, which is what §5.2 is asking for.
  double sampledDimensionCost = 0;
};

// Optimises a floor plan for `programme` on the ground it is given.
//
// `lots` is the ground each floor may stand on, by level, counter clockwise
// metres - the paper's Fx0 for the bottom floor, and for an upper floor the
// storey plate of a building that has already been massed. They are polygons
// and not boxes: a lot allocated by the town is a wedge, a trapezium or a
// burgage strip far more often than it is a rectangle, and a plan fitted to
// the bounding box of a wedge stands in the street.
//
// A level with no lot still has to stand over the floor below it. Only the
// bottom floor with no lot is free, which is the case where the building's
// own outline is the thing being decided.
//
// `site` is ground every floor must stand within, whatever its own lot says -
// the plot the town allocated. It is a second constraint and not a substitute:
// a massed plate can overhang its plot by the thickness of an eave, and a plan
// fitted to the plate inherits the overhang. Empty means the lots are the only
// word on where the building may stand.
bool optimiseFloorPlan(const Programme &,
                       const std::map<int32_t, std::vector<std::pair<double, double>>> &lots,
                       uint64_t seed,
                       const FloorPlanSettings &,
                       FloorPlan &out,
                       std::string *error = nullptr,
                       const std::vector<std::pair<double, double>> &site = {});

// The common case: one piece of ground, under the bottom floor. An empty lot
// means the plan may take whatever ground it wants.
bool optimiseFloorPlan(const Programme &,
                       const std::vector<std::pair<double, double>> &lot,
                       uint64_t seed,
                       const FloorPlanSettings &,
                       FloorPlan &out,
                       std::string *error = nullptr);

}  // namespace procedural::world::generator
