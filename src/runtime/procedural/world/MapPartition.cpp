#include "procedural/world/MapPartition.hpp"

#include "procedural/world/MapCoast.hpp"

#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/Predicates.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world
{

namespace
{

// Circumcentre of a triangle. This is the Voronoi vertex dual to the triangle,
// so the accuracy here is the accuracy of every cell boundary.
bool circumcentre(
    const Point2 &a, const Point2 &b, const Point2 &c, Point2 &centre)
{
  const double ax = b.x - a.x;
  const double ay = b.y - a.y;
  const double bx = c.x - a.x;
  const double by = c.y - a.y;
  const double d = 2.0 * (ax * by - ay * bx);
  // Collinear or degenerate. The caller drops the triangle rather than
  // producing an infinite Voronoi vertex.
  if (std::abs(d) < 1e-12)
    return false;
  const double aLengthSquared = ax * ax + ay * ay;
  const double bLengthSquared = bx * bx + by * by;
  centre.x = a.x + (by * aLengthSquared - ay * bLengthSquared) / d;
  centre.y = a.y + (ax * bLengthSquared - bx * aLengthSquared) / d;
  return true;
}

// Sorts a cell's boundary vertices counter-clockwise about its site. The dual
// walk produces them in triangle order, which is not the polygon order.
void sortAboutSite(const Point2 &site, std::vector<Point2> &polygon)
{
  std::sort(polygon.begin(), polygon.end(),
      [&site](const Point2 &lhs, const Point2 &rhs)
      {
        return std::atan2(lhs.y - site.y, lhs.x - site.x) <
            std::atan2(rhs.y - site.y, rhs.x - site.x);
      });
}

// Removes vertices that repeat within a tolerance. Adjacent triangles sharing a
// circumcentre would otherwise leave duplicate polygon corners.
void removeDuplicates(std::vector<Point2> &polygon, const double tolerance)
{
  const auto near = [tolerance](const Point2 &lhs, const Point2 &rhs)
  {
    return std::abs(lhs.x - rhs.x) < tolerance &&
        std::abs(lhs.y - rhs.y) < tolerance;
  };
  polygon.erase(std::unique(polygon.begin(), polygon.end(), near),
      polygon.end());
  if (polygon.size() > 1u && near(polygon.front(), polygon.back()))
    polygon.pop_back();
}

} // namespace

bool buildVoronoiCells(
    const std::vector<Point2> &sites,
    const std::vector<Point2> &window,
    const double toleranceMetres,
    std::vector<VoronoiCell> &out,
    std::string *error)
{
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };

  out.clear();
  if (sites.size() < 3u)
    return fail("a Voronoi partition needs at least three sites");

  initialisePredicates();

  // Ghost sites, mirrored across each window edge.
  //
  // A site on the convex hull of the set has an *unbounded* dual, built from
  // too few circumcentres to be a polygon, and it gets dropped. With hundreds
  // of sites that is a handful of border cells; with the fifteen or so inside
  // one parent cell it is most of them, and a hierarchy level came out with a
  // third of the cells it asked for.
  //
  // Reflecting every site across every window edge puts a mirror site outside
  // each edge, which bounds every real cell exactly at the window - the
  // standard construction, and cheap here because the window is convex.
  std::vector<Point2> all = sites;
  const size_t realCount = sites.size();
  if (!window.empty())
  {
    for (size_t edge = 0u; edge < window.size(); ++edge)
    {
      const Point2 &a = window[edge];
      const Point2 &b = window[(edge + 1u) % window.size()];
      const double ex = b.x - a.x;
      const double ey = b.y - a.y;
      const double lengthSquared = ex * ex + ey * ey;
      if (lengthSquared <= 0.0)
        continue;
      for (size_t index = 0u; index < realCount; ++index)
      {
        const Point2 &site = sites[index];
        const double along =
            ((site.x - a.x) * ex + (site.y - a.y) * ey) / lengthSquared;
        const Point2 foot(a.x + ex * along, a.y + ey * along);
        all.push_back(
            Point2(2.0 * foot.x - site.x, 2.0 * foot.y - site.y));
      }
    }
  }

  ConstrainedDelaunayTriangulator triangulator;
  if (!triangulator.triangulate(all, {}))
    return fail("partition triangulation stalled");

  // Voronoi cells are the dual: each site's cell is bounded by the
  // circumcentres of the triangles incident to it.
  // Cells are only emitted for the real sites; the ghosts exist to bound them
  // and are then forgotten.
  const size_t siteCount = all.size();
  std::vector<std::vector<Point2>> cellPolygons(siteCount);
  std::vector<std::vector<uint32_t>> cellNeighbours(siteCount);

  const auto linkNeighbours = [&](const int64_t lhs, const int64_t rhs)
  {
    if (lhs < 0 || rhs < 0 || lhs == rhs)
      return;
    if (static_cast<size_t>(lhs) >= siteCount ||
        static_cast<size_t>(rhs) >= siteCount)
      return;
    auto &list = cellNeighbours[static_cast<size_t>(lhs)];
    if (std::find(list.begin(), list.end(), static_cast<uint32_t>(rhs)) ==
        list.end())
      list.push_back(static_cast<uint32_t>(rhs));
  };

  for (uint64_t triangle = 0u; triangle < triangulator.triangleCount();
       ++triangle)
  {
    if (!triangulator.triangleExists(triangle))
      continue;
    const int64_t i0 = triangulator.vertexOfTriangle(triangle, 0);
    const int64_t i1 = triangulator.vertexOfTriangle(triangle, 1);
    const int64_t i2 = triangulator.vertexOfTriangle(triangle, 2);
    // Triangles touching the super-triangle vertices lie outside the input and
    // have no meaningful dual. Their sites' cells are bounded by the window
    // instead, which is what makes border cells finite.
    if (static_cast<size_t>(i0) >= siteCount ||
        static_cast<size_t>(i1) >= siteCount ||
        static_cast<size_t>(i2) >= siteCount)
      continue;

    Point2 centre{};
    if (!circumcentre(all[i0], all[i1], all[i2], centre))
      continue;

    cellPolygons[i0].push_back(centre);
    cellPolygons[i1].push_back(centre);
    cellPolygons[i2].push_back(centre);

    linkNeighbours(i0, i1);
    linkNeighbours(i1, i0);
    linkNeighbours(i1, i2);
    linkNeighbours(i2, i1);
    linkNeighbours(i2, i0);
    linkNeighbours(i0, i2);
  }

  std::vector<uint32_t> siteToCell(siteCount, kInvalidNode);
  for (size_t site = 0u; site < realCount; ++site)
  {
    std::vector<Point2> &polygon = cellPolygons[site];
    if (polygon.size() < 3u)
      continue;
    sortAboutSite(all[site], polygon);
    removeDuplicates(polygon, toleranceMetres);
    if (polygon.size() < 3u)
      continue;
    if (!window.empty())
    {
      polygon = clipPolygonToConvex(polygon, window);
      if (polygon.size() < 3u)
        continue;
    }
    siteToCell[site] = static_cast<uint32_t>(out.size());
    out.push_back(VoronoiCell{all[site], std::move(polygon), {}});
  }

  // Neighbour links rewritten in cell indices, and only between sites that
  // actually became cells. Symmetry is enforced here rather than trusted,
  // because an asymmetric link makes blending and boundary contracts apply to
  // one side only - a wrong world rather than a crash.
  for (size_t site = 0u; site < realCount; ++site)
  {
    const uint32_t cell = siteToCell[site];
    if (cell == kInvalidNode)
      continue;
    for (const uint32_t neighbourSite : cellNeighbours[site])
    {
      // Ghosts are not neighbours of anything.
      if (neighbourSite >= realCount)
        continue;
      const uint32_t neighbour = siteToCell[neighbourSite];
      if (neighbour == kInvalidNode)
        continue;
      auto &list = out[cell].neighbours;
      if (std::find(list.begin(), list.end(), neighbour) == list.end())
        list.push_back(neighbour);
      auto &other = out[neighbour].neighbours;
      if (std::find(other.begin(), other.end(), cell) == other.end())
        other.push_back(cell);
    }
  }

  if (out.empty())
    return fail("partition produced no cells");
  return true;
}

bool buildPartition(
    const PartitionSettings &settings,
    MapGraph &graph,
    std::string *error)
{
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };

  if (!(settings.extentMetres > 0.0) || !(settings.cellSpacingMetres > 0.0))
    return fail("partition extent and cell spacing must be positive");
  if (settings.cellSpacingMetres >= settings.extentMetres)
    return fail("cell spacing must be smaller than the extent");

  PoissonDiskSampler sampler;
  PoissonDiskSampler::Settings samplerSettings{};
  samplerSettings.radius = settings.cellSpacingMetres;
  samplerSettings.width = settings.extentMetres;
  samplerSettings.height = settings.extentMetres;
  samplerSettings.seed = settings.seed;
  samplerSettings.method = PoissonDiskSampler::Method::RandomThrows;
  const std::vector<Point2> sites = sampler.generate(samplerSettings);
  if (sites.size() < 3u)
    return fail("partition produced fewer than three sites");

  std::vector<VoronoiCell> cells;
  // No window: border cells stay unbounded here and clipCellsToExtent bounds
  // them, which is the order the coast stage already expects.
  if (!buildVoronoiCells(
          sites, {}, settings.cellSpacingMetres * 1e-6, cells, error))
    return false;

  std::vector<uint32_t> cellToNode(cells.size(), kInvalidNode);
  for (size_t index = 0u; index < cells.size(); ++index)
  {
    MapNode node{};
    node.level = 0u;
    node.site = cells[index].site;
    node.area = cells[index].area;
    // Everything starts as ocean. A raiser decides what becomes land, and
    // leaving classification to that one stage is what keeps the archetype the
    // single source of truth for the world's shape.
    node.type = CellType::Ocean;
    node.content = CellType::Ocean;
    node.land = false;
    cellToNode[index] = graph.addNode(std::move(node));
  }
  for (size_t index = 0u; index < cells.size(); ++index)
    for (const uint32_t neighbour : cells[index].neighbours)
      graph.node(cellToNode[index])
          .neighbours.push_back(cellToNode[neighbour]);

  if (graph.nodeCount() == 0u)
    return fail("partition produced no cells");
  return true;
}

} // namespace procedural::world
