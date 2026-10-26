#pragma once
#include "MapGraph.hpp"
#include "WorldCityDemand.hpp"
#include "WorldDomainGeneratorAdapters.hpp"
#include "WorldPipeline.hpp"
#include <memory>

namespace procedural::world::rewrite {
// W0 actual-world input adapter. Selects one settlement site on the generated
// map and composes the city/territory brief from the map's own products: leaf
// Voronoi cells (refined on demand through the map hierarchy), the continuous
// elevation surface, explicit ocean/lake cells and river segments, the climate
// moisture field and the biome classification. No synthetic flat terrain or
// assumed water enters the brief; every rejected candidate keeps its reason.
// Authored scale classes; the population brief decides the class, the class
// decides the grammar (lanes, blocks, walls, plaza).
enum class SettlementClass : uint8_t
{
  Hamlet,
  Village,
  Town,
  City,
  Capital
};
SettlementClass settlementClassOf(uint32_t population);
const char *settlementClassName(SettlementClass);

struct WorldSiteSelectionSettings
{
  uint64_t producer = 1, revision = 1;
  uint32_t preferredCell = kInvalidNode; // Explicit leaf choice; still validated.
  double windowMetres = 3200, suitabilitySpacingMetres = 16;
  double planningSpacingMetres = 40, hinterlandSpacingMetres = 180; // Map refinement spacings.
  double maximumSiteGradient = .08, minimumWaterClearanceMetres = 60, waterReachMetres = 1500;
  double riverBankMetres = 6;          // Wet margin beside the explicit river corridor.
  double waterSupplyReachMetres = 600; // Explicit potable-water eligibility distance for director sites.
  double cropAreaPerResident = 2500;   // Authored game coefficient for rural area budgets.
  double coreAreaFactor = 1.6;         // Walled core area over the demanded district area (towns and cities).
  double outskirtsAreaFactor = .9;     // Outer-ward planning cells beyond the wall, as a share of the core area; the rest of the leaf is farmland.
  double exitOffsetMetres = 40;        // Road exits sit this far outside the urban cell boundary.
  uint32_t maximumRuralSources = 4, maximumCandidates = 512, maximumAttempts = 4;
  // Planned civic centre: a regular polygon cut into the planning tiling at
  // the settlement centre (0 sides keeps the Voronoi cell). Radius scales
  // with the settlement class when zero.
  // Village forms after the classic typology: Haufendorf (cluster), Strassendorf
  // (street), Runddorf (round: a ring of farms about a green with the church),
  // Angerdorf (an elongated green between two roads), Streusiedlung (scattered
  // farmsteads). Auto picks by class and site.
  enum class VillageForm : uint8_t
  {
    Auto,
    Cluster,
    Street,
    Round,
    Anger,
    Scattered
  } villageForm = VillageForm::Auto;
  double plazaAspect = 1; // > 1 stretches a four-sided plaza along the road axis (Anger).
  uint32_t plazaSides = 6;
  double plazaRadiusMetres = 0;
  PopulationBrief population{1, 120, 120};
  CityDemandSettings demand;
  SuitabilitySettings suitability; // Bounds and spacing come from the window.
  CityTerritorySettings territory = [] {
    CityTerritorySettings t;
    t.fields.entranceWidth = 4; // v8 field entrance; also bounds the field access grid per region.
    t.fields.trackWidth = 6;
    t.fields.headland = 6;
    t.rural.maximumPatches = 192; // Farms fill the leaf around the town, not just the exits.
    t.farmsteadInterval = 4;
    return t;
  }(); // rural.regions and area limits derive from the actual regions.
};

struct WorldSiteCandidate
{
  uint32_t cell = kInvalidNode;
  uint64_t region = 0;
  Position centre;
  double elevation = 0, gradient = 0, hinterlandGradient = 0, openNeighbours = 0, waterDistance = 0, fertility = 0, moisture = 0, score = 0;
  uint8_t biome = 0;
  bool accepted = false;
  std::string rejection;
};

struct WorldSettlementSite
{
  uint64_t id = 0, revision = 0;
  uint32_t cell = kInvalidNode;
  uint64_t region = 0;
  Position centre;      // World frame.
  Position origin;      // World-frame corner of the local domain.
  Position localCentre; // Site centre in the local frame.
  std::string regionName;
  // Every brief coordinate below is local to `origin`: the settlement
  // generators keep absolute geometric tolerances that only hold near the
  // origin. Consumers add `origin` back when publishing into the world.
  SuitabilitySettings bounds;
  std::vector<WorldSiteCandidate> candidates; // Score order; rejections remain visible.
  std::vector<std::string> rejections;        // Composition omissions for the accepted site.
  PopulationBrief populationBrief;            // Same brief the director replays for tenancy.
  std::vector<SettlementHousehold> households;
  CityPopulationDemand demand;
  std::vector<RuralRegion> regions;               // Same list as brief.settings.rural.regions.
  std::vector<std::vector<Position>> urbanGround; // Planning cell polygons, clipped to the window.
  TerritoryGenerationBrief brief;
  std::shared_ptr<const MapSurfaceField> surface;  // Immutable capture behind brief.terrain.
  std::function<bool(double, double)> water;       // Explicit map water predicate (local frame).
  std::function<bool(double, double)> waterSupply; // Potable-water eligibility (local frame).
  std::vector<uint32_t> refinedLeaves;
  uint32_t attempts = 0;
  double planningArea = 0, hinterlandArea = 0, openArea = 0;
  SettlementClass settlementClass = SettlementClass::Village;
  WorldSiteSelectionSettings::VillageForm villageForm = WorldSiteSelectionSettings::VillageForm::Cluster; // Chosen form (villages and hamlets).
  uint64_t plazaCell = 0, templeCell = 0;                                                                 // Prepared institutional sites at the centre.
};

bool selectWorldSettlementSite(const GeneratedWorld &, const WorldSiteSelectionSettings &, WorldSettlementSite &, std::string *error = nullptr);

// Kingdom grammar: settlement roles across the whole world. Sites are ranked
// with the same candidate ledger, then chosen with role-dependent spacing:
// one capital, towns, villages, hamlets. Links are planning roads (a spanning
// tree over towns plus village-to-town links), not engineered routes.
struct KingdomSettings
{
  uint32_t towns = 3, villages = 8, hamlets = 12;
  // Road villages: linear villages along the trunk roads between towns, at
  // this interval, far from the capital.
  double roadVillageIntervalMetres = 1500, roadVillageReachMetres = 450, roadVillageMinimumFromCapitalMetres = 1200;
  uint32_t roadVillages = 6;
  double capitalSpacingMetres = 3000, townSpacingMetres = 2000, villageSpacingMetres = 900, hamletSpacingMetres = 500;
  uint32_t capitalPopulation = 2500, townPopulation = 700, villagePopulation = 150, hamletPopulation = 40;
};

struct KingdomSettlement
{
  uint64_t id = 0;
  uint32_t cell = kInvalidNode;
  SettlementClass role = SettlementClass::Village;
  Position centre;
  double score = 0;
  uint32_t population = 0;
  uint64_t parentTown = 0;
  std::string regionName;
  bool alongRoad = false; // Linear village placed on a trunk road.
};

struct KingdomLink
{
  uint64_t from = 0, to = 0;
  Position a, b;
  bool trunk = false; // Town-to-town spanning tree, or village-to-town branch.
};

struct KingdomPlan
{
  std::vector<KingdomSettlement> settlements;
  std::vector<KingdomLink> links;
  std::vector<std::string> rejections;
};

bool selectKingdomSites(const GeneratedWorld &, const WorldSiteSelectionSettings &, const KingdomSettings &, KingdomPlan &, std::string *error = nullptr);

// Typed DAG adapters. The world product is shared immutably; payload changes
// must advance the brief revision like any other director input.
struct WorldSiteGenerationBrief
{
  std::shared_ptr<const GeneratedWorld> world;
  WorldSiteSelectionSettings settings;
};

GenerationTask worldSettlementSiteGenerator(uint64_t domain);      // world.site-brief -> world.settlement-site
GenerationTask settlementTerritoryBriefGenerator(uint64_t domain); // world.settlement-site -> city.territory-brief
} // namespace procedural::world::rewrite
