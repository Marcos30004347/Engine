#pragma once

#include "procedural/Predicates.hpp"

#include <cstdint>
#include <vector>

namespace procedural
{

// Poisson-disk (blue-noise) sampling by parallel dart throwing.
//
// The region is divided into cells sized so that a cell can hold at most one
// sample, and cells are processed in independent blocks. That is what makes this
// the sampler to move onto compute: a block reads only its own cells and a fixed
// halo of neighbours, so blocks carry no dependency on each other and the whole
// grid is one dispatch. A sequential dart-throwing loop, which is the usual way
// to write this, has a dependency on every previously accepted sample and cannot
// be parallelised at all.
//
// The guarantee is a hard one: no two samples closer than `radius`.
class PoissonDiskSampler
{
public:
  // Reproducibility differs sharply between the two, which matters for a
  // compute port because a GPU rounds differently again:
  //
  //   RandomThrows is bit-identical across compiler settings - verified at -O0,
  //   -O2 and -O2 -ffp-contract=off, all 373 samples identical. It only ever
  //   compares existing grid quantities, and its geometric decisions go through
  //   the exact predicates.
  //
  //   FillVoids is not, and cannot be made so by routing more decisions through
  //   exact predicates. It *constructs* new sample positions from circle-circle
  //   intersections and chord subdivisions, which need square roots; the
  //   constructed coordinates differ between targets and the exact predicates
  //   then answer correctly about different points. Measured: 454 / 448 / 425
  //   samples under those same three settings. Exactness fixes decisions, not
  //   constructions.
  //
  // Use RandomThrows where the result has to match a CPU reference.
  enum class Method : uint8_t
  {
    // After dart throwing settles, find the uncovered gaps between accepted
    // samples and place additional samples inside them. Denser and more even.
    FillVoids = 0,
    // Dart throwing alone.
    RandomThrows = 1,
  };

  struct Settings
  {
    // Minimum distance between any two samples.
    double radius = 1.0;
    double width = 1.0;
    double height = 1.0;
    uint64_t seed = 0u;
    Method method = Method::FillVoids;
  };

  // Upper bound on the samples `generate` can emit for a region. Exposed
  // because it is a pure function of the grid and callers sizing their own
  // storage should not have to run the sampler to find out.
  static uint64_t maximumSampleCount(double radius, double width, double height);

  // Samples over [0, width) x [0, height). Deterministic in `seed`.
  std::vector<Point2> generate(const Settings &settings);

private:
  // Per-cell occupancy state. A cell is claimed before its sample is confirmed,
  // which is how neighbouring blocks avoid both placing into the same gap.
  enum class CellState : uint8_t
  {
    Empty = 0,
    Test = 1,
    Accepted = 2,
    Done = 3,
    Rejected = 4,
  };

  uint64_t dartThrowing(uint64_t seed, uint64_t blockX, uint64_t blockY);
  uint64_t generateTriangleVoids(uint64_t seed, uint64_t blockX, uint64_t blockY);
  uint64_t pointInsideTriangleVoids(uint64_t seed, uint64_t blockX, uint64_t blockY);
  void encodeTriangle(
      Point2 p1, Point2 p2, Point2 p3, Point2 origin, int64_t index, char slot);
  uint64_t dividePolygonByChord(
      uint64_t tail, Point2 polygonVertices[16][16], uint64_t polygonSize[16],
      Point2 circleCentre, double radius, int polygon, uint64_t polygonCell);

  // The grid. Held as members rather than threaded through every call because
  // every pass below reads all of it; on a compute port these are the buffers.
  int64_t mRows = 0;
  int64_t mCols = 0;
  double mRadius = 0.0;
  double mCellSize = 0.0;
  std::vector<CellState> mCellState;
  // Two bytes per cell: the sample's position within its own cell, quantised.
  std::vector<uint8_t> mCellPointCoords;
  // Encoded void triangles per cell, used only by Method::FillVoids.
  std::vector<uint8_t> mCellTriangles;
};

} // namespace procedural
