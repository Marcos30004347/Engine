#include "Skeleton.hpp"

// For triangulating a face. A roof is three-dimensional and this file really
// belongs beside the prism rather than in the 2D layer; the include runs the
// right way round (geom3 knows geom2, not the reverse) but the home is wrong.
#include "procedural/geom3/Prism.hpp"
#include "Polygon.hpp"
#include "Quantise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace procedural::geom2 {

namespace {

// Footprints use the shared micrometre grid. Intersections need a finer local
// grid: rounding a bisector to a footprint unit can bend a micrometre-wide
// cell into a steep fin. This grid only identifies vertices in this roof; it
// does not change the world-format quantisation.
constexpr double roofUnitsPerMetre = 1e9;

int64_t roofUnit(double x)
{
  return int64_t(std::llround(x * roofUnitsPerMetre));
}

double roofMetres(int64_t x)
{
  return double(x) / roofUnitsPerMetre;
}

// A wall of the plan, and with it the plane of the roof above it.
//
// The roof over a wall is the plane containing that wall and rising inward at
// the pitch. Before the pitch is applied its height at a point is the distance
// from the point to the wall's own line, measured inward - one dot product, and
// the only arithmetic this construction needs.
struct Wall
{
  double ax = 0, az = 0; // Start, in metres.
  double bx = 0, bz = 0; // End.
  double ux = 0, uz = 0; // Unit direction, a to b.
  double nx = 0, nz = 0; // Unit normal, into the building.

  // A wall the roof rises *to* rather than falls from: a gable end, or a party
  // wall shared with a taller mass. It has no face of its own and is simply
  // left out of the competition below, which is the whole of what a gable is
  // here.
  bool raised = false;
  uint32_t source = 0;
};

double riseOver(const Wall &w, const double px, const double pz)
{
  return (px - w.ax) * w.nx + (pz - w.az) * w.nz;
}

// Sutherland and Hodgman, clipping a convex polygon by the half plane a.p <= c.
//
// Convex in, convex out, so it is exact and needs no boolean. Everything the
// envelope below asks for is an intersection of half planes, which is convex
// however many of them there are.
void clipTo(std::vector<std::pair<double, double>> &poly, const double ax, const double az, const double c)
{
  if (poly.size() < 3)
  {
    poly.clear();

    return;
  }

  std::vector<std::pair<double, double>> kept;
  kept.reserve(poly.size() + 4);

  for (size_t i = 0; i < poly.size(); ++i)
  {
    const auto &p = poly[i];
    const auto &q = poly[(i + 1) % poly.size()];
    const double dp = ax * p.first + az * p.second - c;
    const double dq = ax * q.first + az * q.second - c;

    if (dp <= 0)
    {
      kept.push_back(p);
    }

    if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
    {
      const double t = dp / (dp - dq);

      kept.push_back({p.first + (q.first - p.first) * t, p.second + (q.second - p.second) * t});
    }
  }

  poly.swap(kept);

  if (poly.size() < 3)
  {
    poly.clear();
  }
}

// For orthogonal plans the inward wavefront is the boundary's L-infinity
// distance field. A finite eave contributes max(normal distance, distance
// beyond either endpoint); the roof is the minimum over all eaves. These
// continuous piecewise-linear functions need no visibility classification.
// Omitting a gabled eave retains continuity at reflex corners as well.
bool roofSubdivision(const Polygon &ground, std::vector<Wall> &walls, std::vector<std::vector<Polygon>> &pieces)
{
  bool convex = ground.holes.empty();
  for (size_t i = 0; i < ground.outer.size(); ++i)
  {
    convex = convex && orientation(ground.outer[i], ground.outer[(i + 1) % ground.outer.size()], ground.outer[(i + 2) % ground.outer.size()]) >= 0;
  }

  using Loop = std::vector<std::pair<double, double>>;

  struct Cell
  {
    Loop loop;
    size_t plane;
  };

  std::vector<Cell> cells;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Point> vertices;
  if (!geom3::triangulate(ground, triangles, vertices, nullptr))
  {
    return false;
  }
  for (const auto &t : triangles)
  {
    Loop loop;
    for (auto i : t)
    {
      loop.emplace_back(toMetres(vertices[i].x), toMetres(vertices[i].y));
    }
    cells.push_back({std::move(loop), size_t(-1)});
  }

  const size_t boundaryCount = walls.size();
  const auto cut = [&](Loop loop, const size_t a, const size_t b) {
    const Wall &p = walls[a], &q = walls[b];
    clipTo(loop, p.nx - q.nx, p.nz - q.nz, p.nx * p.ax + p.nz * p.az - q.nx * q.ax - q.nz * q.az);
    return loop;
  };
  const auto hasArea = [](const Loop &loop) {
    if (loop.size() < 3)
    {
      return false;
    }
    double twice = 0;
    const auto origin = loop.front();
    for (size_t i = 0; i < loop.size(); ++i)
    {
      const auto &a = loop[i], &b = loop[(i + 1) % loop.size()];
      twice += (a.first - origin.first) * (b.second - origin.second) - (a.second - origin.second) * (b.first - origin.first);
    }
    return std::abs(twice) > 1e-12;
  };

  for (size_t e = 0; e < boundaryCount; ++e)
  {
    const Wall w = walls[e];
    if (w.raised)
    {
      continue;
    }
    const size_t back = walls.size();
    Wall p = w;
    p.raised = false;
    p.nx = -w.nx;
    p.nz = -w.nz;
    walls.push_back(p);
    p.nx = -w.ux;
    p.nz = -w.uz;
    walls.push_back(p);
    p.ax = w.bx;
    p.az = w.bz;
    p.nx = w.ux;
    p.nz = w.uz;
    walls.push_back(p);
    const std::vector<size_t> planes = convex ? std::vector<size_t>{e} : std::vector<size_t>{e, back, back + 1, back + 2};
    std::vector<Cell> next;
    for (const Cell &cell : cells)
    {
      Loop winning = cell.loop;
      if (cell.plane != size_t(-1))
      {
        // If a single branch is already above the old face throughout this
        // cell, the new eave cannot win anywhere. This also resolves coplanar
        // ties without introducing irrelevant subdivision edges.
        bool dominated = false;
        for (const size_t plane : planes)
        {
          bool above = true;
          for (const auto &q : cell.loop)
          {
            above = above && riseOver(walls[plane], q.first, q.second) >= riseOver(walls[cell.plane], q.first, q.second) - 1e-12;
          }
          dominated = dominated || above;
        }
        if (dominated)
        {
          next.push_back(cell);
          continue;
        }
        // The new max-of-planes wins only where every branch is below the
        // old face. Peel off the complementary convex regions one at a time;
        // this partitions the cell instead of independently claiming it.
        for (const size_t plane : planes)
        {
          Loop retained = cut(winning, cell.plane, plane);
          if (hasArea(retained))
          {
            next.push_back({std::move(retained), cell.plane});
          }
          winning = cut(std::move(winning), plane, cell.plane);
          if (!hasArea(winning))
          {
            break;
          }
        }
      }
      if (!hasArea(winning))
      {
        continue;
      }
      for (const size_t plane : planes)
      {
        Loop domain = winning;
        for (const size_t other : planes)
        {
          if (other != plane)
          {
            domain = cut(std::move(domain), other, plane);
          }
        }
        if (hasArea(domain))
        {
          next.push_back({std::move(domain), plane});
        }
      }
    }
    cells.swap(next);
  }
  pieces.resize(walls.size());
  for (const Cell &cell : cells)
  {
    Polygon part;
    for (const auto &q : cell.loop)
    {
      Point p{roofUnit(q.first), roofUnit(q.second)};
      if (part.outer.empty() || p != part.outer.back())
      {
        part.outer.push_back(p);
      }
    }
    if (part.outer.size() > 1 && part.outer.front() == part.outer.back())
    {
      part.outer.pop_back();
    }
    if (part.outer.size() < 3 || doubleSignedArea(part.outer) == 0)
    {
      continue;
    }
    if (!counterClockwise(part.outer))
    {
      reverse(part.outer);
    }
    // Cells were obtained by convex clipping; only a pinched ring introduced
    // by quantisation is invalid here. Coordinates are roof-grid units, not
    // footprint-grid units, so polygonFault's footprint bounds do not apply.
    std::set<Point> unique(part.outer.begin(), part.outer.end());
    if (unique.size() != part.outer.size())
    {
      return false;
    }
    pieces[cell.plane].push_back(std::move(part));
  }
  return true;
}

} // namespace

bool roofFromSkeleton(const Polygon &ground, const RoofOptions &options, std::vector<RoofTriangle> &out, std::string *error)
{
  out.clear();
  if (error)
  {
    error->clear();
  }
  if (!std::isfinite(options.slope) || !std::isfinite(options.ridgeHeight) || options.ridgeHeight < 0 || (options.ridgeHeight == 0 && options.slope <= 0))
  {
    if (error)
    {
      *error = "roof pitch or ridge height is invalid";
    }
    return false;
  }

  if (ground.outer.size() < 3)
  {
    if (error != nullptr)
    {
      *error = "a roof needs at least three edges";
    }

    return false;
  }

  const std::string fault = polygonFault(ground);

  if (!fault.empty())
  {
    if (error != nullptr)
    {
      *error = "the outline is not a simple polygon: " + fault;
    }

    return false;
  }

  if (std::abs(areaMetres(ground)) < 1e-6)
  {
    if (error != nullptr)
    {
      *error = "the outline has no area";
    }

    return false;
  }

  // ---- the walls --------------------------------------------------------
  //
  // Every ring: the outline, and any court the building wraps around. A court's
  // walls are walls, and the roof over a range between an outer wall and a
  // court is those two meeting - which needs no special case at all once both
  // are in the same list.
  std::vector<Wall> walls;

  {
    std::vector<const Ring *> rings{&ground.outer};

    for (const Ring &hole : ground.holes)
    {
      rings.push_back(&hole);
    }

    for (const Ring *ring : rings)
    {
      for (size_t i = 0; i < ring->size(); ++i)
      {
        Wall w;
        w.ax = toMetres((*ring)[i].x);
        w.az = toMetres((*ring)[i].y);
        w.bx = toMetres((*ring)[(i + 1) % ring->size()].x);
        w.bz = toMetres((*ring)[(i + 1) % ring->size()].y);

        const double length = std::hypot(w.bx - w.ax, w.bz - w.az);

        if (length < 1e-6)
        {
          continue;
        }

        w.ux = (w.bx - w.ax) / length;
        w.uz = (w.bz - w.az) / length;
        w.nx = -w.uz; // Counter-clockwise outer, clockwise hole: inside is left of both.
        w.nz = w.ux;

        w.source = uint32_t(walls.size());
        walls.push_back(w);
      }

    }
  }

  if (walls.size() < 3)
  {
    if (error != nullptr)
    {
      *error = "the outline has no usable edges";
    }

    return false;
  }

  // Gable boundaries carry vertical panels instead of contributing eaves.
  {
    size_t raised = 0;

    for (size_t i = 0; i < walls.size() && i < options.gables.size(); ++i)
    {
      walls[i].raised = options.gables[i];
      raised += options.gables[i] ? 1 : 0;
    }

    if (raised == walls.size())
    {
      if (error)
      {
        *error = "a roof needs at least one non-gabled eave";
      }
      return false;
    }
  }

  std::vector<std::vector<Polygon>> pieces(walls.size());
  if (!roofSubdivision(ground, walls, pieces))
  {
    if (error)
    {
      *error = "roof subdivision failed";
    }
    return false;
  }

  struct Facet
  {
    Polygon shape;
    size_t wall;
  };

  std::vector<Facet> facets;
  double peak = 0;
  for (size_t i = 0; i < pieces.size(); ++i)
  {
    for (Polygon &part : pieces[i])
    {
      for (const Point &q : part.outer)
      {
        peak = std::max(peak, riseOver(walls[i], roofMetres(q.x), roofMetres(q.y)));
      }
      facets.push_back({std::move(part), i});
    }
  }

  if (facets.empty())
  {
    if (error != nullptr)
    {
      *error = "the skeleton produced no faces";
    }

    return false;
  }

  const double pitch = options.ridgeHeight > 0 && peak > 1e-6 ? options.ridgeHeight / peak : options.slope;

  // The faces, made to agree about their shared edges before any of them is
  // triangulated.
  //
  // Two faces can share a line without sharing an edge. Where three faces meet,
  // one may run the whole length while the other two end part way along it:
  // every vertex is where it should be, the surface has no gap in it, and it
  // still comes apart when it is drawn, because a rasteriser walks each
  // triangle from its own vertices and two walks along the same line from
  // different endpoints do not land on the same pixels.
  //
  // So each face is given the vertices its neighbours end on. This is not a
  // repair - nothing moves, and the height over every point is what it was,
  // since a vertex inserted on an edge of a plane lies in that plane. It is
  // what makes the faces a subdivision rather than a heap of polygons, and it
  // has to happen before the triangulator sees them, because afterwards the
  // only thing left to do is cut triangles up and hope.
  {
    std::set<std::pair<int64_t, int64_t>> corners;

    for (const Facet &facet : facets)
    {
      for (const Point &q : facet.shape.outer)
      {
        corners.insert({q.x, q.y});
      }

      for (const Ring &hole : facet.shape.holes)
      {
        for (const Point &q : hole)
        {
          corners.insert({q.x, q.y});
        }
      }
    }

    const auto conform = [&](Ring &ring) {
      Ring grown;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Point &a = ring[i];
        const Point &b = ring[(i + 1) % ring.size()];

        grown.push_back(a);

        const double ax = roofMetres(a.x), az = roofMetres(a.y);
        const double bx = roofMetres(b.x), bz = roofMetres(b.y);
        const double ex = bx - ax, ez = bz - az;
        const double span = ex * ex + ez * ez;

        if (span < 1e-24)
        {
          continue;
        }

        std::vector<std::pair<double, Point>> on;

        for (const auto &corner : corners)
        {
          if ((corner.first == a.x && corner.second == a.y)
              || (corner.first == b.x && corner.second == b.y))
          {
            continue;
          }

          const double qx = roofMetres(corner.first), qz = roofMetres(corner.second);
          const double at = ((qx - ax) * ex + (qz - az) * ez) / span;

          if (at <= 1e-9 || at >= 1. - 1e-9)
          {
            continue;
          }

          if (std::hypot(qx - (ax + ex * at), qz - (az + ez * at)) > 2e-9)
          {
            continue;
          }

          on.push_back({at, Point{corner.first, corner.second}});
        }

        std::vector<size_t> order(on.size());

        for (size_t k = 0; k < order.size(); ++k)
        {
          order[k] = k;
        }

        std::sort(order.begin(), order.end(),
                  [&](const size_t l, const size_t r) { return on[l].first < on[r].first; });

        for (const size_t k : order)
        {
          grown.push_back(on[k].second);

        }
      }

      ring.clear();
      for (const Point &q : grown)
      {
        if (ring.empty() || q != ring.back())
        {
          ring.push_back(q);
        }
      }
      if (ring.size() > 1 && ring.front() == ring.back())
      {
        ring.pop_back();
      }
    };

    for (Facet &facet : facets)
    {
      conform(facet.shape.outer);

      for (Ring &hole : facet.shape.holes)
      {
        conform(hole);
      }
    }
  }

  // A subdivision vertex has one height. Evaluating adjacent planes
  // independently after XY quantisation can put that height on opposite sides
  // of a rounding boundary. Store it once for all incident cells.
  std::map<Point, double> heights;
  bool continuous = true;
  for (const Facet &facet : facets)
  {
    const auto record = [&](const Ring &ring) {
      for (const Point &q : ring)
      {
        const double height = std::max(0.0, riseOver(walls[facet.wall], roofMetres(q.x), roofMetres(q.y))) * pitch;
        const auto inserted = heights.emplace(q, height);
        continuous = continuous && std::abs(inserted.first->second - height) <= 8e-9 * pitch;
      }
    };
    record(facet.shape.outer);
    for (const Ring &hole : facet.shape.holes)
    {
      record(hole);
    }
  }

  if (!continuous)
  {
    if (error)
    {
      *error = "roof subdivision has inconsistent vertex heights";
    }
    return false;
  }

  for (const Facet &facet : facets)
  {
    // Clipping produces convex cells. A centre fan preserves every boundary
    // vertex, including collinear subdivision vertices, without an ear-clipping
    // step that can discard their very thin triangles.
    double cx = 0, cz = 0;
    for (const Point &q : facet.shape.outer)
    {
      cx += roofMetres(q.x);
      cz += roofMetres(q.y);
    }
    cx /= facet.shape.outer.size();
    cz /= facet.shape.outer.size();
    const double cy = std::max(0.0, riseOver(walls[facet.wall], cx, cz)) * pitch;
    const Ring &ring = facet.shape.outer;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Point &a = ring[i], &b = ring[(i + 1) % ring.size()];
      if (a == b)
      {
        continue;
      }
      out.push_back({roofMetres(a.x), heights.at(a), roofMetres(a.y), roofMetres(b.x), heights.at(b), roofMetres(b.y), cx, cy, cz, walls[facet.wall].source, false});
    }
  }

  if (out.empty())
  {
    if (error != nullptr)
    {
      *error = "the skeleton produced no surface";
    }

    return false;
  }

  // A gable is the vertical strip under each exposed slope edge. Reuse
  // its vertices directly so the cap and roof have identical boundaries.
  const size_t slopes = out.size();
  for (const Wall &w : walls)
  {
    if (!w.raised)
    {
      continue;
    }
    const double length = std::hypot(w.bx - w.ax, w.bz - w.az);
    for (size_t i = 0; i < slopes; ++i)
    {
      const RoofTriangle t = out[i];
      const double x[3]{t.ax, t.bx, t.cx}, y[3]{t.ay, t.by, t.cy}, z[3]{t.az, t.bz, t.cz};
      for (int k = 0; k < 3; ++k)
      {
        const int j = (k + 1) % 3;
        const double a = (x[k] - w.ax) * w.ux + (z[k] - w.az) * w.uz;
        const double b = (x[j] - w.ax) * w.ux + (z[j] - w.az) * w.uz;
        if (std::abs(riseOver(w, x[k], z[k])) > 2e-9 || std::abs(riseOver(w, x[j], z[j])) > 2e-9 || std::min(a, b) < -2e-9 || std::max(a, b) > length + 2e-9 || std::abs(a - b) < 1e-9)
        {
          continue;
        }
        if (y[j] > 1e-12)
        {
          out.push_back({x[k], y[k], z[k], x[j], y[j], z[j], x[j], 0, z[j], w.source, true});
        }
        if (y[k] > 1e-12)
        {
          out.push_back({x[k], y[k], z[k], x[j], 0, z[j], x[k], 0, z[k], w.source, true});
        }
      }
    }
  }

  // Match the winding convention of the CCW footprint triangulation: the
  // gable and slope must traverse their common top edge in opposite directions.
  for (size_t i = slopes; i < out.size(); ++i)
  {
    std::swap(out[i].bx, out[i].cx);
    std::swap(out[i].by, out[i].cy);
    std::swap(out[i].bz, out[i].cz);
  }

  return true;
}

bool roofFromSkeleton(const std::vector<std::pair<double, double>> &outline, const RoofOptions &options, std::vector<RoofTriangle> &out, std::string *error)
{
  Polygon ground;

  for (const auto &q : outline)
  {
    if (!representable(q.first) || !representable(q.second))
    {
      out.clear();
      if (error)
      {
        *error = "roof coordinate is outside the geometry grid";
      }
      return false;
    }
    ground.outer.push_back({toUnit(q.first), toUnit(q.second)});
  }

  return roofFromSkeleton(ground, options, out, error);
}

} // namespace procedural::geom2
