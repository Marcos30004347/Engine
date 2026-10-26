#include "procedural/world/WorldCityComposition.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/WorldCityRelease.hpp"
#include "procedural/world/WorldCityPlotContractsIntegration.hpp"
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;

static std::vector<Position> rect(double x, double z, double w, double d)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + d}, {x, 0, z + d}};
}

int main()
{
  int failures = 0;
  auto check = [&](bool ok, const char *s) {
    std::cout << (ok ? "PASS " : "FAIL ") << s << '\n';
    failures += !ok;
  };
  std::string why;
  CityTerrainRouteCandidate candidate;
  candidate.id = 1;
  candidate.roads.revision = 6;
  candidate.roads.roadNodes = {{1, CityRoadNodeKind::Endpoint, {0, 0, 0}, {}}, {2, CityRoadNodeKind::Endpoint, {30, 0, 0}, {}}};
  CityRoadEdge edge;
  edge.id = 1;
  edge.from = 1;
  edge.to = 2;
  edge.path = {{0, 0, 0}, {30, 0, 0}};
  edge.halfWidth = 1.5;
  candidate.roads.roadEdges = {edge};
  candidate.benches = {{1, 1, {CityConstructionKind::Complex, 100}, rect(2, 1.5, 22, 14.5), 0}};
  CityTerrainRoutePlan terrain;
  CityTerrainRouteSettings ts;
  std::vector<CityTerrainRouteRejection> rejected;
  auto flat = [](double, double) {
    return 0.;
  };
  check(planCityTerrainRoutes({candidate}, flat, ts, {}, {}, terrain, rejected, &why), "neighbourhood terrain fixture solves");
  CityPlotSettings settings;
  settings.maximumAttempts = 1024;
  settings.minimumBuildingGap = .3;
  CityPlotContractPlan plots, report;
  bool fitted = planCityPlotContracts(terrain, {{100, 1, rect(2, 4, 18, 12)}}, {{10, 30, 2, 1}, {11, 30, 2, 1}, {12, 30, 4, 3}}, {}, settings, plots, report, &why);
  if (!fitted)
  {
    std::cerr << why << '\n';
  }
  check(fitted, "three real programmes include a taller requested landmark");
  if (!fitted)
  {
    return 1;
  }
  CityCompletionPlan city;
  city.revision = 6;
  city.skeleton = candidate.roads;
  CitySkeletonRegion region;
  region.id = 100;
  region.kind = CityRegionKind::Block;
  region.landUse = CityLandUse::UrbanBlock;
  region.boundary = rect(2, 1.5, 19, 14.5);
  city.skeleton.regions = {region};
  CityReliefPlan relief;
  check(applyCityPlotContracts(plots, city, relief, &why), "real plots publish before composition");
  CityCompositionBlock block;
  block.block = 100;
  block.boundary = region.boundary;
  block.publicSpaces = {rect(2, 1.5, 18, 2.5)};
  CityCompositionSettings policy;
  CityCompositionPlan composition;
  std::vector<CityLandmarkRequest> landmarks = {{12, {{9, 1.7, -5}}}};
  std::vector<CityPropRequest> props = {{1001, 10, CityFunctionalProp::Bench, {{17, 0, 2.25}, {7.4, 0, 2.25}}}};
  bool planned = planCityComposition(city.plotContracts, nullptr, {block}, landmarks, props, policy, composition, &why);
  if (!planned)
  {
    std::cerr << why << '\n';
  }
  check(planned, "coherent palette, density, public space, sightline and useful bench solve together");
  auto previous = composition;
  check(planned && composition.propAttempts == 2, "prop search skips the blocked candidate without moving an entrance");
  for (auto kind : {CityFunctionalProp::MarketStall, CityFunctionalProp::Signboard})
  {
    auto variant = props;
    variant[0].kind = kind;
    variant[0].candidates = {{7.4, 0, 2.15}};
    CityCompositionPlan other;
    check(planCityComposition(city.plotContracts, nullptr, {block}, landmarks, variant, policy, other, &why) && !other.props.front().parts.empty(), "functional prop variants retain full interaction clearance");
  }
  auto noBudget = policy;
  noBudget.maximumPropAttempts = 1;
  CityCompositionPlan exhausted;
  check(!planCityComposition(city.plotContracts, nullptr, {block}, landmarks, props, noBudget, exhausted, &why), "prop search budget fails explicitly");
  auto palette = block;
  palette.palette = CityNeighbourhoodPalette::SlateStone;
  CityCompositionPlan styled;
  check(planCityComposition(city.plotContracts, nullptr, {palette}, landmarks, props, policy, styled, &why) && styled.buildings.front().roof == 20, "neighbourhood material variant preserves accepted geometry");
  auto crowded = policy;
  crowded.maximumCoverage = .36;
  check(!planCityComposition(city.plotContracts, nullptr, {block}, landmarks, props, crowded, styled, &why), "overcrowded layout fails the upper density target");

  auto overhanging = block;
  overhanging.boundary = rect(2, 1.5, 18, 14.5);
  check(!planCityComposition(city.plotContracts, nullptr, {overhanging}, landmarks, props, policy, composition, &why), "native turret projection must fit its neighbourhood reservation");
  auto sparse = block;
  sparse.boundary = rect(2, 1.5, 100, 14.5);
  check(
      !planCityComposition(city.plotContracts, nullptr, {sparse}, landmarks, props, policy, composition, &why) && composition.buildings.size() == previous.buildings.size(),
      "sparse layout rejected transactionally without dropping demand");
  auto noSpace = block;
  noSpace.publicSpaces.clear();
  check(!planCityComposition(city.plotContracts, nullptr, {noSpace}, landmarks, props, policy, composition, &why), "usable open space is mandatory");
  auto misaligned = std::make_shared<CityPlotContractPlan>(*city.plotContracts);
  misaligned->plots[0].street.z -= 3;
  check(!planCityComposition(misaligned, nullptr, {block}, landmarks, props, policy, composition, &why), "frontage band rejects a recessed outlier");
  auto squeezed = std::make_shared<CityPlotContractPlan>(*city.plotContracts);
  for (auto &o : squeezed->plots[0].dwelling.openings)
  {
    if (o.exterior && o.kind == OpeningKind::Door)
    {
      o.b.x = o.a.x + (o.b.x - o.a.x) * .1;
      o.b.z = o.a.z + (o.b.z - o.a.z) * .1;
      break;
    }
  }
  check(!planCityComposition(squeezed, nullptr, {block}, landmarks, props, policy, composition, &why), "squeezed actual doorway fails before meshing");
  auto blocked = props;
  blocked[0].candidates = {plots.plots.front().door};
  check(!planCityComposition(city.plotContracts, nullptr, {block}, landmarks, blocked, policy, composition, &why), "functional prop cannot block contracted doorway");
  auto weak = landmarks;
  weak[0].building = 10;
  check(!planCityComposition(city.plotContracts, nullptr, {block}, weak, props, policy, composition, &why), "ordinary roof cannot be labelled a dominant landmark");
  auto hidden = landmarks;
  auto box = city_composition_detail::bounds(plots.plots.front());
  hidden[0].cameras = {{(box.x0 + box.x1) * .5, box.y0 + 1, (box.z0 + box.z1) * .5}};
  check(!planCityComposition(city.plotContracts, nullptr, {block}, hidden, props, policy, composition, &why), "occluded approach camera fails visibility gate");
  CityCompositionPlan reversed;
  auto reordered = std::make_shared<CityPlotContractPlan>(*city.plotContracts);
  std::reverse(reordered->plots.begin(), reordered->plots.end());
  check(
      planCityComposition(reordered, nullptr, {block}, landmarks, props, policy, reversed, &why) && reversed.buildings.size() == previous.buildings.size() &&
          reversed.buildings.front().building == previous.buildings.front().building,
      "composition ordering is deterministic");
  if (planned)
  {
    check(applyCityComposition(previous, city, &why), "composition publishes against exact snapshots");
    SettlementFabric fabric;
    fabric.terrain = flat;
    CityModelSettings options;
    options.streetDetails = false;
    CityModelPlan model;
    bool built = buildCityModel(city, fabric, options, model, &why, &relief);
    if (!built)
    {
      std::cerr << why << '\n';
    }
    check(built && model.buildings.size() == 3 && model.functionalProps == std::vector<uint64_t>{1001}, "normal city mesh emits composed real buildings and prop");
    bool roof = false;
    for (size_t i = 0; i < model.materials.size(); ++i)
    {
      if (model.materials[i] == 14)
      {
        roof = true;
        check(model.surfaceMaterials[i] == 2, "neighbourhood roof palette reaches native mesh");
        break;
      }
    }
    check(roof, "native roof geometry exists");
    if (built)
    {
      std::ofstream out("phase6-neighbourhood.obj");
      for (auto v : model.mesh.vertices)
      {
        out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
      }
      for (size_t i = 0; i < model.mesh.triangles.size(); ++i)
      {
        out << "usemtl " << model.surfaceMaterials[i] << '\n';
        auto t = model.mesh.triangles[i];
        out << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
      }
    }
    std::optional<std::vector<SettlementHistoricalBuilding>> history = std::vector<SettlementHistoricalBuilding>{{10,"home",0,1},{11,"home",0,1},{12,"home",0,3}};
    auto gate = assessCityRelease(city,relief,model,history,{true,true});
    check(gate.accepted && gate.generated==3, "release accepts identical accepted plans with all buildings and tiles");
    check(!assessCityRelease(city,relief,model,history,{true,false}).accepted, "partial runtime success cannot pass release");
    check(!assessCityRelease(city,relief,model,history,{}).accepted, "zero runtime jobs cannot pass release");
    auto wrongProgramme = history;
    wrongProgramme->at(0).use="keep";
    check(!assessCityRelease(city,relief,model,wrongProgramme,{true}).accepted, "ordinary house cannot fulfil a historical keep identity");
    wrongProgramme = history;
    wrongProgramme->at(2).floors=1;
    check(!assessCityRelease(city,relief,model,wrongProgramme,{true}).accepted, "historical storey count must match accepted contract");
    auto extra = history;
    extra->push_back({99,"keep"});
    auto missing = assessCityRelease(city,relief,model,extra,{true});
    check(!missing.accepted && missing.missingBuildings==std::vector<uint64_t>{99}, "missing historical castle is not hidden by ordinary placement totals");
    auto filler = model;
    CityModelBuilding invented;
    invented.id=999;
    filler.buildings.push_back(invented);
    check(!assessCityRelease(city,relief,filler,history,{true}).accepted, "filler housing cannot inflate historical placement");
    auto alias = city;
    alias.historicalLots[1].lot=alias.historicalLots[0].lot;
    check(!assessCityRelease(alias,relief,model,history,{true}).accepted, "one physical building cannot satisfy two historical buildings");
    auto legacy = city;
    legacy.plotContracts.reset();
    check(!assessCityRelease(legacy,relief,model,history,{true}).accepted, "legacy candidate is not an accepted blueprint");
    auto stale = relief;
    stale.constructionTerrain=std::make_shared<CityTerrainRoutePlan>(*relief.constructionTerrain);
    check(!assessCityRelease(city,stale,model,history,{true}).accepted, "release requires the same canonical terrain snapshot");
    check(!assessCityRelease(city,relief,model,std::nullopt,{true}).accepted, "missing history is not an empty successful history");
    auto changed = city;
    changed.plotContracts = reordered;
    size_t before = model.buildings.size();
    check(!buildCityModel(changed, fabric, options, model, &why, &relief) && model.buildings.size() == before, "mesher rejects stale composition transactionally");
  }
  std::cout << "CITY_COMPOSITION_TESTS failures=" << failures << '\n';
  return failures ? 1 : 0;
}
