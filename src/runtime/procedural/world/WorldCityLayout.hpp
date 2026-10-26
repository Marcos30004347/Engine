#pragma once

#include "WorldCityBuildingRegions.hpp"
#include "WorldSurfaceArrangement.hpp"
#include <array>

namespace procedural::world::rewrite {
enum class CityRecipe : uint8_t
{
  OrganicVoronoi,
  RecursiveBlocks,
  PlannedBlocks,
  LSystemRoads // Parish and Muller 2001: segment growth with global goals and local constraints.
};
enum class CityObjectKind : uint8_t
{
  House,
  Workshop,
  Garden,
  RejectedGap
};
enum class CityBuildingType : uint8_t
{
  Cottage,
  ShopHouse,
  MerchantHouse,
  Inn,
  WorkshopYard,
  Warehouse,
  Garden,
  WorkerLodging
};

struct CityBuildingPart
{
  std::vector<Position> footprint; // Convex CCW shell portion; rectangular wings or fitted polygonal parcel roofs.
  double height = 0;
};

struct CityPolygon
{
  std::vector<Position> outer;
  std::vector<std::vector<Position>> holes;
};

struct CityStreet
{
  uint64_t id = 0;
  Position a, b;
  double halfWidth = 2;
  bool arterial = false;
  std::vector<Position> path; // Sampled map centerline; endpoints/intersections stay locked.
  uint8_t hierarchy = 0;      // Local=0, district street=1, planned avenue=2; fixed arteries use arterial.
};

struct CityGate
{
  uint64_t id = 0;
  Position position;
  uint64_t externalRoad = 0;
};

struct CityHousingArea
{
  uint64_t id = 0;
  DistrictUse use = DistrictUse::Residential;
  // Ordinary housing policy masks supplied by the master plan. The largest
  // overlap assigns an entire physical block; these are not landmark reserves.
  std::vector<std::vector<Position>> pieces;
  double targetPlotArea = 100;
  bool reclaimLocalStreets = false; // Only completely contained local links; connectivity and main routes stay protected.
  double gardenFraction = 0;        // Minimum fraction of each fitted plot left outside its shell envelope.
};

struct CitySettings
{
  uint64_t seed = 7, revision = 1;
  CityRecipe recipe = CityRecipe::OrganicVoronoi;
  // Voronoi patches and recursive layouts both refine toward targetBlockArea.
  double siteSpacing = 28, targetBlockArea = 950, minimumBlockArea = 120;
  double streetHalfWidth = 2, minimumWidth = 5, maximumWidth = 20, minimumDepth = 6, maximumDepth = 26;
  double frontageGap = 1, setback = 1, overhang = .4, maximumFoundationRelief = 1.5;
  double houseWeight = .75, workshopWeight = .15, gardenWeight = .10;
  double streetCurvature = 1.5, junctionRadius = 1;
  bool walls = false;
  double wallSetback = 6;                             // Metres from the wall face; gates keep clear approaches.
  double wallCornerInset = 0, wallSectionLength = 40; // Faceted perimeter; maximum straight tower-to-tower span.
  double wallTowerRadius = 3, wallTowerHeight = 7;
  // Caller-supplied convex CCW buildable land for connected outer Voronoi wards.
  // Omitted masks never imply buildable countryside.
  std::vector<std::vector<Position>> suburbLand;
  double suburbRoadLength = 95, suburbGrowthRadius = 65, suburbCellSpacing = 32;
  uint32_t maximumSuburbWards = 24;
  // Opt-in city housing: partition final street blocks, retain interior lots as
  // shared open space. The existing frontage recipe remains the village default.
  bool blockHousing = false;
  bool regularBuildingShells = true; // Street-aligned rectangles and courtyard wings within irregular plots.
  bool emitBuildings = true;         // Planning pass keeps street geometry without generating houses.
  double targetPlotArea = 100;
  std::vector<CityHousingArea> housingAreas;
  std::vector<CityPlanDistrict> districts; // Accepted master-plan allocations, before housing.
  // PlannedBlocks uses coherent axes within coarse districts, then partitions
  // houses independently. Radians; zero turn keeps an aligned planned extension.
  double plannedAxisRadians = 0, plannedDistrictTurnRadians = 0, plannedBlockAspect = 1.5;
  uint32_t maximumSites = 64, maximumSplitDepth = 10, maximumObjects = 2000;
  uint32_t siteRelaxations = 0; // Lloyd iterations on the organic ward sites (Watabou relaxes its central patches).
  bool alleyAccess = false;     // Interior parcels are built too, reached through the frontage gaps (Watabou's alleys).
  double alleyReachMetres = 18; // An alley-served parcel farther than this from its street stays a court.
  double gridChaos = 0;         // Watabou's createAlleys: angle jitter of lot cuts on large parcels (0 = orthogonal).
  bool frontageStrips = false;  // Lots are strips extruded from each street edge to a lot depth set by the block's plot area; the interior is a court.
  bool watabouLots = false;     // Lots by Watabou's createAlleys (generator::LotPlanner); wins over frontage strips.
  bool lotsOnly = false;        // Stop after the lots: no houses, no director sites.
  bool roadRowLots = false;     // Every block is one row of lots along its streets (villages); the interior stays open.
  // Houses per hectare by district use (index = DistrictUse); > 0 sets the lot
  // area of that use as 7000 / density (a lot is about 70 % of its share of
  // the block), so wealth sets how many houses a block carries.
  std::array<double, 16> housesPerHectare{};
  std::vector<std::vector<Position>> wardCells;      // Non-empty: these convex cells are the wards, uncut (Watabou: patch = ward = block).
  bool wardBuildings = false;                        // Watabou's ward geometry: every ward is bisected into buildings with alleys (createAlleys), outskirts thinned (filterOutskirts); no rings, courts or estates.
  std::vector<std::vector<Position>> outerWardCells; // Planning cells outside the wall: the outer wards (village rows along the gate roads) instead of the jittered lattice.
  bool laneHierarchy = false;                        // Organic ward edges are narrow lanes; paths between neighbouring gates are district streets.
  bool mixedBlockGrammar = false;                    // Organic city: middle-class and merchant wards are gridded, the rest stay organic (Might of Merchants).
  uint32_t maximumBlockSides = 0;                    // > 0: organic blocks with more sides are cut along the best chord (the class grammar sets 5).
  double maximumBlockDepth = 0;                      // > 0: organic blocks deeper than this are cut so every lot fronts a street.
  // Parish-Muller road growth (LSystemRoads, generator::RoadGrowth): highways
  // follow the population density and the pattern (0 basic, 1 raster, 2 radial,
  // 3 elevation), streets branch at right angles after a delay; ends snap to
  // crossings and roads within the radii.
  uint8_t lsystemPattern = 0;
  double lsystemHighwayLength = 52, lsystemStreetLength = 34, lsystemIntersectionSnapMetres = 10, lsystemRoadSnapMetres = 8;
  double lsystemHighwayBranchProbability = .15, lsystemStreetBranchProbability = .6;
  int lsystemStreetDelay = 4;
  uint32_t lsystemMaximumSegments = 200;
};

struct CityWard
{
  uint64_t id = 0;
  std::vector<std::vector<Position>> pieces;
};

struct CityFrontage
{
  uint64_t id = 0, ward = 0;
  Position a, b;
  uint64_t street = 0;
  int side = 1;               // Left (+1) or right (-1) of the sampled street path.
  std::vector<Position> path; // Supporting street; ward coverage bounds placement.
};

struct CityObject
{
  uint64_t id = 0, ward = 0, frontage = 0, street = 0;
  uint64_t block = 0; // Final physical street block, independent of allocation wards.
  CityObjectKind kind = CityObjectKind::RejectedGap;
  std::vector<Position> footprint, clearance; // Fitted plot and conservative clearance.
  CityBuildingType buildingType = CityBuildingType::Cottage, requestedBuildingType = CityBuildingType::Cottage;
  std::vector<CityBuildingPart> parts; // Authoritative shell geometry, not the fitted plot.
  std::vector<Position> courtyard;     // Open yard within the plot, never roofed.
  bool compactFallback = false;
  bool outsideWalls = false;
  Position entrance, access;
  double foundation = 0, height = 0;
};

// Generated IDs are scoped to one city product. The world adapter must namespace
// generated IDs when combining settlements; fixed arterial IDs are preserved.
struct CityWallTower
{
  uint64_t id = 0;
  Position center;
  std::vector<Position> footprint;
  double height = 7;
};

struct CityLayout
{
  uint64_t revision = 0;
  double wallSetback = 0;
  CityPolygon envelope;                       // Accepted envelope after optional wall rounding.
  std::vector<std::vector<Position>> suburbs; // Accepted exterior ground, excluding enclosure and reservations.
  std::vector<Position> sites;
  std::vector<CityWard> wards;
  std::vector<CityStreet> streets, walls;
  std::vector<CityWallTower> towers;
  std::vector<uint64_t> reclaimedStreets; // Eligible local streets removed before final block extraction.
  std::vector<CityObject> objects;
  std::vector<CityFrontage> frontages;
  std::vector<CityStreetBlock> blocks; // Populated by the block housing path.
  std::vector<CityPlanDistrict> districts;
  std::vector<std::vector<Position>> reservations; // Explicit and net compound exclusions.
  // All pieces are convex and map-owned. Object instances remain separate.
  std::vector<SurfaceContour> contours;
  uint32_t rejectedAreas = 0, splitAttempts = 0;
  uint32_t subdivisionCells = 0; // Before dissolving discarded-street boundaries.
  uint32_t unrefinedBlocks = 0;  // Oversized patches retained for working depth or split budget.
  uint32_t alleyServed = 0;      // Interior parcels built through the alley gaps (alleyAccess).
  uint32_t lsystemSegments = 0;  // Road segments grown by the LSystemRoads recipe.
};

// Envelope/hole rings must be simple and disjoint. Reservations are convex CCW
// polygons. Fixed arteries are immutable; gates require a named external road.
// V1 street surfaces require a pre-engineered level platform. Unprepared slopes
// fail instead of inheriting terrain crossfall. Buildings can span bounded relief.
// Atomic on error, including disconnected circulation and exhausted budgets.
bool generateCityLayout(
    const CityPolygon &,
    const std::vector<std::vector<Position>> &reservations,
    const std::vector<CityStreet> &fixedArteries,
    const std::vector<CityGate> &,
    const CitySettings &,
    const std::function<double(double, double)> &terrain,
    CityLayout &,
    std::string *error = nullptr);

struct CityInstanceTriangle
{
  std::array<Position, 3> points;
  uint64_t object = 0;
  uint32_t material = 12;
};

// Untextured massing with terrain-seated foundation walls and pitched roofs.
bool meshCityInstances(const CityLayout &, const std::function<double(double, double)> &terrain, std::vector<CityInstanceTriangle> &, std::string *error = nullptr);
// Convex CCW polygons only. Touching counts as overlap.
bool cityPolygonsOverlap(const std::vector<Position> &, const std::vector<Position> &);
} // namespace procedural::world::rewrite
