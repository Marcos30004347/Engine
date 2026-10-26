#pragma once

// Phase 3 of the living city history: livelihoods and the local economy.
//
// The economic ledger owns jobs, workplaces, stocks, coin, household budgets
// and the deficits that follow when they do not balance. It reads people from
// the validated social snapshot and homes from the household ledger, and
// writes neither. Occupation, social class, political office and household
// wealth are recorded separately; none is inferred from another.
//
// Units are integers of authored goods (food, timber, stone, tools, coin),
// so nothing is ever spent twice or driven negative: every transfer is a
// bounded move between two named stocks. All coefficients are tuning data.

#include "WorldCityHouseholds.hpp"
#include <array>
#include <memory>

namespace procedural::world::rewrite {

enum class CityOccupation
{
  Farmer,
  Labourer,
  Carpenter,
  Mason,
  Smith,
  Merchant,
  Innkeeper,
  Guard,
  Cleric,
  Administrator,
  Domestic,
  Baker, Brewer, Weaver, Potter, Herbalist, Scribe
};

constexpr uint32_t cityOccupationCount = 17;

// The dwelling programme's word for the room an occupation works in: a
// smith's house has a forge, a farmer's a byre. Empty for an occupation
// whose building is its own programme (a merchant's shop, an innkeeper's
// inn) or that has no room of its own.
inline const char *cityTradeOf(const CityOccupation occupation)
{
  switch (occupation)
  {
  case CityOccupation::Baker: return "bakery";
  case CityOccupation::Brewer: return "brewery";
  case CityOccupation::Weaver: return "weaving";
  case CityOccupation::Potter: return "pottery";
  case CityOccupation::Herbalist: return "herbalist";
  case CityOccupation::Scribe: return "scriptorium";
  case CityOccupation::Smith: return "smithy";
  case CityOccupation::Carpenter: return "carpentry";
  case CityOccupation::Mason: return "masonry";
  case CityOccupation::Farmer: return "farmstead";
  case CityOccupation::Guard: return "guardhouse";
  case CityOccupation::Administrator: return "hall";
  default: return "";
  }
}

enum class CityLivelihood
{
  Employed,
  SelfEmployed,
  Caregiver,
  Unemployed,
  UnableToWork,
  Retired
};

enum class CityDependency
{
  Dependent,
  Apprentice
};

enum class CitySocialClass
{
  Peasant,
  Craft,
  Burgher,
  Clergy,
  Noble
};

// A working life: taught, then journeying, then a master who teaches; and
// the plan to keep a shop of one's own.
enum class CityCareerStage
{
  None,
  Apprentice,
  Journeyman,
  Master
};

enum class CityGood
{
  Food,
  Timber,
  Stone,
  Tools,
  Coin
};

constexpr uint32_t cityGoodCount = 5;

const char *cityOccupationName(CityOccupation);
const char *cityLivelihoodName(CityLivelihood);
const char *cityDependencyName(CityDependency);
const char *citySocialClassName(CitySocialClass);
const char *cityGoodName(CityGood);
const char *cityCareerStageName(CityCareerStage);

struct CityStock
{
  std::array<int64_t, cityGoodCount> units{};

  int64_t &operator[](CityGood good)
  {
    return units[uint32_t(good)];
  }

  int64_t operator[](CityGood good) const
  {
    return units[uint32_t(good)];
  }

  bool covers(const CityStock &cost) const;
  CityStock &operator+=(const CityStock &);
  CityStock &operator-=(const CityStock &);
  bool operator==(const CityStock &other) const
  {
    return units == other.units;
  }
};

struct CityOccupationProfile
{
  uint32_t skillRequired = 0;
  int64_t wagePerMonth = 0;              // Coin, paid by the workplace owner.
  CityStock inputsPerWorker, outputsPerWorker; // Per month; a partial run needs inputs for every worker it runs.
  bool publicService = false;            // No goods; paid from the treasury.
  bool needsLand = false;                // Runs only where the workplace's land is accessible.
  bool needsTrade = false;               // Earns only where the settlement's trade is open.
  bool clergyOnly = false;               // Eligible only with the social role the story gives.
};

struct CityEconomyPolicy
{
  std::array<CityOccupationProfile, cityOccupationCount> occupations;
  std::array<int64_t, cityGoodCount> prices{1, 1, 3, 5, 1}; // Coin per unit at the settlement store.
  int64_t foodPerPersonPerMonth = 4;
  int64_t subsistenceFoodPerPersonPerMonth = 2; // The store's free ration when a household cannot buy.
  int64_t upkeepPerHouseholdPerMonth = 1;
  int64_t upkeepPerPersonPerMonth = 1; // Cloth, candles, ale: what a person buys at the market every month.
  // Savings above the reserve are spent at the market at this rate: what a
  // prosperous house buys beyond bread, and how coin comes back round.
  uint32_t householdSpendingPerMille = 100;
  int64_t householdCoinReserve = 40;
  uint32_t taxPerMille = 200;
  uint32_t wealthTaxPerMille = 20;    // Monthly, on coin above the household reserve.
  uint32_t monthWeeks = 4;
  uint32_t reserveMonths = 3;         // Food a household or store keeps before selling or exporting.
  int64_t householdGoodsReserve = 10; // Timber, stone and tools a household keeps before selling.
  CityStock exportReserve;            // Building stock the store keeps back from trade (see the constructor).
  CityStock importFloor;              // Below this the merchants buy the good in, coin permitting (see the constructor).
  uint32_t apprenticesPerMentor = 1;
  uint32_t mentorSkill = 2;
  uint32_t apprenticeshipYears = 3;   // Fewer and the youth graduates untrained.
  uint32_t masterYears = 5;           // As a journeyman, with the skill, before the town calls him master.
  uint32_t apprenticeHalfWork = 2;    // An apprentice counts for one worker in this many.
  uint32_t maxSkill = 5;
  uint32_t retirementAge = 65;
  int64_t tradeUnitsPerMerchant = 20; // Exported per month where trade is open.
  // The market is the crown's: what its store earns above a working float goes
  // to the treasury each month as dues. The public production trades (a
  // labourer's yard, the smithy) are the market's too, and it pays them.
  uint32_t marketDuesPerMille = 500;
  int64_t storeFloatCoin = 100;
  // One public treasury per realm: a settlement's taxes and dues go to its
  // seat, and the crown's works anywhere in the realm are paid from there.
  std::map<HistoryNode, HistoryNode> treasurySeat;
  std::set<std::pair<HistoryNode, HistoryNode>> routes; // Commuting between two settlements, either way.
  std::set<HistoryNode> tradeOpen;
  std::map<uint32_t, uint32_t> harvestPerMille; // Story year index -> farm yield; absent years are 1000.
  uint32_t seed = 1;                            // Founding and migrant skills.

  CityEconomyPolicy();
};

struct CityWorkplaceSpec
{
  HistoryNode settlement = 0;
  CityOccupation occupation = CityOccupation::Farmer;
  uint32_t slots = 1;
  std::optional<HistoryNode> owner; // A person; unset is public, run from the treasury.
  bool landAccessible = true;
};

struct CityWorkplace
{
  uint64_t id = 0;
  HistoryNode settlement = 0;
  CityOccupation occupation = CityOccupation::Farmer;
  uint32_t slots = 1;
  std::optional<HistoryNode> owner;
  bool landAccessible = true;
  uint32_t openedWeek = 0;
  std::optional<uint32_t> retiredWeek;
  std::vector<HistoryNode> workers;                // In hiring order; the last hired leaves first.
  std::map<HistoryNode, HistoryNode> apprentices;  // Apprentice -> mentor among the workers.
};

struct CityLivelihoodRecord
{
  bool adult = false;
  CityLivelihood livelihood = CityLivelihood::Unemployed;
  CityDependency dependency = CityDependency::Dependent;
  CitySocialClass socialClass = CitySocialClass::Peasant;
  std::optional<CityOccupation> occupation;
  std::optional<uint64_t> workplace;
  std::optional<HistoryNode> mentor;
  std::array<uint8_t, cityOccupationCount> skills{};
  bool unable = false; // Set from outside; the story has no injury yet.

  // The career: the trade learned, from whom, and where it is going.
  CityCareerStage career = CityCareerStage::None;
  std::optional<CityOccupation> trade;
  std::vector<HistoryNode> mentors; // In order; the first is the master who took them on.
  bool taughtByParent = false;
  std::optional<uint32_t> apprenticedWeek, graduatedWeek, masteredWeek, openedWeek;
  std::optional<CityOccupation> ambition; // A shop of this trade, when the savings allow.
  std::optional<uint64_t> quitFrom;       // The shop that could not pay, and
  std::optional<uint32_t> quitWeek;       // when: not tried again for a season.
};

struct CityHouseholdAccount
{
  CityStock stock;
  int64_t incomeThisMonth = 0;
  int64_t unpaidWages = 0;   // Cumulative coin owed and never paid.
  int64_t foodShortfall = 0; // Cumulative units under the full ration.
  uint32_t hungryMonths = 0; // Months under even the subsistence ration.
};

struct CitySettlementEconomy
{
  CityStock store;        // The market: goods and coin.
  int64_t treasury = 0;   // Public coin: taxes in, public wages and projects out.
  int64_t unpaidPublicWages = 0;
  int64_t foodShortfall = 0;
  int64_t exportedCoin = 0, importedCoin = 0; // Coin that entered or left with trade.
};

// Running totals for the food and coin gates.
struct CityEconomyTotals
{
  int64_t foodProduced = 0, foodConsumed = 0, foodExported = 0, foodImported = 0;
  int64_t coinGranted = 0, coinFromTrade = 0, coinToTrade = 0, coinSpentOutside = 0;
};

enum class CityEconomyEventKind
{
  WorkplaceOpened,
  WorkplaceRetired,
  Hired,
  SelfEmployed,
  Released,
  Apprenticed,
  Graduated,
  Retired,
  Caregiving,
  Unable,
  ClassChanged,
  Produced,
  Wages,
  UnpaidWages,
  Bought,
  Sold,
  Subsistence,
  Hunger,
  Tax,
  Upkeep,
  Exported,
  Imported,
  OwnerChanged,
  Mastered,   // A journeyman recognised as master of the trade.
  Ambition,   // A journeyman resolves to keep a shop of his own.
  Opened      // The shop opens under its founder.
};

struct CityEconomyEvent
{
  uint32_t week = 0;
  CityEconomyEventKind kind = CityEconomyEventKind::Hired;
  std::optional<HistoryNode> person, settlement;
  HouseholdId household = 0;
  uint64_t workplace = 0;
  CityGood good = CityGood::Coin;
  int64_t amount = 0;
};

const char *cityEconomyEventName(CityEconomyEventKind);

struct CityEconomyFacts
{
  uint32_t workingAge = 0, employed = 0, unemployed = 0, freeSlots = 0, apprentices = 0, guards = 0, merchants = 0, administrators = 0;
  int64_t foodStock = 0, foodNeedPerMonth = 0, storeCoin = 0, treasury = 0, householdCoin = 0;
  double work = 0, food = 0; // Prospects in [0, 1] for migration.
};

class CityEconomyLedger
{
public:
  explicit CityEconomyLedger(CityEconomyPolicy policy = {});

  const CityEconomyPolicy &policy() const
  {
    return rules;
  }

  // Authored stocks: the founding granary, treasury and purses.
  void grantStore(HistoryNode settlement, CityGood, int64_t units);
  void grantTreasury(HistoryNode settlement, int64_t coin);
  void grantHousehold(HouseholdId, CityGood, int64_t units);

  uint64_t openWorkplace(const CityWorkplaceSpec &, uint32_t week);
  bool retireWorkplace(uint64_t id, uint32_t week);
  bool transferOwnership(uint64_t id, std::optional<HistoryNode> owner, uint32_t week);
  void markUnableToWork(HistoryNode person, bool unable);
  void grantSkill(HistoryNode person, CityOccupation, uint32_t level); // Authored mastery.

  // Move a whole household account into another (inheritance, escheat).
  bool transferAccount(HouseholdId from, std::optional<HouseholdId> to, HistoryNode settlementForEscheat, uint32_t week);

  // Spend from a treasury or a household for something outside the ledger
  // (a project escrow). Refused, untouched, when the stock does not cover it.
  bool spendTreasury(HistoryNode settlement, const CityStock &cost);
  HistoryNode treasuryOf(HistoryNode settlement) const;
  bool spendHousehold(HouseholdId, const CityStock &cost);
  void refundTreasury(HistoryNode settlement, const CityStock &);
  void refundHousehold(HouseholdId, const CityStock &);
  // Coin an escrow pays out to a builder's household: it re-enters the ledger.
  void payFromEscrow(HouseholdId, int64_t coin, uint32_t week);

  // A household buys goods from its settlement's store at the store's prices.
  // Refused, untouched, when the store lacks the goods or the purse the coin.
  bool buyFromStore(HouseholdId, HistoryNode settlement, const CityStock &goods, uint32_t week);

  // Workers of these trades employed at a settlement: the construction crew.
  std::vector<HistoryNode> workersAt(HistoryNode settlement, std::initializer_list<CityOccupation>) const;

  // Weekly: allocation follows the social and household ledgers of the same
  // week; the monthly budget runs on every `monthWeeks`-th week after the first.
  bool observe(const CityHistorySnapshot &, const CityHouseholdLedger &, std::string *error = nullptr);

  bool validate(const CityHistorySnapshot &, const CityHouseholdLedger &, std::string *error = nullptr) const;

  CityEconomyFacts facts(HistoryNode settlement, const CityHouseholdLedger &) const;

  const std::map<uint64_t, CityWorkplace> &workplaces() const
  {
    return places;
  }

  const std::map<HistoryNode, CityLivelihoodRecord> &livelihoods() const
  {
    return records;
  }

  const std::map<HouseholdId, CityHouseholdAccount> &accounts() const
  {
    return purses;
  }

  const std::map<HistoryNode, CitySettlementEconomy> &settlements() const
  {
    return towns;
  }

  const CityEconomyTotals &totals() const
  {
    return sums;
  }

  const std::vector<CityEconomyEvent> &events() const
  {
    return *log;
  }

  size_t eventMark() const
  {
    return log->size();
  }

  void truncateEvents(size_t mark)
  {
    log->resize(std::min(mark, log->size()));
  }

  std::optional<uint32_t> week() const
  {
    return lastWeek;
  }

  int64_t coinInLedger() const; // Every purse, store and treasury.

  // A settlement the economy runs: one that was granted a store or a
  // treasury, or has a workplace. A household anywhere else is abroad: it
  // neither eats from a granary nor pays a crown here.
  bool simulated(HistoryNode settlement) const
  {
    return homeSettlements.count(settlement) != 0;
  }

private:
  class Week;

  CityEconomyPolicy rules;
  std::map<uint64_t, CityWorkplace> places;
  std::map<HistoryNode, CityLivelihoodRecord> records;
  std::map<HouseholdId, CityHouseholdAccount> purses;
  std::map<HistoryNode, CitySettlementEconomy> towns;
  std::set<HistoryNode> homeSettlements;
  CityEconomyTotals sums;
  std::shared_ptr<std::vector<CityEconomyEvent>> log = std::make_shared<std::vector<CityEconomyEvent>>();
  std::optional<uint32_t> firstWeek, lastWeek;
  uint64_t nextWorkplace = 1;

  bool reachable(HistoryNode from, HistoryNode to) const;
};

constexpr uint64_t workplaceIdTag = uint64_t(0x57) << 56; // 'W'

} // namespace procedural::world::rewrite
