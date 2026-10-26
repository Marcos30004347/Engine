#include "json.hpp"
#include "procedural/world/WorldPatchMesh.hpp"
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
using namespace procedural::world::rewrite;
using nlohmann::json;
json encode(const PatchMesh &mesh)
{
  json vertices = json::array(), constraints = json::array();
  for (const auto &v : mesh.vertices)
    vertices.push_back({v.position.x, v.position.y, v.position.z});
  for (const auto &e : mesh.constraints)
    constraints.push_back({e.sourceEdge, e.a, e.b});
  return {{"id", mesh.patchId}, {"vertices", vertices}, {"triangles", mesh.triangles}, {"constraints", constraints}, {"area", mesh.domainArea}, {"triangleArea", mesh.triangleArea}};
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
  // A 100x100 parcel with a 20x20 reserved hole, and its road neighbor.
  BoundaryEndpoint a{1, {0, 10, 0}}, b{2, {100, 10, 0}}, c{3, {100, 15, 100}}, d{4, {0, 15, 100}};
  BoundaryEndpoint e{5, {120, 10.4, 0}}, f{6, {120, 15.4, 100}};
  BoundaryEndpoint h0{7, {40, 12, 40}}, h1{8, {60, 12, 40}}, h2{9, {60, 13, 60}}, h3{10, {40, 13, 60}};
  BoundaryRegistry registry;
  std::string error;
  check(
      registry.commit({{1, a, b, {}}, {2, b, c, {.25, .5, .75}}, {3, c, d, {}}, {4, d, a, {}}, {5, b, e, {}}, {6, e, f, {}}, {7, f, c, {}}, {8, h0, h1, {}}, {9, h1, h2, {}}, {10, h2, h3, {}}, {11, h3, h0, {}}}, &error),
      "canonical parcel and road boundary registry");
  PatchDomain land{1, {{1, false}, {2, false}, {3, false}, {4, true}}, {{{8, false}, {9, false}, {10, false}, {11, true}}}};
  PatchDomain road{2, {{5, false}, {6, false}, {7, true}, {2, true}}, {}};
  PatchMesh landMesh, roadMesh;
  auto landHeight = [](double, double z)
  {
    return 10 + .05 * z;
  };
  RoadDesignProfile design;
  check(design.build({{0, 10.2, 10, .02}, {100, 15.2, 10, .02}}, .10, .02, &error), "road design profile feeds patch compiler");
  auto roadHeight = [&](double x, double z)
  {
    double height = 0;
    return design.height(z, x - 110, height) ? height : std::numeric_limits<double>::quiet_NaN();
  };
  bool landOk = compilePatchMesh(land, registry, {{20, 0, 20}, {80, 0, 80}}, landHeight, landMesh, &error);
  check(landOk, "CDT land parcel with reserved hole");
  if (!landOk)
    std::cerr << error << '\n';
  bool roadOk = compilePatchMesh(road, registry, {{110, 0, 50}}, roadHeight, roadMesh, &error);
  check(roadOk, "inclined road is an independent CDT mesh");
  if (!roadOk)
    std::cerr << error << '\n';
  check(landOk && landMesh.domainArea == 9600 && std::abs(landMesh.triangleArea - 9600) < 1e-8, "hole remains excluded from parcel area");
  bool heights = roadOk;
  for (const auto &v : roadMesh.vertices)
    heights &= std::abs(v.position.y - roadHeight(v.position.x, v.position.z)) < 1e-12;
  check(heights, "road mesh follows grade and cross slope design plane");
  bool shared = true;
  size_t matched = 0;
  for (const auto &v : landMesh.vertices)
    for (const auto &w : roadMesh.vertices)
      if (v.owner == w.owner && v.index == w.index && v.owner != UINT64_MAX)
      {
        ++matched;
        shared &= v.position.x == w.position.x && v.position.y == w.position.y && v.position.z == w.position.z;
      }
  check(shared && matched == 5, "independent patch meshes share all five seam stations exactly");
  bool winding = true;
  for (const auto *m : {&landMesh, &roadMesh})
    for (const auto &t : m->triangles)
    {
      const auto a = m->vertices[t[0]].position, b = m->vertices[t[1]].position, c = m->vertices[t[2]].position;
      winding &= (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z) > 0;
    }
  check(winding && !landMesh.triangles.empty(), "all patch triangles face upward");
  std::vector<std::future<json>> jobs;
  for (int i = 0; i < 8; ++i)
    jobs.push_back(
        std::async(
            std::launch::async,
            [&]
            {
              PatchMesh m;
              if (!compilePatchMesh(land, registry, {{20, 0, 20}, {80, 0, 80}}, landHeight, m))
                return json();
              return encode(m);
            }));
  bool repeat = true;
  for (auto &j : jobs)
    repeat &= j.get() == encode(landMesh);
  check(repeat, "parallel independent CDT builds preserve patch output");
  const auto saved = encode(landMesh);
  auto bad = land;
  bad.outer[1].reversed = true;
  check(!compilePatchMesh(bad, registry, {}, landHeight, landMesh, &error) && encode(landMesh) == saved, "disconnected boundary rejected without replacing valid mesh");
  check(!compilePatchMesh(land, registry, {{50, 0, 50}}, landHeight, landMesh, &error), "sample in reserved hole rejected");
  check(!compilePatchMesh(land, registry, {{100, 0, 33}}, landHeight, landMesh, &error), "unregistered seam insertion rejected");
  BoundaryEndpoint q0{21, {0, 0, 0}}, q1{22, {10, 0, 10}}, q2{23, {0, 0, 10}}, q3{24, {10, 0, 0}};
  BoundaryRegistry crossed;
  crossed.commit({{21, q0, q1, {}}, {22, q1, q2, {}}, {23, q2, q3, {}}, {24, q3, q0, {}}});
  PatchDomain bow{3, {{21, false}, {22, false}, {23, false}, {24, true}}, {}};
  check(!compilePatchMesh(bow, crossed, {}, landHeight, landMesh, &error), "self-crossing patch rejected before triangulation");
  if (argc == 2)
  {
    std::ofstream out(argv[1]);
    out << json({{"schema", 1}, {"seed", 7}, {"status", failures ? "FAIL" : "PASS"}, {"assertions", assertions}, {"meshes", {saved, encode(roadMesh)}}}).dump(2) << '\n';
    if (!out)
      return 2;
  }
  return failures ? 1 : 0;
}
