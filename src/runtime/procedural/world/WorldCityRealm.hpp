#pragma once

// Phase 4 of the living city history: the ruling family, ownership and
// decisions.
//
// The story graph owns the crown, the heir, the consort and the pretenders;
// this ledger reads them and adds what the city needs and the story does not
// say: who ACTS for the realm when the king is a child or the throne is bare
// (a regent, by a configurable policy), who inherits a dead owner's property,
// and which projects the crown and the households intend, can afford, and
// have reserved. Nothing here builds anything: a funded project is an escrow
// and a reserved site, waiting for phase 5 to make it physical.
//
// Rules run in a fixed order over validated facts. Every reservation is an
// atomic check-and-debit against one stock and one site set, so no order of
// firing can spend a treasury twice or book a site twice.

#include "WorldCityEconomy.hpp"
#include <memory>

namespace procedural::world::rewrite {

enum class CityAuthorityKind
{
  Ruler,  // An adult ruler holds the crown.
  Regent, // A minority or a vacancy, with someone acting.
  None    // Nobody can act for the realm; recorded, not papered over.
};

struct CitySuccessionPolicy
{
  uint32_t majorityAge = 18;
  bool regencyByConsort = true; // An adult consort acts first.
  bool regencyByKin = true;     // Then the eldest adult of the ruling house.
  bool electionFallback = true; // Then the head of the largest household at the seat.
};

struct CityRealm
{
  HistoryNode realm = 0;
  std::optional<HistoryNode> ruler, consort, heir, regent;
  std::set<HistoryNode> claimants;
  std::optional<HistoryNode> seat;  // Settlement of the last ruler seen.
  std::optional<HistoryNode> house; // House of the last ruler seen.
  bool vacant = false;              // The story's `wants a king`.
  CityAuthorityKind authority = CityAuthorityKind::None;
  const char *authorityReason = "";

  std::optional<HistoryNode> actor() const
  {
    return authority == CityAuthorityKind::Ruler ? ruler : authority == CityAuthorityKind::Regent ? regent : std::nullopt;
  }
};

enum class CityRealmEventKind
{
  Crowned,
  CrownReleased,
  HeirNamed,
  HeirReleased,
  ConsortTaken,
  ConsortReleased,
  ClaimRaised,
  ClaimDropped,
  RegencyBegun,
  RegencyEnded,
  NoAuthority,
  Inherited,
  Escheated,
  ProjectProposed,
  ProjectFunded,
  ProjectBlocked,
  ProjectCancelled,
  ProjectStarted,
  ProjectSuspended,
  ProjectResumed,
  ProjectCompleted,
  ProjectRetired
};

struct CityRealmEvent
{
  uint32_t week = 0;
  CityRealmEventKind kind = CityRealmEventKind::Crowned;
  std::optional<HistoryNode> realm, settlement, person, before, after;
  uint64_t asset = 0; // A workplace or a project.
  int64_t amount = 0;
  std::string reason;
};

const char *cityRealmEventName(CityRealmEventKind);
const char *cityAuthorityName(CityAuthorityKind);

enum class CityProjectKind
{
  Home,
  Shop,
  Inn,
  Workshop,
  Farm,
  Market,
  Church,
  Keep,
  Wall,
  Road,
  Stair,     // Access to a higher terrace: nothing above is occupied before it.
  Repair,    // A new version of a standing building, same footprint.
  Extension, // An added floor on a standing building.
  Rebuild,   // Demolition and replacement; the tenant is relocated first.
  Manor,     // The ruler's hall, first stage of the seat's defences.
  Palisade   // A fortified manor: the first enclosure, in timber.
};

constexpr uint32_t cityProjectKindCount = 16;

enum class CityProjectState
{
  Proposed,
  Reserved,
  Funded,
  Building,
  Completed,
  Suspended,
  Cancelled,
  Retired
};

enum class CitySponsorKind
{
  Household,
  Treasury
};

const char *cityProjectKindName(CityProjectKind);
const char *cityProjectStateName(CityProjectState);

struct CityProjectIntent
{
  CityProjectKind kind = CityProjectKind::Home;
  HistoryNode settlement = 0;
  CitySponsorKind sponsor = CitySponsorKind::Household;
  HouseholdId household = 0;            // Sponsor household, or zero for the treasury.
  std::optional<HistoryNode> sponsorPerson; // The head, ruler or regent who asked.
  std::string reason;
  uint32_t priority = 0;
  std::optional<std::string> site;      // Exclusive site key within the settlement.
};

struct CityProject
{
  uint64_t id = 0;
  CityProjectIntent intent;
  CityStock cost, escrow;
  CityProjectState state = CityProjectState::Proposed;
  uint32_t proposedWeek = 0;
  std::optional<uint32_t> fundedWeek, startedWeek, completedWeek, closedWeek;
  std::string blockedBy; // Why it is not funded or not started, in words a player can read.
  CityStock spent;       // Escrow drawn for materials and builders.
};

struct CityProjectPolicy
{
  std::array<CityStock, cityProjectKindCount> costs;
  uint32_t reviewWeeks = 4;
  uint32_t marketHouseholds = 6, keepHouseholds = 8, wallHouseholds = 10;
  double keepSafety = 0.7, wallSafety = 0.8;
  // The crown houses its people: with this many households waiting and the
  // treasury holding this many times a home's coin, it commissions one.
  uint32_t crownHousingWaiting = 2, crownHousingReserveHomes = 4;
  uint32_t crownWorksIdle = 6; // Idle adults before the crown opens a farm or a workshop.

  CityProjectPolicy();
};

class CityRealmLedger
{
public:
  CityRealmLedger(CitySuccessionPolicy succession = {}, CityProjectPolicy projects = {});

  // Weekly, after the economy. Inheritance and project escrow move stock in
  // the economy ledger; that is the only thing this ledger writes elsewhere.
  bool observe(const CityHistorySnapshot &, const CityHouseholdLedger &, CityEconomyLedger &, std::string *error = nullptr);

  // A rule outside this ledger (a test, a future graph rule) may propose too.
  // Resolved at the next review with everything else, in priority order.
  uint64_t propose(const CityProjectIntent &, uint32_t week);

  // Construction, driven by the spatial ledger of phase 5. Every transition
  // is checked; an illegal one is refused and changes nothing.
  bool startBuilding(uint64_t id, uint32_t week, std::string *error = nullptr);                  // Funded -> Building
  bool suspendProject(uint64_t id, uint32_t week, const std::string &reason, std::string *error = nullptr); // Building -> Suspended
  bool resumeProject(uint64_t id, uint32_t week, std::string *error = nullptr);                  // Suspended -> Building
  // Moves part of a project's escrow out: materials consumed, coin paid. Refused when the escrow does not cover it.
  bool drawEscrow(uint64_t id, const CityStock &part, std::string *error = nullptr);
  // Building -> Completed; whatever escrow is left goes back to the sponsor.
  bool completeProject(uint64_t id, uint32_t week, CityEconomyLedger &, std::string *error = nullptr);
  // Funded/Building/Suspended -> Cancelled with a reason; the remaining escrow goes back to the sponsor.
  bool rejectProject(uint64_t id, uint32_t week, const std::string &reason, CityEconomyLedger &, std::string *error = nullptr);
  // A completed structure taken down, with its record kept.
  bool retireProject(uint64_t id, uint32_t week, const std::string &reason, std::string *error = nullptr);
  // Why a funded project has not started: the spatial ledger's word to the player.
  void noteProject(uint64_t id, const std::string &note);

  bool validate(const CityHistorySnapshot &, std::string *error = nullptr) const;

  // Feuds among the houses living there, tempered by the watch and by whether
  // anyone can act for the realm. In [0, 1].
  double safety(HistoryNode settlement, const CityHistorySnapshot &, const CityEconomyLedger &, const CityHouseholdLedger &) const;

  const std::map<HistoryNode, CityRealm> &realms() const
  {
    return crowns;
  }

  // The projects still open: proposed, funded, building or suspended. A
  // week scans these; the closed ones live in the archive below, shared by
  // every weekly copy and consulted by id.
  const std::map<uint64_t, CityProject> &projects() const
  {
    return works;
  }

  const std::map<uint64_t, CityProject> &closedProjects() const
  {
    return *archive;
  }

  const CityProject *project(uint64_t id) const
  {
    const auto open = works.find(id);

    if (open != works.end())
    {
      return &open->second;
    }

    const auto closed = archive->find(id);

    return closed == archive->end() ? nullptr : &closed->second;
  }

  const std::vector<CityRealmEvent> &events() const
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

private:
  class Week;

  void refund(CityProject &, CityEconomyLedger &);

  CitySuccessionPolicy succession;
  CityProjectPolicy planning;
  void close(uint64_t id); // Moves a project into the archive.

  std::map<HistoryNode, CityRealm> crowns;
  std::map<uint64_t, CityProject> works;
  std::shared_ptr<std::map<uint64_t, CityProject>> archive = std::make_shared<std::map<uint64_t, CityProject>>();
  // Derived indexes share the archive's lifetime/copy semantics. Closed
  // project history grows for centuries; weekly queries need only these keys.
  std::shared_ptr<std::map<std::pair<CityProjectKind, HistoryNode>, uint32_t>> completedCounts =
      std::make_shared<std::map<std::pair<CityProjectKind, HistoryNode>, uint32_t>>();
  std::shared_ptr<std::map<std::pair<HistoryNode, std::string>, uint32_t>> completedSites =
      std::make_shared<std::map<std::pair<HistoryNode, std::string>, uint32_t>>();

  std::set<std::pair<HistoryNode, std::string>> sites;
  std::set<HistoryNode> living;                              // Last week's living, for deaths.
  std::map<HouseholdId, std::set<HistoryNode>> lastMembers;  // Last week's membership, for extinction.
  std::shared_ptr<std::vector<CityRealmEvent>> log = std::make_shared<std::vector<CityRealmEvent>>();
  std::optional<uint32_t> firstWeek, lastWeek;
  uint64_t nextProject = 1;
};

constexpr uint64_t projectIdTag = uint64_t(0x50) << 56; // 'P'

} // namespace procedural::world::rewrite
