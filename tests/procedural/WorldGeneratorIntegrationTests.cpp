#include "procedural/world/WorldDomainGeneratorAdapters.hpp"
#include "procedural/world/WorldGenerationEdits.hpp"
#include "procedural/world/WorldNativeGenerator.hpp"
#include "procedural/world/WorldPatchGeneratorAdapters.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace procedural::world::rewrite;
int main(int argc, char **argv)
{
  int failed = 0, checks = 0;
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    ++checks;
    failed += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << " " << error << '\n';
    error.clear();
  };
  CityPlanGenerationBrief city;
  for (int z = 0; z < 3; ++z)
    for (int x = 0; x < 3; ++x)
    {
      CityPlanCell cell;
      cell.id = z * 3 + x + 1;
      cell.boundary = {{x * 20., 0, z * 20.}, {x * 20. + 20, 0, z * 20.}, {x * 20. + 20, 0, z * 20. + 20}, {x * 20., 0, z * 20. + 20}};
      city.cells.push_back(cell);
    }
  city.gates = {1};
  city.districts = {{1, DistrictUse::Residential, 400, 800, true}};
  GenerationSnapshot outputs;
  check(
      runGenerators({cityMasterPlanGenerator(1)}, {{{"city.plan-brief", 1}, {city, 1}}}, 7, outputs, {}, &error) && !std::any_cast<const CityMasterPlan &>(outputs.at({"city.master-plan", 1}).value).districts.empty(),
      "native city allocation through common interface");
  RuntimeBuildInput runtime;
  runtime.dependencies = {1, 1, 1, 1, 1};
  runtime.map.minX = 0;
  runtime.map.minZ = 0;
  runtime.map.maxX = 32;
  runtime.map.maxZ = 32;
  runtime.map.cutsX = {16};
  runtime.map.cutsZ = {16};
  runtime.terrain = [](double, double)
  {
    return 0.;
  };
  runtime.errors.resize(1);
  runtime.densities.resize(1);
  runtime.covers = {{0, {0}, false, false}};
  SurfaceContour ground;
  ground.id = 1;
  ground.loop = {{0, 0, 0}, {32, 0, 0}, {32, 0, 32}, {0, 0, 32}};
  runtime.producers = {{1, 0, {ground}}};
  check(runGenerators({runtimeWorldGenerator(1)}, {{{"runtime.brief", 1}, {runtime, 1}}}, 7, outputs, {}, &error), "native world builder executes through common interface");
  if (outputs.count({"runtime.candidate", 1}))
  {
    auto &assets = std::any_cast<const RuntimeBuildResult &>(outputs.at({"runtime.candidate", 1}).value).assets;
    WorldRuntimePublication publication;
    uint64_t ticket = 0;
    bool ok = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 1000000, ticket, &error);
    if (ok)
      for (uint32_t i = 0; i < assets.levels[0].world.tiles.size(); ++i)
        ok &= publication.complete(ticket, i, assets.dependencies, &error);
    auto snapshot = publication.acquire();
    RuntimeSurfaceHit hit;
    check(ok && snapshot && queryRuntimeSurface(*snapshot, 16, 16, 0, hit, &error) && hit.height == 0, "DAG world candidate publishes and serves real collision/surface queries");
  }
  // W0: actual-world settlement input adapter on a generated world.
  {
    procedural::world::WorldPipelineSettings settings;
    settings.brief.extentMetres = 12000;
    settings.brief.seed = 17;
    settings.brief.reliefMetres = 900;
    settings.brief.tags = {procedural::world::Tag::Coast, procedural::world::Tag::Lowland};
    settings.cellSpacingMetres = 400;
    settings.hierarchyLevels = 1;
    auto world = std::make_shared<procedural::world::GeneratedWorld>();
    bool generated = procedural::world::generateWorldDirected(settings, *world, {}, &error);
    check(generated, "actual 12 km world generates for settlement integration");
    WorldSiteSelectionSettings siteSettings;
    WorldSettlementSite site;
    bool selected = generated && selectWorldSettlementSite(*world, siteSettings, site, &error);
    check(selected, "W0 site selection composes a brief from the actual world");
    if (selected)
    {
      size_t rejected = 0;
      for (auto &c : site.candidates)
        rejected += !c.accepted;
      std::cout << "W0_SITE cell=" << site.cell << " region=\"" << site.regionName << "\" centre=(" << site.centre.x << "," << site.centre.z << ") elevation=" << site.centre.y << " candidates=" << site.candidates.size()
                << " rejected=" << rejected << " attempts=" << site.attempts << " planningCells=" << site.brief.input.cells.size() << " regions=" << site.regions.size() << " gates=" << site.brief.input.gateCells.size()
                << " sources=" << site.brief.input.ruralSources.size() << " districts=" << site.brief.input.districts.size() << " origin=(" << site.origin.x << "," << site.origin.z << ") size=" << site.bounds.maxX << "x"
                << site.bounds.maxZ << " openArea=" << site.openArea << " hinterlandArea=" << site.hinterlandArea << "\n";
      size_t shown = 0;
      for (auto &c : site.candidates)
        if (!c.accepted && shown++ < 6)
          std::cout << "  rejected cell " << c.cell << ": " << c.rejection << "\n";
      for (auto &r : site.rejections)
        std::cout << "  omission: " << r << "\n";
      double lo = 1e300, hi = -1e300;
      for (auto &cell : site.brief.input.cells)
      {
        double y = site.brief.terrain(cell.boundary[0].x, cell.boundary[0].z);
        lo = std::min(lo, y);
        hi = std::max(hi, y);
      }
      check(hi > lo, "terrain callback samples the actual surface rather than a flat default");
      check(!site.water(site.localCentre.x, site.localCentre.z), "site centre is not on explicit map water");
      bool waterSeen = false, waterExplicit = true;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land && n.site.x - site.origin.x > site.bounds.minX && n.site.x - site.origin.x < site.bounds.maxX && n.site.y - site.origin.z > site.bounds.minZ &&
            n.site.y - site.origin.z < site.bounds.maxZ)
        {
          waterSeen = true;
          waterExplicit &= site.water(n.site.x - site.origin.x, n.site.y - site.origin.z);
        }
      check(!waterSeen || waterExplicit, "explicit ocean/lake cells inside the domain read as water");
      WorldSettlementSite repeat;
      bool same = selectWorldSettlementSite(*world, siteSettings, repeat, &error) && repeat.id == site.id && repeat.cell == site.cell && repeat.candidates.size() == site.candidates.size() &&
                  repeat.brief.input.cells.size() == site.brief.input.cells.size() && repeat.regions.size() == site.regions.size() && repeat.brief.input.gateCells == site.brief.input.gateCells &&
                  repeat.brief.suitability.fingerprint() == site.brief.suitability.fingerprint();
      for (size_t i = 0; same && i < site.candidates.size(); ++i)
        same &= site.candidates[i].cell == repeat.candidates[i].cell && site.candidates[i].rejection == repeat.candidates[i].rejection;
      for (size_t i = 0; same && i < site.regions.size(); ++i)
        same &= site.regions[i].id == repeat.regions[i].id && site.regions[i].cover == repeat.regions[i].cover;
      for (size_t i = 0; same && i < site.brief.input.cells.size(); ++i)
        same &= site.brief.input.cells[i].id == repeat.brief.input.cells[i].id;
      check(same, "same seed yields the same site, candidate ledger, region and cell IDs");
      uint32_t waterCell = procedural::world::kInvalidNode;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land)
        {
          waterCell = n.id;
          break;
        }
      auto bad = siteSettings;
      bad.preferredCell = waterCell;
      WorldSettlementSite rejectedSite;
      bool refused = waterCell != procedural::world::kInvalidNode && !selectWorldSettlementSite(*world, bad, rejectedSite, &error) && !error.empty();
      std::cout << "  explicit water cell " << waterCell << " -> " << error << "\n";
      check(refused, "a preferred water cell is rejected with a visible reason");
      CityTerritory territory;
      bool composed = generateCityTerritory(site.brief.input, site.brief.suitability, site.brief.settings, site.brief.terrain, territory, &error);
      std::string territoryMessage = "W0 territory allocates districts and farmland on actual terrain: " + error;
      check(composed, territoryMessage.c_str());
      if (composed)
      {
        std::cout << "W0_TERRITORY districts=" << territory.regions.districts.size() << " omissions=" << territory.regions.omissions.size() << " urbanArea=" << territory.regions.assignedArea << " patches=" << territory.rural.patches.size()
                  << " cropArea=" << territory.cropArea << " farms=" << territory.holdings.size() << " contours=" << territory.surfaces.contours.size() << " eligibleArea=" << territory.rural.eligibleArea
                  << " ruralRejections=" << territory.rural.rejections.size() << "\n";
        for (auto &o : territory.regions.omissions)
          std::cout << "  district omission " << o.request << ": " << o.reason << "\n";
        size_t ruralShown = 0;
        for (auto &r : territory.rural.rejections)
          if (ruralShown++ < 40)
            std::cout << "  rural: " << r << "\n";
        {
          const auto &w = site.brief.suitability;
          const char *names[] = {"Wet", "Flood", "Cliff", "Talus", "Brink", "Steep", "Rough", "Inaccessible", "Ramp", "PoorSoil", "Saturated"};
          std::vector<size_t> histogram(11, 0);
          size_t eligible = 0, walkable = 0;
          for (auto &c : w.cells())
          {
            eligible += c.reasons == 0;
            walkable += c.walkable;
            for (int b = 0; b < 11; ++b)
              histogram[b] += (c.reasons >> b) & 1;
          }
          std::cout << "  suitability cells=" << w.cells().size() << " eligible=" << eligible << " walkable=" << walkable;
          for (int b = 0; b < 11; ++b)
            if (histogram[b])
              std::cout << " " << names[b] << "=" << histogram[b];
          std::cout << "\n";
          size_t regionShown = 0;
          for (auto &region : site.regions)
          {
            if (region.cover != RuralLandCover::Open || regionShown++ >= 8)
              continue;
            std::vector<size_t> h(11, 0);
            size_t cells = 0, blocked = 0, noAccess = 0;
            double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
            for (auto p : region.boundary)
            {
              x0 = std::min(x0, p.x);
              z0 = std::min(z0, p.z);
              x1 = std::max(x1, p.x);
              z1 = std::max(z1, p.z);
            }
            for (size_t i = 0; i < w.cells().size(); ++i)
            {
              auto c = w.cells()[i].center;
              if (c.x < x0 || c.x > x1 || c.z < z0 || c.z > z1)
                continue;
              ++cells;
              blocked += w.cells()[i].reasons != 0;
              noAccess += !territory.rural.catchments[i];
              for (int b = 0; b < 11; ++b)
                h[b] += (w.cells()[i].reasons >> b) & 1;
            }
            std::cout << "  region " << region.id << " cells=" << cells << " blocked=" << blocked << " noCatchment=" << noAccess;
            for (int b = 0; b < 11; ++b)
              if (h[b])
                std::cout << " " << names[b] << "=" << h[b];
            std::cout << "\n";
          }
        }
        check(!territory.regions.districts.empty(), "population-derived districts are allocated to actual planning cells");
      }
      // Fabric (terrace, streets, houses, residents) and W1 runtime publication.
      if (composed)
      {
        SettlementFabricSettings fabricSettings;
        SettlementFabric fabric;
        bool fabricOK = composeSettlementFabric(site, territory, fabricSettings, fabric, &error);
        std::string fabricMessage = "settlement fabric composes on the actual territory: " + error;
        check(fabricOK, fabricMessage.c_str());
        if (fabricOK)
        {
          std::cout << "W1_FABRIC terrace=" << (fabric.terraceAccepted ? "accepted" : "rejected") << " level=" << fabric.terraceLevel << " step=" << fabric.terraceStep << " polygon=" << fabric.polygon.outer.size() << " holes=" << fabric.polygon.holes.size()
                    << " routeCells=" << fabric.routeCells << " arteries=" << fabric.arteries.size() << " gates=" << fabric.gates.size() << " layout=" << (fabric.layoutAccepted ? "accepted" : "rejected") << " streets=" << fabric.layout.streets.size()
                    << " objects=" << fabric.layout.objects.size() << " populated=" << fabric.populated << " housed=" << fabric.population.housed << " unhoused=" << fabric.population.unhoused << " buildings=" << fabric.population.buildings.size()
                    << " structures=" << fabric.structures.triangles.size() << " farmTriangles=" << fabric.farmTriangles << "\n";
          for (auto &r : fabric.rejections)
            std::cout << "  fabric: " << r << "\n";
          {
            size_t vertices = 0, cityVertices = 0, houses = 0, gardens = 0, gaps = 0;
            for (auto &c : territory.surfaces.contours)
              vertices += c.loop.size();
            for (auto &c : fabric.layout.contours)
              cityVertices += c.loop.size();
            for (auto &o : fabric.layout.objects)
              o.kind == CityObjectKind::Garden ? ++gardens : o.kind == CityObjectKind::RejectedGap ? ++gaps : ++houses;
            std::cout << "  territory contours=" << territory.surfaces.contours.size() << " vertices=" << vertices << " city contours=" << fabric.layout.contours.size() << " vertices=" << cityVertices << " houses=" << houses << " gardens=" << gardens
                      << " gaps=" << gaps << " polygonArea=" << [&] { double a = 0; auto &p = fabric.polygon.outer; for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x; return a * .5; }() << "\n";
            for (auto &c : territory.surfaces.contours)
              if (c.loop.size() < 3 || c.loop.size() > 256)
                std::cout << "  bad contour id=" << c.id << " material=" << c.material << " role=" << int(c.role) << " vertices=" << c.loop.size() << "\n";
            for (int variant = 0; variant < 5 && !fabric.layoutAccepted; ++variant)
            {
              CitySettings probe = fabricSettings.city;
              probe.seed = site.id ^ 0x9E3779B97F4A7C15ULL;
              probe.emitBuildings = false;
              probe.streetCurvature = 0;
              if (variant >= 1)
                probe.siteSpacing = 45;
              if (variant >= 3)
                probe.maximumSites = 24;
              std::vector<CityStreet> arteries = fabric.arteries;
              if (variant == 2 || variant == 4)
              {
                // Gate tree only: root -> each gate, plus the gate approaches.
                arteries.clear();
                std::set<std::pair<std::pair<long long, long long>, std::pair<long long, long long>>> seen;
                uint64_t id = 5000;
                auto key = [](Position p) { return std::make_pair((long long)std::llround(p.x * 1e6), (long long)std::llround(p.z * 1e6)); };
                for (auto &route : territory.regions.gateConnections)
                  for (size_t i = 1; i < route.size(); ++i)
                  {
                    auto k = std::minmax(key(route[i - 1]), key(route[i]));
                    if (std::hypot(route[i].x - route[i - 1].x, route[i].z - route[i - 1].z) >= 1 && seen.insert(k).second)
                      arteries.push_back({++id, route[i - 1], route[i], 2.5, true});
                  }
                for (auto &a : fabric.arteries)
                  for (auto &g : fabric.gates)
                    if (g.externalRoad == a.id)
                      arteries.push_back(a);
              }
              probe.districts.clear();
              for (auto &d : territory.regions.districts)
                if (std::find(fabric.releasedCompounds.begin(), fabric.releasedCompounds.end(), d.id) == fabric.releasedCompounds.end())
                  probe.districts.push_back(d);
              CityLayout probeLayout;
              std::string probeError;
              bool ok = generateCityLayout(fabric.polygon, fabric.reservations, arteries, fabric.gates, probe, fabric.terrain, probeLayout, &probeError);
              std::cout << "  layout probe " << variant << " arteries=" << arteries.size() << " spacing=" << probe.siteSpacing << " maxSites=" << probe.maximumSites << ": " << (ok ? "ok" : probeError) << " streets=" << probeLayout.streets.size() << " contours=" << probeLayout.contours.size()
                        << " wards=" << probeLayout.wards.size() << " objects=" << probeLayout.objects.size() << " sites=" << probeLayout.sites.size() << "\n";
            }
          }
          check(fabric.terraceAccepted && fabric.layoutAccepted && !fabric.layout.objects.empty(), "village streets and houses are laid out on the bounded terrace");
          check(fabric.populated && fabric.population.housed > 0, "residents are housed through the director with real terrain/water eligibility");
          RuntimeBuildInput runtimeInput;
          SettlementRuntimeSettings runtimeSettings;
          bool briefOK = composeSettlementRuntimeInput(site, territory, fabric, runtimeSettings, runtimeInput, &error);
          std::string briefMessage = "W1 runtime brief composes from site, territory and fabric: " + error;
          check(briefOK, briefMessage.c_str());
          RuntimeBuildResult built;
          bool buildOK = briefOK && buildRuntimeWorld(runtimeInput, nullptr, built, &error);
          std::string buildMessage = "W1 actual runtime world builds from the settlement products: " + error;
          check(buildOK, buildMessage.c_str());
          if (buildOK)
          {
            const auto &assets = built.assets;
            std::cout << "W1_RUNTIME tiles=" << assets.levels[0].world.tiles.size() << " faces=" << assets.levels[0].world.map.faces.size() << " triangles=" << assets.levels[0].world.mesh.triangles.size() << " structures=" << assets.structures.triangles.size()
                      << " covers=" << assets.coverDefinitions.size() << " changedTiles=" << built.changedTiles.size() << "\n";
            WorldRuntimePublication publication;
            uint64_t ticket = 0;
            bool published = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 4000000, ticket, &error);
            for (uint32_t i = 0; published && i < assets.levels[0].world.tiles.size(); ++i)
              published &= publication.complete(ticket, i, assets.dependencies, &error);
            auto snapshot = publication.acquire();
            RuntimeSurfaceHit street, field;
            Position streetPoint = fabric.layout.streets.front().path.front();
            Position fieldPoint;
            for (auto &f : territory.fields)
              if (!f.fields.empty())
              {
                for (auto v : f.fields.front().boundary)
                {
                  fieldPoint.x += v.x / f.fields.front().boundary.size();
                  fieldPoint.z += v.z / f.fields.front().boundary.size();
                }
                break;
              }
            bool streetOK = published && snapshot && queryRuntimeSurface(*snapshot, streetPoint.x, streetPoint.z, 0, street, &error) && std::abs(street.height - fabric.terraceLevel) < 1e-3;
            bool fieldOK = published && snapshot && queryRuntimeSurface(*snapshot, fieldPoint.x, fieldPoint.z, 0, field, &error) && std::abs(field.height - site.brief.terrain(fieldPoint.x, fieldPoint.z)) < .5;
            std::cout << "  street query " << street.height << " vs terrace " << fabric.terraceLevel << "; field query " << field.height << " vs terrain " << site.brief.terrain(fieldPoint.x, fieldPoint.z) << " " << error << "\n";
            check(streetOK, "published street surface agrees with the terrace the houses were laid out on");
            check(fieldOK, "published field surface agrees with the actual world terrain");
          }
        }
      }
      GenerationSnapshot chain;
      WorldSiteGenerationBrief brief{world, siteSettings};
      SettlementFabricSettings chainFabric;
      SettlementRuntimeSettings chainRuntime;
      bool ran = runGenerators(
          {worldSettlementSiteGenerator(1), settlementTerritoryBriefGenerator(1), cityTerritoryGenerator(1), settlementFabricGenerator(1), settlementRuntimeBriefGenerator(1), runtimeWorldGenerator(1)},
          {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {chainFabric, 1}}, {{"settlement.runtime-settings", 1}, {chainRuntime, 1}}},
          17,
          chain,
          {},
          &error);
      check(ran && chain.count({"runtime.candidate", 1}), "site -> territory -> fabric -> runtime chain executes through the typed director");
    }
  }
  std::vector<GenerationTask> catalogue{
    landPartitionGenerator(1),
    coastDetailGenerator(1),
    riverNetworkGenerator(1),
    watershedGenerator(1),
    channelGenerator(1),
    roadNetworkGenerator(1),
    roadEarthworkGenerator(1),
    cityDemandGenerator(1),
    cityMasterPlanGenerator(1),
    cityLayoutGenerator(1),
    ruralPatchesGenerator(1),
    cityTerritoryGenerator(1),
    settlementProgrammeGenerator(1),
    runtimeWorldGenerator(1),
    farmFieldsGenerator(1),
    dwellingPlanGenerator(1),
    dwellingMeshGenerator(1),
    worldSettlementSiteGenerator(1),
    settlementTerritoryBriefGenerator(1),
    settlementFabricGenerator(1),
    settlementRuntimeBriefGenerator(1)};
  bool valid = true;
  for (auto &task : catalogue)
    valid &= !task.descriptor.id.empty() && !task.descriptor.reads.empty() && task.descriptor.writes.size() == 1;
  check(valid && catalogue.size() == 21, "all twenty-one native domain adapters expose typed contracts");
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    auto write = [&](const std::vector<GenerationTask> &tasks)
    {
      file << "[";
      bool first = true;
      for (auto &task : tasks)
      {
        if (!first)
          file << ",";
        first = false;
        file << "{\"id\":" << std::quoted(task.descriptor.id) << ",\"domain\":" << task.descriptor.domain << ",\"parallelSafe\":" << (task.parallelSafe ? "true" : "false");
        for (auto field : {false, true})
        {
          file << (field ? ",\"writes\":[" : ",\"reads\":[");
          bool initial = true;
          for (auto &port : field ? task.descriptor.writes : task.descriptor.reads)
          {
            if (!initial)
              file << ",";
            initial = false;
            file << "{\"product\":" << std::quoted(port.key.product) << ",\"schema\":" << port.schema << "}";
          }
          file << "]";
        }
        file << "}";
      }
      file << "]";
    };
    file << "{\"schema\":1,\"activeMapDAG\":";
    write(procedural::world::worldPipelineGenerators());
    file << ",\"nativeAdapters\":";
    write(catalogue);
    file << "}\n";
  }
  std::cout << checks << " checks, " << failed << " failures\n";
  return failed ? 1 : 0;
}
