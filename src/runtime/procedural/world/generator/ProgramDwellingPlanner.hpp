#pragma once

#include <string>
#include <vector>

#include <map>

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/generator/ArchitecturalProgram.hpp"
#include "procedural/world/generator/ComplexDwellingPlanner.hpp"
#include "procedural/world/generator/FloorPlanOptimiser.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"

namespace procedural::world::generator {

// The second dwelling planner: one that starts from a programme.
//
// Stage D of .plan/bayesian-architectural-program.md. `ComplexDwellingPlanner`
// stays exactly as it is - it cuts a footprint into pieces and labels the
// pieces afterwards, which is a good generator and produces a *partition*.
// This one goes the other way: the rooms and their areas and the doors between
// them are decided first, by the network, and the geometry is asked to realise
// them.
//
// The difference shows in what can go wrong. A partition cannot fail to fill
// its footprint and cannot be asked for a room it did not happen to cut; a
// programme can ask for eleven rooms on a floor that only fits nine, and for a
// door between two rooms that end up on opposite sides of the plan. So this
// planner reports how well it did - area asked against area achieved, and the
// fraction of the programme's doors the geometry actually delivers - and that
// last number is the one to watch, because it is what distinguishes a plan
// from a partition.
//
// How it works, in one paragraph. Each floor is a polygon and each of its rooms
// is a site in a weighted power diagram over it: the weight is the room's
// demand, so a cell's area follows the area the programme asked for rather than
// the accident of where its neighbours are. The sites start where the adjacency
// graph wants them - linked rooms pull together, everything pushes apart, and
// circulation ends up in the middle because it is linked to the most - and the
// weights are then driven to the target areas by a few rounds of feedback.
// Nothing here is Merrell's §5 optimisation; it is the placement that the plan
// says to use until that exists, with the programme's own room list and areas
// driving it instead of a generic room-area target.
struct ProgrammePlanSettings
{
  // How far the outer wall stands inside the footprint, and how thick the
  // partitions between rooms are drawn.
  double wallThickness = .22;

  // Rounds of Lloyd relaxation before the area feedback starts. Relaxation
  // makes cells compact; without it a site cluster left by the spring layout
  // comes out as a row of slivers.
  uint32_t relaxIterations = 6;

  // Rounds of weight feedback. Each round measures every cell and moves its
  // weight toward the area the programme asked for.
  uint32_t areaIterations = 24;

  // How hard a round pushes. One is the whole correction at once, which
  // oscillates; a third converges in about twenty rounds without ringing.
  double areaGain = .35;

  // Rounds of the spring layout that seeds the sites from the adjacency graph.
  uint32_t layoutIterations = 400;

  // How much larger than its programme area a circulation room is drawn.
  //
  // A hall is small and has to touch everything, and a power diagram's cells
  // are compact, so a cell sized honestly at four square metres cannot reach
  // the six rooms that open off a landing.
  //
  // Inflating circulation buys the doors back, and it was set at 1.8 while
  // that was the only lever: 78% of doors at 13% area error. Giving
  // circulation a *spine* of sites instead - see the planner - buys the same
  // doors geometrically rather than by lying about room sizes, and this could
  // come back down: 1.15 gives 77% of doors at 4%. It is not 1.0 because the
  // last sliver of slack is still worth a point of delivery.
  double circulationBoost = 1.15;

  // A cell smaller than this is not a room. The programme's room is recorded
  // as dropped rather than drawn as a splinter.
  double minimumRoomMetres = 2.2;

  // Which way the street lies from the middle of the building, as a
  // direction. Rooms the programme marks `frontsStreet` are started against
  // that edge and held there while the springs pull everything else about.
  //
  // A shop is not a room that happens to face the street: facing the street is
  // what makes it a shop, and a counting house with its trading window on the
  // yard is a store room. So this is a hard pull rather than a preference.
  double streetX = 0, streetZ = -1;

  // How strongly. One holds a street room against the frontage whatever the
  // adjacency graph wants; zero lets the springs decide and the window ends up
  // wherever the plan is tidiest.
  double streetPull = .55;

  // The height of a storey, and how much of it the ground floor gets.
  //
  // A building is not a stack of identical slices. The ground floor of a
  // Georgian or Victorian house of any pretension is a foot or two taller than
  // the bedroom floor above it and half as tall again as the attic, because
  // the rooms that receive people are the tall ones - and the elevation, the
  // window heights and the whole proportion of the front follow from that.
  // Drawn at one height throughout, a mansion reads as an office block.
  //
  // These are multipliers on `storeyMetres`, applied by `ceilingMetres`, and
  // they scale with what the household could afford: a labourer's cottage has
  // the same height everywhere because it has one room per floor and no
  // pretension to keep up.
  // Share of the ground it is given that the building covers.
  //
  // Two thirds when the ground is a lot, because a house does not fill its
  // plot. One when the ground is already the house's own footprint - which is
  // the case when the plot planner has placed it, having applied the setbacks
  // and the standing's own share itself. Covering two thirds of *that* would
  // apply the share twice and put a cottage in the middle of a house-sized
  // hole.
  //
  // Lowered from just under two thirds, because what the rest of the lot is for
  // is not "not the house". It is where the household's work happens - the
  // yard the water and the washing and the privy are in, the croft behind it,
  // the pen and the midden - and at .62 those came out as ribbons a metre or
  // two across between the house and the boundary, which is not a yard. Half
  // the lot leaves a yard somebody can turn a cart in.
  double coverage = .52;

  double storeyMetres = 2.9;

  // How far each floor above the ground oversails the one below it, and how
  // deep an arcade stands in front of the ground floor. Nought for neither.
  //
  // These reached the composed massing and stopped there. The programme path
  // hard-coded both to zero - with a comment above the jetty saying precisely
  // what that costs, "without it every storey plate has the outline of the one
  // below and the building is a prism however good its plan" - so a town laid
  // out by the optimiser came out as four hundred rectangular boxes while the
  // path it replaced had jetties and fifty one arcades.
  double jettyMetres = 0;
  double arcadeDepthMetres = 0;
  double groundStoreyFactor = 1.18;
  double atticStoreyFactor = .84;
  double foundationMetres = .3;
};

// How tall the given storey is, in metres.
//
// `level` is the programme's own floor number, so -1 is a basement; `storeys`
// is how many stand above ground. The top floor of a building with three or
// more is an attic and is drawn lower; a basement is lower still, being mostly
// service. Everything between is the plain storey height.
double ceilingMetres(const ProgrammePlanSettings &, int32_t level, uint32_t storeys,
                     PlotStanding);

// ...and where its floor sits above the foundation, which is the heights of
// everything below it added up.
double floorElevation(const ProgrammePlanSettings &, int32_t level, uint32_t storeys,
                      PlotStanding);

// One room of the programme, placed.
struct PlacedRoom
{
  uint32_t programmeRoom = 0;  // Index into Programme::rooms.
  RoomUse use = RoomUse::Kitchen;
  int32_t floor = 0;

  geom2::Polygon boundary;

  double askedMetres = 0;     // What the programme wanted.
  double achievedMetres = 0;  // What the geometry gave it.
};

// A door the geometry actually delivered: the two placed rooms share a wall,
// and the programme asked them to be joined.
struct PlacedDoor
{
  uint32_t a = 0, b = 0;  // Indices into ProgrammeBuilding::rooms.
  bool door = true;       // False is an open wall.
  double x = 0, z = 0;    // Middle of the shared wall, metres.
};

// A programme the geometry could not join: both rooms are on the plan, the
// programme asked for a door, and their cells do not touch.
struct UndeliveredDoor
{
  uint32_t programmeA = 0, programmeB = 0;
  RoomUse useA = RoomUse::Kitchen, useB = RoomUse::Kitchen;
  int32_t floor = 0;
};

// The way in, placed.
struct PlacedEntrance
{
  uint32_t room = 0;             // Index into ProgrammeBuilding::rooms.
  double x = 0, z = 0;           // On the wall, metres.
  double normalX = 0, normalZ = 0;  // Which way it faces.
};

// A piece of the lot that is not the building: the yard, the croft behind it,
// the pen the stock is put in, the midden the byre is mucked out to, the
// garden of a house rich enough to have ground it does not work.
//
// These are rooms of the programme like any other - the network drew them,
// gave them areas, and linked them to the rooms whose doors open onto them -
// and they are the reason a cottage on a large plot is not a mistake. A
// building that fills a fifth of its lot and leaves the rest blank is the
// mistake; the rest of the lot is *where the household's work happens*.
struct PlacedGround
{
  RoomUse use = RoomUse::Yard;
  geom2::Polygon boundary;
  double askedMetres = 0, achievedMetres = 0;
};

// Who is in the building, and what they could afford to build.
//
// The plan is not the whole answer to "what is this building": a settlement
// wants to know how many people live here before it knows whether it has
// housed its population, and the furnisher wants to know what they could
// afford before it puts anything in a room. Both are decided by the programme
// - the household is why the rooms are there - so both are reported with it
// rather than guessed from the floor area afterwards.
struct Household
{
  // People who sleep here. Beds first: a bedroom is a bedroom because someone
  // sleeps in it. A house with none is not empty - it is a one-room cottage,
  // and the family sleeps in the room it lives in.
  uint32_t residents = 0;

  // ...and people who work here without living here: the shop, the workshop,
  // the counting house, the warehouse floor.
  uint32_t workers = 0;

  // Servants, apprentices and labourers included in `residents`. A wealthy
  // house is not a large family; it is a small family and a staff.
  uint32_t servants = 0;

  // Nought to one, from the standing the programme was drawn for. What a
  // furnisher, a material choice or a tax roll reads.
  double wealth = 0;

  // One line: "a comfortable shop house of 8 rooms on 2 storeys, 6 living
  // here and 2 working". For logs and for the gallery, not for parsing.
  std::string description;
};

struct ProgrammeBuilding
{
  Programme programme;

  // The ground the building stands on, counter clockwise, metres.
  std::vector<std::pair<double, double>> footprint;

  std::vector<PlacedRoom> rooms;
  std::vector<PlacedDoor> doors;
  std::vector<PlacedEntrance> entrances;
  std::vector<PlacedGround> grounds;
  std::vector<UndeliveredDoor> undelivered;

  Household household;

  // --- how well it did ---

  // Mean of |achieved - asked| / asked over the rooms that were placed. Zero
  // is a perfect realisation; a fifth is a plan you would still recognise.
  double areaError = 0;

  // Share of the programme's doors the geometry delivered, counting only the
  // ones whose rooms were both placed. This is the number that says whether
  // the result is a plan or a partition.
  double doorsDelivered = 0;

  uint32_t roomsPlaced = 0, roomsDropped = 0;

  // What §5 actually cost, when the interior came from the optimiser. Read
  // `dimensions` against `sampledDimensionCost`: a plan whose dimension cost
  // is at or below the one the programme's own sampled areas would score is
  // at least as likely as the programme it came from, and that - not the
  // area error - is what §5.2 is asking for.
  LayoutCost layoutCost, startCost;

  // What the search actually did: how many moves it offered and how many it
  // took. Carried out of the optimiser because a cost alone cannot tell a plan
  // that was optimised from one that never moved.
  //
  // Two of the class gallery's eighteen buildings ran thirty thousand
  // iterations and accepted nothing - their canonical start was already
  // illegal, so every candidate inherited the violation and was refused - and
  // what got measured, rendered and shipped was the start. Nothing said so,
  // because nothing was looking: the cost went in at 372.3 and came out at
  // 372.3 and no check reads the cost.
  uint32_t proposed = 0, accepted = 0;
  double sampledDimensionCost = 0;
  bool optimised = false;

  // Rooms the programme did not place because the floor they belong to has no
  // ground under it - a basement on a plot that was never dug.
  uint32_t floorsPlanned = 0;
};

// Samples a programme for the brief and realises it on `footprint`.
//
// An empty footprint means "use the programme's own", which is the case the
// plan is really about: the network owns a Footprint variable, so the massing
// can take its outline from the programme rather than from a coverage fraction
// of a lot. Pass one to impose the ground instead, and the brief's
// footprintX/Z should be set to match so the programme is sampled to fit it.
// The ground each floor stands on, when the caller has massed the building
// already. Keyed by the programme's floor level, so -1 is a basement.
//
// A floor with no entry falls back to the footprint, shrunk to what that floor
// asks for. A floor with one uses it exactly, which is how a storey of a
// composed building - a set of rectangular wings, some of them stopping short
// of the top - becomes the region its rooms are partitioned in.
using ProgrammeFloorRegions = std::map<int32_t, geom2::Polygon>;

bool planProgrammeBuilding(const ProgrammeBrief &,
                           uint64_t seed,
                           const ProgrammePlanSettings &,
                           const std::vector<std::pair<double, double>> &footprint,
                           ProgrammeBuilding &out,
                           std::string *error = nullptr,
                           const ProgrammeFloorRegions *regions = nullptr);

// The programme massed and realised as a building the rest of the engine
// understands: wings, storeys, roof faces, walls, doors and stairs.
//
// This is the piece that plugs the programme generator into everything already
// built. `massComposedBuilding` lays down rectangular wings and pushes further
// wings off the sides of those standing, which is Watabou's rule for a mansion
// - "a wing is a rectangular block with its own roof", grouped into clusters
// whose roofs merge - and it is already written and tested here. So the
// exterior is massed first, the programme's rooms are partitioned inside each
// storey of it, and the roof comes back with the massing rather than being
// invented afterwards.
//
// `plot` is the ground the building may stand on. Empty means "make one", at
// the size the programme's own rooms ask for.
//
// The interior comes from §5's optimiser when `optimise` is given, and from the
// power diagram otherwise. They are not the same thing and the difference is
// not a tuning knob: the optimiser produces rectilinear rooms against a cost
// function and takes a second or two; the power diagram produces cells at any
// angle in a millisecond and cannot fail. A town wanting a hundred houses in a
// frame wants the second; a building the camera will stand inside wants the
// first.
bool planProgrammeDwelling(const ProgrammeBrief &,
                           uint64_t seed,
                           const ProgrammePlanSettings &,
                           const ComplexDwellingSettings &,
                           const std::vector<rewrite::Position> &plot,
                           ProgrammeBuilding &out,
                           rewrite::DwellingPlan &plan,
                           std::string *error = nullptr,
                           const FloorPlanSettings *optimise = nullptr);


// The interior of a house the plot planner has already placed.
//
// `PlotPlanner` decides how a lot's ground is used - what share the house
// takes, where the yard and the garden and the fence go, which edge is the
// street - and composes the house's exterior. Its own comment says the
// interior "is the next layer's problem". This is that layer: the footprint it
// chose is taken as given and the programme is planned inside it, exterior and
// all, so the walls that get built are the walls of the rooms.
//
// `house` is that footprint, counter clockwise, metres. Everything else is as
// `planProgrammeDwelling`, which this calls with the coverage set to one.
bool planProgrammeInside(const std::vector<rewrite::Position> &house,
                         const ProgrammeBrief &,
                         uint64_t seed,
                         const ProgrammePlanSettings &,
                         const ComplexDwellingSettings &,
                         ProgrammeBuilding &,
                         rewrite::DwellingPlan &,
                         std::string *error = nullptr,
                         const FloorPlanSettings *optimise = nullptr);

}  // namespace procedural::world::generator
