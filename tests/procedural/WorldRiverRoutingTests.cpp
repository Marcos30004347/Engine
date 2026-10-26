#include "json.hpp"
#include "procedural/world/WorldChannelMesh.hpp"
#include "procedural/world/WorldPhysicalTerrain.hpp"
#include "procedural/world/WorldRiverChannels.hpp"
#include "procedural/world/WorldRiverRouting.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
json encode(const ChannelMeshProduct &m)
{
  json vertices = json::array();
  for (auto p : m.vertices)
    vertices.push_back({p.x, p.y, p.z});
  return {{"vertices", vertices}, {"triangles", m.triangles}, {"roles", m.roles}};
}
json paths(const RiverGrowthProduct &g)
{
  json result = json::array();
  for (auto &path : g.reachPaths)
  {
    json row = json::array();
    for (auto p : path)
      row.push_back({p.x, p.y, p.z});
    result.push_back(row);
  }
  return result;
}
int main(int argc, char **argv)
{
  int failures = 0;
  json checks = json::array();
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    checks.push_back({{"name", name}, {"pass", ok}});
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok)
      std::cerr << error << '\n';
    failures += !ok;
  };
  RiverGrowthProduct graph;
  graph.nodes = {{{0, .8, -150}, UINT32_MAX, 1, 2}, {{0, 2, 0}, 0, 2, 2}, {{-60, 3.4, 120}, 1, 1, 1}, {{65, 3.4, 120}, 1, 1, 1}};
  auto ground = [&](double x, double z)
  {
    double best = 1e30;
    for (size_t i = 1; i < graph.nodes.size(); ++i)
    {
      auto a = graph.nodes[i].position, b = graph.nodes[graph.nodes[i].receiver].position;
      double L = std::hypot(b.x - a.x, b.z - a.z), ux = (b.x - a.x) / L, uz = (b.z - a.z) / L;
      double t = std::clamp(((x - a.x) * ux + (z - a.z) * uz) / L, 0., 1.);
      double lateral = -(x - a.x) * uz + (z - a.z) * ux;
      double target = (i == 1 ? 17 : 11) * std::pow(std::sin(3.141592653589793 * t), 2) * std::sin((i == 1 ? 3.2 : 2.5) * 3.141592653589793 * t + .3 * i);
      double endpoint = std::hypot(x - a.x - ux * t * L, z - a.z - uz * t * L);
      double d = std::abs(lateral - target);
      if (t == 0 || t == 1)
        d = endpoint;
      best = std::min(best, a.y + (b.y - a.y) * t + .8 + 8 * (1 - std::exp(-.012 * d * d / 8)));
    }
    return best;
  };
  auto soft = [](double, double)
  {
    return 0.;
  };
  RiverRoutingSettings settings;
  settings.corridorHalfWidthMetres = 28;
  settings.meanderWeight = .003;
  settings.minimumBendRadiusMetres = 7;
  settings.smoothingPasses = 40;
  RiverGrowthProduct routed;
  std::vector<RiverRouteMetrics> metrics;
  check(refineRiverPaths(graph, ground, soft, settings, routed, metrics, &error), "Valley-aware heading routes and smoothed confluence build");
  if (routed.reachPaths.empty())
    return 1;
  bool identity = routed.nodes.size() == graph.nodes.size(), bounds = true, downhill = true;
  for (size_t i = 0; i < graph.nodes.size(); ++i)
  {
    identity &= routed.nodes[i].receiver == graph.nodes[i].receiver;
    if (i)
    {
      auto &p = routed.reachPaths[i];
      auto a = graph.nodes[i].position, b = graph.nodes[graph.nodes[i].receiver].position;
      identity &= p.front().x == a.x && p.front().y == a.y && p.front().z == a.z && p.back().x == b.x && p.back().y == b.y && p.back().z == b.z;
      for (size_t j = 1; j < p.size(); ++j)
        downhill &= p[j].y <= p[j - 1].y;
    }
  }
  for (auto m : metrics)
    bounds &= m.maximumOffset <= settings.corridorHalfWidthMetres && m.maximumCurvature <= 1 / settings.minimumBendRadiusMetres + 1e-9 && m.maximumIncision <= settings.maximumIncisionMetres;
  check(identity, "Catchment receivers junction controls and outlets retain identity");
  check(bounds, "Smoothed paths retain corridor incision and curvature bounds");
  check(downhill, "True arc-length water stations remain downhill");
  bool noLoops = true;
  double curvedCut = 0, straightCut = 0;
  for (size_t i = 1; i < routed.nodes.size(); ++i)
  {
    const auto &p = routed.reachPaths[i];
    auto a = p.front(), b = p.back();
    for (size_t j = 1; j < p.size(); ++j)
    {
      noLoops &= (p[j].x - p[j - 1].x) * (b.x - a.x) + (p[j].z - p[j - 1].z) * (b.z - a.z) > 0;
      double t = double(j) / (p.size() - 1);
      curvedCut += std::max(0., ground(p[j].x, p[j].z) - p[j].y);
      straightCut += std::max(0., ground(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t) - (a.y + (b.y - a.y) * t));
    }
  }
  check(noLoops, "Forward valley parameter prevents self loops and backtracking");
  check(curvedCut < straightCut * .9, "Routed curves reduce incision relative to straight valley shortcuts");
  check(metrics[0].sinuosity > 1.025 && metrics[0].sinuosity < 1.4, "Lowland route has nonzero bounded sinuosity");
  RiverGrowthProduct repeat;
  std::vector<RiverRouteMetrics> repeated;
  check(refineRiverPaths(graph, ground, soft, settings, repeat, repeated, &error) && paths(repeat) == paths(routed), "Repeated seed and inputs produce identical centerlines");
  auto blocked = [](double, double)
  {
    return std::numeric_limits<double>::infinity();
  };
  auto saved = paths(repeat);
  check(!refineRiverPaths(graph, ground, blocked, settings, repeat, repeated, &error) && paths(repeat) == saved, "Blocked corridor fails atomically without straight fallback");
  auto uphill = graph;
  uphill.nodes[0].position.y = 10;
  check(!refineRiverPaths(uphill, ground, soft, settings, repeat, repeated), "Uphill outlet requires structural resolution");
  auto bad = settings;
  bad.longitudinalSpacingMetres = 0;
  check(!refineRiverPaths(graph, ground, soft, bad, repeat, repeated), "Invalid routing resolution rejected");
  bad = settings;
  bad.minimumBendRadiusMetres = 10000;
  check(!refineRiverPaths(graph, ground, soft, bad, repeat, repeated), "Smoothing cannot silently exceed curvature budget");
  RiverGrowthProduct simple;
  simple.nodes = {{{0, 0, 0}, UINT32_MAX, 1, 1}, {{0, 1, 200}, 0, 1, 1}};
  auto flat = [](double, double z)
  {
    return .005 * z + .8;
  };
  auto obstacle = [](double x, double z)
  {
    return 100 * std::exp(-x * x / 40 - (z - 100) * (z - 100) / 300);
  };
  RiverGrowthProduct detour;
  std::vector<RiverRouteMetrics> detourStats;
  auto detourSettings = settings;
  detourSettings.meanderAmplitudeMetres = 0;
  check(refineRiverPaths(simple, flat, obstacle, detourSettings, detour, detourStats, &error), "Resistant ground forces a feasible lateral detour");
  double offset = 0;
  if (!detour.reachPaths.empty())
    for (auto p : detour.reachPaths[1])
      if (std::abs(p.z - 100) < 3)
        offset = std::max(offset, std::abs(p.x));
  check(offset > 10, "Detour bypasses resistant core instead of decorative jitter");
  RiverGrowthProduct confined = simple;
  confined.nodes[1].position.y = 40;
  auto mountain = [](double x, double z)
  {
    return .2 * z + .8 + 8 * (1 - std::exp(-.3 * x * x / 8));
  };
  RiverGrowthProduct mountainPath;
  std::vector<RiverRouteMetrics> mountainStats;
  check(refineRiverPaths(confined, mountain, soft, settings, mountainPath, mountainStats, &error) && mountainStats[0].sinuosity < 1.005, "Steep confined stream remains nearly straight without forced meanders");
  auto crossing = graph;
  crossing.nodes = {{{50, 0, 0}, UINT32_MAX, 1, 1}, {{-50, 1, 100}, 0, 1, 1}, {{-50, 0, 0}, UINT32_MAX, 1, 1}, {{50, 1, 100}, 2, 1, 1}};
  auto crossGround = [](double, double z)
  {
    return z * .01 + .8;
  };
  auto straight = settings;
  straight.meanderAmplitudeMetres = 0;
  check(!refineRiverPaths(crossing, crossGround, soft, straight, repeat, repeated, &error), "Unrelated crossing channels rejected before publication");
  auto malformed = routed;
  malformed.reachPaths[1][5].x += .1;
  WorldRiverChannels malformedChannels;
  RiverPath decoded;
  check(!makeRiverPath(malformed.reachPaths[1], graph.nodes[1].position, graph.nodes[0].position, decoded), "Changed path with stale arc-length water heights is rejected");
  malformed = routed;
  malformed.reachPaths[0] = {{0, 0, 0}};
  std::vector<RiverPath> invalidPaths;
  check(!resolveRiverPaths(malformed, invalidPaths), "Outlet cannot carry an invented outgoing centerline");
  auto partialBarrier = [](double, double z)
  {
    return z > 95 && z < 105 ? std::numeric_limits<double>::infinity() : 0.;
  };
  check(!refineRiverPaths(simple, flat, partialBarrier, settings, repeat, repeated), "A barrier across the valley cannot be smoothed through");
  auto negative = [](double, double)
  {
    return -1.;
  };
  check(!refineRiverPaths(simple, flat, negative, settings, repeat, repeated), "Negative resistance is rejected as invalid ground");
  bad = settings;
  bad.corridorHalfWidthMetres = 100000;
  check(!refineRiverPaths(simple, flat, soft, bad, repeat, repeated), "Oversized routing allocation fails before work begins");
  WatershedProduct cells;
  cells.landAreaMetres2 = 5500000;
  cells.cells = {{0, 0, {}}, {1, 500000, {}}, {2, 2000000, {}}, {3, 3000000, {}}};
  ChannelSettings channelSettings;
  channelSettings.minimumBottomWidthMetres = 4;
  channelSettings.bankTransitionMetres = 4;
  channelSettings.confluenceBlendMetres = 7;
  WorldRiverChannels channels, oldChannels;
  check(channels.buildFromWatersheds(routed, cells, 3.6, 1, channelSettings, false, &error), "Curved channels rebuild from exclusive watershed areas");
  if (channels.reaches().empty())
    return 1;
  check(std::abs(channels.ledger()[0].outgoing - 5.5) < 1e-10, "Curved routing conserves the 5.5 cubic metre outlet discharge");
  oldChannels.buildFromWatersheds(graph, cells, 3.6, 1, channelSettings);
  check(
      channels.reaches()[0].length > oldChannels.reaches()[0].length && channels.reaches()[0].energySlope < oldChannels.reaches()[0].energySlope &&
          channels.reaches()[0].normal.depth > oldChannels.reaches()[0].normal.depth,
      "Longer path lowers slope and recomputes hydraulic depth");
  WorldSplineLandforms forms;
  forms.build({});
  WorldPhysicalTerrain oldPhysical, physical;
  std::vector<RiverReservation> reserves;
  for (uint32_t i = 1; i < 4; ++i)
    reserves.push_back({i, 8, 12, 2, 30});
  check(oldPhysical.build(forms, ground, {}, graph, reserves, &error) && !channels.validateReservations(oldPhysical), "Curved channels reject stale straight terrain reservations");
  check(physical.build(forms, ground, {}, routed, reserves, &error) && channels.validateReservations(physical, &error), "Physical cuts and final channel profiles share rebuilt curved reservations");
  auto finalBase = [&](double x, double z)
  {
    PhysicalSample sample;
    if (!physical.sample(x, z, sample))
      return std::numeric_limits<double>::quiet_NaN();
    return sample.height;
  };
  bool follows = true;
  double maxError = 0;
  for (size_t i = 0; i < channels.reaches().size(); ++i)
  {
    const auto &r = channels.reaches()[i];
    double previous = 1e30;
    for (int k = 0; k <= 300; ++k)
    {
      double station = r.length * k / 300;
      auto p = r.path.at(station);
      ChannelSample sample;
      ChannelSection section;
      double water;
      channels.section(i, station, section, water);
      bool ok = channels.sample(p.x, p.z, finalBase, sample);
      follows &= ok && sample.wet && sample.terrainHeight < sample.waterHeight && sample.terrainHeight <= previous + 1e-7;
      maxError = std::max(maxError, std::abs(sample.waterHeight - water));
      previous = sample.terrainHeight;
    }
  }
  check(follows, "Final curved bed stays wet open and downhill through confluence");
  ChannelSample junctionQuery;
  channels.sample(2, 3, finalBase, junctionQuery);
  const auto &trunk = channels.reaches()[0];
  double expectedWater = trunk.a.y + 3 * (trunk.a.y - trunk.b.y) / trunk.length;
  check(junctionQuery.wet && std::abs(junctionQuery.waterHeight - expectedWater) < 1e-8, "Confluence evaluates one water plane at the query across branch projections");
  ChannelMeshSettings meshSettings;
  meshSettings.minX = -100;
  meshSettings.maxX = 100;
  meshSettings.minZ = -160;
  meshSettings.maxZ = 130;
  meshSettings.spacingMetres = 2;
  ChannelMeshProduct mesh;
  check(buildChannelMesh(channels, finalBase, meshSettings, mesh, &error), "Curved water and bank patches compile through CDT");
  bool manifold = true, seams = true;
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  std::set<uint32_t> vertices;
  int triangles = 0;
  std::map<std::pair<double, double>, double> terrainVertices;
  for (size_t i = 0; i < mesh.triangles.size(); ++i)
  {
    auto t = mesh.triangles[i];
    if (mesh.roles[i] == 1)
    {
      ++triangles;
      for (int e = 0; e < 3; ++e)
      {
        auto a = t[e], b = t[(e + 1) % 3];
        vertices.insert(a);
        if (a > b)
          std::swap(a, b);
        ++edges[{a, b}];
      }
    }
    else
      for (auto v : t)
      {
        auto p = mesh.vertices[v];
        terrainVertices[{p.x, p.z}] = p.y;
      }
  }
  for (auto [edge, count] : edges)
  {
    manifold &= count <= 2;
    if (count == 1)
      for (auto v : {edge.first, edge.second})
      {
        auto p = mesh.vertices[v];
        auto it = terrainVertices.find({p.x, p.z});
        seams &= it != terrainVertices.end() && std::abs(it->second - p.y) < 1e-7;
      }
  }
  check(manifold && int(vertices.size()) - int(edges.size()) + triangles == 1, "Curved confluence water remains one manifold disk");
  check(seams, "Curved shoreline water and terrain vertices meet without cracks");
  check(std::abs(mesh.terrainAreaMetres2 - 58000) < 1e-6, "Curved channel meshing conserves full terrain domain area");
  WatershedProduct mountainCells;
  mountainCells.landAreaMetres2 = 2000000;
  mountainCells.cells = {{0, 0, {}}, {1, 2000000, {}}};
  WorldRiverChannels mountainChannels;
  ChannelMeshProduct mountainMesh;
  auto mountainMeshSettings = meshSettings;
  mountainMeshSettings.minX = -40;
  mountainMeshSettings.maxX = 40;
  mountainMeshSettings.minZ = -10;
  mountainMeshSettings.maxZ = 210;
  check(
      mountainChannels.buildFromWatersheds(mountainPath, mountainCells, 3.6, 1, channelSettings, false, &error) && buildChannelMesh(mountainChannels, mountain, mountainMeshSettings, mountainMesh, &error),
      "Confined stream produces native terrain and water geometry");
  const auto &merged = channels.reaches()[0];
  check(
      merged.normal.waterWidth > channels.reaches()[1].normal.waterWidth && merged.normal.waterWidth > channels.reaches()[2].normal.waterWidth && merged.normal.area > channels.reaches()[1].normal.area &&
          merged.normal.area > channels.reaches()[2].normal.area,
      "Merged lowland river is wider and has greater flow area than either tributary");
  bool flowArea = true;
  json hydraulic = json::array();
  for (const auto &reach : channels.reaches())
  {
    const auto &section = reach.normal;
    flowArea &= std::abs(section.area * section.velocity - reach.discharge) < 1e-10;
    hydraulic.push_back({{"upstream", reach.from - 1}, {"discharge", reach.discharge}, {"width", section.waterWidth}, {"depth", section.depth}, {"area", section.area}, {"velocity", section.velocity}});
  }
  check(flowArea, "Every curved reach preserves discharge as wetted area times velocity");
  json rows = json::array();
  for (auto m : metrics)
    rows.push_back(
        {{"upstream", m.upstreamNode},
         {"length", m.lengthMetres},
         {"sinuosity", m.sinuosity},
         {"curvature", m.maximumCurvature},
         {"incision", m.maximumIncision},
         {"offset", m.maximumOffset},
         {"states", m.statesVisited}});
  json heightRows = json::array();
  for (int z = -160; z <= 130; z += 2)
  {
    json row = json::array();
    for (int x = -100; x <= 100; x += 2)
      row.push_back(ground(x, z));
    heightRows.push_back(row);
  }
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", checks},
    {"phase", 3},
    {"mesh", encode(mesh)},
    {"confinedMesh", encode(mountainMesh)},
    {"paths", paths(routed)},
    {"confinedPaths", paths(mountainPath)},
    {"detourPaths", paths(detour)},
    {"routes", rows},
    {"hydraulics", hydraulic},
    {"terrainHeights", heightRows},
    {"waterArea", mesh.waterAreaMetres2},
    {"outletDischarge", channels.ledger()[0].outgoing}};
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    file << report.dump(2) << '\n';
  }
  std::cout << "METRIC routes " << rows.dump() << " waterArea " << mesh.waterAreaMetres2 << " triangles " << mesh.triangles.size() << " waterComparison " << maxError << '\n';
  return failures ? 1 : 0;
}
