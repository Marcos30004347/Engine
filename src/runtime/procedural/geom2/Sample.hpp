#pragma once

#include "procedural/geom2/Polygon.hpp"

namespace procedural::geom2 {

// Point sampling inside a region.
//
// A façade, not an implementation: `PoissonDiskSampler` and
// `WeightedSampleElimination` already work, are already tested, and already
// carry documented determinism properties that a generic replacement would not.
// What they lack is a way to sample a *region* rather than a rectangle, and one
// tolerance policy shared with the rest of this library.

enum class SampleMethod : uint8_t
{
  // Dart throwing alone. Bit-identical across compiler settings, because every
  // decision it makes is a comparison of existing grid quantities.
  //
  // Use this wherever a result has to match between runs or machines.
  Reproducible,

  // Dart throwing, then filling the uncovered gaps. Denser and more even, and
  // *not* reproducible across compiler settings: it constructs new positions
  // from circle intersections and square roots, so the constructed coordinates
  // differ between targets. Exactness fixes decisions, not constructions.
  Dense
};

struct SampleSettings
{
  double spacingMetres = 10;
  uint64_t seed = 1;
  SampleMethod method = SampleMethod::Reproducible;

  // Samples closer than this to the region's boundary are dropped, so a site
  // never lands where its cell would be a sliver.
  double marginMetres = 0;

  uint32_t maximumSamples = 100000;
};

// Blue-noise samples strictly inside the region, with no two closer than the
// spacing. Concave regions and holes are handled by rejection against the
// polygon, so a sample never lands in a courtyard or across a river.
bool samplePolygon(const Polygon &, const SampleSettings &, std::vector<Point> &out, std::string *error = nullptr);

// Thins an existing point set to a blue-noise subset of the requested size,
// keeping the points the caller already has rather than inventing new ones.
// The way to pick twenty building sites out of two hundred candidates without
// them clumping.
bool thinToCount(const std::vector<Point> &candidates, size_t count, std::vector<Point> &out, double regionAreaMetres, std::string *error = nullptr);

} // namespace procedural::geom2
