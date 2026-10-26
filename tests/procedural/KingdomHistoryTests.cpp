// A kingdom is one history.
//
// Five settlements founded in one week on a chain of roads and run through
// one chronicle for ten years. What is asked of the result: every
// settlement comes out with a dated brief that reconciles, people moved
// between the settlements, the capital holds the seat and the most people,
// the buildings carry use words and trades the composer reads, and the
// same plan and seed give the same history twice.
#include "procedural/world/WorldKingdomHistory.hpp"
#include "procedural/world/WorldStoryGraph.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <map>
#include <string>

using namespace procedural::world;
using namespace procedural::world::rewrite;

namespace {

int failures = 0;

void check(const bool ok, const std::string &what)
{
  std::cout << (ok ? "  PASSED " : "  FAILED ") << what << "\n";
  failures += ok ? 0 : 1;
}

KingdomPlan smallKingdom()
{
  KingdomPlan plan;
  const struct { uint64_t id; SettlementClass role; uint32_t population; double x; } sites[] = {
    {1, SettlementClass::Capital, 2500, 0}, {2, SettlementClass::Town, 700, 2000}, {3, SettlementClass::Village, 150, 3000},
    {4, SettlementClass::Village, 150, -900}, {5, SettlementClass::Hamlet, 40, 3500}};
  for (const auto &s : sites)
  {
    KingdomSettlement one;
    one.id = s.id;
    one.role = s.role;
    one.population = s.population;
    one.centre = {s.x, 0, 0};
    plan.settlements.push_back(one);
  }
  plan.links = {{1, 2, {}, {}, true}, {2, 3, {}, {}, false}, {1, 4, {}, {}, false}, {3, 5, {}, {}, false}};
  return plan;
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 1 && std::string(argv[1]) == "--centuries") {
    auto plan = smallKingdom();
    KingdomHistorySettings config; config.years = 200; config.maximumLiving = 5000;
    KingdomHistoryResult history;std::shared_ptr<const WorldStoryGraph> graph;std::string error;
    const bool ran = generateWorldStory(plan, config, history, graph, &error);
    if (!ran) std::cerr << error << '\n';
    check(ran && graph->year == 1200, "two centuries written directly into the story graph");
    bool valid=ran, ancestry=false, causes=false, assumed=false;
    uint32_t living=0;
    if(ran) {
      for(uint32_t i=0;i<graph->edges.partitionCount();++i) {std::string problem;valid &= graph->edges.partitionAt(i).checkInvariants(problem);}
      for(const auto &[id,p]:graph->people)if(!p.diedYear){++living;valid &= graph->buildings.count(p.home)&&graph->households.count(p.household);}
      for(const auto &e:graph->stories){causes|=e.cause!=0;assumed|=e.assumed&&e.year<1000;}
      graphrewrite::compiler::EdgeTypeId type;graph->types.find("parent of",type);ancestry=graph->edges.edgeCount(type)>0;
      for(const auto &page:graph->edges.partition(type,graphrewrite::runtime::KeyOrder::ByOrigin).allPages())
        for(const auto &edge:page)valid &= graph->people.at(edge.origin).bornYear+18<=graph->people.at(edge.target).bornYear;
    }
    check(valid&&living==history.report.living&&living<=5000,"graph partitions, current residents and construction references reconcile");
    check(std::all_of(history.settlements.begin(),history.settlements.end(),[](const auto &site){return site.brief.demand.population>0;}),"population ceiling does not starve later settlements of new generations");
    check(ancestry&&causes&&assumed,"graph retains ancestry, legendary premises and causal recovery stories");
    KingdomHistoryResult again;std::shared_ptr<const WorldStoryGraph> repeated;
    check(generateWorldStory(plan,config,again,repeated,&error)&&again.report.living==history.report.living&&repeated->stories.size()==graph->stories.size(),"seeded annual history is repeatable");
    check(history.milestones.empty()&&history.report.streamBytes==0&&history.report.eventBytes==0,"world story creates no snapshots or ledger records");
    std::cout << "200 years: " << history.report.totalSeconds << " seconds, " << history.report.living << " residents, " << history.report.buildings << " buildings\n";
    return failures ? 1 : 0;
  }
  check(kingdomFoundingPopulation(SettlementClass::Village, 900) == 499 &&
        kingdomFoundingPopulation(SettlementClass::City, 300) == 1001, "role-based population bounds");
  CityScaleSettings census; census.establishedSettlements = true; census.enrichedHistory = true;
  census.settlements = {{10000, 1, 1, 84, 3, 499}, {10100, 1, 3, 180, 6, 1001}};
  const auto societyConfig = cityScaleSociety(census);
  CitySocietyDriver founders(societyConfig);
  std::map<HistoryNode, uint32_t> counts;
  for (const auto &[id, person] : founders.current().people) if (person.settlement) ++counts[*person.settlement];
  check(counts[10000] == 499 && counts[10100] == 1001, "odd-sized founding censuses are exact");
  const auto script = cityScaleScript(census, founders.current());
  uint32_t farms = 0; for (const auto &w : script.workplaces) farms += w.spec.occupation == CityOccupation::Farmer;
  check(farms > 0 && script.workplaces.size() >= founders.current().houseHeads.size(), "households start with farms or workshops and public services");
  std::set<CityOccupation> trades; int64_t startingFood = 0;
  for (const auto &w : script.workplaces) trades.insert(w.spec.occupation);
  for (const auto &grant : script.grants) if (grant.good == CityGood::Food) startingFood += grant.amount;
  check(trades.count(CityOccupation::Carpenter) && trades.count(CityOccupation::Smith) &&
        trades.count(CityOccupation::Baker) && trades.count(CityOccupation::Weaver) &&
        trades.count(CityOccupation::Merchant) && trades.count(CityOccupation::Herbalist), "founding crafts include tools, construction, food, textiles and services");
  check(startingFood == 1500 * 4 * 6, "starting food covers six months of census demand");
  auto invalid = smallKingdom(); KingdomHistorySettings tooSmall; tooSmall.establishedSettlements = true; tooSmall.maximumLiving = 100;
  KingdomHistoryResult refused;
  check(!runKingdomHistory(invalid, tooSmall, refused) && refused.error.find("maximumLiving") != std::string::npos, "population budget fails explicitly before simulation");
  auto cancelPlan = smallKingdom(); cancelPlan.settlements = {cancelPlan.settlements.back()}; cancelPlan.links.clear();
  KingdomHistorySettings cancelled; cancelled.establishedSettlements = true;
  uint32_t cancellationChecks = 0;
  cancelled.cancelled = [&] { return ++cancellationChecks == 3; };
  KingdomHistoryResult interrupted;
  check(!runKingdomHistory(cancelPlan, cancelled, interrupted) && interrupted.error == "world history cancelled" && cancellationChecks == 3,
        "long runs can cancel during the first simulated year");
  std::cout << "the kingdom as one history\n";

  KingdomHistorySettings settings;
  settings.seed = 41;
  settings.years = 10;
  KingdomHistoryResult result;
  const auto started = std::chrono::steady_clock::now();
  const bool ran = runKingdomHistory(smallKingdom(), settings, result);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  if (!ran)
  {
    std::cerr << "  " << result.error << "\n";
  }

  std::cout << "  " << settings.years << " years in " << seconds << " s: living " << result.report.living << ", households " << result.report.households
            << ", buildings " << result.report.buildings << ", arrivals " << result.arrivals << ", departures " << result.departures << "\n";
  check(ran && result.settlements.size() == 5, "five settlements run as one history");

  uint32_t capitalPeople = 0, most = 0;
  bool reconciled = true, useWords = true, anyTrade = false, anyFarm = true;
  const std::set<std::string> uses = {"home", "shop", "inn", "workshop", "farm", "market", "church", "keep", "manor"};

  for (const auto &one : result.settlements)
  {
    std::cout << "    " << settlementClassName(one.role) << " " << one.settlement << ": " << one.brief.demand.population << " people, " << one.households.size()
              << " households, " << one.buildings.size() << " buildings\n";
    reconciled = reconciled && one.brief.demand.population == one.brief.citizens.size() && one.brief.households.size() == one.households.size();
    uint32_t farms = 0;

    for (const auto &b : one.buildings)
    {
      useWords = useWords && uses.count(b.use) != 0;
      anyTrade = anyTrade || !b.trade.empty();
      farms += b.use == "farm" ? 1 : 0;
    }

    // Farming is a workplace before it is a building: the founding farmers
    // work the land from the first week, and farm buildings follow as
    // projects. So the question is whether anyone farms, not whether a
    // farm stands.
    bool farmers = false;
    for (const auto &c : one.brief.citizens)
    {
      farmers = farmers || (c.occupation && *c.occupation == CityOccupation::Farmer);
    }
    anyFarm = anyFarm && (farmers || farms > 0);
    most = std::max(most, one.brief.demand.population);

    if (one.role == SettlementClass::Capital)
    {
      capitalPeople = one.brief.demand.population;
    }
  }

  check(reconciled, "every settlement's brief reconciles its people and households");
  check(capitalPeople > 0 && capitalPeople == most, "the capital holds the most people");
  check(result.arrivals > 0, "people moved between the settlements");
  check(useWords, "every building carries a use word the composer reads");
  check(anyTrade, "some buildings carry a trade word");
  check(anyFarm, "every settlement farms");
  check(seconds < 120., "ten years of a five-settlement kingdom run inside two minutes");

  KingdomHistoryResult again;
  check(runKingdomHistory(smallKingdom(), settings, again) && again.report.hash == result.report.hash, "the same plan and seed give the same history");

  {
    procedural::world::WorldPipelineSettings config;config.brief.seed=17;config.brief.extentMetres=12000;config.brief.reliefMetres=900;
    config.brief.tags={procedural::world::Tag::Coast,procedural::world::Tag::Lowland};config.cellSpacingMetres=400;
    config.historyYears=2;config.historyTowns=0;config.historyVillages=1;config.historyHamlets=1;config.historyRoadVillages=0;
    procedural::world::GeneratedWorld world;std::string error;
    const bool generated=procedural::world::generateWorldDirected(config,world,{},&error);
    if(!generated)std::cerr<<error<<'\n';
    check(generated&&world.civilization&&world.civilization->story,"runtime world pipeline retains one authoritative story graph");
    bool bound=generated&&bool(world.civilization),ages=bound;
    if(bound)for(size_t i=0;i<world.civilization->kingdom.settlements.size();++i) {
      WorldSiteSelectionSettings site;site.preferredCell=world.civilization->kingdom.settlements[i].cell;
      applyWorldSettlementHistory(world,site);const auto &record=world.civilization->history.settlements[i];
      bound &= site.historicalHouseholds.has_value()&&site.historicalBuildings.has_value()&&site.population.maximum==record.brief.demand.population;
      for(const auto &b:record.buildings)ages &= b.architecturalAgeYears==(record.brief.week-b.builtWeek)/52;
      site.historicalHouseholds=std::vector<SettlementHousehold>{};applyWorldSettlementHistory(world,site);bound &= site.historicalHouseholds->empty();
    }
    check(bound,"every world settlement binds its own people/buildings and preserves explicit snapshots");
    check(ages,"world construction dates propagate to architectural ages");
    bool legends = false;
    if(generated)for(const auto &fact:world.civilization->story->stories)legends |= fact.assumed&&fact.year<1000;
    check(legends, "world legendary premises are labeled and precede the simulated era");
    uint32_t censusTotal = 0;
    if (generated) for (const auto &[node, count] : world.civilization->history.foundingPopulations) censusTotal += count;
    check(generated && censusTotal == world.civilization->history.report.startingLiving && censusTotal >= 1500,
          "world census uses established populations instead of tiny founding fixtures");
  }

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");
  return failures == 0 ? 0 : 1;
}
