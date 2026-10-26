#pragma once

#include "procedural/geom2/Polygon.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geom3 {

using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

// The 3D layer, which is deliberately thin.
//
// Everything this generator builds in three dimensions is a footprint with a
// height: a house, a wall, a terrace, a road surface, a river bed. So the solid
// primitive is a prism over a 2D polygon, and the 2D polygon keeps all the
// geometric machinery - booleans, offsetting, cutting - rather than a second
// set of it existing up here.
//
// There is no straight skeleton. Making one robust is research-grade work
// (split events, simultaneous degenerate events), and a roof can be had from a
// sequence of inward offsets at a fixed slope until that cost is worth paying.

struct Vertex
{
  double x = 0;
  double y = 0;
  double z = 0;
};

struct Mesh
{
  std::vector<Vertex> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;

  // Per triangle, which part of the prism it came from. Kept because a
  // consumer almost always wants to material the walls and the caps
  // differently, and recovering that from a normal is guesswork on a
  // vertical wall.
  enum class Part : uint8_t
  {
    Bottom,
    Top,
    Wall
  };

  std::vector<Part> parts;
};

// A prism over a polygon. Heights are in metres and may be given per outer
// vertex, which is what puts a building on sloping ground without cutting the
// footprint up.
struct Prism
{
  Polygon footprint;
  double baseHeight = 0;
  double topHeight = 1;

  // Optional, one per outer-ring vertex. Empty means flat at baseHeight.
  std::vector<double> baseProfile;

  // Optional, one per outer-ring vertex. Empty means flat at topHeight.
  std::vector<double> topProfile;
};

// Triangulates a polygon with holes into the xz plane at a constant height.
//
// Ear clipping with the exact orientation predicate, holes bridged into the
// outer ring first. Chosen over the constrained Delaunay triangulator because
// the caps of a prism only need to be a valid triangulation, not a
// well-shaped one, and bridging is easier to keep exact than filtering
// Delaunay triangles by containment.
bool triangulate(const Polygon &, std::vector<std::array<uint32_t, 3>> &triangles, std::vector<Point> &vertices, std::string *error = nullptr);

// Walls, a bottom cap and a top cap. Winding is outward-facing throughout.
bool extrude(const Prism &, Mesh &out, std::string *error = nullptr);

// A wall along an open path: the path is offset into a ribbon and extruded.
// The one 3D operation that is not simply a footprint, because the footprint
// has to be derived from a centreline first.
bool thicken(const std::vector<Point> &path, double halfWidthMetres, double baseHeight, double topHeight, Mesh &out, std::string *error = nullptr);

} // namespace procedural::geom3
