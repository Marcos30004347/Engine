#pragma once

#include "WorldCitySkeleton.hpp"
#include <map>

namespace procedural::world::rewrite {
enum class CityLandTenure : uint8_t
{
  Private,
  Common,
  Civic,
  Military,
  Protected
};

enum class CityParcelAccess : uint8_t
{
  Road,
  Easement,
  IntentionallyNone
};

enum class CityParcelSource : uint8_t
{
  PlanningCell,
  WorldRegion
};

struct CityExteriorSettings
{
  double wetlandFloodRisk = .75;
  double floodplainFloodRisk = .35;
  double unusableSlope = .6;
  double arableFertility = .65;
  double pastureFertility = .35;
  double militaryDefense = .75;
  double maximumAccessDistance = 600;
  // Overrides are city-planning decisions such as cemetery, moat or quarry.
  // Natural cover, water and biome remain authoritative world-map data.
  std::map<uint64_t, CityLandUse> cellUses;
};

struct CityExteriorParcel
{
  uint64_t id = 0, cell = 0, accessNode = 0;
  CityParcelSource source = CityParcelSource::PlanningCell;
  uint32_t sourceNode = UINT32_MAX, parentNode = UINT32_MAX;
  uint64_t mapRegion = 0;
  uint8_t biome = 0;
  CityLandUse use = CityLandUse::Unusable;
  CityLandTenure tenure = CityLandTenure::Protected;
  CityParcelAccess access = CityParcelAccess::IntentionallyNone;
  std::vector<Position> boundary;
  double area = 0, suitability = 0;
  std::string reason;
};

struct CityExteriorAllocation
{
  uint64_t revision = 0;
  std::vector<CityExteriorParcel> parcels;
  double planningArea = 0, allocatedArea = 0, anonymousArea = 0;
};

// Planning cells are the settlement adapter's city-domain tessellation.
// Existing districts and rural patches decide ownership first; city-policy
// overrides then classify the remainder. Wider world regions, ocean, rivers
// and biomes are never regenerated or copied into the city parcel layer.
bool allocateCityExterior(
    const WorldSettlementSite &,
    const CityTerritory &,
    const CitySkeleton &,
    const CityExteriorSettings &,
    CityExteriorAllocation &,
    std::string *error = nullptr);

// Adds one stable ExteriorParcel region per planning cell and Access contacts
// to the nearest road node selected by allocateCityExterior.
bool appendCityExterior(const CityExteriorAllocation &, CitySkeleton &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
