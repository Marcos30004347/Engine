#include "procedural/geometry/TerrainParcelStrategy.hpp"
#include "procedural/geometry/TerrainParcels.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::geometry
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double unitRandom(uint64_t n) { return double(mix(n) >> 11) / double(1ull << 53); }
double range(uint64_t n, double lo, double hi) { return lo + (hi - lo) * unitRandom(n); }

// Hedged fields.
//
// A field is a rectangle. It is not a rectangle because someone drew one: it is
// a rectangle because it is ploughed in parallel furrows, and a plough turns at
// the ends of a straight run. So the rule that gives buildings their shape is
// the rule that gives fields theirs, and for the same reason - cut straight,
// perpendicular to the piece's own longest edge, and the children keep edges
// parallel to the parent.
//
// This replaces a cut that bent like a hedge line, rotated by up to a right
// angle on a retry, and fell back to a corner-to-corner diagonal when refused.
// Each of those was defensible on its own and together they were the twists and
// the big triangles: a bent cut leaves a piece that is not convex, a
// non-convex piece refuses the next cut, a refused cut rotates, and a rotated
// cut across a wedge makes a smaller wedge. The grain survives, because the
// grain is what makes a district's fields share a direction; it just no longer
// competes with three sources of randomness.
class EnclosureStrategy final : public ParcelStrategy
{
public:
  ParcelStrategyKind kind() const override { return ParcelStrategyKind::Enclosure; }
  const char *name() const override { return "enclosure"; }
  ParcelCutPlan planCut(const ParcelCutRequest &r) const override
  {
    ParcelCutPlan plan;
    plan.bend = 0.0;
    plan.allowDiagonalFallback = false;
    // The longest edge, snapped to the district grain.
    //
    // Taking the grain alone halves the piece in one direction every time, so a
    // block came out as a bundle of ribbons - measured at a 4.9 median aspect
    // ratio with 60% of fields longer than 4:1. Taking the longest edge alone
    // ignores the grain and a parish loses its common direction. Choosing
    // whichever of the grain and its perpendicular lies closer to the longest
    // edge does both: the cut squares the piece up, and it can only ever point
    // one of two ways in a district, which is what a field pattern is.
    double fromX = 0, fromY = 0, edgeX = 1, edgeY = 0, longest = 0;
    const auto &polygon = *r.polygon;
    for (size_t k = 0, n = polygon.size(); k < n; ++k)
    {
      const Point2 &a = polygon[k], &b = polygon[(k + 1u) % n];
      const double span = std::hypot(b.x - a.x, b.y - a.y);
      if (span > longest)
      { longest = span; fromX = a.x; fromY = a.y;
        edgeX = (b.x - a.x) / span; edgeY = (b.y - a.y) / span; }
    }
    const double grainX = r.givenX, grainY = r.givenY;
    const double along = std::abs(edgeX * grainX + edgeY * grainY);
    const double across = std::abs(edgeX * -grainY + edgeY * grainX);
    if (along >= across) { plan.axisX = grainX; plan.axisY = grainY; }
    else { plan.axisX = -grainY; plan.axisY = grainX; }
    // Through a point on the longest edge, as the town does: the two halves are
    // unequal but both are square to the parent. A retry slides that point
    // along the edge and never turns the cut.
    const double spread = .5;
    const double base = (1 - spread) * .5 + unitRandom(r.seed + 21u) * spread;
    const double ratio = r.attempt == 0 ? base
        : std::clamp(base + range(r.seed + 91u + r.attempt, -.3, .3), .1, .9);
    plan.at = longest > 1e-6
        ? Point2(fromX + edgeX * longest * ratio, fromY + edgeY * longest * ratio)
        : r.givenOrigin;
    return plan;
  }
};

// The open fields of a village: one direction for the whole furlong, held
// across every piece, so the result is a bundle of parallel strips.
class OpenFieldStrategy final : public ParcelStrategy
{
public:
  ParcelStrategyKind kind() const override { return ParcelStrategyKind::OpenField; }
  const char *name() const override { return "open field"; }
  ParcelCutPlan planCut(const ParcelCutRequest &r) const override
  {
    ParcelCutPlan plan;
    plan.axisX = r.givenX; plan.axisY = r.givenY;   // never rotated, never bent
    plan.bend = 0.0;
    // A selion is a straight run; a diagonal across the end of a furlong is a
    // triangle, and a triangle is not a strip.
    plan.allowDiagonalFallback = false;
    plan.at = r.attempt == 0 ? r.givenOrigin
        : Point2(r.centre.x + plan.axisX * range(r.seed + 60u + r.attempt,
                     -r.settings->cutJitter, r.settings->cutJitter) * std::sqrt(r.areaSquareMetres),
              r.centre.y + plan.axisY * range(r.seed + 80u + r.attempt,
                  -r.settings->cutJitter, r.settings->cutJitter) * std::sqrt(r.areaSquareMetres));
    return plan;
  }
};

// Building plots, after Watabou's `Cutter.bisect`. The cut runs perpendicular
// to the longest edge and passes through a point *on* that edge, so a plot
// stays square to the block it belongs to; it is jittered in angle only while
// the piece is still well above target size, and never bent, and never allowed
// to fall back to a diagonal - all three of which produce the triangular and
// wedge-shaped houses this strategy exists to stop.
class TownshipStrategy final : public ParcelStrategy
{
public:
  ParcelStrategyKind kind() const override { return ParcelStrategyKind::Township; }
  const char *name() const override { return "township"; }
  ParcelCutPlan planCut(const ParcelCutRequest &r) const override
  {
    ParcelCutPlan plan;
    plan.bend = 0.0;
    plan.allowDiagonalFallback = false;
    plan.refuseLargeTriangles = false;
    // The longest edge, and the point along it the cut goes through.
    double longest = 0; Point2 from{}, to{};
    const auto &polygon = *r.polygon;
    for (size_t k = 0, n = polygon.size(); k < n; ++k)
    {
      const Point2 &a = polygon[k], &b = polygon[(k + 1u) % n];
      const double span = std::hypot(b.x - a.x, b.y - a.y);
      if (span > longest) { longest = span; from = a; to = b; }
    }
    if (!(longest > 1e-6))
    { plan.axisX = r.axisX; plan.axisY = r.axisY; plan.at = r.centre; return plan; }
    // `spread` is Watabou's 0.8 * gridChaos: how unevenly the block divides.
    const double spread = .8 * r.settings->blockChaos;
    const double base = (1 - spread) * .5 + unitRandom(r.seed + 21u) * spread;
    // Once a piece is within four times the target it is cut square, with no
    // angle jitter at all. That is what keeps the final plots rectangular.
    const double jitter = r.areaSquareMetres > r.targetSquareMetres * 4.0
        ? range(r.seed + 40u + r.attempt, -1.0, 1.0) * (kPi / 6.0) * r.settings->blockChaos
        : 0.0;
    const double turn = r.attempt == 0 ? 0.0 : jitter;
    const double ux = (to.x - from.x) / longest, uy = (to.y - from.y) / longest;
    plan.axisX = ux * std::cos(turn) - uy * std::sin(turn);
    plan.axisY = ux * std::sin(turn) + uy * std::cos(turn);
    // A retry moves the cut along the block, never around it.
    const double ratio = r.attempt == 0 ? base
        : std::clamp(base + range(r.seed + 91u + r.attempt, -.28, .28), .12, .88);
    plan.at = {from.x + (to.x - from.x) * ratio, from.y + (to.y - from.y) * ratio};
    return plan;
  }
};

// A road, a river bank or a shore. The line was handed down and the strategy's
// only job is to pass it through unchanged: it is a boundary whatever shape it
// leaves behind, and a narrow verge beside one is a real parcel.
class InfrastructureStrategy final : public ParcelStrategy
{
public:
  ParcelStrategyKind kind() const override { return ParcelStrategyKind::Infrastructure; }
  const char *name() const override { return "infrastructure"; }
  ParcelCutPlan planCut(const ParcelCutRequest &r) const override
  {
    ParcelCutPlan plan;
    plan.axisX = r.givenX; plan.axisY = r.givenY;
    plan.at = r.givenOrigin;
    plan.bend = 0.0;
    plan.refuseLargeTriangles = false;
    return plan;
  }
};

const EnclosureStrategy kEnclosure;
const OpenFieldStrategy kOpenField;
const TownshipStrategy kTownship;
const InfrastructureStrategy kInfrastructure;
}

const ParcelStrategy &parcelStrategy(ParcelStrategyKind kind)
{
  switch (kind)
  {
    case ParcelStrategyKind::OpenField:      return kOpenField;
    case ParcelStrategyKind::Township:       return kTownship;
    case ParcelStrategyKind::Infrastructure: return kInfrastructure;
    default:                                 return kEnclosure;
  }
}
}
