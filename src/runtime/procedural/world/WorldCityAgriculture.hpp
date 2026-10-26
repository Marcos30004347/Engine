#pragma once

#include "WorldCitySkeleton.hpp"

namespace procedural::world::rewrite {
struct CityCropCoefficient
{
  CityLandUse use = CityLandUse::Arable;
  double annualYieldPerSquareMetre = .35;
  double lossFraction = .2;
  double workersPerHectare = 2;
};

struct CityAgricultureSettings
{
  uint32_t population = 0;
  double annualConsumptionPerResident = 500;
  double importFraction = 0;
  double reserveFraction = .2;
  double availableWorkers = 0;
  std::vector<CityCropCoefficient> crops = {
      {CityLandUse::Arable, .35, .2, 2},
      {CityLandUse::Orchard, .25, .15, 3},
      {CityLandUse::Vineyard, .18, .12, 4},
      {CityLandUse::Pasture, .08, .1, 1}};
};

struct CityAgriculturalParcel
{
  uint64_t parcel = 0, holding = 0, accessNode = 0;
  CityLandUse use = CityLandUse::Arable;
  double area = 0, annualGrossYield = 0, annualUsableYield = 0;
  double requiredWorkers = 0, assignedWorkers = 0;
};

struct CityAgriculturalHolding
{
  uint64_t id = 0, accessNode = 0;
  std::vector<uint64_t> parcels;
  double area = 0, annualUsableYield = 0, workers = 0;
};

struct CityAgriculturePlan
{
  uint64_t revision = 0;
  uint32_t population = 0, supportedResidents = 0;
  std::vector<CityAgriculturalParcel> parcels;
  std::vector<CityAgriculturalHolding> holdings;
  double requiredAnnualYield = 0, annualGrossYield = 0, annualUsableYield = 0;
  double productiveArea = 0, assignedWorkers = 0, foodShortfall = 0;
};

// Budgets the already-generated, subdivided FarmField polygons. World Voronoi
// cells are provenance only and are never treated as field geometry here.
// Production stops at local demand plus reserve; unmet demand is quantified.
bool planCityAgriculture(
    const CityTerritory &,
    const CitySkeleton &,
    const CityAgricultureSettings &,
    CityAgriculturePlan &,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
