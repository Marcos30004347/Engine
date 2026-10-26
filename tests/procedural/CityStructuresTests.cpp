#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/WorldCityYardBoundaries.hpp"
#include "procedural/world/WorldCityStructuresIntegration.hpp"
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;

int main()
{
  int failures = 0;
  auto check = [&](bool ok, const char *s) {
    std::cout << (ok ? "PASS " : "FAIL ") << s << '\n';
    failures += !ok;
  };
  check(cityYardNeedsBoundary(150,70) && !cityYardNeedsBoundary(60,50),
        "substantial yards get boundaries while narrow urban gaps remain open");
  std::vector<char> free(41,1);free[20]=0;
  const auto separated=cityYardBoundaryRuns(10,free,.25);
  check(separated.size()==2 && separated[0].second<5 && separated[1].first>5,
        "a reserved crossing is never bridged by fence gap cleanup");
  check(separated.size()==2 && separated[1].first-separated[0].second<=.25+1e-6,
        "sampled fence ends meet the protected interval without extra gaps");
  const auto inset=cityYardInsetFence({{0,0,0},{30,0,0},{6,0,18}},
      {{5,0,3},{13,0,3},{13,0,8},{5,0,8}});
  check(inset && inset->size()==4 && (*inset)[0].x>0 && (*inset)[2].z<18,
        "an acute lot may use a smaller square fence around its house");
  const auto clipped=cityYardInsetFence({{0,0,0},{30,0,0},{6,0,18}},
      {{3.2,0,3},{8,0,3},{8,0,7},{3.2,0,7}});
  check(clipped && clipped->size()>3 && std::none_of(clipped->begin(),clipped->end(),
      [](const Position &p){return std::abs(p.x-30)<1e-6 && std::abs(p.z)<1e-6;}),
        "a cramped house keeps its entrance while the remote acute corner is cut back");
  const auto stepped=cityYardInsetFence(
      {{1214.001,0,415.806},{1219.804,0,409.289},{1221.519,0,407.362},
       {1223.918,0,404.668},{1223.155,0,409.924},{1232.729,0,411.314},
       {1228.019,0,416.604},{1223.953,0,421.170}},
      {{1222.835,0,418.806},{1217.906,0,416.149},{1220.563,0,411.220},
       {1225.492,0,413.877}},
      {{1218.244,0,418.263},{1218.378,0,418.013},{1219.786,0,418.772},
       {1219.652,0,419.022}});
  check(stepped && stepped->size()>8 &&
      std::none_of(stepped->begin(),stepped->end(),[](Position p){return std::hypot(p.x-1223.918,p.z-404.668)<.25;}),
      "stairs near an unchanged side do not prevent beveling a remote needle corner");
  check(!cityYardInsetFence({{0,0,0},{20,0,0},{20,0,20},{0,0,20}},
      {{5,0,5},{12,0,5},{12,0,12},{5,0,12}}),
        "an ordinary lot keeps its existing boundary");
  const auto gates=cityYardBoundaryRuns(10,std::vector<char>(41,1),.25,5);
  check(gates.size()==2 && gates[0].second<=4.1 && gates[1].first>=5.9,
        "yard gate preserves a full 1.8 metre walking opening");
  check(cityYardBoundaryRuns(10,std::vector<char>(41,0),.25).empty(),
        "occupied pathway cannot receive boundary geometry");
  auto terrain = std::make_shared<CityTerrainRoutePlan>();
  terrain->revision = 1;
  terrain->ground = [](double, double) {
    return 0.;
  };
  terrain->constraints = {{{{-8, 0, -2}, {24, 0, 14}}}};
  auto plots = std::make_shared<CityPlotContractPlan>();
  plots->revision = 1;
  plots->terrain = terrain;
  CityStructureProgramme input;
  CityStructureTower tower;
  tower.id = 1;
  tower.box = {0, -.2, 0, 12, 4, 12};
  tower.walk = 1;
  tower.stairs = {{1, {{6, 1, 6}, {1.5, 1, 6}, {1.5, 1, 2}, {2.7, 1, 2}, {9, 4, 2}, {10.2, 4, 2}}, 1.2, 2.1, true}};
  input.towers.push_back(tower);
  CityStructureWall wall;
  wall.id = 2;
  wall.towerA = 1;
  wall.a = {6, 1, 6};
  wall.b = {22, 1, 6};
  wall.bottom = -.2;
  input.walls.push_back(wall);
  input.stairs = {{3, {{24, 0, 6}, {22, 1, 6}}, 1.2, 2.1, true}};
  CityStructurePlan plan;
  std::string why;
  bool ok = planCityStructures(plots, input, plan, &why);
  if (!ok)
  {
    std::cerr << why << '\n';
  }
  check(ok && !plan.mesh.triangles.empty(), "tower socket, real floors and stair opening mesh as one assembly");
  auto saved = plan;
  auto inaccessible = input;
  inaccessible.stairs.clear();
  check(!planCityStructures(plots, inaccessible, plan, &why), "elevated wall and tower require connected ground access");
  auto wrong = input;
  wrong.walls[0].a.y = 2;
  check(!planCityStructures(plots, wrong, plan, &why) && plan.mesh.triangles.size() == saved.mesh.triangles.size(), "mismatched wall walk fails transactionally");
  wrong = input;
  wrong.protectedVolumes = {{13, .8, 5.5, 15, 3, 6.5}};
  check(!planCityStructures(plots, wrong, plan, &why), "wall cannot invade required empty passage");
  wrong = input;
  wrong.towers[0].stairs.clear();
  check(!planCityStructures(plots, wrong, plan, &why), "tower top needs actual stair access");
  CityStructureProgramme stairs;
  stairs.stairs = {{5, {{0, 0, 0}, {6, 2, 0}, {7.2, 2, 0}, {7.2, 2, 2.4}, {6, 2, 2.4}, {0, 4, 2.4}, {-1.2, 4, 2.4}}, 1.2, 2.1, true}};
  CityStructurePlan switchback;
  ok = planCityStructures(plots, stairs, switchback, &why);
  if (!ok)
  {
    std::cerr << why << '\n';
  }
  check(ok, "switchback stairs have full-width level turning landings");
  auto badStairs = stairs;
  badStairs.protectedVolumes = {{-.9, 3.5, 2, -.3, 5, 2.8}};
  check(!planCityStructures(plots, badStairs, plan, &why), "stair exit clearance detects a blocking solid");
  auto blockedExit = stairs;
  CityStructureWall obstruction;
  obstruction.id = 10;
  obstruction.a = {-1.2, 5, 2.4};
  obstruction.b = {0, 5, 2.4};
  obstruction.bottom = -.2;
  obstruction.width = 1.2;
  obstruction.retaining = true;
  blockedExit.walls.push_back(obstruction);
  check(!planCityStructures(plots, blockedExit, plan, &why), "retaining wall cannot fence off a stair exit");
  CityStructureProgramme bridge;
  bridge.bridges = {{6, {0, 3, 0, 10, 3.4, 2}, {0, -.2, 0, 1, 3, 2}, {9, -.2, 0, 10, 3, 2}, {1, 0, 0, 9, 2.8, 2}}};
  bridge.bridges[0].approaches = {{6, {{-6, 0, 1}, {0, 3.4, 1}}, 1.2, 2.1, true}, {6, {{16, 0, 1}, {10, 3.4, 1}}, 1.2, 2.1, true}};
  check(planCityStructures(plots, bridge, plan, &why), "bridge reserves clear space below and matched grounded abutments");
  bridge.bridges[0].below.y1 = 3.2;
  check(!planCityStructures(plots, bridge, plan, &why), "bridge deck cannot consume underpass headroom");
  auto missing = bridge;
  missing.bridges[0].below.y1 = 2.8;
  missing.bridges[0].approaches.clear();
  check(!planCityStructures(plots, missing, plan, &why), "bridge requires two usable arrivals");
  CityStructureProgramme quay;
  quay.quays = {{7, {0, -.2, 0, 10, 1, 1}, {0, 0, 1, 10, .5, 4}}};
  check(planCityStructures(plots, quay, plan, &why), "stone quay contains channel with dry freeboard");
  quay.quays[0].bank.z1 = 2;
  check(!planCityStructures(plots, quay, plan, &why), "quay cannot block water channel");
  CityStructureProgramme gate;
  CityStructureWall g;
  g.id = 8;
  g.a = {0, 4, 0};
  g.b = {10, 4, 0};
  g.bottom = -.2;
  g.portals = {{4, 0, -2, 6, 2.5, 2}};
  gate.walls = {g};
  gate.stairs = {{9, {{18, 0, 0}, {10, 4, 0}}, 1.2, 2.1, true}};
  check(planCityStructures(plots, gate, plan, &why), "gate passage retains structural lintel and continuous walk");
  gate.walls[0].portals.push_back({5, 0, -2, 7, 2.5, 2});
  check(!planCityStructures(plots, gate, plan, &why), "overlapping gate apertures rejected before meshing");
  gate.walls[0].portals.clear();
  gate.walls[0].bottom = 5;
  check(!planCityStructures(plots, gate, plan, &why), "wall supports cannot exceed walk height");
  CityCompletionPlan city;
  city.revision = 1;
  city.plotContracts = plots;
  city.skeleton.revision = 1;
  CitySkeletonRegion block;
  block.id = 100;
  block.kind = CityRegionKind::Block;
  block.landUse = CityLandUse::UrbanBlock;
  block.boundary = {{-8, 0, -2}, {24, 0, -2}, {24, 0, 14}, {-8, 0, 14}};
  city.skeleton.regions = {block};
  CityReliefPlan relief;
  relief.constructionTerrain = terrain;
  check(applyCityStructures(saved, city, &why), "accepted structures publish against exact plot snapshot");
  auto newer = std::make_shared<CityPlotContractPlan>(*plots);
  city.plotContracts = newer;
  check(!applyCityStructures(saved, city, &why), "stale structural publication is rejected");
  city.plotContracts = plots;
  SettlementFabric fabric;
  fabric.terrain = terrain->ground;
  CityModelSettings options;
  options.infrastructureOnly = true;
  options.streetDetails = false;
  CityModelPlan model;
  bool built = buildCityModel(city, fabric, options, model, &why, &relief);
  if (!built)
  {
    std::cerr << why << "\n";
  }
  check(built && model.structuralPieces == saved.solids.size(), "normal city model emits accepted structural assembly");
  city.plotContracts = newer;
  check(!buildCityModel(city, fabric, options, model, &why, &relief), "normal model rejects changed plot snapshot");
  if (!switchback.mesh.triangles.empty())
  {
    std::ofstream out("phase5-switchback.obj");
    for (auto v : switchback.mesh.vertices)
    {
      out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    }
    for (auto t : switchback.mesh.triangles)
    {
      out << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
    }
  }
  if (!saved.mesh.triangles.empty())
  {
    std::ofstream out("phase5-structures.obj");
    for (auto v : saved.mesh.vertices)
    {
      out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    }
    for (auto t : saved.mesh.triangles)
    {
      out << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
    }
  }
  std::cout << "CITY_STRUCTURES_TESTS failures=" << failures << '\n';
  return failures ? 1 : 0;
}
