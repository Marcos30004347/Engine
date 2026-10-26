#include "procedural/geom2/Partition.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::geom2 {

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

// Clips a convex ring to the half-plane {x : nx * x + ny * y <= c}, in metres.
//
// Sutherland-Hodgman, with the side test taken on the exact grid and only the
// new crossing points constructed. A cell is convex by construction - it is an
// intersection of half-planes - so this stays convex, and the one concave step,
// meeting the region, is left to the boolean.
Ring clipHalfPlane(const Ring &ring, const double nx, const double ny, const double c)
{
  if (ring.empty())
  {
    return {};
  }

  const auto value = [&](const Point &p) {
    return nx * toMetres(p.x) + ny * toMetres(p.y) - c;
  };

  Ring out;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Point &a = ring[i];
    const Point &b = ring[(i + 1) % ring.size()];
    const double va = value(a);
    const double vb = value(b);

    if (va <= 0.0)
    {
      out.push_back(a);
    }

    if ((va < 0.0 && vb > 0.0) || (va > 0.0 && vb < 0.0))
    {
      const double t = va / (va - vb);
      const double x = toMetres(a.x) + (toMetres(b.x) - toMetres(a.x)) * t;
      const double y = toMetres(a.y) + (toMetres(b.y) - toMetres(a.y)) * t;

      if (representable(x) && representable(y))
      {
        out.push_back({toUnit(x), toUnit(y)});
      }
    }
  }

  // Quantisation can put two crossings on one grid point.
  Ring tidy;

  for (const auto &p : out)
  {
    if (tidy.empty() || tidy.back() != p)
    {
      tidy.push_back(p);
    }
  }

  while (tidy.size() > 1 && tidy.front() == tidy.back())
  {
    tidy.pop_back();
  }

  return tidy.size() >= 3 ? tidy : Ring{};
}

} // namespace

bool centroid(const Polygon &polygon, Point &out)
{
  __int128 doubledArea = 0;
  long double x = 0;
  long double y = 0;

  const auto accumulate = [&](const Ring &ring) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Point &a = ring[i];
      const Point &b = ring[(i + 1) % ring.size()];
      const __int128 cross = __int128(a.x) * b.y - __int128(b.x) * a.y;

      doubledArea += cross;
      x += (long double)(double(cross)) * (long double)(a.x + b.x);
      y += (long double)(double(cross)) * (long double)(a.y + b.y);
    }
  };

  accumulate(polygon.outer);

  for (const auto &hole : polygon.holes)
  {
    accumulate(hole);
  }

  if (doubledArea == 0)
  {
    return false;
  }

  const long double scale = 1.0L / (3.0L * (long double)(double(doubledArea)));

  out = {int64_t(std::llround(double(x * scale))), int64_t(std::llround(double(y * scale)))};

  return std::abs(out.x) <= kMaximumUnit && std::abs(out.y) <= kMaximumUnit;
}

bool partitionRegion(const Polygon &region, const std::vector<Site> &sites, const PartitionOptions &options, std::vector<PartitionCell> &out, std::string *error)
{
  out.clear();

  const std::string fault = polygonFault(region);

  if (!fault.empty())
  {
    return fail(error, "partition region is not a valid polygon");
  }

  if (sites.empty())
  {
    return fail(error, "partition needs at least one site");
  }

  for (const auto &site : sites)
  {
    if (!std::isfinite(site.weight))
    {
      return fail(error, "site weight is not finite");
    }
  }

  const Box box = bounds(region);
  const double margin = toMetres(std::max(box.maxX - box.minX, box.maxY - box.minY)) + 1.0;
  const double minX = toMetres(box.minX) - margin;
  const double minY = toMetres(box.minY) - margin;
  const double maxX = toMetres(box.maxX) + margin;
  const double maxY = toMetres(box.maxY) + margin;

  for (size_t i = 0; i < sites.size(); ++i)
  {
    Ring cell{{toUnit(minX), toUnit(minY)}, {toUnit(maxX), toUnit(minY)}, {toUnit(maxX), toUnit(maxY)}, {toUnit(minX), toUnit(maxY)}};

    const double px = toMetres(sites[i].position.x);
    const double py = toMetres(sites[i].position.y);

    for (size_t j = 0; j < sites.size() && !cell.empty(); ++j)
    {
      if (j == i)
      {
        continue;
      }

      const double qx = toMetres(sites[j].position.x);
      const double qy = toMetres(sites[j].position.y);
      const double nx = 2.0 * (qx - px);
      const double ny = 2.0 * (qy - py);

      if (nx == 0.0 && ny == 0.0)
      {
        // Two sites at one point. The earlier one keeps the ground; the later
        // one is squeezed out, which is the same answer either ordering gives.
        if (j < i)
        {
          cell.clear();
        }

        continue;
      }

      // The radical axis: |x - p|^2 - wp = |x - q|^2 - wq. With both weights
      // zero this is the perpendicular bisector and the diagram is Voronoi.
      const double c = (qx * qx + qy * qy - sites[j].weight) - (px * px + py * py - sites[i].weight);

      cell = clipHalfPlane(cell, nx, ny, c);
    }

    if (cell.size() < 3)
    {
      continue;
    }

    if (!counterClockwise(cell))
    {
      reverse(cell);
    }

    std::vector<Polygon> clipped;
    BooleanOptions pass;
    pass.limits = options.limits;

    if (!booleanOp({toOperand(region), toOperand(cell)}, Operation::Intersection, pass, clipped, error))
    {
      return false;
    }

    for (auto &polygon : clipped)
    {
      out.push_back({i, std::move(polygon)});
    }
  }

  return true;
}

bool relax(const Polygon &region, std::vector<Site> &sites, const uint32_t iterations, const PartitionOptions &options, std::string *error)
{
  for (uint32_t pass = 0; pass < iterations; ++pass)
  {
    std::vector<PartitionCell> cells;

    if (!partitionRegion(region, sites, options, cells, error))
    {
      return false;
    }

    // A site may own several disconnected pieces once the region is concave.
    // It moves to the centroid of its largest piece, because the centroid of
    // the union can land in the gap between them - outside the region and
    // outside every piece the site actually holds.
    std::vector<double> best(sites.size(), 0.0);
    std::vector<Point> target(sites.size());
    std::vector<bool> moved(sites.size(), false);

    for (const auto &cell : cells)
    {
      const double area = areaMetres(cell.polygon);
      Point middle;

      if (area > best[cell.site] && centroid(cell.polygon, middle))
      {
        best[cell.site] = area;
        target[cell.site] = middle;
        moved[cell.site] = true;
      }
    }

    for (size_t i = 0; i < sites.size(); ++i)
    {
      if (moved[i])
      {
        sites[i].position = target[i];
      }
    }
  }

  return true;
}

} // namespace procedural::geom2
