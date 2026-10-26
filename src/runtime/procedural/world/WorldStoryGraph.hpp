#pragma once
#include "WorldCityHistoricalBrief.hpp"
#include <memory>

namespace procedural::world::rewrite {
struct KingdomHistorySettings;
struct KingdomHistoryResult;
struct KingdomPlan;

// One evolving graph. Payloads describe nodes; relationships live in the
// engine's paged edge store. No weekly snapshots or economic ledgers.
struct WorldStoryPerson {
  uint32_t id=0, bornYear=0, diedYear=0, settlement=0, household=0;
  uint32_t home=0, workplace=0;
  CityOccupation occupation=CityOccupation::Farmer;
};
struct WorldStoryBuilding {
  uint32_t id=0, settlement=0, household=0, builtYear=0;
  CityBuildingUse use=CityBuildingUse::Home;
  CityOccupation occupation=CityOccupation::Farmer;
  uint32_t floors=1;
};
struct WorldStoryEvent {
  uint32_t id=0, year=0, settlement=0, subject=0, cause=0;
  std::string kind, narrative;
  bool assumed=false;
};
struct WorldStoryGraph {
  graphrewrite::compiler::EdgeTypeTable types;
  graphrewrite::runtime::EdgePageStore edges;
  std::map<uint32_t,uint64_t> settlements; // Graph node -> world settlement identity.
  std::map<uint32_t,uint32_t> households; // Household node -> settlement node.
  std::map<uint32_t,uint32_t> foodNeeds; // Current monthly demand, no stock accounting.
  std::map<uint32_t,WorldStoryPerson> people;
  std::map<uint32_t,WorldStoryBuilding> buildings;
  std::vector<WorldStoryEvent> stories;
  uint32_t year=1000;
  WorldStoryGraph();
  void connect(const char *relationship,uint32_t from,uint32_t to);
  void disconnect(const char *relationship,uint32_t from,uint32_t to);
};
// Annual demographic and story rules write directly into the graph, then
// project its current people/buildings into the existing geometry contract.
bool generateWorldStory(const KingdomPlan &,const KingdomHistorySettings &,KingdomHistoryResult &,
                        std::shared_ptr<const WorldStoryGraph> &,std::string *);
} // namespace procedural::world::rewrite
