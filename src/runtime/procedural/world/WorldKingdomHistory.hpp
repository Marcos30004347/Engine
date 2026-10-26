#pragma once

#include "WorldCityHistoricalBrief.hpp"
#include "WorldCityScale.hpp"
#include "WorldSettlementDirector.hpp"
#include "WorldSettlementSiteAdapter.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace procedural::world::rewrite {

// The kingdom as one history.
//
// A kingdom plan is a set of sites with roles and the roads between them. A
// kingdom history founds a settlement at every one of them in the same
// week, with households by role and public work by role, and runs the whole
// of it through one chronicle: one social graph, so people marry and move
// between settlements; one economy, so a village's grain reaches the town's
// market; one realm, so the crown at the capital funds a wall at a town.
// What comes out is a dated brief for every settlement, which is what the
// world composer takes in place of a synthetic population.
struct KingdomHistorySettings
{
  uint32_t seed = 41;
  uint32_t years = 200;
  bool establishedSettlements = false; // World pipeline enables role-based founding censuses.
  // Legacy small-foundation mode scales households from the site target.
  // Established worlds instead use exact role-bounded population censuses.
  double foundingShare = .27;
  double personsPerHousehold = 4.2;
  uint32_t maximumLiving = 20000;
  bool keepStream = false;
  bool enrichedHistory = true;
  std::function<bool()> cancelled;
  std::function<void(uint32_t)> progress; // Completed simulated years.
};

struct KingdomHistorySettlement
{
  uint64_t settlement = 0; // The plan's id.
  HistoryNode node = 0;    // The history's node.
  SettlementClass role = SettlementClass::Village;
  HistoricalCityBrief brief;
  std::vector<SettlementHousehold> households;
  std::vector<SettlementHistoricalBuilding> buildings;
  std::vector<SettlementHistoricalEnclosure> enclosures;
  bool manor = false, castle = false;
};

struct KingdomLegendaryFact
{
  uint64_t settlement = 0;
  uint32_t year = 0; // Before the simulated era (year 1000).
  std::string kind, narrative;
  bool assumed = true; // A generated premise, never a simulated event.
};

struct KingdomHistoryMilestone
{
  uint32_t elapsedYears = 0;
  std::map<HistoryNode, uint32_t> populations;
};

// Role limits apply at founding; subsequent growth/migration remains simulated.
uint32_t kingdomFoundingPopulation(SettlementClass, uint32_t requested);

struct KingdomHistoryResult
{
  std::vector<KingdomHistorySettlement> settlements;
  CityScaleReport report;
  std::map<HistoryNode, uint32_t> foundingPopulations;
  std::vector<KingdomLegendaryFact> legendaryFacts;
  std::vector<KingdomHistoryMilestone> milestones;
  uint32_t arrivals = 0, departures = 0; // Moves between the kingdom's settlements.
  std::string error;
};

struct WorldStoryGraph;
// Immutable product carried by the generated world and shared by its views.
struct WorldCivilization {
  KingdomPlan kingdom;
  KingdomHistoryResult history;
  std::shared_ptr<const WorldStoryGraph> story;
};
bool populateWorldHistory(GeneratedWorld &,const WorldPipelineSettings &,const std::function<bool()> &,std::string *,const std::function<void(uint32_t)> &progress = {});
// Explicit snapshot inputs win. An unspecified site selects the capital;
// requested cells only bind when they belong to the historical kingdom.
void applyWorldSettlementHistory(const GeneratedWorld &,WorldSiteSelectionSettings &);

// The history node a plan settlement is simulated under.
HistoryNode kingdomHistoryNode(size_t index);

// Runs the kingdom's history and projects every settlement. The chronicle
// is kept when a pointer is given, for a caller that wants the full state.
bool runKingdomHistory(const KingdomPlan &, const KingdomHistorySettings &, KingdomHistoryResult &, std::unique_ptr<CityChronicle> *chronicle = nullptr);

// The records a settlement's brief hands the world composer: households as
// demand, buildings with their use words and trades, enclosures from the
// defence ledger.
void kingdomHistoryRecords(const CityState &, HistoryNode settlement, const HistoricalCityBrief &, KingdomHistorySettlement &);

} // namespace procedural::world::rewrite
