#pragma once

// Phase 7 of the living city history: the manor, the keep and the walls.
//
// The seat's defences grow in stages the ledgers can justify: a manor when
// someone rules, a palisade around it when the seat is threatened or grown,
// a keep, a curtain wall, and a castle once the keep stands inside a closed
// circuit with towers. The manor and the keep are buildings the spatial
// ledger raises on reserved parcels; this ledger owns the circuits.
//
// A circuit is built in funded segments by the crew the economy employs,
// with a gate wherever an open way or a stair crosses it, so no route is ever
// severed. An incomplete circuit is never reported closed. An outer circuit
// encloses more terraces; only once it is closed and gated is the inner one
// retired to a district boundary, its record kept.

#include "WorldCityFabric.hpp"

namespace procedural::world::rewrite {

enum class CityDefenceStage
{
  None,
  Manor,
  FortifiedManor,
  Keep,
  Curtain,
  Castle
};

struct CityDefenceSettings
{
  double segmentMetres = 30;      // A funded piece of wall.
  double towerSpacingMetres = 45;
  double marginMetres = 4;        // Between the terraces enclosed and the wall line.
  double palisadeHeight = 3.5, wallHeight = 6, wallWidth = 1.5, palisadeWidth = .6;
  double gateWidth = 4.5, towerRadius = 2.5, towerHeight = 8;
  uint32_t workUnitsPerSegment = 6, workUnitsPerGate = 8, workUnitsPerTower = 6;
  uint32_t workUnitsPerBuilderPerMonth = 4;
  uint32_t idleMonthsBeforeSuspension = 1;
};

struct CityWallSegmentRecord
{
  uint64_t id = 0;
  Position a, b;
  CityStructureState state = CityStructureState::Planned;
  uint32_t workUnits = 0, workDone = 0;
  std::optional<uint32_t> completedWeek;
};

struct CityGateRecord
{
  uint64_t id = 0, segment = 0, route = 0; // Route zero is a stair or the approach.
  Position position;
  CityStructureState state = CityStructureState::Planned;
  uint32_t workUnits = 0, workDone = 0;
  std::optional<uint32_t> completedWeek;
};

struct CityTowerRecord
{
  uint64_t id = 0, segment = 0; // The segment whose end it stands on.
  Position position;
  CityStructureState state = CityStructureState::Planned;
  uint32_t workUnits = 0, workDone = 0;
  std::optional<uint32_t> completedWeek;
};

struct CityDefenceCircuit
{
  uint64_t id = 0, project = 0;
  uint32_t era = 0;
  bool palisade = false;
  std::vector<Position> boundary; // Closed, counter-clockwise in plan.
  std::vector<CityWallSegmentRecord> segments;
  std::vector<CityGateRecord> gates;
  std::vector<CityTowerRecord> towers;
  std::set<uint64_t> enclosedBlocks;
  CityWallLifecycle lifecycle = CityWallLifecycle::Planned; // Planned while incomplete or abandoned, Standing when closed, Absorbed when retired.
  std::optional<uint32_t> closedWeek, absorbedWeek;
  uint32_t idleMonths = 0;
  bool abandoned = false;

  bool closed() const;
};

struct CityDefenceSite
{
  HistoryNode settlement = 0;
  CityDefenceStage stage = CityDefenceStage::None;
  std::vector<CityDefenceCircuit> circuits;
  uint64_t nextId = 1;
};

enum class CityDefenceEventKind
{
  StageReached,
  CircuitPlanned,
  SegmentCompleted,
  GateCompleted,
  TowerCompleted,
  CircuitClosed,
  CircuitAbandoned,
  CircuitAbsorbed,
  WorksSuspended,
  WorksResumed,
  Rejected
};

struct CityDefenceEvent
{
  uint32_t week = 0;
  CityDefenceEventKind kind = CityDefenceEventKind::StageReached;
  HistoryNode settlement = 0;
  uint64_t subject = 0;
  std::string detail;
};

const char *cityDefenceStageName(CityDefenceStage);
const char *cityDefenceEventName(CityDefenceEventKind);

class CityDefenceLedger
{
public:
  explicit CityDefenceLedger(CityDefenceSettings = {});

  // Weekly, after the fabric. Reads the fabric's ways, stairs and buildings;
  // moves wall projects through construction in the realm; pays the crew.
  bool observe(const CityHistorySnapshot &, const CityHouseholdLedger &, CityEconomyLedger &, CityRealmLedger &, const CityFabricLedger &, std::string *error = nullptr);

  bool validate(const CityHistorySnapshot &, const CityFabricLedger &, std::string *error = nullptr) const;

  // Adds the walls, gates and towers standing at `week` to a completion plan
  // (built flags follow the date), for meshing and rendering.
  void applyTo(HistoryNode settlement, uint32_t week, CityCompletionPlan &) const;

  const std::map<HistoryNode, CityDefenceSite> &sites() const
  {
    return grounds;
  }

  const std::vector<CityDefenceEvent> &events() const
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

  const CityDefenceSettings &settings() const
  {
    return rules;
  }

private:
  class Week;

  CityDefenceSettings rules;
  std::map<HistoryNode, CityDefenceSite> grounds;
  std::shared_ptr<std::vector<CityDefenceEvent>> log = std::make_shared<std::vector<CityDefenceEvent>>();
  std::optional<uint32_t> lastWeek;
};

// Ways and stairs of a site that cross a closed boundary, with where.
struct CityBoundaryCrossing
{
  uint64_t route = 0; // Zero for a stair or the approach.
  size_t edge = 0;    // Boundary edge index.
  Position position;
};

std::vector<CityBoundaryCrossing> cityBoundaryCrossings(const CityFabricSite &, const std::vector<Position> &boundary);
bool cityInsideBoundary(const std::vector<Position> &boundary, const Position &);

} // namespace procedural::world::rewrite
