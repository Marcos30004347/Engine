#pragma once

#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/hierarchy/Generator.hpp"

namespace procedural::world::hierarchy {

// A cell-planned dwelling on each lot.
//
// A BINDING, not a replacement. `HouseFootprints` and the planners behind it
// keep their own id and their own role, and the policy picks which generator
// serves a region - so a town can have cell-planned dwellings in one district
// and polygon-subdivided ones in the next, at the same time, and the two can be
// compared on one render.
//
// It lives in its own file rather than in `Generators.cpp` for the same reason:
// adding this changes nothing that was already there.
class CellDwellings final : public RegionPopulator
{
public:
  struct Settings
  {
    generator::CellDwellingSettings planner{};

    // How far the building stands back from its lot boundary before the grid
    // is laid. The lot is the ground; this is the part of it the house may
    // stand on.
    double setbackMetres = 1.0;

    // How the household stands on its plot, which decides where in the lot the
    // building goes - on the street line, or back behind its ground.
    generator::PlotStanding standing = generator::PlotStanding::Modest;

    generator::BuildingClass building = generator::BuildingClass::Dwelling;

    std::vector<std::string> tags;

    // A lot smaller than this is not offered a building.
    double minimumAreaMetres = 24;
  };

  CellDwellings() = default;
  explicit CellDwellings(Settings settings) : settings_(std::move(settings))
  {
  }

  Descriptor descriptor() const override;

  bool populate(const Request &, std::vector<Object> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// The plan into faces. See the definition for why it stands alone.
void buildCellDwellingSolid(const world::rewrite::DwellingPlan &, Solid &);

} // namespace procedural::world::hierarchy
