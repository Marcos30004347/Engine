#pragma once

#include "procedural/world/WorldGenerationContracts.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world::generator {

using PatchId = uint64_t;

using rewrite::Position;

// A convex counter-clockwise ring in the region's local frame.
using ConvexRing = std::vector<Position>;

// Explicit upstream cover label. Never inferred from colour or shape.
enum class PatchCover : uint8_t
{
  Open,
  Forest,
  Rock,
  Water,
  Urban
};

// Measured upstream. Every value is in [0, 1] except elevation (metres).
struct PatchAttributes
{
  double elevationMetres = 0;
  double slope = 0;
  double floodRisk = 0;
  double fertility = 0;
  double defence = 0;
  bool buildable = true;
  bool navigableShore = false;
};

// Shared-edge midpoint to a sibling region; the planning-access portal.
struct PatchPortal
{
  PatchId neighbour = 0;
  Position position;
};

// World-frame origin of a region's local coordinates. Child generators keep
// absolute geometric tolerances, so every region works near its own origin.
struct PatchFrame
{
  Position origin;

  Position toLocal(Position p) const
  {
    return {p.x - origin.x, p.y, p.z - origin.z};
  }

  Position toWorld(Position p) const
  {
    return {p.x + origin.x, p.y, p.z + origin.z};
  }
};

// The one geometric hand-off between a parent generator and its children.
// Parents emit a welded tiling of these; a child receives exactly one and
// must not read outside it.
struct PatchRegion
{
  PatchId id = 0;
  PatchId parent = 0;
  PatchFrame frame;
  ConvexRing boundary;
  PatchCover cover = PatchCover::Open;
  PatchAttributes attributes;
  std::vector<PatchPortal> portals;
};

} // namespace procedural::world::generator
