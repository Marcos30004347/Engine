#include "json.hpp"
#include "procedural/world/WorldCityAgriculture.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;

Poly rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}

json poly(const Poly &p)
{
  json a = json::array();
  for (const auto &v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
}

int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const std::string &message)
  {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
  };
  CityTerritory territory;
  territory.revision = 4;
  territory.rural.patches.resize(2);
  territory.rural.patches[0].id = 10;
  territory.rural.patches[0].meanSuitability = .65;
  territory.rural.patches[1].id = 20;
  territory.rural.patches[1].meanSuitability = .8;
  territory.fields.resize(2);
  for (uint64_t i = 0; i < 12; ++i)
  {
    FarmField field;
    field.id = 100 + i;
    field.patch = i < 6 ? 10 : 20;
    field.boundary = rect((i % 6) * 100, (i / 6) * 100, 100, 100);
    field.access = {{i < 6 ? 0.0 : 600.0, 0, 100.0}};
    field.area = 10000;
    territory.fields[i < 6 ? 0 : 1].fields.push_back(field);
  }
  CitySkeleton skeleton;
  skeleton.revision = 4;
  skeleton.roadNodes = {{10, CityRoadNodeKind::Endpoint, {0, 0, 100}, {}}, {20, CityRoadNodeKind::Endpoint, {600, 0, 100}, {}}};
  CityAgricultureSettings settings;
  settings.population = 30;
  settings.availableWorkers = 20;
  CityAgriculturePlan plan;
  std::string error;
  check(planCityAgriculture(territory, skeleton, settings, plan, &error), "budget subdivided field polygons: " + error);
  check(plan.annualUsableYield >= plan.requiredAnnualYield, "selected fields satisfy local demand plus reserve");
  check(plan.productiveArea < 120000, "allocator stops instead of consuming every generated field");
  check(plan.assignedWorkers <= settings.availableWorkers + 1e-9, "farm labour remains within the settlement workforce budget");
  bool parented = !plan.parcels.empty();
  std::map<uint64_t, uint64_t> holdingOf;
  for (const auto &h : plan.holdings)
    for (auto p : h.parcels)
      parented &= holdingOf.emplace(p, h.id).second && h.accessNode;
  for (const auto &p : plan.parcels)
    parented &= holdingOf[p.parcel] == p.holding && p.accessNode;
  check(parented, "every productive parcel belongs to one route-connected holding");
  check(plan.holdings.size() == 2, "fields cluster into holdings by their access route");

  auto shortage = settings;
  shortage.population = 1000;
  CityAgriculturePlan shortPlan;
  check(planCityAgriculture(territory, skeleton, shortage, shortPlan, &error), "bounded shortage remains a valid plan: " + error);
  check(shortPlan.foodShortfall > 0 && shortPlan.supportedResidents < shortage.population, "insufficient land reports a quantified food shortfall");

  auto replayTerritory = territory;
  for (auto &fields : replayTerritory.fields)
    std::reverse(fields.fields.begin(), fields.fields.end());
  CityAgriculturePlan replay;
  check(planCityAgriculture(replayTerritory, skeleton, settings, replay, &error), "plan permuted agriculture: " + error);
  bool same = replay.parcels.size() == plan.parcels.size();
  for (size_t i = 0; same && i < plan.parcels.size(); ++i)
    same &= replay.parcels[i].parcel == plan.parcels[i].parcel;
  check(same, "agricultural selection is deterministic under input permutation");

  if (argc > 1)
  {
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    std::map<uint64_t, const CityAgriculturalParcel *> selected;
    for (const auto &p : plan.parcels)
      selected[p.parcel] = &p;
    json j;
    j["required"] = plan.requiredAnnualYield;
    j["usable"] = plan.annualUsableYield;
    j["parcels"] = json::array();
    for (const auto &product : territory.fields)
      for (const auto &p : product.fields)
        j["parcels"].push_back({{"id", p.id}, {"use", "arable"}, {"boundary", poly(p.boundary)}, {"selected", selected.count(p.id) != 0}, {"holding", selected.count(p.id) ? selected[p.id]->holding : 0}});
    std::ofstream(out / "city-agriculture-c3.json") << j.dump(2);
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
