#pragma once
#include "WorldDwellingPlan.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::rewrite {
struct PopulationBrief
{
  uint64_t seed = 1;
  uint32_t minimum = 0, maximum = 0;
};

struct SettlementHousehold
{
  uint64_t id = 0;
  uint32_t members = 0, workers = 0;
  bool collective = false;
};

bool generateSettlementPopulation(const PopulationBrief &, std::vector<SettlementHousehold> &, std::string *error = nullptr);
enum class ProgrammeKind
{
  Home,
  Shop,
  Collective,
  Temple,
  Civic,
  Warehouse
};

struct SettlementProgramme
{
  std::string id;
  ProgrammeKind kind = ProgrammeKind::Home;
  DwellingUse dwelling = DwellingUse::Cottage;
  uint32_t floors = 1;
  double minimumPlotArea = 50;
  uint32_t serviceCatchmentCapacity = 0;
};

std::vector<SettlementProgramme> settlementProgrammeCatalogue();

struct SettlementTravelNode
{
  uint64_t id = 0;
  Position position;
};

struct SettlementTravelEdge
{
  uint64_t a = 0, b = 0;
  double cost = 0;
}; // Positive metres or authored travel cost; barriers omit links.

struct SettlementSite
{
  uint64_t id = 0, node = 0;
  DwellingBrief shell;
  double elevationPreference = 0, centrality = 0, slope = 0;
  bool buildable = true, water = true;
  uint32_t allowedKinds = 0x3f; // Explicit geography/director eligibility, not inferred from shape.
};

struct ProgrammeDemand
{
  std::string programme;
  uint32_t count = 0;
};

struct ProgrammeDeficit
{
  std::string programme;
  uint32_t count = 0;
};

struct SettlementBuilding
{
  uint64_t id = 0, site = 0, node = 0;
  std::string programme;
  ProgrammeKind kind = ProgrammeKind::Home;
  uint32_t serviceCapacity = 0;
  DwellingPlan interior;
  int builtYear = 0;
};

struct SettlementTenancy
{
  uint64_t household = 0, building = 0;
  uint32_t residents = 0;
};

struct SettlementEmployment
{
  uint64_t household = 0, building = 0;
  uint32_t workers = 0;
  double travelCost = 0;
};

struct SettlementService
{
  uint64_t household = 0, building = 0;
  uint32_t residents = 0;
  double travelCost = 0;
};

struct SettlementEvent
{
  uint64_t id = 0;
  int year = 0;
  uint32_t arrivals = 0, departures = 0, buildingsAdded = 0;
};

struct SettlementDirectorInput
{
  uint64_t settlement = 0, seed = 1;
  int year = 1;
  PopulationBrief population;
  std::vector<SettlementProgramme> catalogue;
  std::vector<SettlementSite> sites;
  std::vector<SettlementTravelNode> nodes;
  std::vector<SettlementTravelEdge> edges;
  std::vector<ProgrammeDemand> demands; // Optional explicit programme requests; otherwise population-derived.
  uint32_t maximumFitAttempts = 8192;
  uint32_t maximumNewBuildings = 512, foodResidentCapacity = 100000, waterResidentCapacity = 100000;
  double maximumCommute = 1200, maximumServiceTravel = 800;
};
enum class SettlementSizeClass
{
  Hamlet,
  Village,
  Town,
  City,
  LargeCity
};

struct SettlementState
{
  uint64_t settlement = 0, revision = 0;
  SettlementSizeClass sizeClass = SettlementSizeClass::Hamlet; // Art label; never grants political status.
  int year = 0;
  std::vector<SettlementHousehold> households;
  std::vector<SettlementBuilding> buildings;
  std::vector<SettlementTenancy> tenancies;
  std::vector<SettlementEmployment> employment;
  std::vector<SettlementService> services;
  std::vector<ProgrammeDeficit> unmetProgrammes;
  std::vector<SettlementEvent> history;
  std::vector<uint64_t> serviceReserveSites; // Retained sites keep future facilities from being crowded out.
  std::vector<std::string> rejections;
  uint32_t fitAttempts = 0;
  uint32_t population = 0, housed = 0, unhoused = 0, workers = 0, employed = 0, unserved = 0, foodShortfall = 0, waterShortfall = 0;
};

struct SettlementGrowth
{
  uint64_t event = 0, expectedRevision = 0;
  int year = 0;
  PopulationBrief arrivals;
  std::vector<uint64_t> departingHouseholds; // Whole households; never split merely to meet a target.
};

// Deterministic, bounded programme fit, measured room capacity, whole-household
// tenancy, shortest-path jobs/services and supply ledgers. Only validated prepared
// sites can be built. No automatic terrain mutation, shell scaling or WFC.
bool planSettlement(const SettlementDirectorInput &, SettlementState &, std::string *error = nullptr);
// Reuses buildings and occupied homes, fills vacancies before constructing on
// new prepared sites. Atomic, revision-checked, replayable local growth/decline.
bool advanceSettlement(const SettlementDirectorInput &, const SettlementState &, const SettlementGrowth &, SettlementState &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
