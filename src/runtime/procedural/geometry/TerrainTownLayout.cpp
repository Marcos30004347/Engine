#include "procedural/geometry/TerrainTownLayout.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::geometry
{
namespace
{
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double unitRandom(uint64_t n) { return double(mix(n) >> 11) / double(1ull << 53); }
constexpr double kPi = 3.14159265358979323846;

double signedArea(const std::vector<Point2> &p)
{
  double total = 0;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    total += p[j].x * p[i].y - p[i].x * p[j].y;
  return .5 * total;
}
double area(const std::vector<Point2> &p) { return p.size() < 3u ? 0.0 : std::abs(signedArea(p)); }

// Clip a polygon by the half-plane `dot(q - at, n) <= 0`, returning both sides.
// Sutherland-Hodgman: exact for a convex subject, and a convex subject always
// yields two convex pieces, which is the invariant the whole layout rests on.
void clipByLine(const std::vector<Point2> &poly, const Point2 at, const double nx,
    const double ny, std::vector<Point2> &inside, std::vector<Point2> &outside)
{
  inside.clear(); outside.clear();
  const size_t n = poly.size();
  if (n < 3u) return;
  const auto side = [&](const Point2 q) { return (q.x - at.x) * nx + (q.y - at.y) * ny; };
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 a = poly[i], b = poly[(i + 1u) % n];
    const double da = side(a), db = side(b);
    if (da <= 0) inside.push_back(a);
    if (da >= 0) outside.push_back(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0))
    {
      const double t = da / (da - db);
      const Point2 crossing{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      inside.push_back(crossing);
      outside.push_back(crossing);
    }
  }
  if (inside.size() < 3u) inside.clear();
  if (outside.size() < 3u) outside.clear();
}

// Andrew's monotone chain. Only reached when a ward arrives non-convex, which
// the original never has to handle because its patches come straight from a
// Voronoi diagram.
std::vector<Point2> convexHull(std::vector<Point2> points)
{
  if (points.size() < 3u) return points;
  std::sort(points.begin(), points.end(),
      [](const Point2 &a, const Point2 &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  points.erase(std::unique(points.begin(), points.end(),
      [](const Point2 &a, const Point2 &b) { return a.x == b.x && a.y == b.y; }), points.end());
  if (points.size() < 3u) return points;
  const auto cross = [](const Point2 &o, const Point2 &a, const Point2 &b)
  { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
  std::vector<Point2> hull(points.size() * 2u);
  size_t k = 0;
  for (size_t i = 0; i < points.size(); ++i)
  {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
    hull[k++] = points[i];
  }
  for (size_t i = points.size() - 1u, t = k + 1u; i > 0; --i)
  {
    while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0) --k;
    hull[k++] = points[i - 1];
  }
  hull.resize(k > 0 ? k - 1u : 0u);
  return hull;
}

bool isConvex(const std::vector<Point2> &p)
{
  const size_t n = p.size();
  if (n < 3u) return false;
  int sign = 0;
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 &a = p[i], &b = p[(i + 1u) % n], &c = p[(i + 2u) % n];
    const double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    if (std::abs(cross) < 1e-9) continue;
    const int here = cross > 0 ? 1 : -1;
    if (sign == 0) sign = here;
    else if (here != sign) return false;
  }
  return true;
}

// `Ward.createAlleys`, with the gap kept as a piece so the ward stays covered.
void createAlleys(const std::vector<Point2> &poly, const WardRecipe &recipe,
    const uint64_t seed, const bool split, const uint32_t depth,
    std::vector<TownPiece> &out)
{
  const double square = area(poly);
  if (poly.size() < 3u || !(square > 1e-6)) return;
  // A safety the original does not need: it relies on the cut never failing,
  // which convexity guarantees. Anything that reaches here degenerate is
  // emitted as open ground rather than as a house.
  if (depth >= 24u) { out.push_back({poly, TownPieceKind::Court}); return; }

  // The stop size is drawn per piece: minSq * 2^(4*sizeChaos*(r-0.5)).
  const double stop = recipe.minimumSquareMetres
      * std::pow(2.0, 4.0 * recipe.sizeChaos * (unitRandom(seed + 11u) - .5));
  if (square < stop)
  {
    out.push_back({poly, unitRandom(seed + 13u) >= recipe.emptyProbability
        ? TownPieceKind::Building : TownPieceKind::Court});
    return;
  }

  // The longest edge, and a point along it.
  double longest = -1.0; size_t at = 0;
  for (size_t i = 0, n = poly.size(); i < n; ++i)
  {
    const Point2 &a = poly[i], &b = poly[(i + 1u) % n];
    const double span = std::hypot(b.x - a.x, b.y - a.y);
    if (span > longest) { longest = span; at = i; }
  }
  if (!(longest > 1e-6)) { out.push_back({poly, TownPieceKind::Court}); return; }
  const Point2 v0 = poly[at], v1 = poly[(at + 1u) % poly.size()];

  const double spread = .8 * recipe.gridChaos;
  const double ratio = (1 - spread) * .5 + unitRandom(seed + 17u) * spread;
  // Zero angle jitter once the piece is within four times the target: this is
  // what keeps the final plots square to their block.
  const double angleSpread = kPi / 6.0 * recipe.gridChaos
      * (square < recipe.minimumSquareMetres * 4.0 ? 0.0 : 1.0);
  const double angle = (unitRandom(seed + 19u) - .5) * angleSpread;

  const Point2 through{v0.x + (v1.x - v0.x) * ratio, v0.y + (v1.y - v0.y) * ratio};
  // `Cutter.bisect`: the cut runs perpendicular to the longest edge, so the
  // half-plane normal runs *along* it.
  const double dx = (v1.x - v0.x) / longest, dy = (v1.y - v0.y) / longest;
  const double cosB = std::cos(angle), sinB = std::sin(angle);
  const double nx = dx * cosB - dy * sinB, ny = dy * cosB + dx * sinB;

  const double gap = split ? kAlleyMetres : 0.0;
  std::vector<Point2> left, rest, strip, right;
  clipByLine(poly, {through.x - nx * gap * .5, through.y - ny * gap * .5}, nx, ny, left, rest);
  if (gap > 0 && !rest.empty())
    clipByLine(rest, {through.x + nx * gap * .5, through.y + ny * gap * .5}, nx, ny, strip, right);
  else { strip.clear(); right = rest; }

  if (left.empty() || right.empty())
  {
    // The cut refused. In the original this cannot happen; here it means the
    // piece is degenerate, and it must not be emitted as a building.
    out.push_back({poly, TownPieceKind::Court});
    return;
  }
  if (!strip.empty()) out.push_back({strip, TownPieceKind::Alley});

  int index = 0;
  for (const std::vector<Point2> *half : {&left, &right})
  {
    const double halfSquare = area(*half);
    // The child's own cut leaves an alley only if it is large enough. The
    // original's threshold is minSq/(r*r), and r*r is biased small, so the
    // threshold is usually large and most deep cuts touch - which is what makes
    // a terrace rather than a field of detached sheds.
    const double denominator = std::max(1e-3,
        unitRandom(seed + 23u + uint64_t(index)) * unitRandom(seed + 29u + uint64_t(index)));
    const bool childSplit = halfSquare > recipe.minimumSquareMetres / denominator;
    createAlleys(*half, recipe, mix(seed + 101u + uint64_t(index)), childSplit, depth + 1u, out);
    ++index;
  }
}
}

WardRecipe wardRecipe(DistrictKind kind, uint64_t seed)
{
  // One unit squared in metres squared, so `minSq` converts directly.
  constexpr double kUnitSquare = kWatabouUnitMetres * kWatabouUnitMetres;
  const double r1 = unitRandom(seed + 1u), r2 = unitRandom(seed + 2u);
  const double biased = r1 * r2;   // the original's `Random.float()*Random.float()`
  WardRecipe recipe;
  switch (kind)
  {
    case DistrictKind::Craft:      // CraftsmenWard: small to large, moderately regular
      recipe = {(10.0 + 80.0 * biased) * kUnitSquare, .5 + r1 * .2, .6, .04, true};
      break;
    case DistrictKind::Gentry:     // PatriciateWard: large, moderately regular
      recipe = {(80.0 + 30.0 * biased) * kUnitSquare, .5 + r1 * .3, .8, .2, true};
      break;
    case DistrictKind::Temple:     // AdministrationWard: large and regular
      recipe = {(80.0 + 30.0 * biased) * kUnitSquare, .1 + r1 * .3, .3, .04, true};
      break;
    case DistrictKind::Farmstead:  // a steading, not a street of houses
      recipe = {(40.0 + 60.0 * biased) * kUnitSquare, .6 + r1 * .4, .8, .35, true};
      break;
    case DistrictKind::Market:
    case DistrictKind::Castle:
      recipe.laid = false;         // open ground; the ward draws itself
      break;
    default:                       // Common, and the Slum end of it
      recipe = {(10.0 + 50.0 * biased) * kUnitSquare, .5 + r1 * .3, .7, .04, true};
      break;
  }
  return recipe;
}

std::vector<Point2> cityBlock(const std::vector<Point2> &ward,
    const std::vector<double> &insetPerEdge)
{
  if (ward.size() < 3u) return {};
  std::vector<Point2> block = ward;
  if (signedArea(block) < 0) std::reverse(block.begin(), block.end());
  // `Polygon.shrink`: cut by each edge's inward offset in turn. On a convex
  // ward every cut succeeds and the result stays convex.
  for (size_t i = 0, n = ward.size(); i < n && block.size() >= 3u; ++i)
  {
    const double inset = i < insetPerEdge.size() ? insetPerEdge[i] : 0.0;
    if (!(inset > 0)) continue;
    const Point2 a = ward[i], b = ward[(i + 1u) % n];
    const double span = std::hypot(b.x - a.x, b.y - a.y);
    if (!(span > 1e-9)) continue;
    // Inward normal for a counter-clockwise ring.
    const double nx = (b.y - a.y) / span, ny = -(b.x - a.x) / span;
    const Point2 at{(a.x + b.x) * .5 + nx * inset, (a.y + b.y) * .5 + ny * inset};
    std::vector<Point2> inside, outside;
    clipByLine(block, at, -nx, -ny, inside, outside);
    if (inside.size() >= 3u) block.swap(inside);
  }
  return block.size() >= 3u ? block : std::vector<Point2>{};
}

std::vector<TownPiece> layWard(const std::vector<Point2> &block,
    const WardRecipe &recipe, uint64_t seed, double outskirts)
{
  std::vector<TownPiece> pieces;
  if (block.size() < 3u || !recipe.laid) return pieces;
  WardRecipe thinned = recipe;
  // A ward on the rim keeps a fraction of its houses. Watabou's own filter is
  // per building and per edge; this is the same curve applied to the ward.
  thinned.emptyProbability = std::clamp(
      recipe.emptyProbability + .75 * std::pow(std::clamp(outskirts, 0.0, 1.0), 1.6), 0.0, .92);
  std::vector<Point2> convex = block;
  if (!isConvex(block))
  {
    // The hull of a non-convex plot encloses ground the plot does not own, so
    // laying it would put buildings outside the plot and over its neighbours -
    // a partition defect, not merely an ugly one. Accept the hull only when the
    // ground it adds is negligible; otherwise the plot stays open. A 14,444 m²
    // "house" was one of these, a deeply re-entrant plot hulled into a block.
    std::vector<Point2> hull = convexHull(block);
    const double own = area(block), hulled = area(hull);
    if (hull.size() < 3u || !(own > 1e-6) || hulled > own * 1.08)
    { pieces.push_back({block, TownPieceKind::Court}); return pieces; }
    convex.swap(hull);
  }
  if (convex.size() < 3u) return pieces;
  if (signedArea(convex) < 0) std::reverse(convex.begin(), convex.end());
  createAlleys(convex, thinned, seed, true, 0u, pieces);
  return pieces;
}
}
