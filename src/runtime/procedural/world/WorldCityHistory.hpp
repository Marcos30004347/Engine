#pragma once

#include "WorldSettlementDirector.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include <map>
#include <optional>
#include <set>

namespace procedural::world::rewrite {

using HistoryNode = uint32_t; // Graph node zero is valid; missing values use optional.
enum class CityLifeStage
{
  Child,
  ApprenticeAge,
  Adult,
  Elder
};
enum class CitySocialRole
{
  Resident,
  Clergy,
  Heir,
  Consort,
  Ruler
};

struct CityHistoryPerson
{
  HistoryNode id = 0;
  uint32_t bornWeek = 0;
  bool alive = false;
  std::optional<HistoryNode> settlement, dynasty, spouse;
  std::vector<HistoryNode> parents;
  CityLifeStage stage = CityLifeStage::Child;
  CitySocialRole role = CitySocialRole::Resident;
};

struct CityHistorySnapshot
{
  uint32_t week = 0;
  std::map<HistoryNode, CityHistoryPerson> people; // Includes dead ancestors.
  std::map<HistoryNode, HistoryNode> rulers;       // Realm -> living ruler.

  // Optional realm and conflict facts, read when the story declares them.
  // Only the living hold or claim anything here.
  std::map<HistoryNode, HistoryNode> heirs, consorts;           // Realm -> person.
  std::map<HistoryNode, std::set<HistoryNode>> claimants;       // Realm -> pretenders.
  std::set<HistoryNode> vacantRealms;                           // `wants a king`.
  std::map<HistoryNode, HistoryNode> houseHeads;                // House -> living head.
  std::map<HistoryNode, HistoryNode> sworn;                     // House -> realm.
  std::set<std::pair<HistoryNode, HistoryNode>> feuds;          // House -> house, as declared.
};

// Read-only boundary to the existing story simulation. Required schema is
// checked; malformed or contradictory graphs leave output unchanged.
bool readCityHistorySnapshot(const graphrewrite::compiler::EdgeTypeTable &, const graphrewrite::runtime::EdgePageStore &, uint32_t week, uint32_t dateBase, CityHistorySnapshot &, std::string *error = nullptr);

enum class CityHistoryEventKind
{
  Arrival,
  Departure,
  Birth,
  Death,
  Move,
  Marriage,
  Separation,
  Succession,
  RoleChange,
  LifeStageChange,
  FeudStarted,
  FeudEnded
};

struct CityHistoryEvent
{
  uint32_t week = 0;
  CityHistoryEventKind kind = CityHistoryEventKind::Arrival;
  HistoryNode subject = 0;
  std::optional<HistoryNode> before, after;
};

const char *cityHistoryEventName(CityHistoryEventKind);

// Observe EVERY story week to retain exact change dates. Initial cast is a
// baseline, not births/immigration. A gap is rejected, not silently interpolated.
// Inputs must be validated projections from readCityHistorySnapshot.
class CityHistoryLedger
{
public:
  bool observe(const CityHistorySnapshot &, std::string *error = nullptr);

  const std::optional<CityHistorySnapshot> &current() const
  {
    return latest;
  }

  const std::vector<CityHistoryEvent> &events() const
  {
    return log;
  }

private:
  std::optional<CityHistorySnapshot> latest;
  std::vector<CityHistoryEvent> log;
};

// Phase 2 gate: for every settlement, this week's residents equal last
// week's plus births, arrivals and moves in, minus deaths, departures and
// moves out. `weekEvents` are the ledger events observed for `after.week`.
bool cityPopulationReconciles(const CityHistorySnapshot &before, const CityHistorySnapshot &after, const std::vector<CityHistoryEvent> &weekEvents, std::string *error = nullptr);

struct CityHistoryDemand
{
  uint32_t residents = 0, children = 0, apprenticeAge = 0, workingAge = 0, elders = 0;
  // Provisional co-resident dynasty groups, NOT tenancies or nuclear families.
  // This is an upper-level demand input until explicit households land in phase 2.
  std::vector<SettlementHousehold> familyCohorts;
};

CityHistoryDemand cityHistoryDemand(const CityHistorySnapshot &, HistoryNode settlement);
// Capacity-sized estimates for the existing demand planner (max eight residents
// per unit). These are not persistent domestic units or occupancy assignments.
std::vector<SettlementHousehold> cityHistoryHousingEstimates(const CityHistoryDemand &);
} // namespace procedural::world::rewrite
