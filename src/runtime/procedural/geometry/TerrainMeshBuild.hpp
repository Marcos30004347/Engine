#pragma once

#include "procedural/geometry/TerrainMesh.hpp"
#include "procedural/world/WorldPipeline.hpp"

#include <functional>
#include <string>
#include <vector>

namespace procedural::geometry
{

// Multiplies the vertical tolerance at a point. Below one refines harder than
// the error alone would ask, which is how a valley floor or a ridge crest gets
// the vertices an eye reads a landscape by while a big smooth flank does not.
using ToleranceFunction = std::function<double(Point2)>;

struct TerrainMeshProgress
{
  const char *stage = "meshing";
  uint32_t completedTiles = 0, totalTiles = 0, activeWorkers = 0, workerCount = 0;
  uint64_t triangles = 0;
  double elapsedSeconds = 0;
};

struct TerrainMeshSettings
{
  // Build tile. Independent of the Voronoi partition; this is the unit of
  // parallelism, of storage, and of the border contract.
  double tileSizeMetres = 4000.0;

  // Error-driven refinement. A point is inserted only where the piecewise
  // linear surface departs from the sampled one by more than this, which is
  // what makes flat ground nearly free: a plain converges with no interior
  // points at all.
  double verticalToleranceMetres = 6.0;
  uint32_t maximumRefinementRounds = 6u;
  // Refuse to split a triangle smaller than this however large its error. The
  // stop that keeps a cliff in the source data from refining without bound.
  double minimumEdgeMetres = 8.0;

  // Chain resampling. Zero derives a spacing from the tile size.
  double chainSpacingMetres = 0.0;
  // Border sampling. Must be a function of position only - see the border
  // contract - so it is one number for the whole world, not per tile.
  double borderSpacingMetres = 0.0;

  // Seed each tile with Poisson samples before error refinement. Zero disables.
  double poissonSpacingMetres = 0.0;
  uint64_t poissonSeed = 0u;
  uint32_t poissonLayers = 3u;
  // Relocate only free samples, before spacing/constraint clearance checks.
  std::function<Point2(Point2, double)> poissonPlacement;
  // Traced raster polylines already encode shape; retain their corners.
  bool preserveChainVertices = false;
  // Subdivide long constraint spans without changing their plan geometry.
  // Needed when a region edge crosses many texels of a sampled heightfield.
  double maximumConstraintSegmentMetres = 0.0;

  // 0 interpolates between contours; 1 makes each band a flat shelf. Anything
  // between lerps, which is the dial for how N64 the result looks.
  double terraceStrength = 0.0;
  double terraceIntervalMetres = 0.0;

  // Delaunay refinement bound. Zero disables the quality pass entirely; the
  // error criterion measures height and is blind to shape, so without this a
  // pair of close constraint chains leaves a ladder of slivers.
  double minimumAngleDegrees = 0.0;
  uint32_t maximumQualityRounds = 3u;

  // A candidate point closer to a chain than this fraction of the chain
  // spacing is projected onto it rather than inserted beside it: a vertex just
  // off a ridge is worse than no vertex, because it samples below the crest.
  double snapFraction = 0.4;

  // Carve the map's drainage network into the surface.
  bool carveRivers = true;
  // Rock detail on cliff edges.
  bool beautifyEdges = true;

  // Optional per-point tolerance scale. Empty means uniform.
  ToleranceFunction toleranceScale;
  // Largest triangle allowed at a point, in square metres; zero means no limit.
  //
  // Refinement is otherwise driven purely by vertical error, so ground that is
  // flat is never subdivided however large the triangles get - which is correct
  // for fidelity and wrong for everything else. A farm patch genuinely is flat
  // and genuinely does not need geometry; a river bend is flat across its width
  // and needs a great deal, because what has to be resolved there is the plan
  // shape, not the height. This is the knob that says so, and it is what lets
  // each region carry its own triangle density.
  std::function<double(Point2)> maximumTriangleArea;

  // 0 selects the hardware concurrency. Tiles are independent, so this changes
  // wall clock and must not change a single byte of output.
  uint32_t workerCount = 0u;
  // Stable spatial filtering; false is a reference path for parity benchmarks.
  bool indexConstraints = true;
  // Serialized heartbeat callbacks, including builds that retain tiles.
  // Throwing aborts the build after active workers finish.
  std::function<void(const TerrainMeshProgress &)> progress;

  // Invoked once per complete tile, potentially concurrently. A false return
  // aborts the build; callers publish their archive manifest only on success.
  std::function<bool(MeshTile &, std::string &)> tileSink;
  bool retainTiles = true;
  // Conservative local vertex removal: retain constraints and tile seams,
  // and validate replacement triangles against sampled source heights/materials.
  uint32_t simplificationRounds = 0;
  double simplificationToleranceMetres = 1.0;
  uint32_t uniformSubdivisionRounds = 0; // conforming refinement after CDT/Poisson
  uint64_t maximumTriangleCount = 0; // zero means unlimited; never truncate tiles

};

// Height and material of the authored surface at a point, in world metres.
using HeightFunction = std::function<double(Point2)>;
using MaterialFunction = std::function<uint8_t(Point2)>;

// Strokes: rivers, and later roads.
//
// A stroke is a centreline expanded into a corridor whose cross-section is
// *level* - both banks of a section at the same elevation, always, because a
// river's water surface is horizontal across its width. Offsetting along the
// horizontal normal rather than the surface normal is what makes that true;
// draping a corridor over a sampled field gives a groove cut into a tilted
// hillside instead.
struct StrokeSettings
{
  // Downstream hydraulic geometry (Leopold & Maddock): w = a Q^0.5,
  // d = c Q^0.4. A 5 cumec headwater comes out 5.6 m wide and 0.6 m deep, a
  // 2000 cumec trunk 112 m and 6.3 m - and both arrive from the map's own
  // hydrology with no new tuning surface.
  double widthCoefficient = 2.5;
  double depthCoefficient = 0.30;
  double minimumWidthMetres = 6.0;
  double minimumDepthMetres = 0.6;

  // Flat reaches. A river runs as pools separated by short drops rather than
  // as a smooth ramp, and quantising the water surface is what makes a lake
  // simply a very wide reach that a river meets exactly.
  double reachIntervalMetres = 4.0;

  // Meander amplitude as a multiple of channel width; the empirical wavelength
  // is ten to fourteen widths.
  double meanderScale = 1.6;

  // Bank height below which no face is cut: a channel crossing a flat plain is
  // not incised and a step there would be a fence.
  double minimumBankMetres = 0.8;

  // Radius of the disc that fills a confluence, as a multiple of the
  // downstream channel's half width.
  double junctionScale = 1.4;

  double spacingMetres = 90.0;
  uint8_t bankMaterial = 10u;
  uint64_t seed = 0u;

  double widthMetres(double dischargeCumecs) const;
  double depthMetres(double dischargeCumecs) const;
};

// The river network, resolved before any geometry exists.
//
// Topology first, geometry second. Building a corridor per reach and patching
// the seams afterwards is what makes confluences overlap: the reaches never
// know about each other, so nothing can decide how three of them should meet.
// Resolving the network first means a junction is a *node* that knows every
// reach incident on it and how wide each arrives, and the geometry step is then
// a straightforward emission with nothing left to negotiate.
struct ChannelVertex
{
  Point2 position;
  double dischargeCumecs = 0.0;
  double widthMetres = 0.0;
  double depthMetres = 0.0;
  // Water surface, already quantised to flat reaches and already monotone.
  double surfaceMetres = 0.0;
  double bedMetres = 0.0;
};

// One length of channel, headwater-or-junction to junction-or-mouth. Ends are
// node indices into ChannelNetwork::nodes, or kInvalidChannelNode at a headwater or
// a mouth.
constexpr uint32_t kInvalidChannelNode = 0xffffffffu;

struct ChannelReach
{
  std::vector<ChannelVertex> vertices;
  uint32_t upstreamNode = kInvalidChannelNode;
  uint32_t downstreamNode = kInvalidChannelNode;
  uint32_t mapCellCount = 0u;
};

// A confluence. Every reach that arrives, and the merged channel that leaves.
struct ChannelNode
{
  Point2 position;
  double surfaceMetres = 0.0;
  double bedMetres = 0.0;
  // Outgoing width: the merged channel is sized by the summed discharge, which
  // is why a trunk is wider than either tributary rather than as wide as the
  // larger one.
  double widthMetres = 0.0;
  std::vector<uint32_t> incoming;
  uint32_t outgoing = kInvalidChannelNode;
};

struct ChannelNetwork
{
  std::vector<ChannelReach> reaches;
  std::vector<ChannelNode> nodes;
  uint32_t carvedCells = 0u;
  uint32_t duplicateCells = 0u;
};

// Phase one: resolve the map's drainage into reaches and junction nodes, with
// every width, depth and water surface already decided.
ChannelNetwork resolveChannelNetwork(
    const world::GeneratedWorld &world, const StrokeSettings &settings);

struct StrokeGeometry
{
  std::vector<Chain> chains;
  std::vector<StepQuad> quads;
  uint32_t courseCount = 0u;
  uint32_t junctionCount = 0u;
  // Channel cells visited, and how many were visited twice. Splitting reaches
  // at confluences is what drives the second number to zero: walking every
  // headwater to the sea instead carves each shared trunk once per tributary.
  uint32_t carvedCells = 0u;
  uint32_t duplicateCells = 0u;
  double channelLengthMetres = 0.0;
  double maximumWidthMetres = 0.0;
  double maximumDepthMetres = 0.0;
};

// What the geometry step needs to know about the ground the channel is cut
// into, independent of which generator produced it. Decoupling this is what
// lets the analytic map and the raster source share one carving path instead
// of growing two that drift.
struct StrokeGround
{
  HeightFunction height;
  std::function<world::Biome(Point2)> cover;
};

// Phase two: emit corridors and junction polygons from the resolved network.
StrokeGeometry buildStrokeGeometry(
    const ChannelNetwork &network,
    const StrokeGround &ground,
    const StrokeSettings &settings);

StrokeGeometry buildStrokeGeometry(
    const ChannelNetwork &network,
    const world::GeneratedWorld &world,
    const StrokeSettings &settings);

// Both phases, for callers that do not need the network itself.
StrokeGeometry buildStrokes(
    const world::GeneratedWorld &world, const StrokeSettings &settings);

// Ramps, and the graph they make.
//
// A ramp is not a new primitive. A cliff face's gradient is riser/setback, so
// widening the setback locally until that falls under the walkable bound *is*
// the ramp - the outward spur of the plan, expressed as a modulation of the
// step rather than as separate geometry. That is why it costs almost nothing
// and why it cannot crack: it is the same ruled strip, wider in one place.
struct RampSettings
{
  // The bound that makes a face walkable. 0.35 is about 19 degrees.
  double maximumWalkableGradient = 0.35;
  // Ramps per block boundary run, and how much of the run one occupies.
  uint32_t rampsPerRun = 1u;
  double rampWidthMetres = 220.0;
  uint64_t seed = 0u;
};

// Edge detail. Applied to the brink and toe after they are traced and before
// they reach the triangulator, so the detail is *in* the mesh rather than
// displaced on top of it.
struct EdgeDetailSettings
{
  // Displacement is a smooth vector field evaluated at the point's own
  // position, never a per-chain random walk. That is what removes the
  // agreement problem instead of solving it: every copy of a shared vertex
  // moves to the same place, whoever is asking, so polygons stay closed and
  // neighbours stay flush with no canonicalisation at all.
  double warpAmplitudeMetres = 14.0;
  double warpWavelengthMetres = 260.0;
  uint32_t warpOctaves = 3u;
  // Vertical jitter on the brink, as a fraction of the riser, to break the
  // perfect isoline. The toe gets less: a talus base is smoother than a rim.
  double brinkJitterFraction = 0.06;
  double toeJitterFraction = 0.02;
  // Buttresses push the toe out and notches pull it in, both as fractions of
  // the local setback. These are what stop a face reading as extruded.
  double featureStrength = 0.45;
  double featureWavelengthMetres = 420.0;
  uint64_t seed = 0u;
};

struct CliffSettings
{
  EdgeDetailSettings detail{};
  bool beautify = true;
  // A drop under this is not a cliff, it is a join: the two terraces are one
  // surface and are walkable between.
  double flushJoinMetres = 1.0;

  RampSettings ramps{};
  bool cutRamps = true;
  // Multiplies the tabled talus angle's setback. One is the rock's own repose.
  double setbackScale = 1.0;
  // Clamp, as a fraction of the local cell size, so an offset cannot fold
  // through the cell beyond it at a concave corner.
  double maximumSetbackFraction = 0.35;
  // Resampling along the brink; the toe takes the same parameterisation.
  double spacingMetres = 60.0;
  // Rock exposed on the face, and the cover the top keeps.
  uint8_t faceMaterial = 11u;
  uint64_t seed = 0u;
};

struct CliffGeometry
{
  TerraceGraph terraces;
  std::vector<Chain> chains;
  std::vector<StepQuad> quads;
  uint32_t brinkCount = 0u;
  double totalBrinkLengthMetres = 0.0;
  double meanRiserMetres = 0.0;
  double meanSetbackMetres = 0.0;
};

// Traces the boundary of every plateau block and turns it into brink and toe
// chains plus the face between them.
CliffGeometry buildCliffs(
    const world::GeneratedWorld &world, const CliffSettings &settings);

// Every semantic line of a generated world, as constraint chains: the world
// border, the coastline and lake shores, the elevation contours, and the
// region, biome and landform borders. These already exist in the map and have
// never had a three-dimensional consumer.
std::vector<Chain> gatherWorldChains(
    const world::GeneratedWorld &world, double weldToleranceMetres = 0.0);

// Builds the tiled mesh.
//
// Deterministic in (chains, height, material, settings) alone: tiles are pure
// functions of their own constraints and their borders, borders are a pure
// function of position, and no stage carries a shared random stream. Building
// with one worker and with sixteen must produce identical bytes.
bool buildTerrainMesh(
    const std::vector<Chain> &chains,
    const std::vector<StepQuad> &steps,
    const HeightFunction &height,
    const MaterialFunction &material,
    double extentMetres,
    const TerrainMeshSettings &settings,
    TerrainMesh &mesh,
    std::string *error = nullptr);

inline bool buildTerrainMesh(
    const std::vector<Chain> &chains,
    const HeightFunction &height,
    const MaterialFunction &material,
    const double extentMetres,
    const TerrainMeshSettings &settings,
    TerrainMesh &mesh,
    std::string *error = nullptr)
{
  return buildTerrainMesh(
      chains, {}, height, material, extentMetres, settings, mesh, error);
}

// Convenience path: wires the height and material functions to a generated
// world's own surface and graph.
bool buildTerrainMesh(
    const world::GeneratedWorld &world,
    const TerrainMeshSettings &settings,
    TerrainMesh &mesh,
    std::string *error = nullptr);

} // namespace procedural::geometry
