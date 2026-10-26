#pragma once
#include "WorldCityMasterPlan.hpp"
#include "WorldSettlementDirector.hpp"

namespace procedural::world::rewrite {
struct CityDemandSettings
{
  double housePlotArea = 140, shopPlotArea = 160, lodgingPlotArea = 360, publicPlotArea = 240, openSpaceFraction = .2, circulationFraction = .25;
  double planningCellAreaAllowance = 0; // Upstream maximum cell area; permits whole-cell rounding of a request.
  uint32_t nominalLodgingBeds = 24, serviceCatchmentPopulation = 300;
};

struct CityPopulationDemand
{
  uint32_t population = 0, households = 0, workers = 0, collectiveBeds = 0;
  double programmeLandArea = 0, circulationArea = 0;
  std::vector<ProgrammeDemand> programmes;
  std::vector<DistrictRequest> districts;
};

// Provisional land demand BEFORE streets and shells; final occupancy still comes
// from solved interiors. Gross area includes an explicit circulation allowance.
bool deriveCityPopulationDemand(const std::vector<SettlementHousehold> &, const CityDemandSettings &, CityPopulationDemand &, std::string *error = nullptr);

struct SettlementFoodHolding
{
  uint64_t id = 0;
  double cropArea = 0, annualYieldPerSquareMetre = 0, lossFraction = 0, workersPerHectare = 0, availableWorkers = 0;
  bool accessible = true;
};

struct SettlementFoodSupply
{
  double cropArea = 0, annualGrossYield = 0, annualUsableYield = 0;
  uint32_t supportedResidents = 0;
};

// Authored yield/labour coefficients, not historical estimates. Inaccessible
// holdings contribute no supply; duplicate holdings cannot double the budget.
bool calculateSettlementFoodSupply(const std::vector<SettlementFoodHolding> &, double annualConsumptionPerResident, SettlementFoodSupply &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
