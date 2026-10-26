#pragma once
#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainSettlements.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::geometry
{
// The parcel layer: land allocation between the world map and the mesh.
//
// A cell is a 2D polygon carrying attributes, not a flattened piece of ground -
// the surface inside it still follows the terrain. What the cell fixes is
// material, land use, and the edges around it.
//
// Allocation is per biome. Farmland is subdivided by recursive bent cuts, after
// Watabou's "recursive bisection with a twist", because isotropic Voronoi gives
// hexagonal blobs where real field patterns are elongated convex quads meeting
// near right angles. Biomes that want error-driven refinement instead get a
// coarse allocation used only for material and placement, never forced into the
// geometry.
constexpr uint32_t kNoParcel = 0xFFFFFFFFu;
// Material variants per land use. This is what separates one field from its
// neighbour, so it has to be large enough that collisions are rare across a
// district rather than common.
constexpr uint32_t kParcelToneCount = 16u;

enum class ParcelKind : uint8_t
{
  Rough = 0u,   // unallocated ground: the default, and what mountains stay as
  Field,        // arable, ploughed
  Pasture,      // grazed
  Meadow,
  Orchard,
  Woodland,
  Settlement,   // a built plot
  Yard,         // the garden, croft or court beside one
  Street,       // the made ground between the blocks of a town
  Market,       // open ground at the middle of a town
  Citadel,      // the castle ward
  Building,     // a house footprint
  Road,         // the carriageway between settlements
  Shore,        // the sand or shingle band along the coast
  Riverbank,    // the bed and bar inside the channel
  Count,
};
// What stands on the boundary between two cells. A function of what the edge
// separates and of the climate, never an independent roll.
enum class ParcelEdgeType : uint8_t
{
  None = 0u,
  Hedgerow,     // temperate wet
  Wall,         // upland dry stone
  Fence,        // arid
  Treeline,     // against woodland
  Lane,         // block boundary
  Riparian,     // against water
  Count,
};

struct ParcelCell
{
  std::vector<Point2> boundary;      // closed, counter-clockwise
  Point2 centre{};
  double areaSquareMetres = 0.0;
  ParcelKind kind = ParcelKind::Rough;
  uint8_t biome = 0u;
  // Material variant within the kind: which green a pasture is, which tilled
  // tone a field carries. Stable per cell, which is what makes a field read as
  // one field.
  uint8_t tone = 0u;
  uint32_t blockId = kNoParcel;
  uint32_t settlementId = kNoParcel;
};
struct ParcelEdge
{
  std::vector<Point2> path;
  uint32_t cellA = kNoParcel, cellB = kNoParcel;  // kNoParcel on an outer side
  ParcelEdgeType type = ParcelEdgeType::None;
  // The cell responsible for instantiating whatever stands here, so a hedgerow
  // is planted once rather than twice.
  uint32_t owner = kNoParcel;
};

struct ParcelSettings
{
  bool enabled = true;
  uint64_t seed = 0u;
  // Target parcel area, before jitter. Subdivision stops below this.
  // Overall grain, as a reference field side. The near and far sides below are
  // scaled by this, so one number still moves the whole allocation.
  double targetAreaSquareMetres = 110.0 * 110.0;
  // Field side close to a settlement and out in the deep country. The realised
  // cell comes out around .7 of these linearly, since a cell terminates as soon
  // as its own spread roll clears the target.
  double closeFieldMetres = 95.0;
  double farFieldMetres = 250.0;
  // Where tillage gives out, as a fraction of the settlement grade. Past this
  // the ground is grazed, and past that it is waste.
  double tillageFalloff = .45;
  // How much tree cover the waste regains, added to the sampled cover at the
  // far end of the grade.
  double wildwoodGain = .30;
  // How much tree cover is cleared around a settlement. This is what makes a
  // village sit in open fields with the forest beyond it.
  double clearanceNear = .26;
  // Spread of the target, as a multiplier range. The reference photo varies by
  // an order of magnitude inside one block, and too little spread reads as a
  // grid immediately.
  // Measured: with [.35, 2.6] the realised mean came out 179 m against a 122 m
  // prediction, because a cell terminates as soon as its own roll clears the
  // target, so the high tail of the spread dominates the realised size. A
  // tighter band keeps the variety and puts the mean where it is asked for.
  double areaSpreadLow = .45, areaSpreadHigh = 1.8;
  // --- the grain -----------------------------------------------------------
  // Farmland has a direction, and it varies slowly across the country. Fields
  // in one parish share an alignment; the next parish along shares a different
  // one; the change between them is gradual and follows the lie of the land.
  //
  // Cutting each polygon across its own principal axis cannot produce that: the
  // axis is a property of the piece, so every field is oriented independently
  // of the one beside it and the result is a shard mosaic with no structure at
  // any scale - which is exactly how the farmland read.
  //
  // A low-frequency noise field supplies the opinion instead. It gives an angle
  // at every point; the subdivision cuts along it and across it, alternating,
  // so a district comes out as blocks of parallel fields meeting near right
  // angles, and the grain turns gently from one district to the next.
  double grainScaleMetres = 2600.0;
  // How strongly the grain overrides the piece's own axis. At 1 the cut is
  // purely the grain; lower values let a long thin piece still be halved the
  // sensible way.
  double grainStrength = .82;
  // Slow wobble on the grain, so a district is not perfectly regular.
  double grainWaver = .22;
  double grainWaverScaleMetres = 620.0;
  // A cut is bent rather than straight: this is the lateral offset of its
  // midpoint as a fraction of the cut length. Watabou's "twist"; it hides the
  // recursion and matches how hedge lines actually run.
  double cutBend = .10;
  // Cut position along the long axis, as a fraction either side of centre.
  double cutJitter = .22;
  // Deep enough that the area target binds, not the depth. A map-graph block is
  // several square kilometres and a field is ten thousand square metres, which
  // is nine halvings before any of the spread or the failed splits are paid
  // for; at 9 the recursion stopped on the cap and fields came out seven times
  // the target area, which is what made them dwarf the city blocks.
  uint32_t maximumDepth = 16u;
  // Warping the incoming block boundaries. A map-graph cell edge is straight
  // and hundreds of metres long, and the biome boundary sits on it, so without
  // this the underlying map reads straight through the allocation. Recursive
  // midpoint displacement, hashed canonically so both sides of a shared edge
  // agree. Roughness is the midpoint offset as a fraction of the span; keep it
  // modest, since a heavily displaced chain can fold on itself.
  // Off by default. Warping a block's outline *before* it is divided makes the
  // thing being divided non-convex, and a non-convex piece refuses a straight
  // cut - which is the first link in the chain that ends in triangular fields.
  // The organic boundary it existed to provide is delivered instead by
  // `edgeWander`, which runs *after* subdivision on the finished cell edges, is
  // hashed canonically so neighbours agree, and so cannot affect any cut.
  double blockWarpRoughness = 0.0;
  uint32_t blockWarpDepth = 4u;
  double blockWarpMinimumMetres = 55.0;
  // Width of the sand band along the coast, and how much finer the cells are
  // inside it. A shore is a place, not a gradient: it needs its own cells, and
  // they have to be small enough to follow the coastline rather than step
  // across it.
  double shoreBandMetres = 95.0;
  double featureRefinement = .16;
  // How far past the band the refinement ramps out, as a multiple of it. The
  // ramp is what stops a delicate beach from sitting against a wall of enormous
  // cells - the discontinuity reads worse than either size does.
  double featureFalloff = 6.0;
  // A three-sided piece larger than this multiple of the target is refused and
  // the cut retried. Below it a triangle is just an odd corner of a field.
  double triangleTolerance = 1.4;
  // How far either side of a road centre-line counts as carriageway, as a
  // multiple of its width. The road's two edges are cut in as constraints, so
  // this only has to catch the strip between them.
  double roadHalfWidths = .62;
  // Below this a cell is a fold rather than a parcel and is dropped.
  double minimumAreaSquareMetres = 4.0;
  // Ground steeper than this is never parcelled.
  double maximumSlope = .30;
  // A candidate cut closer than this to an existing constraint is snapped onto
  // it or rejected. Two near-parallel chains a few metres apart are what make a
  // ladder of slivers, which is the failure this layer must not reintroduce.
  // Small enough that it only refuses genuinely degenerate cuts. At 25 m it
  // refused so many that cells came out three and a half times larger, which
  // made every boundary longer - the opposite of the intent.
  double snapDistanceMetres = 7.0;
  // A split that would leave either piece with a corner sharper than this is
  // refused. Two edges meeting at a thin angle are a spike, and a spike is
  // where sliver triangles come from.
  double minimumCornerDegrees = 11.0;
  // A boundary longer than this is broken up and given a gentle wander. A hedge
  // line that runs for half a kilometre dead straight is the one thing in this
  // layer that reads as generated, and a long straight constraint is also what
  // the triangulator turns into a fan of slivers.
  double maximumEdgeMetres = 120.0;
  // Lateral wander of a subdivided boundary, as a fraction of the span between
  // its inserted points.
  double edgeWander = .22;
  // Settlement cells subdivide much further. Watabou's minSq runs 10-90 against
  // wards about 30 units across, so a ward carries roughly twenty-five
  // buildings and a plot is about a fifth of the ward. At 40 m against a 110 m
  // ward ours carried seven, so every house was three times too big and the
  // blocks read as almost empty.
  double settlementAreaSquareMetres = 14.0 * 14.0;
  // Watabou's gridChaos: how unevenly a block is divided, and how much the cut
  // may rotate while the piece is still well above target size.
  double blockChaos = .55;
  // Width of the street peeled off the rim of a ward before it is cut into
  // building plots. Watabou insets each ward and leaves the gap as street;
  // here the ring is kept as cells instead of removed, because the parcel layer
  // has to stay a closed partition for the triangulator - so the street is a
  // material with width rather than a hole.
  double streetWidthMetres = 7.0;
  // A ward smaller than this keeps no street: a hamlet's plots front directly
  // onto the lane outside.
  double streetMinimumWardMetres = 24.0;
  // Share of plots in a ward left unbuilt - garden, croft, yard. Watabou drops
  // these outright with `emptyProb`; keeping them as a distinct material is
  // what makes a block read as buildings with gaps rather than one solid mass.
  double yardFraction = .34;
  // --- houses --------------------------------------------------------------
  // Watabou's `createAlleys` bisects a block perpendicular to its *longest
  // edge*, not across its principal axis, and that is what keeps the pieces
  // rectangular: every child inherits edges parallel to its parent's. He then
  // cuts with an alley gap and drops a few plots with `emptyProb`.
  //
  // The gap cannot be a hole here - the parcel layer has to stay a closed
  // partition for the triangulator - so the house is inset inside its plot and
  // the ring around it becomes the yard. Same result: separated footprints with
  // ground between them.
  double buildingInsetMetres = 1.3;
  // A plot too small to inset keeps no house; one this large is a yard or a
  // court rather than a single building.
  double buildingMinimumMetres = 5.0;
  // Plots deliberately left unbuilt, as Watabou's emptyProb does.
  double buildingEmptyFraction = .12;
  // A wood is a stand, not a set of fields. Wooded blocks take this multiple of
  // the target area, so they stay whole instead of being divided by hedges.
  double woodlandCoarseness = 1.9;
  // Open-field strips. A furlong block that is arable and gentle is cut into
  // parallel selions instead of being bisected into squares: `stripAspect` is
  // the final strip area as a fraction of the field target, so .16 makes a
  // strip about a sixth the area and six times as long as it is wide, and
  // `furlongBlocks` is how large a polygon may be when it enters strip mode.
  // A selion really is a long strip - 200 m by 5-20 m - but open-field farming
  // is the minority case and enclosure is what most ground ended up under. At
  // 0.16 and 0.55 the strips dominated: the median farm cell across the whole
  // island was 4.9 times longer than it was wide and 60% of fields were over
  // 4:1, which reads as corduroy rather than as country. Strips are now a fifth
  // of the ground near a village and are 3:1 rather than 6:1.
  double stripAspect = .34;
  double furlongBlocks = 2.4;
  // Not every field is striped - enclosure was piecemeal, and a map where
  // every arable block is hatched reads as a texture rather than as land.
  double stripedFraction = .20;
  // How far out the strips reach, as a fraction of the settlement grade: .45
  // puts them inside roughly the nearest 1.6 km of a village, which is the
  // walk to the furlong and back.
  double stripedReach = .45;
  // Side of the patch over which woodland cells share one tone, so a wood is
  // one colour across many small cells rather than one enormous cell.
  double standToneMetres = 520.0;
  // Side of the patch over which land *use* is coherent - a holding worked as
  // one. Tone still varies per field inside it.
  double holdingMetres = 260.0;
  // How much a single field may break ranks with its holding.
  double holdingJitter = .30;
  // Distance over which field size grades from close-in smallholding to open
  // field. Beyond it the grading saturates.
  double settlementGradeMetres = 9000.0;
};

// Everything the allocation reads about a point.
struct ParcelFields
{
  std::function<double(Point2)> slope;
  std::function<uint8_t(Point2)> waterType;
  std::function<double(Point2)> treeCover;        // 0..1
  std::function<double(Point2)> soilQuality;      // 0..1
  std::function<double(Point2)> moisture;         // 0..1
  // How farmable the high-level biome says this ground is, 0..1. The parcel
  // layer decorates the map; it must not contradict it, so a desert or an
  // icefield is never divided into fields however gentle and well-drained the
  // ground happens to be.
  std::function<double(Point2)> cultivable;
  // Distance to the nearest shoreline, and whether this point is inside a river
  // channel. Both exist so the allocation can give those features a plate of
  // their own: until it did, a shore was a colour blend with no region and a
  // river bed had no cell at all, so neither could be seen in the mesh.
  std::function<double(Point2)> coastDistance;
  std::function<bool(Point2)> riverChannel;
  // Distance to the nearest river channel. The inside-ness test alone is not
  // enough: a cell wider than the channel has its centroid outside it, so the
  // channel is neither refined toward nor recognised - measured, only 27% of
  // river positions landed in a river cell and 21% had no cell at all.
  std::function<double(Point2)> riverDistance;
  // Whether a point lies inside the sand band as it was actually cut. A
  // distance test cannot answer this: the band's inland edge is a simplified,
  // smoothed contour, so the strip in the partition and the set of points
  // within `shoreBandMetres` of the sea are two different regions.
  std::function<bool(Point2)> inShoreBand;
  std::function<double(Point2)> elevationMetres;
  // True where rock is exposed, so the mask can refuse to parcel it.
  std::function<bool(Point2)> exposedRock;
};

struct ParcelLayer
{
  std::vector<ParcelCell> cells;
  std::vector<ParcelEdge> edges;
  // Cell lookup by position. kNoParcel outside the allocated area.
  uint32_t cellAt(Point2 point) const;
  void buildIndex(double extentMetres);
private:
  double extent_ = 0.0;
  uint32_t side_ = 0u;
  std::vector<std::vector<uint32_t>> buckets_;
};

// `blocks` are the polygons to subdivide - map-graph cells work well, being
// already a planar partition at roughly block scale. `infrastructure` are
// polylines that must cut rather than be cut: roads, river banks, shorelines,
// cliff brinks.
// `wardBlocks` marks which entries of `blocks` are settlement wards, which are
// the only ones that get a street peeled off them. Empty means none; a radius
// test around the settlement cannot stand in for it, because a map-graph block
// kilometres across can contain a town centre without being a ward.
// `wardBlocks` carries `uint8_t(DistrictKind) + 1` for a settlement ward and 0
// for anything else, so the allocation knows both which blocks are wards and
// what each ward is for. A market is open ground; a castle is its own material;
// the rest are built.
ParcelLayer allocateParcels(double extentMetres, const std::vector<std::vector<Point2>> &blocks,
    const std::vector<std::vector<Point2>> &infrastructure, const ParcelFields &fields,
    const SettlementNetwork &settlements, const ParcelSettings &settings = {},
    const std::vector<uint8_t> &wardBlocks = {});
}
