#pragma once
#include "WorldCityChronicle.hpp"
#include "WorldCityDemand.hpp"
#include <stdexcept>

namespace procedural::world::rewrite {
// Disposable, geometry-free contract. IDs belong to the historical settlement,
// not to a rendered mesh. Missing rooms/locations stay explicitly unresolved.
struct HistoricalBuildingBrief
{
  uint64_t id = 0;
  CityBuildingUse use = CityBuildingUse::Home;
  uint32_t builtWeek = 0, floors = 1, residents = 0, workers = 0;
  std::optional<HouseholdId> household;
  std::optional<uint64_t> workplace;
  // The programme word for the trade worked here, from the workplace's
  // occupation: smithy, bakehouse, carpentry, masonry, farmstead,
  // guardhouse, hall. Empty when the building has no workplace, or its
  // occupation has no room of its own (a merchant's shop is a shop).
  std::string trade;
};

inline const char *historicalTradeOf(const CityOccupation occupation)
{
  return cityTradeOf(occupation);
}

struct HistoricalCitizenBrief
{
  HistoryNode id = 0;
  HouseholdId household = 0;
  std::optional<uint64_t> home, workplace, workBuilding;
  std::optional<CityOccupation> occupation;
  bool apprentice = false;
  std::string duty;
};

struct HistoricalCityBrief
{
  HistoryNode settlement = 0;
  uint32_t week = 0, waitingHouseholds = 0;
  CityPopulationDemand demand;
  std::vector<SettlementHousehold> households;
  std::vector<HistoricalBuildingBrief> buildings;
  std::vector<HistoricalCitizenBrief> citizens;
};

inline bool historicalCityBrief(const CityState &state, HistoryNode settlement, HistoricalCityBrief &output, std::string *error = nullptr)
{
  const auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!state.previous)
  {
    return fail("history has no dated snapshot");
  }
  HistoricalCityBrief out;
  out.settlement = settlement;
  out.week = state.previous->week;
  std::map<HistoryNode, HouseholdId> membership;
  std::map<HouseholdId, uint64_t> homes;
  std::map<uint64_t, uint64_t> workplaces;
  std::vector<SettlementHousehold> households;
  for (const auto &[id, h] : state.homes.households())
  {
    if (h.dissolvedWeek || h.settlement != settlement)
    {
      continue;
    }
    SettlementHousehold demand;
    demand.id = id;
    for (auto person : h.members)
    {
      auto p = state.previous->people.find(person);
      if (p == state.previous->people.end() || !p->second.alive || p->second.settlement != settlement)
      {
        return fail("household contains a nonresident");
      }
      if (!membership.emplace(person, id).second)
      {
        return fail("citizen belongs to multiple households");
      }
      ++demand.members;
      auto job = state.economy.livelihoods().find(person);
      if (job != state.economy.livelihoods().end() && job->second.adult && job->second.workplace)
      {
        ++demand.workers;
      }
    }
    if (demand.members)
    {
      households.push_back(demand);
    }
    out.waitingHouseholds += h.housing == CityHousingState::Waiting;
  }
  if (!deriveCityPopulationDemand(households, {}, out.demand, error))
  {
    return false;
  }
  out.households = households;
  auto site = state.fabric.sites().find(settlement);
  if (site != state.fabric.sites().end())
  {
    for (const auto &[id, b] : site->second.buildings)
    {
      auto v = b.active(out.week);
      if (!v)
      {
        continue;
      }
      HistoricalBuildingBrief brief;
      brief.id = id;
      brief.use = b.use;
      brief.builtWeek = b.versions.front().fromWeek;
      brief.floors = v->floors;
      brief.household = b.tenant;
      brief.workplace = b.workplace;
      if (b.tenant)
      {
        auto h = state.homes.households().find(*b.tenant);
        if (h == state.homes.households().end() || !homes.emplace(*b.tenant, id).second)
        {
          return fail("invalid historical tenancy");
        }
        brief.residents = uint32_t(h->second.members.size());
      }
      if (b.workplace)
      {
        auto w = state.economy.workplaces().find(*b.workplace);
        if (w == state.economy.workplaces().end() || w->second.retiredWeek || !workplaces.emplace(*b.workplace, id).second)
        {
          return fail("invalid historical workplace building");
        }
        brief.workers = uint32_t(w->second.workers.size() + w->second.apprentices.size());
        brief.trade = historicalTradeOf(w->second.occupation);
      }
      out.buildings.push_back(brief);
    }
  }
  for (const auto &[id, p] : state.previous->people)
  {
    if (!p.alive || p.settlement != settlement)
    {
      continue;
    }
    auto h = membership.find(id);
    if (h == membership.end())
    {
      return fail("resident has no household");
    }
    HistoricalCitizenBrief citizen;
    citizen.id = id;
    citizen.household = h->second;
    if (homes.count(h->second))
    {
      citizen.home = homes.at(h->second);
    }
    auto job = state.economy.livelihoods().find(id);
    if (job != state.economy.livelihoods().end())
    {
      const auto &j = job->second;
      citizen.occupation = j.occupation;
      citizen.workplace = j.workplace;
      citizen.apprentice = !j.adult && j.dependency == CityDependency::Apprentice;
      if (j.workplace && workplaces.count(*j.workplace))
      {
        citizen.workBuilding = workplaces.at(*j.workplace);
      }
      if (j.workplace && j.occupation == CityOccupation::Guard)
      {
        citizen.duty = "soldier/watch";
      }
      else if (j.workplace && j.occupation == CityOccupation::Administrator)
      {
        citizen.duty = "steward/clerk";
      }
      else if (j.workplace && j.occupation == CityOccupation::Domestic)
      {
        citizen.duty = "household-service";
      }
    }
    out.citizens.push_back(citizen);
  }
  if (out.citizens.size() != out.demand.population)
  {
    return fail("population demand does not reconcile");
  }
  output = std::move(out);
  return true;
}

struct HistoricalActivity
{
  HistoryNode person = 0;
  std::string activity;
  std::optional<uint64_t> building;
  // Assigned by the geometry binding layer only, never guessed from a count.
  std::optional<uint32_t> room;
};

inline std::vector<HistoricalActivity> historicalDailyActivities(const HistoricalCityBrief &brief, uint32_t minute)
{
  if (minute >= 1440)
  {
    throw std::invalid_argument("minute must be within a day");
  }
  std::vector<HistoricalActivity> out;
  for (const auto &p : brief.citizens)
  {
    HistoricalActivity a;
    a.person = p.id;
    a.building = p.home;
    if (minute < 360 || minute >= 1320)
    {
      a.activity = "sleep";
    }
    else if (minute >= 480 && minute < 1020 && p.workplace)
    {
      a.activity = p.apprentice ? "apprenticeship" : "work";
      a.building = p.workBuilding;
    }
    else if ((minute >= 420 && minute < 480) || (minute >= 1080 && minute < 1140))
    {
      a.activity = "meal";
    }
    else
    {
      a.activity = "home";
    }
    out.push_back(std::move(a));
  }
  return out;
}
} // namespace procedural::world::rewrite
