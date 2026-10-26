#pragma once

#include "procedural/geom2/Polygon.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geom2 {

// A planar mesh in which every face is a quadrilateral.
//
// The layer between "an arrangement" and "a grid", which this library did not
// have. An arrangement answers where things cross; a quad mesh answers what the
// cells are, who their neighbours are, and what happens when one is subdivided -
// and it answers them in integers, with no search.
//
// Everything here follows from the one restriction. Because a face always has
// four sides, face `f` owns sides `4f..4f+3`, `next` and `prev` are arithmetic,
// the face of a side is a shift, and a face needs no vertex count, no side list
// and no pointer. What a general half-edge structure spends three indirections
// on, this spends none.
//
// Subdividing a quad gives four quads, so the structure is closed under its own
// refinement. That is the property the city grid is built on: one operation
// produces the next level, and the level after that, without the cell type ever
// changing.

inline constexpr uint32_t kNoSide = 0xFFFFFFFFu;
inline constexpr uint32_t kNoFace = 0xFFFFFFFFu;
inline constexpr uint32_t kNoVertex = 0xFFFFFFFFu;

// One side of one quad: the vertex it leaves, and the side of the neighbouring
// quad that runs back along it. `next` is not stored, because it is arithmetic.
struct QuadSide
{
  uint32_t origin = kNoVertex;
  uint32_t twin = kNoSide; // kNoSide on the boundary.
};

// Side `s` belongs to face `s >> 2` and is its `s & 3`-th side.
inline constexpr uint32_t faceOf(const uint32_t side)
{
  return side >> 2;
}

inline constexpr uint32_t sideIndex(const uint32_t side)
{
  return side & 3u;
}

inline constexpr uint32_t sideOf(const uint32_t face, const uint32_t index)
{
  return (face << 2) | (index & 3u);
}

inline constexpr uint32_t nextSide(const uint32_t side)
{
  return (side & ~3u) | ((side + 1) & 3u);
}

inline constexpr uint32_t prevSide(const uint32_t side)
{
  return (side & ~3u) | ((side + 3) & 3u);
}

struct QuadMesh
{
  std::vector<Point> vertices;
  std::vector<QuadSide> sides;      // Four per face, in counter-clockwise order.
  std::vector<uint32_t> vertexSide; // One outgoing side per vertex, kNoSide if isolated.

  uint32_t faces() const
  {
    return uint32_t(sides.size() / 4);
  }

  uint32_t destination(const uint32_t side) const
  {
    return sides[nextSide(side)].origin;
  }

  // The face across a side, or kNoFace at the boundary.
  uint32_t neighbour(const uint32_t side) const
  {
    const uint32_t t = sides[side].twin;

    return t == kNoSide ? kNoFace : faceOf(t);
  }

  // The four corners of a face, counter-clockwise.
  void corners(uint32_t face, Point out[4]) const;

  Point centroid(uint32_t face) const;

  double areaMetres(uint32_t face) const;
};

// The canonical identifier of the undirected edge a side belongs to.
//
// A wall is one wall seen from two rooms, so anything a caller attaches to an
// edge must be attached to both of its sides or to neither. One comparison
// rather than a table: the lower of the two side indices names the pair.
inline uint32_t edgeOf(const QuadMesh &mesh, const uint32_t side)
{
  const uint32_t t = mesh.sides[side].twin;

  return (t == kNoSide || side < t) ? side : t;
}

// Every way the structure can be inconsistent, as a message, or empty when it is
// sound. The same role `ringFault` and `arrangementFault` play: one definition of
// valid, called by every operation on its input and its output, so a structural
// break is reported where it happens rather than three stages downstream.
//
// Faults: a vertex index out of range, a side count not a multiple of four, a
// twin that is not involutive, a twin whose endpoints do not reverse this side's,
// a `vertexSide` that does not leave its vertex, a vertex with no outgoing side
// recorded, a face with a repeated corner, a face with a zero-length side, a
// face that is not strictly counter-clockwise, and a face that is not simple.
std::string quadMeshFault(const QuadMesh &mesh);

// Merge vertices that share a grid point, then relink the twins.
//
// Exact, and needs no tolerance: coordinates are on the integer grid, so two
// vertices are the same vertex or they are not. Faces are untouched; only the
// vertex array shrinks and the origins are remapped.
void weld(QuadMesh &mesh);

// Drop faces, then drop whatever is left unreferenced.
//
// `keep` is indexed by face. Vertices no longer used by any face are removed and
// the remaining indices are compacted, so the result is a valid mesh rather than
// one with holes in its arrays.
void dropFaces(QuadMesh &mesh, const std::vector<bool> &keep);

// A mesh of arbitrary polygons, used only as the input to `orthoSubdivide`.
struct PolyMesh
{
  std::vector<Point> vertices;
  std::vector<std::vector<uint32_t>> faces; // Counter-clockwise, three or more.
};

// Conway's ortho operator: the one construction that makes "everything is a
// quad" true whatever the input was.
//
// An n-gon becomes n quads, each spanning a corner, the two edge midpoints
// beside it and the face centre. A triangle gives three, a quad gives four, and
// the result is conforming across a shared edge because both faces place the
// midpoint at the same grid point.
//
// Midpoints and centres are rounded to the grid, which is the only place this
// operation loses anything. A face whose edges are shorter than two grid units
// cannot be subdivided without collapsing, and is reported rather than folded.
bool orthoSubdivide(const PolyMesh &in, QuadMesh &out, std::string *error = nullptr);

// Ortho subdivision of selected faces of a quad mesh, in place.
//
// `refine` is indexed by face. Unrefined faces are carried over unchanged, so
// the result is a mesh in which a refined face's neighbour may meet two faces
// across what used to be one side - the T-vertex the hierarchy expects. Those
// sides are left with `twin == kNoSide`; the hierarchy layer records the
// one-to-two adjacency, because the mesh has nowhere to put it.
//
// `edgeRemap`, when given, receives for each undirected edge of the input the
// output edges that cover it, so an attribute array attached to the input's
// edges can be carried onto the result.
//
// Three cases, and the third is the one a two-child struct gets wrong: when one
// of an edge's faces refines and the other does not, the refined side becomes
// two half edges while the unrefined side stays whole, and after the twins are
// rebuilt none of the three are paired. The same wall is then three edges in the
// output, and an attribute that reaches only two of them leaves a gap exactly
// where the T-vertex is.
struct EdgeSplit
{
  uint32_t parent = kNoSide; // Canonical input edge.

  // One when neither face refined; two when both did, in the parent's own
  // direction; three when exactly one did - the two halves, then the unrefined
  // face's whole copy.
  uint32_t child[3] = {kNoSide, kNoSide, kNoSide};
  uint8_t count = 0;
};

bool refineFaces(QuadMesh &mesh,
                 const std::vector<bool> &refine,
                 std::vector<uint32_t> *parentOfFace = nullptr,
                 std::vector<EdgeSplit> *edgeRemap = nullptr,
                 std::string *error = nullptr);

// Neighbour lists, as the compressed rows every later stage consumes: routing,
// flood fill and constraint propagation all want "the neighbours of face f" and
// none of them should walk sides to get it.
struct FaceAdjacency
{
  std::vector<uint32_t> offset;    // faces() + 1 entries.
  std::vector<uint32_t> neighbour; // Face indices.
  std::vector<uint8_t> side;       // Which side of the owning face each crosses.
};

FaceAdjacency faceAdjacency(const QuadMesh &mesh);

// The boundary cycles, as chains of vertex indices. The outer cycle is first;
// the rest bound holes. Each chain is closed implicitly, as a `Ring` is.
std::vector<std::vector<uint32_t>> boundaryCycles(const QuadMesh &mesh);

} // namespace procedural::geom2
