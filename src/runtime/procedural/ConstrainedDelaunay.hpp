#pragma once

#include "procedural/Predicates.hpp"

#include <cstdint>
#include <vector>

namespace procedural
{

// A constraint: the two point indices that must end up joined by an edge.
struct Constraint
{
  int64_t origin = 0;
  int64_t end = 0;

  Constraint() = default;

  Constraint(const int64_t inOrigin, const int64_t inEnd) : origin(inOrigin), end(inEnd)
  {
  }
};

// Constrained Delaunay triangulation.
//
// The state is deliberately kept as parallel arrays of indices rather than as
// linked triangle objects, and the algorithm as a fixed sequence of passes over
// those arrays - locate, insert, mark, flip, relink - each of which touches
// every element independently. That is not an accident of the port: it is what
// lets the whole thing move onto compute dispatches later, with each pass
// becoming a kernel and each array a buffer. Restructuring it into a
// pointer-linked mesh would read better and would throw that away.
//
// `kForInParallel` marks the loops that carry no cross-iteration dependency, so
// the ones that can become a dispatch stay identifiable.
class ConstrainedDelaunayTriangulator
{
public:
  struct Settings
  {
    // Passes to run before giving up. Zero means "until it converges", which is
    // safe only because of the stall detection described on `triangulate`.
    uint64_t maximumIterations = 0u;
    // Restore the empty-circumcircle property after each round of constraint
    // insertion. See `triangulate` for why this can prevent convergence.
    bool ensureDelaunay = true;
  };

  // Result of a run. `triangleVertices` holds three point indices per triangle;
  // a triangle whose first index is `kInvalidIndex` occupies an unused slot, so
  // `triangleCount()` is a capacity and not a live count.
  static constexpr int64_t kInvalidIndex = -1;

  ConstrainedDelaunayTriangulator() = default;

  // Triangulates `points`, forcing every constraint to appear as an edge.
  //
  // Returns false when the run stalled - when neither a point nor a constraint
  // was newly inserted for several consecutive passes. That is not a
  // theoretical case: with `ensureDelaunay` set, restoring the circumcircle
  // property can undo the marking that constraint insertion depends on, and
  // certain arrangements of long overlapping constraints never reach a fixed
  // point. Detecting it is what keeps `maximumIterations == 0` from hanging.
  bool triangulate(
      const std::vector<Point2> &points, const std::vector<Constraint> &constraints,
      const Settings &settings);

  bool triangulate(
      const std::vector<Point2> &points, const std::vector<Constraint> &constraints)
  {
    return triangulate(points, constraints, Settings{});
  }

  // Number of triangle slots, live and unused alike.
  uint64_t triangleCount() const
  {
    return mTrianglesCount;
  }

  bool triangleExists(const uint64_t triangle) const
  {
    return vertexOfTriangle(triangle, 0) != kInvalidIndex &&
        vertexOfTriangle(triangle, 1) != kInvalidIndex &&
        vertexOfTriangle(triangle, 2) != kInvalidIndex;
  }

  int64_t vertexOfTriangle(const uint64_t triangle, const uint32_t corner) const
  {
    return mTriangleVertices[triangle * 3u + corner];
  }

  int64_t neighbourOfTriangle(const uint64_t triangle, const uint32_t corner) const
  {
    return mTriangleNeighbours[triangle * 6u + corner];
  }

  // All points, the three bounding super-triangle vertices included. Those
  // occupy the last three slots; a triangle touching one of them lies outside
  // the input's own triangulation and is not expected to satisfy the
  // circumcircle property against the input points.
  Point2 vertex(const int64_t index) const
  {
    return Point2(mPointCoordinates[index * 2], mPointCoordinates[index * 2 + 1]);
  }

  uint64_t inputPointCount() const
  {
    return mPointsCount;
  }

  bool pointIsIncluded(const uint64_t point) const
  {
    return mPointIsIncluded[point] != 0;
  }

  bool constraintIsIncluded(const uint64_t constraint) const
  {
    return mSegmentIsIncluded[constraint] != 0;
  }

  // Live triangles, as flat triples of point indices. Convenience for callers
  // that want the mesh rather than the working state.
  std::vector<int64_t> collectTriangles() const;

private:
  // Element accessors. Named for what they mean rather than for the array they
  // land in, because the arrays are an encoding and the callers below are not.
  Point2 vertexCoordinates(int64_t point) const;
  void setVertexCoordinates(int64_t point, const Point2 &value);
  int64_t triangleVertexToInsert(int64_t triangle) const;
  void setTriangleVertexToInsert(int64_t triangle, int64_t vertex);
  int8_t vertexIsIncluded(int64_t point) const;
  void setVertexIsIncluded(int64_t point, int8_t included);
  int64_t pointContainingTriangle(int64_t point) const;
  void setPointContainingTriangle(int64_t point, int64_t triangle);
  int64_t triangleVertex(int64_t triangle, int64_t corner) const;
  void setTriangleVertex(int64_t triangle, int64_t corner, int64_t vertex);
  int64_t triangleNeighbour(int64_t triangle, int64_t corner) const;
  void setTriangleNeighbour(int64_t triangle, int64_t corner, int64_t value);
  int64_t triangleFutureNeighbour(int64_t triangle, int64_t corner) const;
  void setTriangleFutureNeighbour(int64_t triangle, int64_t corner, int64_t value);
  int8_t triangleEdgeState(int64_t triangle, int64_t corner) const;
  void setTriangleEdgeState(int64_t triangle, int64_t corner, int8_t state);
  int8_t triangleEdgeToFlip(int64_t triangle) const;
  void setTriangleEdgeToFlip(int64_t triangle, int8_t edge);
  int64_t triangleSegmentId(int64_t triangle) const;
  void setTriangleSegmentId(int64_t triangle, int64_t segment);
  int64_t segmentVertex(int64_t segment, int64_t which) const;
  void setSegmentVertex(int64_t segment, int64_t which, int64_t vertex);
  int8_t segmentIsIncluded(int64_t segment) const;
  void setSegmentIsIncluded(int64_t segment, int8_t included);

  // The passes. Each is independent per element and carries no cross-iteration
  // dependency unless its comment says otherwise, which is what makes them
  // candidates for compute dispatches.
  void locationStep();
  void preUpdate();
  void insertionStep();
  void releaseFinishedSegmentClaims();
  void markingStep();
  void determineInCircleStep();
  void determineSwapping();
  void swapEdges();
  void updateNeighbours();

  void initialiseTriangles();
  void initialisePoints();
  void initialiseSegments();
  void initialiseSuperTriangle();
  void initialiseState(
      const std::vector<Point2> &points, const std::vector<Constraint> &constraints);

  std::vector<double> mPointCoordinates;
  std::vector<int8_t> mPointIsIncluded;
  std::vector<int64_t> mPointContainingTriangle;

  std::vector<int64_t> mTriangleVertices;
  std::vector<int64_t> mTriangleVertexToInsert;
  std::vector<int64_t> mTriangleSegmentIndex;
  std::vector<int64_t> mTriangleNeighbours;
  std::vector<int8_t> mTriangleEdgeState;
  std::vector<int8_t> mTriangleEdgeToFlip;

  std::vector<int64_t> mSegmentPointIndices;
  std::vector<int8_t> mSegmentIsIncluded;

  uint64_t mTrianglesCount = 0u;
  uint64_t mPointsCount = 0u;
  uint64_t mSegmentsCount = 0u;

  int8_t mThereIsEdgesToSwap = 0;
  int8_t mThereIsPointsToInsert = 0;
  int8_t mThereIsSegmentToInsert = 0;
};

} // namespace procedural
