#include "WorldKingdomHistory.hpp"
#include "WorldStoryGraph.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {

HistoryNode kingdomHistoryNode(const size_t index)
{
  // Clear of the scale fixture's 100/200, its origin at 950 and its crown at
  // 900, and of each other.
  return HistoryNode(10000 + 100 * index);
}

namespace {

uint32_t roleRank(const SettlementClass role)
{
  switch (role)
  {
  case SettlementClass::Hamlet: return 0;
  case SettlementClass::Village: return 1;
  case SettlementClass::Town: return 2;
  case SettlementClass::City: return 3;
  case SettlementClass::Capital: return 4;
  }

  return 1;
}

} // namespace

uint32_t kingdomFoundingPopulation(SettlementClass role, uint32_t requested)
{
  const uint32_t lower[] = {24, 100, 500, 1001, 1500};
  const uint32_t upper[] = {99, 499, 1000, 2400, 3200};
  const auto rank = roleRank(role);
  return std::clamp(requested, lower[rank], upper[rank]);
}

void kingdomHistoryRecords(const CityState &state, const HistoryNode node, const HistoricalCityBrief &brief, KingdomHistorySettlement &out)
{
  out.brief = brief;
  out.households = brief.households;
  out.buildings.clear();
  out.enclosures.clear();
  out.manor = out.castle = false;

  for (const auto &b : brief.buildings)
  {
    SettlementHistoricalBuilding record;
    record.id = b.id;
    record.use = cityBuildingUseName(b.use);
    record.builtWeek = b.builtWeek;
    record.floors = b.floors;
    record.residents = b.residents;
    record.trade = b.trade;
    record.architecturalAgeYears = brief.week>=b.builtWeek?(brief.week-b.builtWeek)/52:0;
    out.manor = out.manor || b.use == CityBuildingUse::Manor;
    out.castle = out.castle || b.use == CityBuildingUse::Keep;
    out.buildings.push_back(std::move(record));
  }

  const auto site = state.fabric.sites().find(node);
  const auto defence = state.defence.sites().find(node);

  if (site == state.fabric.sites().end() || defence == state.defence.sites().end())
  {
    return;
  }

  // Funded, dated enclosures from the ledger: which standing buildings each
  // one encloses, so the composer can rebuild the circuit round them and
  // not round the city's age.
  for (const auto &c : defence->second.circuits)
  {
    SettlementHistoricalEnclosure enclosure;
    enclosure.id = c.id;
    enclosure.era = c.era;
    enclosure.palisade = c.palisade;
    enclosure.abandoned = c.abandoned;
    enclosure.absorbed = c.lifecycle == CityWallLifecycle::Absorbed;
    enclosure.closed = c.closed() && c.closedWeek && *c.closedWeek <= brief.week;
    enclosure.closedWeek = c.closedWeek ? *c.closedWeek : 0;

    for (const auto &[id, building] : site->second.buildings)
    {
      const auto parcel = site->second.parcels.find(building.parcel);

      if (building.active(brief.week) && parcel != site->second.parcels.end() && c.enclosedBlocks.count(parcel->second.block))
      {
        enclosure.buildings.push_back(id);
      }
    }

    out.enclosures.push_back(std::move(enclosure));
  }
}

bool runKingdomHistory(const KingdomPlan &plan, const KingdomHistorySettings &settings, KingdomHistoryResult &result, std::unique_ptr<CityChronicle> *keep)
{
  result = KingdomHistoryResult{};

  if (plan.settlements.empty())
  {
    result.error = "a kingdom needs at least one settlement";

    return false;
  }

  if (settings.years == 0 || settings.years > CityScaleMaximumYears)
  {
    result.error = "kingdom years must be 1..2000";

    return false;
  }

  CityScaleSettings scale;
  scale.mode = CityScaleMode::City;
  scale.seed = settings.seed;
  scale.years = settings.years;
  scale.keepStream = settings.keepStream;
  scale.maximumLiving = settings.maximumLiving;
  scale.enrichedHistory = settings.enrichedHistory;
  scale.establishedSettlements = settings.establishedSettlements;
  scale.cancelled = settings.cancelled;
  scale.yearlyObserver=[&](uint32_t year,const CityState &state,std::string &error){
    if(settings.cancelled && settings.cancelled()){error="world history cancelled";return false;}
    if (settings.progress) settings.progress(year);
    if (year % 10 == 0 || year == settings.years) {
      KingdomHistoryMilestone milestone; milestone.elapsedYears = year;
      for (const auto &[node, site] : state.fabric.sites()) milestone.populations[node] = 0;
      for (const auto &[id, person] : state.previous->people)
        if (person.alive && person.settlement && milestone.populations.count(*person.settlement))
          ++milestone.populations[*person.settlement];
      result.milestones.push_back(std::move(milestone));
    }
    return true;
  };

  std::map<uint64_t, HistoryNode> nodes;
  uint32_t best = 0;

  for (size_t i = 0; i < plan.settlements.size(); ++i)
  {
    const auto &settled = plan.settlements[i];
    CityScaleSettlement one;
    one.node = kingdomHistoryNode(i);
    one.role = roleRank(settled.role);
    one.households = std::max(3u, uint32_t(std::lround(double(settled.population) * settings.foundingShare / settings.personsPerHousehold)));
    if (settings.establishedSettlements) {
      one.population = kingdomFoundingPopulation(settled.role, settled.population);
      one.households = (one.population + 3) / 4;
      result.foundingPopulations[one.node] = one.population;
      const uint32_t ancient = 450 + uint32_t((settings.seed * 17ull + settled.id * 31) % 300);
      const std::string name = std::string(settlementClassName(settled.role)) + " " + std::to_string(settled.id);
      const char *origins[] = {"A refuge beside an old road became a farming community.",
        "Families settled around a spring traditionally protected by a legendary founder.",
        "Survivors of an earlier kingdom reclaimed abandoned fields and masonry."};
      result.legendaryFacts.push_back({settled.id, ancient, "origin", name + ": " + origins[(settings.seed + settled.id) % 3], true});
      result.legendaryFacts.push_back({settled.id, ancient + 80, "land-charter",
        name + ": inherited household plots and farm rights underpin the founding census and farming workplaces.", true});
      result.legendaryFacts.push_back({settled.id, 980, "provisioning",
        name + (one.role >= 2 ? ": a market charter supports craft workshops, a watch and communal granaries." :
          ": farming households share stores and support local crafts and road maintenance."), true});
    }
    // The base the fabric builds on: wide and high for a capital, a few
    // terraces for a hamlet.
    const uint32_t widths[] = {60, 84, 120, 180, 240}, tiers[] = {2, 3, 4, 6, 8};
    one.baseWidth = widths[one.role];
    one.tiers = tiers[one.role];
    scale.settlements.push_back(one);
    nodes[settled.id] = one.node;

    // The seat: the capital, or failing one the most populous site.
    const uint32_t rank = one.role * 1000000 + settled.population;

    if (!scale.seat || rank > best)
    {
      best = rank;
      scale.seat = one.node;
    }
  }

  for (const auto &link : plan.links)
  {
    const auto a = nodes.find(link.from), b = nodes.find(link.to);

    if (a != nodes.end() && b != nodes.end() && a->second != b->second)
    {
      scale.routes.push_back({a->second, b->second});
    }
  }

  std::unique_ptr<CityChronicle> chronicle;

  if (!runCityScale(scale, result.report, &chronicle))
  {
    result.error = result.report.error;

    return false;
  }

  const CityState &state = chronicle->state();

  for (size_t i = 0; i < plan.settlements.size(); ++i)
  {
    KingdomHistorySettlement one;
    one.settlement = plan.settlements[i].id;
    one.node = kingdomHistoryNode(i);
    one.role = plan.settlements[i].role;
    std::string error;

    if (!historicalCityBrief(state, one.node, one.brief, &error))
    {
      result.error = "settlement " + std::to_string(one.settlement) + ": " + error;

      return false;
    }

    kingdomHistoryRecords(state, one.node, one.brief, one);
    result.settlements.push_back(std::move(one));
  }

  for (const auto &e : state.social.events())
  {
    result.arrivals += e.kind == CityHistoryEventKind::Arrival ? 1 : 0;
    result.departures += e.kind == CityHistoryEventKind::Departure ? 1 : 0;
  }

  if (keep != nullptr)
  {
    *keep = std::move(chronicle);
  }

  return true;
}

bool populateWorldHistory(GeneratedWorld &world,const WorldPipelineSettings &settings,const std::function<bool()> &cancelled,std::string *error,const std::function<void(uint32_t)> &progress) {
  world.civilization.reset();
  if(!settings.historyYears)return true;
  auto result=std::make_shared<WorldCivilization>();
  WorldSiteSelectionSettings sites;sites.population.seed=settings.brief.seed;
  KingdomSettings kingdom;kingdom.towns=settings.historyTowns;kingdom.villages=settings.historyVillages;
  kingdom.hamlets=settings.historyHamlets;kingdom.roadVillages=settings.historyRoadVillages;
  if(!selectKingdomSites(world,sites,kingdom,result->kingdom,error))return false;
  if(cancelled && cancelled()){if(error)*error="world history cancelled";return false;}
  KingdomHistorySettings history;history.seed=uint32_t(settings.brief.seed);history.years=settings.historyYears;
  history.establishedSettlements=true;
  history.maximumLiving=settings.historyMaximumLiving;history.cancelled=cancelled;history.progress=progress;
  if(!generateWorldStory(result->kingdom,history,result->history,result->story,error))return false;
  world.civilization=std::move(result);return true;
}
void applyWorldSettlementHistory(const GeneratedWorld &world,WorldSiteSelectionSettings &settings) {
  if(!world.civilization || settings.historicalHouseholds)return;
  const auto &c=*world.civilization;
  for(size_t i=0;i<c.kingdom.settlements.size();++i){const auto &site=c.kingdom.settlements[i];
    if(settings.preferredCell!=kInvalidNode?site.cell!=settings.preferredCell:site.role!=SettlementClass::Capital)continue;
    const auto &h=c.history.settlements.at(i);
    settings.preferredCell=site.cell;settings.historicalHouseholds=h.households;settings.historicalBuildings=h.buildings;
    settings.historicalEnclosures=h.enclosures;settings.historicalManor=h.manor;settings.historicalCastle=h.castle;
    settings.population.minimum=settings.population.maximum=h.brief.demand.population;return;
  }
}

} // namespace procedural::world::rewrite
