#pragma once

// Phase 8 of the living city history: replay, inspection and the player's
// view of it all.
//
// Every city ledger is a deterministic function of the weekly social
// snapshot stream, the policies it was built with and the founding script.
// So a checkpoint is that stream, those policies and that script; replaying
// them rebuilds the same ledgers, hash for hash, without the story runner.
// A saved chronicle reloads and resumes on the next week of a matching
// story, with identical future hashes. Scrubbing the timeline reads the
// versions and dates the ledgers already keep and mutates nothing.
//
// The story runner itself has no save; a resumed run re-simulates the story
// from its seed, which is deterministic, and feeds the chronicle from there.

#include "WorldCityDefence.hpp"
#include "json.hpp"
#include <array>

namespace procedural::world::rewrite {

struct CityGrant
{
  uint32_t week = 0;
  HistoryNode settlement = 0; // For store and treasury grants.
  HouseholdId household = 0;  // For purse grants; zero otherwise.
  bool treasury = false;
  CityGood good = CityGood::Coin;
  int64_t amount = 0;
};

struct CitySkillGrant
{
  HistoryNode person = 0;
  CityOccupation occupation = CityOccupation::Farmer;
  uint32_t level = 1;
};

struct CityWorkplaceFounding
{
  uint32_t week = 0;
  CityWorkplaceSpec spec;
};

// What the preview or a test does to the ledgers by hand, as data, so a
// replay can do it again in the same order at the same week.
struct CityFoundingScript
{
  std::map<HistoryNode, CityFabricSettings> sites; // Founded at the first week.
  std::vector<CityWorkplaceFounding> workplaces;
  std::vector<CityGrant> grants;
  std::vector<CitySkillGrant> skills;
  std::vector<std::pair<uint32_t, CityProjectIntent>> proposals; // Week, intent.
  int64_t foundingHouseholdCoin = 0; // Every household seeded at the first week starts with this.
  int64_t newcomerHouseholdCoin = 0; // A household arriving from outside brings this.
};

struct CityPolicies
{
  CityHouseholdPolicy households; // Capacities of managed settlements without a site.
  CityEconomyPolicy economy;
  CitySuccessionPolicy succession;
  CityProjectPolicy projects;
  CityDefenceSettings defence;
};

struct CityState
{
  CityHistoryLedger social;
  CityHouseholdLedger homes;
  CityEconomyLedger economy;
  CityRealmLedger realm;
  CityFabricLedger fabric;
  CityDefenceLedger defence;
  std::optional<CityHistorySnapshot> previous;
  uint32_t weeks = 0;
};

class CityChronicle
{
public:
  CityChronicle(CityPolicies, CityFoundingScript);

  // One validated weekly snapshot: recorded, then every ledger advanced in
  // the fixed order. Fails transactionally.
  bool observe(const CityHistorySnapshot &, std::string *error = nullptr);

  const CityState &state() const
  {
    return current;
  }

  const std::vector<CityHistorySnapshot> &stream() const
  {
    return weeks;
  }

  // The checkpoint: policies, script and stream. `load` rebuilds by replay.
  nlohmann::json save() const;
  static bool load(const nlohmann::json &, CityChronicle &out, std::string *error = nullptr);

  // Fresh ledgers fed the recorded stream up to and including `week`
  // (or the whole stream). Equal to the live state, hash for hash.
  bool replay(std::optional<uint32_t> week, CityState &out, std::string *error = nullptr) const;

  static uint64_t hashOf(const CityState &);

  uint64_t stateHash() const
  {
    return hashOf(current);
  }

  // Scrubbing: what stood and who lived at a date. Mutates nothing.
  nlohmann::json at(uint32_t week) const;

  // Inspection.
  nlohmann::json inspectPerson(HistoryNode) const;
  nlohmann::json inspectBuilding(HistoryNode settlement, uint64_t building) const;
  nlohmann::json explainProject(uint64_t project) const;

  // Yearly series per settlement: population, jobs, food, treasury, homes.
  nlohmann::json series() const;

  const CityPolicies &policies() const
  {
    return rules;
  }

  const CityFoundingScript &script() const
  {
    return founding;
  }

  // Off, the stream is not kept: memory stays bounded for a long run, and
  // replay, save and scrubbing by date are unavailable and say so.
  void setStreamRetention(bool keep)
  {
    keepStream = keep;
  }

  // Seconds spent in each ledger so far: social, households, economy,
  // realm, fabric, defence. What a week costs, by part.
  const std::array<double, 6> &timings() const
  {
    return seconds;
  }

  double slowestWeekSeconds() const
  {
    return slowest;
  }

private:
  bool apply(CityState &, const CityHistorySnapshot &, std::string *error) const;

  CityPolicies rules;
  CityFoundingScript founding;
  bool keepStream = true;
  mutable std::array<double, 6> seconds{};
  mutable double slowest = 0;
  std::vector<CityHistorySnapshot> weeks;
  uint32_t firstWeek = 0, recorded = 0;
  std::vector<nlohmann::json> yearly; // Series rows, one per 52 weeks.
  CityState current;
};

nlohmann::json cityPoliciesJson(const CityPolicies &);
bool cityPoliciesFromJson(const nlohmann::json &, CityPolicies &, std::string *error = nullptr);
nlohmann::json cityScriptJson(const CityFoundingScript &);
bool cityScriptFromJson(const nlohmann::json &, CityFoundingScript &, std::string *error = nullptr);
nlohmann::json citySnapshotJson(const CityHistorySnapshot &);
bool citySnapshotFromJson(const nlohmann::json &, CityHistorySnapshot &, std::string *error = nullptr);

} // namespace procedural::world::rewrite
