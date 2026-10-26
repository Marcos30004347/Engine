#pragma once

// Phase 2 of the living city history: explicit domestic units.
//
// The story graph owns people, ancestry, marriage, dynasty and offices. This
// ledger owns HOUSEHOLDS: who shares a home, who is responsible for whom, and
// whether that home exists yet. A household is neither a dynasty (an extended
// lineage) nor a tenancy (occupancy of one specific building); it is the unit
// that will later hold a budget and ask for a dwelling.
//
// Every rule here is a deterministic policy over validated weekly snapshots.
// Membership never touches the parent tree: an orphan taken in by kin keeps
// the parents the graph recorded.

#include "WorldCityHistory.hpp"
#include <memory>
#include <set>

namespace procedural::world::rewrite {

using HouseholdId = uint64_t; // Tagged stable id; zero is "no household".

constexpr HouseholdId householdIdTag = uint64_t(0x48) << 56; // 'H'

enum class CityHouseholdOrigin
{
  Founding,       // Authored starting cast.
  LocalFormation, // Formed in place: marriage, independence, orphans.
  Migration,      // Relocated from a named settlement (see `origin`).
  Unknown         // Arrived from nowhere the graph names.
};

enum class CityHousingState
{
  Unmanaged, // The settlement has no housing stock policy; nothing is counted.
  Housed,
  Waiting // On the settlement's waiting list; homeless in the city's terms.
};

struct CityHousehold
{
  HouseholdId id = 0;
  uint32_t foundedWeek = 0;
  std::optional<uint32_t> dissolvedWeek;
  std::optional<HistoryNode> settlement; // Where the unit lives; kept after dissolution for the record.
  std::optional<HistoryNode> origin;     // Settlement it came from, when the graph named one.
  CityHouseholdOrigin kind = CityHouseholdOrigin::Unknown;
  std::optional<HistoryNode> head; // Oldest working-age member; not the dynasty's `is head of`.
  std::set<HistoryNode> members;   // Living people only.
  CityHousingState housing = CityHousingState::Unmanaged;
  std::optional<uint32_t> waitingSince;
  bool unguarded = false;       // Holds children with no adult or elder and no kin took them in.
  bool dependentElders = false; // Elders only: allowed to live alone, reported for later care rules.
};

enum class CityHouseholdEventKind
{
  Founded,
  Joined,
  Left,
  Relocated,
  Dissolved,
  Housed,
  HomeReleased,
  GuardianAssigned,
  Unguarded
};

struct CityHouseholdEvent
{
  uint32_t week = 0;
  CityHouseholdEventKind kind = CityHouseholdEventKind::Founded;
  HouseholdId household = 0;
  std::optional<HistoryNode> person;
  std::optional<HistoryNode> before, after; // Settlement or household, by kind.
  const char *cause = "";
};

const char *cityHouseholdEventName(CityHouseholdEventKind);
const char *cityHouseholdOriginName(CityHouseholdOrigin);
const char *cityHousingStateName(CityHousingState);

struct CityHouseholdPolicy
{
  // Settlements with a managed housing stock: how many households it can
  // house. Any settlement absent here is unmanaged and never has a waiting
  // list. This is a stand-in until phase 5 publishes real dwelling capacity.
  std::map<HistoryNode, uint32_t> housingCapacity;

  // An unmarried adult who is not the head may found a home at this age when
  // the settlement has a free home. Never forced; unset keeps them home.
  std::optional<uint32_t> independenceAgeYears;
};

// Observes consecutive weekly snapshots. The first seeds one household per
// co-resident authored house (a priest with no house is a household of one).
// Updates are transactional: a week that fails validation changes nothing.
class CityHouseholdLedger
{
public:
  explicit CityHouseholdLedger(CityHouseholdPolicy policy = {});

  bool observe(const CityHistorySnapshot &, std::string *error = nullptr);

  const std::map<HouseholdId, CityHousehold> &households() const
  {
    return state.households;
  }

  std::optional<HouseholdId> householdOf(HistoryNode person) const;

  const std::vector<CityHouseholdEvent> &events() const
  {
    return *state.log;
  }

  // The log is shared between a ledger and its weekly working copy, so a
  // week costs its own events and not the whole history. A failed week is
  // cut back to its mark.
  size_t eventMark() const
  {
    return state.log->size();
  }

  void truncateEvents(size_t mark)
  {
    state.log->resize(std::min(mark, state.log->size()));
  }

  const CityHouseholdPolicy &policy() const
  {
    return rules;
  }

  // Phase 5 publishes real dwelling capacity here; it takes effect at the
  // next observed week's housing allocation. Nobody is evicted by a cut:
  // housed households stay housed and the stock simply has no vacancy.
  void setHousingCapacity(HistoryNode settlement, uint32_t homes)
  {
    rules.housingCapacity[settlement] = homes;
  }

  std::optional<uint32_t> week() const
  {
    return state.seeded ? std::optional<uint32_t>(state.week) : std::nullopt;
  }

  // Waiting list of a settlement in service order, and its housed count.
  std::vector<HouseholdId> waitingList(HistoryNode settlement) const;
  uint32_t housedHouseholds(HistoryNode settlement) const;
  uint32_t livingHouseholds(HistoryNode settlement) const;

  // Phase 2 gate, checked against the snapshot the ledger last accepted: every
  // living local person has exactly one live household, in their settlement;
  // no household holds the dead; managed settlements never exceed capacity.
  bool validate(const CityHistorySnapshot &, std::string *error = nullptr) const;

private:
  struct State
  {
    bool seeded = false;
    uint32_t week = 0;
    uint64_t nextSequence = 1;
    std::map<HouseholdId, CityHousehold> households;
    std::map<HistoryNode, HouseholdId> index;
    std::shared_ptr<std::vector<CityHouseholdEvent>> log = std::make_shared<std::vector<CityHouseholdEvent>>();
  };

  class Week;

  CityHouseholdPolicy rules;
  State state;
};

} // namespace procedural::world::rewrite
