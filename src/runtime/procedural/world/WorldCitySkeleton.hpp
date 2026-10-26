#pragma once

#include "WorldCityPlaza.hpp"
#include "WorldCityTerritory.hpp"
#include "WorldCityWalls.hpp"
#include "WorldSettlementFabricAdapter.hpp"

namespace procedural::world::rewrite {
enum class CityLandUse : uint8_t
{
  UrbanDistrict,
  UrbanBlock,
  UrbanLot,
  Arable,
  Pasture,
  Orchard,
  Vineyard,
  Common,
  ManagedWood,
  WildLand,
  Wetland,
  Floodplain,
  Quarry,
  ClayPit,
  Cemetery,
  Mill,
  Farmstead,
  RoadReserve,
  WallClearance,
  Moat,
  Harbour,
  Shore,
  MilitaryReserve,
  FutureGrowth,
  Unusable,
  Water
};

enum class CityRegionKind : uint8_t
{
  District,
  Block,
  Lot,
  RuralPatch,
  Field,
  Farmyard,
  Reservation,
  ExteriorParcel
};

enum class CityRegionContactKind : uint8_t
{
  SharedBoundary,
  Contains,
  Access
};

enum class CityRoadKind : uint8_t
{
  Street,
  Approach,
  FieldTrack
};

enum class CityRoadNodeKind : uint8_t
{
  Endpoint,
  Junction,
  Gate
};

enum class CityBlockStrategy : uint8_t { Unplanned, Frontage, Courtyard, Crofts, Infill, Common };

struct CitySkeletonRegion
{
  uint64_t id = 0, source = 0, parent = 0;
  CityRegionKind kind = CityRegionKind::District;
  CityLandUse landUse = CityLandUse::UrbanDistrict;
  DistrictUse districtUse = DistrictUse::Residential;
  std::vector<Position> boundary;
  bool exterior = false;
  double area = 0;
  CityBlockStrategy blockStrategy = CityBlockStrategy::Unplanned;
  std::string lotProgramme; // Block planner role; empty for externally authored lots.
  bool attached = false;
  // An authored public threshold overrides nearest-road inference. Two endpoints.
  std::vector<Position> publicFrontage;
  bool courtFacing = false; // Dwelling entry and grid face the block’s shared court.
};

struct CityRegionContact
{
  uint64_t id = 0, a = 0, b = 0;
  CityRegionContactKind kind = CityRegionContactKind::SharedBoundary;
  Position point, localA, localB;
  double sharedLength = 0;
};

struct CityRoadNode
{
  uint64_t id = 0;
  CityRoadNodeKind kind = CityRoadNodeKind::Endpoint;
  Position position;
  std::vector<uint64_t> edges;
};

struct CityRoadEdge
{
  uint64_t id = 0, source = 0, from = 0, to = 0;
  CityRoadKind kind = CityRoadKind::Street;
  std::vector<Position> path;
  double halfWidth = 0;
};

struct CityRegionAccess
{
  uint64_t id = 0, region = 0, roadNode = 0;
  Position point;
};

struct CityWallNode
{
  uint64_t id = 0, era = 0;
  Position position;
  std::vector<uint64_t> edges, gates;
};

struct CityWallEdge
{
  uint64_t id = 0, era = 0, from = 0, to = 0;
  Position a, b;
};

struct CitySkeletonGate
{
  uint64_t id = 0, source = 0, wallNode = 0, roadNode = 0, era = 0;
  Position position;
};

struct CityPublicSpace
{
  enum class Kind : uint8_t { Sidewalk, Court, Garden, Yard };
  uint64_t owner = 0;
  Kind kind = Kind::Sidewalk;
  std::vector<Position> boundary;
  double rise = 0; // A court's rise is divided into broad, accessible stone steps.
};

struct CitySkeleton
{
  uint64_t revision = 0;
  std::vector<CitySkeletonRegion> regions;
  std::vector<CityRegionContact> regionContacts;
  std::vector<CityRegionAccess> regionAccess;
  std::vector<CityRoadNode> roadNodes;
  std::vector<CityRoadEdge> roadEdges;
  std::vector<CityWallNode> wallNodes;
  std::vector<CityWallEdge> wallEdges;
  std::vector<CitySkeletonGate> gates;
  // Circulation inside a block: the gaps the lot subdivision left between
  // groups of lots. Not streets - a street is a road the network knows about -
  // but ground you can walk, which is what a lot set back from the frontage
  // needs in order to be reached at all.
  std::vector<std::vector<Position>> alleys;
  std::vector<CityPublicSpace> publicSpaces;
  // The market square's structures: arcade, gates, dais, monument. The
  // square's ground is a Court public space like any other; this is what
  // stands on it. The stalls and the hall are lots under the square's region.
  std::vector<CityPlazaPlan> plazas;
};

// Composes the existing authoritative products. Geometry is referenced by stable
// IDs and copied only as immutable polygon/path values. The adapter does not
// regenerate streets, walls, fields or lots. Contacts are undirected records;
// callers obtain A->B and B->A by selecting the opposite endpoint.
bool composeCitySkeleton(
    const CityTerritory &,
    const CityWalls *,
    const SettlementFabric &,
    CitySkeleton &,
    std::string *error = nullptr);

const char *cityLandUseName(CityLandUse);
const char *cityRegionKindName(CityRegionKind);
} // namespace procedural::world::rewrite
