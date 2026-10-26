// A trade is a room before it is a label.
//
// The history names what a household works at; this is the test that the
// name reaches the ground floor as a forge, an oven room, a byre or a
// guardroom, that the trade word survives the crossing from the historical
// record to the lot development, and that a person's working day can be
// bound to that room and nothing else.
#include "procedural/world/WorldCityHistoricalBrief.hpp"
#include "procedural/world/WorldCityHistoricalPlacement.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::world;
using namespace procedural::world::rewrite;

namespace {

int failures = 0;

void check(const bool ok, const std::string &what)
{
  std::cout << (ok ? "  PASSED " : "  FAILED ") << what << "\n";
  failures += ok ? 0 : 1;
}

// A house of the given trade on a plain lot, through the same planner and
// realiser the city uses.
bool houseOfTrade(const std::string &trade, const uint64_t seed, DwellingPlan &plan, std::string &error)
{
  generator::CellDwellingBrief brief;
  brief.id = 7000 + seed;
  brief.seed = seed;
  brief.building = generator::BuildingClass::Dwelling;
  brief.floors = 2;
  brief.tags.push_back(trade);
  const double w = 11., d = 13.;
  brief.lot.outer = {{0, 0}, {geom2::toUnit(w), 0}, {geom2::toUnit(w), geom2::toUnit(d)}, {0, geom2::toUnit(d)}};
  brief.frontage = {0};
  brief.capacity = generator::CellDwellingBrief::Capacity{w * d * .8, 5, 14., true};
  generator::CellDwellingSettings settings;
  generator::CellHouse house;
  generator::Ledger ledger;
  return generator::planCellDwelling(brief, settings, house, ledger, &error) && generator::realiseCellDwelling(house, settings, plan, &error);
}

bool hasRoom(const DwellingPlan &plan, const RoomKind kind, const bool groundFloor)
{
  for (const auto &room : plan.rooms)
  {
    if (room.kind == kind && (!groundFloor || room.floor == 0))
    {
      return true;
    }
  }
  return false;
}

} // namespace

int main()
{
  std::cout << "trade rooms\n";

  // --- the room programme ---------------------------------------------------
  {
    struct Case { const char *trade; RoomKind kind; const char *name; };
    const Case cases[] = {{"smithy", RoomKind::Forge, "a smith's house has a forge on the ground floor"},
                          {"bakehouse", RoomKind::Bakehouse, "a baker's house has an oven room on the ground floor"},
                          {"carpentry", RoomKind::Workshop, "a carpenter's house has a workshop on the ground floor"},
                          {"farmstead", RoomKind::Byre, "a farmstead has a byre on the ground floor"},
                          {"guardhouse", RoomKind::Guardroom, "a guardhouse has a guardroom on the ground floor"},
                          {"hall", RoomKind::CountingHouse, "a civic hall has a counting house"}};
    for (const auto &c : cases)
    {
      uint32_t planned = 0, withRoom = 0;
      for (uint64_t seed = 1; seed <= 6; ++seed)
      {
        DwellingPlan plan;
        std::string error;
        if (!houseOfTrade(c.trade, seed, plan, error))
        {
          continue;
        }
        ++planned;
        withRoom += hasRoom(plan, c.kind, true) ? 1 : 0;
      }
      std::cout << "  " << c.trade << ": " << withRoom << " of " << planned << " houses carry the room\n";
      check(planned >= 4 && withRoom == planned, c.name);
    }

    // A house with no trade has none of these rooms.
    uint32_t stray = 0;
    for (uint64_t seed = 1; seed <= 4; ++seed)
    {
      DwellingPlan plan;
      std::string error;
      if (!houseOfTrade("stairwell", seed, plan, error))
      {
        continue;
      }
      stray += (hasRoom(plan, RoomKind::Forge, false) || hasRoom(plan, RoomKind::Bakehouse, false) || hasRoom(plan, RoomKind::Byre, false) || hasRoom(plan, RoomKind::Guardroom, false)) ? 1 : 0;
    }
    check(stray == 0, "a house with no trade gets no trade room");
  }

  // --- the historical record crosses with its trade -------------------------
  {
    check(std::string(historicalTradeOf(CityOccupation::Smith)) == "smithy" && std::string(historicalTradeOf(CityOccupation::Farmer)) == "farmstead" &&
          std::string(historicalTradeOf(CityOccupation::Merchant)).empty(),
          "occupations name their programme word, and a merchant's shop is a shop");

    CityCompletionPlan plan;
    plan.revision = 1;
    std::vector<Position> block{{0, 0, 0}, {60, 0, 0}, {60, 0, 40}, {0, 0, 40}};
    plan.skeleton.regions.push_back({1, 1, 0, CityRegionKind::Block, CityLandUse::UrbanBlock, DistrictUse::Residential, block, false, 2400});
    uint64_t next = 10;
    for (double x = 2; x < 56; x += 12)
    {
      std::vector<Position> lot{{x, 0, 2}, {x + 10, 0, 2}, {x + 10, 0, 14}, {x, 0, 14}};
      plan.skeleton.regions.push_back({next, next, 1, CityRegionKind::Lot, CityLandUse::UrbanLot, DistrictUse::Residential, lot, false, 120});
      CityLotDevelopment development;
      development.lot = next;
      development.developed = true;
      plan.lots.push_back(development);
      ++next;
    }
    // A farmstead lot, as a holding's farmhouse site, far from the block.
    {
      std::vector<Position> site{{200, 0, 200}, {212, 0, 200}, {212, 0, 209}, {200, 0, 209}};
      plan.skeleton.regions.push_back({next, next, 0, CityRegionKind::Lot, CityLandUse::Farmstead, DistrictUse::Residential, site, true, 108});
      plan.skeleton.regions.back().lotProgramme = "farmstead";
      CityLotDevelopment development;
      development.lot = next;
      development.developed = true;
      plan.lots.push_back(development);
      ++next;
    }
    std::vector<SettlementHistoricalBuilding> records{{1, "workshop", 1, 1, 3, "smithy"}, {2, "home", 2, 2, 4, ""}, {3, "farm", 3, 1, 5, ""}, {4, "workshop", 4, 1, 2, ""}};
    std::string error;
    const bool placed = allocateHistoricalCityLots(records, plan, &error);
    if (!placed)
    {
      std::cerr << "  " << error << "\n";
    }
    std::map<uint64_t, std::string> trades;
    for (const auto &binding : plan.historicalLots)
    {
      for (const auto &lot : plan.lots)
      {
        if (lot.lot == binding.lot)
        {
          trades[binding.building] = lot.trade;
        }
      }
    }
    for (const auto &binding : plan.historicalLots)
    {
      std::cout << "  building " << binding.building << " -> lot " << binding.lot << " trade '" << trades[binding.building] << "'\n";
    }
    check(placed && trades[1] == "smithy" && trades[2].empty() && trades[3] == "farmstead" && trades[4] == "carpentry",
          "a workshop keeps its smith, a farm is a farmstead, a nameless workshop is a carpenter's, a home has no trade");
    uint64_t farmLot = 0;
    for (const auto &binding : plan.historicalLots)
    {
      if (binding.building == 3) farmLot = binding.lot;
    }
    check(farmLot == next - 1, "the farm takes the holding's farmhouse lot before any urban lot");
  }

  // --- a day bound to rooms --------------------------------------------------
  {
    CityModelBuilding smithy;
    smithy.roomList = {{1, 0, "entrance", "Hall"}, {2, 0, "service", "Forge"}, {3, 1, "sleeping", "Bedroom"}, {4, 0, "service", "Kitchen"}, {5, 0, "living", "LivingRoom"}};
    CityModelBuilding bare;
    bare.roomList = {{9, 0, "store", "Store"}};
    check(bindActivityRoom(smithy, "work") == std::optional<uint64_t>(2) && bindActivityRoom(smithy, "apprenticeship") == std::optional<uint64_t>(2),
          "work in a smith's house is bound to the forge");
    check(bindActivityRoom(smithy, "sleep") == std::optional<uint64_t>(3), "sleep is bound to the bedroom");
    check(bindActivityRoom(smithy, "meal") == std::optional<uint64_t>(4), "a meal is bound to the kitchen");
    check(bindActivityRoom(smithy, "home") == std::optional<uint64_t>(5), "the rest of the day is bound to the living room");
    check(!bindActivityRoom(bare, "sleep") && !bindActivityRoom(bare, "work"), "a building with no room of the kind binds nothing rather than guessing");
  }

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");
  return failures == 0 ? 0 : 1;
}
