// The quad mesh: topology, ortho subdivision, welding and refinement.
//
// Every assertion here is about the structure rather than about a shape, because
// the structure is what the city grid is built on: if a twin is not involutive
// or a subdivided face does not tile its parent, nothing downstream can be
// diagnosed. The validity predicate is tested against deliberately broken meshes
// first - a predicate that never fires is not a predicate.

#include "procedural/geom2/Arrangement.hpp"
#include "procedural/geom2/QuadMesh.hpp"

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

// A rows x columns grid of unit quads, built directly rather than through any
// operation under test.
QuadMesh lattice(const uint32_t rows, const uint32_t columns, const double step = 1.0)
{
  QuadMesh mesh;

  for (uint32_t i = 0; i <= rows; ++i)
  {
    for (uint32_t j = 0; j <= columns; ++j)
    {
      mesh.vertices.push_back(at(double(j) * step, double(i) * step));
    }
  }

  const auto index = [&](const uint32_t i, const uint32_t j) { return i * (columns + 1) + j; };

  for (uint32_t i = 0; i < rows; ++i)
  {
    for (uint32_t j = 0; j < columns; ++j)
    {
      mesh.sides.push_back({index(i, j), kNoSide});
      mesh.sides.push_back({index(i, j + 1), kNoSide});
      mesh.sides.push_back({index(i + 1, j + 1), kNoSide});
      mesh.sides.push_back({index(i + 1, j), kNoSide});
    }
  }

  weld(mesh);

  return mesh;
}

double totalArea(const QuadMesh &mesh)
{
  double sum = 0;

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    sum += mesh.areaMetres(f);
  }

  return sum;
}

PolyMesh singleFace(const std::vector<Point> &corners)
{
  PolyMesh mesh;
  mesh.vertices = corners;
  mesh.faces.push_back({});

  for (uint32_t i = 0; i < corners.size(); ++i)
  {
    mesh.faces.back().push_back(i);
  }

  return mesh;
}

void testFaultPredicate()
{
  std::cout << "quadMeshFault\n";

  const QuadMesh sound = lattice(3, 3);
  check(quadMeshFault(sound).empty(), "a hand-built 3x3 grid is sound");
  check(sound.faces() == 9, "a 3x3 grid has nine faces");
  check(sound.vertices.size() == 16, "a 3x3 grid has sixteen vertices after welding");

  {
    QuadMesh broken = sound;
    broken.sides[0].twin = 7;
    check(!quadMeshFault(broken).empty(), "a non-involutive twin is a fault");
  }

  {
    QuadMesh broken = sound;
    broken.vertices.push_back(at(9, 9));
    broken.vertexSide.push_back(kNoSide);
    check(!quadMeshFault(broken).empty(), "a vertex with no outgoing side is a fault");
  }

  {
    QuadMesh broken = sound;
    std::swap(broken.sides[1], broken.sides[3]);
    check(!quadMeshFault(broken).empty(), "a clockwise face is a fault");
  }

  {
    QuadMesh broken = sound;
    broken.sides[2].origin = broken.sides[0].origin;
    check(!quadMeshFault(broken).empty(), "a face with a repeated corner is a fault");
  }

  {
    QuadMesh broken = sound;
    std::swap(broken.sides[2].origin, broken.sides[3].origin);
    check(!quadMeshFault(broken).empty(), "a self-intersecting face is a fault");
  }

  {
    QuadMesh broken = sound;
    broken.sides.pop_back();
    check(!quadMeshFault(broken).empty(), "a side count that is not a multiple of four is a fault");
  }
}

void testOrthoSubdivision()
{
  std::cout << "orthoSubdivide\n";

  {
    QuadMesh out;
    std::string error;
    const PolyMesh triangle = singleFace({at(0, 0), at(6, 0), at(3, 6)});
    check(orthoSubdivide(triangle, out, &error), "a triangle subdivides: " + error);
    check(out.faces() == 3, "a triangle gives three quads");
    check(quadMeshFault(out).empty(), "the three quads are sound: " + quadMeshFault(out));
    check(std::abs(totalArea(out) - 18.0) < 1e-6, "the three quads tile the triangle");
  }

  {
    QuadMesh out;
    const PolyMesh quad = singleFace({at(0, 0), at(4, 0), at(4, 4), at(0, 4)});
    check(orthoSubdivide(quad, out, nullptr), "a quad subdivides");
    check(out.faces() == 4, "a quad gives four quads");
    check(quadMeshFault(out).empty(), "the four quads are sound");
    check(std::abs(totalArea(out) - 16.0) < 1e-6, "the four quads tile the quad");
  }

  {
    QuadMesh out;
    const PolyMesh pentagon = singleFace({at(0, 0), at(4, 0), at(6, 3), at(2, 6), at(-1, 3)});
    check(orthoSubdivide(pentagon, out, nullptr), "a pentagon subdivides");
    check(out.faces() == 5, "a pentagon gives five quads");
    check(quadMeshFault(out).empty(), "the five quads are sound");
  }

  {
    // Two faces sharing an edge: the children must meet at the shared midpoint
    // rather than at two coincident-but-separate vertices.
    PolyMesh pair;
    pair.vertices = {at(0, 0), at(4, 0), at(4, 4), at(0, 4), at(8, 0), at(8, 4)};
    pair.faces = {{0, 1, 2, 3}, {1, 4, 5, 2}};

    QuadMesh out;
    check(orthoSubdivide(pair, out, nullptr), "two faces subdivide");
    check(out.faces() == 8, "two quads give eight quads");
    check(quadMeshFault(out).empty(), "the shared edge stayed conforming");

    uint32_t boundarySides = 0;

    for (uint32_t s = 0; s < out.sides.size(); ++s)
    {
      if (out.sides[s].twin == kNoSide)
      {
        ++boundarySides;
      }
    }

    check(boundarySides == 12, "eight quads across a shared edge leave twelve boundary sides");
  }
}

void testTopology()
{
  std::cout << "topology\n";

  const QuadMesh mesh = lattice(4, 5);

  uint32_t interior = 0, boundary = 0;

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    if (mesh.sides[s].twin == kNoSide)
    {
      ++boundary;
    }
    else
    {
      ++interior;
      check(mesh.sides[mesh.sides[s].twin].twin == s, "twin is involutive");
    }
  }

  check(boundary == 2 * (4 + 5), "a 4x5 grid has eighteen boundary sides");
  check(interior % 2 == 0, "interior sides come in pairs");

  // V - E + F = 1 for a disc, counting bounded faces only.
  const uint32_t edges = interior / 2 + boundary;
  check(uint32_t(mesh.vertices.size()) + mesh.faces() - edges == 1, "Euler characteristic of a disc is one");

  const FaceAdjacency adjacency = faceAdjacency(mesh);
  bool symmetric = true;

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
    {
      const uint32_t other = adjacency.neighbour[k];
      bool back = false;

      for (uint32_t m = adjacency.offset[other]; m < adjacency.offset[other + 1]; ++m)
      {
        back = back || adjacency.neighbour[m] == f;
      }

      symmetric = symmetric && back;
    }
  }

  check(symmetric, "face adjacency is symmetric");

  const std::vector<std::vector<uint32_t>> cycles = boundaryCycles(mesh);
  check(cycles.size() == 1, "a disc has one boundary cycle");
  check(cycles.front().size() == 2 * (4 + 5), "the cycle visits every boundary vertex");
}

void testNonSelfIntersection()
{
  std::cout << "non-self-intersection\n";

  // Feeding every face boundary into the arrangement is the check the per-face
  // orientation test cannot make: a mesh of individually sound quads that
  // overlap each other has the right winding everywhere and the wrong geometry.
  const QuadMesh mesh = lattice(3, 3);

  std::vector<InputRing> rings;

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    Point c[4];
    mesh.corners(f, c);
    rings.push_back({{c[0], c[1], c[2], c[3]}, f, false});
  }

  Arrangement arrangement;
  std::string error;
  check(buildArrangement(rings, {}, arrangement, &error), "the arrangement builds: " + error);
  check(arrangementFault(arrangement).empty(), "the arrangement is sound");
  check(arrangement.faces.size() == mesh.faces() + 1, "nine quads plus the unbounded face arrange into ten faces");

  QuadMesh overlapping = mesh;
  overlapping.sides[0].origin = overlapping.sides[4 * 8].origin;
  check(!quadMeshFault(overlapping).empty(), "a face dragged across its neighbours is caught");
}

void testWeldAndDrop()
{
  std::cout << "weld and dropFaces\n";

  QuadMesh split;
  split.vertices = {at(0, 0), at(1, 0), at(1, 1), at(0, 1), at(1, 0), at(2, 0), at(2, 1), at(1, 1)};
  split.sides = {{0, kNoSide}, {1, kNoSide}, {2, kNoSide}, {3, kNoSide}, {4, kNoSide}, {5, kNoSide}, {6, kNoSide}, {7, kNoSide}};
  split.vertexSide.assign(8, kNoSide);

  weld(split);

  check(split.vertices.size() == 6, "welding two quads at a shared edge leaves six vertices");
  check(quadMeshFault(split).empty(), "the welded mesh is sound");
  check(split.sides[1].twin != kNoSide, "the shared edge found its twin");

  QuadMesh mesh = lattice(3, 3);
  std::vector<bool> keep(mesh.faces(), true);
  keep[4] = false; // The centre, which leaves a hole.

  dropFaces(mesh, keep);

  check(mesh.faces() == 8, "dropping the centre leaves eight faces");
  check(quadMeshFault(mesh).empty(), "the mesh with a hole is sound");
  check(boundaryCycles(mesh).size() == 2, "a hole is a second boundary cycle");

  const std::vector<std::vector<uint32_t>> cycles = boundaryCycles(mesh);
  check(cycles.front().size() == 12 && cycles.back().size() == 4, "the outer cycle comes first");
}

void testRefinement()
{
  std::cout << "refineFaces\n";

  {
    QuadMesh mesh = lattice(2, 2, 4.0);
    const double before = totalArea(mesh);

    std::vector<bool> refine(mesh.faces(), true);
    std::vector<uint32_t> parents;
    std::string error;

    check(refineFaces(mesh, refine, &parents, nullptr, &error), "refining every face: " + error);
    check(mesh.faces() == 16, "four faces refine into sixteen");
    check(quadMeshFault(mesh).empty(), "the uniformly refined mesh is sound: " + quadMeshFault(mesh));
    check(std::abs(totalArea(mesh) - before) < 1e-6, "refinement conserves area");
    check(parents.size() == 16 && parents[0] == 0 && parents[15] == 3, "every child names its parent");
  }

  {
    // One face refined and its neighbour not: the T-vertex the hierarchy is
    // built to tolerate. The coarse side loses its twin, which is how the
    // one-to-two adjacency announces itself.
    QuadMesh mesh = lattice(1, 2, 4.0);
    const double before = totalArea(mesh);

    std::vector<bool> refine(mesh.faces(), false);
    refine[0] = true;

    std::vector<EdgeSplit> remap;
    check(refineFaces(mesh, refine, nullptr, &remap, nullptr), "refining one of two faces");
    check(mesh.faces() == 5, "one refined face and one whole face give five");
    check(quadMeshFault(mesh).empty(), "a mesh with a T-vertex is sound: " + quadMeshFault(mesh));
    check(std::abs(totalArea(mesh) - before) < 1e-6, "partial refinement conserves area");

    uint32_t orphaned = 0;

    for (uint32_t s = 0; s < mesh.sides.size(); ++s)
    {
      if (mesh.sides[s].twin == kNoSide)
      {
        ++orphaned;
      }
    }

    check(orphaned == 12, "only the refined face's internal cross is paired");

    uint32_t whole = 0, split = 0, straddling = 0;

    for (const EdgeSplit &entry : remap)
    {
      whole += entry.count == 1 ? 1 : 0;
      split += entry.count == 2 ? 1 : 0;
      straddling += entry.count == 3 ? 1 : 0;
    }

    check(remap.size() == 7, "two quads sharing an edge have seven undirected edges");
    check(split == 3, "the refined face's three outer edges each give two halves");
    check(straddling == 1, "the shared edge gives two halves and the coarse copy");
    check(whole == 3, "the unrefined face's own edges are carried over whole");
  }
}

void testEdgeIdentity()
{
  std::cout << "edge identity\n";

  const QuadMesh mesh = lattice(3, 3);
  bool consistent = true;

  for (uint32_t s = 0; s < mesh.sides.size(); ++s)
  {
    const uint32_t t = mesh.sides[s].twin;

    if (t == kNoSide)
    {
      consistent = consistent && edgeOf(mesh, s) == s;

      continue;
    }

    consistent = consistent && edgeOf(mesh, s) == edgeOf(mesh, t);
    consistent = consistent && edgeOf(mesh, s) == std::min(s, t);
  }

  check(consistent, "both sides of an edge name the same edge");

  // An attribute array indexed by the canonical edge survives refinement when
  // the remap is applied, which is what lets a wall stay a wall across a level.
  QuadMesh refined = lattice(2, 2, 4.0);
  std::vector<uint32_t> attribute(refined.sides.size(), 0);

  for (uint32_t s = 0; s < refined.sides.size(); ++s)
  {
    attribute[edgeOf(refined, s)] = 7;
  }

  std::vector<EdgeSplit> remap;
  std::vector<bool> refine(refined.faces(), true);
  const QuadMesh before = refined;

  check(refineFaces(refined, refine, nullptr, &remap, nullptr), "refining to test the remap");

  std::vector<uint32_t> carried(refined.sides.size(), 0);
  uint32_t carriedCount = 0;

  for (const EdgeSplit &entry : remap)
  {
    if (attribute[entry.parent] == 0)
    {
      continue;
    }

    for (uint8_t k = 0; k < entry.count; ++k)
    {
      carried[entry.child[k]] = attribute[entry.parent];
      ++carriedCount;
    }
  }

  const uint32_t parentEdges = uint32_t(remap.size());
  check(parentEdges == 12, "a 2x2 grid has twelve undirected edges");
  check(carriedCount == 24, "each of the twelve edges hands its attribute to two children");
}

} // namespace

int main()
{
  std::cout << "Geom2QuadMeshTests\n";

  testFaultPredicate();
  testOrthoSubdivision();
  testTopology();
  testNonSelfIntersection();
  testWeldAndDrop();
  testRefinement();
  testEdgeIdentity();

  if (failures != 0)
  {
    std::cerr << failures << " failed\n";

    return 1;
  }

  std::cout << "all passed\n";

  return 0;
}
