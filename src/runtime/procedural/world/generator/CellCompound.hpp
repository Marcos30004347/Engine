#pragma once

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/Ledger.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// A compound: several buildings, a wall around them, and the open ground
// between. What a castle, a manor, a church precinct and a civic hall all are,
// and what none of them can be built as by the dwelling planner alone.
//
// The claim this rests on is the same one P13 rests on and stated the other way
// round. A class changes how ONE building is measured and shaped; it cannot
// make a building that is several buildings. A castle is not a large house with
// a courtyard - it is a curtain wall with ranges leaning on it, towers at the
// corners, a gatehouse over the entrance and a keep standing free in the ward,
// and every one of those is a separate plan with its own storeys and its own
// stair. So the compound lays out the ground and the walls, and then calls the
// dwelling planner once per wing.
//
// Everything here is in the caller's own local frame, in metres.

enum class CellCompoundKind : uint8_t
{
  // A curtain with ranges against it, corner towers, a gatehouse and a keep.
  Castle,

  // A hall range at the head of a court, a service wing down one side, and a
  // garden wall with a gate closing the front.
  Manor,

  // One axial building in a walled churchyard, with a west tower.
  Church,

  // A single civic range behind a forecourt.
  Hall
};

const char *name(CellCompoundKind);

// A run of enclosing wall. A gate segment is a gap in it, not a thinner piece.
struct CellCurtain
{
  geom2::Point a{};
  geom2::Point b{};
  double thicknessMetres = 1.2;
  double heightMetres = 6;
  bool gate = false;
};

// One building of the compound.
//
// `lot` is the ground it was given and `house` is what was planned on it. The
// lot is a rectangle by construction, and that is deliberate: a wing is laid
// along a wall, and a range with a shaved corner is the thing the compound
// exists to avoid.
struct CellWing
{
  std::string role;
  BuildingClass building = BuildingClass::ManorHouse;
  geom2::Polygon lot;

  // Which lot edge faces the court or the approach. The front door opens onto
  // it, which is what stops a range's only door being in the curtain wall.
  std::vector<uint32_t> frontage;

  CellHouse house;
  bool planned = false;
  std::string note;
};

struct CellCompound
{
  uint64_t id = 0;
  CellCompoundKind kind = CellCompoundKind::Manor;

  geom2::Polygon precinct;  // What the caller reserved.
  geom2::Polygon enclosure; // The wall line, as a ring.

  std::vector<CellCurtain> walls;
  std::vector<geom2::Polygon> towers;

  // Open ground: never built on, never roofed. A court with a building
  // standing in it carries that building as a HOLE, so "no roof over a court"
  // stays a question about the polygon rather than about a list of exceptions.
  std::vector<geom2::Polygon> courts;

  std::vector<CellWing> wings;

  geom2::Point gate{};
  bool solved = false;

  uint32_t plannedWings() const
  {
    uint32_t count = 0;

    for (const CellWing &wing : wings)
    {
      count += wing.planned ? 1u : 0u;
    }

    return count;
  }
};

struct CellCompoundBrief
{
  uint64_t id = 0;
  uint64_t seed = 1;
  CellCompoundKind kind = CellCompoundKind::Manor;

  // Counter-clockwise, metres, in the caller's local frame.
  geom2::Polygon precinct;

  // Where the street reaches the precinct. The side nearest it becomes the
  // front, the gate goes in it, and every wing is turned to face inward from
  // there - so a compound is oriented by its approach and not by its bounding
  // box.
  geom2::Point approach{};
};

bool planCellCompound(const CellCompoundBrief &, const CellDwellingSettings &, CellCompound &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
