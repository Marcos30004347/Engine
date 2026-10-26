#include "procedural/world/hierarchy/Generators.hpp"
#include "procedural/world/generator/PlotPlanner.hpp"

#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Sample.hpp"

#include <set>

#include <functional>

#include <algorithm>
#include <cmath>

namespace procedural::world::hierarchy {

namespace {

constexpr double kPi = 3.14159265358979323846;

Point pointAt(const double x, const double y)
{
  return {geom2::toUnit(x), geom2::toUnit(y)};
}

struct Extent
{
  double minX = 0;
  double minY = 0;
  double maxX = 0;
  double maxY = 0;

  double width() const
  {
    return maxX - minX;
  }

  double height() const
  {
    return maxY - minY;
  }

  double centreX() const
  {
    return (minX + maxX) * .5;
  }

  double centreY() const
  {
    return (minY + maxY) * .5;
  }

  double diagonal() const
  {
    return std::hypot(width(), height());
  }
};

Extent extentOf(const Polygon &polygon)
{
  const geom2::Box box = geom2::bounds(polygon);

  return {geom2::toMetres(box.minX), geom2::toMetres(box.minY), geom2::toMetres(box.maxX), geom2::toMetres(box.maxY)};
}

Extent extentOf(const Region &region)
{
  return extentOf(region.boundary);
}

Edge widthed(const Point &a, const Point &b, const double halfWidth, const std::string &producer, const uint64_t feature)
{
  Edge edge;
  edge.path = {a, b};
  edge.roles.push_back({EdgeRole::Road, producer, feature});
  edge.bands.push_back({-halfWidth, halfWidth, Cover::Road, "carriageway"});

  return edge;
}

Edge cut(const Point &a, const Point &b, const std::string &producer, const uint64_t feature)
{
  Edge edge;
  edge.path = {a, b};
  edge.roles.push_back({EdgeRole::LotBoundary, producer, feature});

  return edge;
}

} // namespace

Descriptor GridStreets::descriptor() const
{
  return {"streets.grid", Role::Streets, Scope::Region, 1, true};
}

bool GridStreets::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.spacingMetres > 0.0) || !(settings_.halfWidthMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "grid streets need a positive spacing and width";
    }

    return false;
  }

  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal();
  const double cx = extent.centreX();
  const double cy = extent.centreY();

  const double cosine = std::cos(settings_.axisRadians);
  const double sine = std::sin(settings_.axisRadians);

  uint64_t feature = 0;

  for (int axis = 0; axis < 2; ++axis)
  {
    const double dx = axis == 0 ? cosine : -sine;
    const double dy = axis == 0 ? sine : cosine;
    const double nx = -dy;
    const double ny = dx;

    const int steps = int(reach / settings_.spacingMetres);

    for (int k = -steps; k <= steps; ++k)
    {
      const double offset = double(k) * settings_.spacingMetres;
      const double px = cx + nx * offset;
      const double py = cy + ny * offset;

      out.push_back(widthed(pointAt(px - dx * reach, py - dy * reach), pointAt(px + dx * reach, py + dy * reach), settings_.halfWidthMetres, "streets.grid", ++feature));
    }
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "streets.grid: region smaller than one block");
  }

  return true;
}

Descriptor RadialStreets::descriptor() const
{
  return {"streets.radial", Role::Streets, Scope::Region, 1, true};
}

bool RadialStreets::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (settings_.spokes < 2)
  {
    if (error != nullptr)
    {
      *error = "radial streets need at least two spokes";
    }

    return false;
  }

  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal();
  const double cx = extent.centreX();
  const double cy = extent.centreY();

  for (uint32_t k = 0; k < settings_.spokes; ++k)
  {
    // Jittered by a purpose-addressed draw, not by a counter: shuffling the
    // work list cannot move a spoke.
    const double jitter = (request.context.unitRandom("streets.radial", request.region.id, 1, k) - .5) * .3;
    const double angle = 2.0 * kPi * double(k) / double(settings_.spokes) + jitter;

    out.push_back(widthed(pointAt(cx, cy), pointAt(cx + std::cos(angle) * reach, cy + std::sin(angle) * reach), settings_.halfWidthMetres, "streets.radial", k + 1));
  }

  // A ring road partway out, as a closed chain of widthed edges.
  const double radius = std::max(extent.width(), extent.height()) * .5 * settings_.ringFraction;

  if (radius > settings_.halfWidthMetres * 2.0)
  {
    const uint32_t segments = std::max(8u, settings_.spokes * 3);

    for (uint32_t k = 0; k < segments; ++k)
    {
      const double a0 = 2.0 * kPi * double(k) / double(segments);
      const double a1 = 2.0 * kPi * double(k + 1) / double(segments);

      out.push_back(widthed(pointAt(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius), pointAt(cx + std::cos(a1) * radius, cy + std::sin(a1) * radius), settings_.halfWidthMetres, "streets.radial", 1000 + k));
    }
  }
  else
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "streets.radial: region too small for a ring road");
  }

  return true;
}

Descriptor StripLots::descriptor() const
{
  return {"lots.strip", Role::Lots, Scope::Region, 1, true};
}

bool StripLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.targetAreaMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "strip lots need a positive target area";
    }

    return false;
  }

  const double area = request.region.areaMetres();

  if (area <= settings_.targetAreaMetres)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "lots.strip: block already at target area");

    return true;
  }

  // Cuts in both directions, not one.
  //
  // The first version cut only across the block's longest axis, into
  // area / target strips. On a block much larger than one lot in both
  // directions that gives strips of enormous aspect - a 260 m block at a
  // 4,200 m2 target came out as sixteen lots 16 m wide and 260 m deep - which
  // the gallery render showed as hair-thin slivers. A lot has a frontage and a
  // depth, and both are bounded.
  //
  // So: depth from the target area at the frontage ratio, width from the
  // target area at that depth, and cuts at both spacings.
  const double depth = std::sqrt(settings_.targetAreaMetres / std::max(1.2, settings_.frontageRatio));
  const double width = settings_.targetAreaMetres / std::max(depth, 1.0);

  // The lot grid is built on the block's own longest edge, not on the world
  // axes.
  //
  // This measured the region's axis-aligned bounding box and cut vertical and
  // horizontal lines through it, so a block standing at any angle to the world
  // got a grid standing at a different angle to the block. The render shows it
  // immediately: the streets run one way and the lots inside them run another,
  // which no settlement has ever looked like.
  //
  // Watabou never refers to a world axis. Ward.createAlleys cuts the *longest
  // edge of the current piece, perpendicular to it*, so a lot takes its
  // frontage on the short side and the subdivision inherits the block's own
  // orientation - which is the street's, because the block is what the streets
  // left behind. The same rule applies here: take the longest edge as the local
  // u axis, measure the block along u and its normal, and lay the rows and the
  // lot divisions in that frame.
  const Ring &outline = request.region.boundary.outer;

  if (outline.size() < 3u)
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "lots.strip: block has no boundary to align to");

    return true;
  }

  double ux = 1, uy = 0, longest = -1;

  for (size_t i = 0; i < outline.size(); ++i)
  {
    const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
    const double bx = geom2::toMetres(outline[(i + 1) % outline.size()].x), by = geom2::toMetres(outline[(i + 1) % outline.size()].y);
    const double length = std::hypot(bx - ax, by - ay);

    if (length > longest)
    {
      longest = length;
      ux = (bx - ax) / length;
      uy = (by - ay) / length;
    }
  }

  if (!(longest > 0))
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "lots.strip: block has no edge long enough to align to");

    return true;
  }

  // The block's extent in its own frame: along the frontage edge, and across
  // it. Both are needed, because a lot has a frontage and a depth.
  const double nx = -uy, ny = ux;
  double minU = 1e300, maxU = -1e300, minN = 1e300, maxN = -1e300;

  for (const auto &point : outline)
  {
    const double px = geom2::toMetres(point.x), py = geom2::toMetres(point.y);
    const double u = px * ux + py * uy, n = px * nx + py * ny;

    minU = std::min(minU, u);
    maxU = std::max(maxU, u);
    minN = std::min(minN, n);
    maxN = std::max(maxN, n);
  }

  const double spanU = maxU - minU, spanN = maxN - minN;
  const double reach = std::hypot(spanU, spanN) + 1.0;

  // Rows run parallel to the frontage edge at the lot depth; each row is then
  // divided along the edge at the lot width.
  const int rows = std::max(1, int(std::llround(spanN / std::max(depth, 1.0))));
  const int lots = std::max(1, int(std::llround(spanU / std::max(width, 1.0))));

  uint64_t feature = 0;

  // A cut line in the block's frame: `alongEdge` divides a row into lots (the
  // line runs across the block, normal to the frontage), otherwise it separates
  // one row of lots from the next (the line runs parallel to the frontage).
  const auto cutAt = [&](const bool alongEdge, const double t, const uint64_t id) {
    const double jitter = (request.context.unitRandom("lots.strip", request.region.id, alongEdge ? 2 : 3, id) - .5) * settings_.jitter;
    const double clamped = std::min(.985, std::max(.015, t + jitter / double((alongEdge ? lots : rows) + 1)));

    // A point on the cut, and the direction the cut runs in.
    const double cu = alongEdge ? minU + spanU * clamped : (minU + maxU) * .5;
    const double cn = alongEdge ? (minN + maxN) * .5 : minN + spanN * clamped;

    const double px = cu * ux + cn * nx, py = cu * uy + cn * ny;
    const double dx = alongEdge ? nx : ux, dy = alongEdge ? ny : uy;

    out.push_back(cut(pointAt(px - dx * reach, py - dy * reach), pointAt(px + dx * reach, py + dy * reach), "lots.strip", id));
  };

  for (int k = 1; k < rows; ++k)
  {
    cutAt(false, double(k) / double(rows), ++feature);
  }

  for (int k = 1; k < lots; ++k)
  {
    cutAt(true, double(k) / double(lots), ++feature);
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "lots.strip: block takes one lot whole");
  }

  return true;
}


// Does this polygon have an edge lying along the given ring?
//
// Collinear overlap, not shared corners: two regions that share a boundary in
// an arrangement need not agree on where its vertices fall once either side has
// been split by something else, and a shared-corner test calls plainly
// adjacent pieces strangers.
static bool liesAlong(const Polygon &poly, const Ring &ring, const double tolerance, const double minimumOverlap)
{
  for (size_t i = 0; i < poly.outer.size(); ++i)
  {
    const double px = geom2::toMetres(poly.outer[i].x), py = geom2::toMetres(poly.outer[i].y);
    const double qx = geom2::toMetres(poly.outer[(i + 1) % poly.outer.size()].x), qy = geom2::toMetres(poly.outer[(i + 1) % poly.outer.size()].y);

    if (std::hypot(qx - px, qy - py) < minimumOverlap)
    {
      continue;
    }

    for (size_t j = 0; j < ring.size(); ++j)
    {
      const double ax = geom2::toMetres(ring[j].x), ay = geom2::toMetres(ring[j].y);
      const double bx = geom2::toMetres(ring[(j + 1) % ring.size()].x), by = geom2::toMetres(ring[(j + 1) % ring.size()].y);
      const double sx = bx - ax, sy = by - ay;
      const double length = std::hypot(sx, sy);

      if (length < 1e-9)
      {
        continue;
      }

      if (std::abs(sx * (py - ay) - sy * (px - ax)) / length > tolerance || std::abs(sx * (qy - ay) - sy * (qx - ax)) / length > tolerance)
      {
        continue;
      }

      const double t0 = ((px - ax) * sx + (py - ay) * sy) / length;
      const double t1 = ((qx - ax) * sx + (qy - ay) * sy) / length;

      if (std::min(length, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) >= minimumOverlap)
      {
        return true;
      }
    }
  }

  return false;
}

Descriptor RingLots::descriptor() const
{
  return {settings_.ward.empty() ? std::string("lots.ring") : "lots.ring." + settings_.ward, Role::Lots, Scope::Region, 1, true};
}

// Where an infinite line through (px, py) along (nx, ny) enters and leaves a
// ring, as a chord with a whisker past each end. False when it misses.
static bool chordThrough(const Ring &ring, const double px, const double py, const double nx, const double ny, geom2::Point &from, geom2::Point &to)
{
  double entry = 0, exit = 0;
  int crossings = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const geom2::Point &e0 = ring[i];
    const geom2::Point &e1 = ring[(i + 1) % ring.size()];
    const double ex0 = geom2::toMetres(e0.x), ey0 = geom2::toMetres(e0.y);
    const double sx = geom2::toMetres(e1.x) - ex0, sy = geom2::toMetres(e1.y) - ey0;
    const double denominator = nx * sy - ny * sx;

    if (std::abs(denominator) < 1e-12)
    {
      continue;
    }

    const double u = ((ex0 - px) * ny - (ey0 - py) * nx) / denominator;

    if (u < 0 || u > 1)
    {
      continue;
    }

    const double t = ((ex0 - px) + sx * u) * nx + ((ey0 - py) + sy * u) * ny;
    entry = crossings == 0 ? t : std::min(entry, t);
    exit = crossings == 0 ? t : std::max(exit, t);
    ++crossings;
  }

  if (crossings < 2 || exit - entry < 1e-6)
  {
    return false;
  }

  from = pointAt(px + nx * (entry - .05), py + ny * (entry - .05));
  to = pointAt(px + nx * (exit + .05), py + ny * (exit + .05));

  return true;
}

bool RingLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  // A perimeter block: lots around the edge, common ground in the middle.
  //
  // The previous version computed Cutter.ring's sequential peel faithfully and
  // then emitted its slices as chords into a global arrangement, which is not
  // the same thing at all - the arrangement knows nothing about the order the
  // slices were taken in and recombines them into faces the peel never made.
  // Measured, 42 of the lots it produced had no edge on a street: interior
  // pieces the peel would never have left.
  //
  // The band and the court are named directly instead. The court is the block
  // offset inward by one lot depth, emitted as a closed edge; the lots are
  // chords of the band between that ring and the block's own boundary. Every
  // lot then has a piece of the block edge - which is a street - by
  // construction rather than by a predicate that has to be kept true.
  const Polygon &block = request.region.boundary;

  if (block.outer.size() < 3u)
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "lots.ring: the block has no boundary to build around");

    return true;
  }

  // The depth has to fit the block: a band taken off every edge of a small one
  // meets itself in the middle and leaves no court.
  const double blockSize = std::sqrt(std::max(request.region.areaMetres(), 1.0));
  const double depth = std::min(settings_.depthMetres, blockSize * .26);

  std::vector<Polygon> inner;

  if (!geom2::offsetPolygon(block, -depth, {}, inner, error))
  {
    return false;
  }

  // No court to be had: the block is all frontage, so every chord across it
  // reaches the edge on both sides and plain radial cuts front the street.
  const bool hasCourt = !inner.empty() && geom2::areaMetres(inner.front()) > depth * depth * 2.0;

  if (hasCourt)
  {
    Edge court;
    court.path = inner.front().outer;
    court.path.push_back(inner.front().outer.front()); // Closed.
    court.roles.push_back({EdgeRole::LotBoundary, "lots.ring", 1});

    out.push_back(std::move(court));
  }

  // The lot divisions, after Cutter.ring: corner to corner, then across.
  //
  // The previous cuts were square to the edge they fronted and one depth
  // long, and that is wrong at every corner. The court's corner vertex sits
  // further in than one depth along either edge's normal - it is a depth in
  // along the *bisector* - so a corner cut fell short of the court, the band
  // stayed connected round that corner, and the arrangement recombined it.
  // An administration block came out as one 761 m2 lot with the court as a
  // hole in it, and the house placed on that was a remnant. The two normals
  // from one corner also enclosed a wedge whose only street was the corner
  // point: five lots in the town had no frontage, and all five were these.
  //
  // Watabou cuts from each block vertex to the matching court vertex, so the
  // corner belongs to a strip and every cut reaches the ring it must meet.
  // Then each edge's strip is divided by cuts that run from the block edge
  // to the nearest point of the court, which is what "across the band" means
  // on a band whose two sides are not parallel.
  const Ring &outline = block.outer;
  uint64_t feature = 1;

  const Ring *court = hasCourt ? &inner.front().outer : nullptr;

  // The nearest point of the court ring to a point, or the point pushed one
  // depth inward when there is no court.
  const auto onCourt = [&](const double px, const double py, const double nx, const double ny, double &qx, double &qy) {
    if (court == nullptr)
    {
      qx = px + nx * depth;
      qy = py + ny * depth;

      return;
    }

    double best = 1e300;

    for (size_t j = 0; j < court->size(); ++j)
    {
      const double ax = geom2::toMetres((*court)[j].x), ay = geom2::toMetres((*court)[j].y);
      const double bx = geom2::toMetres((*court)[(j + 1) % court->size()].x), by = geom2::toMetres((*court)[(j + 1) % court->size()].y);
      const double ex = bx - ax, ey = by - ay;
      const double length2 = ex * ex + ey * ey;
      const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - ax) * ex + (py - ay) * ey) / length2)) : 0;
      const double cx = ax + ex * t, cy = ay + ey * t;
      const double d = std::hypot(cx - px, cy - py);

      if (d < best)
      {
        best = d;
        qx = cx;
        qy = cy;
      }
    }
  };

  // A cut from just outside the block edge at (px, py) to just past its
  // landing on the court.
  const auto across = [&](const double px, const double py, const double nx, const double ny) {
    double qx = 0, qy = 0;
    onCourt(px, py, nx, ny, qx, qy);

    const double dx = qx - px, dy = qy - py;
    const double length = std::hypot(dx, dy);

    if (length < 1e-6)
    {
      return;
    }

    const double ex = dx / length, ey = dy / length;
    out.push_back(cut(pointAt(px - ex * 1.0, py - ey * 1.0), pointAt(qx + ex * .4, qy + ey * .4), "lots.ring", ++feature));
  };

  // Corners are where the boundary really turns, and runs are what lies
  // between them.
  //
  // A block boundary is not a polygon with a few corners: it follows smoothed
  // streets, so a junction leaves a concave arc of half a dozen vertices a
  // few decimetres apart. A corner cut at every vertex "however short the
  // edge" - which is what this did - fired at each of those and fanned a
  // sheaf of cuts into the court from each notch: sixteen slivers on one
  // block, none with a street. A corner is a *convex* turn past twenty
  // degrees; a concave one, or a gentle one, is part of the run either side
  // and its ground belongs to that run's lots.
  const size_t n = outline.size();

  std::vector<double> ex(n), ey(n), elen(n);

  for (size_t i = 0; i < n; ++i)
  {
    const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
    const double bx = geom2::toMetres(outline[(i + 1) % n].x), by = geom2::toMetres(outline[(i + 1) % n].y);
    elen[i] = std::hypot(bx - ax, by - ay);
    ex[i] = elen[i] > 1e-9 ? (bx - ax) / elen[i] : 0;
    ey[i] = elen[i] > 1e-9 ? (by - ay) / elen[i] : 0;
  }

  std::vector<bool> corner(n, false);
  size_t corners = 0;

  for (size_t i = 0; i < n; ++i)
  {
    const size_t h = (i + n - 1) % n;

    if (elen[h] < 1e-9 || elen[i] < 1e-9)
    {
      continue;
    }

    const double cross = ex[h] * ey[i] - ey[h] * ex[i];
    const double dot = ex[h] * ex[i] + ey[h] * ey[i];

    // Counter clockwise: a positive cross is a turn to the left, which is a
    // convex corner.
    corner[i] = cross > 0 && std::atan2(cross, dot) > .35;
    corners += corner[i] ? 1 : 0;
  }

  if (corners == 0)
  {
    // A boundary with no real corner - which a rounded block can be - still
    // needs one division or the band is a ring; the vertex furthest from the
    // centre stands in.
    geom2::Point middle;
    geom2::centroid(block, middle);
    double far = -1;
    size_t at = 0;

    for (size_t i = 0; i < n; ++i)
    {
      const double d = std::hypot(geom2::toMetres(outline[i].x - middle.x), geom2::toMetres(outline[i].y - middle.y));

      if (d > far)
      {
        far = d;
        at = i;
      }
    }

    corner[at] = true;
    corners = 1;
  }

  // Corner cuts: block vertex to the court's nearest point.
  for (size_t i = 0; i < n; ++i)
  {
    if (!corner[i])
    {
      continue;
    }

    const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
    const double nx = -ey[i], ny = ex[i];

    double qx = 0, qy = 0;
    onCourt(ax, ay, nx, ny, qx, qy);

    const double dx = qx - ax, dy = qy - ay;
    const double d = std::hypot(dx, dy);

    if (d > 1e-6)
    {
      out.push_back(cut(pointAt(ax - dx / d * 1.0, ay - dy / d * 1.0), pointAt(qx + dx / d * .4, qy + dy / d * .4), "lots.ring", ++feature));
    }
  }

  // Across cuts, per run: walk from one corner to the next accumulating
  // length, and divide the run by area into lots, placing each cut where
  // the walk crosses its share. On a curved run that is along the curve, not
  // along its chord.
  size_t first = 0;

  while (first < n && !corner[first])
  {
    ++first;
  }

  for (size_t step = 0, i = first; step < n; ++step, i = (i + 1) % n)
  {
    if (!corner[i])
    {
      continue;
    }

    // The run from corner i to the next corner.
    double runLength = 0;
    size_t j = i;

    do
    {
      runLength += elen[j];
      j = (j + 1) % n;
    } while (j != i && !corner[j]);

    if (runLength < 2.0)
    {
      continue;
    }

    const int lots = std::max(1, int(std::llround(runLength * depth / std::max(1.0, settings_.targetAreaMetres))));

    if (lots < 2)
    {
      continue;
    }

    double walked = 0;
    int k = 1;
    j = i;

    do
    {
      const double next = walked + elen[j];

      while (k < lots && runLength * double(k) / double(lots) <= next)
      {
        const double t = runLength * double(k) / double(lots) - walked;
        const double px = geom2::toMetres(outline[j].x) + ex[j] * t, py = geom2::toMetres(outline[j].y) + ey[j] * t;
        across(px, py, -ey[j], ex[j]);
        ++k;
      }

      walked = next;
      j = (j + 1) % n;
    } while (j != i && !corner[j] && k < lots);
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "lots.ring: no edge long enough to carry a lot");
  }

  return true;
}

Descriptor VoronoiBlocks::descriptor() const
{
  return {"blocks.voronoi", Role::Streets, Scope::Region, 1, true};
}

bool VoronoiBlocks::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.spacingMetres > 0.0) || !(settings_.halfWidthMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "voronoi blocks need a positive spacing and street width";
    }

    return false;
  }

  const Polygon &region = request.region.boundary;
  geom2::Point middle;

  if (!geom2::centroid(region, middle))
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "blocks.voronoi: the region has no centroid");

    return true;
  }

  const double mx = geom2::toMetres(middle.x), my = geom2::toMetres(middle.y);
  const Extent extent = extentOf(request.region);
  const double reach = extent.diagonal() * .5 + settings_.spacingMetres;

  // ---- Model.buildPatches: sites on a spiral, not blue noise ---------------
  //
  //   a = sa + sqrt(i) * 5
  //   r = i == 0 ? 0 : 10 + i * (2 + Random.float())
  //
  // The radius grows linearly with i and the angle with its square root, so
  // the patches are dense at the middle and open out toward the edge of the
  // town - which is where the countryside starts. Blue noise is even
  // everywhere, and a town that is even everywhere has no middle. His unit is
  // about 2.5 m against a patch of a dozen units, so the unit here is the
  // spacing over twelve.
  const double unit = settings_.spacingMetres / 12.0;
  const double turn = request.context.unitRandom("blocks.voronoi", request.region.id, 1, 0) * 2.0 * kPi;

  std::vector<geom2::Site> sites;

  for (uint32_t i = 0; i < 4000u; ++i)
  {
    const double angle = turn + std::sqrt(double(i)) * 5.0;
    const double radius = i == 0u ? 0.0 : (10.0 + double(i) * (2.0 + request.context.unitRandom("blocks.voronoi", request.region.id, 2, i))) * unit;

    if (radius > reach)
    {
      break;
    }

    const geom2::Point p = pointAt(mx + std::cos(angle) * radius, my + std::sin(angle) * radius);

    if (geom2::contains(region, p))
    {
      sites.push_back(geom2::Site{p, 0.0});
    }
  }

  if (sites.size() < 2u)
  {
    // A region too small or too odd for the spiral to land in twice: blue
    // noise, so the generator still answers.
    geom2::SampleSettings sampling;
    sampling.spacingMetres = settings_.spacingMetres;
    sampling.seed = request.context.worldSeed ^ (uint64_t(request.region.id) * 0x9e3779b97f4a7c15ull);
    sampling.marginMetres = settings_.spacingMetres * .25;

    std::vector<geom2::Point> seeds;

    if (!geom2::samplePolygon(region, sampling, seeds, error))
    {
      return false;
    }

    sites.clear();

    for (const geom2::Point &seed : seeds)
    {
      sites.push_back(geom2::Site{seed, 0.0});
    }
  }

  if (sites.size() < 2u)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "blocks.voronoi: the region holds fewer than two patches");

    return true;
  }

  // Only the central patches are relaxed - Watabou relaxes three plus the
  // citadel's, three times - so the middle is regular and the rim keeps the
  // spiral's irregularity.
  for (uint32_t pass = 0; pass < settings_.relaxIterations; ++pass)
  {
    std::vector<geom2::PartitionCell> now;

    if (!geom2::partitionRegion(region, sites, {}, now, error))
    {
      return false;
    }

    for (const geom2::PartitionCell &cell : now)
    {
      geom2::Point c;

      if (cell.site < 4u && geom2::centroid(cell.polygon, c))
      {
        sites[cell.site].position = c;
      }
    }
  }

  // A cell too small to be a block is merged into its neighbours.
  //
  // The spiral puts sites near the region's boundary, and a cell clipped by
  // that boundary can be a few square metres; the junction merge shrinks
  // others. Left as they were, those became "blocks" with nothing to cut, and
  // then lots with nothing to build - measured, every unbuilt sliver in a town
  // was one of these, a direct child of the root with a street on every side.
  // Dropping the site and partitioning again hands its ground to the cells
  // around it, which is what a merge is.
  std::vector<geom2::PartitionCell> cells;
  const double smallest = settings_.spacingMetres * settings_.spacingMetres * .2;

  for (int pass = 0; pass < 4; ++pass)
  {
    cells.clear();

    if (!geom2::partitionRegion(region, sites, {}, cells, error))
    {
      return false;
    }

    std::vector<bool> keep(sites.size(), false);
    bool dropped = false;

    for (const geom2::PartitionCell &cell : cells)
    {
      const bool worth = geom2::areaMetres(cell.polygon) >= smallest;
      keep[cell.site] = worth;
      dropped = dropped || !worth;
    }

    if (!dropped || sites.size() <= 2u)
    {
      break;
    }

    std::vector<geom2::Site> kept;

    for (size_t i = 0; i < sites.size(); ++i)
    {
      if (keep[i])
      {
        kept.push_back(sites[i]);
      }
    }

    if (kept.size() < 2u)
    {
      break;
    }

    sites = std::move(kept);
  }

  using Key = std::pair<int64_t, int64_t>;
  using EdgeKey = std::pair<Key, Key>;

  const auto key = [](const geom2::Point &p) {
    return Key{p.x, p.y};
  };

  // ---- Model.optimizeJunctions: merge the short edges ---------------------
  //
  //   if distance(v0, v1) < 8: v0 = (v0 + v1) / 2, every patch using v1 uses v0
  //
  // A Voronoi diagram has junctions where three cells meet a hair apart, and
  // each of those is a corner that every later cut has to respect: a lot with
  // three vertices where one would do, a street with a kink in it. Merging
  // them is what gives the patches clean corners to build against.
  const double merge = settings_.mergeJunctionsMetres > 0 ? settings_.mergeJunctionsMetres : settings_.spacingMetres * .28;

  for (int pass = 0; pass < 2; ++pass)
  {
    std::map<Key, geom2::Point> remap;

    for (const geom2::PartitionCell &cell : cells)
    {
      const Ring &ring = cell.polygon.outer;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const geom2::Point &a = ring[i];
        const geom2::Point &b = ring[(i + 1) % ring.size()];

        if (key(a) == key(b) || std::hypot(geom2::toMetres(b.x - a.x), geom2::toMetres(b.y - a.y)) >= merge)
        {
          continue;
        }

        const geom2::Point mid = pointAt((geom2::toMetres(a.x) + geom2::toMetres(b.x)) * .5, (geom2::toMetres(a.y) + geom2::toMetres(b.y)) * .5);
        remap[key(a)] = mid;
        remap[key(b)] = mid;
      }
    }

    if (remap.empty())
    {
      break;
    }

    for (geom2::PartitionCell &cell : cells)
    {
      Ring fresh;

      for (const geom2::Point &p : cell.polygon.outer)
      {
        const auto it = remap.find(key(p));
        const geom2::Point q = it == remap.end() ? p : it->second;

        if (fresh.empty() || key(q) != key(fresh.back()))
        {
          fresh.push_back(q);
        }
      }

      while (fresh.size() > 1u && key(fresh.front()) == key(fresh.back()))
      {
        fresh.pop_back();
      }

      cell.polygon.outer = fresh;
    }
  }

  // ---- The shared edges, each once, with the two cells that meet on it -----
  std::map<EdgeKey, std::pair<size_t, size_t>> shared;
  std::map<EdgeKey, std::pair<geom2::Point, geom2::Point>> geometry;

  for (size_t index = 0; index < cells.size(); ++index)
  {
    const Ring &ring = cells[index].polygon.outer;

    if (ring.size() < 3u)
    {
      continue;
    }

    for (size_t i = 0; i < ring.size(); ++i)
    {
      geom2::Point pa = ring[i], pb = ring[(i + 1) % ring.size()];
      Key ka = key(pa), kb = key(pb);

      if (kb < ka)
      {
        std::swap(ka, kb);
        std::swap(pa, pb);
      }

      const EdgeKey id{ka, kb};
      auto it = shared.find(id);

      if (it == shared.end())
      {
        shared.emplace(id, std::pair<size_t, size_t>{index, cells.size()});
        geometry.emplace(id, std::pair<geom2::Point, geom2::Point>{pa, pb});
      }
      else if (it->second.second == cells.size())
      {
        it->second.second = index;
      }
    }
  }

  // Corners on the edge of the town stay where they are: a street can be
  // smoothed through the middle, not through the boundary.
  std::set<Key> rimCorners;

  for (const auto &[id, pair] : shared)
  {
    if (pair.second >= cells.size())
    {
      rimCorners.insert(id.first);
      rimCorners.insert(id.second);
    }
  }

  // ---- Topology + Model.buildStreets: routes along the corners -------------
  //
  // The graph is the patch *corners* joined by the shared edges, weighted by
  // length, and a street is the cheapest route over it from a gate to the
  // middle. Walking cell to cell and widening the edge crossed between them
  // gives a row of segments each perpendicular to the direction of travel,
  // which is not a street; that was the first attempt.
  std::map<Key, std::vector<std::pair<Key, EdgeKey>>> corners;

  for (const auto &[id, ends] : geometry)
  {
    corners[id.first].push_back({id.second, id});
    corners[id.second].push_back({id.first, id});
  }

  std::set<EdgeKey> arterial;
  std::vector<std::vector<Key>> routes;

  if (settings_.arteries > 0u && !corners.empty())
  {
    Key heart = corners.begin()->first;
    double nearest = 1e300;

    for (const auto &[corner, links] : corners)
    {
      const double dx = geom2::toMetres(corner.first) - mx, dy = geom2::toMetres(corner.second) - my;
      const double distance = dx * dx + dy * dy;

      if (distance < nearest)
      {
        nearest = distance;
        heart = corner;
      }
    }

    // Gates: the furthest corner in each sector of the compass, so the routes
    // arrive from different sides.
    for (uint32_t g = 0; g < settings_.arteries; ++g)
    {
      const double from = -kPi + 2 * kPi * double(g) / double(settings_.arteries);
      const double to = -kPi + 2 * kPi * double(g + 1) / double(settings_.arteries);

      Key gate = heart;
      double furthest = -1;

      for (const auto &[corner, links] : corners)
      {
        const double dx = geom2::toMetres(corner.first) - mx, dy = geom2::toMetres(corner.second) - my;
        const double bearing = std::atan2(dy, dx);

        if (bearing < from || bearing >= to)
        {
          continue;
        }

        const double distance = dx * dx + dy * dy;

        if (distance > furthest)
        {
          furthest = distance;
          gate = corner;
        }
      }

      if (furthest < 0 || gate == heart)
      {
        continue;
      }

      std::map<Key, double> cost;
      std::map<Key, std::pair<Key, EdgeKey>> came;
      std::set<std::pair<double, Key>> queue;

      cost[gate] = 0;
      queue.insert({0, gate});

      while (!queue.empty())
      {
        const auto [here, corner] = *queue.begin();
        queue.erase(queue.begin());

        if (corner == heart)
        {
          break;
        }

        for (const auto &[next, id] : corners[corner])
        {
          const double dx = geom2::toMetres(next.first) - geom2::toMetres(corner.first);
          const double dy = geom2::toMetres(next.second) - geom2::toMetres(corner.second);
          const double step = here + std::hypot(dx, dy);
          const auto seen = cost.find(next);

          if (seen != cost.end() && seen->second <= step)
          {
            continue;
          }

          if (seen != cost.end())
          {
            queue.erase({seen->second, next});
          }

          cost[next] = step;
          came[next] = {corner, id};
          queue.insert({step, next});
        }
      }

      if (came.count(heart) == 0)
      {
        continue;
      }

      std::vector<Key> route;

      for (Key walk = heart; came.count(walk) != 0; walk = came[walk].first)
      {
        arterial.insert(came[walk].second);
        route.push_back(walk);
      }

      route.push_back(gate);
      routes.push_back(route);
    }
  }

  // ---- Model.smoothStreet: the street is smoothed *in place* ---------------
  //
  //   smoothed = street.smoothVertexEq(3)
  //   for i in 1...street.length - 1: street[i].set(smoothed[i])
  //
  // The street's points are the patches' own corners, so moving them bends the
  // patches to fit the street. That is the whole trick behind a Watabou main
  // street reading as one continuous road rather than as a chain of Voronoi
  // edges each turning at a junction: the junctions move. Ends stay put, and so
  // do corners on the town's boundary.
  std::map<Key, geom2::Point> moved;

  const auto positionOf = [&](const Key &k) {
    const auto it = moved.find(k);

    return it == moved.end() ? pointAt(geom2::toMetres(k.first), geom2::toMetres(k.second)) : it->second;
  };

  for (const std::vector<Key> &route : routes)
  {
    for (uint32_t pass = 0; pass < settings_.smoothingPasses; ++pass)
    {
      std::vector<geom2::Point> before;

      for (const Key &k : route)
      {
        before.push_back(positionOf(k));
      }

      for (size_t i = 1; i + 1 < route.size(); ++i)
      {
        if (rimCorners.count(route[i]) != 0)
        {
          continue;
        }

        const double f = std::max(0.25, settings_.smoothingWeight);
        const double x = (geom2::toMetres(before[i - 1].x) + f * geom2::toMetres(before[i].x) + geom2::toMetres(before[i + 1].x)) / (2.0 + f);
        const double y = (geom2::toMetres(before[i - 1].y) + f * geom2::toMetres(before[i].y) + geom2::toMetres(before[i + 1].y)) / (2.0 + f);

        moved[route[i]] = pointAt(x, y);
      }
    }
  }

  // ---- Emission: every shared edge once, arteries wider --------------------
  std::set<EdgeKey> emitted;
  uint64_t feature = 0;

  for (const auto &[id, ends] : geometry)
  {
    const geom2::Point a = positionOf(id.first);
    const geom2::Point b = positionOf(id.second);

    Key ka = key(a), kb = key(b);

    if (kb < ka)
    {
      std::swap(ka, kb);
    }

    if (!emitted.insert({ka, kb}).second)
    {
      continue;
    }

    if (std::hypot(geom2::toMetres(b.x - a.x), geom2::toMetres(b.y - a.y)) < settings_.halfWidthMetres)
    {
      continue;
    }

    const bool artery = arterial.count(id) != 0;
    const double half = artery ? std::max(settings_.arteryHalfWidthMetres, settings_.halfWidthMetres) : settings_.halfWidthMetres;

    out.push_back(widthed(a, b, half, artery ? "blocks.voronoi.artery" : "blocks.voronoi", ++feature));
  }

  // A many-sided patch gets a street through its middle.
  //
  // Sides are a proxy for size: a Voronoi cell with seven neighbours is a big
  // one, and a big one holds too many houses behind one frontage. The street
  // runs square to the cell's longest edge through its centroid, bounded to
  // the cell, at a width between a street and an artery - so a large patch is
  // two blocks with a real street between them rather than one block with an
  // alley.
  if (settings_.splitSidesAbove > 0u)
  {
    const double splitHalf = settings_.splitHalfWidthMetres > 0 ? settings_.splitHalfWidthMetres : (settings_.halfWidthMetres + std::max(settings_.arteryHalfWidthMetres, settings_.halfWidthMetres)) * .5;

    for (const geom2::PartitionCell &cell : cells)
    {
      Ring current;

      for (const geom2::Point &p : cell.polygon.outer)
      {
        current.push_back(positionOf(key(p)));
      }

      if (current.size() <= settings_.splitSidesAbove)
      {
        continue;
      }

      double ux = 1, uy = 0, longest = -1;

      for (size_t i = 0; i < current.size(); ++i)
      {
        const double ax = geom2::toMetres(current[i].x), ay = geom2::toMetres(current[i].y);
        const double bx = geom2::toMetres(current[(i + 1) % current.size()].x), by = geom2::toMetres(current[(i + 1) % current.size()].y);
        const double length = std::hypot(bx - ax, by - ay);

        if (length > longest)
        {
          longest = length;
          ux = (bx - ax) / length;
          uy = (by - ay) / length;
        }
      }

      Polygon shape;
      shape.outer = current;
      geom2::Point c, from, to;

      if (longest > 0 && geom2::centroid(shape, c) && chordThrough(current, geom2::toMetres(c.x), geom2::toMetres(c.y), -uy, ux, from, to))
      {
        // Only where both sides stay blocks. A chord through a centroid can
        // still clip a corner of an irregular cell, and the corner it clips
        // becomes a sliver no lot rule can use.
        std::vector<Polygon> halves;
        const double whole = geom2::areaMetres(shape);

        if (geom2::cutPolygon(shape, from, to, halves, nullptr) && halves.size() == 2u && geom2::areaMetres(halves[0]) > whole * .3 && geom2::areaMetres(halves[1]) > whole * .3)
        {
          out.push_back(widthed(from, to, splitHalf, "blocks.voronoi.split", ++feature));
        }
      }
    }
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "blocks.voronoi: the patch layer produced no shared edges");
  }

  return true;
}

// ---------------------------------------------------------------------------
// WardAssignment
// ---------------------------------------------------------------------------

const std::string &WardAssignment::wardOf(const RegionTree &tree, const RegionId root, const Region &block, const geom2::Point &centre, const uint64_t seed)
{
  static const std::string craftsmen = "craftsmen";

  if (!assigned_)
  {
    assign(tree, root, centre, seed);
  }

  const auto it = byRegion_.find(block.id);

  return it == byRegion_.end() ? craftsmen : it->second;
}

void WardAssignment::assign(const RegionTree &tree, const RegionId root, const geom2::Point &centre, const uint64_t seed)
{
  assigned_ = true;

  std::vector<const Region *> blocks;

  for (const RegionId id : tree.childrenOf(root))
  {
    const Region *region = tree.find(id);

    if (region != nullptr && region->cover != Cover::Road && region->boundary.outer.size() >= 3u)
    {
      blocks.push_back(region);
    }
  }

  if (blocks.empty())
  {
    return;
  }

  const size_t count = blocks.size();
  using Key = std::pair<int64_t, int64_t>;

  // Adjacency by shared corners: two blocks that share two corners share an
  // edge.
  std::vector<std::set<Key>> corners(count);
  std::vector<double> area(count), cx(count), cy(count);

  for (size_t i = 0; i < count; ++i)
  {
    for (const geom2::Point &p : blocks[i]->boundary.outer)
    {
      corners[i].insert({p.x, p.y});
    }

    geom2::Point c;
    geom2::centroid(blocks[i]->boundary, c);
    area[i] = blocks[i]->areaMetres();
    cx[i] = geom2::toMetres(c.x);
    cy[i] = geom2::toMetres(c.y);
  }

  // How wide the widest street touching each block is.
  //
  // A rich ward belongs on a main street: that is what makes it rich. The
  // street regions are the ribbons between the blocks, and a ribbon's width is
  // its area over its longest edge; a block is on one when their bounds meet.
  // Bounds rather than shared corners, because two regions that share a
  // boundary in an arrangement need not agree on where its vertices fall.
  std::vector<double> frontage(count, 0.0);
  {
    struct Ribbon
    {
      double width = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    std::vector<Ribbon> ribbons;

    for (const RegionId id : tree.childrenOf(root))
    {
      const Region *region = tree.find(id);

      if (region == nullptr || region->cover != Cover::Road || region->boundary.outer.size() < 3u)
      {
        continue;
      }

      Ribbon ribbon;
      ribbon.x0 = ribbon.y0 = 1e300;
      ribbon.x1 = ribbon.y1 = -1e300;

      double longest = 0;

      for (size_t i = 0; i < region->boundary.outer.size(); ++i)
      {
        const double x = geom2::toMetres(region->boundary.outer[i].x), y = geom2::toMetres(region->boundary.outer[i].y);
        const double bx = geom2::toMetres(region->boundary.outer[(i + 1) % region->boundary.outer.size()].x);
        const double by = geom2::toMetres(region->boundary.outer[(i + 1) % region->boundary.outer.size()].y);

        ribbon.x0 = std::min(ribbon.x0, x);
        ribbon.x1 = std::max(ribbon.x1, x);
        ribbon.y0 = std::min(ribbon.y0, y);
        ribbon.y1 = std::max(ribbon.y1, y);
        longest = std::max(longest, std::hypot(bx - x, by - y));
      }

      ribbon.width = longest > 1 ? region->areaMetres() / longest : 0;
      ribbons.push_back(ribbon);
    }

    for (size_t i = 0; i < count; ++i)
    {
      double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;

      for (const geom2::Point &p : blocks[i]->boundary.outer)
      {
        x0 = std::min(x0, geom2::toMetres(p.x));
        x1 = std::max(x1, geom2::toMetres(p.x));
        y0 = std::min(y0, geom2::toMetres(p.y));
        y1 = std::max(y1, geom2::toMetres(p.y));
      }

      for (const Ribbon &ribbon : ribbons)
      {
        if (ribbon.x1 >= x0 - .5 && ribbon.x0 <= x1 + .5 && ribbon.y1 >= y0 - .5 && ribbon.y0 <= y1 + .5)
        {
          frontage[i] = std::max(frontage[i], ribbon.width);
        }
      }
    }
  }

  const auto borders = [&](const size_t a, const size_t b) {
    int shared = 0;

    for (const Key &k : corners[a])
    {
      if (corners[b].count(k) != 0 && ++shared >= 2)
      {
        return true;
      }
    }

    return false;
  };

  // The plaza: the block holding the middle, else the nearest.
  size_t plaza = 0;
  double nearest = 1e300;

  for (size_t i = 0; i < count; ++i)
  {
    if (geom2::contains(blocks[i]->boundary, centre))
    {
      plaza = i;
      nearest = -1;
      break;
    }

    const double d = std::hypot(cx[i] - geom2::toMetres(centre.x), cy[i] - geom2::toMetres(centre.y));

    if (d < nearest)
    {
      nearest = d;
      plaza = i;
    }
  }

  std::vector<std::string> ward(count);
  ward[plaza] = "plaza";

  const auto distance = [&](const size_t i) {
    return std::hypot(cx[i] - cx[plaza], cy[i] - cy[plaza]);
  };

  const auto hash = [seed](const size_t i) {
    uint64_t v = seed ^ (uint64_t(i) + 1u) * 0x9e3779b97f4a7c15ull;
    v ^= v >> 29u;
    v *= 0xbf58476d1ce4e5b9ull;
    v ^= v >> 32u;
    return v;
  };

  // Model.WARDS, in order, then slums for whatever is left, as in the source.
  // Military, park and gate wards are craftsmen here until they exist.
  std::vector<std::string> order = {
    "craftsmen", "craftsmen", "merchant", "craftsmen", "craftsmen", "temple",
    "craftsmen", "craftsmen", "craftsmen", "craftsmen", "craftsmen",
    "craftsmen", "craftsmen", "craftsmen", "administration", "craftsmen",
    "slum", "craftsmen", "slum", "patriciate", "plaza",
    "slum", "craftsmen", "craftsmen", "craftsmen", "slum",
    "craftsmen", "craftsmen", "craftsmen", "craftsmen", "slum",
    "craftsmen", "craftsmen", "patriciate", "plaza", "merchant"};

  // "Some shuffling": a tenth of the list, adjacent pairs swapped.
  for (size_t k = 0; k < order.size() / 10; ++k)
  {
    const size_t index = size_t(hash(1000 + k) % (order.size() - 1));
    std::swap(order[index], order[index + 1]);
  }

  size_t next = 0;

  for (size_t assigned = 1; assigned < count; ++assigned)
  {
    const std::string cls = next < order.size() ? order[next++] : "slum";

    size_t best = count;
    double bestRate = 1e300;

    for (size_t i = 0; i < count; ++i)
    {
      if (!ward[i].empty())
      {
        continue;
      }

      double rate;

      // A main street is worth this much distance to a ward that wants one.
      const double onAStreet = frontage[i] * 12.0;

      if (cls == "merchant")
      {
        rate = distance(i) - onAStreet;
      }
      else if (cls == "administration")
      {
        rate = (borders(i, plaza) ? 0 : distance(i)) - onAStreet;
      }
      else if (cls == "temple")
      {
        rate = borders(i, plaza) ? -1.0 / std::max(area[i], 1.0) : distance(i) * area[i];
      }
      else if (cls == "slum")
      {
        rate = -distance(i);
      }
      else if (cls == "patriciate")
      {
        rate = 0;

        for (size_t j = 0; j < count; ++j)
        {
          if (j != i && ward[j] == "slum" && borders(i, j))
          {
            rate += 1;
          }
        }

        rate -= frontage[i] * .25;
      }
      else if (cls == "plaza")
      {
        // A second market: not touching the first, and not much larger.
        rate = borders(i, plaza) ? 1e300 : area[i] / std::max(area[plaza], 1.0);
      }
      else
      {
        rate = double(hash(i) % 1000000u);
      }

      if (rate < bestRate)
      {
        bestRate = rate;
        best = i;
      }
    }

    if (best == count)
    {
      break;
    }

    ward[best] = cls;
  }

  // Outside the walls is a different kind of ground.
  //
  // Watabou's inset table draws the line in miniature - a patch outside is
  // inset by an ALLEY where one inside is inset by a REGULAR_STREET - and
  // filterOutskirts thins the buildings out there by how near a road or a gate
  // they are. Both say the same thing: a plot beyond the wall is not a town
  // plot at a smaller size. It is working ground with a house on it.
  //
  // With no wall built yet, the town's own extent is the line: the outermost
  // ring of blocks is outskirts, whatever ward they were otherwise assigned.
  // That is a stand-in for a wall and not a substitute for one, and it is
  // marked as such so the wall can replace it without the plot rules moving.
  double furthest = 0;

  for (size_t i = 0; i < count; ++i)
  {
    furthest = std::max(furthest, distance(i));
  }

  for (size_t i = 0; i < count; ++i)
  {
    const bool outside = furthest > 1 && distance(i) > furthest * .72 && ward[i] != "plaza" && ward[i] != "temple";

    byRegion_[blocks[i]->id] = outside ? "outskirts" : (ward[i].empty() ? "craftsmen" : ward[i]);
  }
}

Descriptor BisectionLots::descriptor() const
{
  return {settings_.ward.empty() ? std::string("lots.bisection") : "lots.bisection." + settings_.ward, Role::Lots, Scope::Region, 1, true};
}

// The town generator's ward table, quoted. `r * r` rather than `r` is not a
// typo: squaring a uniform draw biases every size range toward its small end,
// which is why most of a quarter is modest and a few plots are not.
BisectionLots::Settings BisectionLots::craftsmen()
{
  Settings s;
  s.minimumAreaMetres = 10 * 6.25;
  s.gridChaos = .6;
  s.sizeChaos = .6;
  s.emptyProbability = .04;
  s.alleyMetres = 1.5;
  s.maximumDepth = 12; // small to large, moderately regular
  s.ward = "craftsmen";
  s.minimumAreaSpreadMetres = 80 * 6.25;

  return s;
}

BisectionLots::Settings BisectionLots::merchant()
{
  Settings s;
  s.minimumAreaMetres = 50 * 6.25;
  s.gridChaos = .65;
  s.sizeChaos = .7;
  s.emptyProbability = .15;
  s.alleyMetres = 1.5;
  s.maximumDepth = 12; // medium to large, many yards
  s.ward = "merchant";
  s.minimumAreaSpreadMetres = 60 * 6.25;
  s.streetMetres = 2.5;

  return s;
}

BisectionLots::Settings BisectionLots::patriciate()
{
  Settings s;
  s.minimumAreaMetres = 80 * 6.25;
  s.gridChaos = .65;
  s.sizeChaos = .8;
  s.emptyProbability = .2;
  s.alleyMetres = 1.5;
  s.maximumDepth = 12; // large plots, a fifth left as garden
  s.ward = "patriciate";
  s.minimumAreaSpreadMetres = 30 * 6.25;
  s.streetMetres = 3.0;

  return s;
}

BisectionLots::Settings BisectionLots::administration()
{
  Settings s;
  s.minimumAreaMetres = 80 * 6.25;
  s.gridChaos = .25;
  s.sizeChaos = .3;
  s.emptyProbability = .04;
  s.alleyMetres = 1.5;
  s.maximumDepth = 12; // large and deliberately regular
  s.ward = "administration";
  s.minimumAreaSpreadMetres = 30 * 6.25;
  s.streetMetres = 3.0;

  return s;
}

BisectionLots::Settings BisectionLots::slum()
{
  Settings s;
  s.minimumAreaMetres = 10 * 6.25;
  s.gridChaos = .8;
  s.sizeChaos = .8;
  s.emptyProbability = .03;
  s.alleyMetres = 1.5;
  s.maximumDepth = 12; // small and chaotic
  s.ward = "slum";
  s.minimumAreaSpreadMetres = 30 * 6.25;

  return s;
}

bool BisectionLots::propose(const Request &request, std::vector<Edge> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (!(settings_.minimumAreaMetres > 0))
  {
    if (error != nullptr)
    {
      *error = "bisection lots need a positive minimum area";
    }

    return false;
  }

  const RegionId region = request.region.id;
  const Context &context = request.context;

  uint64_t draws = 0;

  // One stream, advanced per draw, so the recursion is reproducible without
  // depending on the order two sibling branches happen to be visited in.
  const auto random = [&]() {
    return context.unitRandom("lots.bisection", region, 7, draws++);
  };

  uint32_t cuts = 0;

  // The streets a piece can be reached from: the block's own outline to begin
  // with, and every alley opened inside it after that.
  //
  // A chord splits a piece into two, and each half keeps an arc of the piece's
  // boundary - but that boundary may itself be made of earlier chords, so a
  // half can end up bounded entirely by cuts and reachable from nothing. That
  // is not a hypothetical: 63 of 451 lots in the town had no edge on a road.
  // Alleys are what Watabou's rule exists to provide, so a cut that would
  // landlock a half opens one whether or not the size rule asked for it.
  struct Frontage
  {
    double ax = 0, ay = 0, bx = 0, by = 0;

    // How far a piece's edge may stand off this line and still front it. An
    // alley is recorded by its centreline, and the halves are peeled half its
    // width away from that line - so a tolerance that does not know the width
    // rejects the very frontage the alley just created.
    double standoff = 0;
  };

  const auto fronts = [](const Polygon &piece, const std::vector<Frontage> &streets) {
    for (size_t i = 0; i < piece.outer.size(); ++i)
    {
      const double px = geom2::toMetres(piece.outer[i].x), py = geom2::toMetres(piece.outer[i].y);
      const double qx = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].x), qy = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].y);
      const double ex = qx - px, ey = qy - py;
      const double length = std::hypot(ex, ey);

      if (length < 1.5)
      {
        continue;
      }

      for (const Frontage &street : streets)
      {
        const double sx = street.bx - street.ax, sy = street.by - street.ay;
        const double slength = std::hypot(sx, sy);

        if (slength < 1e-9)
        {
          continue;
        }

        // Collinear: both of the edge's ends lie on the street's line.
        const double d0 = std::abs(sx * (py - street.ay) - sy * (px - street.ax)) / slength;
        const double d1 = std::abs(sx * (qy - street.ay) - sy * (qx - street.ax)) / slength;

        const double tolerance = street.standoff + .35;

        if (d0 > tolerance || d1 > tolerance)
        {
          continue;
        }

        // ...and they overlap it along its length by enough to be a doorway.
        const double t0 = ((px - street.ax) * sx + (py - street.ay) * sy) / slength;
        const double t1 = ((qx - street.ax) * sx + (qy - street.ay) * sy) / slength;
        const double lo = std::max(0.0, std::min(t0, t1)), hi = std::min(slength, std::max(t0, t1));

        if (hi - lo >= 1.5)
        {
          return true;
        }
      }
    }

    return false;
  };

  std::vector<Frontage> blockStreets;

  for (size_t i = 0; i < request.region.boundary.outer.size(); ++i)
  {
    const auto &a = request.region.boundary.outer[i];
    const auto &b = request.region.boundary.outer[(i + 1) % request.region.boundary.outer.size()];

    blockStreets.push_back({geom2::toMetres(a.x), geom2::toMetres(a.y), geom2::toMetres(b.x), geom2::toMetres(b.y), 0.0});
  }

  // This block's minSq: base + spread * r * r, drawn once per block the way a
  // ward draws it once at construction. Two draws, because r * r is the
  // product of two independent draws in the source, not the square of one.
  const double minimumArea = settings_.minimumAreaMetres + settings_.minimumAreaSpreadMetres * context.unitRandom("lots.bisection", region, 9, 0) * context.unitRandom("lots.bisection", region, 9, 1);

  // The two fixed axes, when the orthogonal rule is in force: the block's own
  // longest edge, and its perpendicular. Taken once, from the region, so every
  // cut at every depth lands on one of them.
  double axisX = 1, axisY = 0;

  if (settings_.orthogonal)
  {
    double longest = -1;
    const Ring &outline = request.region.boundary.outer;

    for (size_t i = 0; i < outline.size(); ++i)
    {
      const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
      const double bx = geom2::toMetres(outline[(i + 1) % outline.size()].x), by = geom2::toMetres(outline[(i + 1) % outline.size()].y);
      const double length = std::hypot(bx - ax, by - ay);

      if (length > longest)
      {
        longest = length;
        axisX = (bx - ax) / length;
        axisY = (by - ay) / length;
      }
    }
  }

  // Watabou recurses on polygons and keeps the leaves. We recurse the same way
  // but keep the *cuts*, because in this model the child regions are derived
  // from the edges rather than authored - the arrangement rebuilds exactly the
  // same pieces from the same chords.
  const std::function<void(const Polygon &, uint32_t, bool, const std::vector<Frontage> &)> bisect = [&](const Polygon &piece, const uint32_t depth, const bool split, const std::vector<Frontage> &streets) {
    const double area = geom2::areaMetres(piece);

    if (depth >= settings_.maximumDepth || piece.outer.size() < 3)
    {
      return;
    }

    // Rule 4: the stopping size is log-uniform about the target.
    const double stop = minimumArea * std::pow(2.0, 4.0 * settings_.sizeChaos * (random() - .5));

    // ...but area is not the only thing that decides a lot is finished.
    //
    // Halving is a sequence: a square piece cut once is two halves of 1:2, and
    // cut again is four squares. Stopping on area alone stops wherever that
    // sequence happens to be when the target is crossed, so half the lots in a
    // town came out 1:2 or thinner - the thin lots in the render, and no cut
    // rule could fix them because the cut was not what was wrong.
    //
    // A piece that is still long is not finished, whatever its area: it keeps
    // splitting across its length until it is square enough or too small to
    // split again. That is what makes the sides of a lot as equal as the block
    // allows.
    double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;
    {
      double ux = 1, uy = 0, longestEdge = -1;

      for (size_t i = 0; i < piece.outer.size(); ++i)
      {
        const double bx = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].x - piece.outer[i].x);
        const double by = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].y - piece.outer[i].y);
        const double length = std::hypot(bx, by);

        if (length > longestEdge)
        {
          longestEdge = length;
          ux = bx / length;
          uy = by / length;
        }
      }

      for (const geom2::Point &p : piece.outer)
      {
        const double x = geom2::toMetres(p.x), y = geom2::toMetres(p.y);
        const double u = x * ux + y * uy, v = -x * uy + y * ux;

        loU = std::min(loU, u);
        hiU = std::max(hiU, u);
        loV = std::min(loV, v);
        hiV = std::max(hiV, v);
      }
    }

    const double spanU = hiU - loU, spanV = hiV - loV;
    const double squareEnough = std::max(spanU, spanV) > 1e-9 ? std::min(spanU, spanV) / std::max(spanU, spanV) : 1.0;

    // Too small to halve again whatever its shape: two of these would be under
    // the floor.
    const bool cannotHalve = area < settings_.minimumLotMetres * 2.2;

    if (cannotHalve || (area < stop && squareEnough >= settings_.squareEnoughRatio))
    {
      return;
    }

    // Rule 5, hoisted: an alley only while the group is large - or, on the
    // first cut of a ward that has one, a street. Needed up here because the
    // ribbon comes off both halves before they are measured.
    const bool street = depth == 0 && settings_.streetMetres > 0;
    double gap = street ? settings_.streetMetres : (split ? settings_.alleyMetres : 0.0);

    // Rule 1: the longest edge, and its start vertex.
    size_t longest = 0;
    double best = -1;

    for (size_t i = 0; i < piece.outer.size(); ++i)
    {
      const geom2::Point &a = piece.outer[i];
      const geom2::Point &b = piece.outer[(i + 1) % piece.outer.size()];
      const double length = std::hypot(geom2::toMetres(b.x - a.x), geom2::toMetres(b.y - a.y));

      if (length > best)
      {
        best = length;
        longest = i;
      }
    }

    const geom2::Point &v0 = piece.outer[longest];
    const geom2::Point &v1 = piece.outer[(longest + 1) % piece.outer.size()];

    const double ax = geom2::toMetres(v0.x), ay = geom2::toMetres(v0.y);
    const double dx = geom2::toMetres(v1.x) - ax, dy = geom2::toMetres(v1.y) - ay;
    const double edgeLength = std::hypot(dx, dy);

    if (!(edgeLength > 1e-9))
    {
      return;
    }

    // Rule 3: how far off perpendicular - nothing once the piece is small, and
    // nothing at all under the orthogonal rule.
    const double angleSpread = settings_.orthogonal ? 0.0 : (kPi / 6) * settings_.gridChaos * (area < minimumArea * 4 ? 0.0 : 1.0);
    const double angle = (random() - .5) * angleSpread;

    // The directions a cut may run in, best first.
    //
    // Watabou's rule gives one: perpendicular to the longest edge, which is
    // what puts a lot's frontage on its short side. The perpendicular of that
    // is offered as a second, because a piece that has already been cut down
    // one way can only be improved by being cut the other, and the choice
    // between them is made on the shapes they actually produce rather than
    // assumed.
    std::vector<std::pair<double, double>> directions;

    if (settings_.orthogonal)
    {
      const double ux = dx / edgeLength, uy = dy / edgeLength;
      const double alongFirst = std::abs(ux * axisX + uy * axisY);
      const double alongSecond = std::abs(ux * -axisY + uy * axisX);

      if (alongFirst < alongSecond)
      {
        directions.push_back({axisX, axisY});
        directions.push_back({-axisY, axisX});
      }
      else
      {
        directions.push_back({-axisY, axisX});
        directions.push_back({axisX, axisY});
      }
    }
    else
    {
      const double cosine = std::cos(angle), sine = std::sin(angle);
      const double ex = dx * cosine - dy * sine, ey = dy * cosine + dx * sine;
      const double length = std::hypot(ex, ey);

      directions.push_back({-ey / length, ex / length});
      directions.push_back({ex / length, ey / length});
    }

    // How much of its own bounding rectangle a piece fills, and the narrower
    // of that rectangle's sides. A rectangle fills 1, a triangle 0.5; a sliver
    // fills perfectly and is still a sliver, so both numbers are needed.
    const auto shapeOf = [](const Polygon &poly, const double ux, const double uy, double &fill, double &narrow, double &wide) {
      double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

      for (const geom2::Point &p : poly.outer)
      {
        const double x = geom2::toMetres(p.x), y = geom2::toMetres(p.y);
        const double u = x * ux + y * uy, v = -x * uy + y * ux;

        loU = std::min(loU, u);
        hiU = std::max(hiU, u);
        loV = std::min(loV, v);
        hiV = std::max(hiV, v);
      }

      const double w = hiU - loU, h = hiV - loV;

      narrow = std::min(w, h);
      wide = std::max(w, h);
      fill = w * h > 1e-9 ? geom2::areaMetres(poly) / (w * h) : 0.0;
    };

    // One candidate: the cut running along (nx, ny) placed so that the share
    // of the area on one side is `fraction`.
    //
    // The position is found by bisection over the cut's offset rather than by
    // a ratio along an edge, because only the offset has a monotone relation
    // to the area it cuts off - which is exactly what makes it searchable.
    const auto place = [&](const double nx, const double ny, const double fraction, std::vector<Polygon> &halves, geom2::Point &from, geom2::Point &to) {
      const double sx = -ny, sy = nx;
      double lo = 1e300, hi = -1e300;

      for (const geom2::Point &p : piece.outer)
      {
        const double t = geom2::toMetres(p.x) * sx + geom2::toMetres(p.y) * sy;

        lo = std::min(lo, t);
        hi = std::max(hi, t);
      }

      if (hi - lo < 1e-6)
      {
        return false;
      }

      bool found = false;

      for (int step = 0; step < 24; ++step)
      {
        const double mid = (lo + hi) * .5;

        geom2::Point a, b;
        std::vector<Polygon> pieces;

        if (!chordThrough(piece.outer, sx * mid, sy * mid, nx, ny, a, b) || !geom2::cutPolygon(piece, a, b, pieces, nullptr) || pieces.size() < 2)
        {
          // The chord missed or produced one piece: the line is outside the
          // polygon on this side, so move toward the middle of what is left.
          lo = lo + (hi - lo) * .25;
          hi = hi - (hi - lo) * .25;

          continue;
        }

        // The share on the low side of the sweep.
        double low = 0, total = 0;

        for (const Polygon &part : pieces)
        {
          geom2::Point c;
          const double partArea = geom2::areaMetres(part);

          total += partArea;

          if (geom2::centroid(part, c) && geom2::toMetres(c.x) * sx + geom2::toMetres(c.y) * sy < mid)
          {
            low += partArea;
          }
        }

        if (total < 1e-9)
        {
          return false;
        }

        halves = std::move(pieces);
        from = a;
        to = b;
        found = true;

        if (low / total < fraction)
        {
          lo = mid;
        }
        else
        {
          hi = mid;
        }
      }

      return found && halves.size() >= 2;
    };

    // The ribbon comes off both halves before anything is measured, as
    // Polygon.cut(p1, p2, gap) does - half1.peel(point2, gap/2). Without it
    // the recursion measures halves the arrangement will never produce, and a
    // 40 m2 half comes out as a 12 m2 lot once an alley is taken off each of
    // its sides.
    const auto peel = [&](std::vector<Polygon> &halves, const geom2::Point &from, const geom2::Point &to) {
      if (gap <= 0)
      {
        return;
      }

      const double nx = geom2::toMetres(to.y - from.y), ny = -geom2::toMetres(to.x - from.x);
      const double length = std::hypot(nx, ny);

      if (length < 1e-9)
      {
        return;
      }

      for (Polygon &half : halves)
      {
        geom2::Point c;

        if (!geom2::centroid(half, c))
        {
          continue;
        }

        const double side = ((geom2::toMetres(c.x - from.x)) * nx + (geom2::toMetres(c.y - from.y)) * ny) < 0 ? -1.0 : 1.0;
        const double ox = nx / length * gap * .5 * side, oy = ny / length * gap * .5 * side;

        std::vector<Polygon> peeled;

        if (!geom2::cutPolygon(half, pointAt(geom2::toMetres(from.x) + ox, geom2::toMetres(from.y) + oy), pointAt(geom2::toMetres(to.x) + ox, geom2::toMetres(to.y) + oy), peeled, nullptr) || peeled.size() < 2)
        {
          continue;
        }

        size_t largest = 0;

        for (size_t i = 1; i < peeled.size(); ++i)
        {
          if (geom2::areaMetres(peeled[i]) > geom2::areaMetres(peeled[largest]))
          {
            largest = i;
          }
        }

        half = peeled[largest];
      }
    };

    // Rule 2, on area rather than on length: the cut halves the piece, varied
    // by a fraction of the area so lots differ without any of them being an
    // accident of the shape they were cut from.
    const double fraction = .5 + (random() - .5) * settings_.areaVariation;

    // A piece far above the target is cut even when no candidate is good: a
    // block-sized lot reads as a mistake where a mediocre pair reads as a plot.
    const bool mustCut = area > minimumArea * 3;

    std::vector<Polygon> chosen;
    geom2::Point chosenFrom, chosenTo;
    double chosenScore = -1;

    for (const auto &[nx, ny] : directions)
    {
      std::vector<Polygon> halves;
      geom2::Point from, to;

      if (!place(nx, ny, fraction, halves, from, to))
      {
        continue;
      }

      peel(halves, from, to);

      // Acceptance and ranking are different questions, and mixing them was a
      // mistake worth naming: folding squareness into the floor as well as the
      // score lowered the bar for every candidate and let wedges back in - 12
      // to 37 on the same town - while fixing the terraces it was aimed at. A
      // gate says whether a cut may be taken at all; a score only orders the
      // cuts that may.
      //
      // The worse of the two halves is the score: one good lot and one wedge
      // is a bad cut, and averaging would hide that.
      double score = 1e300;
      bool usable = true;

      for (const Polygon &half : halves)
      {
        if (geom2::areaMetres(half) < settings_.minimumLotMetres)
        {
          usable = false;
          break;
        }

        double fill = 0, narrow = 0, wide = 0;
        shapeOf(half, nx, ny, fill, narrow, wide);

        // The gates: not a sliver, and not a wedge.
        if (narrow < settings_.minimumWidthMetres || (!mustCut && fill < settings_.minimumFillFraction))
        {
          usable = false;
          break;
        }

        // The score: fill says the piece is not a wedge, squareness that it is
        // not a terrace, and a lot needs to be neither.
        const double squareness = wide > 1e-9 ? narrow / wide : 0.0;

        score = std::min(score, fill * (1.0 - settings_.squarenessWeight + settings_.squarenessWeight * squareness));
      }

      if (!usable || score <= chosenScore)
      {
        continue;
      }

      chosen = std::move(halves);
      chosenFrom = from;
      chosenTo = to;
      chosenScore = score;
    }

    // Nothing cuts this piece acceptably, so it is not cut: what would have
    // been two poor lots stays one good one. That is the merge - done before
    // the bad lots exist rather than by stitching them back together
    // afterwards, which needs no adjacency graph and cannot leave a hole.
    if (chosenScore < 0)
    {
      return;
    }

    // Would either half be landlocked? Then the cut is an alley, and both
    // halves front it. Re-placed rather than patched, because opening the gap
    // takes a ribbon off both halves and so changes the shapes that were
    // measured.
    if (gap <= 0)
    {
      bool reachable = true;

      for (const Polygon &half : chosen)
      {
        reachable = reachable && fronts(half, streets);
      }

      if (!reachable)
      {
        gap = settings_.alleyMetres;

        std::vector<Polygon> halves;
        geom2::Point from, to;

        // The direction that was chosen, recovered from its chord.
        const double cx = geom2::toMetres(chosenTo.x - chosenFrom.x), cy = geom2::toMetres(chosenTo.y - chosenFrom.y);
        const double clength = std::hypot(cx, cy);

        if (clength > 1e-9 && place(cx / clength, cy / clength, fraction, halves, from, to))
        {
          peel(halves, from, to);

          bool usable = halves.size() >= 2;

          for (const Polygon &half : halves)
          {
            usable = usable && geom2::areaMetres(half) >= settings_.minimumLotMetres;
          }

          if (usable)
          {
            chosen = std::move(halves);
            chosenFrom = from;
            chosenTo = to;
          }
          else
          {
            // No room for an alley here: leave the piece whole rather than
            // cut a lot nothing can reach.
            return;
          }
        }
        else
        {
          return;
        }
      }
    }

    Edge edge;
    edge.path = {chosenFrom, chosenTo};
    edge.roles.push_back({EdgeRole::LotBoundary, "lots.bisection", ++cuts});

    if (gap > 0)
    {
      edge.bands.push_back({-gap * .5, gap * .5, Cover::Road, street ? "street" : "alley"});
    }

    out.push_back(std::move(edge));

    std::vector<Frontage> onward = streets;

    if (gap > 0)
    {
      onward.push_back({geom2::toMetres(chosenFrom.x), geom2::toMetres(chosenFrom.y), geom2::toMetres(chosenTo.x), geom2::toMetres(chosenTo.y), gap * .5});
    }

    for (const auto &half : chosen)
    {
      // Watabou's own condition, verbatim: the next level splits with an alley
      // only while the half is large relative to a product of two draws, which
      // is usually small - so alleys separate groups, not neighbours.
      const double threshold = settings_.minimumAreaMetres / std::max(1e-6, random() * random());

      bisect(half, depth + 1, geom2::areaMetres(half) > threshold, onward);
    }
  };

  bisect(request.region.boundary, 0, true, blockStreets);

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, region, "lots.bisection: block already at target size");
  }

  return true;
}

Descriptor RoadsideHouses::descriptor() const
{
  return {"buildings.roadside", Role::Buildings, Scope::Region, 1, true};
}

bool RoadsideHouses::populate(const Request &request, std::vector<Object> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (request.region.cover == Cover::Road || request.region.cover == Cover::Water)
  {
    return true;
  }

  const Ring &outline = request.region.boundary.outer;

  if (outline.size() < 3u)
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "buildings.roadside: the parcel has no frontage to build against");

    return true;
  }

  if (!(settings_.frontageMetres > 0) || !(settings_.depthMetres > 0))
  {
    if (error != nullptr)
    {
      *error = "roadside houses need a positive frontage and depth";
    }

    return false;
  }

  // Which of the parcel's own edges are road frontage.
  //
  // A portal records the neighbour across a boundary, so an edge whose
  // neighbour is a road is frontage and an edge onto open ground is not. When
  // the parcel has no portals at all - the gallery hands one straight to the
  // generator - every edge is treated as frontage, because a parcel drawn on
  // its own is a parcel on a lane.
  std::vector<bool> frontage(outline.size(), request.region.portals.empty());

  for (const Portal &portal : request.region.portals)
  {
    const Region *neighbour = request.tree.find(portal.neighbour);

    if (neighbour == nullptr || neighbour->cover != Cover::Road)
    {
      continue;
    }

    // The portal names a midpoint; the frontage edge is the one it lies on.
    double nearest = 1e300;
    size_t best = 0;

    for (size_t i = 0; i < outline.size(); ++i)
    {
      const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
      const double bx = geom2::toMetres(outline[(i + 1) % outline.size()].x), by = geom2::toMetres(outline[(i + 1) % outline.size()].y);
      const double mx = (ax + bx) * .5, my = (ay + by) * .5;
      const double distance = std::hypot(geom2::toMetres(portal.midpoint.x) - mx, geom2::toMetres(portal.midpoint.y) - my);

      if (distance < nearest)
      {
        nearest = distance;
        best = i;
      }
    }

    frontage[best] = true;
  }

  const double step = settings_.frontageMetres + std::max(0.0, settings_.spacingMetres);
  uint64_t placed = 0;

  for (size_t i = 0; i < outline.size(); ++i)
  {
    if (!frontage[i])
    {
      continue;
    }

    const double ax = geom2::toMetres(outline[i].x), ay = geom2::toMetres(outline[i].y);
    const double bx = geom2::toMetres(outline[(i + 1) % outline.size()].x), by = geom2::toMetres(outline[(i + 1) % outline.size()].y);
    const double length = std::hypot(bx - ax, by - ay);

    if (length < settings_.frontageMetres)
    {
      continue;
    }

    // Along the road, and into the parcel. The boundary is counter clockwise,
    // so the inward normal is to the left of the edge.
    const double ux = (bx - ax) / length, uy = (by - ay) / length;
    const double nx = -uy, ny = ux;

    const int houses = std::max(1, int(std::floor(length / step)));
    const double margin = (length - double(houses) * step + step - settings_.frontageMetres) * .5;

    for (int k = 0; k < houses; ++k)
    {
      const uint64_t id = ++placed;

      // Nudged along the lane and in its setback, so a row of houses is a row
      // of houses rather than a terrace of identical ones.
      const double slide = (request.context.unitRandom("buildings.roadside", request.region.id, 1, id) - .5) * settings_.jitter * settings_.spacingMetres;
      const double back = settings_.setbackMetres * (1.0 + (request.context.unitRandom("buildings.roadside", request.region.id, 2, id) - .5) * settings_.jitter);

      const double from = margin + double(k) * step + slide;

      if (from < 0 || from + settings_.frontageMetres > length)
      {
        continue;
      }

      // The house, square to its own road: frontage along the edge, depth away
      // from it, starting at the setback.
      const double x0 = ax + ux * from, y0 = ay + uy * from;

      Ring shell;
      shell.push_back(pointAt(x0 + nx * back, y0 + ny * back));
      shell.push_back(pointAt(x0 + ux * settings_.frontageMetres + nx * back, y0 + uy * settings_.frontageMetres + ny * back));
      shell.push_back(pointAt(x0 + ux * settings_.frontageMetres + nx * (back + settings_.depthMetres), y0 + uy * settings_.frontageMetres + ny * (back + settings_.depthMetres)));
      shell.push_back(pointAt(x0 + nx * (back + settings_.depthMetres), y0 + ny * (back + settings_.depthMetres)));

      Polygon house;
      house.outer = shell;

      // Only what actually fits inside the parcel. A house half in a field is
      // not a house, and clipping is what lets this run on a parcel of any
      // shape rather than only on a rectangle.
      std::vector<Polygon> kept;

      if (!geom2::booleanOp({geom2::toOperand(house), geom2::toOperand(request.region.boundary)}, geom2::Operation::Intersection, {}, kept, error))
      {
        return false;
      }

      if (kept.empty() || geom2::areaMetres(kept.front()) < settings_.frontageMetres * settings_.depthMetres * .6)
      {
        continue;
      }

      // A corner belongs to one house, not to two.
      //
      // Each frontage is walked independently, so where two lanes meet the
      // house placed against one edge and the house placed against the other
      // both claim the corner and the render shows them overlapping. Dropping
      // the later of the two is the whole rule - the first frontage walked
      // keeps the corner.
      bool clashes = false;

      for (const Object &existing : out)
      {
        if (existing.kind != "house")
        {
          continue;
        }

        std::vector<Polygon> overlap;

        if (geom2::booleanOp({geom2::toOperand(kept.front()), geom2::toOperand(existing.footprint)}, geom2::Operation::Intersection, {}, overlap, nullptr) && !overlap.empty())
        {
          clashes = true;
          break;
        }
      }

      if (clashes)
      {
        continue;
      }

      const double draw = request.context.unitRandom("buildings.roadside", request.region.id, 3, id);

      Object building;
      building.kind = "house";
      building.footprint = kept.front();
      building.anchor = pointAt(x0 + ux * settings_.frontageMetres * .5 + nx * (back + settings_.depthMetres * .5), y0 + uy * settings_.frontageMetres * .5 + ny * (back + settings_.depthMetres * .5));
      building.heightMetres = settings_.storeyMetres * (draw < .8 ? 1.0 : 2.0);
      building.region = request.region.id;
      out.push_back(std::move(building));

      // The driveway: a strip from the middle of the frontage back to the
      // road. Watabou's villages draw these as secondary roads, and without
      // them a set-back house has an aspect onto the lane but no access to it.
      if (!(settings_.drivewayWidthMetres > 0) || !(back > 0))
      {
        continue;
      }

      const double cx = x0 + ux * settings_.frontageMetres * .5, cy = y0 + uy * settings_.frontageMetres * .5;
      const double half = settings_.drivewayWidthMetres * .5;

      Ring drive;
      drive.push_back(pointAt(cx - ux * half, cy - uy * half));
      drive.push_back(pointAt(cx + ux * half, cy + uy * half));
      drive.push_back(pointAt(cx + ux * half + nx * back, cy + uy * half + ny * back));
      drive.push_back(pointAt(cx - ux * half + nx * back, cy - uy * half + ny * back));

      Polygon driveway;
      driveway.outer = drive;

      std::vector<Polygon> drivewayKept;

      if (geom2::booleanOp({geom2::toOperand(driveway), geom2::toOperand(request.region.boundary)}, geom2::Operation::Intersection, {}, drivewayKept, nullptr) && !drivewayKept.empty())
      {
        Object access;
        access.kind = "driveway";
        access.footprint = drivewayKept.front();
        access.anchor = pointAt(cx + nx * back * .5, cy + ny * back * .5);
        access.heightMetres = 0;
        access.region = request.region.id;
        out.push_back(std::move(access));
      }
    }
  }

  if (out.empty())
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "buildings.roadside: no frontage long enough for a house");
  }

  return true;
}

Descriptor HouseFootprints::descriptor() const
{
  return {settings_.ward.empty() ? std::string("buildings.setback") : "buildings.setback." + settings_.ward, Role::Buildings, Scope::Region, 1, true};
}

bool HouseFootprints::populate(const Request &request, std::vector<Object> &out, Ledger &ledger, std::string *error)
{
  out.clear();

  if (request.region.cover == Cover::Road || request.region.cover == Cover::Water)
  {
    // Not a mistake to skip: a road is a region like any other and simply
    // holds no houses.
    return true;
  }

  if (request.region.areaMetres() < settings_.minimumAreaMetres)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "buildings.setback: lot below the minimum area");

    return true;
  }

  if (settings_.maximumAreaMetres > 0 && request.region.areaMetres() > settings_.maximumAreaMetres)
  {
    ledger.add(LedgerKind::Omitted, request.region.id, "buildings.setback: above the largest plot allowed");

    return true;
  }

  const Region *parent = request.tree.find(request.region.parent);

  // The court of a perimeter block is common ground.
  //
  // A court is the piece that does not touch the block's own boundary. That is
  // what a perimeter block *means*: the lots are around the edge and the court
  // is what they enclose, reached through them. It is a structural test, not a
  // threshold, so there is nothing to tune and nothing to get backwards. The
  // region's *own* producer names the rule that made it; testing the parent's
  // was silently inert, because a ring lot's parent is the block.
  if (parent != nullptr && request.region.producer.rfind("lots.ring", 0) == 0)
  {
    if (!liesAlong(request.region.boundary, parent->boundary.outer, .25, 1.2))
    {
      ledger.add(LedgerKind::Omitted, request.region.id, "buildings.setback: the court of a perimeter block");

      return true;
    }
  }

  // ---- the brief ------------------------------------------------------------
  //
  // This generator used to be the whole story: inset the lot, keep the piece,
  // then a coverage cap, then a yard, then a fence, then outbuildings, each a
  // special case of the last. It is now an adapter. The plot is described -
  // its ground, its street, its standing - and the lot planner decides how
  // the ground is used; this only turns the answer into objects.
  namespace gen = procedural::world::generator;
  using procedural::world::rewrite::Position;

  gen::PlotBrief brief;
  brief.id = request.region.id;
  brief.seed = request.context.worldSeed;
  brief.standing = settings_.standing;
  gen::describeStanding(brief);

  // The settings override the standing where they say something.
  if (settings_.maximumCoverage > 0 && settings_.maximumCoverage < 1)
  {
    brief.houseShare = settings_.maximumCoverage;
  }

  if (settings_.minimumStoreys > 0)
  {
    brief.minimumStoreys = settings_.minimumStoreys;
  }

  if (settings_.maximumStoreys > 0)
  {
    brief.maximumStoreys = std::max(brief.minimumStoreys, settings_.maximumStoreys);
  }

  // The standing's own setback, capped by the settings and scaled down on a
  // small lot - a flat metre and a half is a yard on a large plot and most
  // of the ground on a small one.
  brief.setbackMetres = std::min({brief.setbackMetres, settings_.setbackMetres, std::sqrt(std::max(request.region.areaMetres(), 1.0)) * .12});
  brief.storeyMetres = settings_.storeyMetres;

  for (const geom2::Point &q : request.region.boundary.outer)
  {
    brief.lot.push_back(Position{geom2::toMetres(q.x), 0, geom2::toMetres(q.y)});
  }

  for (const Ring &hole : request.region.boundary.holes)
  {
    std::vector<Position> ring;

    for (const geom2::Point &q : hole)
    {
      ring.push_back(Position{geom2::toMetres(q.x), 0, geom2::toMetres(q.y)});
    }

    brief.holes.push_back(std::move(ring));
  }

  // Which edges are street.
  //
  // A portal records the neighbour across a boundary, so an edge whose
  // neighbour is a road is frontage. A lot inside a block has no road portals
  // - the roads are the block's edges, one level up - so there the frontage
  // is every lot edge lying along the block's own boundary, which is the line
  // the streets were drawn on. The old rule, "the longest edge of the lot",
  // was a guess, and on a deep narrow lot it put the house side-on to the
  // street.
  const Ring &outline = request.region.boundary.outer;

  for (size_t i = 0; i < outline.size(); ++i)
  {
    const geom2::Point &p = outline[i], &q = outline[(i + 1) % outline.size()];
    bool street = false;

    for (const Portal &portal : request.region.portals)
    {
      const Region *neighbour = request.tree.find(portal.neighbour);

      if (neighbour == nullptr || neighbour->cover != Cover::Road)
      {
        continue;
      }

      const double mx = (geom2::toMetres(p.x) + geom2::toMetres(q.x)) * .5, my = (geom2::toMetres(p.y) + geom2::toMetres(q.y)) * .5;

      if (std::hypot(geom2::toMetres(portal.midpoint.x) - mx, geom2::toMetres(portal.midpoint.y) - my) < 1.0)
      {
        street = true;
      }
    }

    if (!street && parent != nullptr)
    {
      Polygon edge;
      edge.outer = {p, q, q, p};
      street = liesAlong(edge, parent->boundary.outer, .25, 1.2);
    }

    if (street)
    {
      brief.frontage.push_back({Position{geom2::toMetres(p.x), 0, geom2::toMetres(p.y)}, Position{geom2::toMetres(q.x), 0, geom2::toMetres(q.y)}});
    }
  }

  gen::ComplexDwellingSettings massing;
  gen::PlotPlan plan;

  if (!gen::planPlot(brief, massing, plan, error))
  {
    return false;
  }

  if (plan.house.empty())
  {
    ledger.add(LedgerKind::Rejected, request.region.id, "buildings.setback: " + plan.note);

    return true;
  }

  const auto emitPolygon = [&](const char *kind, const std::vector<Position> &ring, const std::vector<std::vector<Position>> &holes, const double height) {
    Object object;
    object.kind = kind;
    object.heightMetres = height;
    object.region = request.region.id;

    for (const Position &q : ring)
    {
      object.footprint.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
    }

    for (const auto &hole : holes)
    {
      Ring inner;

      for (const Position &q : hole)
      {
        inner.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (inner.size() >= 3)
      {
        object.footprint.holes.push_back(std::move(inner));
      }
    }

    if (object.footprint.outer.size() < 3 || !geom2::polygonFault(object.footprint).empty())
    {
      return;
    }

    object.anchor = object.footprint.outer.front();
    out.push_back(std::move(object));
  };

  const auto emit = [&](const char *kind, const std::vector<Position> &ring, const double height) { emitPolygon(kind, ring, {}, height); };
  const auto emitGround = [&](const char *kind, const gen::PlotGround &ground) { emitPolygon(kind, ground.outer, ground.holes, 0); };

  // The built form, carried rather than re-derived.
  //
  // What draws the town used to mass each building again from its footprint,
  // which meant the standing table lived in two places and the two drifted.
  // The layer that plans the building is the layer that knows its geometry.
  const auto solidOf = [&](const gen::PlotBuilding &built) {
    auto solid = std::make_shared<Solid>();

    const auto band = [&](const std::vector<Position> &ring, const double base, const double top, std::vector<Solid::Face> &into) {
      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Position &a = ring[i];
        const Position &c = ring[(i + 1) % ring.size()];

        if (std::hypot(c.x - a.x, c.z - a.z) < 1e-6)
        {
          continue;
        }

        into.push_back({{a.x, base, a.z}, {c.x, base, c.z}, {c.x, top, c.z}, {a.x, top, a.z}});
      }
    };

    for (const auto &level : built.levels)
    {
      band(level.outer, level.base, level.top, solid->walls);

      for (const auto &hole : level.holes)
      {
        band(hole, level.base, level.top, solid->innerWalls);
      }
    }

    for (const auto &face : built.roofs)
    {
      Solid::Face out3;

      for (const Position &q : face.polygon)
      {
        out3.push_back({q.x, q.y, q.z});
      }

      (face.soffit ? solid->soffits : face.gable ? solid->gables : solid->slopes).push_back(std::move(out3));
    }

    for (const auto &post : built.pillars)
    {
      band(post.footprint, post.base, post.top, solid->pillars);

      Solid::Face cap;

      for (const Position &q : post.footprint)
      {
        cap.push_back({q.x, post.top, q.z});
      }

      solid->pillars.push_back(std::move(cap));
    }

    return solid;
  };

  // --- the interior, where one was asked for --------------------------------
  //
  // The programme generator plans the rooms inside the footprint the plot
  // planner chose and re-masses the exterior onto the plan it made, so the
  // walls that get built are the walls of the rooms. Where it declines - a
  // programme that will not fit the ground it was given - the shell the plot
  // planner composed stands, which is what the town had before.
  gen::ProgrammeBuilding programmeBuilding;
  rewrite::DwellingPlan interior;
  bool furnished = false;

  if (settings_.interior != Settings::Interior::None && plan.house.size() >= 3)
  {
    gen::ProgrammeBrief programme;
    programme.building = settings_.building;
    programme.standing = brief.standing;
    programme.storeys = std::max(1u, plan.storeys);

    gen::ProgrammePlanSettings rooms;
    gen::FloorPlanSettings optimise;

    furnished = gen::planProgrammeInside(
      plan.house, programme, brief.seed ^ 0x5DEECE66Dull, rooms, massing, programmeBuilding,
      interior, nullptr,
      settings_.interior == Settings::Interior::ProgrammeOptimised ? &optimise : nullptr);

    if (!furnished)
    {
      ledger.add(LedgerKind::Omitted, request.region.id,
                 "buildings.setback: no programme fits the house");
    }
  }

  if (furnished)
  {
    // The plan's own storeys and roof, and its partitions: the exterior the
    // programme re-massed onto its rooms rather than the one composed before
    // them.
    gen::PlotBuilding planned;
    planned.wings = interior.wings;
    planned.roofs = interior.roofs;
    planned.levels = interior.storeys;
    planned.pillars = interior.pillars;

    std::vector<Position> outline;

    for (const auto &corner : programmeBuilding.footprint)
    {
      outline.push_back({corner.first, 0, corner.second});
    }

    emitPolygon("house", outline.size() >= 3 ? outline : plan.house, {},
                settings_.storeyMetres * double(std::max<size_t>(1, interior.storeys.size())));

    if (!out.empty() && out.back().kind == "house")
    {
      auto solid = solidOf(planned);

      // The partitions, which is what a house with an interior has that a
      // shell does not.
      for (const rewrite::DwellingWall &wall : interior.walls)
      {
        if (wall.exterior)
        {
          continue;
        }

        solid->innerWalls.push_back({{wall.a.x, wall.a.y, wall.a.z},
                                     {wall.b.x, wall.b.y, wall.b.z},
                                     {wall.b.x, wall.b.y + 2.4, wall.b.z},
                                     {wall.a.x, wall.a.y + 2.4, wall.a.z}});
      }

      out.back().solid = std::move(solid);
    }
  }
  else
  {
    emitPolygon("house", plan.house, plan.courts,
                settings_.storeyMetres * double(plan.storeys));

    if (!out.empty() && out.back().kind == "house")
    {
      out.back().solid = solidOf(plan.built);
    }
  }

  for (size_t i = 0; i < plan.outbuildings.size(); ++i)
  {
    emit(brief.fenced ? "barn" : "outbuilding", plan.outbuildings[i], settings_.storeyMetres * (brief.fenced ? 1.4 : 1.0));

    if (!out.empty() && i < plan.sheds.size() && (out.back().kind == "barn" || out.back().kind == "outbuilding"))
    {
      out.back().solid = solidOf(plan.sheds[i]);
    }
  }

  for (const auto &piece : plan.garden)
  {
    emitGround("garden", piece);
  }

  for (const auto &piece : plan.yard)
  {
    emitGround(brief.fenced ? "paddock" : "yard", piece);
  }

  for (const auto &piece : plan.fence)
  {
    emitPolygon("fence", piece.outer, piece.holes, settings_.fenceHeightMetres);
  }

  for (const auto &piece : plan.path)
  {
    emitGround("path", piece);
  }

  return true;
}

Descriptor SlopeField::descriptor() const
{
  return {"terrain.slope", Role::Terrain, Scope::Region, 1, true};
}

bool SlopeField::evaluate(const RegionTree &, const Context &context, std::vector<Region *> &regions, Ledger &ledger, std::string *)
{
  if (!context.terrain)
  {
    for (auto *region : regions)
    {
      if (region != nullptr)
      {
        ledger.add(LedgerKind::Omitted, region->id, "terrain.slope: no terrain supplied");
      }
    }

    return true;
  }

  for (auto *region : regions)
  {
    if (region == nullptr || region->boundary.outer.size() < 3)
    {
      continue;
    }

    // Sampled at the ring vertices, which is enough to say whether a region is
    // buildable without meshing it.
    double lowest = 0;
    double highest = 0;
    bool first = true;

    for (const auto &p : region->boundary.outer)
    {
      const double height = context.terrain(geom2::toMetres(p.x), geom2::toMetres(p.y));

      if (first || height < lowest)
      {
        lowest = height;
      }

      if (first || height > highest)
      {
        highest = height;
      }

      first = false;
    }

    const geom2::Box box = geom2::bounds(region->boundary);
    const double span = std::max(1.0e-6, std::hypot(geom2::toMetres(box.maxX - box.minX), geom2::toMetres(box.maxY - box.minY)));

    region->attributes.elevationMetres = (lowest + highest) * .5;
    region->attributes.slope = (highest - lowest) / span;
    region->attributes.buildable = region->attributes.slope <= settings_.unbuildableSlope;

    if (!region->attributes.buildable)
    {
      ledger.add(LedgerKind::Capacity, region->id, "terrain.slope: too steep to build on");
    }
  }

  return true;
}

} // namespace procedural::world::hierarchy
