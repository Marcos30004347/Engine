#pragma once

#include "PatchRegion.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace procedural::world::generator {

// Planar geometry shared by every generator that hands regions to a child:
// clipping, welding of shared boundaries, the consumers' own convexity and
// overlap predicates, and the union of an exact tiling.

// Micrometre-quantised vertex identity for edge cancellation and welding.
using TilingKey = std::pair<int64_t, int64_t>;

TilingKey tilingKey(Position p);

double cross(Position a, Position b, Position c);

double signedArea(const ConvexRing &ring);

double length(Position a, Position b);

// Area-weighted centroid; falls back to the first vertex for a degenerate ring.
Position centroid(const ConvexRing &ring);

// Mean of the vertices; the master plan measures cell centres this way.
Position vertexMean(const ConvexRing &ring);

bool insideEvenOdd(const ConvexRing &ring, Position point);

bool strictlyInsideConvex(const ConvexRing &ring, Position point);

double segmentDistance(Position point, Position a, Position b);

double polygonDistance(Position point, const ConvexRing &ring);

bool onRing(const ConvexRing &ring, Position point, double tolerance);

// Sutherland-Hodgman against a rectangle; convex in, convex out, empty when
// the result is degenerate.
ConvexRing clipToRect(ConvexRing ring, double minX, double minZ, double maxX, double maxZ);

// Snap T-junction vertices onto the neighbouring edge and insert them, so the
// tiling shares exact vertices instead of micron-sized overlaps.
void weldTiling(std::vector<ConvexRing> &rings);

// The rural and master-plan generators' own predicates with their absolute
// tolerances; returns an empty string when the ring passes.
std::string tilingFault(const ConvexRing &ring, double minX, double minZ, double maxX, double maxZ, double crossTolerance, double vertexTolerance);

bool tilesOverlap(const ConvexRing &a, const ConvexRing &b, double tolerance);

// Union of exactly tiling convex cells by directed-edge cancellation; every
// ring stays simple by taking the rightmost turn at pinch vertices.
bool unionTiles(const std::vector<ConvexRing> &tiles, std::vector<ConvexRing> &outers, std::vector<ConvexRing> &holes);

} // namespace procedural::world::generator
