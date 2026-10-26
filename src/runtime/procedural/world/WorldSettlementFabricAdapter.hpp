#pragma once
#include "WorldCityDirectorAdapter.hpp"
#include "WorldCityTerritory.hpp"
#include "WorldRuntimeBuild.hpp"
#include "WorldSettlementSiteAdapter.hpp"
namespace procedural::world::rewrite
{
// Village fabric on the accepted territory: the allocated planning cells are
// joined into one urban polygon, levelled onto a bounded terrace (published
// through the terrain callback, so streets, houses, render and collision all
// read the same ground), then streets/houses come from the existing city
// layout and residents from the settlement director. Rejections are recorded
// on the product; the territory stands even when no fabric is built.
struct SettlementFabricSettings
{
  double maximumTerraceStepMetres = 2.5; // Largest cut/fill accepted at the terrace edge.
  double arteryHalfWidth = 2.5;
  CitySettings city = []
  {
    CitySettings c; // Village lanes: sparse organic wards; districts/housing areas are filled by the adapter.
    c.siteSpacing = 60;
    c.maximumSites = 16;
    c.targetBlockArea = 2400;
    return c;
  }();
  SettlementDirectorInput director; // Policy only; sites/network come from the layout.
  bool meshFarmBuildings = true;
};
struct SettlementFabric
{
  uint64_t revision = 0;
  CityPolygon polygon; // Local frame, like every product below.
  std::vector<std::vector<Position>> reservations;
  std::vector<CityStreet> arteries;
  std::vector<CityGate> gates;
  std::vector<uint64_t> releasedCompounds; // Compounds crossed by a gate route, released to ordinary land.
  uint32_t routeCells = 0;                 // Unallocated circulation cells joined into the polygon.
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
struct SettlementRuntimeSettings
{
  double groundSpacingMetres = 40, modifiedSpacingMetres = 8, coverSpacingMetres = 4, tileMetres = 800;
  uint32_t maximumFaces = 20000; // The v8 capital budget; exceeding it is a visible failure.
};
// Existing runtime build input from the site, territory and fabric products.
// Ground and engineered contours keep the producers' own ownership; structures
// carry the city and farm instances. Coordinates stay in the site-local frame.
bool composeSettlementRuntimeInput(const WorldSettlementSite &, const CityTerritory &, const SettlementFabric &, const SettlementRuntimeSettings &, RuntimeBuildInput &, std::string *error = nullptr);
GenerationTask settlementFabricGenerator(uint64_t domain);       // site + territory + fabric settings -> settlement.fabric
GenerationTask settlementRuntimeBriefGenerator(uint64_t domain); // site + territory + fabric + runtime settings -> runtime.brief
} // namespace procedural::world::rewrite
