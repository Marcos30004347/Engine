#pragma once

#include "WorldCitySurfaces.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace procedural::world::hierarchy { struct DwellingVoxelAssembly; struct Skeleton; }
namespace procedural::world::rewrite {

// All horizontal terrain addresses use one world lattice. Tiles own half-open
// cell ranges; a sample on a border is addressed by its global integer vertex,
// never by a tile-local floating point coordinate.
struct CityVoxelTileGrid
{
  static constexpr int32_t millimetresPerVoxel = 100;
  static constexpr int32_t voxelsPerTile = 320;
  int32_t minTileX = 0, minTileZ = 0, maxTileX = 0, maxTileZ = 0; // Max exclusive.
};

struct CityVoxelTileAddress { int32_t x = 0, z = 0; };
CityVoxelTileAddress cityVoxelTileOfCell(int64_t voxelX, int64_t voxelZ);
// The returned height is an exact millimetre value shared by both adjacent
// tile builds. The callable must be the same immutable relief field for both.
bool cityVoxelTerrainSample(int64_t vertexX, int64_t vertexZ,
                            const std::function<double(double,double)> &ground,
                            int64_t &heightMillimetres);

enum class CityVoxelFeatureKind : uint8_t
{
  Terrain, Road, Paving, RetainingWall, WaterBank, Water, Foundation
};
enum class CityVoxelFallback : uint8_t { TerrainHeightfield, SurfacePolygon, SolidMesh, WaterPlane };

struct CityVoxelFeature
{
  CityVoxelFeatureKind kind = CityVoxelFeatureKind::Terrain;
  CityVoxelFallback fallback = CityVoxelFallback::TerrainHeightfield;
  uint64_t owner = 0;
  uint32_t sourceIndex = 0;
  std::array<int64_t,4> boundsMillimetres{}; // x0,z0,x1,z1, inclusive geometric bounds.
  CityVoxelTileAddress firstTile{}, lastTile{}; // Inclusive spatial query range.
};

// A candidate is authored from the common lot boundary before either house
// compiles. Resolve it after accepted building IDs are known: a rejected
// neighbour cannot suppress the surviving house's outside face.
struct CityVoxelPartyLine
{
  std::array<int64_t,4> edgeMillimetres{}; // Canonical endpoint order.
  uint64_t buildingA = 0, buildingB = 0;
};
struct CityVoxelPartySeam
{
  std::array<int64_t,4> edgeMillimetres{};
  uint64_t buildingA = 0, buildingB = 0;
  uint64_t faceOwner = 0; // Geometric tie-break for a coincident face/cell.
  bool shared = false; // Both buildings accepted; only partyContacts suppress faces.
};

// Compact authored strips only. Keep these for city-wide joining instead of
// retaining skeletons or dense 10 cm occupancy for every house.
struct CityVoxelPartyFacet
{
  std::array<int64_t,4> edgeMillimetres{};
  uint64_t building = 0;
  uint32_t part = 0;
  int64_t along0 = 0, along1 = 0, bottom = 0, top = 0, offset = 0;
};
struct CityVoxelPartyContact
{
  std::array<int64_t,4> edgeMillimetres{};
  uint64_t faceOwner = 0;
  uint32_t partA = 0, partB = 0; // Both source appearances remain addressable.
  int64_t along0 = 0, along1 = 0, bottom = 0, top = 0;
};

struct CityVoxelBoundaryPlan
{
  CityVoxelTileGrid terrain;
  std::vector<CityVoxelFeature> features;
  std::vector<CityVoxelPartySeam> partySeams;
  std::vector<CityVoxelPartyContact> partyContacts;
  // On an exact tile plane the tile with the lower coordinate emits a
  // coplanar face. Both tiles still meet at the same global border samples.
  // The coarse fallback is the same terrain field or source surface polygon.
  bool lowerTileOwnsBorderFaces = true;
};

bool cityVoxelFeature(CityVoxelFeatureKind kind, CityVoxelFallback fallback,
                      uint64_t owner, uint32_t sourceIndex,
                      const std::vector<Position> &ring, CityVoxelFeature &result);
void addCityVoxelBoundaryFeature(CityVoxelBoundaryPlan &, const CityVoxelFeature &);
bool buildCityVoxelBoundaryPlan(const CitySurfaceStructure &surfaces,
                                const std::vector<CityVoxelPartyLine> &partyLines,
                                const std::set<uint64_t> &acceptedBuildings,
                                CityVoxelBoundaryPlan &result,
                                std::string *error = nullptr);
bool collectCityVoxelPartyFacets(const hierarchy::DwellingVoxelAssembly &,
                                 const hierarchy::Skeleton &,
                                 const std::vector<CityVoxelPartyLine> &,
                                 std::vector<CityVoxelPartyFacet> &,
                                 std::string *error = nullptr);
bool resolveCityVoxelPartyContacts(CityVoxelBoundaryPlan &,
                                   const std::vector<CityVoxelPartyFacet> &,
                                   std::string *error = nullptr);
bool auditCityVoxelBoundaryPlan(const CityVoxelBoundaryPlan &, std::string *error = nullptr);

struct CityVoxelFoundationSample
{
  int64_t xMillimetres = 0, zMillimetres = 0;
  int64_t groundMillimetres = 0, topMillimetres = 0;
  // The terrain owns the interface itself and occupancy below the ground;
  // the named foundation mesh owns volume above it up to topMillimetres.
};
bool sampleCityVoxelFoundationJoin(const std::vector<Position> &footprint,
                                   double supportTop,
                                   const std::function<double(double,double)> &ground,
                                   std::vector<CityVoxelFoundationSample> &result,
                                   std::string *error = nullptr);

} // namespace procedural::world::rewrite
