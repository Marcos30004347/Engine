#include "WorldCityAgriculture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
uint64_t mix(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t holdingId(uint64_t access)
{
  return mix(mix(0xcbf29ce484222325ULL, 300), access);
}
} // namespace

bool planCityAgriculture(const CityTerritory &territory, const CitySkeleton &skeleton, const CityAgricultureSettings &settings, CityAgriculturePlan &result, std::string *error)
{
  auto fail = [&](const char *message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!settings.population || !std::isfinite(settings.annualConsumptionPerResident) || settings.annualConsumptionPerResident <= 0 ||
      !std::isfinite(settings.importFraction) || settings.importFraction < 0 || settings.importFraction > 1 ||
      !std::isfinite(settings.reserveFraction) || settings.reserveFraction < 0 ||
      !std::isfinite(settings.availableWorkers) || settings.availableWorkers < 0)
    return fail("invalid city agriculture demand");
  std::map<CityLandUse, CityCropCoefficient> coefficients;
  for (const auto &c : settings.crops)
  {
    if (!std::isfinite(c.annualYieldPerSquareMetre) || c.annualYieldPerSquareMetre < 0 || !std::isfinite(c.lossFraction) || c.lossFraction < 0 || c.lossFraction > 1 || !std::isfinite(c.workersPerHectare) || c.workersPerHectare < 0)
      return fail("invalid city agriculture coefficient");
    if (!coefficients.emplace(c.use, c).second)
      return fail("duplicate city agriculture coefficient");
  }
  struct Candidate
  {
    const FarmField *field = nullptr;
    uint64_t accessNode = 0;
    double suitability = 0;
    CityCropCoefficient crop;
    double score = 0;
  };
  std::vector<Candidate> candidates;
  const auto distance2 = [](const Position &a, const Position &b)
  {
    const double dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
  };
  const auto centre = [](const std::vector<Position> &p)
  {
    Position c;
    for (const auto &v : p) { c.x += v.x; c.y += v.y; c.z += v.z; }
    if (!p.empty()) { c.x /= p.size(); c.y /= p.size(); c.z /= p.size(); }
    return c;
  };
  const CityCropCoefficient crop = coefficients.count(CityLandUse::Arable) ? coefficients.at(CityLandUse::Arable) : CityCropCoefficient{};
  for (size_t patchIndex = 0; patchIndex < territory.fields.size(); ++patchIndex)
  {
    const double suitability = patchIndex < territory.rural.patches.size() ? territory.rural.patches[patchIndex].meanSuitability : 1.0;
    for (const auto &field : territory.fields[patchIndex].fields)
    {
      const Position access = !field.access.empty() ? field.access.front() : centre(field.boundary);
      uint64_t accessNode = 0;
      double best = std::numeric_limits<double>::max();
      for (const auto &node : skeleton.roadNodes)
        if (distance2(access, node.position) < best)
        {
          best = distance2(access, node.position);
          accessNode = node.id;
        }
      if (!accessNode)
        continue;
      const double usablePerArea = crop.annualYieldPerSquareMetre * (1 - crop.lossFraction);
      candidates.push_back({&field, accessNode, suitability, crop, usablePerArea * (.25 + .75 * suitability)});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b)
            { return a.score != b.score ? a.score > b.score : a.field->id < b.field->id; });

  CityAgriculturePlan out;
  out.revision = std::max(territory.revision, skeleton.revision);
  out.population = settings.population;
  out.requiredAnnualYield = settings.population * settings.annualConsumptionPerResident * (1 - settings.importFraction) * (1 + settings.reserveFraction);
  double workers = settings.availableWorkers;
  std::map<uint64_t, size_t> holdings;
  for (const auto &candidate : candidates)
  {
    if (out.annualUsableYield + 1e-9 >= out.requiredAnnualYield || workers <= 1e-9)
      break;
    const auto &p = *candidate.field;
    const auto &c = candidate.crop;
    const double requiredWorkers = p.area / 10000 * c.workersPerHectare;
    const double assignedWorkers = requiredWorkers > 0 ? std::min(requiredWorkers, workers) : 0;
    const double labour = requiredWorkers > 0 ? assignedWorkers / requiredWorkers : 1;
    const double gross = p.area * c.annualYieldPerSquareMetre;
    const double usable = gross * (1 - c.lossFraction) * labour;
    if (usable <= 1e-9)
      continue;
    const uint64_t hid = holdingId(p.patch ? p.patch : candidate.accessNode);
    out.parcels.push_back({p.id, hid, candidate.accessNode, CityLandUse::Arable, p.area, gross, usable, requiredWorkers, assignedWorkers});
    auto hit = holdings.find(hid);
    if (hit == holdings.end())
    {
      holdings[hid] = out.holdings.size();
      out.holdings.push_back({hid, candidate.accessNode, {}, 0, 0, 0});
      hit = holdings.find(hid);
    }
    auto &holding = out.holdings[hit->second];
    holding.parcels.push_back(p.id);
    holding.area += p.area;
    holding.annualUsableYield += usable;
    holding.workers += assignedWorkers;
    out.productiveArea += p.area;
    out.annualGrossYield += gross;
    out.annualUsableYield += usable;
    out.assignedWorkers += assignedWorkers;
    workers -= assignedWorkers;
  }
  out.foodShortfall = std::max(0.0, out.requiredAnnualYield - out.annualUsableYield);
  out.supportedResidents = uint32_t(std::floor(out.annualUsableYield / settings.annualConsumptionPerResident));
  std::sort(out.parcels.begin(), out.parcels.end(), [](const auto &a, const auto &b) { return a.parcel < b.parcel; });
  std::sort(out.holdings.begin(), out.holdings.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  result = std::move(out);
  if (error)
    error->clear();
  return true;
}
} // namespace procedural::world::rewrite
