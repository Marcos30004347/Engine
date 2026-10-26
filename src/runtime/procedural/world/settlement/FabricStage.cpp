#include "procedural/world/settlement/FabricStage.hpp"

#include "procedural/world/WorldCityDirectorAdapter.hpp"
#include "procedural/world/WorldSettlementDirector.hpp"
#include "procedural/world/generator/CompoundPlanner.hpp"
#include "procedural/world/generator/PatchTiling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::settlement {

using namespace ::procedural::world::rewrite;
using namespace ::procedural::world::generator;

namespace {

using Poly = std::vector<Position>;

bool housingUse(DistrictUse u)
{
  return u == DistrictUse::Residential || u == DistrictUse::Suburb || u == DistrictUse::WealthyResidential || u == DistrictUse::WorkerHousing;
}

// Welds a triangle into the shared mesh, so two faces that meet share a vertex
// rather than each carrying its own copy of the seam.
//
// Copied verbatim from the adapter rather than rewritten. An earlier pass
// reimplemented it and quietly dropped the expected normal, which desynchronised
// the parallel triangle and normal arrays and crashed the runtime composer
// several stages downstream. A move is a move.
void appendTriangle(SplineFaceMesh &mesh, std::vector<uint32_t> &materials, std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> &index, const std::array<Position, 3> &p, uint32_t material)
{
  std::array<uint32_t, 3> ids;

  for (int k = 0; k < 3; ++k)
  {
    auto key = std::make_tuple(int64_t(std::llround(p[k].x * 1e4)), int64_t(std::llround(p[k].y * 1e4)), int64_t(std::llround(p[k].z * 1e4)));
    auto it = index.find(key);

    if (it == index.end())
    {
      it = index.emplace(key, uint32_t(mesh.vertices.size())).first;
      mesh.vertices.push_back(p[k]);
    }

    ids[k] = it->second;
  }

  const auto a = p[0], b = p[1], c = p[2];
  Position n{(b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)};
  double l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);

  if (!(l > 1e-12))
  {
    return;
  }

  mesh.triangles.push_back(ids);
  mesh.expectedNormals.push_back({n.x / l, n.y / l, n.z / l});
  materials.push_back(material);
}

} // namespace

bool PopulationStage::run(FabricBuild &build, std::string *error)
{
  (void)error;

  // The phases were written against these names on one enormous stack. Binding
  // them here rather than rewriting three hundred lines of references is what
  // keeps this a move and not a rewrite - and the move is what makes the next
  // one possible.
  SettlementFabric &r = build.fabric;
  const SettlementFabricSettings &s = build.settings;
  const WorldSettlementSite &site = build.site;
  const CityTerritory &territory = build.territory;
  const CitySettings &city = build.city;
  const uint64_t plazaCell = build.plazaCell;
  auto &accepted = build.acceptedJetty;

  // The untreated ground, as opposed to r.terrain which is the terrace the
  // streets and houses were levelled onto. Farm buildings stand on the real
  // ground, which is why the two are kept apart.
  const auto &base = site.brief.terrain;

  (void)s;
  (void)site;
  (void)territory;
  (void)city;
  (void)plazaCell;
  (void)accepted;
  (void)base;

  // Residents through the director with explicit terrain/water eligibility.
  SettlementDirectorInput policy = s.director;
  policy.settlement = site.id;
  policy.seed = site.populationBrief.seed;
  policy.population = site.populationBrief;
  if (policy.catalogue.empty())
  {
    policy.catalogue = settlementProgrammeCatalogue();
    // Two-floor houses on narrow plots, with the planner's turning stair; the
    // researched six-programme catalogue itself stays unchanged.
    policy.catalogue.push_back({"town_house", ProgrammeKind::Home, DwellingUse::Cottage, 2, 42, 0});
    policy.catalogue.push_back({"small_cottage", ProgrammeKind::Home, DwellingUse::Cottage, 1, 28, 0}); // Alley lots in packed wards.
  }
  // Fit budgets scale with the settlement class; a city offers hundreds of plots.
  {
    const uint32_t scale = site.settlementClass == SettlementClass::Capital ? 16 : site.settlementClass == SettlementClass::City ? 8 : site.settlementClass == SettlementClass::Town ? 4 : 1;
    policy.maximumFitAttempts = std::max(policy.maximumFitAttempts, 8192u * scale);
    policy.maximumNewBuildings = std::max(policy.maximumNewBuildings, 512u * scale);
  }
  policy.sites.clear();
  const auto &w = site.brief.suitability;
  // Neighbourhood class per house: the accepted district piece containing its
  // entrance. Wealthy and institutional districts exclude jettying.
  auto districtUseAt = [&](Position p, DistrictUse &use) {
    for (const auto &d : r.layout.districts)
    {
      for (const auto &piece : d.pieces)
      {
        if (strictlyInsideConvex(piece, p) || onRing(piece, p, 1e-6))
        {
          use = d.use;
          return true;
        }
      }
    }
    return false;
  };
  std::map<uint64_t, double> streetWidth;
  for (const auto &st : r.layout.streets)
  {
    streetWidth[st.id] = st.halfWidth * 2;
  }
  // Opposite facades: houses on the same street whose access points face each
  // other. Decided in id order so a later house sees the earlier projection.
  for (const auto &o : r.layout.objects)
  {
    if (o.kind != CityObjectKind::House && o.kind != CityObjectKind::Workshop)
    {
      continue;
    }
    SettlementSite ss;
    ss.id = o.id;
    size_t cell = w.cellAt(o.entrance.x, o.entrance.z);
    ss.buildable = cell != SIZE_MAX && w.cells()[cell].walkable && !(w.cells()[cell].reasons & Wet) && !site.water(o.entrance.x, o.entrance.z);
    ss.water = site.waterSupply(o.entrance.x, o.entrance.z);
    ss.slope = 0; // On the accepted terrace.
    DistrictUse use = DistrictUse::Residential;
    districtUseAt(o.entrance, use);
    JettyRequest q;
    q.poorOrMiddleNeighbourhood = use == DistrictUse::Residential || use == DistrictUse::Craftsmen || use == DistrictUse::WorkerHousing || use == DistrictUse::Suburb;
    q.poorOrMiddleHousehold =
        o.buildingType == CityBuildingType::Cottage || o.buildingType == CityBuildingType::ShopHouse || o.buildingType == CityBuildingType::WorkshopYard || o.buildingType == CityBuildingType::WorkerLodging;
    q.institutional =
        !(use == DistrictUse::Residential || use == DistrictUse::Craftsmen || use == DistrictUse::WorkerHousing || use == DistrictUse::Suburb || use == DistrictUse::Merchant || use == DistrictUse::WealthyResidential);
    q.floors = o.buildingType == CityBuildingType::ShopHouse ? 2 : (o.parts.empty() ? 1 : o.parts.front().height >= 5.4 ? 2 : 1);
    q.streetWidthMetres = streetWidth.count(o.street) ? streetWidth[o.street] : 4;
    q.seed = site.id;
    q.house = o.id;
    for (const auto &other : r.layout.objects)
    {
      if (other.id < o.id && other.street == o.street && accepted.count(other.id) && length(other.access, o.access) < 8 && length(other.entrance, o.entrance) > q.streetWidthMetres * .5)
      {
        q.oppositeJettyMetres = std::max(q.oppositeJettyMetres, accepted[other.id]);
      }
    }
    JettyDecision decision = decideJetty(q);
    if (decision.jettied)
    {
      ss.shell.jettyMetres = decision.perFloor;
      accepted[o.id] = decision.total;
      ++r.jettied;
    }
    else if (decision.eligible)
    {
      decision.reason.find("narrow") != std::string::npos ? ++r.jettyRejected : ++r.flush;
    }
    else
    {
      ++r.jettyExcluded;
    }
    policy.sites.push_back(ss);
  }
  r.rejections.push_back(
      "jettying: " + std::to_string(r.jettied) + " projected, " + std::to_string(r.flush) + " flush by mix, " + std::to_string(r.jettyRejected) + " rejected for street clearance, " + std::to_string(r.jettyExcluded) +
      " excluded by class or single storey");
  std::string directorError;
  if (!prepareCityDirector(r.layout, policy, r.directorInput, &directorError))
  {
    r.rejections.push_back("director adapter rejected the layout: " + directorError);
  }
  else if (!planSettlement(r.directorInput, r.population, &directorError))
  {
    r.rejections.push_back("settlement director rejected the programme: " + directorError);
  }
  else
  {
    r.populated = true;
  }
  if (std::getenv("CITY_DEBUG"))
  {
    size_t buildable = 0, watered = 0;
    for (const auto &ss : policy.sites)
    {
      buildable += ss.buildable;
      watered += ss.water;
    }
    std::fprintf(stderr, "layout: objects=%zu alleyServed=%u blocks=%zu wards=%zu\n", r.layout.objects.size(), r.layout.alleyServed, r.layout.blocks.size(), r.layout.wards.size());
    std::fprintf(
        stderr,
        "director: sites=%zu buildable=%zu water=%zu buildings=%zu housed=%u unhoused=%u fitAttempts=%u unmet=%zu rejections=%zu\n",
        policy.sites.size(),
        buildable,
        watered,
        r.population.buildings.size(),
        r.population.housed,
        r.population.unhoused,
        r.population.fitAttempts,
        r.population.unmetProgrammes.size(),
        r.population.rejections.size());
    for (size_t i = 0; i < r.population.rejections.size() && i < 6; ++i)
    {
      std::fprintf(stderr, "director: %s\n", r.population.rejections[i].c_str());
    }
    for (size_t i = 0; i < r.population.unmetProgrammes.size() && i < 4; ++i)
    {
      std::fprintf(stderr, "director unmet: %s x%u\n", r.population.unmetProgrammes[i].programme.c_str(), r.population.unmetProgrammes[i].count);
    }
  }

  return true;
}

bool CompoundStage::run(FabricBuild &build, std::string *error)
{
  (void)error;

  // The phases were written against these names on one enormous stack. Binding
  // them here rather than rewriting three hundred lines of references is what
  // keeps this a move and not a rewrite - and the move is what makes the next
  // one possible.
  SettlementFabric &r = build.fabric;
  const SettlementFabricSettings &s = build.settings;
  const WorldSettlementSite &site = build.site;
  const CityTerritory &territory = build.territory;
  const CitySettings &city = build.city;
  const uint64_t plazaCell = build.plazaCell;
  auto &accepted = build.acceptedJetty;

  // The untreated ground, as opposed to r.terrain which is the terrace the
  // streets and houses were levelled onto. Farm buildings stand on the real
  // ground, which is why the two are kept apart.
  const auto &base = site.brief.terrain;

  (void)s;
  (void)site;
  (void)territory;
  (void)city;
  (void)plazaCell;
  (void)accepted;
  (void)base;

  // A2 compounds: every accepted non-housing district (except the plaza) gets
  // its structure inside the layout's own compound inset, approached from the
  // district's boundary portal.
  {
    generator::Ledger compoundLedger;
    const double margin = std::max(4., city.streetHalfWidth * 1.75 + city.setback + 1);
    for (const auto &d : r.layout.districts)
    {
      if (housingUse(d.use) || d.use == DistrictUse::Civic || (d.anchorCell == plazaCell && d.use != DistrictUse::Temple))
      {
        continue;
      }
      generator::CompoundKind kind = d.use == DistrictUse::Castle || d.use == DistrictUse::Citadel || d.use == DistrictUse::Military ? generator::CompoundKind::Castle
                                     : d.use == DistrictUse::Manor                                                                   ? generator::CompoundKind::Manor
                                     : d.use == DistrictUse::Temple || d.use == DistrictUse::Church                                  ? generator::CompoundKind::Church
                                                                                                                                     : generator::CompoundKind::Hall;
      auto pieces = d.pieces;
      Poly precinct;
      double largest = 0;
      for (const auto &piece : pieces)
      {
        Poly inner = piece;
        for (size_t k = 0; k < piece.size() && !inner.empty(); ++k)
        {
          auto a = piece[k], bpt = piece[(k + 1) % piece.size()];
          double nx = a.z - bpt.z, nz = bpt.x - a.x, len = std::hypot(nx, nz); // Inward normal of a CCW edge.
          if (len < 1e-9)
          {
            continue;
          }
          nx /= len;
          nz /= len;
          const double c = nx * a.x + nz * a.z - margin;
          Poly out;
          for (size_t i = 0; i < inner.size(); ++i)
          {
            auto u = inner[i], v = inner[(i + 1) % inner.size()];
            double du = nx * u.x + nz * u.z - c, dv = nx * v.x + nz * v.z - c;
            if (du >= 0)
            {
              out.push_back(u);
            }
            if ((du < 0) != (dv < 0))
            {
              out.push_back({u.x + (v.x - u.x) * du / (du - dv), 0, u.z + (v.z - u.z) * du / (du - dv)});
            }
          }
          inner = out;
        }
        if (inner.size() >= 3 && signedArea(inner) > largest)
        {
          largest = signedArea(inner);
          precinct = inner;
        }
      }
      if (precinct.empty())
      {
        r.rejections.push_back("compound " + std::to_string(d.id) + " (" + districtUseName(d.use) + "): no precinct survives the street margin");
        continue;
      }
      generator::CompoundBrief brief;
      brief.id = d.id;
      brief.seed = site.id;
      brief.kind = kind;
      brief.precinct = precinct;
      brief.approach = d.access.size() >= 2 ? d.access[d.access.size() - 2] : vertexMean(precinct);
      brief.foundation = r.terraceLevel;
      generator::CompoundPlan compound;
      std::string compoundError;
      if (generator::planCompound(brief, r.terrain, compound, compoundLedger, &compoundError))
      {
        r.compounds.push_back(std::move(compound));
      }
      else
      {
        r.rejections.push_back("compound " + std::to_string(d.id) + " (" + districtUseName(d.use) + "): " + compoundError);
      }
    }
    for (const auto &entry : compoundLedger.entries())
    {
      r.rejections.push_back(entry.message);
    }
  }

  return true;
}

bool NeighbourhoodStage::run(FabricBuild &build, std::string *error)
{
  (void)error;

  // The phases were written against these names on one enormous stack. Binding
  // them here rather than rewriting three hundred lines of references is what
  // keeps this a move and not a rewrite - and the move is what makes the next
  // one possible.
  SettlementFabric &r = build.fabric;
  const SettlementFabricSettings &s = build.settings;
  const WorldSettlementSite &site = build.site;
  const CityTerritory &territory = build.territory;
  const CitySettings &city = build.city;
  const uint64_t plazaCell = build.plazaCell;
  auto &accepted = build.acceptedJetty;

  // The untreated ground, as opposed to r.terrain which is the terrace the
  // streets and houses were levelled onto. Farm buildings stand on the real
  // ground, which is why the two are kept apart.
  const auto &base = site.brief.terrain;

  (void)s;
  (void)site;
  (void)territory;
  (void)city;
  (void)plazaCell;
  (void)accepted;
  (void)base;

  // Neighbourhood ledgers: what the director actually placed in each district.
  {
    std::map<uint64_t, NeighbourhoodLedger> ledgers;
    for (const auto &d : r.layout.districts)
    {
      NeighbourhoodLedger n;
      n.district = d.id;
      n.use = d.use;
      n.area = d.area;
      ledgers[d.id] = n;
    }
    std::map<uint64_t, uint64_t> objectDistrict;
    for (const auto &o : r.layout.objects)
    {
      if (o.kind != CityObjectKind::House && o.kind != CityObjectKind::Workshop)
      {
        continue;
      }
      for (const auto &d : r.layout.districts)
      {
        for (const auto &piece : d.pieces)
        {
          if (strictlyInsideConvex(piece, o.entrance) || onRing(piece, o.entrance, 1e-6))
          {
            objectDistrict[o.id] = d.id;
            ledgers[d.id].houses++;
            if (accepted.count(o.id))
            {
              ledgers[d.id].jettied++;
            }
          }
        }
      }
    }
    std::map<uint64_t, uint64_t> buildingDistrict;
    for (const auto &b : r.population.buildings)
    {
      if (objectDistrict.count(b.site))
      {
        buildingDistrict[b.id] = objectDistrict[b.site];
        auto &n = ledgers[objectDistrict[b.site]];
        n.buildings++;
        if (b.kind == ProgrammeKind::Shop)
        {
          n.shops++;
        }
      }
    }
    for (const auto &tenancy : r.population.tenancies)
    {
      if (buildingDistrict.count(tenancy.building))
      {
        ledgers[buildingDistrict[tenancy.building]].residents += tenancy.residents;
      }
    }
    for (const auto &job : r.population.employment)
    {
      if (buildingDistrict.count(job.building))
      {
        ledgers[buildingDistrict[job.building]].workers += job.workers;
      }
    }
    for (auto &[id, n] : ledgers)
    {
      r.neighbourhoods.push_back(n);
    }
  }

  return true;
}

bool InstanceStage::run(FabricBuild &build, std::string *error)
{
  (void)error;

  // The phases were written against these names on one enormous stack. Binding
  // them here rather than rewriting three hundred lines of references is what
  // keeps this a move and not a rewrite - and the move is what makes the next
  // one possible.
  SettlementFabric &r = build.fabric;
  const SettlementFabricSettings &s = build.settings;
  const WorldSettlementSite &site = build.site;
  const CityTerritory &territory = build.territory;
  const CitySettings &city = build.city;
  const uint64_t plazaCell = build.plazaCell;
  auto &accepted = build.acceptedJetty;

  // The untreated ground, as opposed to r.terrain which is the terrace the
  // streets and houses were levelled onto. Farm buildings stand on the real
  // ground, which is why the two are kept apart.
  const auto &base = site.brief.terrain;

  (void)s;
  (void)site;
  (void)territory;
  (void)city;
  (void)plazaCell;
  (void)accepted;
  (void)base;

  // Instances: city massing on the terrace, farm buildings on the real ground.
  std::string meshError;
  if (!meshCityInstances(r.layout, r.terrain, r.instances, &meshError))
  {
    r.rejections.push_back("city instance meshing failed: " + meshError);
  }
  std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> index;
  for (const auto &t : r.instances)
  {
    appendTriangle(r.structures, r.structuralMaterials, index, t.points, t.material);
  }
  if (s.meshFarmBuildings)
  {
    for (const auto &holding : territory.holdings)
    {
      for (const auto &b : holding.buildings)
      {
        Poly p = b.footprint;
        if (p.size() < 3)
        {
          continue;
        }
        if (signedArea(p) < 0)
        {
          std::reverse(p.begin(), p.end());
        }
        Poly bottom = p, top = p;
        bool seated = true;
        for (size_t i = 0; i < p.size(); ++i)
        {
          bottom[i].y = base(p[i].x, p[i].z);
          top[i].y = b.foundation + b.height;
          seated &= std::isfinite(bottom[i].y) && bottom[i].y <= b.foundation + 1e-6;
        }
        if (!seated)
        {
          r.rejections.push_back("farm building " + std::to_string(b.id) + " is not seated on its foundation");
          continue;
        }
        size_t before = r.structures.triangles.size();
        for (size_t i = 0; i < p.size(); ++i)
        {
          size_t j = (i + 1) % p.size();
          appendTriangle(r.structures, r.structuralMaterials, index, {bottom[i], top[i], top[j]}, 12);
          appendTriangle(r.structures, r.structuralMaterials, index, {bottom[i], top[j], bottom[j]}, 12);
        }
        Position peak = vertexMean(p);
        double perimeter = 0;
        for (size_t i = 0; i < p.size(); ++i)
        {
          perimeter += length(p[i], p[(i + 1) % p.size()]);
        }
        peak.y = b.foundation + b.height + std::min(3., std::abs(signedArea(p)) / perimeter * .5);
        for (size_t i = 0; i < p.size(); ++i)
        {
          appendTriangle(r.structures, r.structuralMaterials, index, {top[i], peak, top[(i + 1) % p.size()]}, 13);
        }
        r.farmTriangles += uint32_t(r.structures.triangles.size() - before);
      }
    }
  }

  return true;
}

const std::vector<StagePtr> &fabricTailStages()
{
  static const std::vector<StagePtr> stages = {
    std::make_shared<PopulationStage>(),
    std::make_shared<CompoundStage>(),
    std::make_shared<NeighbourhoodStage>(),
    std::make_shared<InstanceStage>(),
  };

  return stages;
}

} // namespace procedural::world::settlement
