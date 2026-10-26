#include "json.hpp"
#include "procedural/world/WorldRiverChannels.hpp"
#include "procedural/world/WorldRoadEarthworks.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
namespace
{
double side(Position a, Position b, Position p)
{
  return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
}
bool heightAt(const SurfaceArrangementMesh &m, double x, double z, uint32_t layer, double &y, SurfaceRole *role = nullptr)
{
  Position q{x, 0, z};
  for (auto &t : m.triangles)
  {
    if (t.layer != layer)
      continue;
    auto a = m.vertices[t.vertices[0]], b = m.vertices[t.vertices[1]], c = m.vertices[t.vertices[2]];
    double d = side(a, b, c), u = side(b, c, q) / d, v = side(c, a, q) / d, w = 1 - u - v;
    if (std::min({u, v, w}) >= -1e-8)
    {
      y = u * a.y + v * b.y + w * c.y;
      if (role)
        *role = t.role;
      return true;
    }
  }
  return false;
}
} // namespace
int main(int argc, char **argv)
{
  int failures = 0;
  json checks = json::array(), scenes = json::array();
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    checks.push_back({{"name", name}, {"pass", ok}});
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok)
      std::cout << error << '\n';
    failures += !ok;
  };
  auto dump = [&](const char *name, const RoadEarthworkProduct &p, const SurfaceArrangementMesh &m, const RoadField &ground)
  {
    json vertices = json::array(), triangles = json::array(), roles = json::array(), materials = json::array(), faces = json::array(), edges = json::array(), layers = json::array(), normals = json::array();
    for (auto v : m.vertices)
      vertices.push_back({v.x, v.y, v.z});
    for (auto &t : m.triangles)
    {
      triangles.push_back(t.vertices);
      roles.push_back(t.role == SurfaceRole::Water ? 1 : (t.role == SurfaceRole::Road || t.role == SurfaceRole::Shoulder || t.role == SurfaceRole::Bridge ? 2 : 0));
      materials.push_back(t.material);
      faces.push_back(t.face);
      layers.push_back(t.layer);
      normals.push_back({0, 1, 0});
    }
    for (auto &t : p.structures)
    {
      std::array<uint32_t, 3> ids;
      for (int i = 0; i < 3; ++i)
      {
        ids[i] = vertices.size();
        auto v = t.points[i];
        vertices.push_back({v.x, v.y, v.z});
      }
      triangles.push_back(ids);
      roles.push_back(2);
      materials.push_back(7);
      faces.push_back(0);
      layers.push_back(2);
      auto a = t.points[0], b = t.points[1], c = t.points[2];
      double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x),
             len = std::sqrt(nx * nx + ny * ny + nz * nz);
      normals.push_back({nx / len, ny / len, nz / len});
    }
    for (auto &e : p.map.edges)
      edges.push_back(
          {{"a", e.a},
           {"b", e.b},
           {"id", e.id},
           {"left", e.leftFace},
           {"right", e.rightFace},
           {"materialBoundary", e.materialBoundary},
           {"surfaceBreak", e.surfaceBreak},
           {"layer", e.layer},
           {"sources", e.sourceContours}});
    json section = json::array();
    for (int i = 0; i <= 320; ++i)
    {
      double z = i * .25, y = 0;
      SurfaceRole role;
      bool ok = heightAt(m, 60, z, 0, y, &role);
      section.push_back({{"z", z}, {"ground", ground(60, z)}, {"surface", ok ? json(y) : json()}});
    }
    json longitudinal = json::array();
    for (int i = 0; i <= 480; ++i)
    {
      double x = i * .25, g = 0, w = 0, deck = 0;
      bool hasGround = heightAt(m, x, 40, 0, g), hasWater = heightAt(m, x, 40, 1, w), hasDeck = heightAt(m, x, 40, 2, deck);
      longitudinal.push_back({{"x", x}, {"natural", ground(x, 40)}, {"ground", hasGround ? json(g) : json()}, {"water", hasWater ? json(w) : json()}, {"deck", hasDeck ? json(deck) : json()}});
    }
    scenes.push_back(
        {{"name", name},
         {"phase", 10},
         {"mesh", {{"vertices", vertices}, {"triangles", triangles}, {"roles", roles}, {"materials", materials}, {"faces", faces}, {"layers", layers}, {"expectedNormals", normals}}},
         {"mapEdges", edges},
         {"section", section},
         {"longitudinal", longitudinal},
         {"groundArea", m.groundArea},
         {"faceCount", p.map.faces.size()},
         {"maxReach", p.maximumReachMetres},
         {"bridges", p.bridges}});
  };
  RoadSettings roadSettings;
  roadSettings.crownSlope = 0;
  roadSettings.maximumEarthworkMetres = 6;
  roadSettings.corridorHalfWidthMetres = 60;
  auto dry = [](double, double)
  {
    return RoadWaterSample{};
  };
  auto flat = [](double, double)
  {
    return 0.;
  };
  auto makeRoad = [&](Position a, Position b, const RoadField &terrain, RoadProduct &road)
  {
    std::vector<Position> path;
    std::vector<RoadHeightLock> locks;
    int n = int(std::ceil(std::hypot(b.x - a.x, b.z - a.z) / 4));
    for (int i = 0; i <= n; ++i)
    {
      double t = double(i) / n;
      path.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
      locks.push_back({uint32_t(i), path.back().y});
    }
    return designRoad(path, locks, terrain, flat, roadSettings, road, &error);
  };
  auto hill = [](double x, double z)
  {
    return .05 * x + .30 * (z - 40);
  };
  RoadProduct road;
  check(makeRoad({20, 1, 40}, {100, 5, 40}, hill, road), "graded hillside road design builds");
  RoadNetworkProduct network;
  network.dependencyRevision = 10;
  network.roads = {{{1, 2}, road}};
  RoadEarthworkSettings settings;
  RoadEarthworkProduct hillside;
  SurfaceArrangementMesh hillMesh;
  bool ok = generateRoadEarthworks(network, hill, dry, {}, {}, settings, hillside, &error);
  check(ok, "hillside exports cut fill and road contours before CDT");
  check(ok && meshSurfaceArrangement(hillside.map, hill, {}, hillMesh, &error), "hillside labeled map faces compile through CDT");
  bool cut = false, fill = false, shoulder = false, ground = false;
  for (auto &f : hillside.map.faces)
  {
    cut |= f.role == SurfaceRole::Cut;
    fill |= f.role == SurfaceRole::Fill;
    shoulder |= f.role == SurfaceRole::Shoulder;
    ground |= f.role == SurfaceRole::Ground;
  }
  check(cut && fill && shoulder && ground, "ground cut fill and shoulders retain distinct map roles");
  double y = 0;
  check(heightAt(hillMesh, 60, 42, 0, y) && std::abs(y - 3) < 1e-7 && hill(60, 42) > y + .5, "old terrain above uphill road tread is removed");
  check(heightAt(hillMesh, 60, 38, 0, y) && std::abs(y - 3) < 1e-7 && hill(60, 38) < y - .5, "downhill road tread has explicit fill support");
  check(heightAt(hillMesh, 60, 60, 0, y) && std::abs(y - hill(60, 60)) < 1e-7, "terrain outside earthwork footprint is unchanged");
  check(std::abs(hillMesh.groundArea - 9600) < 1e-5, "ground surface covers domain exactly once by area");
  std::set<std::pair<uint32_t, uint32_t>> meshEdges;
  for (auto &t : hillMesh.triangles)
    for (int i = 0; i < 3; ++i)
      meshEdges.insert(std::minmax(t.vertices[i], t.vertices[(i + 1) % 3]));
  bool preserved = !hillside.map.edges.empty();
  for (auto &e : hillside.map.edges)
    preserved &= meshEdges.count(std::minmax(e.a, e.b)) != 0;
  check(preserved, "every canonical contour edge survives final CDT");
  bool faceLabels = true;
  for (auto &t : hillMesh.triangles)
  {
    auto &f = hillside.map.faces[t.face - 1];
    faceLabels &= t.material == f.material && t.role == f.role && t.layer == f.layer;
  }
  check(faceLabels && !hillMesh.triangles.empty(), "triangle materials come from owning arrangement faces");
  SurfaceRefinement refined;
  refined.groundSpacingMetres = 6;
  refined.modifiedSpacingMetres = 1.5;
  SurfaceArrangementMesh dense;
  check(meshSurfaceArrangement(hillside.map, hill, refined, dense, &error) && dense.triangles.size() > hillMesh.triangles.size(), "local refinement adds detail inside fixed map faces");
  bool stable = dense.vertices.size() >= hillside.map.vertices.size();
  for (size_t i = 0; i < hillside.map.vertices.size() && stable; ++i)
    stable &= dense.vertices[i].x == hillMesh.vertices[i].x && dense.vertices[i].y == hillMesh.vertices[i].y && dense.vertices[i].z == hillMesh.vertices[i].z;
  check(stable, "refinement preserves exact shared contour positions");
  check(heightAt(dense, 60, 42, 0, y) && std::abs(y - 3) < 1e-7, "refinement cannot tilt the road plane");
  if (!hillMesh.triangles.empty())
    dump("hillside-carve", hillside, hillMesh, hill);
  auto small = settings;
  small.maximumReachMetres = .5;
  auto saved = hillside;
  check(!generateRoadEarthworks(network, hill, dry, {}, {}, small, saved, &error) && saved.map.revision == hillside.map.revision, "insufficient daylight reach fails without modifying prior product");
  RoadProduct raised;
  check(makeRoad({20, 2, 40}, {100, 2, 40}, flat, raised), "raised embankment road design builds");
  network.roads = {{{1, 2}, raised}};
  RoadEarthworkProduct embankment;
  SurfaceArrangementMesh fillMesh;
  ok = generateRoadEarthworks(network, flat, dry, {}, {}, settings, embankment, &error) && meshSurfaceArrangement(embankment.map, flat, {}, fillMesh, &error);
  check(ok, "embankment meets ground along exported outer contour");
  check(heightAt(fillMesh, 60, 40, 0, y) && std::abs(y - 2) < 1e-7 && heightAt(fillMesh, 60, 55, 0, y) && std::abs(y) < 1e-7, "embankment has elevated road and unchanged external ground");
  if (ok)
    dump("embankment", embankment, fillMesh, flat);
  RoadProduct trunk, branch;
  check(makeRoad({20, 0, 40}, {100, 0, 40}, flat, trunk) && makeRoad({60, 0, 40}, {60, 0, 68}, flat, branch), "compatible T-junction approaches build");
  network.roads = {{{1, 2}, trunk}, {{1, 3}, branch}};
  RoadEarthworkProduct junction;
  SurfaceArrangementMesh junctionMesh;
  ok = generateRoadEarthworks(network, flat, dry, {}, {}, settings, junction, &error) && meshSurfaceArrangement(junction.map, flat, {}, junctionMesh, &error);
  check(ok, "T junction unifies road ownership in the map before meshing");
  check(std::abs(junctionMesh.groundArea - 9600) < 1e-5 && heightAt(junctionMesh, 61.1, 41.3, 0, y) && std::abs(y) < 1e-8, "intersection has complete nonstacked ground coverage");
  if (ok)
    dump("t-junction", junction, junctionMesh, flat);
  auto shifted = branch;
  for (auto &section : shifted.sections)
  {
    section.center.y += .5;
    for (auto &p : section.points)
      p.y += .5;
  }
  for (auto &t : shifted.triangles)
    for (auto &p : t.points)
      p.y += .5;
  network.roads[1].design = shifted;
  check(!generateRoadEarthworks(network, flat, dry, {}, {}, settings, saved, &error), "incompatible junction elevations require redesign instead of stacked ribbons");
  auto channel = [](double x, double)
  {
    if (x <= 50 || x >= 70)
      return 0.;
    if (x < 54)
      return -(x - 50);
    if (x > 66)
      return -(70 - x);
    return -4.;
  };
  auto water = [](double x, double)
  {
    return RoadWaterSample{x > 50 && x < 70, -2};
  };
  RoadProduct crossing;
  check(makeRoad({20, 1, 40}, {100, 1, 40}, channel, crossing), "locked bridge approach design builds");
  network.roads = {{{1, 2}, crossing}};
  std::vector<SurfaceContour> river;
  auto rect = [&](uint64_t id, double a, double b, uint32_t layer, SurfaceRole role, uint32_t material, bool terrain)
  {
    SurfaceContour c;
    c.id = id;
    c.layer = layer;
    c.role = role;
    c.material = material;
    c.terrainSurface = terrain;
    c.plane.constant = -2;
    c.loop = {{a, 0, 0}, {b, 0, 0}, {b, 0, 80}, {a, 0, 80}};
    river.push_back(c);
  };
  rect(1, 50, 54, 0, SurfaceRole::RiverBank, 5, true);
  rect(2, 54, 66, 0, SurfaceRole::RiverBed, 6, true);
  rect(3, 66, 70, 0, SurfaceRole::RiverBank, 5, true);
  rect(4, 52, 68, 1, SurfaceRole::Water, 9, false);
  check(!generateRoadEarthworks(network, channel, water, river, {}, settings, saved, &error), "undeclared ground-road river crossing is rejected");
  BridgeSpan span;
  span.firstStation = 7;
  span.lastStation = 13;
  RoadEarthworkProduct bridge;
  SurfaceArrangementMesh bridgeMesh;
  ok = generateRoadEarthworks(network, channel, water, river, {span}, settings, bridge, &error) && meshSurfaceArrangement(bridge.map, channel, {}, bridgeMesh, &error);
  check(ok, "bridge deck and channel occupy separate contour layers");
  check(heightAt(bridgeMesh, 60, 40, 0, y) && std::abs(y + 4) < 1e-7, "bridge leaves river bed unchanged below it");
  check(heightAt(bridgeMesh, 60, 40, 1, y) && std::abs(y + 2) < 1e-7, "water sheet is continuous beneath bridge");
  check(heightAt(bridgeMesh, 60, 40, 2, y) && std::abs(y - 1) < 1e-7 && bridge.bridges == 1 && !bridge.structures.empty(), "bridge has deck thickness and dry abutment geometry");
  if (ok)
    dump("bridge-channel", bridge, bridgeMesh, channel);
  auto submerged = span;
  submerged.minimumWaterClearanceMetres = 4;
  check(!generateRoadEarthworks(network, channel, water, river, {submerged}, settings, saved, &error), "insufficient bridge soffit clearance rejected");
  auto wetEnd = span;
  wetEnd.firstStation = 9;
  check(!generateRoadEarthworks(network, channel, water, river, {wetEnd}, settings, saved, &error), "wet bridge abutment rejected");
  auto longSpan = span;
  longSpan.maximumSpanMetres = 10;
  check(!generateRoadEarthworks(network, channel, water, river, {longSpan}, settings, saved, &error), "bridge structural span budget enforced");
  auto invalid = settings;
  invalid.fillSlope = 0;
  check(!generateRoadEarthworks(network, channel, water, river, {span}, invalid, saved, &error), "invalid earthwork slope rejected");
  SurfaceRefinement exhausted;
  exhausted.maximumInteriorPoints = 0;
  check(!meshSurfaceArrangement(hillside.map, hill, exhausted, dense, &error), "local refinement budget fails atomically");
  SurfaceContour bad;
  {
    SurfaceContour a, b;
    a.id=901; b.id=902;
    a.role=b.role=SurfaceRole::Road;
    a.terrainSurface=b.terrainSurface=true;
    a.material=b.material=1;
    a.loop={{10,0,10},{30,0,10},{30,0,14},{10,0,14}};
    b.loop={{18,0,5},{22,0,5},{22,0,25},{18,0,25}};
    a.plane.constant=1; b.plane.constant=20;
    SurfaceArrangement junction;
    const auto slope=[](double x,double z){return .02*x+.03*z;};
    check(arrangeSurfaceContours({a,b},settings.map,slope,1,junction,&error),
          "terrain-following road junction ignores unused per-road plane heights");
    a.terrainSurface=b.terrainSurface=false;
    check(!arrangeSurfaceContours({a,b},settings.map,slope,1,junction,&error),
          "incompatible locked road planes still reject the junction");
  }
  bad.id = 1;
  bad.loop = {{10, 0, 10}, {30, 0, 30}, {10, 0, 30}, {30, 0, 10}};
  SurfaceArrangement untouched = hillside.map;
  check(!arrangeSurfaceContours({bad}, settings.map, flat, 12, untouched, &error) && untouched.revision == 10, "invalid crossing contour cannot replace the map");

  std::vector<Position> curve;
  std::vector<RoadHeightLock> curveLocks;
  for (int i = 0; i <= 20; ++i)
  {
    double x = 20 + i * 4, z = 40 + 4 * std::sin(i * 3.141592653589793 / 20);
    curve.push_back({x, hill(x, z), z});
    curveLocks.push_back({uint32_t(i), curve.back().y});
  }
  RoadProduct curved;
  check(designRoad(curve, curveLocks, hill, flat, roadSettings, curved, &error), "curved hillside design respects road grade");
  network.roads = {{{1, 2}, curved}};
  RoadEarthworkProduct curvedEarth;
  SurfaceArrangementMesh curvedMesh;
  bool curveOK = generateRoadEarthworks(network, hill, dry, {}, {}, settings, curvedEarth, &error) && meshSurfaceArrangement(curvedEarth.map, hill, {}, curvedMesh, &error);
  check(curveOK, "curved hillside contours preserve continuous cut fill seams");
  if (curveOK)
    dump("curved-hillside", curvedEarth, curvedMesh, hill);
  WorldRiverChannels hydraulic;
  check(hydraulic.build({{1, {60, -2, 0}, 0}, {2, {60, -1.9, 80}, 0}}, {{2, 1, 1}}, {{1, 2, 2000000, 3.6, 1}}, {}, &error), "conservative hydraulic river supplies bridge terrain and water");
  auto actualGround = [&](double x, double z)
  {
    ChannelSample value;
    if (!hydraulic.sample(x, z, flat, value))
      return std::numeric_limits<double>::quiet_NaN();
    return value.terrainHeight;
  };
  auto actualWater = [&](double x, double z)
  {
    ChannelSample value;
    hydraulic.sample(x, z, flat, value);
    return RoadWaterSample{std::abs(x - 60) < hydraulic.reaches()[0].normal.waterWidth * .5 + 8, value.waterHeight};
  };
  RoadProduct actualRoad;
  check(makeRoad({20, 2, 40}, {100, 2, 40}, actualGround, actualRoad), "bridge approach locks fit final hydraulic terrain");
  network.roads = {{{1, 2}, actualRoad}};
  river.clear();
  double bottom = hydraulic.reaches()[0].normal.bottomWidth * .5, bank = hydraulic.reaches()[0].normal.waterWidth * .5;
  rect(1, 60 - bank - 8, 60 - bank, 0, SurfaceRole::RiverBank, 5, true);
  rect(2, 60 - bank, 60 - bottom, 0, SurfaceRole::RiverBank, 5, true);
  rect(3, 60 - bottom, 60 + bottom, 0, SurfaceRole::RiverBed, 6, true);
  rect(4, 60 + bottom, 60 + bank, 0, SurfaceRole::RiverBank, 5, true);
  rect(5, 60 + bank, 60 + bank + 8, 0, SurfaceRole::RiverBank, 5, true);
  rect(6, 60 - bank, 60 + bank, 1, SurfaceRole::Water, 9, false);
  river.back().plane.z = .1 / 80;
  auto actualSpan = span;
  actualSpan.firstStation = 6;
  actualSpan.lastStation = 14;
  RoadEarthworkProduct hydraulicBridge;
  SurfaceArrangementMesh hydraulicMesh;
  bool hydraulicOK =
      generateRoadEarthworks(network, actualGround, actualWater, river, {actualSpan}, settings, hydraulicBridge, &error) && meshSurfaceArrangement(hydraulicBridge.map, actualGround, {}, hydraulicMesh, &error);
  check(hydraulicOK, "bridge consumes actual river bed bank and water contours");
  check(hydraulicOK && heightAt(hydraulicMesh, 60, 40, 0, y) && std::abs(y - actualGround(60, 40)) < 1e-7, "actual hydraulic bed is preserved beneath bridge");
  check(
      hydraulicOK && heightAt(hydraulicMesh, 60, 40, 1, y) && std::abs(y - actualWater(60, 40).waterHeight) < 1e-7 && std::abs(hydraulic.reaches()[0].discharge - 2) < 1e-9,
      "water elevation and conserved two cumecs discharge remain unchanged");
  if (hydraulicOK)
    dump("hydraulic-bridge", hydraulicBridge, hydraulicMesh, actualGround);

  network.roads = {{{1, 2}, road}};
  auto ditchSettings = settings;
  ditchSettings.ditchDepthMetres = .25;
  RoadEarthworkProduct ditch;
  SurfaceArrangementMesh ditchMesh;
  bool ditchOK = generateRoadEarthworks(network, hill, dry, {}, {}, ditchSettings, ditch, &error) && meshSurfaceArrangement(ditch.map, hill, {}, ditchMesh, &error);
  check(ditchOK, "optional cut-side ditch exports its own constrained material faces");
  bool hasDitch = false;
  for (auto &face : ditch.map.faces)
    hasDitch |= face.role == SurfaceRole::Ditch;
  check(hasDitch, "drainage trough is geometry rather than a painted line");
  if (ditchOK)
    dump("hillside-ditch", ditch, ditchMesh, hill);
  RoadEarthworkProduct repeated;
  check(
      generateRoadEarthworks(network, hill, dry, {}, {}, settings, repeated, &error) && repeated.map.vertices.size() == hillside.map.vertices.size() && repeated.map.faces.size() == hillside.map.faces.size(),
      "same input repeats contour topology deterministically");
  std::set<uint64_t> trace;
  for (auto &edge : hillside.map.edges)
    for (auto source : edge.sourceContours)
      trace.insert(source);
  check(trace.size() == hillside.contours.size(), "map edges retain traceable source contour IDs");

  auto malformed = network;
  malformed.roads[0].design.triangles[0].points[0].x = std::numeric_limits<double>::quiet_NaN();
  check(!generateRoadEarthworks(malformed, hill, dry, {}, {}, settings, saved, &error), "nonfinite road geometry rejected before corridor sampling");
  auto broken = hillside.map;
  broken.edges[0].a = UINT32_MAX;
  check(!meshSurfaceArrangement(broken, hill, {}, dense, &error), "malformed map edge rejected without unsafe indexing");
  broken = hillside.map;
  broken.edges.erase(broken.edges.begin());
  check(!meshSurfaceArrangement(broken, hill, {}, dense, &error), "missing map constraint cannot silently disappear in CDT");
  bool crease = false;
  for (auto &e : hillside.map.edges)
    if (e.leftFace && e.rightFace)
    {
      auto &a = hillside.map.faces[e.leftFace - 1], &b = hillside.map.faces[e.rightFace - 1];
      if (a.role != b.role && (a.role == SurfaceRole::Cut || b.role == SurfaceRole::Cut))
        crease |= e.surfaceBreak;
    }
  check(crease, "road-to-cut creases are explicit shared surface breaklines");

  RoadProduct coplanar;
  makeRoad({20, 0, 40}, {100, 0, 40}, channel, coplanar);
  network.roads = {{{1, 2}, coplanar}};
  check(!generateRoadEarthworks(network, channel, water, {}, {span}, settings, saved, &error), "coplanar or buried bridge deck is rejected instead of flickering with terrain");

  double endArea = 0;
  for (auto &t : bridge.structures)
  {
    auto a = t.points[0], b = t.points[1], c = t.points[2];
    if (std::abs(a.x - b.x) < 1e-8 && std::abs(a.x - c.x) < 1e-8 && (std::abs(a.x - 48) < 1e-8 || std::abs(a.x - 72) < 1e-8))
      endArea += std::abs((b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y)) * .5;
  }
  check(std::abs(endArea - 24) < 1e-7, "bridge abutments own end faces without overlapping slab caps");
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << json({{"assertions", checks}, {"failures", failures}, {"scenes", scenes}}).dump(2);
  }
  return failures ? 1 : 0;
}
