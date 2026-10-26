#pragma once

#include "WorldCityExterior.hpp"
#include "WorldCityGrowth.hpp"

#include <array>
#include <functional>
#include <memory>

namespace procedural::world::rewrite {

enum class CityWallFaceKind : uint8_t
{
  Outside,
  Inside,
  Up,
  Bottom,
  Left,
  Right
};

struct CityWallFace
{
  CityWallFaceKind kind = CityWallFaceKind::Outside;
  std::array<Position, 4> corners{};
};

struct CityWallStructureSegment
{
  uint64_t id = 0, wall = 0, era = 0;
  CityWallLifecycle lifecycle = CityWallLifecycle::Standing;
  Position a, b;
  double width = 0, height = 0, depth = 0;
  bool built = true, wallWalkContinuous = true, sharedWithCastle = false;
  std::array<CityWallFace, 6> faces{};
  bool palisade = false;
};

struct CityGatehouseStructure
{
  uint64_t id = 0, wall = 0, era = 0, wallNode = 0, roadNode = 0;
  Position position;
  double openingWidth = 0, depth = 0, height = 0;
  bool built = true;
};

struct CityWallTowerStructure
{
  uint64_t id = 0, wall = 0, era = 0;
  CityWallLifecycle lifecycle = CityWallLifecycle::Standing;
  Position position;
  double radius = 0, height = 0;
  bool built = true;
};

struct CitySuburbTopology
{
  uint64_t id = 0, zone = 0, accessNode = 0;
  std::vector<Position> boundary;
  double gateDistance = 0, density = 0;
};

struct CityHarbourTopology
{
  uint64_t id = 0, region = 0, accessNode = 0, controllingCastle = 0;
  std::vector<Position> quay;
};

enum class CityResidenceKind : uint8_t { House, Mansion, Manor, Inn, Pub, Dormitory };
enum class CityGroundCover : uint8_t { Automatic, Grass, Dirt, Stone };

struct CityLotDevelopment
{
  uint64_t lot = 0;
  bool developed = true;
  double density = 1;
  // Allocation policy belongs to the city plan. These are preferences passed
  // to the dwelling fitter; usable geometry determines the actual capacity.
  bool merchantHouse = false;
  double preferredFootprintMetres2 = 40, preferredRoomMetres2 = 15;
  uint32_t desiredRooms = 2, maximumFloors = 4;
  CityResidenceKind residence = CityResidenceKind::House;
  bool rowHouse = false; // Rectangular shared-wall frontage; district controls height.
  uint32_t minimumFloors = 1;
  // The trade the household works at, as the dwelling programme's tag:
  // smithy, bakehouse, carpentry, masonry, farmstead, guardhouse, hall,
  // brewhouse. Empty is a house with no trade room. The history names it
  // from the workplace in the building; a planned block names it from the
  // lot's role.
  std::string trade;
  uint32_t architecturalAgeYears=0;
  CityGroundCover groundCover = CityGroundCover::Automatic;
  // maximumFloors == 0 means no district height cap; programme demand decides.
};

struct CitySmoothedRoute
{
  uint64_t id = 0;
  CityRoadKind kind = CityRoadKind::Street;
  std::vector<uint64_t> edges;
  std::vector<Position> path;
  double halfWidth = 0;
  CityGroundCover groundCover = CityGroundCover::Automatic;
};

struct CityWallHistory
{
  uint64_t olderWall = 0, newerWall = 0;
  CityWallLifecycle olderState = CityWallLifecycle::Absorbed;
};

struct CityCompletionSettings
{
  double wallWidth = 1.5, wallHeight = 6, gateOpeningWidth = 5;
  double gatehouseDepth = 5, gatehouseHeight = 8;
  double suburbDensityDecay = 180;
  double towerRadius = 2.5, towerHeight = 8;
  double wallClearanceDistance = 18, cemeteryMinimumArea = 250;
  // Road hierarchy is assigned after graph chains are collapsed. Approaches
  // are broad, through streets medium, and short block-edge lanes varied.
  double approachHalfWidth = 3.2, arterialHalfWidth = 2.6;
  double localLaneMinimumHalfWidth = 1.15, localLaneMaximumHalfWidth = 1.85;
  // How far a street may be pulled off its authored line to become straight.
  //
  // Small on purpose. The road grower emits chains of points whose turns are
  // a fifth of a degree - five millimetres off the chord - so a few
  // centimetres of tolerance collapses those runs to the straight lines they
  // already are, without moving the line anywhere. A metre of tolerance does
  // straighten real bends, and it also drags the centreline across the lots
  // that were laid out against it: the ribbons then meet at angles their
  // booleans cannot resolve and the whole city mesh fails with "boundary
  // trace produced a hole with no containing ring". Redundant vertices are
  // free to remove; the line itself is not free to move.
  double streetStraightenMetres = .15;
  // Chords across a rounded corner. Two is a mitre with a nick off it; zero
  // leaves the corner sharp.
  uint32_t streetCornerSteps = 8;
  bool timberDefences = false; // Historical palisades are archival by default.
};

// Final city-scale structural product. It is deliberately still simple: wall
// prisms and gatehouse dimensions are stored as faces, while mesh generation
// merely triangulates them. The completed skeleton owns every historical wall
// and gate contact, so no later pass locates crossings by visual proximity.
struct CityWaterway
{
  uint64_t route=0;
  std::vector<uint64_t> edges;
  Position a,b;
  double halfWidth=1.2,bankWidth=.4,waterLevel=0,bedLevel=0;
  std::vector<std::pair<double,double>> bridges; // Stations along the channel.
  struct Portal { uint64_t wall=0; double from=0,to=0; };
  std::vector<Portal> portals;
  std::function<double(double,double)> bankGround;
};

struct CityHistoricalLotBinding {
  uint64_t building = 0, lot = 0; // Zero lot means an explicit unmet request.
  double requiredFootprintArea = 0; // Programme demand, retained even when no lot fits.
};

struct CityPlotContractPlan;
struct CityStructurePlan;
struct CityCompositionPlan;
struct CityCompletionPlan
{
  std::shared_ptr<const CityPlotContractPlan> plotContracts;
  std::shared_ptr<const CityStructurePlan> structureContracts;
  std::shared_ptr<const CityCompositionPlan> composition;
  std::vector<CityHistoricalLotBinding> historicalLots;
  std::shared_ptr<const CityGrowthPlan> historicalGrowth;
  uint64_t revision = 0;
  CitySkeleton skeleton;
  CityExteriorAllocation exterior;
  std::vector<CityWallStructureSegment> wallSegments;
  std::vector<CityGatehouseStructure> gatehouses;
  std::vector<CityWallTowerStructure> towers;
  std::vector<CitySuburbTopology> suburbs;
  std::vector<CityHarbourTopology> harbours;
  std::vector<CityLotDevelopment> lots;
  std::vector<CitySmoothedRoute> routes;
  std::vector<CityWallHistory> history;
  std::vector<CityWaterway> waterways;
};

bool realizeCityGrowthWalls(const CityGrowthPlan &, const CityCastlePlan &,
    const CityCompletionSettings &, CityCompletionPlan &, std::string *error = nullptr);

bool completeCityPlan(
    const CitySkeleton &,
    const CityExteriorAllocation &,
    const CityGrowthPlan &,
    const CityCastlePlan &,
    const CityCompletionSettings &,
    CityCompletionPlan &,
    std::string *error = nullptr);

const char *cityWallFaceKindName(CityWallFaceKind);

} // namespace procedural::world::rewrite
