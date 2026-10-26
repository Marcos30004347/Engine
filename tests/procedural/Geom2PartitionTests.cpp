// Partition (Voronoi, power diagrams, Lloyd) and the 3D prism layer.
//
// The partition tests all run against a concave region, because that is the
// case a dual-of-the-Delaunay construction gets wrong: it produces convex cells
// over the whole plane and leaves the clipping to the caller, and the clipping
// is the part that was never concave-safe.

#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Sample.hpp"
#include "procedural/geom3/Prism.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

using namespace procedural;
using namespace procedural::geom2;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

Point at(const double x, const double y)
{
  return {toUnit(x), toUnit(y)};
}

Polygon boxPolygon(const double x0, const double y0, const double x1, const double y1)
{
  Polygon p;
  p.outer = {at(x0, y0), at(x1, y0), at(x1, y1), at(x0, y1)};

  return p;
}

Polygon ellPolygon()
{
  Polygon p;
  p.outer = {at(0, 0), at(12, 0), at(12, 4), at(4, 4), at(4, 12), at(0, 12)};

  return p;
}

double totalArea(const std::vector<PartitionCell> &cells)
{
  double area = 0.0;

  for (const auto &cell : cells)
  {
    const std::string fault = polygonFault(cell.polygon);

    if (!fault.empty())
    {
      std::cerr << "    (invalid cell: " << fault << ")\n";
      ++failures;
    }

    area += areaMetres(cell.polygon);
  }

  return area;
}

void voronoiTests()
{
  std::cout << "Voronoi\n";

  const Polygon region = boxPolygon(0, 0, 10, 10);
  std::vector<PartitionCell> cells;
  std::string error;

  // Two sites either side of the middle: the boundary is the perpendicular
  // bisector, so each takes exactly half.
  check(partitionRegion(region, {{at(2.5, 5), 0}, {at(7.5, 5), 0}}, {}, cells, &error) && cells.size() == 2, "two sites split a square: " + error);
  check(std::abs(totalArea(cells) - 100.0) < 1e-6, "and the cells tile the region exactly");
  check(cells.size() == 2 && std::abs(areaMetres(cells[0].polygon) - 50.0) < 1e-6, "into two equal halves");

  // Four sites in a symmetric cross: four equal quarters.
  check(partitionRegion(region, {{at(2.5, 2.5), 0}, {at(7.5, 2.5), 0}, {at(7.5, 7.5), 0}, {at(2.5, 7.5), 0}}, {}, cells, &error) && cells.size() == 4, "four sites make four cells");
  check(std::abs(totalArea(cells) - 100.0) < 1e-6, "which still tile the region");

  bool equal = true;

  for (const auto &cell : cells)
  {
    if (std::abs(areaMetres(cell.polygon) - 25.0) > 1e-6)
    {
      equal = false;
    }
  }

  check(equal, "each a quarter of the square");

  // A single site takes everything.
  check(partitionRegion(region, {{at(1, 1), 0}}, {}, cells, &error) && cells.size() == 1 && std::abs(totalArea(cells) - 100.0) < 1e-6, "one site takes the whole region");
}

void concaveRegionTests()
{
  std::cout << "Concave regions and holes\n";

  const Polygon l = ellPolygon();
  std::vector<PartitionCell> cells;
  std::string error;

  check(std::abs(areaMetres(l) - 80.0) < 1e-9, "the L region measures eighty square metres");

  check(partitionRegion(l, {{at(2, 2), 0}, {at(9, 2), 0}, {at(2, 9), 0}}, {}, cells, &error) && !cells.empty(), "sites partition a concave region: " + error);
  check(std::abs(totalArea(cells) - 80.0) < 1e-6, "and the cells tile it exactly, notch included");

  // A site's cell can be split in two by the region's own concavity: its
  // half-plane cell spans the notch, and the region does not.
  Polygon holed = boxPolygon(0, 0, 10, 10);
  Ring hole = {at(4, 4), at(6, 4), at(6, 6), at(4, 6)};
  reverse(hole);
  holed.holes.push_back(hole);

  check(partitionRegion(holed, {{at(2, 5), 0}, {at(8, 5), 0}}, {}, cells, &error) && std::abs(totalArea(cells) - 96.0) < 1e-6, "a region with a hole is tiled without the hole: " + error);
}

void powerTests()
{
  std::cout << "Power diagrams\n";

  const Polygon region = boxPolygon(0, 0, 10, 10);
  std::vector<PartitionCell> cells;
  std::string error;

  // Equal weights reproduce the Voronoi diagram exactly.
  check(partitionRegion(region, {{at(2.5, 5), 3.0}, {at(7.5, 5), 3.0}}, {}, cells, &error) && cells.size() == 2 && std::abs(areaMetres(cells[0].polygon) - 50.0) < 1e-6, "equal weights give the Voronoi diagram");

  // A heavier site claims more ground, which is the point: a district's cell
  // can be sized by its demand instead of by how its neighbours are spaced.
  check(partitionRegion(region, {{at(2.5, 5), 10.0}, {at(7.5, 5), 0.0}}, {}, cells, &error) && cells.size() == 2, "a weighted pair still makes two cells");
  check(cells.size() == 2 && areaMetres(cells[0].polygon) > 50.0, "and the heavier site takes more than half");
  check(std::abs(totalArea(cells) - 100.0) < 1e-6, "while the cells still tile the region");

  // The boundary moves by (heavy - light) / (2 * distance): 10 / 10 = 1 metre,
  // so the heavier cell is six wide rather than five.
  check(cells.size() == 2 && std::abs(areaMetres(cells[0].polygon) - 60.0) < 1e-6, "by exactly the distance the radical axis moves");

  // A site buried under a heavy neighbour simply has no cell. That is an
  // ordinary outcome of weighting, not a failure to report.
  check(partitionRegion(region, {{at(5, 5), 0.0}, {at(5.1, 5), 1000.0}}, {}, cells, &error) && cells.size() == 1, "a buried site has no cell");
  check(std::abs(totalArea(cells) - 100.0) < 1e-6, "and its ground goes to the heavy one");
}

void relaxTests()
{
  std::cout << "Lloyd relaxation\n";

  const Polygon region = boxPolygon(0, 0, 10, 10);
  std::string error;

  // Two sites huddled in a corner spread out under relaxation, and their
  // cells even up.
  std::vector<Site> sites{{at(1, 1), 0}, {at(2, 1), 0}};

  check(relax(region, sites, 12, {}, &error), "relaxation runs: " + error);

  std::vector<PartitionCell> cells;

  check(partitionRegion(region, sites, {}, cells, &error) && cells.size() == 2, "and still gives two cells");

  const double first = areaMetres(cells[0].polygon);
  const double second = areaMetres(cells[1].polygon);

  check(std::abs(first - second) < 5.0, "whose areas have evened out");
  check(std::abs(first + second - 100.0) < 1e-6, "and still tile the region");

  // A centroid must land inside its own polygon for a convex cell.
  Point middle;

  check(centroid(boxPolygon(0, 0, 10, 10), middle) && middle == at(5, 5), "the centroid of a square is its middle");
  check(!centroid(Polygon{}, middle), "a polygon with no area has no centroid");
}

void prismTests()
{
  std::cout << "Prisms\n";

  std::string error;
  geom3::Mesh mesh;

  geom3::Prism prism;
  prism.footprint = boxPolygon(0, 0, 4, 3);
  prism.baseHeight = 0;
  prism.topHeight = 2.5;

  check(geom3::extrude(prism, mesh, &error), "a box extrudes: " + error);
  check(mesh.triangles.size() == 2 + 2 + 4 * 2, "into two caps and four walls");
  check(mesh.parts.size() == mesh.triangles.size(), "with a part recorded per triangle");

  bool indicesValid = true;

  for (const auto &t : mesh.triangles)
  {
    for (const uint32_t v : t)
    {
      if (v >= mesh.vertices.size())
      {
        indicesValid = false;
      }
    }
  }

  check(indicesValid, "and every triangle index in range");

  // A courtyard: the inside of the hole is as much a wall as the outside.
  geom3::Prism courtyard;
  courtyard.footprint = boxPolygon(0, 0, 10, 10);
  Ring hole = {at(4, 4), at(6, 4), at(6, 6), at(4, 6)};
  reverse(hole);
  courtyard.footprint.holes.push_back(hole);
  courtyard.topHeight = 3;

  check(geom3::extrude(courtyard, mesh, &error), "a courtyard block extrudes: " + error);

  size_t walls = 0;

  for (const auto part : mesh.parts)
  {
    if (part == geom3::Mesh::Part::Wall)
    {
      ++walls;
    }
  }

  check(walls == 8 * 2, "with walls on the courtyard as well as the street");

  // A concave footprint, which is what the ear clipper is really for.
  geom3::Prism ell;
  ell.footprint = ellPolygon();
  ell.topHeight = 4;

  check(geom3::extrude(ell, mesh, &error), "a concave footprint extrudes: " + error);

  // Sloping ground: a per-vertex base profile puts a building on a hillside
  // without cutting its footprint up.
  geom3::Prism slope;
  slope.footprint = boxPolygon(0, 0, 4, 4);
  slope.baseProfile = {0.0, 0.5, 0.8, 0.3};
  slope.topHeight = 5;

  check(geom3::extrude(slope, mesh, &error), "a prism on sloping ground extrudes: " + error);

  bool profileUsed = false;

  for (const auto &v : mesh.vertices)
  {
    if (std::abs(v.y - 0.8) < 1e-9)
    {
      profileUsed = true;
    }
  }

  check(profileUsed, "and takes its base heights from the profile");

  // A wall from a centreline.
  check(geom3::thicken({at(0, 0), at(10, 0), at(10, 6)}, 0.4, 0.0, 3.0, mesh, &error), "a wall thickens from a centreline: " + error);
  check(!mesh.triangles.empty(), "into a mesh");
}

void sampleTests()
{
  std::cout << "Sampling\n";

  std::string error;
  std::vector<Point> points;

  const Polygon l = ellPolygon();
  SampleSettings settings;
  settings.spacingMetres = 1.5;
  settings.seed = 7;

  check(samplePolygon(l, settings, points, &error) && !points.empty(), "a concave region samples: " + error);

  bool allInside = true;
  bool spaced = true;

  for (size_t i = 0; i < points.size(); ++i)
  {
    if (!contains(l, points[i]))
    {
      allInside = false;
    }

    for (size_t j = i + 1; j < points.size(); ++j)
    {
      const double dx = toMetres(points[i].x - points[j].x);
      const double dy = toMetres(points[i].y - points[j].y);

      if (std::hypot(dx, dy) + 1e-9 < settings.spacingMetres)
      {
        spaced = false;
      }
    }
  }

  check(allInside, "every sample lands inside the region, notch included");
  check(spaced, "and no two are closer than the spacing");

  // The reproducible method exists so a result can be relied on between runs.
  std::vector<Point> again;

  check(samplePolygon(l, settings, again, &error) && again == points, "the reproducible method gives the same points twice");

  // Samples must never land in a hole.
  Polygon holed = boxPolygon(0, 0, 12, 12);
  Ring hole = {at(4, 4), at(8, 4), at(8, 8), at(4, 8)};
  reverse(hole);
  holed.holes.push_back(hole);

  check(samplePolygon(holed, settings, points, &error), "a holed region samples: " + error);

  bool clearOfHole = true;

  for (const auto &p : points)
  {
    if (contains(holed.holes[0], p) && !onBoundary(holed.holes[0], p))
    {
      clearOfHole = false;
    }
  }

  check(clearOfHole, "and no sample lands in the hole");

  // Thinning keeps points the caller already has, rather than inventing new
  // ones: twenty building sites out of two hundred candidates.
  std::vector<Point> thinned;

  check(points.size() > 12 && thinToCount(points, 12, thinned, areaMetres(holed), &error) && thinned.size() == 12, "thinning keeps the requested count: " + error);

  bool fromCandidates = true;

  for (const auto &p : thinned)
  {
    if (std::find(points.begin(), points.end(), p) == points.end())
    {
      fromCandidates = false;
    }
  }

  check(fromCandidates, "and every kept point came from the candidates");
}

void triangulationTests()
{
  std::cout << "Triangulation\n";

  std::string error;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Point> vertices;

  check(geom3::triangulate(boxPolygon(0, 0, 4, 4), triangles, vertices, &error) && triangles.size() == 2, "a square triangulates into two: " + error);

  check(geom3::triangulate(ellPolygon(), triangles, vertices, &error) && triangles.size() == 4, "a concave L triangulates into four");

  // A polygon with a hole: the bridge makes it one ring the clipper can walk.
  Polygon holed = boxPolygon(0, 0, 10, 10);
  Ring hole = {at(4, 4), at(6, 4), at(6, 6), at(4, 6)};
  reverse(hole);
  holed.holes.push_back(hole);

  check(geom3::triangulate(holed, triangles, vertices, &error), "a holed square triangulates: " + error);

  // Triangulation must conserve area, which is the one check that catches a
  // clipper that quietly dropped or doubled a piece.
  double area = 0.0;

  for (const auto &t : triangles)
  {
    area +=
        0.5 *
        std::abs(
            double((__int128(vertices[t[1]].x) - vertices[t[0]].x) * (__int128(vertices[t[2]].y) - vertices[t[0]].y) - (__int128(vertices[t[1]].y) - vertices[t[0]].y) * (__int128(vertices[t[2]].x) - vertices[t[0]].x))) *
        kMetresPerUnit * kMetresPerUnit;
  }

  check(std::abs(area - 96.0) < 1e-6, "and its triangles sum to the polygon's area");
}

} // namespace

int main()
{
  std::cout << "geom2 partition and geom3 prism tests\n";

  voronoiTests();
  concaveRegionTests();
  powerTests();
  relaxTests();
  prismTests();
  triangulationTests();
  sampleTests();

  if (failures != 0)
  {
    std::cerr << failures << " partition/prism test(s) failed\n";

    return 1;
  }

  std::cout << "All partition and prism tests passed\n";

  return 0;
}
