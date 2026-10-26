#include "json.hpp"
#include "procedural/world/WorldCoastDetail.hpp"
#include "procedural/world/WorldLandGenerator.hpp"
#include "procedural/world/WorldLandProduct.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
using namespace procedural;
using namespace procedural::world;
using namespace procedural::world::rewrite;
using nlohmann::json;
std::vector<VoronoiCell> gridCells(int size)
{
  std::vector<VoronoiCell> cells;
  for (int z = 0; z < size; ++z)
    for (int x = 0; x < size; ++x)
    {
      double a = x * 100., b = z * 100.;
      cells.push_back({{a + 50, b + 50}, {{a, b}, {a + 100, b}, {a + 100, b + 100}, {a, b + 100}}, {}});
    }
  return cells;
}
json encode(const LandProduct &p)
{
  json cells = json::array(), coasts = json::array();
  for (auto &c : p.cells)
  {
    json ring = json::array();
    for (auto use : c.patch.outer)
    {
      auto e = p.boundaries.find(use.edgeId);
      auto &v = use.reversed ? e->vertices.back() : e->vertices.front();
      ring.push_back({v.position.x, v.position.z});
    }
    cells.push_back({{"id", c.id}, {"land", c.land}, {"ring", ring}});
  }
  for (auto &loop : p.coasts)
  {
    json ring = json::array();
    for (auto use : loop.edges)
    {
      auto e = p.boundaries.find(use.edgeId);
      auto &v = use.reversed ? e->vertices.back() : e->vertices.front();
      ring.push_back({v.position.x, v.position.z});
    }
    coasts.push_back({{"ring", ring}, {"area", loop.signedAreaMetres2}});
  }
  return {{"cells", cells}, {"coasts", coasts}, {"components", p.components}, {"landArea", p.landAreaMetres2}, {"totalArea", p.totalAreaMetres2}};
}
int main(int argc, char **argv)
{
  int failures = 0;
  json assertions = json::array();
  auto check = [&](bool pass, const char *name)
  {
    assertions.push_back({{"name", name}, {"pass", pass}});
    std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
    failures += !pass;
  };
  WorldSpec spec;
  spec.extentMetres = 2000;
  auto cells = gridCells(20);
  LandSettings settings;
  settings.cores = {210};
  LandProduct land;
  std::string error;
  bool ok = buildLandProduct(spec, cells, settings, land, &error);
  check(ok, "Connected land product publishes");
  if (!ok)
  {
    std::cerr << error << '\n';
    return 1;
  }
  check(land.components == 1 && land.coasts.size() == 1, "Single island has one oriented closed loop");
  check(land.landAreaMetres2 >= 1600000 && land.landAreaMetres2 <= 1610000, "Land budget error bounded by one planning cell");
  double coastArea = 0;
  for (auto &loop : land.coasts)
    coastArea += loop.signedAreaMetres2;
  check(std::abs(coastArea - land.landAreaMetres2) < 1e-6, "Coast signed area equals selected land area");
  auto expected = encode(land);
  bool repeated = true;
  std::vector<std::future<json>> jobs;
  for (int i = 0; i < 4; ++i)
    jobs.push_back(
        std::async(
            std::launch::async,
            [&]
            {
              LandProduct p;
              return buildLandProduct(spec, cells, settings, p) ? encode(p) : json();
            }));
  for (auto &j : jobs)
    repeated &= j.get() == expected;
  check(repeated, "Four parallel runs publish identical products");
  auto bad = cells;
  bad[210].area[1].x += 1;
  check(!buildLandProduct(spec, bad, settings, land, &error) && encode(land) == expected, "Cracked partition rejected atomically");
  bad = cells;
  std::reverse(bad[210].area.begin(), bad[210].area.end());
  check(!buildLandProduct(spec, bad, settings, land, &error), "Reversed cell winding rejected");
  bad = cells;
  bad[210].area[1] = bad[210].area[0];
  check(!buildLandProduct(spec, bad, settings, land, &error), "Collapsed cell rejected");
  LandSettings archipelago;
  archipelago.cores = {104, 315};
  archipelago.landFraction = .08;
  LandProduct islands;
  check(buildLandProduct(spec, cells, archipelago, islands, &error) && islands.components == 2 && islands.coasts.size() == 2, "Constructed archipelago retains two islands");
  LandSettings tiny;
  tiny.cores = {210};
  tiny.landFraction = .0025;
  LandProduct one;
  check(buildLandProduct(spec, cells, tiny, one, &error) && one.coasts.size() == 1 && one.coasts[0].edges.size() == 4, "Single cell island retained explicitly");
  LandSettings peninsula;
  peninsula.cores = {200};
  peninsula.landFraction = .08;
  LandProduct border;
  check(buildLandProduct(spec, cells, peninsula, border, &error) && border.components == 1, "Border-connected land closes along world rectangle");
  LandSettings lake;
  lake.landFraction = .96;
  for (uint32_t i = 0; i < 400; ++i)
    if (i / 20 < 8 || i / 20 > 11 || i % 20 < 8 || i % 20 > 11)
      lake.cores.push_back(i);
  LandProduct ring;
  check(
      buildLandProduct(spec, cells, lake, ring, &error) && ring.coasts.size() == 2 &&
          std::any_of(
              ring.coasts.begin(),
              ring.coasts.end(),
              [](auto &l)
              {
                return l.signedAreaMetres2 < 0;
              }),
      "Inland water hole has opposite winding");
  LandSettings impossible = tiny;
  impossible.landFraction = .001;
  check(!buildLandProduct(spec, cells, impossible, land, &error), "Impossible core area budget rejected");
  LandSettings touching;
  touching.landFraction = .005;
  touching.cores = {210, 231};
  check(!buildLandProduct(spec, cells, touching, land, &error), "Vertex-touching islands rejected before meshing");
  ReliefControls controls;
  std::vector<ReliefProvince> provinces = {{1, {300, 0, 200}, {300, 0, 1800}, 300, .02, .2}, {2, {1500, 0, 200}, {1500, 0, 1800}, 300, .03, .24}};
  check(controls.build(provinces, {.001, .02}, &error), "Separate elongated slope provinces build");
  auto ridge = controls.sample(300, 1000), plain = controls.sample(900, 1000), edge = controls.sample(600, 1000);
  check(ridge.terrainLambda > plain.terrainLambda && plain.terrainLambda == .02 && edge.terrainLambda == .02, "Two ranges separated by lowland with compact support");
  auto reversed = provinces;
  std::reverse(reversed.begin(), reversed.end());
  ReliefControls other;
  other.build(reversed, {.001, .02});
  check(other.sample(300, 1000).terrainLambda == ridge.terrainLambda, "Province order does not change evaluator");
  auto strong = provinces;
  strong[0].strength = 4;
  check(other.build(strong, {.001, .02}) && other.sample(300, 1000).terrainLambda > ridge.terrainLambda &&
            other.sample(300, 1000).terrainLambda <= .2 && other.sample(900, 1000).terrainLambda == .02,
        "Range strength increases the slope control within its compact support and paper bounds");
  strong[0].strength = -1;
  check(!other.build(strong, {.001, .02}), "Negative range strength rejected");
  provinces[0].terrainLambda = .3;
  check(!controls.build(provinces, {.001, .02}, &error) && controls.sample(300, 1000).terrainLambda == ridge.terrainLambda, "Out of paper lambda range rejected atomically");
  check(encode(land) == expected, "Slope controls never mutate frozen coastline");
  json samples = json::array();
  for (int z = 0; z <= 40; ++z)
    for (int x = 0; x <= 40; ++x)
    {
      auto s = controls.sample(x * 50, z * 50);
      samples.push_back({x * 50, z * 50, s.riverSlope, s.terrainLambda});
    }
  LandProduct generated;
  bool generatedOK = generateLand(spec, 130, {{1000, 0, 1000}}, .4, generated, &error);
  check(generatedOK, "Poisson Voronoi land entry point produces canonical mesh domains");
  if (!generatedOK)
    std::cerr << error << '\n';
  LandProduct again;
  check(generateLand(spec, 130, {{1000, 0, 1000}}, .4, again, &error) && encode(generated) == encode(again), "Procedural land entry point repeats exactly");
  check(!generateLand(spec, 0, {{1000, 0, 1000}}, .4, again, &error), "Invalid procedural sampling budget rejected");
  LandProduct detailed;
  bool detailedOK = generatedOK && detailLandCoasts(generated, {}, detailed, &error);
  check(detailedOK, "Bounded coast detail compiles every land and ocean CDT patch");
  if (!detailedOK)
    std::cerr << error << '\n';
  check(detailed.components == generated.components && detailed.coasts.size() == generated.coasts.size(), "Coast detail preserves island topology");
  check(std::abs(detailed.totalAreaMetres2 - generated.totalAreaMetres2) < 1e-4, "Coast detail preserves world coverage");
  LandProduct detailedAgain;
  check(detailLandCoasts(generated, {}, detailedAgain, &error) && encode(detailed) == encode(detailedAgain), "Coast detail repeats exactly");
  CoastDetailSettings invalid;
  invalid.amplitude = 1;
  check(!detailLandCoasts(generated, invalid, detailedAgain, &error) && encode(detailedAgain) == encode(detailed), "Unsafe coast amplitude rejected atomically");
  bool seedSweep = true;
  for (uint64_t seed = 1; seed <= 8; ++seed)
  {
    WorldSpec seeded = spec;
    seeded.seed = seed;
    LandProduct base, detail;
    if (!generateLand(seeded, 160, {{1000, 0, 1000}}, .4, base, &error) || !detailLandCoasts(base, {}, detail, &error))
    {
      seedSweep = false;
      std::cerr << "seed " << seed << ": " << error << '\n';
    }
  }
  check(seedSweep, "Eight procedural seeds pass partition coast and per-patch CDT gates");
  bool membership = detailed.cells.size() == generated.cells.size();
  for (size_t i = 0; i < detailed.cells.size() && membership; ++i)
    membership &= detailed.cells[i].land == generated.cells[i].land && detailed.cells[i].id == generated.cells[i].id;
  check(membership, "Coast detail retains cell IDs and land membership");
  check(detailedOK && encode(detailed) != encode(generated), "Coast detail actually changes physical boundary geometry");
  LandSettings insetSettings;
  insetSettings.cores = {210};
  insetSettings.landFraction = .4;
  insetSettings.oceanMarginMetres = 200;
  LandProduct inset;
  bool insetOK = buildLandProduct(spec, cells, insetSettings, inset, &error);
  bool insetClear = insetOK;
  for (const auto &c : inset.cells)
    if (c.land)
      for (auto use : c.patch.outer)
        for (auto v : inset.boundaries.find(use.edgeId)->vertices)
          insetClear &= v.position.x >= 200 && v.position.x <= 1800 && v.position.z >= 200 && v.position.z <= 1800;
  check(insetClear, "Island ocean reservation keeps every land vertex away from world border");
  auto savedInset = encode(inset);
  insetSettings.cores = {0};
  check(!buildLandProduct(spec, cells, insetSettings, inset, &error) && encode(inset) == savedInset, "Ocean-reserved core rejected atomically");
  insetSettings.cores = {210};
  insetSettings.landFraction = .95;
  check(!buildLandProduct(spec, cells, insetSettings, inset, &error) && encode(inset) == savedInset, "Unreachable island area budget fails without consuming ocean reservation");
  insetSettings.oceanMarginMetres = -1;
  check(!buildLandProduct(spec, cells, insetSettings, inset, &error), "Negative ocean margin rejected");
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"schema", 1},
    {"fixture", "land"},
    {"assertions", assertions},
    {"detailed", encode(detailed)},
    {"generated", encode(generated)},
    {"land", expected},
    {"archipelago", encode(islands)},
    {"inlandWater", encode(ring)},
    {"slopes", samples}};
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << report.dump(2) << '\n';
    if (!f)
      return 2;
  }
  return failures ? 1 : 0;
}
