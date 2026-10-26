#pragma once
#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainSettlements.hpp"
#include <cstdint>
#include <vector>

namespace procedural::geometry
{
// Laying out the inside of a ward, after Watabou's TownGeneratorOS.
//
// This is a port of `Ward.createAlleys` and `Cutter.bisect`, kept deliberately
// close to the original, because every attempt to reproduce the look with a
// general-purpose subdivision has failed in the same way and for the same
// reason. Measured on the 48 km island: 10.7% of footprints were triangles, the
// aspect ratio at the 99th percentile was 31.8:1, and the largest "house" was
// 6,525 m² against a 101 m² median.
//
// The cause is one invariant the original has and the general subdivision does
// not: **the block is always convex.** `getCityBlock` shrinks a convex Voronoi
// patch by a sequence of half-plane cuts, and a half-plane cut of a convex
// polygon is convex. A line perpendicular to an edge of a convex polygon always
// crosses its outline exactly twice, so the cut can never fail. In the original
// a failed cut would be fatal - `Polygon.cut` returns the piece *whole* and
// `createAlleys` would recurse on it forever - and it never happens, because
// convexity makes it impossible. Our blocks came from the parcel layer with
// warped boundaries and infrastructure cuts, so they were not convex, cuts
// failed, and a failed cut emitted the piece unchanged. That is the sliver.
//
// Two further differences, both visible:
//
// - **The alley is the gap in the cut.** `Cutter.bisect` takes a `gap` and
//   `Polygon.cut` peels each half back by `gap/2`, so the space between
//   buildings is left by the cut itself and only some cuts leave one. Insetting
//   every finished plot by a constant instead gives every house an identical
//   collar, which reads as a grid of tiles rather than as terraces off an alley.
// - **The stop size is random per piece**: `minSq · 2^(4·sizeChaos·(r−0.5))`.
//   With sizeChaos 0.6 that is a five-fold spread, which is what stops a ward
//   looking stamped.
//
// Scale: the original is unitless. Its city is 15 wards inside a radius of ~47
// units, and the reference plan's scale bar puts a walled city at ~820 m and
// ~13 wards across, so one unit is ~2.6 m. Every figure below is given in
// original units and converted once, here, so the provenance stays checkable.
constexpr double kWatabouUnitMetres = 2.6;

// `Ward.MAIN_STREET`, `REGULAR_STREET` and `ALLEY`. These are full widths; the
// inset applied to a block edge is half of one, because the neighbour insets by
// the other half.
constexpr double kMainStreetMetres = 2.0 * kWatabouUnitMetres;      // 5.2 m
constexpr double kRegularStreetMetres = 1.0 * kWatabouUnitMetres;   // 2.6 m
constexpr double kAlleyMetres = 0.6 * kWatabouUnitMetres;           // 1.6 m

// The four numbers that distinguish one ward from another, from the `CommonWard`
// subclasses. `minimumSquareMetres` is `minSq`, the target plot size.
struct WardRecipe
{
  double minimumSquareMetres = 130.0;
  double gridChaos = .6;        // how unevenly a block divides, and cut jitter
  double sizeChaos = .6;        // spread of the stop size, as a power of two
  double emptyProbability = .04;// a finished plot left as a court instead
  bool laid = true;             // false for a market, a park or a castle ward
};

// The recipe for a district, with the original's randomisation. Watabou draws
// `minSq` as `a + b·r·r` - a product of two uniforms, so it is biased towards
// the small end and a ward of large plots is the exception.
WardRecipe wardRecipe(DistrictKind kind, uint64_t seed);

// One piece of a laid-out ward. The original keeps only the buildings and draws
// them over the ward; a planar partition cannot do that, so the alleys and
// courts come back as pieces too and the ward stays covered exactly.
//
// The two kinds of open ground are not the same thing and must not share a
// material: an alley is the gap the cut left and is made ground, a court is a
// plot that was never built on and is a garden. Emitting both as yard put a
// green line down every alley, so a terrace read as a hedge row.
enum class TownPieceKind : uint8_t { Building = 0u, Alley, Court };
struct TownPiece
{
  std::vector<Point2> shape;
  TownPieceKind kind = TownPieceKind::Building;
};

// Insets each edge of a ward by its own distance - `Ward.getCityBlock`. A main
// street is wider than a lane, so the inset is per edge, which is why the
// original's streets vary in width and ours, at a constant 7 m, read as a
// honeycomb. Implemented as a sequence of half-plane cuts, like
// `Polygon.shrink`, so a convex ward gives a convex block.
std::vector<Point2> cityBlock(const std::vector<Point2> &ward,
    const std::vector<double> &insetPerEdge);

// How far out of the built-up area a ward sits, 0 at the middle and 1 at the
// edge. Watabou's `filterOutskirts` thins the buildings of any ward the city
// does not enclose, by their distance from the edges that face the town, which
// is what makes a town fade into its fields instead of stopping at a line.
// This is the same effect driven by the one number we have to hand.
// `block` must be convex; if it is not, its convex hull is taken, because the
// alternative is a cut that fails and a sliver emitted as a house.
std::vector<TownPiece> layWard(const std::vector<Point2> &block,
    const WardRecipe &recipe, uint64_t seed, double outskirts = 0.0);
}
