#pragma once

#include "procedural/world/hierarchy/Generator.hpp"

#include <string>
#include <vector>

namespace procedural::world::hierarchy {

// An elevation: the built solid seen square-on from one side.
//
// Not the 3D track and not a second model of the building. P10's solid is the
// real geometry; this is that geometry projected onto a vertical plane and
// sorted back to front, which is the cheapest honest check a plan cannot give:
// whether the storeys line up, whether the windows make bays, whether the roof
// meets the wall where the plan says it does. Every face here IS a face of the
// solid, so nothing in an elevation can disagree with the massing except by
// being wrong about the projection - and the projection is four lines.
//
// Direction: which way the viewer looks, in the plan's own frame. `North` is
// looking toward +z from -z (the viewer stands south of the building), and so
// round. `across` runs left to right on the page, `height` up it.
enum class ElevationSide : uint8_t
{
  North,
  East,
  South,
  West
};

const char *name(ElevationSide);

struct ElevationFace
{
  std::string kind; // The solid's group: walls, reveals, joinery, slopes, gables, pillars, ...
  std::vector<std::pair<double, double>> outline; // (across, height), metres.
  double depth = 0; // Distance toward the viewer; larger is nearer.
};

struct Elevation
{
  ElevationSide side = ElevationSide::South;
  std::vector<ElevationFace> faces; // Back to front: draw in order.
  double left = 0, right = 0, bottom = 0, top = 0; // The extent.
};

// The solid from one side. Faces that face AWAY from the viewer are dropped -
// the far wall of a box is not part of its elevation - and the rest are sorted
// by their mean depth, nearest last.
Elevation elevationOf(const Solid &, ElevationSide);

} // namespace procedural::world::hierarchy
