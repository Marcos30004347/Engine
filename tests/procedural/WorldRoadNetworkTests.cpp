#include "json.hpp"
#include "procedural/world/WorldSettlementRoads.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
using namespace procedural::world::rewrite;
using nlohmann::json;
int main(int argc, char **argv)
{
  json assertions = json::array(), scenes = json::array();
  int failures = 0;
  auto check = [&](bool pass, const char *name)
  {
    assertions.push_back({{"name", name}, {"pass", pass}});
    std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
    failures += !pass;
  };
  auto dump = [&](const char *name, const RoadProduct &p, const RoadField &ground)
  {
    json sections = json::array(), triangles = json::array(), samples = json::array();
    for (auto &s : p.sections)
    {
      json points = json::array();
      for (auto &q : s.points)
        points.push_back({q.x, q.y, q.z});
      sections.push_back({{"center", {s.center.x, s.center.y, s.center.z}}, {"distance", s.distanceMetres}, {"ground", s.naturalHeightMetres}, {"points", points}});
    }
    for (auto &t : p.triangles)
    {
      json points = json::array();
      for (auto &q : t.points)
        points.push_back({q.x, q.y, q.z});
      triangles.push_back({{"points", points}, {"shoulder", t.shoulder}});
    }
    for (int z = -70; z <= 70; z += 2)
      for (int x = 0; x <= 240; x += 2)
      {
        double h = ground(x, z);
        samples.push_back({x, z, std::isfinite(h) ? json(h) : json()});
      }
    scenes.push_back(
        {{"name", name},
         {"sections", sections},
         {"triangles", triangles},
         {"terrain", samples},
         {"maximumGrade", p.maximumGrade},
         {"maximumGradeChange", p.maximumGradeChange},
         {"maximumEarthwork", p.maximumEarthwork},
         {"statesVisited", p.statesVisited},
         {"cost", p.routeCost}});
  };
  std::string error;
  auto zero = [](double, double)
  {
    return 0.;
  };
  double inf = std::numeric_limits<double>::infinity();
  RoadSettings s;
  s.crownSlope = 0;
  RoadProduct flat;
  bool ok = routeRoad({0, 0, 0}, {240, 0, 0}, zero, zero, s, flat, &error);
  std::cout << error << '\n';
  check(ok, "flat routed road compiles through CDT");
  check(flat.sections.size() == 61 && flat.triangles.size() == 480, "road crown and shoulders have explicit constrained panels");
  double y = 99;
  check(flat.height(100, 2, y) && std::abs(y) < 1e-10, "surface query inside flat road");
  check(!flat.height(100, 5, y) && !flat.height(-1, 0, y), "surface rejects outside width and endpoints");
  bool normals = !flat.triangles.empty();
  for (auto &t : flat.triangles)
  {
    auto &a = t.points[0];
    auto &b = t.points[1];
    auto &c = t.points[2];
    double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    normals &= ny > 0 && std::abs(nx) + std::abs(nz) < 1e-8;
  }
  check(normals, "flat triangle normals point up");
  auto hill = [](double x, double z)
  {
    return .05 * x + .30 * z;
  };
  RoadProduct incline;
  check(routeRoad({0, 0, 0}, {240, 12, 0}, hill, zero, s, incline, &error), "five percent road on thirty percent hillside");
  bool plane = !incline.triangles.empty();
  for (auto &t : incline.triangles)
    for (auto &p : t.points)
      plane &= std::abs(p.y - .05 * p.x) < 1e-7;
  check(plane, "inclined panels retain zero transverse grade");
  check(std::abs(incline.maximumGrade - .05) < 1e-8 && std::abs(incline.maximumEarthwork - 1.2) < 1e-7, "full-width earthwork captures hillside bench requirement");
  dump("hillside-level-crossfall", incline, hill);
  auto cross = s;
  cross.crossSlope = .02;
  RoadProduct crossfall;
  check(routeRoad({0, 0, 0}, {240, 12, 0}, hill, zero, cross, crossfall, &error), "explicit two percent crossfall design");
  plane = !crossfall.triangles.empty();
  for (auto &t : crossfall.triangles)
    for (auto &p : t.points)
      plane &= std::abs(p.y - (.05 * p.x + .02 * p.z)) < 1e-7;
  check(plane, "two percent local plane at all triangle corners");
  bool interior = true;
  for (auto &t : crossfall.triangles)
  {
    Position p{};
    for (auto &q : t.points)
    {
      p.x += q.x / 3;
      p.z += q.z / 3;
    }
    interior &= crossfall.height(p.x, p.z, y) && std::abs(y - (.05 * p.x + .02 * p.z)) < 1e-7;
  }
  check(interior, "interior barycentric queries match designed plane");
  auto crown = s;
  crown.crownSlope = .02;
  RoadProduct crowned;
  check(routeRoad({0, 0, 0}, {240, 12, 0}, hill, zero, crown, crowned, &error), "crowned road has independent left and right planes");
  plane = !crowned.triangles.empty();
  for (auto &t : crowned.triangles)
    for (auto &p : t.points)
      plane &= std::abs(p.y - (.05 * p.x - .02 * std::abs(p.z))) < 1e-7;
  check(plane, "crown edge preserved without triangles crossing it");
  dump("hillside-crowned", crowned, hill);
  auto obstacle = [&](double x, double z)
  {
    if (x > 95 && x < 145 && z > -15 && z < 15)
      return inf;
    return .012 * x + 1.5 * std::sin(x * .023) * std::sin(z * .03);
  };
  auto risk = [](double x, double z)
  {
    return 1.5 * std::exp(-std::pow((z + 12) / 22, 2) - std::pow((x - 120) / 75, 2));
  };
  RoadProduct detour;
  auto detourSettings = crown;
  detourSettings.smoothingPasses = 8;
  detourSettings.minimumBendRadiusMetres = 8;
  ok = routeRoad({0, obstacle(0, 0), 0}, {240, obstacle(240, 0), 0}, obstacle, risk, detourSettings, detour, &error);
  std::cout << "detour: " << error << '\n';
  check(ok, "blocked ridge yields a validated terrain-aware detour");
  double offset = 0;
  for (auto &q : detour.sections)
    offset = std::max(offset, std::abs(q.center.z));
  check(offset > 19, "detour clears obstacle by full road width");
  if (ok)
    dump("ridge-detour", detour, obstacle);
  RoadProduct repeat;
  check(
      routeRoad({0, obstacle(0, 0), 0}, {240, obstacle(240, 0), 0}, obstacle, risk, detourSettings, repeat, &error) && repeat.routeCost == detour.routeCost && repeat.sections.size() == detour.sections.size(),
      "repeat route is deterministic");
  auto wall = [&](double x, double)
  {
    return x > 95 && x < 145 ? inf : 0.;
  };
  RoadProduct preserved = flat;
  check(!routeRoad({0, 0, 0}, {240, 0, 0}, wall, zero, s, preserved, &error) && preserved.triangles.size() == flat.triangles.size(), "blocked corridor fails atomically without straight fallback");
  auto edgeHazard = [&](double, double z)
  {
    return z > 2.5 ? inf : 0.;
  };
  std::vector<Position> path;
  for (int x = 0; x <= 100; x += 4)
    path.push_back({double(x), 0, 0});
  check(!designRoad(path, {}, edgeHazard, zero, s, preserved, &error), "hazard under shoulder rejects clear centerline");
  path.back().y = 30;
  check(!designRoad(path, {}, zero, zero, s, preserved, &error), "impossible endpoint grade and cut fail");
  path.back().y = 0;
  check(designRoad(path, {{12, 1.}}, zero, zero, s, repeat, &error) && std::abs(repeat.sections[12].center.y - 1) < 1e-9, "interior gate elevation remains locked");
  check(!designRoad(path, {{0, 1.}}, zero, zero, s, preserved, &error), "conflicting endpoint lock rejected");
  auto bump = [](double x, double)
  {
    return 2 * std::exp(-std::pow((x - 50) / 3, 2));
  };
  RoadProduct graded;
  check(
      designRoad(path, {}, bump, zero, s, graded, &error) && graded.maximumGrade <= s.maximumGrade + 1e-6 && graded.maximumGradeChange <= s.maximumGradeChangePerMetre + 1e-6,
      "profile solver cuts bump under grade and grade-change limits");
  dump("graded-bump", graded, bump);
  auto tight = s;
  tight.maximumEarthworkMetres = .1;
  check(!routeRoad({0, 0, 0}, {240, 12, 0}, hill, zero, tight, preserved, &error), "full-width excessive earthwork rejected");
  auto bad = s;
  bad.spacingMetres = 0;
  check(!routeRoad({0, 0, 0}, {240, 0, 0}, zero, zero, bad, preserved, &error), "invalid search settings rejected");
  check(!routeRoad({0, 0, 0}, {1, 0, 0}, zero, zero, s, preserved, &error), "short impossible road rejected");
  check(
      !designRoad(
          path,
          {},
          zero,
          [](double, double)
          {
            return -1.;
          },
          s,
          preserved,
          &error),
      "negative resistance rejected");
  std::vector<RoadAnchor> anchors = {{4, 1, {30, 0, 20}, false}, {2, 0, {200, 0, 0}, true}, {1, 0, {0, 0, 0}, true}, {3, 0, {400, 0, 0}, true}};
  std::vector<RoadLink> links;
  check(selectRoadLinks(anchors, links, &error) && links.size() == 3, "town spanning tree and village-parent links selected");
  check(links.size() == 3 && links[0].from == 1 && links[0].to == 2 && links[1].from == 1 && links[1].to == 4 && links[2].from == 2 && links[2].to == 3, "network selection has deterministic required topology");
  std::reverse(anchors.begin(), anchors.end());
  std::vector<RoadLink> reordered;
  check(selectRoadLinks(anchors, reordered, &error) && reordered.size() == links.size() && reordered[0].from == links[0].from, "anchor input order does not change links");
  anchors.back().parentTown = 99;
  check(!selectRoadLinks(anchors, reordered, &error), "missing village parent rejected");

  std::map<uint32_t, Position> shared;
  bool consistent = true;
  for (auto &t : detour.triangles)
    for (size_t k = 0; k < 3; ++k)
    {
      auto [it, added] = shared.emplace(t.vertexIds[k], t.points[k]);
      if (!added)
        consistent &= std::hypot(it->second.x - t.points[k].x, it->second.z - t.points[k].z) < 1e-9 && std::abs(it->second.y - t.points[k].y) < 1e-9;
    }
  check(consistent && shared.size() == detour.sections.size() * 5, "adjacent CDT panels share exact road-edge identities");
  bool inclinedNormals = true;
  for (auto &t : incline.triangles)
  {
    auto a = t.points[0], b = t.points[1], c = t.points[2];
    double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    inclinedNormals &= ny > 0 && std::abs(nx / ny + .05) < 1e-8 && std::abs(nz / ny) < 1e-8;
  }
  check(inclinedNormals, "inclined triangle normals match longitudinal design grade");
  std::vector<RoadAnchor> pair = {{1, 0, {0, 0, 0}, true}, {2, 1, {240, 0, 0}, false}};
  RoadNetworkProduct network;
  check(buildRoadNetwork(pair, zero, zero, s, 7, network, &error) && network.roads.size() == 1 && network.dependencyRevision == 7, "required road network publishes with dependency revision");
  check(!buildRoadNetwork(pair, wall, zero, s, 8, network, &error) && network.dependencyRevision == 7, "failed required link preserves previous entire network");
  check(!buildRoadNetwork(pair, zero, zero, s, 0, network, &error), "missing dependency revision rejected");
  WorldSplineLandforms forms;
  WorldPhysicalTerrain physical;
  WorldRiverChannels channels;
  auto landscape = [](double x, double z)
  {
    return .008 * x + 1.1 * std::sin(x * .021) * std::cos(z * .027);
  };
  check(forms.build({}, {}, &error) && physical.build(forms, landscape, {}, {}, {}, &error), "integrated final terrain builds");
  SuitabilitySettings suitabilitySettings;
  suitabilitySettings.minX = 0;
  suitabilitySettings.maxX = 240;
  suitabilitySettings.minZ = -68;
  suitabilitySettings.maxZ = 68;
  WorldSuitability suitability;
  auto soil = [](double x, double z)
  {
    return .65 + .2 * std::sin(x * .027) * std::cos(z * .045);
  };
  check(
      suitability.build(
          physical,
          channels,
          soil,
          [](double, double)
          {
            return .5;
          },
          {{100, 0, 0}},
          suitabilitySettings,
          9,
          &error),
      "integrated terrain supplies settlement suitability");
  SettlementSettings reserve;
  reserve.townAreaMetres2 = 640;
  reserve.villageAreaMetres2 = 256;
  reserve.minimumSettlementGapMetres = 36;
  reserve.minimumExpansionRatio = .2;
  SettlementProduct settlements;
  check(reserveSettlements(suitability, reserve, settlements, &error) && settlements.reservations.size() == 3, "integrated scene reserves town and two villages");
  for (const auto &r : settlements.reservations)
    std::cout << "anchor " << r.id << " " << r.center.x << " " << r.center.z << "\n";
  RoadNetworkProduct integrated;
  auto integratedSettings = crown;
  integratedSettings.minimumBendRadiusMetres = 8;
  bool integratedOK = buildSettlementRoads(suitability, settlements, physical, channels, integratedSettings, integrated, &error);
  std::cout << "integrated: " << error << '\n';
  check(integratedOK && integrated.roads.size() == 2, "Step 08 reservations feed actual graded regional road meshes");
  if (integratedOK)
    for (size_t i = 0; i < integrated.roads.size(); ++i)
    {
      std::string name = "settlement-link-" + std::to_string(i + 1);
      dump(name.c_str(), integrated.roads[i].design, landscape);
    }
  auto stale = settlements;
  stale.suitabilityFingerprint++;
  check(!buildSettlementRoads(suitability, stale, physical, channels, integratedSettings, network, &error), "stale settlement snapshot cannot generate roads");

  auto exhausted = s;
  exhausted.spacingMetres = 1;
  exhausted.lateralSpacingMetres = .5;
  exhausted.corridorHalfWidthMetres = 1000;
  check(!routeRoad({0, 0, 0}, {4000, 0, 0}, zero, zero, exhausted, preserved, &error), "oversized heading search rejects before allocating states");
  auto reversedPath = path;
  reversedPath[10].x = reversedPath[9].x - 1;
  check(!designRoad(reversedPath, {}, zero, zero, s, preserved, &error), "backtracking road path rejected");
  auto sharp = path;
  sharp[10].z = 20;
  check(!designRoad(sharp, {}, zero, zero, s, preserved, &error), "sharp authored bend rejected before mesh publication");
  auto invalidCore = settlements;
  invalidCore.reservations[0].core = {UINT32_MAX};
  check(!buildSettlementRoads(suitability, invalidCore, physical, channels, integratedSettings, network, &error), "invalid settlement core cannot index road field");
  if (integratedOK)
  {
    bool supported = true;
    for (auto &r : integrated.roads)
      for (auto &t : r.design.triangles)
        for (auto &p : t.points)
          supported &= suitability.cellAt(p.x, p.z) != SIZE_MAX;
    check(supported, "map-edge reservation gets full-width supported connection point");
  }
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << json({{"assertions", assertions}, {"scenes", scenes}, {"failures", failures}}).dump(2);
  }
  return failures ? 1 : 0;
}
