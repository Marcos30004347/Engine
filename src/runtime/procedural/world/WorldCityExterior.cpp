#include "WorldCityExterior.hpp"
#include "MapClimate.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace procedural::world::rewrite {
namespace {
constexpr double epsilon = 1e-6;

bool wooded(const Biome biome)
{
  switch (biome)
  {
  case Biome::Taiga:
  case Biome::TemperateForest:
  case Biome::TemperateRainforest:
  case Biome::MontaneForest:
  case Biome::TropicalSeasonalForest:
  case Biome::TropicalRainforest:
  case Biome::SubtropicalForest:
    return true;
  default:
    return false;
  }
}

uint64_t mix(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t stableId(uint64_t domain, uint64_t source)
{
  return mix(mix(0xcbf29ce484222325ULL, domain), source);
}

double area(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return std::abs(a) * .5;
}

Position centroid(const std::vector<Position> &p)
{
  Position c;
  for (const auto &v : p)
  {
    c.x += v.x;
    c.y += v.y;
    c.z += v.z;
  }
  if (!p.empty())
  {
    c.x /= p.size();
    c.y /= p.size();
    c.z /= p.size();
  }
  return c;
}

bool inside(const Position &p, const std::vector<Position> &poly)
{
  if (poly.size() < 3)
    return false;
  bool result = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
  {
    const auto &a = poly[i], &b = poly[j];
    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
      result = !result;
  }
  return result;
}

double distance2(const Position &a, const Position &b)
{
  const double dx = a.x - b.x, dz = a.z - b.z;
  return dx * dx + dz * dz;
}

bool accessibleUse(CityLandUse use)
{
  switch (use)
  {
  case CityLandUse::Wetland:
  case CityLandUse::Floodplain:
  case CityLandUse::WildLand:
  case CityLandUse::Shore:
  case CityLandUse::Unusable:
  case CityLandUse::Water:
    return false;
  default:
    return true;
  }
}

CityLandTenure tenure(CityLandUse use)
{
  switch (use)
  {
  case CityLandUse::Common:
  case CityLandUse::Pasture:
  case CityLandUse::ManagedWood:
    return CityLandTenure::Common;
  case CityLandUse::UrbanDistrict:
  case CityLandUse::Cemetery:
  case CityLandUse::RoadReserve:
  case CityLandUse::Harbour:
  case CityLandUse::FutureGrowth:
    return CityLandTenure::Civic;
  case CityLandUse::WallClearance:
  case CityLandUse::Moat:
  case CityLandUse::MilitaryReserve:
    return CityLandTenure::Military;
  case CityLandUse::Wetland:
  case CityLandUse::Floodplain:
  case CityLandUse::WildLand:
  case CityLandUse::Shore:
  case CityLandUse::Unusable:
  case CityLandUse::Water:
    return CityLandTenure::Protected;
  default:
    return CityLandTenure::Private;
  }
}

} // namespace

bool allocateCityExterior(
    const WorldSettlementSite &site,
    const CityTerritory &territory,
    const CitySkeleton &skeleton,
    const CityExteriorSettings &settings,
    CityExteriorAllocation &result,
    std::string *error)
{
  CityExteriorAllocation out;
  out.revision = std::max(territory.revision, skeleton.revision);
  std::set<uint64_t> ids;
  std::set<uint64_t> urbanCells;
  for (const auto &district : territory.regions.districts)
    urbanCells.insert(district.cells.begin(), district.cells.end());

  for (const auto &cell : site.brief.input.cells)
  {
    const double cellArea = area(cell.boundary);
    if (!cell.id || !ids.insert(cell.id).second || cell.boundary.size() < 3 || cellArea <= epsilon)
    {
      if (error)
        *error = "exterior allocation requires unique non-zero cell IDs and positive polygons";
      return false;
    }
    CityExteriorParcel p;
    p.id = stableId(200, cell.id);
    p.cell = cell.id;
    p.source = CityParcelSource::PlanningCell;
    p.sourceNode = cell.sourceNode;
    p.parentNode = cell.parentNode;
    p.mapRegion = cell.mapRegion;
    p.biome = cell.biome;
    p.boundary = cell.boundary;
    p.area = cellArea;
    p.suitability = std::clamp(cell.fertility * (1 - cell.floodRisk) * (1 - cell.slope), 0.0, 1.0);
    auto forced = settings.cellUses.find(cell.id);
    if (forced != settings.cellUses.end())
    {
      p.use = forced->second;
      p.reason = "explicit planning override";
    }
    else if (urbanCells.count(cell.id))
    {
      p.use = CityLandUse::UrbanDistrict;
      p.reason = "accepted city district";
    }
    else
    {
      const Position centre = centroid(cell.boundary);
      bool rural = false;
      for (const auto &patch : territory.rural.patches)
        if (inside(centre, patch.boundary))
        {
          rural = true;
          break;
        }
      if (rural)
      {
        p.use = CityLandUse::Arable;
        p.reason = "accepted agricultural patch";
      }
      else if (!cell.buildable || cell.slope >= settings.unusableSlope)
      {
        p.use = CityLandUse::Unusable;
        p.reason = "unbuildable or excessive slope";
      }
      else if (cell.navigableShore)
      {
        p.use = CityLandUse::Shore;
        p.reason = "navigable shore reserve";
      }
      else if (cell.floodRisk >= settings.wetlandFloodRisk)
      {
        p.use = CityLandUse::Wetland;
        p.reason = "high flood risk";
      }
      else if (cell.floodRisk >= settings.floodplainFloodRisk)
      {
        p.use = CityLandUse::Floodplain;
        p.reason = "seasonal flood risk";
      }
      else if (cell.outsideOldWall && wooded(Biome(cell.biome)))
      {
        p.use = CityLandUse::ManagedWood;
        p.reason = "wooded subcell retained as managed woodland";
      }
      else if (cell.outsideOldWall && Biome(cell.biome) == Biome::Mediterranean)
      {
        p.use = CityLandUse::Orchard;
        p.reason = "Mediterranean subcell allocated to orchard";
      }
      else if (cell.outsideOldWall && cell.fertility >= settings.arableFertility)
      {
        p.use = CityLandUse::Arable;
        p.reason = "fertile exterior land";
      }
      else if (cell.outsideOldWall && cell.fertility >= settings.pastureFertility)
      {
        p.use = CityLandUse::Pasture;
        p.reason = "moderate exterior fertility";
      }
      else if (cell.outsideOldWall && cell.defense >= settings.militaryDefense)
      {
        p.use = CityLandUse::MilitaryReserve;
        p.reason = "defensible exterior position";
      }
      else if (cell.outsideOldWall)
      {
        p.use = CityLandUse::Common;
        p.reason = "uncommitted exterior common";
      }
      else
      {
        p.use = CityLandUse::FutureGrowth;
        p.reason = "unassigned interior growth reserve";
      }
    }
    p.tenure = tenure(p.use);
    if (accessibleUse(p.use) && !skeleton.roadNodes.empty())
    {
      const Position centre = centroid(p.boundary);
      double best = settings.maximumAccessDistance * settings.maximumAccessDistance;
      for (const auto &node : skeleton.roadNodes)
        if (distance2(centre, node.position) <= best)
        {
          best = distance2(centre, node.position);
          p.accessNode = node.id;
        }
      p.access = p.accessNode ? CityParcelAccess::Road : CityParcelAccess::Easement;
    }
    else
      p.access = CityParcelAccess::IntentionallyNone;
    out.planningArea += p.area;
    out.allocatedArea += p.area;
    out.parcels.push_back(std::move(p));
  }
  out.anonymousArea = std::max(0.0, out.planningArea - out.allocatedArea);
  std::sort(out.parcels.begin(), out.parcels.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  result = std::move(out);
  return true;
}

bool appendCityExterior(const CityExteriorAllocation &allocation, CitySkeleton &skeleton, std::string *error)
{
  std::set<uint64_t> regionIds;
  for (const auto &r : skeleton.regions)
    regionIds.insert(r.id);
  std::set<uint64_t> roadIds;
  for (const auto &n : skeleton.roadNodes)
    roadIds.insert(n.id);
  for (const auto &parcel : allocation.parcels)
  {
    if (!regionIds.insert(parcel.id).second)
    {
      if (error)
        *error = "exterior parcel collides with an existing skeleton region ID";
      return false;
    }
    if (parcel.accessNode && !roadIds.count(parcel.accessNode))
    {
      if (error)
        *error = "exterior parcel names a missing road access node";
      return false;
    }
    skeleton.regions.push_back({parcel.id, parcel.cell, 0, CityRegionKind::ExteriorParcel, parcel.use, DistrictUse::Residential, parcel.boundary, parcel.use != CityLandUse::UrbanDistrict, parcel.area});
    if (parcel.accessNode)
    {
      const Position c = centroid(parcel.boundary);
      skeleton.regionAccess.push_back({stableId(201, parcel.id), parcel.id, parcel.accessNode, c});
    }
  }
  std::sort(skeleton.regions.begin(), skeleton.regions.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  std::sort(skeleton.regionContacts.begin(), skeleton.regionContacts.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  std::sort(skeleton.regionAccess.begin(), skeleton.regionAccess.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  skeleton.revision = std::max(skeleton.revision, allocation.revision);
  return true;
}
} // namespace procedural::world::rewrite
