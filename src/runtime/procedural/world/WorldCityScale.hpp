#pragma once

// Phase 9: the scale matrix, as one reusable harness for the benchmark tool
// and the tests. A mode is a society, a site and a founding purse; a run is
// the driver and the chronicle stepping together, the ledgers' prospects
// pulling the society's newcomers.

#include "WorldCityChronicle.hpp"
#include "WorldCitySociety.hpp"
#include <functional>

namespace procedural::world::rewrite {

enum class CityScaleMode
{
  Village, // Stable: a few houses, no immigration to speak of.
  Town,    // Declining: high mortality, emigration, no newcomers.
  City     // Growing: strong immigration onto a wide, tall base.
};

inline constexpr uint32_t CityScaleMaximumYears = 2000;

// One settlement of an explicit kingdom: founded with this many households,
// on a base sized by its role, linked to the others by the plan's roads.
struct CityScaleSettlement
{
  HistoryNode node = 0;
  uint32_t households = 6;
  uint32_t role = 1;        // SettlementClass as an integer: 0 hamlet, 1 village, 2 town, 3 city, 4 capital.
  uint32_t baseWidth = 84;  // Terrace width of the base the fabric builds on.
  uint32_t tiers = 3;
  uint32_t population = 0; // Optional exact founding census; zero keeps the household fixture.
};

struct CityScaleSettings
{
  CityScaleMode mode = CityScaleMode::Village;
  // When given, these replace the mode's fixed settlements: a whole kingdom
  // simulated as one history, with migration between its settlements and
  // one treasury at the seat.
  std::vector<CityScaleSettlement> settlements;
  std::vector<std::pair<HistoryNode, HistoryNode>> routes; // Commuting and trade, either way.
  HistoryNode seat = 0;                                    // The capital; its first house holds the crown.
  uint32_t seed = 1;
  uint32_t years = 25;
  bool keepStream = true;
  bool establishedSettlements = false; // Provision an inhabited world, rather than a small founding fixture.
  bool enrichedHistory = false; // Seeded harvest arcs and a wider founding economy.
  // Read-only milestone hook; false stops the run with its diagnostic.
  std::function<bool(uint32_t elapsedYears, const CityState &, std::string &)> yearlyObserver;
  std::function<bool()> cancelled; // Checked before founding and each simulated week.
  uint32_t maximumLiving = 2000; // The published limit of the scale matrix; a guard, never a death.
};

struct CityScaleReport
{
  uint32_t weeks = 0, living = 0, households = 0, homes = 0, buildings = 0, wallsStanding = 0, freeParcels = 0;
  uint32_t startingLiving = 0, livingEverywhere = 0; // `living` counts the residents of simulated settlements.
  std::string stage;
  std::map<std::string, size_t> events;     // Per ledger.
  size_t eventBytes = 0, streamBytes = 0;   // Estimates from record sizes.
  std::array<double, 6> ledgerSeconds{};    // Social, households, economy, realm, fabric, defence.
  double totalSeconds = 0, slowestWeekSeconds = 0;
  uint64_t hash = 0;
  CitySocietyDriver::Counts society;
  int64_t treasury = 0, foodStock = 0;
  uint32_t hungryHouseholdMonths = 0, openProposals = 0, waitingHouseholds = 0, employed = 0, unemployed = 0;
  int64_t storeCoin = 0, householdCoin = 0, unpaidWages = 0;
  bool coinConserved = false, foodBalanced = false;
  std::map<std::string, uint32_t> blocked; // Reason prefix -> open proposals.
  std::map<std::string, uint32_t> crownProjects; // "kind state" -> count.
  std::map<HistoryNode, std::string> settlements; // A one-line summary each.
  int64_t storeTimber = 0, storeStone = 0;
  std::string error;
};

CitySocietySettings cityScaleSociety(const CityScaleSettings &);
CityPolicies cityScalePolicies(const CityScaleSettings &);
CityFoundingScript cityScaleScript(const CityScaleSettings &, const CityHistorySnapshot &founding);

// Runs the whole matrix cell. `chronicle`, if given, receives the finished
// chronicle for further inspection.
bool runCityScale(const CityScaleSettings &, CityScaleReport &, std::unique_ptr<CityChronicle> *chronicle = nullptr);

const char *cityScaleModeName(CityScaleMode);

} // namespace procedural::world::rewrite
