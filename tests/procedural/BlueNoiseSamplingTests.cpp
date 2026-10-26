// Blue-noise sampling, ported from procedural_generation_lab.
//
// Two algorithms live here and they are different things:
//
//   pseudo_poison_sampling     - parallel dart throwing over a block/cell grid,
//                                producing a Poisson-disk set directly. This is
//                                the one shaped for compute: cells own their
//                                state and blocks are independent.
//   weighted_sample_elimination - Yuksel's method, taking a dense input set and
//                                eliminating down to a target count while
//                                keeping the remainder well separated.
//
// What makes a sample set "blue noise" is a property, not a value, so the tests
// check the property: a hard minimum separation for the Poisson set, and for
// elimination, that the surviving points are better separated than an arbitrary
// subset of the same size taken from the same input.

#include "procedural/Geometry.hpp"
#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/WeightedSampleElimination.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

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

double nearestNeighbourDistance(const std::vector<Point2> &points, const size_t index)
{
  double best = std::numeric_limits<double>::max();
  for (size_t other = 0; other < points.size(); ++other)
  {
    if (other == index) continue;
    const double dx = points[index].x - points[other].x;
    const double dy = points[index].y - points[other].y;
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
  }
  return best;
}

double minimumSeparation(const std::vector<Point2> &points)
{
  double best = std::numeric_limits<double>::max();
  for (size_t i = 0; i < points.size(); ++i)
    best = std::min(best, nearestNeighbourDistance(points, i));
  return best;
}

double meanNearestNeighbour(const std::vector<Point2> &points)
{
  if (points.empty()) return 0.0;
  double total = 0.0;
  for (size_t i = 0; i < points.size(); ++i) total += nearestNeighbourDistance(points, i);
  return total / static_cast<double>(points.size());
}

std::vector<Point2> runPoisson(
    const double radius, const double width, const double height, const uint64_t seed,
    const PoissonDiskSampler::Method method = PoissonDiskSampler::Method::FillVoids)
{
  PoissonDiskSampler sampler;
  PoissonDiskSampler::Settings settings;
  settings.radius = radius;
  settings.width = width;
  settings.height = height;
  settings.seed = seed;
  settings.method = method;
  return sampler.generate(settings);
}

// The defining property of a Poisson-disk set: no two samples closer than the
// radius it was asked for.
void testPoissonMinimumDistance()
{
  std::cout << "poisson disk minimum separation\n";
  const double radius = 8.0;
  const std::vector<Point2> samples = runPoisson(radius, 200.0, 200.0, 1234);

  std::cout << "  " << samples.size() << " samples for radius " << radius << "\n";
  check(samples.size() > 50, "produced a non-trivial number of samples");
  if (samples.size() < 2) return;

  const double separation = minimumSeparation(samples);
  std::cout << "  minimum separation " << separation << " against radius " << radius << "\n";
  // The grid is built so a cell holds at most one sample; the guarantee is the
  // radius itself, allowing for the algorithm's own tolerance.
  check(separation > radius * 0.99, "no two samples closer than the radius");
}

void testPoissonStaysInRegion()
{
  std::cout << "poisson samples stay inside the region\n";
  const double width = 150.0;
  const double height = 90.0;
  const std::vector<Point2> samples = runPoisson(6.0, width, height, 99);

  size_t outside = 0;
  for (const Point2 &p : samples)
    if (p.x < 0.0 || p.y < 0.0 || p.x > width || p.y > height) ++outside;

  std::cout << "  " << samples.size() << " samples, " << outside << " outside\n";
  // This used to leak a sample on non-square regions. The cause was the disc
  // test deciding boundary cases with an epsilon on inexact arithmetic; with it
  // routed through the exact predicate the void-filling geometry no longer
  // strays outside the grid it was given.
  check(outside == 0, "every sample lies within the requested region");
}

// The block loop bounds were swapped upstream, which is invisible on a square
// region and walks off the grid on anything else. These aspect ratios are the
// regression test for that.
void testPoissonNonSquareRegions()
{
  std::cout << "poisson on non-square regions\n";
  const struct
  {
    double width;
    double height;
  } shapes[] = {{200.0, 20.0}, {20.0, 200.0}, {300.0, 40.0}, {40.0, 300.0}};

  bool allProduced = true;
  bool allSeparated = true;
  for (const auto &shape : shapes)
  {
    const std::vector<Point2> samples = runPoisson(5.0, shape.width, shape.height, 7);
    std::cout << "  " << shape.width << " x " << shape.height << " -> " << samples.size()
              << " samples\n";
    if (samples.empty()) allProduced = false;
    if (samples.size() >= 2 && minimumSeparation(samples) <= 5.0 * 0.99)
      allSeparated = false;
  }
  check(allProduced, "every aspect ratio produces samples");
  check(allSeparated, "separation holds on non-square regions");
}

void testPoissonDeterminism()
{
  std::cout << "poisson determinism\n";
  const std::vector<Point2> a = runPoisson(7.0, 120.0, 120.0, 4242);
  const std::vector<Point2> b = runPoisson(7.0, 120.0, 120.0, 4242);
  bool same = a.size() == b.size();
  for (size_t i = 0; same && i < a.size(); ++i)
    same = a[i].x == b[i].x && a[i].y == b[i].y;
  check(same, "same seed produces the same sample set");

  const std::vector<Point2> c = runPoisson(7.0, 120.0, 120.0, 9999);
  check(!(c.size() == a.size() && c.size() > 0 && c[0].x == a[0].x && c[0].y == a[0].y),
        "a different seed produces a different sample set");
}

// A dense uniform-random input, thinned to a target count. The check that
// matters is not "did it return points" but "are they better spread than an
// arbitrary subset", which is the entire purpose of the algorithm.
void testWeightedSampleElimination()
{
  std::cout << "weighted sample elimination\n";

  const double extent = 100.0;
  const unsigned inputCount = 2000;
  const unsigned targetCount = 200;

  std::vector<Point2> input;
  input.reserve(inputCount);
  unsigned state = 987654321u;
  const auto nextUnit = [&state]()
  {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>((state >> 8) & 0xffffffu) / static_cast<double>(0xffffff);
  };
  for (unsigned i = 0; i < inputCount; ++i)
  {
    Point2 p;
    p.x = nextUnit() * extent;
    p.y = nextUnit() * extent;
    input.push_back(p);
  }

  WeightedSampleElimination eliminator;
  WeightedSampleElimination::Settings settings;
  settings.regionArea = extent * extent;
  settings.targetCount = targetCount;
  const std::vector<Point2> result = eliminator.eliminate(input, {}, settings);

  std::cout << "  kept " << result.size() << "/" << targetCount << "\n";
  check(result.size() == targetCount, "the requested number of samples is returned");
  if (result.size() != targetCount) return;

  // Every survivor has to be one of the inputs; elimination selects, it does
  // not synthesise.
  size_t foreign = 0;
  for (const Point2 &r : result)
  {
    bool found = false;
    for (const Point2 &p : input)
      if (p.x == r.x && p.y == r.y)
      {
        found = true;
        break;
      }
    if (!found) ++foreign;
  }
  check(foreign == 0, "every survivor is one of the input points");

  // The property under test. An arbitrary subset of the same size from the same
  // input is the baseline; elimination has to beat it.
  std::vector<Point2> arbitrary(input.begin(), input.begin() + targetCount);
  const double eliminatedMean = meanNearestNeighbour(result);
  const double arbitraryMean = meanNearestNeighbour(arbitrary);
  std::cout << "  mean nearest-neighbour distance: eliminated " << eliminatedMean
            << " vs arbitrary subset " << arbitraryMean << "\n";
  // The property the whole algorithm exists for. An arbitrary subset of the
  // same size from the same input is the baseline it has to beat.
  check(
      eliminatedMean > arbitraryMean,
      "eliminated set is better separated than an arbitrary subset");
}

// Asking for at least as many samples as were supplied is a pass-through, and
// it is the one path that always worked upstream.
void testWeightedSampleEliminationPassThrough()
{
  std::cout << "weighted sample elimination pass-through\n";
  std::vector<Point2> input;
  for (int i = 0; i < 16; ++i)
  {
    Point2 p;
    p.x = static_cast<double>(i);
    p.y = static_cast<double>(i * 2);
    input.push_back(p);
  }
  WeightedSampleElimination eliminator;
  WeightedSampleElimination::Settings settings;
  settings.regionArea = 100.0;
  settings.targetCount = input.size();
  const std::vector<Point2> result = eliminator.eliminate(input, {}, settings);

  bool identical = true;
  for (size_t i = 0; i < input.size(); ++i)
    identical = identical && result[i].x == input[i].x && result[i].y == input[i].y;
  check(identical, "requesting every sample returns the input unchanged");
}

} // namespace

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout.setf(std::ios::unitbuf);

  initialisePredicates();

  std::cout << "Blue-noise sampling tests\n\n";
  testPoissonMinimumDistance();
  testPoissonStaysInRegion();
  testPoissonNonSquareRegions();
  testPoissonDeterminism();
  testWeightedSampleElimination();
  testWeightedSampleEliminationPassThrough();

  if (failures != 0)
  {
    std::cerr << "\n" << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "\nAll blue-noise sampling tests passed\n";
  return 0;
}
