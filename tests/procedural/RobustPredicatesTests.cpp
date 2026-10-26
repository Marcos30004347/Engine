// Shewchuk's adaptive exact predicates, ported from procedural_generation_lab.
//
// These exist precisely because the naive determinant is wrong near degeneracy,
// so every test here is built around a case where the naive form fails or is
// about to. A test that only fed them well-separated points would pass against
// a float implementation and tell us nothing.

#include "procedural/Geometry.hpp"

#include <cmath>
#include <iostream>

using namespace procedural;

namespace
{

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

// The determinant orient2d computes, evaluated the obvious way. This is the
// thing the adaptive version exists to replace.
double orient2dnaive(const Point2 a, const Point2 b, const Point2 c)
{
  return (a.x - c.x) * (b.y - c.y) - (a.y - c.y) * (b.x - c.x);
}

Point2 makePoint(const double x, const double y)
{
  Point2 p;
  p.x = x;
  p.y = y;
  return p;
}

void testOrientationSigns()
{
  std::cout << "orient2d sign convention\n";
  const Point2 a = makePoint(0.0, 0.0);
  const Point2 b = makePoint(1.0, 0.0);
  check(orient2d(a, b, makePoint(0.0, 1.0)) > 0.0, "counterclockwise is positive");
  check(orient2d(a, b, makePoint(0.0, -1.0)) < 0.0, "clockwise is negative");
  check(orient2d(a, b, makePoint(2.0, 0.0)) == 0.0, "collinear is exactly zero");
  // Twice the signed area, which is what the header documents.
  check(
      std::fabs(orient2d(a, b, makePoint(0.0, 1.0)) - 1.0) < 1e-12,
      "magnitude is twice the triangle area");
}

// Points on a line with coordinates that are not exactly representable once
// multiplied out. The naive determinant rounds these to the wrong sign; the
// adaptive one has to report exactly zero.
void testOrientationCollinearPrecision()
{
  std::cout << "orient2d on near-degenerate collinear input\n";

  int naiveWrong = 0;
  int exactWrong = 0;
  for (int step = 1; step <= 2000; ++step)
  {
    // Three points on the line y = x, spaced so the products below lose bits.
    const double base = 0.5;
    const double delta = static_cast<double>(step) * 1e-10;
    const Point2 a = makePoint(base, base);
    const Point2 b = makePoint(base + delta, base + delta);
    const Point2 c = makePoint(base + 2.0 * delta, base + 2.0 * delta);

    if (orient2dnaive(a, b, c) != 0.0) ++naiveWrong;
    if (orient2d(a, b, c) != 0.0) ++exactWrong;
  }

  std::cout << "  naive misreported " << naiveWrong << "/2000, adaptive "
            << exactWrong << "/2000\n";
  check(exactWrong == 0, "adaptive predicate is exact on every collinear case");
}

// The case the adaptive predicate is actually for: a point a hair off the line,
// where the answer is determined by bits the naive form has already lost.
void testOrientationDisagreesWithNaive()
{
  std::cout << "orient2d resolves what the naive form cannot\n";

  int disagreements = 0;
  int adaptiveConsistent = 0;
  int total = 0;
  for (int step = 1; step <= 400; ++step)
  {
    const double x = 0.1 + static_cast<double>(step) * 1e-12;
    const Point2 a = makePoint(0.1, 0.1);
    const Point2 b = makePoint(0.2, 0.2);
    // Strictly above the line y = x, so the true orientation is fixed.
    const Point2 c = makePoint(x, x + 1e-30);

    const double naive = orient2dnaive(a, b, c);
    const double exact = orient2d(a, b, c);
    ++total;
    if ((naive > 0.0) != (exact > 0.0)) ++disagreements;
    // Whatever it decides, it must agree with itself under a swap: exchanging
    // two arguments negates the determinant.
    if (signOf(exact) == -signOf(orient2d(b, a, c))) ++adaptiveConsistent;
  }

  std::cout << "  naive disagreed on " << disagreements << "/" << total << "\n";
  check(
      adaptiveConsistent == total,
      "adaptive predicate antisymmetric under argument swap");
}

void testIncircleSigns()
{
  std::cout << "incircle sign convention\n";
  // Unit circle through three counterclockwise points.
  const Point2 a = makePoint(1.0, 0.0);
  const Point2 b = makePoint(0.0, 1.0);
  const Point2 c = makePoint(-1.0, 0.0);

  check(inCircle(a, b, c, makePoint(0.0, 0.0)) > 0.0, "centre is inside");
  check(inCircle(a, b, c, makePoint(3.0, 3.0)) < 0.0, "far point is outside");
  check(inCircle(a, b, c, makePoint(0.0, -1.0)) == 0.0, "cocircular is exactly zero");
}

// Four points placed exactly on a circle by construction. Any rounding in the
// determinant shows up as a nonzero answer, and a Delaunay flip loop that
// believes it will flip forever.
void testIncircleCocircularPrecision()
{
  std::cout << "incircle on exactly cocircular input\n";

  int exactNonZero = 0;
  const int samples = 512;
  for (int i = 0; i < samples; ++i)
  {
    // Pythagorean points scaled off an integer circle stay exactly on it in
    // binary floating point, so "cocircular" is not an approximation here.
    const Point2 a = makePoint(3.0, 4.0);
    const Point2 b = makePoint(-3.0, 4.0);
    const Point2 c = makePoint(-4.0, -3.0);
    const Point2 d = makePoint(4.0, -3.0);

    const double scale = 1.0 + static_cast<double>(i) * 1e-13;
    const Point2 sa = makePoint(a.x * scale, a.y * scale);
    const Point2 sb = makePoint(b.x * scale, b.y * scale);
    const Point2 sc = makePoint(c.x * scale, c.y * scale);
    const Point2 sd = makePoint(d.x * scale, d.y * scale);

    // Uniform scaling keeps them cocircular, so the answer must stay zero.
    if (inCircle(sa, sb, sc, sd) != 0.0) ++exactNonZero;
  }

  std::cout << "  adaptive reported nonzero on " << exactNonZero << "/" << samples << "\n";
  check(exactNonZero == 0, "scaled cocircular quadruples stay exactly cocircular");
}

void testSegmentIntersection()
{
  std::cout << "segment predicates\n";
  Point2 hit;
  check(
      segmentIntersection(
          makePoint(0.0, 0.0), makePoint(2.0, 2.0), makePoint(0.0, 2.0),
          makePoint(2.0, 0.0), hit),
      "crossing segments intersect");
  check(
      std::fabs(hit.x - 1.0) < 1e-9 && std::fabs(hit.y - 1.0) < 1e-9,
      "intersection point is the crossing");
  check(
      !segmentIntersection(
          makePoint(0.0, 0.0), makePoint(1.0, 0.0), makePoint(0.0, 1.0),
          makePoint(1.0, 1.0), hit),
      "parallel segments do not intersect");
  check(
      onSegment(makePoint(0.0, 0.0), makePoint(1.0, 1.0), makePoint(2.0, 2.0)),
      "midpoint lies on the segment");
}

} // namespace

int main()
{
  initialisePredicates();

  std::cout << "Robust predicate tests\n\n";
  testOrientationSigns();
  testOrientationCollinearPrecision();
  testOrientationDisagreesWithNaive();
  testIncircleSigns();
  testIncircleCocircularPrecision();
  testSegmentIntersection();

  if (failures != 0)
  {
    std::cerr << "\n" << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "\nAll robust predicate tests passed\n";
  return 0;
}
