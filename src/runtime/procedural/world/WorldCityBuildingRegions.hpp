#pragma once
#include "WorldCityMasterPlan.hpp"
#include "WorldSurfaceArrangement.hpp"

namespace procedural::world::rewrite {
struct CityRegionPolygon
{
  std::vector<Position> outer;
  std::vector<std::vector<Position>> holes;
};

struct CityBuildingRegionSettings
{
  uint32_t landMaterial = 11;
  uint64_t district = 1;
  DistrictUse use = DistrictUse::Residential;
  double setback = 1.5, depth = 7, passageWidth = 3, height = 8;
  double minimumArea = 60, minimumCourtyardArea = 25, maximumRelief = 1.5;
};

struct CityBuildingRegion
{
  uint64_t id = 0, block = 0, district = 0, frontageEdge = 0;
  DistrictUse use = DistrictUse::Residential;
  CityRegionPolygon footprint;
  std::vector<Position> passage;
  Position streetAccess;
  double foundation = 0, height = 0, area = 0;
};

struct CityStreetBlock
{
  uint64_t id = 0;
  CityRegionPolygon boundary;
  std::vector<CityRegionPolygon> courtyards;
  std::string rejection;
  std::vector<CityRegionPolygon> unallocated;  // Remnants, not designed courtyards.
  std::vector<std::vector<Position>> lots;     // Watabou lots (createAlleys), when the layout allocates them.
  std::vector<std::vector<Position>> openLots; // Lots left empty by the empty probability.
  uint64_t district = 0;
  DistrictUse use = DistrictUse::Residential;
};

struct CityRegionTriangle
{
  std::array<Position, 3> points;
  uint64_t region = 0;
  uint32_t material = 12;
};

struct CityBuildingRegions
{
  uint64_t revision = 0;
  std::vector<CityStreetBlock> blocks;
  std::vector<CityBuildingRegion> regions;
  std::vector<SurfaceContour> foundations;
  std::vector<CityRegionTriangle> triangles;
};

// Dissolve arrangement seams and extract actual connected street-bounded land.
// Shared by perimeter regions and parcel housing. Atomic on extraction failure.
bool extractCityStreetBlocks(const SurfaceArrangement &, uint32_t landMaterial, std::vector<CityStreetBlock> &, std::string *error = nullptr);
// Reads the FINAL arranged ground map. Road/water/wall contours must already be
// reconciled. Decomposition seams are dissolved, not promoted to street frontage.
// Bounded conservative polygon erosion; positive passage width is mandatory.
// Unsuitable blocks retain a rejection reason. Invalid input/geometry fails atomically.
// Outputs building masks and massing; building programs and interiors are downstream.
bool generateCityBuildingRegions(const SurfaceArrangement &, const CityBuildingRegionSettings &, const std::function<double(double, double)> &terrain, CityBuildingRegions &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
