#pragma once

#include "procedural/geom2/CellGrid.hpp"
#include "procedural/geom2/CellShape.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/ArchitecturalProgram.hpp"
#include "procedural/world/generator/Ledger.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace procedural::world::generator {

using geom2::Board;
using geom2::CellFrame;
using geom2::CellGrid;
using geom2::Chain;
using geom2::Dir;

// A dwelling planned on a cell grid.
//
// A fourth planner, standing beside `planDwelling`, `planComplexDwelling` and
// `planProgrammeDwelling` rather than replacing any of them. What it brings
// that they do not is Watabou's subdivision: a footprint that is an articulated
// polyomino rather than a rectangle, cut into rooms by walls seeded at the
// corners of its own outline, so the rooms line up with the building.
//
// It is also the first of them handed a POLYGONAL LOT rather than a rectangle
// somebody else already accepted. It decides its own footprint, on a grid laid
// over the ground it is given, building around whatever that ground has on it.

// ---------------------------------------------------------------------------
// The stream
// ---------------------------------------------------------------------------

// One sequential random stream per building, snapshotable.
//
// Deliberately not `Context::unitRandom`: that is purpose-addressed so a result
// does not depend on the order regions are visited, which is the right property
// at the boundary and the wrong one here. This algorithm draws in sequence and
// in places wants to rewind and replay, so the boundary seeds this once and
// everything inside is ordinary.
struct Stream
{
  uint64_t state = 1;

  explicit Stream(const uint64_t seed) : state(seed == 0 ? 0x9E3779B97F4A7C15ull : seed)
  {
  }

  uint64_t next();

  // [0, 1)
  double unit();

  uint64_t snapshot() const
  {
    return state;
  }

  void restore(const uint64_t saved)
  {
    state = saved;
  }

  // Three uniforms averaged: an approximately bell-shaped variate in [0, 1),
  // which is the shape this algorithm reaches for constantly.
  double bell3();

  // Four uniforms, halved and centred: a signed bell in [-1, 1].
  double bell4();

  // A fractional count made whole without bias, which is how every "about this
  // many" in the algorithm becomes an integer.
  int32_t roundStochastic(double);

  uint32_t below(uint32_t limit);
  int32_t range(int32_t low, int32_t high); // Inclusive.

  template <class T> const T &choose(const std::vector<T> &from)
  {
    return from[below(uint32_t(from.size()))];
  }

  // Removes and returns, which is Watabou's `pick` and is not the same as
  // choosing: a caller that draws without replacement gets it from here.
  template <class T> T take(std::vector<T> &from)
  {
    const uint32_t k = below(uint32_t(from.size()));
    T value = from[k];
    from.erase(from.begin() + k);

    return value;
  }

  template <class T> const T &weighted(const std::vector<T> &from, const std::vector<double> &weights)
  {
    double total = 0;

    for (const double w : weights)
    {
      total += w;
    }

    double pick = unit() * total;

    for (size_t k = 0; k + 1 < from.size(); ++k)
    {
      pick -= weights[k];

      if (pick <= 0)
      {
        return from[k];
      }
    }

    return from.back();
  }

  template <class T> std::vector<T> shuffled(std::vector<T> values)
  {
    for (size_t k = values.size(); k-- > 1;)
    {
      std::swap(values[k], values[below(uint32_t(k + 1))]);
    }

    return values;
  }
};

// ---------------------------------------------------------------------------
// The brief
// ---------------------------------------------------------------------------

// The ground a building may stand on, as cells.
//
// `blocked` and `obstacle` are not the same thing and conflating them is a
// mistake worth naming: most blocked ground is simply outside the setback -
// the garden, the yard, the street - and a front door opening onto it is
// exactly right. An obstacle is different. It is a rock, a well, a court, a
// neighbour's wall: ground you cannot build on AND cannot step out onto.
struct SiteBoard
{
  Board buildable; // Where the building may stand.
  Board blocked;   // Everything else on the grid, garden included.
  Board obstacle;  // The part of that which is solid: a door may not open onto it.
};

// How a footprint is made.
//
// No class picks one of these. A class says how big a building is and how fine
// its rooms are, and the stencil generator makes the outline from the grid and
// the seed - which is what `Polyomino` means and what every building gets
// unless a caller says otherwise. A table of shapes per class was tried and
// removed: it can only make the shapes in the table, and at the sizes a real
// lot offers they degenerate into open squares and hairline ranges.
//
// The rest are here for a caller that has already settled the shape, and there
// are exactly two: the `square` tag, and a compound wing whose rectangle the
// curtain wall dictates.
enum class FootprintKind : uint8_t
{
  // Watabou's twelve stencils, stretched and jittered, with the block size
  // scaled to the class's size band. Every building, at every scale, unless a
  // caller has settled the shape itself.
  Polyomino,

  // A plain rectangle. A shop, a warehouse, a terrace house.
  Box,

  // A range with a wing at one end. The commonest large plan there is.
  RangeL,

  // A range with a wing at each end, facing the same way: the English H.
  RangeH,

  // A range with three wings: the Elizabethan E.
  RangeE,

  // A ring around a court. The court is CHOSEN, not stumbled into - the
  // stencils cannot make a ring, and a ring that appeared by accident is a
  // light well nobody designed.
  Courtyard,

  // A nave with transepts and a chancel. Not subdivided: a nave is one space
  // the length of the building, and cutting it into parlours is the failure
  // this kind exists to prevent.
  Axial
};

const char *name(FootprintKind);

struct GridClass
{
  // How this footprint is made. `Polyomino` unless a caller settled it, which
  // only a compound wing and the `square` tag ever do.
  FootprintKind footprint = FootprintKind::Polyomino;

  // The board tier: how many rows of the grid this class may use. Every plan a
  // house makes fits in sixteen; a manor wants thirty-two and a castle
  // sixty-four, and the board's row-per-word layout makes the tier cost
  // nothing but memory.
  uint32_t rows = 16;

  double cellMetres = 2.2;
  uint32_t minimumCells = 10;
  uint32_t maximumCells = 34;

  double storeyMetres = 3.0;
  bool fitToLot = false; // Prefer the largest buildable rectangle, not a random size.
  uint32_t minimumSpanCells = 2;
  double maximumAspectRatio = 0; // Zero preserves authored non-city ranges.
  double compactnessPenalty = .18;
};

struct CoveredPassageRequest
{
  uint64_t id = 0;
  geom2::Polygon polygon; // Whole grid cells; public ground, occupied above.
  double clearance = 2.4;
};

struct CellDwellingBrief
{  struct Capacity
  {
    // City allocation preferences, not class minimums. The fitted footprint
    // and room demand determine height, subject to the caller's floor limit.
    double preferredFootprintMetres2 = 40;
    uint32_t desiredRooms = 2;
    double roomAreaMetres2 = 15;
    bool fillLot = false;
    bool attachedRow = false; // Fit integral grid columns across a shared-wall frontage.
    uint32_t minimumSpanCells = 2; // City lots explicitly request a 4 x 4 base.
    uint32_t minimumFloors = 1;
    double maximumAspectRatio = 0; // Optional city policy; authored compound wings remain unconstrained.
    double compactnessPenalty = .18; // Zero retries full capacity within the same aspect bound.
  };
  std::optional<Capacity> capacity;
  uint64_t id = 0;
  uint64_t seed = 1;
  BuildingClass building = BuildingClass::Dwelling;

  // Counter-clockwise, metres, in the caller's local frame.
  geom2::Polygon lot;

  // Which lot edges face the street. The first one orients the grid, and every
  // one of them is a side the front door may open onto.
  std::vector<uint32_t> frontage;

  double setbackMetres = 0;
  // Reserve access/projection space only along street-facing edges.
  double frontageSetbackMetres = 0;

  // Ground inside the lot the building must build around.
  std::vector<geom2::Polygon> obstacles;

  // Which lot edges are PARTY walls: a neighbour's house stands hard against
  // them. No window opens in one, no door, and the roof gables against it
  // rather than falling to an eave over the neighbour's kitchen. Empty for a
  // house standing on its own lot, which is every house until P21.
  std::vector<uint32_t> party;

  uint32_t floors = 0; // Zero derives it.

  // Accepted street-facing projection of each floor beyond the one below, in
  // metres, indexed by floor. Empty means every floor is flush.
  //
  // An input, not a decision: `decideJetty` settles it upstream from the street
  // width, what the facade opposite already projects, and the household's
  // standing, because a jetty is a negotiation with the street and not a
  // property of the house. Consumed at realisation - see the note below.
  std::vector<double> jettyMetres;

  // How the household stands on its plot, which is what decides where on the
  // ground the building goes.
  //
  // Not "push it to the street": that is only true of the bottom of the scale.
  // A craftsman's house is on the street line with its yard behind, and a
  // patrician's or a mansion stands BACK, behind a forecourt, with the laid-out
  // ground in front where everyone can see it. Getting that backwards makes
  // every rich house look like a shop.
  PlotStanding standing = PlotStanding::Modest;

  // A board the caller has already settled, which overrides the class's.
  //
  // For a building standing on its own lot the class knows best, and this stays
  // empty. For a WING of a compound it does not: the range against a castle's
  // curtain is six metres deep and forty long because the curtain says so, and
  // the class's size band - which describes a whole castle - would refuse it.
  // The class still picks the programme; only the measurements move.
  std::optional<GridClass> grid;

  // Watabou's tag vocabulary, which is the whole authored surface: size,
  // shape, height, wall rule, glazing, stair scheme.
  std::vector<std::string> tags;
  std::optional<CoveredPassageRequest> coveredPassage;
  std::optional<geom2::CellFrame> sharedGrid;
  rewrite::DwellingArchitecture architecture=rewrite::DwellingArchitecture::Vernacular;
  uint32_t architecturalAgeYears=0;

};

struct CellDwellingSettings
{
  GridClass grid;

  // How finely the inscribed rectangle is searched for. A quarter cell is fine
  // enough that the anchor does not wobble between seeds and coarse enough to
  // stay cheap.
  double anchorResolution = .25;

  // Retries before a footprint that will not fit the buildable ground is given
  // up on.
  uint32_t footprintAttempts = 64;

  // Cells per room. The floors are cut at the first; the setback and the
  // basement are decided against a deliberately coarse partition at the
  // second, which is Watabou's own two-pass arrangement and not a stand-in for
  // the fine one.
  double averageRoomCells = 6;
  double coarseRoomCells = 12;

  // A floor with less ground than this under it is not a floor.
  uint32_t minimumFloorCells = 4;

  uint32_t maximumFloors = 8;

  // How many storey heights a tall room stands: a nave, a chancel, a great
  // hall. Two is the usual answer and it is what "spanning multiple floor
  // heights" means.
  double tallRoomStoreys = 2.;

  // How many sides the bell chamber has, and how far its cap rises.
  //
  // Six. Enough to stop reading as a box and few enough that each face is a
  // real face with a real opening in it, which is the point: at ten sides the
  // openings are slots and the thing reads as a barrel.
  uint32_t bellChamberSides = 6;
  double bellChamberCapMetres = 3.2;

  // The smallest a room may be across, in cells. One is off, which is right for
  // a cottage; two is a hard floor and is what the large classes carry, because
  // a manor with a one-cell room has a corridor somebody has called a room.
  uint32_t minimumRoomCells = 1;

  // How many cells across the reserved stair is. One is a stairwell; two or
  // three is a tower, and only the largest classes carry one.
  uint32_t stairTowerCells = 1;

  // How many storeys the stair tower carries above the mass it serves. See
  // `ClassProfile::stairTowerAbove`.
  uint32_t stairTowerAbove = 0;

  // Towers at the angles of the plan, and how far above the eaves they carry.
  // See `ClassProfile::cornerTowers`.
  uint32_t cornerTowers = 0;
  double cornerTowerStoreys = 2.;

  // How wide this class's corridors are. A passage in a house is one cell; a
  // gallery in a castle or a mansion is two, and at three metres a cell that is
  // the difference between squeezing past and walking side by side.
  uint32_t corridorCells = 1;

  // Courts, carried from the class: how much of the ground one takes and how
  // many are cut. Zero courts means none, which is most buildings.
  double courtShare = .16;
  uint32_t courts = 0;

  // A yard: an outdoor room at the BACK of the mass, open to the lot on one
  // side and held by the house on the others. See P18 - a yard is where the
  // work happens, not what is left over, so it is cut from the same cells the
  // rooms are and it is a room in the door graph like a court. Zero yards is
  // off, which is the default; the class says otherwise.
  double yardShare = .14;
  uint32_t yards = 0;

  // How tall the wall round the open sides of a yard is. A yard is fenced, not
  // roomed: built to the storey it read as a room with no roof.
  double yardWallMetres = 1.3;

  // A pen (P27): a second outdoor room, fenced in boards rather than walled
  // in stone, with a gate onto the lane; and a shed against it - one cell,
  // one storey, its own roof, a door onto the pen. Nought is off.
  // Whether a cellar may be dug at all. A hut has none (P27).
  bool cellars = true;

  double penShare = .12;
  uint32_t pens = 0;
  double penFenceMetres = 1.1;
  uint32_t sheds = 0;
  double shedMetres = 2.3;

  // A front that reads as designed rather than as grown.
  //
  // NOT a mirrored plan. Reflecting half a stencil about its centre line was
  // the first answer and it manufactures a small set of shapes - a U, a T, a
  // plus - which recur often enough to read as templates, which is the one
  // thing the stencil path exists to avoid. What a formal building actually
  // reads as is a single straight wall to the street with the door in the
  // middle of it, and that is a constraint on the FRONT rather than on the
  // plan: everything behind it stays the seed's business.
  bool formalFront = false;

  // Above this many cells a floor gets a planned circulation spine instead of
  // relying on corridors emerging from the subdivision. Every house-sized
  // class is below it; the large classes arrive at phase 13.
  uint32_t circulationThresholdCells = 60;

  // ---- stairs --------------------------------------------------------------
  //
  // A flight is walkable or it is not, and the number that decides it is the
  // tread. The riser is settled by the climb - `stepsFor` fixes the count from
  // the height and never trades it away - so what a short run costs is depth
  // underfoot, and below this it is a ladder with a handrail.
  // The narrowest a flight may be. Below this it is a ladder with a handrail,
  // and two of them side by side in one cell is what a half turn becomes on a
  // fine grid.
  double minimumFlightWidthMetres = .8;

  // Prefer a single flight in a long circulation room; retain a turning
  // stair when the available floor cannot safely accommodate one.
  bool preferStraightStairs = false;

  double minimumTreadMetres = .24;
  double comfortableRiserMetres = .18;

  // A floor a flight cannot reach is not a floor. The fallback hatch stays
  // available to a loft or a bell chamber, which is what a ladder is for; an
  // ordinary house that cannot be climbed loses the storey instead, because a
  // storey with no way up is a room nobody will ever stand in.
  bool allowTrapdoor = false;

  // ---- human dimensions ----------------------------------------------------
  //
  // The class profiles count CELLS, and a city house is planned on a one metre
  // cell, so a minimum written in cells becomes half a metre wide down there.
  // These are the same minima in metres; the planner converts them against the
  // cell it actually got. See P-D1.
  // Clear width, not gross. A corridor of 1.4 m on a one metre cell came out
  // as two cells, and two cells minus a wall either side is not a passage two
  // people pass in - it is a passage they turn sideways in. Asking for 2.1
  // takes three cells, which leaves two clear metres between the plaster.
  // Capacity-driven footprints use this pitch; larger public buildings need
  // enough wall span for generous portals without stretching finished meshes.
  // A fixed 1.2 m construction cell forms 2.4 m two-cell bays. Keep this
  // metric across neighboring lots; style changes never squeeze the lattice.
  double capacityCellMetres = 1.2;
  double minimumCorridorMetres = 2.1;
  double minimumRoomSpanMetres = 2.4;


  // Whether a door is removed when a cheaper two-hop route already joins the
  // rooms it serves. This is what turns a plan of rooms-off-rooms into a plan
  // of rooms-off-a-corridor, because a corridor is priced at almost nothing.
  bool pruneRedundantDoors = true;

  // ---- the jetty -----------------------------------------------------------
  //
  // The envelope `decideJetty` already negotiates upstream, enforced here so a
  // brief that was not built by it cannot smuggle a two metre overhang past.
  double jettyPerFloorMetres = .6;
  double jettyTotalMetres = 1.0;

  // ---- openings ------------------------------------------------------------
  //
  // How much of a floor's free outer wall is glazed, before the per-floor ramp
  // is applied. Watabou's own: an upper floor gets MORE glass than a lower one,
  // which is the opposite of what an intuition about structure would suggest
  // and is what makes these facades read as domestic.
  double windowDensity = .75;
  // Whole-building budget, after room lighting preferences. -1 is unlimited.
  int32_t maximumWindows = -1;

  double outerWallMetres = .20;
  double innerWallMetres = .10;
  double windowSillMetres = .95;
  double windowHeightMetres = 1.3;
  double stairWidthMetres = 1.0;
  double stairLandingDepthMetres = 1.0;
  double doorWidthMetres = 1.1;
  double entranceWidthMetres = 1.2;
  double entranceHeightMetres = 2.3;
  uint8_t entranceLeaves = 0; // Zero selects the building-class default.
  rewrite::DoorProfile entranceProfile = rewrite::DoorProfile::Automatic;

  // The stoop: how far it projects and how many treads it has.
  double entranceStepMetres = .28;
  uint32_t entranceSteps = 3;
  // Opt-in site development; the lot grid must have a free landing, flight
  // and ground approach. Dense/religious allocations leave this at zero.
  uint32_t entranceFlightCells = 0;
  double entrancePlatformRise = 1.08;
  bool sidewaysEntrance = true;

  // ---- the balcony ---------------------------------------------------------
  //
  // A floor standing outside the walls on posts, with a rail round it. Zero
  // depth is no balcony, which is the default: it is a thing a building has
  // because something decided it should, not a thing every building has.
  double balconyDepthMetres = 0;
  double balconyShare = .55;
  double balconyMinimumRunMetres = 1.6;
  double balconyRailHeightMetres = 1.0;
  double balconyPostSideMetres = .16;
  double balconyPostSpacingMetres = 1.9;
  double floorBoardMetres = .045;

  // ---- the roof ------------------------------------------------------------
  // How much wider a pair of doors is than a single one. Not twice: a double
  // door shares its jambs, and at twice the width it stops fitting the cell it
  // is cut into.
  double doubleDoorScale = 1.85;

  // Rise per metre of run, at a room of one storey.
  //
  // A roof of this age is steep. The shallow pitch that was here put the apex
  // barely above the wall head, which is a suburban bungalow's profile and not
  // a hall's, and on a church it left the nave ridge below the tower's own
  // shoulder.
  double roofPitch = .7;

  // ...and a roof is steeper the taller the room under it.
  //
  // Not decoration: a room built to two storeys is a hall, a nave or a chancel,
  // and the whole point of building it that tall is that you see it from
  // outside. Its roof is pitched in proportion, so the chancel out-tops the
  // nave and the nave out-tops the aisles beside it - the section a church
  // actually has - without anything naming those rooms here. The ceiling is
  // the cap: past this the skeleton's apex runs away from the plan.
  double roofPitchCeiling = 1.6;

  // ...and however steep the pitch, the ridge stops this far above the eaves.
  //
  // A pitch is a rise per metre of run, so the ridge of a deep building grows
  // with its depth: at 0.7 a twenty metre span peaks seven metres over the
  // wall head, which is a spire on a house and is what merging the tiny lots
  // and glueing the rows produced. A real roof of this age does not do that -
  // it shallows, or it is broken into ranges - so the ridge is capped and the
  // whole piece is scaled to fit, which shallows the pitch and keeps the
  // skeleton the plan asked for. Multiplied by the band's storeys, so a hall
  // still out-tops the ranges beside it.
  double roofRidgeMetres = 3.2;

  // Hip every roof end, even where a wall is shared.
  //
  // A raised edge does not slope: the roof RISES to it. A house hemmed in on
  // three sides by party walls keeps one eave and its roof becomes a single
  // plane leaning on a wall, which is where the one-face roofs come from. A
  // hip slopes at every end, so every roof gets a ridge and the skeleton
  // makes the hips it is for.
  //
  // The cost is named, because a test names it: `streets: no roof falls to an
  // eave on a party line` fails while this is on, and it is right to - a
  // hipped end over a shared wall drains onto the neighbour. Held on as the
  // interim look until the terrace planner can run a row's ridge along the
  // ROW, gabled at the row's two ends and eaved front and back, which is the
  // only thing that answers both.
  bool roofAlwaysHipped = true;

  // What a `fortified` roof stands behind: how far the wall carries past the
  // deck, and how thick it is up there.
  double parapetHeightMetres = 1.1;
  double parapetThicknessMetres = .55;
  double eavesOverhangMetres = .45;

  // Roof windows as actual little buildings: a wall-and-window shell carried
  // by the main roof, capped by its own pair of tiled faces. The probability
  // is per building; the structural phase retains the support relationship.
  double dormerChance = .55;
  uint32_t maximumDormers = 2;
  double dormerWidthMetres = 1.25;
  double dormerDepthMetres = 1.1;
  double dormerWallMetres = .85;
  double dormerRoofMetres = .38;
  // Chimneys are off for now. Not deleted: a house of this age has one, and
  // the rule that finds a cell clear of the storey above in all eight
  // directions is the part that was hard. Turn it back on when the stacks are
  // worth drawing.
  bool chimneys = false;
  double chimneySideMetres = .95;
  double chimneyAboveRidgeMetres = .9;

  // ---- the turret ----------------------------------------------------------
  //
  // A round tower is drawn as a prism, because everything downstream of here
  // wants faces. Six sides is the fewest that reads as round at this scale and
  // the most that stays cheap; the cap is the same count of triangles meeting
  // at a point.
  uint32_t turretSides = 6;
  double turretRadiusMetres = 1.6;
  double turretCapMetres = 2.2;

  // ---- the bell tower ------------------------------------------------------
  //
  // How tall the open stage at the head of a belfry is, and how far the wall
  // carries above it.
  //
  // The bell room is not a room the subdivision cut: the tower is ONE tall
  // room, and the bell room is its top - the stage where the wall opens on all
  // four sides so the bells are heard. That is what makes a tower read as a
  // bell tower rather than as a tall blank box, and it is the only part of a
  // church you are meant to see from the next valley.
  double bellStageMetres = 3.4;
  double bellStageHeadMetres = .9;

  // How often the shaft is climbed by a ladder rather than by a stair.
  double towerLadderChance = .45;

  // How often a church carries its bells in TWO towers flanking the front
  // instead of one at the back. A west front, and it is also most of what
  // stops every church on a sheet reading as the same church.
  double twinTowerChance = .4;

  // How wide the stair that winds round the inside of a tower's wall is.
  //
  // It climbs in quarter turns, one flight per wall, which is how a wall stair
  // is built: there is no room in a tower for a flight that runs anywhere but
  // along a side, and the corner is where you turn.
  double towerStairWidthMetres = .9;
  double towerRiserMetres = .19;
};

// ---------------------------------------------------------------------------
// The plan, as cells
// ---------------------------------------------------------------------------

// A jetty is METRES, not cells.
//
// Worth stating where the model is, because it is the one place this
// representation could be misread. An upper floor's cell area is always a
// SUBSET of the floor below - the setback only ever removes - and P2 asserts
// exactly that. A jetty does not contradict it: it is 0.45 to 0.6 m and a cell
// is 2.2, so a jetty is not a cell at all. It is an outward offset of the
// storey POLYGON, applied to the street-facing walls at realisation, after the
// cells have stopped being relevant. The same is true of a balcony, which is
// not part of any storey plate by construction.
// How a house is climbed. Settled before any room exists, because two of the
// three schemes change how the rooms are cut and the third needs a floor plan
// to find a cell in.
enum class StairScheme : uint8_t
{
  None,      // One storey. Nothing to climb.
  Spiral,    // Legacy unreserved corner turret; new tower requests use Stairwell.
  Stairwell, // One cell reserved on every floor, one above the other.
  PerFloor   // A flight found on each floor once its rooms and doors exist.
};

const char *name(StairScheme);

// Legacy corner-turret metadata. New plans reserve Stairwell::block instead;
// the realiser rejects this unreserved representation so it cannot emit a
// closed pillar intersecting rooms and stairs.
struct Spiral
{
  uint32_t entrance = geom2::kNoEdge; // The contour edge it stands on.
  uint32_t exit = geom2::kNoEdge;     // The edge before it, which the shaft also opens through.
  uint32_t landing = geom2::kNoCell;  // The cell it lets into.
  uint32_t node = geom2::kNoNode;     // What the shaft is centred on.
};

// One cell reserved for the stair on every floor, in the same place.
//
// The contract the subdivision honours: `stair` is its own one-cell room on
// every floor, connected to `landing` through `exit` and to nothing else. The
// setback may not take it away, or an upper floor has no way up to it.
struct Stairwell
{
  uint32_t stair = geom2::kNoCell;
  uint32_t landing = geom2::kNoCell;
  Dir exit = Dir::North;

  // The whole reservation, which is usually the one cell above and at castle
  // scale is a block of them: a square stair TOWER, two or three cells across,
  // standing through every storey with the flight in one corner of it and
  // landings round the rest.
  //
  // A castle's stair is not a cottage's with more steps. It is a structure -
  // the thing that carries a garrison between four floors - and one cell of it
  // is a ladder. Always non-empty and always contains `stair`, so anything that
  // asks "is this cell the stairwell" can ask this instead and be right at both
  // scales.
  Board block;
  bool integratedTower = false; // Flights must stay in this reserved volume.
};

// How a stair is shaped. The stair stands in a WELL: a line of `length` cells
// from `cell` in direction `dir`, the same cells on both floors it joins. On
// the lower floor the well is floor with the flights standing on it; on the
// upper floor it is the hole the flights come up through.
//
// A straight flight climbs the whole well and arrives at its far end; a half
// turn is two flights side by side - up the well on one side, back down it on
// the other - joined by a landing at the far end, which is a piece of floor
// half way up that both flights connect to; the arrival is at the near end,
// where you came in. The well is as long as the form needs to be walked
// (P29): the run is sized from the climb and the tread, never the tread from
// the run, and a form that does not fit any well the two floors share is not
// built. A trapdoor is the honest fallback: a hatch and a ladder, one cell.
enum class StairForm : uint8_t
{
  Straight,    // One flight the length of the well.
  HalfTurn,    // Two flights side by side and a landing at the far end.
  QuarterTurn, // Kept for the name table; no longer built.
  Trapdoor     // No direction at all. A ladder, and the fallback.
};

const char *name(StairForm);

// One end of a stair: the well's entry cell on this floor, the way the well
// runs, and where the stair goes. The same record, `up` reversed, is on the
// other floor.
struct CellStair
{
  uint32_t cell = geom2::kNoCell; // The entry cell: the near end of the well.
  int32_t toLevel = 0;
  Dir dir = Dir::North;           // The well runs this way from the entry cell.
  uint32_t length = 1;            // ...for this many cells.

  // ...and this many cells ACROSS, to the cw side of `dir` from the entry
  // cell. One on a coarse grid, where a cell is already two and a half metres
  // and a flight fits across it; more on a fine one, where it does not. A
  // well narrower than its flights is what turns every city stair into a
  // ladder, and the width is a property of the well, not of the drawing.
  uint32_t across = 1;
  StairForm form = StairForm::Trapdoor;
  bool up = false;

  // What the flights measure, once the well they have was taken into account.
  // `risers` and `tread` are per flight; a half turn has two of them.
  uint32_t risers = 0;
  double riser = 0;
  double tread = 0;
  double width = 1.;   // Of one flight, in metres.
  double landing = 1.; // The turn landing's depth along the well (half turn).

  // A half turn climbs the right of the well and arrives down its left;
  // mirrored, the other way round - chosen so the arrival is on the side
  // the floor (or the door) is.
  bool mirrored = false;
};

// Every cell of a stair's well, entry cell first.
std::vector<uint32_t> wellCells(const CellGrid &, const CellStair &);

// A way through a wall, as the edge it is cut in.
//
// The edge is on room `a`'s side and its twin is on `b`'s, which is the whole
// of what a door is on this model: one lattice edge, shared by two rooms,
// walked from either side.
// What is actually in the opening.
//
// `Open` is not a door that happens to be missing: it is a full cell-width
// archway, and the wall is not drawn there at all. That is Watabou's `NULL`,
// and it is the default - a door leaf is something a room has to earn by being
// somewhere you would close a door on.
enum class DoorKind : uint8_t
{
  Open,    // No wall, no leaf. An archway the width of the cell.
  Doorway, // A gap in the wall, no leaf.
  Solid    // A gap with a door in it.
};

struct CellDoor
{
  uint32_t edge = geom2::kNoEdge;
  uint32_t a = 0;
  uint32_t b = 0;
  DoorKind kind = DoorKind::Open;

  // How many leaves it hangs on, which is how wide it is.
  //
  // Two is a pair of doors, and a building has a handful of thresholds that
  // want them: the way in from the street on a formal front, the way out into a
  // court, the way from a nave into a chancel, the way into the one great room
  // of the house. Everywhere else a door is a door. The distinction is not
  // decoration - a double door is the plan telling you which thresholds
  // matter, and a building where every door is the same size has no hierarchy
  // to read.
  uint8_t leaves = 1;
};

struct CellFloor
{
  int32_t level = 0;
  Board area;
  std::vector<Chain> loops;

  // Filled at phase 4. The rooms partition `area` exactly - plus, on the ground
  // floor, the courts, which are rooms with no floor above and no roof over.
  std::vector<geom2::Board> roomAreas;

  // How tall each room is, in metres, where it is not the storey's own height.
  // Parallel to `roomAreas`; nought means "whatever the storey is".
  //
  // This is how one part of a building stands taller than the rest, and it is a
  // property of the ROOM rather than of the storey - which is the only way to
  // say it, because a nave six and a half metres tall beside a vestry three and
  // a quarter is one storey with two heights in it, not two storeys. Everything
  // that consumes it asks the same question it always asked: a wall is as tall
  // as the tallest room it divides, and a roof dies into anything taller than
  // itself, whether that taller thing is the storey above or the nave next
  // door.
  std::vector<double> roomHeight;

  // Which of those rooms are open to the sky. Parallel to `roomAreas`.
  //
  // A court has to BE a room, whatever else it is, because a room is what a
  // door joins. Leaving it out of the room list left the reference plans'
  // central arrangement unbuildable: in a castle you cross the building through
  // its courts, and a court nothing can open onto is a light well - measured,
  // it put the far room of a castle six doors from the gate instead of three.
  std::vector<uint8_t> roomOutdoor;
  std::vector<Chain> roomContours;
  std::vector<Chain> innerWalls;

  // Reserved circulation, where the floor is large enough to need planning
  // rather than emergence. Empty for anything cottage-sized.
  Board circulation;

  // Filled at phase 5. Every door on this floor, and which rooms it joins.
  std::vector<CellDoor> doors;

  // Filled at phase 6. Every flight standing on this floor.
  std::vector<CellStair> stairs;

  // Filled at phase 7. One entry per room, parallel to `roomAreas`.
  std::vector<rewrite::RoomUse> roomUses;
  std::vector<rewrite::RoomKind> roomKinds;

  // What the purpose asked for, kept because phase 8 needs it: how much of the
  // room's outer wall is glazed (negative means the purpose did not say), and
  // whether its doors are real doors.
  std::vector<double> roomWindowRatio;
  std::vector<uint8_t> roomSolidDoors;

  // Filled at phase 8. Contour edges of this floor that carry a window.
  std::vector<uint32_t> windows;

  // Where a room's kind was settled by the shape rather than by the programme:
  // an axial building's nave and chancel are what they are because of where
  // they stand, and no matching may overrule that.
  std::vector<uint8_t> roomFixed;

  // Which rooms are corridors: every cell of them too narrow to be anything
  // else. Indices into `roomAreas`.
  std::vector<uint32_t> corridors;
};

// One bell tower: the shaft's cells, the deck at the top of the square part,
// the top of the drum under its cap, the circle the drum stands on (kept so the
// deck, the hole through it and the drum cannot disagree), and how the shaft is
// climbed - a stair winding round its wall, or a ladder against one of them.
struct CellBellTower
{
  Board cells;
  double deckMetres = 0;
  double topMetres = 0;
  double x = 0;
  double z = 0;
  double radius = 0;
  bool ladder = false;
};

struct CellHouse
{
  geom2::Polygon architecturalLot;
  rewrite::DwellingArchitecture architecture=rewrite::DwellingArchitecture::Vernacular;
  uint32_t architecturalAgeYears=0;
  CellGrid grid;
  CellFrame frame;
  SiteBoard site;
  GridClass gridClass;
  rewrite::DwellingSiteAccess siteAccess;

  std::vector<CellFloor> floors;
  std::optional<CellFloor> basement;

  // Open ground inside the building: a court, cut out of the footprint.
  //
  // Not part of any floor. That is the whole model and it is what makes every
  // other piece fall out for free: the floor becomes a region with a hole in it
  // (or a U, where the court reaches the outside), which the ring machinery
  // already handles; no storey stands over it, because upper floors are subsets
  // of the ground; and the roof never covers it, because the roof covers the
  // floor. A court is an absence, and absences need no special case.
  Board courts;

  // Which of the courts are pens (P27), and the sheds: one cell each,
  // outside the house and every court, against a pen.
  Board pens;
  std::vector<uint32_t> sheds;

  // The bell towers, each two towers standing one on the other.
  //
  // A square shaft carrying a flat deck, and a six-sided drum on the deck with
  // the bells in it. That is not a compromise between the square tower and the
  // round one - it is how a tower of this kind is actually built: the mass that
  // has to carry the load and butt against the nave is square and shares its
  // walls with the building, and the stage that has to be seen and heard from
  // every direction is not. The change of shape is also where the climb
  // changes: you walk up the square part and you go through a hole in the deck
  // to reach the bells.
  //
  // One at the back, centred on the chancel, or two at the front flanking the
  // door - a west front - which is the other way a church carries its bells
  // and the seed's to choose. Each tower is its own record because nothing
  // about one depends on the other except that both clear the same ridges.
  std::vector<CellBellTower> bellTowers;

  // Drawn once with the footprint: two towers at the front, or one at the
  // back. Read by the partition on every storey so they agree.
  bool twinTowers = false;

  // The two slices along the nave's axis the west front was widened on, in
  // grid coordinates (rows when the nave runs along rows, columns otherwise),
  // so the partition puts the towers on exactly those and never on a porch or
  // a crossing. Empty when there is no west front.
  std::vector<uint32_t> westFrontSlices;

  // Contour edges of the ground floor that lie on a party wall. Settled once
  // from `CellDwellingBrief::party` when the footprint is, so the openings and
  // the roof read one answer. Sorted, for `std::binary_search`.
  std::vector<uint32_t> partyEdges;

  // ...and how far short of the lot line each of them stops, in metres.
  //
  // The lattice is the street's, shared along the row so party walls line
  // up, and an integral number of cells rarely reaches the lot edge: a row
  // house stopped a median 0.56 m short of every shared edge, so a "row" was
  // a line of detached houses a metre apart. The realiser pushes each party
  // run out by its own gap, and the neighbours meet.
  std::vector<double> partyGaps; // Parallel to `partyEdges`.

  // Counted across a run, for the debug summary: how the party-wall push
  // fared. Atomic because lots are planned on a pool.
  struct PartyTally { std::atomic<uint32_t> briefs{0}, houses{0}, pushed{0}, folded{0}; };
  static PartyTally &partyTally();

  uint32_t entrance = geom2::kNoEdge; // A contour edge of the ground floor.

  // Which way the street is, in the grid's own frame - along columns and down
  // rows. Decided with the footprint, because where a building stands on its
  // plot depends on it; kept because the front door, the jetty and the balcony
  // all need to know which side is the front.
  double streetJ = 0;
  double streetI = 0;

  // Metres each floor oversails the one below, carried from the brief. Indexed
  // by floor; the ground floor entry is ignored and never moves.
  std::vector<double> jettyMetres;
  uint32_t entranceLanding = geom2::kNoCell;

  // The climb, settled at phase 3 before any room is cut.
  StairScheme scheme = StairScheme::None;
  std::optional<Spiral> spiral;
  std::optional<Stairwell> stairwell;

  // How many storeys were asked for, which the scheme's probabilities depend
  // on and the stack then tries to build.
  uint32_t plannedFloors = 1;

  std::vector<std::string> tags;

  // What kind of building this is, kept because the PROGRAMME needs it.
  //
  // The class settles the measurements up front and then gets out of the way,
  // which is right for the shape - but a castle has a chapel and an armoury
  // because it is a castle, and drawing them from the same bag of specials a
  // cottage draws from gives a castle a chapel one time in thirteen.
  BuildingClass building = BuildingClass::Dwelling;

  PlotStanding standing = PlotStanding::Modest;

  Board coveredPassage;

  // The cells the footprint took for a covered passage, known from the shell
  // onwards. `coveredPassage` itself is only settled by the reservation,
  // which runs after the stairs are placed - and the stairs took the lane:
  // fifty of fifty-six refusals in the isolated case were a well or a
  // reserved flight standing in it. Anything choosing cells before then
  // keeps off these.
  Board passageReserve;
  std::vector<uint32_t> passagePortals;
  double passageClearance = 0;
  uint64_t passageId = 0;

  const CellFloor *ground() const
  {
    return floors.empty() ? nullptr : &floors.front();
  }

  uint32_t storeys() const
  {
    return uint32_t(floors.size()) + (basement.has_value() ? 1u : 0u);
  }
};

// ---------------------------------------------------------------------------
// Phase 0: the lot frame
// ---------------------------------------------------------------------------

// Lays a grid over the lot and says which of its cells may be built on.
//
// The grid is ORIENTED to the street, ANCHORED on the largest rectangle that
// fits inside the setback, and EXTENDED over the whole lot. The anchoring and
// the extent are two different jobs and it matters that they are: gridding only
// the inscribed rectangle would make every building on every awkward lot
// rectangular, which is the fault an articulated footprint exists to avoid. The
// rectangle is there to prove the lot can hold a building at all and to fix the
// lattice phase on ground that can be built on.
bool frameLot(const CellDwellingBrief &, const CellDwellingSettings &, CellGrid &grid, CellFrame &frame, SiteBoard &site, Ledger &, std::string *error = nullptr);

// ---------------------------------------------------------------------------
// Phase 1: the footprint
// ---------------------------------------------------------------------------

// The twelve stencils, row-major over a three by three grid. A short string
// occupies only the first rows.
extern const char *const kPolyominoStencils[12];

// A polyomino on `grid`, masked to `buildable`.
//
// One stencil, mirrored and transposed as a whole, with each of its three
// columns and three rows stretched independently and each side of each block
// pushed out by a cell one time in three. That jitter is the entire source of
// the offsets and ledges that read as a building put up over time; without it
// the stencil comes out as a tidy arrangement of flush rectangles.
// How far back from the street the building stands, as a fraction of the
// buildable depth. The `PlotStanding` comments in `ArchitecturalProgram.hpp`
// are the source: built over, most of it house with a yard behind, a garden, a
// third of the plot with grounds, a quarter with the rest laid out, a cottage
// on a large pen.
double frontSetbackShare(PlotStanding);

// A polyomino on `grid`, masked to `buildable`, standing on the plot the way
// its household would.
//
// `streetJ` and `streetI` point from the building toward the street in grid
// coordinates; pass zeroes to place it anywhere it fits.
bool planFootprint(const CellGrid &, const SiteBoard &, const GridClass &, const std::vector<std::string> &tags, PlotStanding, bool formalFront, double streetJ, double streetI, Stream &, Board &out, Ledger &, std::string *error = nullptr);

// The front door: a contour edge, drawn against weights that prefer the middle
// of a straight wall and refuse a concave corner outright.
//
// `streetJ` and `streetI` point from the building toward the street in grid
// coordinates. An edge whose outside faces that way is worth three of one that
// does not, which is what puts the door on the front of the house rather than
// wherever the contour happened to offer the flattest wall. An edge whose
// outside is an obstacle is refused outright - a door may open onto a garden,
// and may not open into a rock.
uint32_t pickEntrance(const CellGrid &, const Chain &contour, const SiteBoard &, const Board &courts, double streetJ, double streetI, bool centred, Stream &);

// Phase 0 and phase 1 together: a framed lot with a footprint and a door on it.
bool planCellDwellingShell(const CellDwellingBrief &, const CellDwellingSettings &, CellHouse &, Ledger &, std::string *error = nullptr);

// ---------------------------------------------------------------------------
// Phase 2: the floor stack
// ---------------------------------------------------------------------------

// How many storeys a footprint of this size carries.
//
// Grows as the square root of the ground area, so a large plan gets a tall
// house and a cottage does not. The height tags skew it rather than setting it,
// which is why an explicit floor count clears them.
uint32_t randomFloorCount(const std::vector<std::string> &tags, uint32_t groundCells, uint32_t maximum, Stream &);

// Every floor above the ground, and the basement under it.
//
// Each upper floor starts from the one below and GIVES A ROOM AWAY - chosen as
// the one whose removal leaves the most compact remainder, and accepted with a
// probability that falls as the floor is already compact. That is the whole
// setback rule, and it is why these houses taper and drop wings as they rise
// instead of being extruded.
//
// The room it gives away comes from a coarse partition, not from the fine one:
// a setback is a massing decision and it is taken before any room has a door, a
// purpose or a neighbour.
bool planFloorStack(CellHouse &, const CellDwellingBrief &, const CellDwellingSettings &, Stream &, Ledger &, std::string *error = nullptr);

// ---------------------------------------------------------------------------
// Phase 3: the vertical scheme
// ---------------------------------------------------------------------------

// Which way this house is climbed, and where.
//
// Before any room exists, and that ordering is the point. Two of the three
// schemes are settled from the contour and the entrance alone, and one of them
// - the reserved stairwell - changes how the subdivision runs: the cell becomes
// its own room and the rest of the floor is cut around it. Planning rooms first
// and then hunting for somewhere to put a stair is what produces a stair in a
// corridor, and this ordering is what avoids it.
//
// `plannedFloors` must already be set.
bool rollStairs(CellHouse &, const std::vector<std::string> &tags, const CellDwellingSettings &, Stream &);

// Courts cut out of a footprint, and the footprint with them taken away.
//
// The reference plans (`docs/reference-plans.md`) are unanimous and they are
// what corrected this: a castle is a solid mass of ranges with courts cut out of
// it - off-centre, rectangular, of unequal size, sometimes open on one side -
// and NOT a range-thick ring round a middle. So nothing here draws a shape. The
// footprint is whatever the stencil grew, and a court is a rectangle of cells
// taken out of it, chosen by how well it is enclosed and how much it leaves.
//
// `out` is the remaining built ground and `courts` is what was removed. False
// when the class wants no court or none could be cut, which is not a failure:
// a building without a court is a building.
bool carveCourts(const CellGrid &,
                 const Board &area,
                 double share,
                 uint32_t wanted,
                 double streetJ,
                 double streetI,
                 Stream &,
                 Board &out,
                 Board &courts);

// A yard laid against the BACK of a footprint, on the lot's ground the house
// does not stand on: held by the house on two or three sides and open on the
// rest. Same construction as a court, opposite acceptance - a court must be
// enclosed, a yard must not be. The footprint is untouched; `yard` is the
// ground chosen. False when none could be laid, which is not a failure.
bool carveYard(const CellGrid &,
               const Board &area,
               const Board &buildable,
               double share,
               double streetJ,
               double streetI,
               Stream &,
               Board &yard);

// The arms of a cross, as rooms: nave, chancel and the transepts either side.
// False when the shape does not read as a cross, and the caller then cuts it the
// ordinary way.
// Where the bells go: one tower at the far end, or two at the front corners.
enum class BellTowers : uint8_t
{
  Back,
  TwinFront
};

bool partitionAxial(const CellGrid &, const Board &area, uint32_t entranceLanding, std::vector<Board> &pieces, std::vector<rewrite::RoomKind> &kinds, BellTowers towers = BellTowers::Back, const std::vector<uint32_t> *westFrontSlices = nullptr);

// Reserved circulation for a floor too large for corridors to emerge.
//
// Watabou has no circulation planner at all: corridors appear because the
// subdivision can be told to reward narrow pieces and the door pricing then
// routes traffic through them. On ten to thirty cells that is enough. Above a
// few times that it is not - recursive halving has no notion of a route from
// the door to the far wing - and the spine has to be laid before the rooms.
//
// Inert below the threshold, which is every house-sized class. It is the large
// classes that need it.
Board reserveCirculation(const CellHouse &, const CellFloor &, const CellDwellingSettings &, Ledger &);

// Whether a contour edge of this floor stands on an outside wall.
//
// Every loop, not just the first. A court has an outside wall too: the windows
// that look into a cloister are exterior windows, the walls around it are
// exterior walls, and asking only the outer loop silently turns a courtyard
// range into a building with a dark side.
bool exteriorEdge(const CellFloor &, uint32_t edge);

// ---------------------------------------------------------------------------
// Phase 4: the rooms
// ---------------------------------------------------------------------------

// Cuts every floor into rooms, honouring the stairwell reservation.
bool planRooms(CellHouse &, const CellDwellingSettings &, Stream &, Ledger &, std::string *error = nullptr);

// Phases 0 through 4: a framed lot, a footprint, a door, the storeys, the
// climb, and the rooms.
bool planCellDwelling(const CellDwellingBrief &, const CellDwellingSettings &, CellHouse &, Ledger &, std::string *error = nullptr);

// Milliseconds spent in each planning phase, when `DWELLING_TIMING` is set:
// shell, floors, rooms, doors, stairs, openings.
std::array<double, 6> &planningPhaseMs();

} // namespace procedural::world::generator
