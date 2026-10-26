#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>
#include <vector>

namespace procedural::world
{

struct RiverSettings
{
  // Depth of runoff per year, in metres. Discharge is drainage area times this,
  // so rivers come out in cumecs rather than in arbitrary units - 0.3 m/yr is a
  // temperate catchment. This is the wet end; `runoffScale` scales it per cell
  // from the climate's moisture field, which is what makes a desert riverless
  // rather than merely dry.
  double runoffMetresPerYear = 0.3;

  // Runoff a cell yields at zero moisture, as a fraction of the above. Not
  // zero: a desert does still shed water in a storm, and a wadi that carries a
  // trunk river through it - the Nile - is a real thing that a hard zero would
  // forbid.
  double aridRunoffFraction = 0.04;

  // How much catchment a channel needs before it is drawn as a river, measured
  // in multiples of one cell's own area. This is the map's channel-initiation
  // threshold and it is what sets drainage density.
  //
  // 5 drew a channel through roughly one cell in ten, which on a country-sized
  // map is a blue net over everything and nothing else is visible. Real maps
  // draw the rivers you could navigate.
  double riverThresholdCells = 35.0;

  // Channel heads are selected with a stream-power proxy rather than drainage
  // area alone: discharge * (slope/referenceSlope)^slopeExponent. Steep wet
  // uplands can therefore start small tributaries, while broad low-gradient
  // drainage does not paint parallel blue lines across every plain. Once a
  // head starts, its complete downstream course is retained.
  double channelReferenceGradient = 0.008;
  double channelSlopeExponent = 0.35;

  // Discard independent coastal trickles shorter than this many cells. This
  // prunes one-segment mouths without deleting small tributaries that join a
  // larger drainage system.
  uint32_t minimumRiverSystemCells = 2u;

  // Slope imposed across a filled depression, in metres per cell. Depression
  // filling makes a basin flat, and a flat basin has no flow direction; this
  // tilts it just enough that every cell still has somewhere to send water.
  double fillEpsilonMetres = 0.01;

  // Gradient along a channel above which the reach is marked as falls, and the
  // share of the map's largest discharge needed before it is worth marking. A
  // knickpoint on a trickle is a trickle over a rock.
  //
  // Be clear about what this can mean at map resolution: a waterfall is a
  // sub-cell feature, and with 12 km cells the gradient between two channel
  // cells is an average over 12 km, not a drop. Measured on the default world,
  // channel gradients run median 0.0074, p90 0.016 and max 0.025 - so the
  // first threshold tried, 0.045, was above every gradient on the map and found
  // nothing. This marks the *steepest reaches*, where falls are, rather than
  // individual falls.
  double waterfallGradient = 0.015;
  double waterfallMinimumDropMetres = 25.0;
  double waterfallDischargeFraction = 0.03;
  uint32_t waterfallSeparationCells = 2u;

  // A river cell becomes a canyon only where its surrounding banks rise by a
  // meaningful share of the world's relief and at a steep physical gradient.
  // Gentler incised channels remain valleys.
  double canyonMinimumBankRiseFraction = 0.01;
  double canyonMinimumBankGradient = 0.01;
};

struct LakeSettings
{
  // Minimum water depth above the depression floor.
  double minimumDepthMetres = 20.0;
  // A basin must be wet and receive this share of the map's largest discharge.
  double minimumMoisture = 0.35;
  double minimumInflowFraction = 0.006;
  // Lakes are selected as connected deep cores of a filled basin. These two
  // area gates stop a large shallow depression from flooding wholesale: the
  // tighter of the absolute cap and catchment share wins. Zero disables a cap.
  double maximumAreaSquareMetres = 450.0e6;
  double maximumCatchmentFraction = 0.08;
};

// A knickpoint: where a channel crosses a step it has not yet cut down.
struct Waterfall
{
  Point2 position;
  double dropMetres = 0.0;
  double gradient = 0.0;
  double dischargeCumecs = 0.0;
  uint32_t node = kInvalidNode;
};

struct RiverNode
{
  // Cell this one drains into. kInvalidNode for a sink: water, or a cell with
  // nowhere lower to go, which after depression filling means an outlet.
  uint32_t downstream = kInvalidNode;

  // Elevation after depression filling. Never below the cell's own elevation,
  // and strictly above its downstream neighbour's.
  double filledElevationMetres = 0.0;

  double drainageAreaSquareMetres = 0.0;
  double dischargeCumecs = 0.0;

  // Strahler order. 1 for a headwater channel; increments only where two
  // channels of equal order meet, which is what makes it a hierarchy rather
  // than a hop count.
  uint32_t strahlerOrder = 0u;

  bool river = false;
};

// One drawn length of channel, from a cell to the cell it drains into.
struct RiverSegment
{
  Point2 from;
  Point2 to;
  double dischargeCumecs = 0.0;
  uint32_t strahlerOrder = 0u;
  uint32_t node = kInvalidNode;
  // The segment ends at water: this is a river mouth.
  bool mouth = false;
};

struct RiverNetwork
{
  // Indexed by node id, so a caller can go straight from a picked cell to its
  // river data without a search.
  std::vector<RiverNode> nodes;
  std::vector<RiverSegment> segments;
  std::vector<Waterfall> waterfalls;

  uint32_t riverCells = 0u;
  uint32_t mouths = 0u;
  uint32_t filledCells = 0u;
  uint32_t maximumOrder = 0u;
  double largestDischargeCumecs = 0.0;
};

// Stage 6: the drainage network.
//
// Priority-flood depression filling first, then steepest descent on the filled
// surface. Filling is what makes the gate - every river descends and reaches
// water - a property of the algorithm rather than something to be checked for
// afterwards: the flood guarantees each cell a strictly lower neighbour, so
// following the descent cannot cycle and cannot dead-end.
//
// Reclassifies the land cells a river runs through as Valley where the
// surrounding cells stand meaningfully above them, which is the difference
// between a river crossing a plain and a river in a valley.
// `runoffScale`, if given, is a per-cell multiplier on the runoff - the
// climate's moisture field, normally. Empty means uniform, which is what the
// stage did before a climate existed.
//
// Note the ordering this implies: the climate has to run *before* the rivers,
// because precipitation is what drives them. The riparian moisture that runs
// the other way is a second, local pass.
bool buildRivers(
    const RiverSettings &settings,
    MapGraph &graph,
    RiverNetwork &network,
    const std::vector<double> &runoffScale = {},
    std::string *error = nullptr);

// Turns filled depressions into area-limited lakes.
//
// The flood already knows where they are: a cell whose filled elevation stands
// above its own is under water, and how far above is how deep. Using that is
// both free and correct - a lake is a basin that filled to its spill point.
// Candidate cells are grouped into whole basins, then a connected footprint is
// grown outward from the deepest floor under physical area and catchment-share
// caps. Broad shallow depressions therefore do not flood wholesale.
//
// Separate from buildRivers so the drainage network's invariants - mass
// balance, every course reaching water - are established before the map is
// changed under them. Returns the number of cells flooded.
// `moisture` is the climate stage's field, indexed by node id, or empty to skip
// the humidity test. A basin only holds a lake if the climate can keep it
// filled: the same depression in a desert is a salt flat, and one with nothing
// draining into it is a dry hollow. Both were becoming lakes.
uint32_t fillLakes(
    const RiverNetwork &network,
    const LakeSettings &settings,
    const std::vector<double> &moisture,
    MapGraph &graph);

} // namespace procedural::world
