#include "../WorldCityOccupiedStreets.hpp"
#include "../WorldCitySurfaces.hpp"
#include "../WorldCityWaterways.hpp"
#include "../WorldCityHistoricalPlacement.hpp"
#include "../WorldCityHistoricalGrowth.hpp"
#include "SettlementGenerators.hpp"
#include "procedural/world/WorldCityDistrictUpgrade.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>

namespace procedural::world::generator {

namespace {

using rewrite::GenerationPort;

LedgerKind classify(const std::string &message)
{
  if (message.find("omitted") != std::string::npos || message.find("released") != std::string::npos)
  {
    return LedgerKind::Omitted;
  }

  if (message.find("budget") != std::string::npos || message.find("exceed") != std::string::npos)
  {
    return LedgerKind::Capacity;
  }

  if (message.find("accepted only") != std::string::npos)
  {
    return LedgerKind::Repaired;
  }

  return LedgerKind::Rejected;
}

} // namespace

SettlementSiteGenerator::SettlementSiteGenerator(uint64_t domain) : Generator("world.settlement-site", domain, "world.site-brief", "world.settlement-site", GenerationScope::Neighbourhood)
{
}

bool SettlementSiteGenerator::run(const WorldSiteGenerationBrief &input, WorldSettlementSite &output, Ledger &ledger, std::string *error)
{
  if (!input.world)
  {
    if (error != nullptr)
    {
      *error = "site brief lacks a generated world";
    }

    return false;
  }

  if (!rewrite::selectWorldSettlementSite(*input.world, input.settings, output, error))
  {
    return false;
  }

  for (const auto &candidate : output.candidates)
  {
    if (!candidate.accepted)
    {
      ledger.add(LedgerKind::Rejected, candidate.cell, "site candidate " + std::to_string(candidate.cell) + ": " + candidate.rejection);
    }
  }

  for (const auto &message : output.rejections)
  {
    ledger.add(classify(message), output.cell, message);
  }

  return true;
}

TerritoryBriefGenerator::TerritoryBriefGenerator(uint64_t domain) : Generator("settlement.territory-brief", domain, "world.settlement-site", "city.territory-brief", GenerationScope::Neighbourhood)
{
}

bool TerritoryBriefGenerator::run(const WorldSettlementSite &input, TerritoryGenerationBrief &output, Ledger &, std::string *error)
{
  if (!input.brief.terrain || !input.brief.suitability.fingerprint())
  {
    if (error != nullptr)
    {
      *error = "settlement site carries no composed territory brief";
    }

    return false;
  }

  output = input.brief;

  return true;
}

TerritoryGenerator::TerritoryGenerator(uint64_t domain) : Generator("city.territory", domain, "city.territory-brief", "city.territory", GenerationScope::Neighbourhood)
{
}

bool TerritoryGenerator::run(const TerritoryGenerationBrief &input, CityTerritory &output, Ledger &ledger, std::string *error)
{
  if (!rewrite::generateCityTerritory(input.input, input.suitability, input.settings, input.terrain, output, error))
  {
    return false;
  }

  for (const auto &omission : output.regions.omissions)
  {
    ledger.add(LedgerKind::Omitted, omission.request, "district request " + std::to_string(omission.request) + ": " + omission.reason);
  }

  for (const auto &message : output.rural.rejections)
  {
    ledger.add(classify(message), 0, message);
  }

  for (const auto &message : output.rejections)
  {
    ledger.add(classify(message), 0, message);
  }

  return true;
}

FabricGenerator::FabricGenerator(uint64_t domain) : domain_(domain)
{
}

GenerationDescriptor FabricGenerator::descriptor() const
{
  GenerationDescriptor descriptor;

  descriptor.id = "settlement.fabric";
  descriptor.domain = domain_;
  descriptor.scope = GenerationScope::Neighbourhood;
  descriptor.reads = {
    {{"world.settlement-site", domain_}, typeid(WorldSettlementSite), 1}, {{"city.territory", domain_}, typeid(CityTerritory), 1}, {{"settlement.fabric-settings", domain_}, typeid(SettlementFabricSettings), 1}};
  descriptor.writes = {{{"settlement.fabric", domain_}, typeid(SettlementFabric), 1}};

  return descriptor;
}

bool FabricGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *site = request.context.get<WorldSettlementSite>({"world.settlement-site", domain_});
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *settings = request.context.get<SettlementFabricSettings>({"settlement.fabric-settings", domain_});

  if (site == nullptr || territory == nullptr || settings == nullptr)
  {
    if (error != nullptr)
    {
      *error = "fabric generator is missing its site, territory or settings";
    }

    return false;
  }

  SettlementFabric fabric;

  if (!rewrite::composeSettlementFabric(*site, *territory, *settings, fabric, error))
  {
    return false;
  }

  for (const auto &message : fabric.rejections)
  {
    ledger.add(classify(message), site->cell, message);
  }

  delta.emplace(GenerationKey{"settlement.fabric", domain_}, GenerationProduct{std::move(fabric), 1});

  return true;
}

CitySkeletonGenerator::CitySkeletonGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CitySkeletonGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.skeleton";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"city.territory", domain_}, typeid(CityTerritory), 1}, {{"settlement.fabric", domain_}, typeid(SettlementFabric), 1}};
  d.writes = {{{"city.skeleton", domain_}, typeid(rewrite::CitySkeleton), 1}};
  return d;
}

bool CitySkeletonGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &, std::string *error)
{
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *fabric = request.context.get<SettlementFabric>({"settlement.fabric", domain_});
  if (!territory || !fabric)
  {
    if (error) *error = "city skeleton generator is missing territory or fabric";
    return false;
  }
  rewrite::CitySkeleton skeleton;
  if (!rewrite::composeCitySkeleton(*territory, nullptr, *fabric, skeleton, error))
    return false;
  delta.emplace(GenerationKey{"city.skeleton", domain_}, GenerationProduct{std::move(skeleton), 1});
  return true;
}

CityReliefGenerator::CityReliefGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityReliefGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.relief";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"city.planned-relief", domain_}, typeid(rewrite::CityReliefPlan), 1}};
  d.writes = {{{"city.relief", domain_}, typeid(rewrite::CityReliefPlan), 1}};
  return d;
}

bool CityReliefGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &, std::string *error)
{
  const auto *relief=request.context.get<rewrite::CityReliefPlan>({"city.planned-relief",domain_});
  if(!relief){if(error)*error="city relief is missing its canonical planned relief";return false;}
  delta.emplace(GenerationKey{"city.relief",domain_},GenerationProduct{*relief,1});
  return true;
}

CityExteriorGenerator::CityExteriorGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityExteriorGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.exterior";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {
      {{"world.settlement-site", domain_}, typeid(WorldSettlementSite), 1},
      {{"city.territory", domain_}, typeid(CityTerritory), 1},
      {{"city.skeleton", domain_}, typeid(rewrite::CitySkeleton), 1}};
  d.writes = {{{"city.exterior", domain_}, typeid(rewrite::CityExteriorAllocation), 1}, {{"city.skeleton-complete", domain_}, typeid(rewrite::CitySkeleton), 1}};
  return d;
}

bool CityExteriorGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *site = request.context.get<WorldSettlementSite>({"world.settlement-site", domain_});
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *base = request.context.get<rewrite::CitySkeleton>({"city.skeleton", domain_});
  if (!site || !territory || !base)
  {
    if (error) *error = "city exterior generator is missing site, territory or skeleton";
    return false;
  }
  rewrite::CityExteriorAllocation exterior;
  rewrite::CityExteriorSettings settings;
  settings.maximumAccessDistance = std::max(site->bounds.maxX - site->bounds.minX, site->bounds.maxZ - site->bounds.minZ);
  if (!rewrite::allocateCityExterior(*site, *territory, *base, settings, exterior, error))
    return false;
  rewrite::CitySkeleton complete = *base;
  if (!rewrite::appendCityExterior(exterior, complete, error))
    return false;
  if (exterior.anonymousArea > 1e-6)
    ledger.add(LedgerKind::Rejected, site->cell, "city exterior left anonymous area");
  delta.emplace(GenerationKey{"city.exterior", domain_}, GenerationProduct{std::move(exterior), 1});
  delta.emplace(GenerationKey{"city.skeleton-complete", domain_}, GenerationProduct{std::move(complete), 1});
  return true;
}

CityAgricultureGenerator::CityAgricultureGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityAgricultureGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.agriculture";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"world.settlement-site", domain_}, typeid(WorldSettlementSite), 1},
             {{"city.territory", domain_}, typeid(CityTerritory), 1},
             {{"city.skeleton-complete", domain_}, typeid(rewrite::CitySkeleton), 1}};
  d.writes = {{{"city.agriculture", domain_}, typeid(rewrite::CityAgriculturePlan), 1}};
  return d;
}

bool CityAgricultureGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *site = request.context.get<WorldSettlementSite>({"world.settlement-site", domain_});
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *skeleton = request.context.get<rewrite::CitySkeleton>({"city.skeleton-complete", domain_});
  if (!site || !territory || !skeleton)
  {
    if (error) *error = "city agriculture generator is missing site, territory or city skeleton";
    return false;
  }
  rewrite::CityAgricultureSettings settings;
  settings.population = std::max(1u, site->demand.population);
  settings.availableWorkers = std::max(1u, site->demand.workers);
  rewrite::CityAgriculturePlan plan;
  if (!rewrite::planCityAgriculture(*territory, *skeleton, settings, plan, error))
    return false;
  if (plan.foodShortfall > 0)
    ledger.add(LedgerKind::Capacity, site->cell, "local agriculture shortfall " + std::to_string(plan.foodShortfall) + " annual food units");
  delta.emplace(GenerationKey{"city.agriculture", domain_}, GenerationProduct{std::move(plan), 1});
  return true;
}

CityCastleGenerator::CityCastleGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityCastleGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.castles";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"settlement.fabric", domain_}, typeid(SettlementFabric), 1},
             {{"city.skeleton-complete", domain_}, typeid(rewrite::CitySkeleton), 1}};
  d.writes = {{{"city.castles", domain_}, typeid(rewrite::CityCastlePlan), 1}};
  return d;
}

bool CityCastleGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &, std::string *error)
{
  const auto *fabric = request.context.get<SettlementFabric>({"settlement.fabric", domain_});
  const auto *skeleton = request.context.get<rewrite::CitySkeleton>({"city.skeleton-complete", domain_});
  if (!fabric || !skeleton)
  {
    if (error) *error = "city castle generator is missing fabric or city skeleton";
    return false;
  }
  rewrite::CityCastlePlan plan;
  if (!rewrite::composeCityCastles(*fabric, *skeleton, {}, plan, error))
    return false;
  delta.emplace(GenerationKey{"city.castles", domain_}, GenerationProduct{std::move(plan), 1});
  return true;
}

CityGrowthGenerator::CityGrowthGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityGrowthGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.growth";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"settlement.fabric", domain_}, typeid(SettlementFabric), 1},
             {{"city.skeleton-complete", domain_}, typeid(rewrite::CitySkeleton), 1},
             {{"city.castles", domain_}, typeid(rewrite::CityCastlePlan), 1}};
  d.writes = {{{"city.growth", domain_}, typeid(rewrite::CityGrowthPlan), 1}};
  return d;
}

bool CityGrowthGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *fabric = request.context.get<SettlementFabric>({"settlement.fabric", domain_});
  const auto *skeleton = request.context.get<rewrite::CitySkeleton>({"city.skeleton-complete", domain_});
  const auto *castles = request.context.get<rewrite::CityCastlePlan>({"city.castles", domain_});
  if (!fabric || !skeleton || !castles)
  {
    if (error) *error = "city growth generator is missing fabric, skeleton, or castles";
    return false;
  }
  rewrite::CityGrowthPlan growth;
  if (!rewrite::planCityGrowth(*fabric, *skeleton, *castles, {}, growth, error))
    return false;
  if (!growth.historical && growth.walls.size() != 1)
    ledger.add(LedgerKind::Rejected, 0, "a completed city must publish exactly one main curtain wall");
  delta.emplace(GenerationKey{"city.growth", domain_}, GenerationProduct{std::move(growth), 1});
  return true;
}

CityCompletionGenerator::CityCompletionGenerator(uint64_t domain) : domain_(domain) {}

GenerationDescriptor CityCompletionGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.complete";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"settlement.fabric", domain_}, typeid(SettlementFabric), 1},
             {{"settlement.fabric-settings", domain_}, typeid(SettlementFabricSettings), 1},{{"city.skeleton-complete", domain_}, typeid(rewrite::CitySkeleton), 1},
             {{"city.exterior", domain_}, typeid(rewrite::CityExteriorAllocation), 1},
             {{"city.growth", domain_}, typeid(rewrite::CityGrowthPlan), 1},
             {{"city.castles", domain_}, typeid(rewrite::CityCastlePlan), 1}};
  d.writes = {{{"city.complete", domain_}, typeid(rewrite::CityCompletionPlan), 1},
              {{"city.planned-relief", domain_}, typeid(rewrite::CityReliefPlan), 1}};
  return d;
}

bool CityCompletionGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *fabric=request.context.get<SettlementFabric>({"settlement.fabric",domain_});
  const auto *settings=request.context.get<SettlementFabricSettings>({"settlement.fabric-settings",domain_});
  const auto *skeleton = request.context.get<rewrite::CitySkeleton>({"city.skeleton-complete", domain_});
  const auto *exterior = request.context.get<rewrite::CityExteriorAllocation>({"city.exterior", domain_});
  const auto *growth = request.context.get<rewrite::CityGrowthPlan>({"city.growth", domain_});
  const auto *castles = request.context.get<rewrite::CityCastlePlan>({"city.castles", domain_});
  if (!fabric || !settings || !skeleton || !exterior || !growth || !castles)
  {
    if (error) *error = "city completion generator is missing a canonical input";
    return false;
  }
  rewrite::CityCompletionPlan plan;
  if (!rewrite::completeCityPlan(*skeleton, *exterior, *growth, *castles, {}, plan, error))
    return false;
  if (plan.suburbs.size() != std::count_if(growth->zones.begin(), growth->zones.end(), [](const auto &z) { return z.kind == rewrite::CityGrowthZoneKind::Suburb; }))
    ledger.add(LedgerKind::Rejected, 0, "not every suburb received canonical road topology");
  rewrite::CityReliefPlan relief;
  const bool hill=!fabric->landforms.cells.empty() || settings->raisedDistricts || settings->relief.amplitudeMetres>0 || !settings->terraceWholeTown;
  if(hill && fabric->terrain) {
    auto reliefSettings=settings->reliefPlan;
    if(!fabric->landforms.cells.empty())reliefSettings.terrainFacetMetres=0;
    if(!rewrite::planCityRelief(plan.skeleton,fabric->terrain,reliefSettings,relief,error))return false;
  }
  if(settings->raisedDistricts && fabric->terrain) {
    if(!relief.openGround)relief.ground=relief.openGround=fabric->terrain;
    std::set<uint64_t> occupiedBlocks;
    if(fabric->historicalBuildings) {
      auto demand=plan;
      if(!rewrite::allocateHistoricalCityLots(*fabric->historicalBuildings,demand,error,fabric->historicalEnclosures?&*fabric->historicalEnclosures:nullptr,fabric->terrain,fabric->growthCentre?&*fabric->growthCentre:nullptr))return false;
      std::set<uint64_t> selected;
      for(const auto &lot:demand.lots)if(lot.developed)selected.insert(lot.lot);
      for(const auto &region:demand.skeleton.regions)if(selected.count(region.id))occupiedBlocks.insert(region.parent);
    }
    rewrite::upgradeCityDistricts(plan,relief,settings->reliefPlan,settings->maximumRaisedDistricts,
                                 fabric->historicalBuildings?&occupiedBlocks:nullptr);
    ledger.add(LedgerKind::Repaired,0,"city base: "+std::to_string(relief.platforms.size())+" raised districts with reserved stair courts");
    std::fprintf(stderr,"city base: %zu raised districts with reserved stair courts\n",relief.platforms.size());
  }
  if(settings->stoneWaterways && fabric->terrain) {
    if(!relief.openGround)relief.openGround=relief.ground=fabric->terrain;
    rewrite::planCityWaterway(plan,relief);
    std::fprintf(stderr,"city waterways: %zu stone-lined reaches\n",plan.waterways.size());
  }
  if(fabric->historicalBuildings && !rewrite::allocateHistoricalCityLots(*fabric->historicalBuildings,plan,error,fabric->historicalEnclosures?&*fabric->historicalEnclosures:nullptr,fabric->terrain,fabric->growthCentre?&*fabric->growthCentre:nullptr))return false;
  const auto removedRoads=rewrite::pruneUnoccupiedCityStreets(plan,*fabric);
  // Occupancy can remove roads after completion; remove their sidewalks too.
  if(removedRoads && !rewrite::planCitySidewalks(plan,1.4,error))return false;
  std::fprintf(stderr,"city occupancy: removed %zu unused route chains\n",removedRoads);
  if (fabric->historicalBuildings && fabric->historicalEnclosures && !fabric->historicalEnclosures->empty()) {
    rewrite::CityGrowthPlan historical;
    if (!rewrite::projectHistoricalCityGrowth(*fabric->historicalBuildings,*fabric->historicalEnclosures,plan,*growth,historical,error))return false;
    for (const auto &why:historical.omissions)ledger.add(LedgerKind::Omitted,0,why);
    if (!rewrite::realizeCityGrowthWalls(historical,*castles,{},plan,error))return false;
    plan.historicalGrowth=std::make_shared<const rewrite::CityGrowthPlan>(std::move(historical));
  }
  // Excavation belongs only to retained streets. Otherwise an occupancy-pruned
  // route leaves an unexplained trench through undeveloped terrain.
  if(removedRoads && hill && fabric->terrain && relief.platforms.empty() && plan.waterways.empty()) {
    auto active=plan.skeleton;
    std::set<uint64_t> retained;
    for(const auto &route:plan.routes) {
      retained.insert(route.edges.begin(),route.edges.end());
      if(route.edges.empty())retained.insert(route.id);
    }
    active.roadEdges.erase(std::remove_if(active.roadEdges.begin(),active.roadEdges.end(),
      [&](const auto &edge){return !retained.count(edge.id);}),active.roadEdges.end());
    if(!active.roadEdges.empty()) {
      auto reliefSettings=settings->reliefPlan;
      if(!fabric->landforms.cells.empty())reliefSettings.terrainFacetMetres=0;
      rewrite::CityReliefPlan occupiedRelief;
      if(!rewrite::planCityRelief(active,fabric->terrain,reliefSettings,occupiedRelief,error))return false;
      relief=std::move(occupiedRelief);
    } else {
      relief=rewrite::CityReliefPlan{};
      relief.openGround=relief.ground=fabric->terrain;
    }
  }
  delta.emplace(GenerationKey{"city.planned-relief",domain_},GenerationProduct{std::move(relief),1});
  delta.emplace(GenerationKey{"city.complete", domain_}, GenerationProduct{std::move(plan), 1});
  return true;
}

CityModelGenerator::CityModelGenerator(uint64_t domain,rewrite::CityModelSettings settings) : settings_(settings),domain_(domain) {}

GenerationDescriptor CityModelGenerator::descriptor() const
{
  GenerationDescriptor d;
  d.id = "city.model";
  d.domain = domain_;
  d.scope = GenerationScope::Neighbourhood;
  d.reads = {{{"city.complete", domain_}, typeid(rewrite::CityCompletionPlan), 1},
             {{"settlement.fabric", domain_}, typeid(SettlementFabric), 1},
             {{"city.relief", domain_}, typeid(rewrite::CityReliefPlan), 1}};
  d.writes = {{{"city.model", domain_}, typeid(rewrite::CityModelPlan), 1}};
  return d;
}

bool CityModelGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *city=request.context.get<rewrite::CityCompletionPlan>({"city.complete",domain_});
  const auto *fabric=request.context.get<SettlementFabric>({"settlement.fabric",domain_});
  if (!city || !fabric)
  {
    if (error) *error="city model generator is missing completion or fabric";
    return false;
  }
  const auto *relief=request.context.get<rewrite::CityReliefPlan>({"city.relief",domain_});
  rewrite::CityModelPlan model;
  if (!rewrite::buildCityModel(*city,*fabric,settings_,model,error,relief)) return false;
  if (!model.compoundTriangles && !settings_.infrastructureOnly && !settings_.groundOnly)
    ledger.add(LedgerKind::Omitted,0,"city model has no realised compound dwelling mesh");
  delta.emplace(GenerationKey{"city.model",domain_},GenerationProduct{std::move(model),1});
  return true;
}

RuntimeJobGenerator::RuntimeJobGenerator(uint64_t domain) : domain_(domain)
{
}

GenerationDescriptor RuntimeJobGenerator::descriptor() const
{
  GenerationDescriptor descriptor;

  descriptor.id = "settlement.runtime-jobs";
  descriptor.domain = domain_;
  descriptor.scope = GenerationScope::Neighbourhood;
  descriptor.reads = {
    {{"world.settlement-site", domain_}, typeid(WorldSettlementSite), 1},
    {{"city.territory", domain_}, typeid(CityTerritory), 1},
    {{"settlement.fabric", domain_}, typeid(SettlementFabric), 1},
    {{"city.relief", domain_}, typeid(rewrite::CityReliefPlan), 1},
    {{"city.model", domain_}, typeid(rewrite::CityModelPlan), 1},
    {{"settlement.runtime-settings", domain_}, typeid(SettlementRuntimeSettings), 1}};
  descriptor.writes = {{{"runtime.jobs", domain_}, typeid(RuntimeJobSet), 1}};

  return descriptor;
}

bool RuntimeJobGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *site = request.context.get<WorldSettlementSite>({"world.settlement-site", domain_});
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *fabric = request.context.get<SettlementFabric>({"settlement.fabric", domain_});
  const auto *model = request.context.get<rewrite::CityModelPlan>({"city.model", domain_});
  const auto *settings = request.context.get<SettlementRuntimeSettings>({"settlement.runtime-settings", domain_});

  const auto *relief = request.context.get<rewrite::CityReliefPlan>({"city.relief", domain_});
  // The terrain tiles take the street-graded surface, not the terraced one:
  // see CityReliefPlan::openGround for why a terrain job cannot follow a
  // retaining band.
  const std::function<double(double, double)> *graded = nullptr;
  if(relief)graded=relief->accessGround?&relief->accessGround:relief->openGround?&relief->openGround:nullptr;

  if (site == nullptr || territory == nullptr || fabric == nullptr || model == nullptr || settings == nullptr)
  {
    if (error != nullptr)
    {
      *error = "runtime job generator is missing an input";
    }

    return false;
  }

  RuntimeJobSet jobs;

  if (settings->domain == rewrite::SettlementRuntimeDomain::Full)
  {
    // Batching policy: one job per tile of the hinterland domain. Tile
    // boundaries are the same lines in neighbouring jobs, but each job
    // triangulates its own boundary, so seam vertices are not shared.
    const double tile = settings->tileMetres;

    for (double z = 0; z < site->bounds.maxZ - 1e-6; z += tile)
    {
      for (double x = 0; x < site->bounds.maxX - 1e-6; x += tile)
      {
        SettlementRuntimeSettings tileSettings = *settings;

        tileSettings.domain = rewrite::SettlementRuntimeDomain::Tile;
        tileSettings.tileMinX = x;
        tileSettings.tileMinZ = z;
        tileSettings.tileMaxX = std::min(site->bounds.maxX, x + tile);
        tileSettings.tileMaxZ = std::min(site->bounds.maxZ, z + tile);

        RuntimeJob job;

        job.name = "tile-" + std::to_string(int(x / tile)) + "-" + std::to_string(int(z / tile));
        job.minX = tileSettings.tileMinX;
        job.minZ = tileSettings.tileMinZ;
        job.maxX = tileSettings.tileMaxX;
        job.maxZ = tileSettings.tileMaxZ;

        if (!rewrite::composeSettlementRuntimeInput(*site, *territory, *fabric, tileSettings, job.input, error, graded))
        {
          return false;
        }

        // The city model is an ordinary explicit structure producer. Clip it
        // into the same jobs as terrain so runtime, collision and previews all
        // consume the identical wall/building/road triangles.
        rewrite::appendCityModelTile(*model,job.minX,job.minZ,job.maxX,job.maxZ,
                                     job.input.structures,job.input.structuralMaterials);

        jobs.jobs.push_back(std::move(job));
      }
    }

    ledger.add(LedgerKind::Repaired, site->cell, "hinterland published as " + std::to_string(jobs.jobs.size()) + " tile jobs; seam vertices are not shared between jobs");
  }
  else
  {
    RuntimeJob job;

    job.name = "core";

    if (!rewrite::composeSettlementRuntimeInput(*site, *territory, *fabric, *settings, job.input, error, graded))
    {
      return false;
    }

    job.input.structures.vertices.insert(job.input.structures.vertices.end(),model->mesh.vertices.begin(),model->mesh.vertices.end());
    const uint32_t offset=uint32_t(job.input.structures.vertices.size()-model->mesh.vertices.size());
    for (size_t ti=0;ti<model->mesh.triangles.size();++ti)
    {
      auto t=model->mesh.triangles[ti]; for (auto &vi:t) vi+=offset;
      job.input.structures.triangles.push_back(t);
      job.input.structures.expectedNormals.push_back(model->mesh.expectedNormals[ti]);
      job.input.structuralMaterials.push_back(model->materials[ti]);
    }

    job.minX = job.input.map.minX;
    job.minZ = job.input.map.minZ;
    job.maxX = job.input.map.maxX;
    job.maxZ = job.input.map.maxZ;

    jobs.jobs.push_back(std::move(job));
  }

  delta.emplace(GenerationKey{"runtime.jobs", domain_}, GenerationProduct{std::move(jobs), 1});

  return true;
}

RuntimeBuildGenerator::RuntimeBuildGenerator(uint64_t domain) : domain_(domain)
{
}

GenerationDescriptor RuntimeBuildGenerator::descriptor() const
{
  GenerationDescriptor descriptor;

  descriptor.id = "runtime.jobs.build";
  descriptor.domain = domain_;
  descriptor.scope = GenerationScope::World;
  descriptor.reads = {{{"runtime.jobs", domain_}, typeid(RuntimeJobSet), 1}};
  descriptor.writes = {{{"runtime.results", domain_}, typeid(RuntimeJobResults), 1}};

  return descriptor;
}

bool RuntimeBuildGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *jobs = request.context.get<RuntimeJobSet>({"runtime.jobs", domain_});

  if (jobs == nullptr)
  {
    if (error != nullptr)
    {
      *error = "runtime build generator is missing its jobs";
    }

    return false;
  }

  RuntimeJobResults results;

  for (const auto &job : jobs->jobs)
  {
    if (request.cancelled && request.cancelled())
    {
      if (error != nullptr)
      {
        *error = "cancelled";
      }

      return false;
    }

    RuntimeJobResult result;

    result.name = job.name;
    result.minX = job.minX;
    result.minZ = job.minZ;
    result.maxX = job.maxX;
    result.maxZ = job.maxZ;
    result.built = rewrite::buildRuntimeWorld(job.input, nullptr, result.result, &result.error);

    if (result.built)
    {
      ++results.built;
    }
    else
    {
      ledger.add(LedgerKind::Capacity, 0, "runtime job " + job.name + ": " + result.error);
    }

    results.jobs.push_back(std::move(result));
  }

  if (results.built == 0)
  {
    if (error != nullptr)
    {
      *error = "no runtime job built";
    }

    return false;
  }

  delta.emplace(GenerationKey{"runtime.results", domain_}, GenerationProduct{std::move(results), 1});

  return true;
}

std::vector<GenerationTask> settlementGeneratorTasks(uint64_t domain, std::shared_ptr<Ledger> ledger,rewrite::CityModelSettings modelSettings)
{
  std::vector<GenerationTask> tasks;

  tasks.push_back(toTask(std::make_shared<SettlementSiteGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<TerritoryBriefGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<TerritoryGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<FabricGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CitySkeletonGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityExteriorGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityReliefGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityAgricultureGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityCastleGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityGrowthGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityCompletionGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<CityModelGenerator>(domain,modelSettings), ledger));
  tasks.push_back(toTask(std::make_shared<RuntimeJobGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<RuntimeBuildGenerator>(domain), ledger));

  return tasks;
}

} // namespace procedural::world::generator
