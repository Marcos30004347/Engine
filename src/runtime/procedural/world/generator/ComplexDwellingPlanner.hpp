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

// How the roof is framed, which is a different question from what shape it is.
//
// `RoofStyle` says where the roof falls - hipped, gabled, cross-gabled - and
// two roofs of the same shape can be carried in quite different ways. These
// are the four that an English building of this age is actually built with,
// and they are a sequence: each one buys more open floor under it than the
// last, and costs more to build.
enum class RoofFrameStyle : uint8_t
{
  // Every pair of rafters is its own truss, collared, with no principals and
  // no purlins under them. The earliest and the plainest: a roof of identical
  // sticks, which is what a barn or a plain range has.
  TrussedRafter,

  // A heavy tie across the wall plates at every truss, principals standing on
  // its ends. The tie does the work and nothing has to be braced, so it is the
  // cheapest roof that spans a wide building - and the tie is in the way of
  // the room under it, which is why the others exist.
  TieBeam,

  // Principals and a collar, with arched braces up to the collar from partway
  // down each principal. The tie is gone; the arch under the collar is what
  // stops the pair spreading, and the space under the collar is open.
  CollarBraced,

  // Hammer beams: short cantilevers off the wall head carrying a post up to
  // the principal, braced from below. No member crosses the room at all, which
  // is the whole point of it and the reason it is the roof of a hall.
  HammerBeam
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

  // How thick an inside wall is.
  //
  // The subdivision cuts a plate into rooms, so two rooms meet along a line and
  // the wall between them has no thickness at all: the plan says the plaster of
  // one room is the plaster of the next. Every consumer then has to invent the
  // wall - the mesher grows one either side of the line, the render draws a
  // stroke - and none of them agree, so a room's drawn area is not its floor
  // area and a door in a wall with no depth has no reveal to sit in.
  //
  // So the rooms are inset by half of this and the wall stands in the gap. Thin
  // - a stud partition, not masonry - because an inside wall of this age is
  // studs and lath, and because every centimetre of it comes out of a room.
  double interiorWallThickness = .12;

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

  // How much ground a storey needs before it is worth building.
  //
  // Width alone was the test, and width alone cannot answer it: a plan can be
  // wide enough everywhere and still be a sliver, so a building on a scrap of
  // ground was given every storey the brief asked for and each came out as two
  // rooms over two rooms over two rooms. How many storeys a building has is a
  // question about how much floor there is to divide, and the answer to that is
  // an area.
  //
  // The rooms already follow the ground - the subdivision cuts whatever it is
  // given into pieces of `roomAreaMetres` - so this is the same rule applied
  // one level up: a storey is worth having when it holds a room or two and the
  // circulation to reach them.
  double minimumStoreyAreaMetres = 24;

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
  // Fatter than they were - five and a half to ten metres - because a wing
  // that thin makes a building out of corridors: the ground floor fills its own
  // bounding box well enough on paper and the mass reads as a set of sheds
  // pushed together. A range wide enough for two rooms and something between
  // them is the shape a building of this size actually is.
  double minimumWingDepthMetres = 7.0;
  double maximumWingDepthMetres = 12.0;

  // How far the ridge rises per metre across the mass under it.
  //
  // A third is a farm building, and it is what every roof here used to be.
  // It is the wrong answer for most of a town: a tiled or thatched medieval
  // roof stands between forty five and sixty degrees because that is the
  // pitch that sheds, and a hall or a church carries its whole height in the
  // roof rather than in another storey. The pitch is therefore a decision
  // about the building, taken per mass, and not a constant of the geometry.
  double roofPitch = .34;

  // ---- halls -------------------------------------------------------------
  //
  // A hall is not a large room. It is a room the building is arranged around,
  // one bay deep across the whole width of its range, open from its floor to
  // the roof over it - and the subdivision that cuts a plate into rooms of
  // sixteen square metres can never produce one, because the first thing it
  // does to a large piece is halve it. So the hall is cut out of the plate
  // before the subdivision sees it, and the rooms are what is left.

  // Share of the ground plate the hall takes, and the floor it may not go
  // under: a hall smaller than this is just a big room.
  double hallAreaShare = .45;
  double hallMinimumAreaMetres = 30;

  // ...and it has to be a hall in both directions. A band four metres deep
  // and twenty long is a barn, not a hall.
  double hallMinimumWidthMetres = 4.5;

  // How many storey heights a hall stands open through. Two is a hall with a
  // floor's worth of nothing over it; one is a tall single storey.
  uint32_t hallHeightFloors = 2;

  // ...and the roof over one. Steeper than the rest of the town, which is
  // what makes a hall read as a hall from outside it.
  double hallRoofPitch = .62;

  // ---- the timber frame ---------------------------------------------------
  //
  // What carries a floor, and the ceiling of the storey under it. Sizes in
  // metres, across the member and through it.
  //
  // Two sections and not one, because the ratio between them is the structure:
  // a frame of this age is a few large members carrying many small ones, and a
  // floor drawn as one size of stick at one spacing reads as a grid.

  // The spine: the big member, running the *length* of the building.
  double beamWidthMetres = .20, beamDepthMetres = .26;

  // ...and what puts one there: the longest a joist may span unaided. A house
  // narrow enough for its joists to reach wall to wall needs no spine at all,
  // which is why most burgage plots have none, and a wider one needs one line
  // of spine for every span over this.
  double maximumJoistSpanMetres = 4.2;

  // The small ones over them, carrying the ceiling.
  double joistWidthMetres = .07, joistDepthMetres = .13;
  double joistSpacingMetres = .45;

  // The plate along the top of a wall, which the floor bears on.
  double plateWidthMetres = .16, plateDepthMetres = .16;

  // How thick the boards are.
  //
  // Small, and it matters: boards lie *on* the joists. Laid at the same height
  // the joists reach, the two surfaces are coplanar - the floor reads as a lid
  // flush with its own structure, and the render fights over which of them owns
  // each pixel.
  double floorBoardMetres = .045;

  // A post under a beam end that lands on no wall.
  double postSideMetres = .17;

  // ---- the roof frame -----------------------------------------------------
  //
  // What holds the roof surfaces up, which is a different structure from the
  // floor and not a copy of it: a pair of rafters leaning against each other
  // over a ridge, a tie across their feet to stop them spreading, and a purlin
  // partway up the slope so a rafter does not have to make the whole run.

  double rafterWidthMetres = .09, rafterDepthMetres = .14;
  double rafterSpacingMetres = .65;

  // The ridge the rafters meet over, and the purlin partway up the slope.
  double ridgeWidthMetres = .17, ridgeDepthMetres = .20;
  double purlinWidthMetres = .13, purlinDepthMetres = .16;

  // The tie across the feet of a pair of rafters, and how far apart ties go.
  double tieWidthMetres = .16, tieDepthMetres = .20;
  double tieSpacingMetres = 2.6;

  // ---- trusses -------------------------------------------------------------
  //
  // What every drawing of a medieval timber roof has and a grid of identical
  // rafters does not: a few heavy frames carrying many light ones.
  //
  // A truss is a *frame* - a pair of principal rafters, a tie or a collar
  // across them, and braces making the whole a triangle - standing every few
  // metres and carrying the purlins. The common rafters lie on those purlins
  // and carry only the covering. It is the same relation the floor already has
  // between its spine and its joists, and it is most of what makes a roof read
  // as built rather than as extruded.

  // How far apart the trusses stand. A bay.
  double trussSpacingMetres = 3.6;

  // The principal rafters of a truss: heavier than a common rafter, because
  // they carry the purlins and the commons carry nothing but the roof.
  double principalWidthMetres = .16, principalDepthMetres = .22;

  // The collar: the horizontal across a pair of principals, partway up.
  double collarWidthMetres = .13, collarDepthMetres = .18;

  // ...and where up the slope it sits, as a share of the climb from eaves to
  // ridge. Low is a tie, high is a collar; two thirds is what most of these
  // roofs do.
  double collarHeightShare = .62;

  // The braces from the tie up to the principals, which are what stop the
  // truss folding.
  double trussBraceWidthMetres = .11, trussBraceDepthMetres = .14;
  double trussBraceReachMetres = 1.1;

  // ---- the chimney ---------------------------------------------------------
  //
  // A stack of masonry standing on a hearth and passing up through the roof.
  //
  // The hearth is already in the plan - it is furniture, placed in the room a
  // household lives in - so the chimney has somewhere to be without anything
  // new having to decide it. What it needs from the roof is what a tower needs:
  // a hole, because a stack through a roof is a stack through a roof.
  double chimneySideMetres = .95;

  // How far it stands above the ridge it passes. Too little and it is a lump on
  // the roof; this is what makes it read from the street.
  double chimneyAboveRidgeMetres = .9;

  // ---- the wall frame -----------------------------------------------------
  //
  // A post where two walls meet, and the studs and rails of the outside wall.
  //
  // The junction post is the piece the whole frame is organised around: two
  // walls meeting is where the load goes, and a building of this age puts a
  // post there and frames everything else into it. It is also the one member
  // whose position is not a choice - the plan already says where it goes.

  double junctionPostSideMetres = .19;

  // ...and the knee brace from it up to the plate, which is what stops the
  // frame racking. Without the diagonals a frame is a grid of rectangles and
  // a grid of rectangles folds flat.
  double braceWidthMetres = .09, braceDepthMetres = .11;
  double braceReachMetres = .85;

  // The studs of the outside wall, and how far apart they stand.
  //
  // Wide. Close studding - a stud every hand's breadth - is a real thing and a
  // late and expensive one, and at this scale it reads as hatching rather than
  // as structure: the wall disappears behind its own frame. A bay to a window
  // is what most of a town is, and it leaves panels big enough to be panels.
  double studWidthMetres = .11, studDepthMetres = .13;
  double studSpacingMetres = 1.8;

  // ...and the rail they are framed between, halfway up the storey.
  double railWidthMetres = .13, railDepthMetres = .14;

  // ---- towers ------------------------------------------------------------
  //
  // A square shaft of plan that rises past the rest of the building. Its
  // ground is reserved on every floor it passes through, so no floor cuts a
  // room across it, and it carries the stair - which is what a tower in a
  // building of this age is for.

  // Four point two, and a wider shaft was tried and put back.
  //
  // A tower here stands 4.1 times its own width on average and the worst is
  // 5.4, which is slender for a tower - and widening the shaft to five and a
  // fifth does fix that number (3.5, worst 4.7) at the cost of a worse one:
  // the newel stair's landings are the *flight's* width, so a wider shaft
  // leaves the same small square in a bigger corner and four of the corpus's
  // tower doors stop reaching one. Stoutness has to come from the stair
  // getting wider with the shaft, which is a change to how a newel stair is
  // sized rather than to this number.
  double towerSideMetres = 4.2;

  // How many storeys it stands above the rest of the building.
  //
  // Two, and it was tried at one.
  //
  // One makes a stouter tower and stops it being a tower: the massing raises a
  // shaft until its eaves clear the ridges around it, and with only one storey
  // to give it could not - a hall's spire came out inside the roof below it and
  // the newel stair lost the landing its door opens onto. The slenderness that
  // needed fixing is fixed by the width instead, which is the other term in the
  // same ratio and the one that was actually wrong.
  uint32_t towerExtraStoreys = 2;

  // A spire is steeper than a roof.
  double towerRoofPitch = 1.15;

  // ...or no spire at all: a tower open at the top, with a rail round the
  // walk.
  //
  // The other thing a tower of this age is. A spire is a tower you look at; an
  // open top is a tower you stand on, and the difference is visible from a
  // mile off - a silhouette that ends in a point against one that ends in a
  // line. Which of the two a building gets is a decision about the building,
  // so it is a knob and not a roll: a lookout is open, a belfry is not.
  bool towerOpenTop = false;

  double parapetHeightMetres = 1.05;
  double parapetPostSideMetres = .15;
  double parapetRailWidthMetres = .11, parapetRailDepthMetres = .09;
  double parapetPostSpacingMetres = 1.3;

  // ---- the entrance bay ---------------------------------------------------
  //
  // The recessed front of a town house: the ground floor stops short of the
  // street, the storey above it does not, and what holds the overhang up is a
  // row of posts. You walk in under the building before you walk into it.
  //
  // Like the hall, it is reserved out of the ground plate before the
  // subdivision sees it - and for a sharper reason. The bay is where the
  // building's posts stand, and posts inside a room are a fault, so the room
  // has to exist before anything is cut. It is `RoomUse::Entrance`, its street
  // walls are `WallKind::Open`, and the mass loses that ground at level zero
  // only.

  // How far back from the front the bay goes, and how much of the front it
  // takes.
  double entranceDepthMetres = 2.3;
  double entranceWidthShare = .62;

  // Below this it is a doorway with a lintel, not a bay you stand in.
  double entranceMinimumWidthMetres = 2.4;

  // ...and it may not eat the ground floor: a bay bigger than this share of
  // the plate is a building with no ground floor left.
  double entranceMaximumAreaShare = .3;

  // How far apart the posts along the open front stand, and how thick they
  // are. Two and a half metres is a timber bay; closer reads as a colonnade.
  double entrancePostSpacingMetres = 2.5;
  double entrancePostSideMetres = .22;

  // ---- doors and windows as holes in the wall -----------------------------
  //
  // The planner emits the void, not the joinery: a rectangle cut clean through
  // the wall for a door or window mesh to be set into. It is cut larger than
  // the opening it serves, because the mesh that fills it carries its own
  // frame and a reveal you were given beats a reveal you have to model around.

  // How much wider than the joinery the void is cut, at each end.
  double openingMarginMetres = .12;

  // ...and how much wall is left over the top of it.
  //
  // Not zero. A wall cut to its full height is not a wall with a hole in it,
  // it is two walls - nothing ties the piers together and nothing carries the
  // floor above. A hand's breadth of lintel is the smallest amount that reads
  // as a building.
  double openingHeadroomMetres = .25;

  // A doorway: the joinery reaches the floor, so there is no threshold.
  //
  // The cut stops just over the head of it. A doorway taken to the full height
  // of the wall is not a doorway, it is a gap in the building with a door
  // standing in the bottom of it - and the whole reason to cut generously is
  // to give a *door mesh* room, which a door mesh does not need above its own
  // frame.
  double doorWidthMetres = 1.0;
  double doorHeadMetres = 2.1;

  // A window.
  //
  // Wide, and that is the correction rather than a preference: a window is a
  // rectangle wider than it is tall in almost every building of this age -
  // two or three lights side by side under one head - and a tall narrow slot
  // is an arrow loop. Cut narrow, every wall came out pricked with what looked
  // like defects rather than openings.
  // One size, for every window in every building.
  //
  // Not a range and not a share of the wall. A window is a manufactured thing:
  // a joiner makes one size and the building takes as many of them as it has
  // wall for, which is why a street of these has one window repeated a hundred
  // times rather than a hundred windows. It is also what makes a single window
  // *mesh* usable - every opening is the same rectangle, so the same asset fits
  // all of them, and an opening that varies is an opening nothing can fill.
  // One metre by one metre, and square on purpose.
  //
  // A window is a manufactured thing and the opening is a hole a window goes
  // into, so what the planner owes the mesh that fills it is a standard
  // rectangle at a standard size - the same rectangle every time, on both
  // faces of the wall, so one asset fits all of them. An opening that varies
  // is an opening nothing can fill.
  double windowWidthMetres = 1.0;
  double windowHeightMetres = 1.0;
  double windowSillMetres = .95;

  // How much of a wall's run may be window, and the least wall between two.
  //
  // A share rather than a spacing, because a spacing counts openings and what
  // a facade is judged on is how much of it is glass: the same three metre gap
  // reads as regular on a long range and as a perforated screen on a cottage.
  double windowWallShare = .34;
  double windowPierMetres = 1.6;

  // A wall this near a neighbour gets no windows.
  //
  // A window is a hole you can see out of, and one a stride from the next
  // building's flank is a hole you can see a wall through. Towns are built
  // along party walls, so on a terrace most of a building's outside is not
  // outside at all - which is the difference between a house in a street and
  // a house in a field, and it cannot be decided from the building alone.
  // `DwellingBrief::neighbours` is what supplies the answer.
  double windowNeighbourMetres = 2.2;

  // ...and a merchant's display opening, which is lower and wider than a
  // window because it is a counter you are meant to reach over.
  double shopWindowWidthMetres = 1.9;
  double shopWindowHeightMetres = 1.35;
  double shopWindowSillMetres = .9;

  // ---- the balcony ---------------------------------------------------------
  //
  // A floor that stands outside the walls, on posts, with a rail round it and a
  // door onto it. The mirror of the entrance bay: the bay is ground the ground
  // storey gives up with the building kept over it, and a balcony is floor an
  // upper storey gains with nothing under it but posts.
  //
  // It is not part of any storey plate, and deliberately: a plate is where the
  // walls are, and the whole of what makes a balcony a balcony is that there
  // are none. So it is built after the massing, out of the same pieces the bay
  // already uses - pillars, decking, a rail - and the massing never sees it.

  // How far it stands out from the wall. Zero is no balcony.
  double balconyDepthMetres = 0;

  // ...and how much of the wall it runs along.
  double balconyShare = .55;

  // A roof over it, or open to the sky. Both are real and the difference is
  // large: a covered gallery is a room with one wall missing, and an open deck
  // is a place you stand in the weather.
  bool balconyRoofed = false;

  double balconyRailHeightMetres = 1.0;
  double balconyPostSideMetres = .16;
  double balconyPostSpacingMetres = 1.9;

  // ---- the stair in a tower ------------------------------------------------
  //
  // A tower stair is not a taller version of the one in a corner of a room. It
  // climbs the inside face of the shaft, turning at every corner, and it serves
  // every storey the tower passes rather than joining one floor to the next -
  // which is the whole reason a building has a tower.

  // One flight to a side of the shaft: it winds round the wall rather than
  // filling it.
  bool towerNewelStair = true;

  // How deep one step is, and the most one may rise.
  //
  // A flight is sized by its steps, not by the wall it runs along. Given the
  // side of the shaft to fill, a quarter storey stretched over four and a half
  // metres came out as four treads a metre deep each - which is a ramp with
  // trip hazards in it, not a stair. The run follows from the going and the
  // rise; whatever is left of the side is landing, and a tower stair has long
  // landings.
  double towerTreadMetres = .27;
  double towerRiserMetres = .19;

  // How wide the tread is, measured in from the wall. What is left in the
  // middle is the well, and the newel stands in it.
  double towerStairWidthMetres = 1.0;

  // The wall in the middle that the flights wind around. Zero leaves an open
  // well; anything else is a solid core, which is what makes a shaft safe to
  // climb and what carries the inner end of every tread.
  double towerNewelSideMetres = 1.0;

  // The share of buildings that get one when their use does not ask for it.
  //
  // Zero, and deliberately. A tower is not an ornament a building rolls for;
  // it is what a hall or a manor has, and it came out wrong everywhere else -
  // a lodging house on a wedge of a plot got a four storey shaft standing over
  // a one storey roof, with a spire wider than the shaft under it. The knob
  // stays because a settlement may well want towers on particular buildings;
  // what it may not want is a town that grew them at random.
  double towerChance = 0;

  // How the roof and the floors are framed.
  //
  // Both default to "whatever this building's seed says", which is what makes
  // a street of them look built rather than stamped. Pin either one to get the
  // same structure every time, which is what a test wants.
  //
  // New settings go at the end of this struct on purpose: it is initialised
  // positionally in places, and a field inserted above the last silently
  // re-maps every value after it.
  bool roofFrameFromSeed = true;
  RoofFrameStyle roofFrameStyle = RoofFrameStyle::CollarBraced;

  // The hammer beam, when that is the roof: how far it reaches in from the
  // wall head, and how heavy it is.
  double hammerBeamReachMetres = .9;
  double hammerBeamWidthMetres = .16, hammerBeamDepthMetres = .22;

  // The landings at the two ends of a flight, and the flight itself.
  //
  // A stair is not the sloping part. It is a place to stand before you start
  // climbing, the flight, and a place to arrive that you can walk away from -
  // and the two ends are the half of it that everything else has to agree
  // with. Without them a door opens onto the third step, the floor above is
  // cut away where you arrive, and a building that is perfectly correct in
  // every measurement is one you cannot walk through.
  //
  // So they are built as pieces: landing, flight, landing, each a rectangle in
  // its own right, each a piece of the floor it belongs to. A turn is another
  // landing between two flights and nothing else new.
  double stairLandingDepthMetres = 1.0;
  double stairWidthMetres = 1.0;
  double stairRiserMetres = .18;

  // How much clear floor a door needs on each side of it.
  //
  // Measured from the doorway into the room, and what it is for is the fault
  // in the picture: a flight of steps crossing a doorway. A door has to open
  // onto floor, so anywhere a flight runs is somewhere a door may not be.
  double doorClearanceMetres = .9;

};

// A part of a building that is massed on its own rather than carved with the
// rest of it.
//
// The carve divides an outline into wings at its reflex corners, which is the
// right rule for a plan that is all one height and says nothing about a piece
// that is deliberately taller. A tower is exactly that piece: it stands on
// ground the rest of the building may not use, it carries its own storey
// count, and its roof is its own because no neighbour reaches it.
struct MassingPart
{
  std::vector<rewrite::Position> footprint; // Counter clockwise, on the ground.
  uint32_t storeys = 1;
  double roofPitch = 0; // Zero takes the settings' pitch.
  bool tower = false;

  // Ground the building stands over but not on: taken out of the ground storey
  // and left under the ones above it, with posts along whatever part of its
  // edge is on the outside.
  //
  // The opposite sign of every other part, and the same mechanism. A tower is
  // ground only the tower may use; an entrance bay is ground nothing may be
  // built on at all. Both are decisions the reflex-corner carve cannot reach,
  // because the carve reads an outline and neither of these is visible in one.
  bool recess = false;
};

// The massing of a building: wings, their storeys, their roof clusters and the
// surfaces of the roof over each.
//
// Exposed on its own because a building with no interior - one of the town's
// ordinary houses, which is a footprint and a height - has the same exterior
// as a planned one and no reason to go through the planner to get it.
//
// `outline` is the ground, counter clockwise; its y is ignored.
void massBuilding(const std::vector<rewrite::Position> &outline, double foundation, double floorRise, uint32_t maximumStoreys, const ComplexDwellingSettings &, std::vector<rewrite::DwellingWing> &, std::vector<rewrite::DwellingRoofFace> &, std::vector<rewrite::DwellingStorey> * = nullptr, std::vector<rewrite::DwellingPillar> * = nullptr, double jettyMetres = 0, double arcadeDepthMetres = 0, uint32_t minimumStoreys = 1, const std::vector<MassingPart> *parts = nullptr);

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
