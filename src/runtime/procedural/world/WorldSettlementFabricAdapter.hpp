#pragma once
#include "WorldCityDirectorAdapter.hpp"
#include "WorldCityTerritory.hpp"
#include "WorldRuntimeBuild.hpp"
#include "WorldSettlementSiteAdapter.hpp"
#include "generator/CompoundPlanner.hpp"

namespace procedural::world::rewrite {
// Village fabric on the accepted territory: the allocated planning cells are
// joined into one urban polygon, levelled onto a bounded terrace (published
// through the terrain callback, so streets, houses, render and collision all
// read the same ground), then streets/houses come from the existing city
// layout and residents from the settlement director. Rejections are recorded
// on the product; the territory stands even when no fabric is built.
// Jettying (A1) eligibility and clearance, decided per house before its
// interior is planned. Pure, so the acceptance cases can be checked directly.
struct JettyRequest
{
  bool poorOrMiddleHousehold = true;     // Explicit household/building class.
  bool poorOrMiddleNeighbourhood = true; // Explicit district class.
  bool institutional = false;
  uint32_t floors = 1;
  double streetWidthMetres = 5;       // Full carriageway width in front of the house.
  double oppositeJettyMetres = 0;     // Accepted projection of the facade across the street.
  double minimumCorridorMetres = 2.5; // Corridor that must stay clear at upper-floor height.
  uint64_t seed = 1, house = 1;
};

struct JettyDecision
{
  std::vector<double> perFloor; // Indexed by floor; ground floor 0.
  double total = 0;
  bool eligible = false, jettied = false;
  std::string reason;
};

JettyDecision decideJetty(const JettyRequest &);

struct SettlementFabricSettings
{
  double maximumTerraceStepMetres = 2.5; // Largest cut/fill accepted at the terrace edge.
  uint64_t variantSeed = 0;              // Non-zero: jitter the class grammar (block/plot size, axis, wall rounding, outskirts).
  int recipeOverride = -1;               // >= 0: force a CityRecipe (3 = Parish-Muller L-system roads) after the class grammar.
  bool lotsOnly = false;                 // Render the lot allocation only: no houses, no director.
  bool roadSettlement = false;           // Roads first (generator::SettlementLayout): the road L-system, its faces as districts, chord lots, buildings; no wall.
  bool watabouStreets = true;            // Gate roads follow the planning-cell edges to the plaza (Watabou's topology paths), never cross a cell.
  int patternOverride = -1;              // >= 0: force the road growth pattern (0 basic, 1 raster, 2 radial, 3 elevation).
  double arteryHalfWidth = 2.5;
  CitySettings city = [] {
    CitySettings c; // Village lanes: sparse organic wards; districts/housing areas are filled by the adapter.
    c.siteSpacing = 60;
    c.maximumSites = 16;
    c.targetBlockArea = 2400;
    return c;
  }();
  SettlementDirectorInput director; // Policy only; sites/network come from the layout.
  bool meshFarmBuildings = true;
  bool classGrammar = true; // Derive the layout recipe from the settlement class.
  // Towns and cities wall a convex hull of their allocated cells; unbuildable
  // hull cells become reservations and usable cells outside the wall become
  // suburb land for the layout's outer wards.
  bool convexWalls = true;
  enum class VillageForm : uint8_t
  {
    Auto,      // The site's chosen form (WorldSettlementSite::villageForm), else the gate geometry.
    Nucleated, // Green at the centre, gate roads converging on it.
    Linear     // One through road gate to gate, houses either side.
  } villageForm = VillageForm::Auto;
};

// Per-district economy from the director's fitted buildings, tenancies and
// employment: the neighbourhood is the accepted district, so its ledger is
// measured, not authored.
struct NeighbourhoodLedger
{
  uint64_t district = 0;
  DistrictUse use = DistrictUse::Residential;
  uint32_t houses = 0, buildings = 0, residents = 0, workers = 0, shops = 0, jettied = 0;
  double area = 0;
};

struct SettlementFabric
{
  uint64_t revision = 0;
  CityPolygon polygon; // Local frame, like every product below.
  std::vector<std::vector<Position>> reservations;
  std::vector<CityStreet> arteries;
  std::vector<CityGate> gates;
  std::vector<uint64_t> releasedCompounds;                               // Compounds crossed by a gate route, released to ordinary land.
  uint64_t plazaCell = 0;                                                // Civic anchor the gate roads converge on.
  bool linear = false, walled = false;                                   // Chosen village form; convex wall envelope in use.
  std::vector<std::pair<Position, Position>> wallMoves;                  // Curtain-wall vertex moves (from, to) applied to every shared polygon.
  std::vector<std::vector<Position>> approaches;                         // Gate to rural exit, straight planning tracks (C2).
  uint32_t jettied = 0, flush = 0, jettyExcluded = 0, jettyRejected = 0; // A1 ledger.
  std::vector<NeighbourhoodLedger> neighbourhoods;
  std::vector<generator::CompoundPlan> compounds; // A2: castle, manor, church and hall precincts.
  uint32_t routeCells = 0;                        // Unallocated circulation cells joined into the polygon.
  double terraceLevel = 0, terraceStep = 0;
  bool terraceAccepted = false;
  std::function<double(double, double)> terrain; // Terraced surface, local frame.
  CityLayout layout;
  bool layoutAccepted = false;
  SettlementDirectorInput directorInput;
  SettlementState population;
  bool populated = false;
  std::vector<CityInstanceTriangle> instances;
  SplineFaceMesh structures; // City instances plus farm buildings, local frame.
  std::vector<uint32_t> structuralMaterials;
  uint32_t farmTriangles = 0;
  std::vector<std::string> rejections;
};

bool composeSettlementFabric(const WorldSettlementSite &, const CityTerritory &, const SettlementFabricSettings &, SettlementFabric &, std::string *error = nullptr);
enum class SettlementRuntimeDomain
{
  Core, // Urban polygon plus a margin: fits one arrangement job.
  Full, // Whole hinterland domain: reports the arrangement capacity failure when it does not fit.
  Tile  // One explicit rectangle; the per-tile batching policy issues one job per tile.
};

struct SettlementRuntimeSettings
{
  SettlementRuntimeDomain domain = SettlementRuntimeDomain::Core;
  double coreMarginMetres = 80;
  double tileMinX = 0, tileMinZ = 0, tileMaxX = 0, tileMaxZ = 0; // Domain == Tile only.
  double groundSpacingMetres = 40, modifiedSpacingMetres = 8, coverSpacingMetres = 16, tileMetres = 800;
  double stationSpacingMetres = 5; // Finest station spacing on terrain-following contour edges.
  double heightErrorMetres = .5;   // LOD 0 measured height error against the actual surface.
  double normalErrorDegrees = 180; // Height-only LOD contract for this slice; a normal budget is a later policy.
  uint32_t refinementPasses = 48, maximumAddedVertices = 60000;
  uint32_t maximumFaces = 40000; // Twice the v8 capital budget: outer wards split every road band per piece.
};

// Existing runtime build input from the site, territory and fabric products.
// Ground and engineered contours keep the producers' own ownership; structures
// carry the city and farm instances. Coordinates stay in the site-local frame.
bool composeSettlementRuntimeInput(const WorldSettlementSite &, const CityTerritory &, const SettlementFabric &, const SettlementRuntimeSettings &, RuntimeBuildInput &, std::string *error = nullptr);
GenerationTask settlementFabricGenerator(uint64_t domain);       // site + territory + fabric settings -> settlement.fabric
GenerationTask settlementRuntimeBriefGenerator(uint64_t domain); // site + territory + fabric + runtime settings -> runtime.brief
} // namespace procedural::world::rewrite
