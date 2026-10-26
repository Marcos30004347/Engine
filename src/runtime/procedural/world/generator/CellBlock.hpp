#pragma once

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/generator/AccessGeometry.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/Ledger.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A block is planned on one grid before any lot is planned on its own grid.
// Cells claimed by lots are building ground; every remaining cell inside the
// block is public paving. Height regions cut across both, so a street can climb
// between terraces and a lot receives its ground level as exterior context.
enum class BlockCellKind : uint8_t { Outside, Lot, Paving, CoveredPassage };
enum class ApartmentStairStyle : uint8_t { None, ZigZag };

struct CellBlockLevel
{
  geom2::Polygon region;
  double elevation = 0;
};

struct CellBlockLotBrief
{
  uint64_t id = 0;
  geom2::Polygon polygon;
  BuildingClass building = BuildingClass::Dwelling;
  uint32_t storeys = 1;
  ApartmentStairStyle apartmentStair = ApartmentStairStyle::None;
};

struct CellBlockPassage
{
  uint32_t upperLot=UINT32_MAX;
  CoveredPassageRequest request;
};

struct CellBlockBrief
{
  uint64_t id = 0, seed = 1;
  geom2::Polygon block;
  std::vector<CellBlockLotBrief> lots;
  std::vector<CellBlockLevel> levels; // Later entries override earlier ones.
  double cellMetres = 2.;
  double maximumStairSlope = .62;
  std::vector<CellBlockPassage> passages;
};

struct CellBlockCell
{
  int32_t row = 0, column = 0;
  BlockCellKind kind = BlockCellKind::Outside;
  uint32_t lot = UINT32_MAX;
  double elevation = 0;
  std::vector<rewrite::Position> polygon;
  std::vector<uint32_t> neighbours;
  uint32_t upperLot=UINT32_MAX;
  uint64_t passage=0;
};

struct CellBlockStair
{
  uint32_t lowerCell = UINT32_MAX, upperCell = UINT32_MAX;
  rewrite::DwellingStair flight;
};

struct CellBlockBuilding
{
  uint32_t lot = UINT32_MAX;
  double elevation = 0;
  CellDwellingBrief dwelling;
  ApartmentStairStyle stairStyle = ApartmentStairStyle::None;
  access::Model exteriorAccess;
};

struct CellBlockPlan
{
  uint64_t id = 0;
  double cellMetres = 0;
  std::vector<CellBlockCell> cells;
  std::vector<uint32_t> pavedCells;
  std::vector<CellBlockStair> stairs;
  std::vector<CellBlockBuilding> buildings;
  std::vector<std::vector<uint32_t>> pavedGraph;
};

bool planCellBlock(const CellBlockBrief &, CellBlockPlan &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
