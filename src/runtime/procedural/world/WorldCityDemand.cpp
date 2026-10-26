#include "WorldCityDemand.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace procedural::world::rewrite {
bool deriveCityPopulationDemand(const std::vector<SettlementHousehold> &households, const CityDemandSettings &s, CityPopulationDemand &out, std::string *error)
{
  auto fail = [&](const char *m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  for (double v : {s.housePlotArea, s.shopPlotArea, s.lodgingPlotArea, s.publicPlotArea})
  {
    if (!std::isfinite(v) || v < 20 || v > 10000)
    {
      return fail("invalid provisional plot area");
    }
  }
  if (!std::isfinite(s.openSpaceFraction) || !std::isfinite(s.circulationFraction) || s.openSpaceFraction < 0 || s.openSpaceFraction > .8 || s.circulationFraction < 0 || s.circulationFraction > .8 ||
      !s.nominalLodgingBeds || !s.serviceCatchmentPopulation || households.size() > 100000)
  {
    return fail("invalid land demand policy");
  }
  if (!std::isfinite(s.planningCellAreaAllowance) || s.planningCellAreaAllowance < 0 || s.planningCellAreaAllowance > 1e8)
  {
    return fail("invalid planning cell area allowance");
  }
  CityPopulationDemand d;
  std::set<uint64_t> ids;
  uint32_t families = 0;
  for (auto h : households)
  {
    if (!h.id || !ids.insert(h.id).second || !h.members || h.members > 8 || h.workers > h.members)
    {
      return fail("invalid demand household");
    }
    d.population += h.members;
    d.workers += h.workers;
    d.collectiveBeds += h.collective ? h.members : 0;
    families += !h.collective;
  }
  if (d.population > 100000)
  {
    return fail("demand population budget exceeded");
  }
  d.households = households.size();
  uint32_t shops = families >= 4 ? std::max(1u, families / 5) : 0;
  auto add = [&](const char *name, uint32_t count, DistrictUse use, double area) {
    if (!count)
    {
      return;
    }
    double target = count * area / (1 - s.openSpaceFraction) + s.planningCellAreaAllowance;
    d.programmes.push_back({name, count});
    d.districts.push_back({uint64_t(d.districts.size() + 1), use, area, target, true});
    d.programmeLandArea += target;
  };
  add("ordinary_house", families - shops, DistrictUse::Residential, s.housePlotArea);
  add("general_shop_home", shops, DistrictUse::Craftsmen, s.shopPlotArea);
  add("worker_lodging_hall", (d.collectiveBeds + s.nominalLodgingBeds - 1) / s.nominalLodgingBeds, DistrictUse::WorkerHousing, s.lodgingPlotArea);
  add("community_temple", (d.population + s.serviceCatchmentPopulation - 1) / s.serviceCatchmentPopulation, DistrictUse::Temple, s.publicPlotArea);
  if (d.population >= 100)
  {
    add("town_hall", 1, DistrictUse::Civic, s.publicPlotArea);
  }
  d.circulationArea = d.programmeLandArea * s.circulationFraction / (1 - s.circulationFraction);
  if (error)
  {
    error->clear();
  }
  out = std::move(d);
  return true;
}

bool calculateSettlementFoodSupply(const std::vector<SettlementFoodHolding> &holdings, double consumption, SettlementFoodSupply &out, std::string *error)
{
  auto fail = [&](const char *m) {
    if (error)
    {
      *error = m;
    }
    return false;
  };
  if (!std::isfinite(consumption) || consumption <= 0 || holdings.size() > 4096)
  {
    return fail("invalid food consumption or holding budget");
  }
  SettlementFoodSupply s;
  std::set<uint64_t> ids;
  for (auto h : holdings)
  {
    if (!h.id || !ids.insert(h.id).second)
    {
      return fail("duplicate or invalid productive holding");
    }
    for (double v : {h.cropArea, h.annualYieldPerSquareMetre, h.lossFraction, h.workersPerHectare, h.availableWorkers})
    {
      if (!std::isfinite(v) || v < 0)
      {
        return fail("invalid food coefficient");
      }
    }
    if (h.lossFraction > 1 || h.cropArea > 1e9 || h.annualYieldPerSquareMetre > 1000 || h.workersPerHectare > 10000 || h.availableWorkers > 100000)
    {
      return fail("food coefficient exceeds budget");
    }
    if (!h.accessible)
    {
      continue;
    }
    double required = h.cropArea / 10000 * h.workersPerHectare, labour = required > 0 ? std::min(1., h.availableWorkers / required) : 1.;
    s.cropArea += h.cropArea;
    s.annualGrossYield += h.cropArea * h.annualYieldPerSquareMetre;
    s.annualUsableYield += h.cropArea * h.annualYieldPerSquareMetre * (1 - h.lossFraction) * labour;
  }
  double residents = std::floor(s.annualUsableYield / consumption);
  if (residents > 100000)
  {
    return fail("food supply exceeds settlement population budget");
  }
  s.supportedResidents = uint32_t(residents);
  if (error)
  {
    error->clear();
  }
  out = s;
  return true;
}
} // namespace procedural::world::rewrite
