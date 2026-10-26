#pragma once

#include "procedural/geometry/TerrainMeshBuild.hpp"
#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapEdges.hpp"
#include "procedural/geometry/TerrainHydraulicErosion.hpp"
#include "procedural/geometry/TerrainRidges.hpp"
#include <map>
#include "procedural/geometry/TerrainRiverSurface.hpp"
#include "procedural/geometry/TerrainLandforms.hpp"
#include "procedural/geometry/TerrainDetailField.hpp"
#include "procedural/geometry/TerrainParcels.hpp"
#include "procedural/geometry/TerrainSettlements.hpp"
#include "procedural/geometry/TerrainStreamPower.hpp"

namespace procedural::geometry
{
// One continuous river, from its head or the confluence above it down to the
// next confluence or the sea, with the half-width at every vertex. Consumers
// that stroke a river - the map renderer, the surface mesh, placement - need
// the run, not a bag of segments: a segment list cannot be stroked without
// reassembling it, and reassembling it is what produced the broken rivers.
struct RiverReach
{
  std::vector<Point2> path;
  std::vector<double> width;      // half-width in metres, per path vertex
  double dischargeCumecs = 0.0;
  bool reachesWater = false;      // ends in the sea or a lake rather than inland
};

// A contour scarp: a line cut across the ground after the solver has run.
//
// The solver works at 250 m cells and a cliff face is smaller than one of them,
// so it can neither create nor erode one - measured three times, see the plan.
// These are cut into the finished surface instead, along a contour so the scarp
// inherits the drainage already there, and then weathered at the angle of
// repose, which is the one process that acts at the scale a cliff exists at.
struct ScarpSettings
{
  bool enabled = true;
  uint32_t count = 48u;              // attempted; siting can refuse a candidate
  double lengthMetres = 9000.0;      // before the per-scarp random scaling
  double faceMetres = 190.0;         // horizontal run of the face itself
  double dropMetres = 110.0;         // height lost across the face
  double runoutMetres = 620.0;       // how far the foot slope reaches out
  double taper = .22;                // fraction of the line spent dying at each end
  double wander = .35;               // lateral wander, as a fraction of the face
  double wanderScaleMetres = 2600.0;
  // The ground a scarp will take: too flat and there is nothing to cut into,
  // too steep and it is already a mountainside.
  double minimumSlope = .02;
  double maximumSlope = .30;
  double resistance = 1.6;           // caprock hardness multiplier along the brink
  // Angle-of-repose weathering applied to the cut face afterwards.
  uint32_t weatherRounds = 24u;
  double weatherRate = .5;
  double reposeDegrees = 34.0;
};

// Largest triangle the mesh may carry over each kind of ground, in square
// metres. Density is a property of what is there, not of the camera: a river
// bend and a road edge carry shape at a few metres, and an open moor does not.
// Zero means "no limit from this rule", which is what the wild should be.
struct MeshDensitySettings
{
  bool enabled = true;
  double river = 260.0;
  double shore = 420.0;
  double road = 340.0;
  double settlement = 900.0;
  double farmland = 14000.0;
  double wild = 0.0;
};

// Which region boundaries are extracted as contours and cut into the partition
// before anything is triangulated.
//
// A region whose only definition is a test at a sample point cannot have a
// clean edge: the mesher gives a face the material at its centroid, so where a
// boundary is not a constraint, triangles straddle it and the edge degenerates
// into a zigzag. That is the whole of the triangular-tongue artefact, and the
// two regions that showed it - the sand band and the biome boundaries - were
// exactly the two defined by a sample test rather than by a polygon.
struct RegionContourOptions
{
  // The sand band along the coast, marched on the shore distance itself so the
  // edge follows the true contour rather than a half-cell lattice.
  bool shoreBand = true;
  uint32_t shoreResolution = 3072u;
  double shoreSimplifyMetres = 18.0;
  double shoreBandMetres = 95.0;
  // Boundaries between biomes. Categorical, so the crossings sit at edge
  // midpoints - there is no sense in which a point is 40% into a desert.
  bool biomes = true;
  uint32_t biomeResolution = 2048u;
  double biomeSimplifyMetres = 40.0;
  double biomeMinimumPerimeterMetres = 1200.0;
};

// Import the voxel world-map package in physical, cell-centred coordinates.
// Raw refined elevation remains available for diagnostics; clean base relief
// drives ridge, hydraulic, shore and river construction before CDT extraction.
class TerrainMapSource
{
public:
  bool guidedGeometry = true;
  TerrainRiverSurface rivers;
  LandformSettings landforms;
  SplineLandforms rockFormations;
  HydraulicErosionSettings hydraulics;
  StreamPowerSettings streamPower;
  StreamPowerReport streamPowerReport;
  SettlementSettings settlements;
  ParcelSettings parcels;
  DetailSettings detail;
  ScarpSettings scarps;
  RegionContourOptions regionContours;
  MeshDensitySettings meshDensity;
  double horizontalScale = 1.0;
  bool variedBiomes = false;
  HydraulicErosionReport erosion;
  MountainSkeleton mountains;
  TerrainDetailField detailField;
  // The results of the plan stages, kept so a consumer can read the network and
  // the partition rather than re-deriving them from the mesh.
  SettlementNetwork settlementNetwork;
  ParcelLayer parcelLayer;
  std::vector<RiverReach> riverReaches;
  bool load(const std::string &directory, std::string *error = nullptr);
  bool initialize(const world::MapExportBundle &bundle, std::string *error = nullptr);
  double extentMetres = 0.0;
  uint32_t resolution = 0;
  uint64_t seed = 0;
  std::string elevationField, constructionField;
  double minimumElevation = 0.0, maximumElevation = 0.0;
  double maximumMountainDetailMetres = 0.0;
  double seaLevelMetres = 0.0;
  // A river reads by its valley, not by its channel: the channel itself is
  // 0.6-2 m of water, which is nothing at a 4 m mesh tolerance. This is the
  // incision at 50 cumecs, growing as the 0.35 power of discharge and capped so
  // a trunk river does not saw a canyon.
  double riverValleyMetres = 18.0;
  double riverValleyMaximumMetres = 55.0;
  // Region queries are displaced by noise so a cell boundary is not a straight
  // chord between two sites. At half a map cell the coastline broke into
  // fragments, so the amplitude is capped at a fraction of the mean cell.
  double regionWarpMetres = 150.0;
  double regionWarpCellFraction = .21;

  // Ceiling on the imported field's relief, as a fraction of the world's width.
  // 0.035 is 1.7 km on a 48 km island and 9 km on a 256 km continent, so it
  // only ever bites on worlds small enough for continental relief to be absurd.
  double reliefCeilingFraction = .035;
  // How far a natural boundary - a wood edge, rough ground, pasture - fades
  // into what it borders. Worked and made ground ignores this and keeps its
  // colour exactly to its own edge.
  double naturalBlendMetres = 34.0;
  double regionWarpScaleMetres = 900.0;
  double cellMetres() const { return extentMetres / resolution; }
  double groundHeight(Point2 point) const;
  // `landKnown` lets a caller that has already resolved `waterType(point)`
  // to dry land skip the second resolve. That test walks the region
  // polygons, and `height` pays for it on every land sample otherwise.
  double coastalHeight(Point2 point, bool landKnown = false) const;
  double height(Point2 point) const;
  double waterLevel(Point2 point) const;
  uint8_t waterType(Point2 point) const;
  uint8_t biome(Point2 point) const;
  uint8_t landform(Point2 point) const;
  // Distance to the nearest shoreline. Public because every consumer that has
  // to draw a shore - the parcel layer's sand band, the chunk map, the contour
  // extraction - needs the same number the surface colouring uses.
  double coastDistance(Point2 point) const;
  // Largest triangle the mesh should carry at a point, by what is there: dense
  // where the plan shape matters and coarse where it does not.
  double maximumTriangleArea(Point2 point) const;
  double toleranceScale(Point2 point) const;
  std::array<float, 3> surfaceColour(Point2 point, const MaterialEntry &material) const;
  MaterialFunction materialFunction(MaterialPalette &palette) const;
  void flattenOcean(TerrainMesh &mesh) const;
  // Attach authoritative vector topology to decoded fields before meshing.
  void setGeometry(world::MapGraph graph, world::MapExportFeatures features);
  bool hasVectorRegions() const { return graph_.nodeCount() != 0; }
  std::vector<Chain> constraintChains() const;
  size_t contourCount() const { return features_.contours.lines.size(); }
  size_t riverCount() const { return features_.rivers.segments.size(); }

private:
  size_t index(Point2 point) const;
  double linear(const std::vector<double> &values, Point2 point) const;
  uint32_t regionNode(Point2 point) const;
  // Displacement applied to a region query, and its inverse-to-first-order
  // applied to the chains derived from the same partition so the two agree.
  Point2 regionWarp(Point2 point) const;
  void constructScarps();
  // Re-steepens the scarp faces after the solver has run. See the comment on
  // the definition: the solver's grid cannot represent a cliff, so the cut has
  // to be broad going in and sharpened coming out.
  void sharpenScarps();
  double regionWarpReach() const;
  static constexpr uint32_t kRegionWarpOctaves = 3u;
  size_t bucket(Point2 point) const;
  void addToBuckets(std::vector<std::vector<uint32_t>> &buckets, Point2 a, Point2 b, double radius, uint32_t id);

  void constructMountainRelief();
  double moistureAt(Point2 p) const;
  void evolveLandscape();
  void constructRockFormations();
  // Settlements then parcels, in that order: the roads bound the blocks.
  void allocateLand();
  // The cover a parcel kind wears. Shared by the palette builder and by
  // surfaceColour, which must not substitute the biome over a parcel.
  static world::Biome parcelCover(ParcelKind kind);
  LandformSample rockAt(Point2 p) const;
  // The scarps as cut, kept so the repose weathering afterwards can touch only
  // the ground around them instead of the whole world.
  struct ScarpLine { std::vector<Point2> path; double drop = 0, face = 0, runout = 0; };
  std::vector<ScarpLine> scarpLines_;
  std::vector<double> preRockElevation_, rockDisplacement_, rockHardness_;
  struct Span { Point2 a, b; double bedA = 0, bedB = 0, halfWidth = 0, depth = 0, outerWidth = 0, halfWidthB = 0, depthB = 0, flow = 0; };
  std::vector<Span> coasts_, channels_;
  std::vector<std::vector<uint32_t>> regionBuckets_, coastBuckets_, channelBuckets_, ridgeBuckets_;
  world::MapEdgeSet edges_;
  std::vector<uint8_t> bucketWater_, bucketBiome_, bucketLandform_;
  double shoreWidth_ = 1500;
  static constexpr uint32_t bucketSide_ = 128;
  // The coastline as marched from the water mask: one curve shared by the
  // shore distance, the height blend, the sand band and the mesh constraint.
  std::vector<std::vector<Point2>> coastLines_;
  // The sand band as a region, not as a distance test. The band is cut into the
  // partition as a strip between the coastline and an inland offset, but it was
  // then *labelled* by testing each cell centre against `coastDistance < band` -
  // and the contour is simplified and smoothed, so the strip and the distance
  // band do not agree. Cells inside the cut strip failed the distance test and
  // dropped out, which is why the beach came out as splatter instead of a beach.
  std::vector<std::vector<Point2>> shoreBandRings_;
  std::vector<double> moisture_, uplift_, elevation_, baseElevation_, terrainElevation_, relief_, roughness_, waterLevel_, riverDistance_, discharge_;
  std::vector<uint8_t> water_, biome_, landform_;
  std::vector<uint32_t> region_;
  world::MapGraph graph_;
  world::MapExportFeatures features_;
};
} // namespace procedural::geometry
