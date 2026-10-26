#include "json.hpp"
#include "procedural/world/WorldReliefSurface.hpp"
#include "procedural/world/WorldRidgeFeatures.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
LandProduct square()
{
  LandProduct l;
  l.spec.extentMetres = 2000;
  l.landAreaMetres2 = l.totalAreaMetres2 = 4000000;
  l.components = 1;
  BoundaryEndpoint a{1, {0, 0, 0}}, b{2, {2000, 0, 0}}, c{3, {2000, 0, 2000}}, d{4, {0, 0, 2000}};
  l.boundaries.commit({{1, a, b, {}}, {2, b, c, {}}, {3, c, d, {}}, {4, d, a, {}}});
  PatchDomain p;
  p.id = 1;
  p.outer = {{1, false}, {2, false}, {3, false}, {4, true}};
  l.cells.push_back({0, true, 4000000, p});
  l.coasts.push_back({p.outer, 4000000});
  return l;
}
json encode(const RidgeFeatures &f, const std::vector<Position> &vertices)
{
  json peaks = json::array(), passes = json::array();
  for (auto p : f.peaks)
    if (p.significant)
    {
      auto v = vertices[p.vertex];
      peaks.push_back({{"vertex", p.vertex}, {"position", {v.x, v.y, v.z}}, {"prominence", p.prominenceMetres}, {"parent", p.parentPeak}, {"saddle", p.keySaddle}});
    }
  for (auto p : f.passes)
  {
    auto v = vertices[p.vertex];
    passes.push_back({{"position", {v.x, v.y, v.z}}, {"lowerPeak", p.lowerPeak}, {"higherPeak", p.higherPeak}, {"prominence", p.prominenceMetres}});
  }
  return {{"peaks", peaks}, {"passes", passes}, {"components", f.components}};
}
struct Sampled
{
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 2>> edges;
  RidgeFeatures features;
};
Sampled sample(const WorldReliefSurface &surface, int intervals)
{
  Sampled out;
  for (int z = 0; z <= intervals; ++z)
    for (int x = 0; x <= intervals; ++x)
    {
      double y;
      if (!surface.sample(x * 2000. / intervals, z * 2000. / intervals, y))
        throw std::runtime_error("Unsupported acceptance sample");
      out.vertices.push_back({x * 2000. / intervals, y, z * 2000. / intervals});
      uint32_t i = uint32_t(out.vertices.size() - 1);
      if (x)
        out.edges.push_back({i - 1, i});
      if (z)
        out.edges.push_back({i - uint32_t(intervals + 1), i});
    }
  std::string error;
  if (!analyzeReliefTopology(out.vertices, out.edges, 0, 8, out.features, &error))
    throw std::runtime_error(error);
  return out;
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
  std::string error;
  std::vector<Position> known = {{0, 100, 0}, {1, 40, 0}, {2, 80, 0}, {3, 0, 0}};
  std::vector<std::array<uint32_t, 2>> edges = {{0, 1}, {1, 2}, {1, 3}};
  RidgeFeatures features;
  check(
      analyzeReliefTopology(known, edges, 0, 10, features, &error) && features.peaks.size() == 2 && features.passes.size() == 1 && features.passes[0].heightMetres == 40 && features.passes[0].prominenceMetres == 40,
      "Analytical two-summit graph yields 40 m key saddle prominence");
  auto plateau = known;
  plateau.push_back({0, 100, 1});
  auto plateauEdges = edges;
  plateauEdges.push_back({0, 4});
  RidgeFeatures plateauFeatures;
  check(
      analyzeReliefTopology(plateau, plateauEdges, 0, 10, plateauFeatures, &error) && plateauFeatures.peaks.size() == 2 && plateauFeatures.peaks[0].vertex == 0,
      "Connected equal-height summit plateau has one canonical peak");
  auto badEdges = edges;
  badEdges.push_back({0, 99});
  auto prior = encode(features, known);
  check(!analyzeReliefTopology(known, badEdges, 0, 10, features, &error) && encode(features, known) == prior, "Invalid ridge topology rejected atomically");
  std::reverse(edges.begin(), edges.end());
  for (auto &edge : edges)
    std::swap(edge[0], edge[1]);
  RidgeFeatures permuted;
  check(analyzeReliefTopology(known, edges, 0, 10, permuted, &error) && encode(permuted, known) == prior, "Edge order and orientation do not change prominence hierarchy");
  auto land = square();
  RiverGrowthProduct rivers;
  for (double x : {200., 1000., 1800.})
    for (int z = 0; z <= 9; ++z)
    {
      uint32_t i = uint32_t(rivers.nodes.size());
      rivers.nodes.push_back({{x, z * 200 * .006, z * 200.}, z ? i - 1 : UINT32_MAX, 1, 1});
    }
  ReliefControls controls;
  std::vector<ReliefProvince> provinces;
  uint64_t id = 0;
  for (double x : {600., 1400.})
    for (double z : {500., 1500.})
      provinces.push_back({++id, {x, 0, z - 150}, {x, 0, z + 150}, 350, .01, .24});
  controls.build(provinces, {.001, .005});
  WatershedProduct watersheds;
  bool built = buildWatersheds(land, rivers, controls, false, watersheds, &error);
  check(built && !watersheds.sourceGraphTruncated, "Multi-range fixture uses complete authored drainage with no truncation");
  if (!built)
  {
    std::cerr << error << '\n';
    return 1;
  }
  RidgeFeatures ridges;
  std::vector<Position> ridgeVertices;
  for (auto v : watersheds.vertices)
    ridgeVertices.push_back(v.position);
  check(
      analyzeRidgeFeatures(watersheds, 0, 8, ridges, &error) && encode(ridges, ridgeVertices)["peaks"].size() >= 4 && ridges.passes.size() >= 2,
      "Watershed ridge network has significant summits and passes in both ranges");
  WorldReliefSurface surface;
  ReliefSurfaceSettings settings;
  settings.sampleSpacingMetres = 75;
  settings.roughnessFraction = 0;
  check(surface.build(land, rivers, watersheds, settings, &error), "Multi-range continuous terrain builds without noise maxima");
  auto dartSettings = settings;
  dartSettings.poissonSamplesPerCell = 50;
  WorldReliefSurface dartSurface, repeatedDartSurface;
  bool darts = dartSurface.build(land, rivers, watersheds, dartSettings, &error) && repeatedDartSurface.build(land, rivers, watersheds, dartSettings, &error);
  check(darts, "Watershed dart supports build with expanded domain coverage");
  bool dartCoverage = darts, dartRepeat = darts;
  if (darts)
  {
    const auto &a = dartSurface.field().primitives(), &b = repeatedDartSurface.field().primitives();
    dartRepeat = a.size() == b.size() && a.size() <= 50 * watersheds.cells.size();
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
      dartRepeat &= a[i].centre.x == b[i].centre.x && a[i].centre.y == b[i].centre.y && a[i].centre.z == b[i].centre.z && a[i].radiusMetres == b[i].radiusMetres;
    for (int z = 0; z <= 20; ++z)
      for (int x = 0; x <= 20; ++x)
      {
        double accelerated = 0, reference = 0;
        dartCoverage &= dartSurface.sample(x * 100., z * 100., accelerated) && dartSurface.sample(x * 100., z * 100., reference, false) && std::abs(accelerated - reference) < 1e-9;
      }
  }
  check(dartRepeat, "Dart support selection and radii are deterministic and bounded per watershed");
  check(dartCoverage, "Dart supports cover dense interior and shoreline queries with exact tree parity");
  Sampled coarse = sample(surface, 40), fine = sample(surface, 80);
  auto coarseFeatures = encode(coarse.features, coarse.vertices), fineFeatures = encode(fine.features, fine.vertices);

  check(coarseFeatures["peaks"].size() >= 4 && coarseFeatures["passes"].size() >= 2, "Sampled terrain retains four prominent peaks and connecting passes");
  bool stable = coarseFeatures["peaks"].size() == fineFeatures["peaks"].size();
  for (auto p : coarseFeatures["peaks"])
  {
    bool found = false;
    for (auto q : fineFeatures["peaks"])
    {
      double dx = double(p["position"][0]) - double(q["position"][0]), dz = double(p["position"][2]) - double(q["position"][2]);
      if (std::hypot(dx, dz) <= 100 && std::abs(double(p["position"][1]) - double(q["position"][1])) <= 3)
        found = true;
    }
    stable &= found;
  }
  check(stable, "Peak arrangement stable within 100 m and 3 m height at 50 m and 25 m sampling");
  bool twoRanges = true;
  for (double x : {600., 1400.})
    for (double z : {500., 1500.})
    {
      double mountain = 0, valley = 0;
      twoRanges &= surface.sample(x, z, mountain) && surface.sample(1000, z, valley) && mountain - valley > 15;
    }
  check(twoRanges, "Both authored ranges rise at least 15 m above their shared drainage valley");
  double maximumJump = 0;
  for (auto edge : watersheds.edges)
  {
    auto a = watersheds.vertices[edge.a].position, b = watersheds.vertices[edge.b].position;
    double len = std::hypot(b.x - a.x, b.z - a.z);
    if (len <= 0)
      continue;
    double x = (a.x + b.x) * .5, z = (a.z + b.z) * .5, l = 0, r = 0;
    if (surface.sample(x - (b.z - a.z) / len * 1e-4, z + (b.x - a.x) / len * 1e-4, l) && surface.sample(x + (b.z - a.z) / len * 1e-4, z - (b.x - a.x) / len * 1e-4, r))
      maximumJump = std::max(maximumJump, std::abs(l - r));
  }
  check(maximumJump < .001, "No height discontinuity across any hydrological boundary");
  std::vector<BoundaryInput> meshEdges;
  for (auto edge : land.boundaries.records())
  {
    std::vector<double> splits;
    for (int i = 1; i < 40; ++i)
      splits.push_back(i / 40.);
    meshEdges.push_back({edge.id, {edge.vertices[0].index, edge.vertices[0].position}, {edge.vertices[1].index, edge.vertices[1].position}, splits});
  }
  BoundaryRegistry registry;
  registry.commit(meshEdges);
  std::vector<Position> meshSamples;
  for (int z = 1; z < 40; ++z)
    for (int x = 1; x < 40; ++x)
      meshSamples.push_back({x * 50., 0, z * 50.});
  PatchMesh mesh;
  check(
      compilePatchMesh(
          land.cells[0].patch,
          registry,
          meshSamples,
          [&](double x, double z)
          {
            double h;
            return surface.sample(x, z, h) ? h : std::numeric_limits<double>::quiet_NaN();
          },
          mesh,
          &error) &&
          mesh.triangles.size() == 3200,
      "Acceptance terrain compiles to 3200 CDT triangles with preserved shoreline");
  bool shoreline = true;
  for (auto vertex : registry.records())
    for (auto p : vertex.vertices)
    {
      double h = 99;
      shoreline &= surface.sample(p.position.x, p.position.z, h) && h == 0;
    }
  check(shoreline, "Every exported shore station joins the declared water level exactly");
  json vertices = json::array();
  for (auto v : mesh.vertices)
    vertices.push_back({v.position.x, v.position.y, v.position.z});
  json section = json::array();
  for (int z = 0; z <= 80; ++z)
  {
    double y;
    surface.sample(600, z * 25, y);
    section.push_back({z * 25, y});
  }
  RiverGrowthProduct capacity;
  for (int z = 0; z < 64; ++z)
    for (int x = 0; x < 64; ++x)
      capacity.nodes.push_back({{(x + .5) * 2000 / 64, 0, (z + .5) * 2000 / 64}, UINT32_MAX, 1, 1});
  WatershedProduct capacityWatersheds;
  auto start = std::chrono::steady_clock::now();
  bool capacityOK = buildWatersheds(land, capacity, controls, false, capacityWatersheds, &error);
  double capacitySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  check(
      capacityOK && capacityWatersheds.cells.size() == 4096 && std::abs(capacityWatersheds.landAreaMetres2 - 4000000) < 1e-5 && capacitySeconds < 30,
      "Supported 4096-site watershed budget builds within 30 seconds and preserves coverage");
  WorldReliefSurface capacitySurface;
  auto surfaceStart = std::chrono::steady_clock::now();
  bool capacitySurfaceOK = capacityOK && capacitySurface.build(land, capacity, capacityWatersheds, settings, &error);
  double capacitySurfaceSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - surfaceStart).count();
  double capacityHeight = 0;
  check(capacitySurfaceOK && capacitySurface.sample(1000, 1000, capacityHeight) && capacitySurfaceSeconds < 30, "Full-budget watershed product builds a supported terrain surface within 30 seconds");
  capacity.nodes.push_back({{100, 0, 100}});
  auto capacityArea = capacityWatersheds.landAreaMetres2;
  check(!buildWatersheds(land, capacity, controls, false, capacityWatersheds, &error) && capacityWatersheds.landAreaMetres2 == capacityArea, "Above-budget input rejects without replacing the valid product");
  WatershedProduct noRidges;
  noRidges.vertices = {{{0, 100, 0}, false, {}}, {{1, 0, 0}, false, {}}};
  noRidges.edges = {{0, 1, 0, 1, true}};
  RidgeFeatures noFeatures;
  check(analyzeRidgeFeatures(noRidges, 0, 8, noFeatures, &error) && noFeatures.peaks.empty() && noFeatures.components == 0, "River-only vertices do not become spurious isolated ridge peaks");
  auto flatRivers = rivers;
  for (auto &node : flatRivers.nodes)
  {
    node.position.y = 0;
    node.receiver = UINT32_MAX;
  }
  ReliefControls flatControls;
  flatControls.build({}, {.001, 0});
  WatershedProduct flatWatersheds;
  WorldReliefSurface flatSurface;
  bool flat = buildWatersheds(land, flatRivers, flatControls, false, flatWatersheds, &error) && flatSurface.build(land, flatRivers, flatWatersheds, settings, &error);
  if (flat)
  {
    auto sampled = sample(flatSurface, 20);
    flat = encode(sampled.features, sampled.vertices)["peaks"].empty() && std::all_of(
                                                                              sampled.vertices.begin(),
                                                                              sampled.vertices.end(),
                                                                              [](auto p)
                                                                              {
                                                                                return p.y == 0;
                                                                              });
  }
  check(flat, "Flat preset remains planar and has no invented prominent mountains");
  ReliefControls massifControls;
  massifControls.build({provinces.front()}, {.001, 0});
  WatershedProduct massifWatersheds;
  WorldReliefSurface massifSurface;
  bool massif = buildWatersheds(land, flatRivers, massifControls, false, massifWatersheds, &error) && massifSurface.build(land, flatRivers, massifWatersheds, settings, &error);
  if (massif)
  {
    auto sampled = sample(massifSurface, 40);
    massif = encode(sampled.features, sampled.vertices)["peaks"].size() == 1;
  }
  check(massif, "Single-massif preset produces one significant summit without multi-range assumptions");
  std::cout << "METRIC capacity_seconds " << capacitySeconds << '\n';
  std::cout << "METRIC capacity_surface_seconds " << capacitySurfaceSeconds << '\n';
  json crestPositions = json::array(), crestEdges = json::array();
  for (auto v : watersheds.vertices)
    crestPositions.push_back({v.position.x, v.position.y, v.position.z});
  for (auto e : watersheds.edges)
    crestEdges.push_back({e.a, e.b, e.riverCrossing ? 1u : 0u});
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", assertions},
    {"mesh", {{"vertices", vertices}, {"triangles", mesh.triangles}}},
    {"coarseFeatures", coarseFeatures},
    {"fineFeatures", fineFeatures},
    {"ridgeFeatures", encode(ridges, ridgeVertices)},
    {"section", section},
    {"maximumBoundaryJump", maximumJump},
    {"primitiveCount", surface.field().primitives().size()},
    {"capacitySites", 4096},
    {"capacityPrimitiveCount", capacitySurface.field().primitives().size()},
    {"crestPositions", crestPositions},
    {"crestEdges", crestEdges}};
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << report.dump(2) << '\n';
    if (!f)
      return 2;
  }
  return failures ? 1 : 0;
}
