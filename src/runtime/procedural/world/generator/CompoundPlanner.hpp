#pragma once

#include "Ledger.hpp"
#include "PatchRegion.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"

#include <functional>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A2 compound structures inside a reserved precinct: castle (keep, curtain
// wall, corner towers, gatehouse, bailey, hall and store ranges), manor (hall
// and service wings around a court behind a garden wall), church (nave,
// tower, low precinct wall) and civic hall. Every inhabited wing is a
// rectangular dwelling brief planned by the dwelling planner, so rooms,
// doors and stairs come from the same rules as houses.
enum class CompoundKind : uint8_t
{
  Castle,
  Manor,
  Church,
  Hall
};

struct CompoundWall
{
  Position a, b;
  double thickness = 1.5, height = 6;
  bool gate = false; // Opening segment.
};

struct CompoundTower
{
  ConvexRing footprint;
  double height = 9;
};

struct CompoundWing
{
  std::string role; // keep, hall, stable, store, chapel, nave, tower, service...
  rewrite::DwellingBrief brief;
  rewrite::DwellingPlan plan;
  bool planned = false;
  std::string note;
};

struct CompoundPlan
{
  uint64_t id = 0;
  CompoundKind kind = CompoundKind::Manor;
  ConvexRing precinct;  // Local frame of the caller.
  ConvexRing enclosure; // Wall line (castle/manor/church) as a ring.
  std::vector<CompoundWall> walls;
  std::vector<CompoundTower> towers;
  std::vector<CompoundWing> wings;
  std::vector<ConvexRing> courts;
  Position gate;
  uint32_t plannedWings = 0;
  bool solved = false;
};

struct CompoundBrief
{
  uint64_t id = 0, seed = 1;
  CompoundKind kind = CompoundKind::Manor;
  ConvexRing precinct;   // Convex, counter-clockwise, caller's local frame.
  Position approach;     // Point on or near the precinct edge the street reaches.
  double foundation = 0; // Terrace level for the wings.
};

bool planCompound(const CompoundBrief &, const std::function<double(double, double)> &terrain, CompoundPlan &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
