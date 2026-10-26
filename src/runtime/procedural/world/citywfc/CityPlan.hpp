#pragma once

#include "procedural/world/citywfc/CityGrid.hpp"

#include <functional>

namespace procedural::world::citywfc {

// Labelling the grid: water, wall, gates, roads, bridges, blocks, and the
// vertical column each cell stands in.
//
// Nothing here creates geometry. The grid from milestone one is the only
// geometry there is, and every decision below is a set operation on its cells -
// which is the point of having built a cell complex rather than a pile of
// polygons. A wall is a set, so it is closed by construction. A road is a path,
// so "does this road cross that wall" is an intersection rather than a
// segment-segment test. A block is a connected component of what is left, so
// the classes are an exhaustive partition and a test can say so.
//
// It is also not a solver. Every label here is decided constructively, and that
// is deliberate: the constructive result is what the wave function collapse
// tile set will be validated against. If the city we can build by hand is not a
// legal assignment under the socket model, the socket model is wrong, and that
// is worth discovering before any search code exists.

enum class CellKind : uint8_t
{
  Unassigned, // Never a legal final state; rendered magenta so a gap is loud.
  Water,
  Court,      // Open ground enclosed by building: a yard, not a street.
  Ford,  // A crossing shallow enough to wade: water and road at once.
  Wall,
  Tower,
  Gate,
  Road,
  Bridge, // Road over water.
  Steps,  // Road between two columns whose ground differs by a layer.
  Plaza,
  Block,
  Count
};

const char *cellKindName(CellKind);

inline constexpr uint32_t kNoBlock = 0xFFFFFFFFu;
inline constexpr uint32_t kNoPlot = 0xFFFFFFFFu;
inline constexpr uint8_t kNoSide8 = 0xFFu;

struct CityCell
{
  CellKind kind = CellKind::Unassigned;

  // Which side of the cell the feature runs through: a gate's entry, a bridge's
  // direction of travel, the uphill side of a flight of steps. This is already
  // the socket index the solver will want, which is why it is recorded now
  // rather than derived later.
  uint8_t orientation = kNoSide8;

  uint8_t layerGround = 0;   // The vertical slot the ground sits in.
  uint16_t roadClass = 0;    // 0 none, 1 artery, 2 street, 3 alley.
  uint32_t block = kNoBlock;
  uint32_t plot = kNoPlot;   // The building plot this cell belongs to.
  float height = 0;          // Terrain at the centroid, metres.
};

struct CityPlot
{
  uint32_t id = 0;
  uint32_t block = kNoBlock;
  std::vector<uint32_t> cells;
  double areaMetres = 0;
  bool onStreet = false; // Touches the street network rather than only a court.
};

struct CityBlockInfo
{
  uint32_t id = 0;
  std::vector<uint32_t> cells;
  std::vector<uint32_t> frontage; // Cells of this block touching a road.
  double areaMetres = 0;
  bool landlocked = false;
  bool unreachable = false; // Routing to it failed; recorded, not retried.
};

// One named way through the city: the chain of cells it runs on, and the curve
// a mesher should follow along them.
//
// Both, because they answer different questions and an earlier version kept
// only the curve. It was recorded while routing and the thinning pass ran
// afterwards, so it described a network that no longer existed and the drawn
// line wandered off the road it was supposed to be on. The cells are rebuilt
// from the finished network and the curve from the cells, in that order.
struct CityRoute
{
  std::vector<uint32_t> cells;  // Edge-adjacent, every one of them road.
  std::vector<Position> line;   // Smoothed; for meshing only.
  uint32_t gate = 0;
  uint16_t roadClass = 1;
};

struct CityLabelInput
{
  // Where the city centres itself: the plaza, in the grid's own frame. The
  // walled core is grown from here.
  Position centre{};

  // Road exits, in the grid's frame. Gates are placed on the wall nearest each
  // of these, so the city's gates line up with the roads the world already has.
  // Empty spreads them by angle instead.
  std::vector<Position> exits;

  // True where the world says there is water, in the grid's frame. Cells whose
  // centroid satisfies it become Water before anything else is decided.
  std::function<bool(double, double)> water;

  // Ground height in metres, in the grid's frame.
  std::function<double(double, double)> terrain;

  // Purpose-addressed randomness. Every draw here is a hash of this, what is
  // being decided and the cell's own index - never the traversal order - so
  // permuting the work list cannot change the city.
  uint64_t seed = 1;
};

// Which way round the fabric is built.
//
// Two orders, and they produce different cities from the same grid.
//
// `Subdivide` is the top-down one: enclose a core, run arteries to it, and cut
// the land between them into blocks until each is small enough. It gives a
// legible street hierarchy and blocks that are all roughly the size you asked
// for, and every street is a decision someone made.
//
// `Accrete` is bottom-up and is how the places this is modelled on actually
// grew: put buildings down first, let them claim the ground around them, and
// whatever is left over is the street. Nobody lays out a lane between two
// houses; the lane is what the two houses did not take. It gives courtyards for
// free - a pocket of leftover ground with building all round it is a yard, not
// a street, and the only thing that distinguishes them is whether you can walk
// out of it.
enum class FabricMode : uint8_t
{
  Subdivide,
  Accrete
};

struct CityLabelSettings
{
  FabricMode fabric = FabricMode::Accrete;

  // Accretion. A plot is a seed plus the ground it claims; the spacing is in
  // cells and is what decides how much room is left over to walk in.
  // Spacing 3 leaves a town 39 % built and 58 % street, which is a car park.
  // Spacing 2 with plots of two to five cells lands near two thirds built,
  // which is what the places this is modelled on look like from above.
  uint32_t plotSpacingCells = 2;
  uint32_t plotCellsMinimum = 2;
  uint32_t plotCellsMaximum = 5;

  // A leftover pocket with no way out is a courtyard up to this size; anything
  // larger is opened to the street, because a hectare of unreachable ground in
  // the middle of a city is a mistake rather than a garden.
  double maximumCourtAreaMetres = 900;

  // A pocket this small is a gap between two houses, not a yard. Absorbed into
  // the street if it touches one and into a neighbouring plot if it does not;
  // without this the town is speckled with single-cell courts and reads as
  // noise rather than as blocks.
  uint32_t minimumCourtCells = 3;

  // Lay the through routes before any house is placed.
  //
  // Pure accretion gives no legible network - the streets are whatever the
  // houses left, which at this density is a maze. Real fabric grows along roads
  // that were already there, so the through routes go down first and the houses
  // accrete around them.
  bool primaryStreetsFirst = true;

  // Density falls off from the centre: the middle of a town is built solid and
  // the edge is not. Zero is uniform.
  double densityFalloff = .55;

  // How far from a building a cell may be and still count as street. Beyond it
  // the ground is countryside, not a road nobody built.
  uint32_t streetReachCells = 2;
  // The walled core, as a share of the grid. Cells are taken in order of travel
  // cost from the centre until the share is met, so the core follows the ground
  // rather than a circle.
  double walledAreaFraction = .55;

  // Off by default. The wall is a later decision than the fabric, and building
  // it first constrains where the fabric may go for no reason.
  bool wall = false;

  // Majority-filter passes on the core and the water before their boundaries
  // are taken. A rasterised set has a staircase edge and the wall, the bank and
  // the kerb all inherit it.
  uint32_t coreSmoothingPasses = 3;
  uint32_t waterSmoothingPasses = 2;

  uint32_t maximumGates = 6;
  double gateSpacingMetres = 70;
  double towerTurnDegrees = 50;
  double towerSpacingMetres = 34;

  // Routing. `reuseFactor` below one is what turns independent shortest paths
  // into a network; `turnWeight` is what keeps them straight.
  // The turn penalty saturates: at 20 the mean turn along a road is 22.0
  // degrees, at 80 it is 20.2, and at 300 it is still 20.2. That floor is the
  // grid's own angular noise between adjacent cell centroids, not a routing
  // failure, and no cost function reaches below it. 80 is where it lands, and
  // it also routes fewer cells to get there.
  double reuseFactor = .35;
  double turnWeight = 80;
  double gradeWeight = 40;
  double maximumGrade = .22;
  double bridgeCost = 120;

  // What it costs an alley to cut a new opening through the wall. High enough
  // that it only happens when a pocket of the city is sealed off with no other
  // way in, which is what a postern is for.
  double posternCost = 400;
  double fordDepthMetres = .6;
  uint32_t maximumBridgeCells = 6;
  uint32_t arteryDilation = 0;
  uint32_t maximumRouteExpansions = 400000;

  double plazaRadiusMetres = 18;
  // Bisection lands a block between half the maximum and the maximum, so the
  // average comes out near 0.6 of this. 3200 gives blocks of roughly 1900 m2,
  // which is a burgage row either side of a back lane.
  double maximumBlockAreaMetres = 6000;

  // A block narrower than this is already a row of houses and cutting it again
  // only makes two shorter rows and another street. Without it the splitter
  // grinds every block down to one-cell strips and paves the town.
  double minimumBlockWidthMetres = 20;

  uint32_t maximumAlleys = 400;

  double layerHeightMetres = 3;

  // Corner-cutting passes on each feature outline. Two rounds turn a staircase
  // into a curve and move the line by at most a quarter of a cell; zero keeps
  // the raw cell boundary.
  uint32_t outlineSmoothingPasses = 2;
  bool smoothOutlines = true;

  // Corner cutting on a route's centreline. Chaikin's limit is a quadratic
  // B-spline, so three passes turn the right angles a cell chain makes into
  // curves while staying inside the corridor the cells describe.
  uint32_t routeSmoothingPasses = 3;
};

struct CityLabelMetrics
{
  uint32_t cells = 0, water = 0, wall = 0, towers = 0, gates = 0;
  uint32_t road = 0, bridge = 0, ford = 0, steps = 0, plaza = 0, block = 0;
  uint32_t unassigned = 0, blocks = 0, landlocked = 0, alleys = 0;
  uint32_t court = 0, plots = 0, openedCourts = 0, plotless = 0;

  // Blocks nothing could route to. Counted apart from `landlocked` because the
  // router clears that flag when it gives up, so reporting only `landlocked`
  // says zero while the ledger says two.
  uint32_t unreachable = 0;
  uint32_t droppedGates = 0;
  double roadLengthMetres = 0;

  // The mean turn at a road cell with exactly two road neighbours, in degrees.
  // Zero is a straight line; a staircase through an irregular grid runs to
  // thirty or more. The number to tune the turn penalty against, because
  // "smoother" judged by eye is not a number anyone can regress against.
  double meanRoadTurnDegrees = 0;
  double meanWallTurnDegrees = 0;
  double minimumPassageMetres = 0;
  uint8_t maximumLayer = 0;
};

// The smoothed edge of a feature, for meshing.
//
// A cell boundary is a staircase of quad edges, and a wall, a bank or a kerb
// built straight along it inherits every ninety-degree step in the grid. The
// cells stay authoritative - every question about topology is still asked of
// them - and this is the curve the geometry is built on. Keeping the two apart
// is the same separation as the road centreline: one answers "what is this",
// the other answers "where exactly does it run".
enum class OutlineRole : uint8_t
{
  Wall,
  Water,
  Road,
  Block
};

struct CityOutline
{
  OutlineRole role = OutlineRole::Wall;
  uint32_t feature = 0;          // Block id for Block, zero otherwise.
  std::vector<Position> loop;    // Closed: the last point joins the first.
  double rawLengthMetres = 0;    // Before smoothing, for the ledger and tests.
};

struct CityPlan
{
  std::vector<CityCell> cells; // Indexed by grid face.
  std::vector<CityBlockInfo> blocks;
  std::vector<CityPlot> plots;
  std::vector<CityRoute> routes; // Gate to plaza. The cells are the truth.
  std::vector<CityOutline> outlines;              // Smoothed feature edges, likewise.
  std::vector<uint32_t> gates;                    // Cell indices.
  CityLabelMetrics metrics;
  std::vector<std::string> ledger;
};

// Labels the grid. `grid` is not modified.
//
// Named apart from the existing `CityLabelSettings` and `generateCityMasterPlan`
// on purpose: that is the district allocator of the other city path, and two
// types called the same thing in the same namespace tree is how a caller ends
// up configuring one and running the other.
//
// Atomic on a budget failure. An ordinary shortfall - a gate that could not be
// routed, a channel narrower than a cell, a block that could not be reached -
// is a ledger entry and the city still stands.
bool labelCity(const CityGrid &grid, const CityLabelInput &input, const CityLabelSettings &settings, CityPlan &out, std::string *error = nullptr);

} // namespace procedural::world::citywfc
