// Constrained Delaunay triangulation, ported from procedural_generation_lab.
//
// The implementation is written as a sequence of data-parallel steps over flat
// arrays (`location_step`, `insertion_step`, `marking_step`, `swap_edges`, ...)
// so it can be moved onto compute dispatches without restructuring. That makes
// the interesting question not "does it return something" but "does the state
// it leaves behind satisfy the definition", so the checks below are the
// definition itself: every input point present, every triangle non-degenerate,
// the empty-circumcircle property tested with the exact predicate, and every
// constrained segment actually present as an edge.

#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/Geometry.hpp"

#include <cstdio>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

using namespace procedural;

namespace
{

// `max_iterations == 0` means "run until the fixed point", and the segment
// insertion below does not always reach one - so every call here is bounded.
// Well above what convergence needs: the 64-point cases settle in single-digit
// iterations.
const uint64_t kMaxIterations = 512;
using Triangulator = ConstrainedDelaunayTriangulator;

int failures = 0;

void check(const bool condition, const char *what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

Point2 makePoint(const double x, const double y)
{
  Point2 p;
  p.x = x;
  p.y = y;
  return p;
}

// A triangle touching the bounding super-triangle is outside the input's own
// triangulation, so the Delaunay property against the input points does not
// apply to it.
bool touchesSuperTriangle(const Triangulator &cdt, const uint64_t t)
{
  for (uint32_t corner = 0; corner < 3; ++corner)
    if (static_cast<uint64_t>(cdt.vertexOfTriangle(t, corner)) >= cdt.inputPointCount())
      return true;
  return false;
}

uint64_t liveTriangleCount(const Triangulator &cdt)
{
  uint64_t live = 0;
  for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
    if (cdt.triangleExists(t)) ++live;
  return live;
}

struct Result
{
  std::vector<Point2> points;
  std::vector<Constraint> segments;
};

// A jittered grid. A perfectly regular grid is entirely cocircular, which is a
// legitimate input but leaves the triangulation ambiguous, so the jitter picks
// one answer out while staying far from degenerate.
Result makeJitteredGrid(const int side, const double jitter)
{
  Result out;
  unsigned state = 12345u;
  const auto next = [&state]()
  {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>((state >> 8) & 0xffffu) / 65535.0 - 0.5;
  };
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x)
      out.points.push_back(makePoint(
          static_cast<double>(x) + next() * jitter,
          static_cast<double>(y) + next() * jitter));
  return out;
}

void testProducesTriangulation()
{
  std::cout << "unconstrained triangulation of a jittered grid\n";
  Result input = makeJitteredGrid(8, 0.25);
  const uint64_t pointCount = static_cast<uint64_t>(input.points.size());

  Triangulator cdt;
  Triangulator::Settings settings;
  settings.maximumIterations = kMaxIterations;
  const bool converged = cdt.triangulate(input.points, {}, settings);
  check(converged, "triangulation converges");

  uint64_t included = 0;
  for (uint64_t p = 0; p < pointCount; ++p)
    if (cdt.pointIsIncluded(p)) ++included;
  check(included == pointCount, "every input point was inserted");

  const uint64_t live = liveTriangleCount(cdt);
  std::cout << "  " << live << " live triangles for " << pointCount << " points\n";
  check(live > 0, "triangulation is non-empty");

  // Every live triangle must have three distinct vertices and nonzero area.
  uint64_t degenerate = 0;
  for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
  {
    if (!cdt.triangleExists(t)) continue;
    const int64_t a = cdt.vertexOfTriangle(t, 0);
    const int64_t b = cdt.vertexOfTriangle(t, 1);
    const int64_t c = cdt.vertexOfTriangle(t, 2);
    if (a == b || b == c || a == c)
    {
      ++degenerate;
      continue;
    }
    if (orient2d(cdt.vertex(a), cdt.vertex(b), cdt.vertex(c)) == 0.0)
      ++degenerate;
  }
  check(degenerate == 0, "no degenerate or zero-area triangles");
}

// The defining property: no input point lies strictly inside the circumcircle
// of any triangle. Checked with the exact predicate, because a naive incircle
// reports false violations on exactly the near-cocircular configurations a
// triangulator produces.
void testDelaunayProperty()
{
  std::cout << "empty-circumcircle property\n";
  Result input = makeJitteredGrid(7, 0.3);
  const uint64_t pointCount = static_cast<uint64_t>(input.points.size());

  Triangulator cdt;
  Triangulator::Settings settings;
  settings.maximumIterations = kMaxIterations;
  const bool converged = cdt.triangulate(input.points, {}, settings);
  check(converged, "triangulation converges");

  uint64_t tested = 0;
  uint64_t violations = 0;
  for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
  {
    if (!cdt.triangleExists(t)) continue;
    if (touchesSuperTriangle(cdt, t)) continue;

    const int64_t ia = cdt.vertexOfTriangle(t, 0);
    const int64_t ib = cdt.vertexOfTriangle(t, 1);
    const int64_t ic = cdt.vertexOfTriangle(t, 2);
    Point2 a = cdt.vertex(ia);
    Point2 b = cdt.vertex(ib);
    Point2 c = cdt.vertex(ic);
    // incircle's sign convention requires counterclockwise input.
    if (orient2d(a, b, c) < 0.0) std::swap(b, c);

    ++tested;
    for (uint64_t p = 0; p < pointCount; ++p)
    {
      if (static_cast<int64_t>(p) == ia || static_cast<int64_t>(p) == ib ||
          static_cast<int64_t>(p) == ic)
        continue;
      if (inCircle(a, b, c, cdt.vertex(static_cast<int64_t>(p))) > 0.0)
      {
        ++violations;
        break;
      }
    }
  }

  std::cout << "  " << tested << " interior triangles tested, " << violations
            << " with a point inside their circumcircle\n";
  check(tested > 0, "there are interior triangles to test");
  check(violations == 0, "no triangle contains an input point in its circumcircle");
}

// Every constrained segment has to survive as an edge of some triangle. That is
// the whole difference between this and an unconstrained Delaunay.
void testConstraintsArePresent()
{
  std::cout << "constrained segments are present as edges\n";

  Result input = makeJitteredGrid(6, 0.15);
  const uint64_t pointCount = static_cast<uint64_t>(input.points.size());

  // Diagonals and a long horizontal run, none of which a plain Delaunay
  // triangulation would necessarily choose, so their presence is evidence the
  // constraint was honoured rather than a coincidence.
  const std::vector<std::pair<int64_t, int64_t>> wanted{
    {0, 7}, {14, 21}, {28, 35}, {4, 11}, {12, 13}};
  std::vector<Constraint> segments;
  for (const auto &w : wanted)
  {
    Constraint e;
    e.origin = w.first;
    e.end = w.second;
    segments.push_back(e);
  }

  Triangulator cdt;
  Triangulator::Settings settings;
  settings.maximumIterations = kMaxIterations;
  const bool converged = cdt.triangulate(input.points, segments, settings);
  check(converged, "triangulation converges");

  std::set<std::pair<int64_t, int64_t>> edges;
  for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
  {
    if (!cdt.triangleExists(t)) continue;
    for (int corner = 0; corner < 3; ++corner)
    {
      int64_t u = cdt.vertexOfTriangle(t, corner);
      int64_t v = cdt.vertexOfTriangle(t, (corner + 1) % 3);
      if (u > v) std::swap(u, v);
      edges.insert({u, v});
    }
  }

  uint64_t present = 0;
  for (const Constraint &segment : segments)
  {
    int64_t u = segment.origin;
    int64_t v = segment.end;
    if (u > v) std::swap(u, v);
    if (edges.count({u, v}) != 0) ++present;
  }

  std::cout << "  " << present << "/" << segments.size()
            << " constrained segments found among " << edges.size() << " edges\n";
  check(present == segments.size(), "every constrained segment is an edge");
}

// The parallel steps must not let ordering leak into the result: the same input
// has to produce the same triangulation every time, or a GPU port has no
// reference to be checked against.
void testDeterminism()
{
  std::cout << "determinism\n";
  Result input = makeJitteredGrid(6, 0.2);
  const uint64_t pointCount = static_cast<uint64_t>(input.points.size());

  std::vector<int64_t> first;
  std::vector<int64_t> second;
  for (int run = 0; run < 2; ++run)
  {
    std::vector<Point2> copy = input.points;
    Triangulator cdt;
    Triangulator::Settings settings;
    settings.maximumIterations = kMaxIterations;
    if (!cdt.triangulate(copy, {}, settings))
    {
      check(false, "triangulation converges");
      return;
    }
    std::vector<int64_t> &out = run == 0 ? first : second;
    for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
      for (int corner = 0; corner < 3; ++corner)
        out.push_back(cdt.vertexOfTriangle(t, corner));
  }

  check(first == second, "two runs produce an identical triangulation");
}

// A collinear run is the case the exact predicates exist for: orient2d returns
// exactly zero and the insertion logic has to cope rather than pick a side by
// rounding.
void testCollinearInput()
{
  std::cout << "degenerate input with collinear points\n";
  std::vector<Point2> points;
  for (int i = 0; i < 8; ++i) points.push_back(makePoint(static_cast<double>(i), 0.0));
  for (int i = 0; i < 8; ++i) points.push_back(makePoint(static_cast<double>(i), 1.0));
  const uint64_t pointCount = static_cast<uint64_t>(points.size());

  Triangulator cdt;
  Triangulator::Settings settings;
  settings.maximumIterations = kMaxIterations;
  const bool converged = cdt.triangulate(points, {}, settings);
  check(converged, "collinear input converges");

  uint64_t included = 0;
  for (uint64_t p = 0; p < pointCount; ++p)
    if (cdt.pointIsIncluded(p)) ++included;
  std::cout << "  " << included << "/" << pointCount << " points inserted, "
            << liveTriangleCount(cdt) << " live triangles\n";
  check(included == pointCount, "every collinear point was inserted");
}

// Long overlapping constraints, which used to livelock.
//
// Three long diagonals spanning two rows and five columns each, stacked one
// above the next. Two would insert and the third never would: a flip dropped
// both triangles' segment claims, the two sides of the shared edge then
// disagreed about which segment owned it, and the Delaunay pass cleared the
// crossing mark that the flip depended on. Regression test for that.
void testSegmentInsertionConvergence()
{
  std::cout << "segment insertion reaches a fixed point\n";

  Result input = makeJitteredGrid(6, 0.15);
  const uint64_t pointCount = static_cast<uint64_t>(input.points.size());

  std::vector<Constraint> segments;
  for (int row = 0; row + 1 < 6; row += 2)
  {
    Constraint e;
    e.origin = row * 6;
    e.end = (row + 1) * 6 + 5;
    segments.push_back(e);
  }

  Triangulator cdt;
  Triangulator::Settings settings;
  settings.maximumIterations = kMaxIterations;
  const bool converged = cdt.triangulate(input.points, segments, settings);

  std::set<std::pair<int64_t, int64_t>> edges;
  for (uint64_t t = 0; t < cdt.triangleCount(); ++t)
  {
    if (!cdt.triangleExists(t)) continue;
    for (int corner = 0; corner < 3; ++corner)
    {
      int64_t u = cdt.vertexOfTriangle(t, corner);
      int64_t v = cdt.vertexOfTriangle(t, (corner + 1) % 3);
      if (u > v) std::swap(u, v);
      edges.insert({u, v});
    }
  }
  uint64_t present = 0;
  for (const Constraint &segment : segments)
  {
    int64_t u = segment.origin;
    int64_t v = segment.end;
    if (u > v) std::swap(u, v);
    if (edges.count({u, v}) != 0) ++present;
  }

  std::cout << "  " << present << "/" << segments.size()
            << " long stacked diagonals inserted after " << kMaxIterations
            << " iterations, converged = " << (converged ? "yes" : "no") << "\n";

  // The stall detection still guards this: it is what turns a livelock into a
  // false return rather than a hang, and it must stay for any input that
  // genuinely cannot converge.
  check(converged, "segment insertion terminates cleanly");
  check(present == segments.size(), "every long stacked diagonal is inserted");
}

} // namespace

int main()
{
  // Unbuffered: these tests exercise a fixed-point iteration, and if it fails
  // to converge the buffered output is lost with the process.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout.setf(std::ios::unitbuf);

  initialisePredicates();

  std::cout << "Constrained Delaunay triangulation tests\n\n";
  testProducesTriangulation();
  testDelaunayProperty();
  testConstraintsArePresent();
  testDeterminism();
  testCollinearInput();
  testSegmentInsertionConvergence();

  if (failures != 0)
  {
    std::cerr << "\n" << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "\nAll constrained Delaunay tests passed\n";
  return 0;
}
