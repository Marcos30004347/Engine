#include "json.hpp"
#include "procedural/world/WorldCoastDetail.hpp"
#include "procedural/world/WorldLandGenerator.hpp"
#include "procedural/world/WorldReliefSurface.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
using namespace procedural::world::rewrite;
using nlohmann::json;
LandProduct square()
{
  LandProduct l;
  l.spec.extentMetres = 1000;
  l.landAreaMetres2 = l.totalAreaMetres2 = 1000000;
  l.components = 1;
  BoundaryEndpoint a{1, {0, 0, 0}}, b{2, {1000, 0, 0}}, c{3, {1000, 0, 1000}}, d{4, {0, 0, 1000}};
  l.boundaries.commit({{1, a, b, {}}, {2, b, c, {}}, {3, c, d, {}}, {4, d, a, {}}});
  PatchDomain p;
  p.id = 1;
  p.outer = {{1, false}, {2, false}, {3, false}, {4, true}};
  l.cells.push_back({0, true, 1000000, p});
  l.coasts.push_back({p.outer, 1000000});
  return l;
}
json encode(const WatershedProduct &w)
{
  json cells = json::array(), vertices = json::array(), edges = json::array();
  for (auto c : w.cells)
  {
    json fragments = json::array();
    for (auto &f : c.fragments)
    {
      json ring = json::array();
      for (auto p : f)
        ring.push_back({p.x, p.z});
      fragments.push_back(ring);
    }
    cells.push_back({{"node", c.riverNode}, {"area", c.areaMetres2}, {"fragments", fragments}});
  }
  for (auto v : w.vertices)
    vertices.push_back({{"position", {v.position.x, v.position.y, v.position.z}}, {"coastal", v.coastal}, {"sites", v.equidistantSites}});
  for (auto e : w.edges)
    edges.push_back({e.a, e.b, e.leftCell, e.rightCell, e.riverCrossing ? 1u : 0u});
  return {{"cells", cells}, {"vertices", vertices}, {"edges", edges}, {"area", w.landAreaMetres2}, {"sourceGraphTruncated", w.sourceGraphTruncated}};
}
int main(int argc, char **argv)
{
  int failures = 0;
  json assertions = json::array();
  auto check = [&](bool p, const char *s)
  {
    std::cout << (p ? "PASS " : "FAIL ") << s << '\n';
    assertions.push_back({{"name", s}, {"pass", p}});
    failures += !p;
  };
  auto land = square();
  ReliefControls controls;
  controls.build({}, {.01, .1});
  RiverGrowthProduct rivers;
  rivers.nodes = {{{300, 10, 300}, UINT32_MAX, 1, 1}, {{700, 20, 300}, UINT32_MAX, 1, 1}, {{500, 30, 300 + 200 * std::sqrt(3.)}, UINT32_MAX, 1, 1}};
  WatershedProduct watersheds;
  std::string error;
  bool ok = buildWatersheds(land, rivers, controls, false, watersheds, &error);
  check(ok, "Clipped hydrological Voronoi product builds");
  if (!ok)
  {
    std::cerr << error << '\n';
    return 1;
  }
  double expected = 30 + .1 * 400 / std::sqrt(3.);
  bool analytic = false, coastal = true;
  for (auto v : watersheds.vertices)
  {
    if (!v.coastal)
      analytic |= v.equidistantSites.size() == 3 && std::abs(v.position.y - expected) < 1e-8;
    else
      coastal &= v.position.y == 0;
  }
  check(analytic, "Equilateral crest satisfies max node height plus lambda times circumradius");
  check(coastal, "Coastal clipping endpoints use water level rather than three-site formula");
  check(std::abs(watersheds.landAreaMetres2 - 1000000) < 1e-6, "Clipped watershed areas cover physical land exactly");
  WatershedProduct repeat;
  check(buildWatersheds(land, rivers, controls, false, repeat, &error) && encode(repeat) == encode(watersheds), "Watershed products repeat exactly");
  auto connected = rivers;
  connected.nodes[1].receiver = 0;
  WatershedProduct crossings;
  check(
      buildWatersheds(land, connected, controls, false, crossings, &error) && std::count_if(
                                                                                  crossings.edges.begin(),
                                                                                  crossings.edges.end(),
                                                                                  [](auto e)
                                                                                  {
                                                                                    return e.riverCrossing;
                                                                                  }) == 1,
      "River entry edge classified separately from crest edges");
  auto truncated = rivers;
  truncated.budgetReached = true;
  auto before = encode(repeat);
  check(!buildWatersheds(land, truncated, controls, false, repeat, &error) && encode(repeat) == before, "Truncated drainage rejected atomically without opt-in");
  check(buildWatersheds(land, truncated, controls, true, repeat, &error) && repeat.sourceGraphTruncated, "Explicit diagnostic opt-in preserves truncated source flag");
  auto duplicate = rivers;
  duplicate.nodes[1].position = duplicate.nodes[0].position;
  check(!buildWatersheds(land, duplicate, controls, false, repeat, &error), "Coincident river sites rejected");
  RiverGrowthProduct tied;
  tied.nodes = {{{300, 10, 300}}, {{700, 20, 300}}, {{700, 30, 700}}, {{300, 40, 700}}};
  WatershedProduct cocircular;
  bool tie = buildWatersheds(land, tied, controls, false, cocircular, &error);
  check(
      tie && std::any_of(
                 cocircular.vertices.begin(),
                 cocircular.vertices.end(),
                 [](auto v)
                 {
                   return !v.coastal && v.equidistantSites.size() == 4 && std::abs(v.position.y - (40 + .1 * std::hypot(200, 200))) < 1e-8;
                 }),
      "Cocircular four-site tie has one deterministic crest height");
  CompactTerrainField field;
  std::vector<TerrainPrimitive> primitives = {{1, 0, {0, 10, 0}, 10, 0, 100}, {2, 0, {0, 20, 0}, 10, 0, 100}, {3, 0, {0, 40, 0}, 10, 0, 100}};
  double h = 99;
  check(field.build(primitives, &error) && field.sample(0, 0, h) && std::abs(h - 70. / 3) < 1e-12, "Fixed padded blend tree agrees with analytic equal-weight result");
  check(
      CompactTerrainField::weight(0, 10) == 1 && CompactTerrainField::weight(5, 10) == .5625 && CompactTerrainField::weight(10, 10) == 0 && CompactTerrainField::weight(20, 10) == 0,
      "Dimensionless compact kernel passes centre boundary and outside tests");
  h = 99;
  check(!field.sample(100, 100, h) && h == 99, "Missing support fails without silently returning sea level");
  std::reverse(primitives.begin(), primitives.end());
  CompactTerrainField reversed;
  double h2;
  check(reversed.build(primitives) && field.sample(1, 2, h) && reversed.sample(1, 2, h2) && h == h2, "Primitive publication order leaves fixed tree unchanged");
  primitives[0].radiusMetres = -1;
  check(!field.build(primitives, &error) && field.sample(0, 0, h) && std::abs(h - 70. / 3) < 1e-12, "Invalid primitive build retains previous field");
  WorldReliefSurface surface;
  ReliefSurfaceSettings settings;
  settings.sampleSpacingMetres = 100;
  check(surface.build(land, connected, crossings, settings, &error), "Covered compact terrain surface builds from watershed fragments");
  bool coverage = true, parity = true;
  json samples = json::array();
  double minH = 1e9, maxH = -1e9;
  for (int z = 0; z <= 50; ++z)
    for (int x = 0; x <= 50; ++x)
    {
      double a = 0, b = 0;
      bool indexed = surface.sample(x * 20, z * 20, a), brute = surface.sample(x * 20, z * 20, b, false);
      coverage &= indexed;
      parity &= indexed == brute && a == b;
      if (indexed)
      {
        samples.push_back({x * 20, z * 20, a});
        minH = std::min(minH, a);
        maxH = std::max(maxH, a);
      }
    }
  check(coverage, "Every sample in 51 by 51 land coverage grid has positive support");
  check(parity, "Indexed evaluator is bit-identical to brute-force tree evaluation");
  check(surface.sample(500, 0, h) && h == 0, "Terrain joins exact declared shoreline height");
  h = 99;
  check(!surface.sample(-10, 500, h) && h == 99, "Surface rejects ocean and outside domain");
  bool continuous = surface.sample(500 - 1e-5, 500, h) && surface.sample(500 + 1e-5, 500, h2) && std::abs(h - h2) < 1e-3;
  check(continuous, "Terrain is continuous across hydrological cell boundary");
  check(maxH - minH > 10, "Constructed surface contains nonconstant watershed relief");
  auto holed = land;
  std::vector<BoundaryInput> boundaries;
  for (auto &edge : land.boundaries.records())
    boundaries.push_back({edge.id, {edge.vertices[0].index, edge.vertices[0].position}, {edge.vertices[1].index, edge.vertices[1].position}, {}});
  BoundaryEndpoint ha{11, {400, 0, 400}}, hb{12, {400, 0, 600}}, hc{13, {600, 0, 600}}, hd{14, {600, 0, 400}};
  boundaries.insert(boundaries.end(), {{5, ha, hb, {}}, {6, hb, hc, {}}, {7, hc, hd, {}}, {8, hd, ha, {}}});
  holed.boundaries.commit(boundaries);
  std::vector<BoundaryUse> hole{{5, false}, {6, false}, {7, false}, {8, true}};
  holed.cells[0].patch.holes.push_back(hole);
  holed.cells[0].areaMetres2 = holed.landAreaMetres2 = 960000;
  holed.coasts.push_back({hole, -40000});
  WatershedProduct clippedHole;
  WorldReliefSurface holeSurface;
  bool holeOK = buildWatersheds(holed, connected, controls, false, clippedHole, &error);
  check(holeOK && std::abs(clippedHole.landAreaMetres2 - 960000) < 1e-6, "Hydrological clipping excludes inland water and preserves land area");
  check(holeOK && holeSurface.build(holed, connected, clippedHole, settings, &error) && !holeSurface.sample(500, 500, h), "Continuous surface excludes inland water hole");
  auto invalidRiver = connected;
  invalidRiver.nodes[1].receiver = 999;
  check(!surface.build(land, invalidRiver, crossings, settings, &error) && surface.sample(500, 500, h), "Malformed river receiver rejected without replacing valid surface");
  WorldSpec generatedSpec;
  generatedSpec.extentMetres = 2000;
  LandProduct planning, physical;
  bool generated = generateLand(generatedSpec, 180, {{1000, 0, 1000}}, .6, planning, &error) && detailLandCoasts(planning, {}, physical, &error);
  RiverGrowthProduct generatedRivers;
  WatershedProduct generatedWatersheds;
  WorldReliefSurface generatedSurface;
  if (generated)
  {
    RiverMouth mouth;
    mouth.priority = 4;
    double longest = -1;
    for (auto loop : physical.coasts)
      if (loop.signedAreaMetres2 > 0)
        for (auto use : loop.edges)
        {
          auto edge = physical.boundaries.find(use.edgeId);
          auto a = edge->vertices.front().position, b = edge->vertices.back().position;
          double length = std::hypot(a.x - b.x, a.z - b.z);
          if (length > longest)
          {
            longest = length;
            mouth.coastEdge = use.edgeId;
          }
        }
    RiverGrowthSettings growth;
    growth.maximumNodes = 80;
    growth.coastClearanceMetres = 20;
    growth.separationMetres = 20;
    generated = growRiverGraph(physical, controls, {mouth}, growth, generatedRivers, &error) && generatedRivers.nodes.size() > 10 &&
                buildWatersheds(physical, generatedRivers, controls, true, generatedWatersheds, &error) && generatedSurface.build(physical, generatedRivers, generatedWatersheds, settings, &error);
  }
  check(generated, "Generated land coast river watershed and relief stages compose successfully");
  if (!generated)
    std::cerr << error << '\n';
  json generatedSamples = json::array();
  bool generatedParity = true;
  if (generated)
    for (int z = 0; z <= 40; ++z)
      for (int x = 0; x <= 40; ++x)
      {
        double a = 0, b = 0;
        bool indexed = generatedSurface.sample(x * 50, z * 50, a), brute = generatedSurface.sample(x * 50, z * 50, b, false);
        generatedParity &= indexed == brute && a == b;
        if (indexed)
          generatedSamples.push_back({x * 50, z * 50, a});
      }
  check(generated && generatedParity && !generatedSamples.empty(), "Generated terrain indexed and brute-force samples agree");
  PatchMesh mesh;
  std::vector<Position> meshSamples;
  for (int z = 1; z < 20; ++z)
    for (int x = 1; x < 20; ++x)
      meshSamples.push_back({x * 50., 0, z * 50.});
  // Shore elevation is constant, so registry split interpolation is exact here.
  // Sampling constraints removes long boundary fans without moving the coast.
  BoundaryRegistry meshRegistry;
  std::vector<BoundaryInput> meshEdges;
  for (auto edge : land.boundaries.records())
  {
    std::vector<double> splits;
    for (int i = 1; i < 20; ++i)
      splits.push_back(i / 20.);
    meshEdges.push_back({edge.id, {edge.vertices[0].index, edge.vertices[0].position}, {edge.vertices[1].index, edge.vertices[1].position}, splits});
  }
  meshRegistry.commit(meshEdges);
  bool meshed = compilePatchMesh(
      land.cells[0].patch,
      meshRegistry,
      meshSamples,
      [&](double x, double z)
      {
        double y = 0;
        return surface.sample(x, z, y) ? y : std::numeric_limits<double>::quiet_NaN();
      },
      mesh,
      &error);
  check(meshed && mesh.triangles.size() > 600, "Continuous relief evaluator drives an actual constrained patch mesh");
  json meshVertices = json::array();
  for (auto v : mesh.vertices)
    meshVertices.push_back({v.position.x, v.position.y, v.position.z});
  CompactTerrainField noisy;
  noisy.build({{1, 7, {0, 20, 0}, 100, 2, 20}});
  bool noiseBound = true;
  for (int z = -40; z <= 40; z += 5)
    for (int x = -40; x <= 40; x += 5)
    {
      double value;
      noiseBound &= noisy.sample(x, z, value) && value >= 18 && value <= 22;
    }
  check(noiseBound, "Continuous primitive noise stays inside declared amplitude bound");
  auto scaledLand = land;
  scaledLand.spec.extentMetres *= 2;
  scaledLand.landAreaMetres2 *= 4;
  scaledLand.totalAreaMetres2 *= 4;
  scaledLand.cells[0].areaMetres2 *= 4;
  scaledLand.coasts[0].signedAreaMetres2 *= 4;
  std::vector<BoundaryInput> scaledEdges;
  for (auto e : land.boundaries.records())
  {
    auto a = e.vertices[0], b = e.vertices[1];
    a.position.x *= 2;
    a.position.z *= 2;
    b.position.x *= 2;
    b.position.z *= 2;
    scaledEdges.push_back({e.id, {a.index, a.position}, {b.index, b.position}, {}});
  }
  scaledLand.boundaries.commit(scaledEdges);
  auto scaledRivers = rivers;
  for (auto &node : scaledRivers.nodes)
  {
    node.position.x *= 2;
    node.position.y *= 2;
    node.position.z *= 2;
  }
  WatershedProduct scaledWatersheds;
  check(
      buildWatersheds(scaledLand, scaledRivers, controls, false, scaledWatersheds, &error) && std::any_of(
                                                                                                  scaledWatersheds.vertices.begin(),
                                                                                                  scaledWatersheds.vertices.end(),
                                                                                                  [&](auto v)
                                                                                                  {
                                                                                                    return !v.coastal && std::abs(v.position.y - 2 * expected) < 1e-8;
                                                                                                  }),
      "Crest construction scales consistently in world metres");
  std::vector<procedural::world::VoronoiCell> alternatePartition = {{{250, 500}, {{0, 0}, {500, 0}, {500, 1000}, {0, 1000}}, {}}, {{750, 500}, {{500, 0}, {1000, 0}, {1000, 1000}, {500, 1000}}, {}}};
  LandSettings alternateSettings;
  alternateSettings.cores = {0};
  alternateSettings.landFraction = .999;
  LandProduct alternateLand;
  WatershedProduct alternateWatersheds;
  bool invariant = buildLandProduct(land.spec, alternatePartition, alternateSettings, alternateLand, &error) && buildWatersheds(alternateLand, rivers, controls, false, alternateWatersheds, &error);
  invariant &= alternateWatersheds.vertices.size() == watersheds.vertices.size() && alternateWatersheds.edges.size() == watersheds.edges.size();
  for (size_t i = 0; i < watersheds.vertices.size() && invariant; ++i)
  {
    auto a = watersheds.vertices[i], b = alternateWatersheds.vertices[i];
    invariant &= a.equidistantSites == b.equidistantSites && a.coastal == b.coastal && std::hypot(a.position.x - b.position.x, a.position.z - b.position.z) < 1e-8 && std::abs(a.position.y - b.position.y) < 1e-8;
  }
  check(invariant, "Crest identities and heights survive a different planning partition of the same land");
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", assertions},
    {"watersheds", encode(crossings)},
    {"samples", samples},
    {"primitiveCount", surface.field().primitives().size()},
    {"heightRange", {minH, maxH}},
    {"generatedWatersheds", encode(generatedWatersheds)},
    {"generatedSamples", generatedSamples},
    {"mesh", {{"vertices", meshVertices}, {"triangles", mesh.triangles}}}};
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << report.dump(2) << '\n';
    if (!f)
      return 2;
  }
  return failures ? 1 : 0;
}
