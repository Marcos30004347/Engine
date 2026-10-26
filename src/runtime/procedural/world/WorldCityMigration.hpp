#pragma once

// Phase 2 of the living city history: migration.
//
// The story rules never move anyone: a villager lives where the facts put
// them. This adapter is the one writer of that fact domain. It AUTHORS
// incoming households the way story.graphfacts authors the founding cast -
// as new people at a named neighbouring settlement - and then moves them, or
// moves a local household out, by rewriting `lives in`. Once written, the
// story rules own the people: they court, marry, bear children and die by
// the same rules as everyone else.
//
// Decisions are a deterministic policy over the validated ledgers of the
// week just closed, published as next week's facts. Work, food and safety
// are authored placeholders until phases 3 and 4 compute them. The living
// population guard suspends arrivals; it never kills anyone.

#include "WorldCityHouseholds.hpp"
#include "graphrewrite/compiler/EdgeTypeTable.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"

namespace procedural::world::rewrite {

struct CitySettlementProspects
{
  double work = 0, food = 0, safety = 0; // Each in [0, 1].
};

// Weighted attraction in [0, 1].
double cityMigrationAttraction(const CitySettlementProspects &);

struct CityMigrationPolicy
{
  // Named neighbouring settlements. They become graph nodes; nobody is
  // simulated there beyond the story's own rules, but a household that
  // leaves for one keeps its identity and can come back.
  std::vector<std::string> origins;

  // Settlements open to migration, with their prospects.
  std::map<HistoryNode, CitySettlementProspects> destinations;

  uint32_t reviewIntervalWeeks = 52; // Strategic review; arrivals land after it.
  double arrivalThreshold = 0.4;     // Below this attraction nobody comes.
  double emigrationThreshold = 0.15; // Below this the newest household leaves.
  uint32_t householdsPerReview = 2;  // At full attraction, per destination.
  uint32_t waitingTolerance = 1;     // Arrivals continue while fewer than this many wait for homes.
  uint32_t patienceWeeks = 52;       // Waiting longer than this is reason to leave.
  uint32_t maxLivingPeople = 2000;   // Performance guard only.
  uint32_t seed = 1;
};

enum class CityMigrationEventKind
{
  Generated, // A household authored at an origin; it moves next week.
  Arrived,
  Departed,
  Returned,
  Suspended // The population guard held arrivals back.
};

struct CityMigrationEvent
{
  uint32_t week = 0; // The week the facts were published; visible the week after.
  CityMigrationEventKind kind = CityMigrationEventKind::Generated;
  std::optional<HistoryNode> origin, destination;
  HouseholdId household = 0; // Zero for a generated household the ledger has not seen yet.
  std::vector<HistoryNode> people;
};

const char *cityMigrationEventName(CityMigrationEventKind);

class CityMigrationAdapter
{
public:
  CityMigrationAdapter(CityMigrationPolicy, const graphrewrite::compiler::EdgeTypeTable &, graphrewrite::runtime::EdgePageStore &, graphrewrite::runtime::NameTable &, uint32_t dayBase);

  // False when the story schema lacks an edge the adapter writes.
  bool ready(std::string *error = nullptr) const;

  // Called after the week's ledgers are validated. Executes last week's
  // pending moves, then on a review week decides departures, returns and
  // new arrivals and writes them into the graph for the next week.
  bool publish(uint32_t week, const CityHistorySnapshot &, const CityHouseholdLedger &, std::string *error = nullptr);

  // Later phases compute what the policy authored: the economy's work and
  // food, the realm's safety. Takes effect at the next review.
  void setProspects(HistoryNode destination, const CitySettlementProspects &prospects)
  {
    policy.destinations[destination] = prospects;
  }

  const CityMigrationPolicy &currentPolicy() const
  {
    return policy;
  }

  const std::vector<CityMigrationEvent> &events() const
  {
    return log;
  }

  const std::map<HistoryNode, std::string> &origins() const
  {
    return originNames;
  }

  bool isOrigin(HistoryNode) const;

private:
  struct Schema
  {
    graphrewrite::compiler::EdgeTypeId alive, man, woman, born, young, infant, grown, unmarried, unattached, fertile, husband, wife, child, mother, father, house, head, peace, lives, keepsPeace;
  };

  struct Pending
  {
    HistoryNode origin = 0, destination = 0;
    std::vector<HistoryNode> people;
  };

  CityMigrationPolicy policy;
  const graphrewrite::compiler::EdgeTypeTable &types;
  graphrewrite::runtime::EdgePageStore &graph;
  graphrewrite::runtime::NameTable &names;
  uint32_t dayBase;
  Schema schema{};
  std::string schemaError;
  std::vector<HistoryNode> originNodes;
  std::map<HistoryNode, std::string> originNames;
  std::vector<Pending> pending;
  std::vector<CityMigrationEvent> log;
  uint32_t generated = 0;

  bool roomFor(HistoryNode settlement, const CityHouseholdLedger &, uint32_t promised) const;
  bool hasKinAt(const CityHousehold &, HistoryNode settlement, const CityHistorySnapshot &) const;
  std::vector<HistoryNode> move(const std::vector<HistoryNode> &people, HistoryNode from, HistoryNode to, const CityHistorySnapshot &);
  void makePeace(const std::vector<HistoryNode> &people, HistoryNode settlement, const CityHistorySnapshot &);
  HistoryNode spawnPerson(uint32_t week, bool man, uint32_t ageYears, uint32_t weekOffset, HistoryNode origin, HistoryNode house, bool adult, bool single);
  Pending generate(uint32_t week, HistoryNode destination);
};

} // namespace procedural::world::rewrite
