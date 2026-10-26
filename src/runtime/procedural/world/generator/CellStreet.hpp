#pragma once

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/Ledger.hpp"

#include <string>
#include <vector>

namespace procedural::world::generator {

// A street: houses packed along a frontage, sharing their party walls.
//
// A medieval street is not a row of articulated houses with gaps between them.
// It is narrow rectangles side by side, each one plot wide, gable or eaves to
// the road, and the wall between two of them belongs to both. The dwelling
// planner already does everything a single house needs; what it cannot do is
// stand two houses on ONE lattice so their walls meet exactly, and that is the
// whole of what this adds.
//
// One grid along the street. The frontage is cut into plots measured in whole
// cells, each plot is a rectangle of that grid, and every house is planned on
// its plot with the plot as its footprint - a slum's rule, "the building is
// the lot", which is exactly what a terrace house is. Because every plot is a
// whole number of cells from the street's origin, each house's own lattice
// (framed from its lot) falls on the street's, and a party wall is one line
// for both neighbours.
//
// Everything here is in the caller's own local frame, in metres.
struct CellStreetBrief
{
  uint64_t id = 0;
  uint64_t seed = 1;

  // The frontage, in metres, left to right standing in the road and looking at
  // the houses. The block is on the far side of it.
  geom2::Point a{};
  geom2::Point b{};

  // How deep the block is behind the frontage. A plot may take less.
  double depthMetres = 12;

  // What stands on the plots. One class for the street: a row is a row.
  BuildingClass building = BuildingClass::Dwelling;

  // Plot widths, in cells, drawn between these.
  uint32_t narrowestCells = 2;
  uint32_t widestCells = 5;

  // Cells of open ground between one plot and the next. Nought is a terrace:
  // party walls, one wall per line. One or more is a row of STACKS - separate
  // buildings with a gap a flight can run in, which is what a neighbourhood
  // of galleries needs and a terrace has no room for.
  uint32_t gapCells = 0;

  // How many storeys each plot may carry, at most. Nought leaves it to the
  // class. A stack block asks for three or four.
  uint32_t storeys = 0;
};

// One plot of the street, and the house on it.
struct CellPlot
{
  // Along the frontage, in cells from the street's origin.
  uint32_t fromCell = 0;
  uint32_t widthCells = 2;
  uint32_t depthCells = 4;

  // Rigid terrace the building stands on. Streets leave this at zero;
  // neighbourhoods set it from their terrain before realisation.
  double foundation = 0;

  // The plot's ground, counter-clockwise, edge 0 on the street. Edge 1 is the
  // far side (the next plot), edge 3 the near side (the previous one).
  geom2::Polygon lot;

  CellHouse house;
  bool planned = false;
  std::string note;
};

struct CellStreet
{
  uint64_t id = 0;
  geom2::Point a{};
  geom2::Point b{};
  double cellMetres = 2.2;
  uint32_t cellsAlong = 0;
  std::vector<CellPlot> plots;
  bool solved = false;

  uint32_t plannedPlots() const
  {
    uint32_t count = 0;

    for (const CellPlot &plot : plots)
    {
      count += plot.planned ? 1u : 0u;
    }

    return count;
  }
};

bool planCellStreet(const CellStreetBrief &, const CellDwellingSettings &, CellStreet &, Ledger &, std::string *error = nullptr);

// Every planned house realised, and the party walls built ONCE.
//
// Each house builds its own outer walls, so two neighbours put two walls on
// one line - which is two coincident boxes and a fault a painter's algorithm
// makes visible. The wall between plots k and k+1 is kept in plot k's plan and
// dropped from plot k+1's, along the length the two share. One plan per plot,
// in plot order; an unplanned plot gets an empty plan.
bool realiseCellStreet(const CellStreet &, const CellDwellingSettings &, std::vector<rewrite::DwellingPlan> &, std::string *error = nullptr);

} // namespace procedural::world::generator
