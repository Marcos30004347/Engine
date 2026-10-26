#include "SettlementGenerators.hpp"

#include <cmath>

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
    {{"settlement.runtime-settings", domain_}, typeid(SettlementRuntimeSettings), 1}};
  descriptor.writes = {{{"runtime.jobs", domain_}, typeid(RuntimeJobSet), 1}};

  return descriptor;
}

bool RuntimeJobGenerator::generate(const GenerationRequest &request, GenerationSnapshot &delta, Ledger &ledger, std::string *error)
{
  const auto *site = request.context.get<WorldSettlementSite>({"world.settlement-site", domain_});
  const auto *territory = request.context.get<CityTerritory>({"city.territory", domain_});
  const auto *fabric = request.context.get<SettlementFabric>({"settlement.fabric", domain_});
  const auto *settings = request.context.get<SettlementRuntimeSettings>({"settlement.runtime-settings", domain_});

  if (site == nullptr || territory == nullptr || fabric == nullptr || settings == nullptr)
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

        if (!rewrite::composeSettlementRuntimeInput(*site, *territory, *fabric, tileSettings, job.input, error))
        {
          return false;
        }

        jobs.jobs.push_back(std::move(job));
      }
    }

    ledger.add(LedgerKind::Repaired, site->cell, "hinterland published as " + std::to_string(jobs.jobs.size()) + " tile jobs; seam vertices are not shared between jobs");
  }
  else
  {
    RuntimeJob job;

    job.name = "core";

    if (!rewrite::composeSettlementRuntimeInput(*site, *territory, *fabric, *settings, job.input, error))
    {
      return false;
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

std::vector<GenerationTask> settlementGeneratorTasks(uint64_t domain, std::shared_ptr<Ledger> ledger)
{
  std::vector<GenerationTask> tasks;

  tasks.push_back(toTask(std::make_shared<SettlementSiteGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<TerritoryBriefGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<TerritoryGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<FabricGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<RuntimeJobGenerator>(domain), ledger));
  tasks.push_back(toTask(std::make_shared<RuntimeBuildGenerator>(domain), ledger));

  return tasks;
}

} // namespace procedural::world::generator
