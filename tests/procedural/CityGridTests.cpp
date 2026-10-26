// The irregular quad grid a city is built on.
//
// Three fixture regions, all concave, because concave is the baseline here: an
// L-shaped planning cell, a cluster with a lake in it, and two banks joined by
// an isthmus. Every assertion is a property of the finished grid rather than a
// comparison against a stored one, so a change that improves the grid does not
// have to rewrite the test to prove it.

#include "procedural/geom2/Arrangement.hpp"
#include "procedural/world/citywfc/CityGrid.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace procedural;
using namespace procedural::world::citywfc;
using procedural::world::rewrite::Position;

namespace {

int failures = 0;
std::string svgDirectory;

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

std::string number(const double value, const int places = 2)
{
  std::ostringstream text;
  text << std::fixed << std::setprecision(places) << value;

  return text.str();
}

// The plan plane is x-z; y is elevation and stays zero.
std::vector<Position> rectangle(const double x0, const double z0, const double x1, const double z1)
{
  return {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
}

// An L: one planning cell that no rectangle covers.
std::vector<std::vector<Position>> ellFixture()
{
  return {{{0, 0, 0}, {240, 0, 0}, {240, 0, 90}, {90, 0, 90}, {90, 0, 240}, {0, 0, 240}}};
}

// Nine cells with the middle one missing: a lake, and therefore a hole.
std::vector<std::vector<Position>> lakeFixture()
{
  std::vector<std::vector<Position>> cells;

  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      if (i == 1 && j == 1)
      {
        continue;
      }

      cells.push_back(rectangle(double(j) * 90, double(i) * 90, double(j) * 90 + 90, double(i) * 90 + 90));
    }
  }

  return cells;
}

// Two banks and a narrow crossing: the shape that breaks anything assuming the
// city is one blob.
std::vector<std::vector<Position>> isthmusFixture()
{
  return {rectangle(0, 0, 150, 200), rectangle(150, 80, 230, 120), rectangle(230, 0, 380, 200)};
}

void writeSvg(const CityGrid &grid, const std::string &name)
{
  if (svgDirectory.empty())
  {
    return;
  }

  std::filesystem::create_directories(svgDirectory);

  const geom2::Box box = geom2::bounds(grid.region);
  const double minX = geom2::toMetres(box.minX), minY = geom2::toMetres(box.minY);
  const double width = geom2::toMetres(box.maxX) - minX, height = geom2::toMetres(box.maxY) - minY;
  const double pad = 12;

  std::ostringstream svg;
  svg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='" << (minX - pad) << " " << (minY - pad) << " " << (width + 2 * pad) << " " << (height + 2 * pad) << "' width='1200'>\n";
  svg << "<rect x='" << (minX - pad) << "' y='" << (minY - pad) << "' width='" << (width + 2 * pad) << "' height='" << (height + 2 * pad) << "' fill='#12151b'/>\n";

  static const char *palette[] = {"#4c6ef5", "#37b24d", "#f59f00", "#e64980", "#15aabf", "#7950f2", "#fa5252", "#82c91e", "#1098ad", "#d6336c", "#5c7cfa", "#94d82d"};

  for (uint32_t f = 0; f < grid.mesh.faces(); ++f)
  {
    geom2::Point corner[4];
    grid.mesh.corners(f, corner);

    svg << "<polygon points='";

    for (uint32_t i = 0; i < 4; ++i)
    {
      svg << geom2::toMetres(corner[i].x) << "," << geom2::toMetres(corner[i].y) << " ";
    }

    svg << "' fill='" << palette[grid.hexOfFace[f] % 12] << "' fill-opacity='0.55' stroke='#e9ecef' stroke-width='0.35'/>\n";
  }

  const auto ring = [&](const geom2::Ring &r) {
    svg << "<polygon points='";

    for (const geom2::Point &p : r)
    {
      svg << geom2::toMetres(p.x) << "," << geom2::toMetres(p.y) << " ";
    }

    svg << "' fill='none' stroke='#ffd43b' stroke-width='1.2'/>\n";
  };

  ring(grid.region.outer);

  for (const geom2::Ring &hole : grid.region.holes)
  {
    ring(hole);
  }

  svg << "</svg>\n";

  std::ofstream file(svgDirectory + "/" + name + ".svg");
  file << svg.str();
}

void report(const CityGrid &grid, const std::string &name)
{
  const CityGridQuality &q = grid.quality;

  std::cout << "CITY_WFC_GRID name=" << name
            << " hexes=" << grid.lattice.hexes
            << " tris=" << q.triangles
            << " unpaired=" << number(q.triangles > 0 ? double(q.unpairedTriangles) / double(q.triangles) : 0, 4)
            << " faces=" << q.faces
            << " verts=" << q.vertices
            << " edge=" << number(q.p05Edge) << "/" << number(q.medianEdge) << "/" << number(q.p95Edge)
            << " area=" << number(q.p05Area, 1) << "/" << number(q.medianArea, 1) << "/" << number(q.p95Area, 1)
            << " angle=" << number(q.p05InteriorAngle, 1) << "/" << number(q.minInteriorAngle, 1)
            << " aspect=" << number(q.p95Aspect)
            << " coverage=" << number(q.coverageRatio, 4)
            << " deviation=" << number(q.boundaryDeviationMetres)
            << " residual=" << number(q.residualRatio, 4)
            << " iters=" << grid.relax.iterations
            << "\n";

  std::cout << "    rim  faces=" << q.rimFaces << " p05angle=" << number(q.rimP05Angle, 1) << " min=" << number(q.rimMinAngle, 1) << " p05area=" << number(q.rimP05Area, 1) << "\n";
  std::cout << "    core faces=" << (q.faces - q.rimFaces) << " p05angle=" << number(q.coreP05Angle, 1) << " min=" << number(q.coreMinAngle, 1) << " p05area=" << number(q.coreP05Area, 1) << "\n";

  for (const std::string &entry : grid.ledger)
  {
    std::cout << "    ledger: " << entry << "\n";
  }
}

CityGridSettings defaults()
{
  CityGridSettings settings;
  settings.lattice.targetCellEdgeMetres = 6;
  settings.lattice.hexRadiusTriangles = 5;
  settings.lattice.seed = 20260924;
  settings.lattice.site = 1;

  return settings;
}

void testQualityGate(const std::vector<std::vector<Position>> &cells, const std::string &name)
{
  std::cout << name << "\n";

  CityGrid grid;
  std::string error;

  const bool built = buildCityGrid(cells, defaults(), grid, &error);
  check(built, name + ": the grid builds: " + error);

  if (!built)
  {
    return;
  }

  report(grid, name);
  writeSvg(grid, name);

  const CityGridQuality &q = grid.quality;
  const double target = q.targetEdgeMetres;

  check(geom2::quadMeshFault(grid.mesh).empty(), name + ": the finished mesh is sound");
  check(q.faces > 0, name + ": the grid has cells");

  check(q.p05Edge / target >= .55, name + ": p05 edge >= 0.55 target (" + number(q.p05Edge / target) + ")");
  check(q.p95Edge / target <= 1.70, name + ": p95 edge <= 1.70 target (" + number(q.p95Edge / target) + ")");
  check(q.p05Area / q.targetAreaMetres >= .50, name + ": p05 area >= 0.50 target (" + number(q.p05Area / q.targetAreaMetres) + ")");
  check(q.p95Area / q.targetAreaMetres <= 1.80, name + ": p95 area <= 1.80 target (" + number(q.p95Area / q.targetAreaMetres) + ")");
  check(q.p05InteriorAngle >= 50, name + ": p05 interior angle >= 50 deg (" + number(q.p05InteriorAngle, 1) + ")");
  check(q.minInteriorAngle >= 30, name + ": min interior angle >= 30 deg (" + number(q.minInteriorAngle, 1) + ")");
  check(q.p95Aspect <= 2.2, name + ": p95 aspect <= 2.2 (" + number(q.p95Aspect) + ")");
  check(q.coverageRatio >= .97, name + ": coverage >= 0.97 (" + number(q.coverageRatio, 4) + ")");
  check(q.boundaryDeviationMetres <= .75 * target, name + ": boundary deviation <= 0.75 target (" + number(q.boundaryDeviationMetres) + ")");
  check(q.triangles == 0 || double(q.unpairedTriangles) / double(q.triangles) <= .05, name + ": unpaired triangles <= 5% (" + number(double(q.unpairedTriangles) / double(std::max(1u, q.triangles)), 4) + ")");
}

void testSeam()
{
  std::cout << "hex seam\n";

  CityGrid grid;
  std::string error;
  check(buildCityGrid(ellFixture(), defaults(), grid, &error), "the grid builds for the seam test: " + error);

  const CityGridQuality &q = grid.quality;

  check(q.seamFaces > 0, "some cells straddle two hexagonal patches");
  check(q.interiorMedianEdge > 0, "interior cells were measured");

  std::cout << "    seam     faces=" << q.seamFaces << " medianEdge=" << number(q.seamMedianEdge) << " medianAngle=" << number(q.seamMedianAngle, 1) << "\n";
  std::cout << "    interior faces=" << (q.faces - q.rimFaces - q.seamFaces) << " medianEdge=" << number(q.interiorMedianEdge) << " medianAngle=" << number(q.interiorMedianAngle, 1) << "\n";

  // Relaxing patch by patch, as the infinite construction must, leaves the seam
  // rows visibly worse. Relaxing the whole city at once is what this checks,
  // and it is the regression a thumbnail would not show.
  const double edgeRatio = std::abs(q.seamMedianEdge - q.interiorMedianEdge) / q.interiorMedianEdge;
  const double angleRatio = std::abs(q.seamMedianAngle - q.interiorMedianAngle) / q.interiorMedianAngle;

  check(edgeRatio <= .05, "seam and interior median edges agree within 5% (" + number(edgeRatio, 4) + ")");
  check(angleRatio <= .05, "seam and interior median angles agree within 5% (" + number(angleRatio, 4) + ")");
}

void testRelaxationEarnsItsPlace()
{
  std::cout << "relaxation control\n";

  CityGridSettings relaxed = defaults();
  CityGridSettings raw = defaults();
  raw.relax.iterations = 0;
  raw.settle.iterations = 0;

  CityGrid a, b;
  std::string error;

  check(buildCityGrid(ellFixture(), relaxed, a, &error), "the relaxed grid builds");
  check(buildCityGrid(ellFixture(), raw, b, &error), "the unrelaxed grid builds");

  // What the unrelaxed lattice actually is, measured rather than assumed: every
  // cell is a quarter of an identical rhombus, so the areas are EXACTLY uniform
  // and every cell has the same 60/90/120/90 corners. An earlier version of
  // this test asserted that relaxation narrows the area spread, which is the
  // opposite of the truth - there is no spread to narrow, and relaxation trades
  // some of that uniformity for squareness.
  //
  // So the control measures squareness, which is the thing relaxation is for: a
  // 60 degree corner is a rhombus, not a plot, and no wall or road sits well in
  // one.
  const double relaxedSpread = a.quality.p95Area / a.quality.p05Area;
  const double rawSpread = b.quality.p95Area / b.quality.p05Area;

  std::cout << "    residual ratio  relaxed=" << number(a.quality.residualRatio, 4) << " raw=" << number(b.quality.residualRatio, 4) << "\n";
  std::cout << "    median corner   relaxed=" << number(a.quality.interiorMedianAngle, 1) << " raw=" << number(b.quality.interiorMedianAngle, 1) << "\n";
  std::cout << "    area spread     relaxed=" << number(relaxedSpread) << " raw=" << number(rawSpread) << "\n";

  check(a.quality.fitResidual < b.quality.fitResidual * .5, "relaxation at least halves the fit residual");
  check(b.quality.interiorMedianAngle < 62, "the unrelaxed lattice is rhombic: its median worst corner is about 60 degrees (" + number(b.quality.interiorMedianAngle, 1) + ")");
  check(a.quality.interiorMedianAngle > b.quality.interiorMedianAngle + 10, "relaxation moves the median worst corner at least ten degrees toward square (" + number(a.quality.interiorMedianAngle, 1) + ")");
  check(relaxedSpread <= 2.5, "and pays for it with a bounded area spread (" + number(relaxedSpread) + ")");
  check(a.relax.monotonic, "the residual falls at every pass");

  writeSvg(b, "ell-unrelaxed");
}

void testNonSelfIntersection()
{
  std::cout << "non-self-intersection\n";

  CityGrid grid;
  std::string error;
  check(buildCityGrid(ellFixture(), defaults(), grid, &error), "the grid builds");

  std::vector<geom2::InputRing> rings;

  for (uint32_t f = 0; f < grid.mesh.faces(); ++f)
  {
    geom2::Point c[4];
    grid.mesh.corners(f, c);
    rings.push_back({{c[0], c[1], c[2], c[3]}, f, false});
  }

  geom2::ArrangementLimits limits;
  limits.maximumSegments = 400000;
  limits.maximumVertices = 400000;

  geom2::Arrangement arrangement;
  std::string arrangementError;

  check(geom2::buildArrangement(rings, limits, arrangement, &arrangementError), "the whole grid arranges: " + arrangementError);
  check(geom2::arrangementFault(arrangement).empty(), "the arrangement is sound");
  check(arrangement.faces.size() == grid.mesh.faces() + 1, "the grid's quads do not overlap (" + std::to_string(arrangement.faces.size()) + " vs " + std::to_string(grid.mesh.faces() + 1) + ")");
}

void testDeterminism()
{
  std::cout << "determinism\n";

  CityGrid a, b, c;
  std::string error;

  check(buildCityGrid(lakeFixture(), defaults(), a, &error), "first run");
  check(buildCityGrid(lakeFixture(), defaults(), b, &error), "second run");

  check(a.mesh.vertices == b.mesh.vertices, "two runs give identical vertices");
  check(a.quality.faces == b.quality.faces, "two runs give identical face counts");

  std::vector<std::vector<Position>> permuted = lakeFixture();
  std::reverse(permuted.begin(), permuted.end());

  check(buildCityGrid(permuted, defaults(), c, &error), "permuted run");
  check(a.mesh.vertices == c.mesh.vertices, "permuting the planning cells changes nothing");

  CityGridSettings other = defaults();
  other.lattice.seed = 99;

  CityGrid d;
  check(buildCityGrid(lakeFixture(), other, d, &error), "a different seed builds");
  check(a.mesh.vertices != d.mesh.vertices, "a different seed gives a different grid");
}

void testBudgetsAndDegenerates()
{
  std::cout << "budgets and degenerate input\n";

  {
    CityGridSettings tight = defaults();
    tight.lattice.maximumHexes = 1;

    CityGrid grid;
    std::string error;

    check(!buildCityGrid(ellFixture(), tight, grid, &error), "a hex budget that cannot be met fails");
    check(!error.empty(), "the failure states a reason: " + error);
    check(grid.quality.faces == 0, "a failed build leaves no partial grid");
  }

  {
    CityGrid grid;
    std::string error;

    check(!buildCityGrid({}, defaults(), grid, &error), "no planning cells fails");
  }

  {
    CityGrid grid;
    std::string error;
    const std::vector<std::vector<Position>> sliver = {rectangle(0, 0, 60, .4)};

    check(!buildCityGrid(sliver, defaults(), grid, &error), "a cell thinner than a triangle fails with a reason");
    check(!error.empty(), "the sliver failure states a reason: " + error);
  }

  {
    // Two disjoint components: the largest is gridded, the rest are recorded.
    CityGrid grid;
    std::string error;
    const std::vector<std::vector<Position>> split = {rectangle(0, 0, 180, 180), rectangle(600, 600, 660, 660)};

    check(buildCityGrid(split, defaults(), grid, &error), "disjoint components build: " + error);
    check(!grid.ledger.empty(), "the omitted component is on the ledger");
  }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 1)
  {
    svgDirectory = argv[1];
  }

  std::cout << "CityGridTests\n";

  testQualityGate(ellFixture(), "ell");
  testQualityGate(lakeFixture(), "lake");
  testQualityGate(isthmusFixture(), "isthmus");
  testSeam();
  testRelaxationEarnsItsPlace();
  testNonSelfIntersection();
  testDeterminism();
  testBudgetsAndDegenerates();

  if (failures != 0)
  {
    std::cerr << failures << " failed\n";

    return 1;
  }

  std::cout << "all passed\n";

  return 0;
}
