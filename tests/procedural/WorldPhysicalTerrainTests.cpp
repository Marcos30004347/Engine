#include "json.hpp"
#include "procedural/world/WorldPatchMesh.hpp"
#include "procedural/world/WorldPhysicalTerrain.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
int main(int argc, char **argv)
{
  int failures = 0;
  json checks = json::array();
  auto check = [&](bool ok, const char *s)
  {
    checks.push_back({{"name", s}, {"pass", ok}});
    std::cout << (ok ? "PASS " : "FAIL ") << s << '\n';
    failures += !ok;
  };
  std::string error;
  auto base = [](double, double)
  {
    return 0.;
  };
  SplineFeature f;
  f.heightMetres = 20;
  f.faceWidthMetres = 0;
  for (int i = 0; i < 12; ++i)
  {
    double a = i * 6.283185307179586 / 12;
    f.controls.push_back({400 + 100 * std::cos(a), 0, 300 + 100 * std::sin(a)});
  }
  WorldSplineLandforms land;
  check(land.build({f}, {.005, 1, 4096}), "Step 05 plateau fixture builds");
  AccessRamp ramp;
  ramp.toFeature = 1;
  ramp.stations = {{150, 0, 230}, {350, 20, 230}};
  WorldPhysicalTerrain terrain;
  check(terrain.build(land, base, {ramp}, {}, {}, &error), "20 m rise over 200 m meets 10 percent whole-ramp grade");
  auto shortRamp = ramp;
  shortRamp.stations[0].x = 160;
  check(!terrain.build(land, base, {shortRamp}, {}, {}, &error), "Shorter than H over grade ramp is rejected");
  PhysicalSample sample;
  bool exact = true;
  double maxGrade = 0, maxCross = 0;
  for (int x = 150; x <= 350; ++x)
    for (int z = 227; z <= 233; ++z)
    {
      exact &= terrain.sample(x, z, sample) && std::abs(sample.height - (x - 150) * .1) < 1e-8;
    }
  for (auto p : terrain.rampPanels())
  {
    maxGrade = std::max(maxGrade, std::abs(p.longitudinalGrade));
    maxCross = std::max(maxCross, std::abs(p.crossSlope));
  }
  check(exact && maxGrade <= .1 + 1e-9 && maxCross == 0, "Whole tread is a graded plane with zero inherited cliff cross slope");
  check(terrain.reachableFeatures().count(1), "Validated ramp publishes cap access edge");
  WorldPhysicalTerrain inaccessible;
  check(inaccessible.build(land, base, {}, {}, {}) && !inaccessible.reachableFeatures().count(1), "Mesa without ramp remains inaccessible in travel graph");
  auto wide = ramp;
  wide.halfWidthMetres = 30;
  check(!terrain.build(land, base, {wide}, {}, {}, &error), "Tread plus shoulder endpoint clearance rejects over-wide ramp");
  auto badEnd = ramp;
  badEnd.stations.back().y = 19;
  check(!terrain.build(land, base, {badEnd}, {}, {}, &error), "Ramp cannot terminate in an unjoined height step");
  bool shoulder = true;
  for (int x = 170; x < 340; x += 5)
  {
    PhysicalSample a, b;
    terrain.sample(x, 233, a);
    terrain.sample(x, 233 + 1e-6, b);
    shoulder &= std::abs(a.height - b.height) < 1e-5;
    terrain.sample(x, 237, b);
    SplineSurfaceSample old;
    land.sample(x, 237, base, old);
    shoulder &= b.height == old.height;
  }
  check(shoulder, "Shoulder blends meet tread continuously and original terrain at daylight limit");
  auto turn = ramp;
  turn.maximumGrade = .2;
  turn.stations = {{150, 0, 230}, {230, 10, 230}, {250, 10, 230}, {270, 10, 250}, {270, 10, 270}, {300, 10, 270}, {350, 20, 270}};
  check(terrain.build(land, base, {turn}, {}, {}, &error), "Turning ramp with flat landings validates every panel");
  auto hairpin = ramp;
  hairpin.stations = {{150, 0, 230}, {250, 10, 230}, {180, 15, 230}, {350, 20, 230}};
  check(!terrain.build(land, base, {hairpin}, {}, {}, &error), "Unsupported reversal requires designed landing instead of folded ramp");
  RiverGrowthProduct river;
  river.nodes = {{{600, -1.2, 330}, UINT32_MAX, 1, 1}, {{0, 0, 330}, 0, 1, 1}};
  RiverReservation reservation;
  reservation.upstreamNode = 1;
  reservation.bankTransitionMetres = 30;
  check(!terrain.build(land, base, {ramp}, river, {}, &error), "Plateau cannot obstruct fixed river without canyon reservation");
  check(terrain.build(land, base, {ramp}, river, {reservation}, &error), "Reserved canyon composes after bounded landform weathering and ramp");
  bool downstream = true;
  double previous = 1e20;
  for (int x = 0; x <= 600; ++x)
  {
    terrain.sample(x, 330, sample);
    double bed = -1 - .002 * x;
    downstream &= std::abs(sample.height - std::min(0., bed)) < 1e-6 || std::abs(sample.height - bed) < 1e-6;
    downstream &= sample.height <= previous + 1e-6;
    previous = sample.height;
  }
  // Outside the plateau the reservation is a ceiling on the base; inside it
  // opens the rock all the way to the declared bed. Step 07 owns final bed fill.
  bool opening = true;
  for (int x = 310; x < 480; ++x)
  {
    terrain.sample(x, 330, sample);
    opening &= sample.height <= -1 - .002 * x + 1e-8 && sample.protectedRiver;
  }
  check(downstream, "Reserved bed stays downhill to the fixed outlet");
  check(opening, "Reserved river cuts a real opening through plateau");
  check(terrain.rivers().nodes[1].receiver == 0 && terrain.rivers().nodes[0].position.y == -1.2, "Composition preserves receiver graph and outlet water level");
  auto uphill = river;
  uphill.nodes[0].position.y = 4;
  check(!terrain.build(land, base, {ramp}, uphill, {reservation}), "Uphill outlet condition rejected");
  auto shallow = reservation;
  shallow.maximumCutMetres = 5;
  check(!terrain.build(land, base, {ramp}, river, {shallow}), "Unsupported canyon depth rejected atomically");
  auto collide = river;
  for (auto &n : collide.nodes)
    n.position.z = 230;
  check(!terrain.build(land, base, {ramp}, collide, {reservation}), "Ramp river conflict requires bridge or ford");
  check(terrain.build(land, base, {ramp}, river, {reservation}), "Accepted physical product rebuilt");
  SplineFaceMesh faces;
  check(terrain.verticalFaces(faces, &error), "Vertical strips split and clipped by ramps and river cuts");
  bool cut = true;
  for (auto t : faces.triangles)
  {
    double z = 0, h = 0;
    for (auto i : t)
    {
      z += faces.vertices[i].z / 3;
      h += faces.vertices[i].y / 3;
    }
    if (std::abs(z - 330) < 7.5)
      cut &= h < 2;
  }
  check(cut, "No cliff face spans protected water opening");
  // Cap and lower ground use the exact subdivided face stations as CDT rims.
  std::vector<Position> top, low;
  for (size_t i = 0; i < faces.vertices.size(); i += 4)
  {
    top.push_back(faces.vertices[i]);
    low.push_back(faces.vertices[i + 1]);
  }
  top.push_back(top.front());
  low.push_back(low.front());
  BoundaryRegistry registry;
  std::vector<BoundaryInput> edges;
  uint64_t nextPoint = 1, nextEdge = 1;
  auto ring = [&](std::vector<Position> v)
  {
    std::vector<BoundaryEndpoint> ends;
    for (size_t k = 0; k + 1 < v.size(); ++k)
      ends.push_back({nextPoint++, v[k]});
    std::vector<BoundaryUse> use;
    for (size_t k = 0; k < ends.size(); ++k)
    {
      auto a = ends[k], b = ends[(k + 1) % ends.size()];
      use.push_back({nextEdge, a.id > b.id});
      edges.push_back({nextEdge++, a, b, {}});
    }
    return use;
  };
  auto upper = ring(top), lower = ring(low), outer = ring({{0, 0, 0}, {600, 0, 0}, {600, 0, 600}, {0, 0, 600}, {0, 0, 0}});
  bool meshOK = registry.commit(edges, &error);
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<Position> normals;
  for (int cap = 0; cap < 2; ++cap)
  {
    PatchDomain p;
    p.id = cap + 1;
    p.outer = cap ? upper : outer;
    if (!cap)
      p.holes = {lower};
    std::vector<Position> interior;
    for (int z = 3; z < 600; z += 6)
      for (int x = 3; x < 600; x += 6)
      {
        SplineStation station;
        land.station(1, x, z, station);
        if (cap ? station.signedDistanceMetres < -1e-5 : station.signedDistanceMetres > 1e-5)
          interior.push_back({double(x), 0, double(z)});
      }
    PatchMesh mesh;
    bool ok = compilePatchMesh(
        p,
        registry,
        interior,
        [&](double x, double z)
        {
          PhysicalSample s;
          terrain.sample(x, z, s);
          return s.height;
        },
        mesh,
        &error);
    meshOK &= ok;
    if (!ok)
      std::cerr << error << '\n';
    uint32_t offset = vertices.size();
    for (auto v : mesh.vertices)
      vertices.push_back(v.position);
    for (auto t : mesh.triangles)
    {
      triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
      normals.push_back({0, 1, 0});
    }
  }
  check(meshOK, "Physical terrain CDT preserves clipped upper and lower rims");
  uint32_t offset = vertices.size();
  vertices.insert(vertices.end(), faces.vertices.begin(), faces.vertices.end());
  for (size_t k = 0; k < faces.triangles.size(); ++k)
  {
    auto t = faces.triangles[k];
    triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
    normals.push_back(faces.expectedNormals[k]);
  }
  SplineFaceMesh input{vertices, triangles, normals}, constrained;
  std::vector<uint8_t> roles;
  check(constrainRampMesh(terrain.rampPanels(), input, constrained, roles, &error), "Ramp footprint cuts surrounding CDT into actual planar tread patches");
  if (!error.empty())
    std::cerr << "ramp mesh: " << error << "\n";
  double rampArea = 0;
  bool planar = true;
  for (size_t i = 0; i < constrained.triangles.size(); ++i)
    if (roles[i] == 2)
    {
      auto t = constrained.triangles[i];
      auto a = constrained.vertices[t[0]], b = constrained.vertices[t[1]], c = constrained.vertices[t[2]];
      rampArea += std::abs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x)) * .5;
      for (auto p : {a, b, c})
        planar &= std::abs(p.y - (p.x - 150) * .1) < 1e-8;
    }
  check(planar && std::abs(rampArea - 1200) < 1e-6, "Rendered ramp covers its full 200 by 6 metre footprint on the exact design plane");
  vertices = constrained.vertices;
  triangles = constrained.triangles;
  normals = constrained.expectedNormals;
  json positions = json::array(), expected = json::array(), rampSection = json::array(), riverSection = json::array(), across = json::array();
  for (auto p : vertices)
    positions.push_back({p.x, p.y, p.z});
  for (auto p : normals)
    expected.push_back({p.x, p.y, p.z});
  for (int x = 0; x <= 600; ++x)
  {
    PhysicalSample a;
    SplineSurfaceSample b;
    terrain.sample(x, 230, a);
    land.sample(x, 230, base, b);
    rampSection.push_back({x, b.height, a.height});
    terrain.sample(x, 330, a);
    riverSection.push_back({x, a.height, -.002 * x});
  }
  for (int z = 180; z <= 380; ++z)
  {
    PhysicalSample a;
    SplineSurfaceSample b;
    terrain.sample(400, z, a);
    land.sample(400, z, base, b);
    across.push_back({z, b.height, a.height});
  }
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", checks},
    {"phase", 6},
    {"mesh", {{"vertices", positions}, {"triangles", triangles}, {"expectedNormals", expected}, {"roles", roles}}},
    {"rampSection", rampSection},
    {"riverSection", riverSection},
    {"across", across},
    {"maximumGrade", maxGrade},
    {"maximumCrossSlope", maxCross}};
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    file << report.dump(2) << '\n';
  }
  std::cout << "METRIC max_grade " << maxGrade << " cross_slope " << maxCross << " triangles " << triangles.size() << '\n';
  return failures ? 1 : 0;
}
