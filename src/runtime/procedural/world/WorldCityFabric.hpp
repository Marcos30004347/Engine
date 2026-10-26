#pragma once

// Phases 5 and 6 of the living city history: the spatial ledger.
//
// It owns parcels, buildings and their versions, routes and their surfaces,
// access structures (the approach, the terrace stairs, decks and bridges)
// and the construction that turns a funded project into one of them. The
// site is a terraced city base planned once at founding: terrain, the
// circulation backbone, stair courts and reserved building envelopes come
// first, and only the founding subset is occupied. Everything else waits for
// a project, a crew and, for a higher terrace, its stair.
//
// Nothing is regenerated year by year. A building keeps its id and footprint
// through every version; a rebuild relocates its tenant before demolition and
// leaves exactly one active version at any date. A completed structure
// publishes capacity only after the spatial checks pass: no overlap, an
// entrance at street level, and a finished stair to its terrace.

#include "WorldCityBase.hpp"
#include "WorldCityRealm.hpp"
#include <memory>

namespace procedural::world::rewrite {

struct CityFabricSettings
{
  CityBaseSettings base;
  double doorstepMetres = .45;        // A door and its street within this of each other.
  double attachMetres = 16;           // How far a parcel looks for the way it fronts.
  double minimumHeadroomMetres = 2.1; // Under a deck bridge.
  uint32_t workUnitsPerBuilderPerMonth = 4;
  uint32_t idleMonthsBeforeSuspension = 1;
  uint32_t repairAfterYears = 15;
  uint32_t rebuildAfterYears = 40;
  uint32_t membersPerFloor = 4;       // Beyond this a household asks for an extension.
  uint32_t footpathTraffic = 4;       // Weekly commuters that wear a footpath into a lane...
  uint32_t footpathWeeks = 8;         // ...after this many busy weeks.
};

enum class CityRouteSurface
{
  Footpath,
  Lane,
  Street
};

struct CityRouteRecord
{
  uint64_t id = 0;
  std::vector<Position> path; // y is the graded level.
  double halfWidth = 0;
  CityRouteSurface surface = CityRouteSurface::Lane;
  uint32_t openedWeek = 0;
  std::optional<uint32_t> closedWeek;
  uint32_t traffic = 0, busyWeeks = 0; // Commuters last week, and weeks at or above the footpath threshold.
  std::optional<uint64_t> project;     // The road project that paved it.
};

enum class CityAccessKind
{
  Approach, // From the hillside up into the first terrace's court.
  Stair,    // Between two terraces.
  Deck,     // A raised block's platform.
  Bridge    // Across a stair court at deck level.
};

enum class CityStructureState
{
  Planned,
  Building,
  Completed,
  Retired
};

struct CityAccessStructure
{
  uint64_t id = 0, block = 0; // The block it gives access to.
  CityAccessKind kind = CityAccessKind::Stair;
  CityStructureState state = CityStructureState::Planned;
  Position from, to;           // Lower and upper ends.
  double lowerLevel = 0, upperLevel = 0;
  uint32_t treads = 0, landings = 0;
  double headroom = 0;         // Bridges.
  std::optional<uint32_t> completedWeek;
  std::optional<uint64_t> project;
};

struct CityParcel
{
  uint64_t id = 0, block = 0;
  std::vector<Position> boundary;
  double level = 0, area = 0;
  Position frontage;                  // Where the parcel meets its way.
  std::optional<uint64_t> frontageRoute;
  std::optional<uint64_t> building;   // The one building standing or under work here.
};

enum class CityBuildingUse
{
  Home,
  Shop,
  Inn,
  Workshop,
  Farm,
  Market,
  Church,
  Keep,
  Manor
};

struct CityBuildingVersion
{
  uint32_t version = 1;
  uint32_t fromWeek = 0;
  std::optional<uint32_t> toWeek; // Exclusive; unset while standing.
  std::vector<Position> footprint;
  double base = 0;
  uint32_t floors = 1;
  const char *change = "built";
  uint64_t project = 0;
  uint64_t geometryHash = 0; // Deterministic over footprint, base, floors and use.
};

struct CityBuilding
{
  uint64_t id = 0, parcel = 0;
  CityBuildingUse use = CityBuildingUse::Home;
  std::vector<CityBuildingVersion> versions;
  bool ruin = false; // Retired and left standing as a trace; never overlaps an active version.
  std::optional<uint32_t> retiredWeek;
  std::optional<HouseholdId> sponsorHousehold;
  std::optional<HistoryNode> sponsorPerson;
  std::set<HouseholdId> builders;      // Households paid from the escrow.
  std::optional<HouseholdId> tenant;   // Homes.
  std::optional<uint64_t> workplace;   // Published economy capacity.
  std::optional<CityOccupation> occupation; // What the workplace works at, kept so a regenerated lot keeps its trade.

  const CityBuildingVersion *active(uint32_t week) const;
  const CityBuildingVersion *standing() const; // Active now, if any.
};

struct CityConstruction
{
  uint64_t project = 0, building = 0;
  CityProjectKind kind = CityProjectKind::Home;
  uint32_t workUnits = 0, workDone = 0, idleMonths = 0, startedWeek = 0;
  std::optional<uint64_t> structure, route; // Stairs and roads.
  std::optional<HouseholdId> relocatedTo;   // A rebuild's tenant, housed elsewhere meanwhile.
};

enum class CityFabricEventKind
{
  Founded,
  TenancyAssigned,
  TenancyReleased,
  Relocated,
  ConstructionStarted,
  ConstructionSuspended,
  ConstructionResumed,
  ConstructionCompleted,
  ConstructionRejected,
  CapacityPublished,
  VersionAdded,
  BuildingRetired,
  RouteUpgraded,
  RouteClosed,
  RouteClosureRefused,
  AccessCompleted,
  Waiting // A funded project with nowhere to go yet, and why.
};

struct CityFabricEvent
{
  uint32_t week = 0;
  CityFabricEventKind kind = CityFabricEventKind::Founded;
  HistoryNode settlement = 0;
  uint64_t subject = 0; // Building, route, structure or project by kind.
  HouseholdId household = 0;
  std::string detail;
};

const char *cityFabricEventName(CityFabricEventKind);
const char *cityRouteSurfaceName(CityRouteSurface);
const char *cityAccessKindName(CityAccessKind);
const char *cityStructureStateName(CityStructureState);
const char *cityBuildingUseName(CityBuildingUse);

// One settlement's ground and what stands on it.
// The immutable ground a site was founded on, shared by every weekly copy.
struct CityFabricGround
{
  CityBasePlan base;             // Terrain, circulation, envelopes, stairs.
  CityCompletionPlan completion; // The lotted base.
  CityReliefPlan relief;         // Levels per lot.
};

struct CityFabricSite
{
  HistoryNode settlement = 0;
  uint32_t foundedWeek = 0;
  CityFabricSettings settings;
  std::shared_ptr<const CityFabricGround> ground;
  std::map<uint64_t, CityParcel> parcels;
  std::map<uint64_t, CityBuilding> buildings;
  std::map<uint64_t, CityRouteRecord> routes;
  std::map<uint64_t, CityAccessStructure> structures;
  std::map<uint64_t, CityConstruction> constructions; // By project id.
  Position exit, court;            // The approach's foot and head.
  uint64_t nextBuilding = 1, nextStructure = 1;
};

struct CityFabricFacts
{
  uint32_t homes = 0, occupiedHomes = 0, vacantHomes = 0, freeParcels = 0, reachableFreeParcels = 0, underConstruction = 0, suspended = 0;
  uint32_t routesOpen = 0, streets = 0, lanes = 0, footpaths = 0;
  std::set<uint64_t> occupiedBlocks;
};

class CityFabricLedger
{
public:
  explicit CityFabricLedger(CityFabricSettings defaults = {});

  // Plans the base and builds the founding homes on the first terrace, one
  // per founding household as far as the parcels go. Returns the number of
  // homes: the housing capacity the household ledger must start from.
  bool found(HistoryNode settlement, const CityFabricSettings &, uint32_t week, uint32_t foundingHouseholds, std::string *error = nullptr);

  // Weekly, after the realm. Publishes capacity into the household and
  // economy ledgers and moves projects through construction in the realm.
  bool observe(const CityHistorySnapshot &, CityHouseholdLedger &, CityEconomyLedger &, CityRealmLedger &, std::string *error = nullptr);

  // Phase 6: a way may close only if every occupied home still reaches its
  // work, the court and the exit. Refused otherwise, and recorded either way.
  bool closeRoute(HistoryNode settlement, uint64_t route, uint32_t week, const CityEconomyLedger &, const CityHouseholdLedger &, std::string *error = nullptr);

  // Phase 5 and 6 gates for one site against the ledgers of the same week.
  bool validate(const CityHistorySnapshot &, const CityHouseholdLedger &, const CityEconomyLedger &, std::string *error = nullptr) const;

  // Timeline query, mutating nothing: what stood at `week`.
  std::vector<std::pair<uint64_t, const CityBuildingVersion *>> standingAt(HistoryNode settlement, uint32_t week) const;

  // The incremental view: versions that began or ended in (from, to], so a
  // client holding the state at `from` can rebuild only what changed.
  struct CityVersionChange
  {
    uint64_t building = 0;
    const CityBuildingVersion *version = nullptr;
    bool began = false, ended = false;
  };

  std::vector<CityVersionChange> changesBetween(HistoryNode settlement, uint32_t from, uint32_t to) const;

  // Every occupied home reaches the court, the exit and its tenant's work.
  bool reachable(HistoryNode settlement, const CityEconomyLedger &, const CityHouseholdLedger &, std::string *why = nullptr, std::optional<uint64_t> withoutRoute = {}) const;

  // A completion plan at a date: developed lots are the homes standing then.
  CityCompletionPlan completionAt(HistoryNode settlement, uint32_t week) const;

  CityFabricFacts facts(HistoryNode settlement) const;

  const std::map<HistoryNode, CityFabricSite> &sites() const
  {
    return grounds;
  }

  const std::vector<CityFabricEvent> &events() const
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

  CityFabricSettings defaults;
  std::map<HistoryNode, CityFabricSite> grounds;
  std::shared_ptr<std::vector<CityFabricEvent>> log = std::make_shared<std::vector<CityFabricEvent>>();
  std::optional<uint32_t> lastWeek;
};

uint64_t cityGeometryHash(const CityBuildingVersion &, CityBuildingUse);

} // namespace procedural::world::rewrite
