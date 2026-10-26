// Integration export of the rewrite's real land, drainage, watershed and relief products.
#include "json.hpp"
#include "procedural/world/WorldCoastDetail.hpp"
#include "procedural/world/WorldLandGenerator.hpp"
#include "procedural/world/WorldReliefSurface.hpp"
#include "procedural/world/WorldRiverChannels.hpp"
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
int main(int argc, char **argv)
{
  if (argc != 2 && argc != 3 && argc != 4)
  {
    std::cerr << "usage: WorldIslandFoundation output.json [maximumNodes [--growth-only|--headwater-coasts|--mountain-range]]\n";
    return 2;
  }
  auto start = std::chrono::steady_clock::now();
  WorldSpec spec;
  spec.seed = 27;
  spec.extentMetres = 6000;
  LandProduct planning, land;
  std::string error;
  auto fail = [&]
  {
    std::cerr << "FAIL " << error << '\n';
    return 1;
  };
  if (!generateLand(spec, 420, {{3000, 0, 3000}}, .52, planning, &error, 180) || !detailLandCoasts(planning, {}, land, &error))
    return fail();
  std::cout << "PASS physical island: " << land.cells.size() << " cells\n";
  ReliefControls controls;
  const bool guidedRange = argc == 4 && std::string(argv[3]) == "--mountain-range";
  const std::vector<Position> rangeGuide = {{1750, 0, 2600}, {2150, 0, 3100}, {2600, 0, 3850}, {3300, 0, 4250}, {3950, 0, 4150}};
  auto rangeDistance = [&](Position p)
  {
    double distance = 1e30;
    for (size_t i = 1; i < rangeGuide.size(); ++i)
    {
      auto a = rangeGuide[i - 1], b = rangeGuide[i];
      double dx = b.x - a.x, dz = b.z - a.z;
      double t = std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
      distance = std::min(distance, std::hypot(p.x - a.x - t * dx, p.z - a.z - t * dz));
    }
    return distance;
  };
  // Two independent slope maps, evaluated BEFORE growing drainage (paper §3).
  // Opinionated regional preset: reserve only one fifth of inland planning
  // regions for the range. These control slopes BEFORE the river grammar.
  std::vector<std::pair<double, std::pair<uint32_t, Position>>> ranked;
  for (const auto &cell : land.cells)
    if (cell.land)
    {
      Position centre{};
      size_t count = 0;
      for (auto use : cell.patch.outer)
      {
        const auto &vertices = land.boundaries.find(use.edgeId)->vertices;
        const auto p = vertices[use.reversed ? vertices.size() - 1 : 0].position;
        centre.x += p.x;
        centre.z += p.z;
        ++count;
      }
      centre.x /= count;
      centre.z /= count;
      // An elongated inland range, defined by region selection, not height uplift.
      const double score = guidedRange ? rangeDistance(centre) : std::pow((centre.x - 2850) / 1100, 2) + std::pow((centre.z - 3500) / 650, 2);
      ranked.push_back({score, {cell.id, centre}});
    }
  std::sort(
      ranked.begin(),
      ranked.end(),
      [](const auto &a, const auto &b)
      {
        return a.first < b.first;
      });
  std::vector<ReliefProvince> provinces;
  std::vector<uint32_t> mountainRegions;
  for (size_t i = 0; i < std::max<size_t>(1, ranked.size() / 5); ++i)
  {
    mountainRegions.push_back(ranked[i].second.first);
    auto p = ranked[i].second.second;
    provinces.push_back({i + 1, p, p, 650, .22, .25});
  }
  if (guidedRange)
  {
    provinces.clear();
    for (size_t i = 1; i < rangeGuide.size(); ++i)
      provinces.push_back({i, rangeGuide[i - 1], rangeGuide[i], 560, .18, .25, 4.});
  }
  if (!controls.build(provinces, {.003, .018}, &error))
    return fail();
  std::vector<RiverMouth> mouths;
  std::vector<Position> selected;
  // Select coastal outlets in four separated sectors; never fabricate ocean nodes.
  for (Position target : std::vector<Position>{{3000, 0, 0}, {0, 0, 3000}, {6000, 0, 3000}, {3000, 0, 6000}})
  {
    double best = 1e30;
    RiverMouth m;
    Position chosen;
    for (const auto &loop : land.coasts)
      if (loop.signedAreaMetres2 > 0)
        for (auto use : loop.edges)
        {
          const auto *e = land.boundaries.find(use.edgeId);
          auto a = e->vertices.front().position, b = e->vertices.back().position;
          Position p{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
          bool clear = true;
          for (auto q : selected)
            clear &= std::hypot(p.x - q.x, p.z - q.z) > 1000;
          double d = std::hypot(p.x - target.x, p.z - target.z);
          if (clear && d < best)
          {
            best = d;
            m = {use.edgeId, .5, 4};
            chosen = p;
          }
        }
    if (best < 1e29)
    {
      mouths.push_back(m);
      selected.push_back(chosen);
    }
  }
  RiverGrowthSettings growth;
  growth.edgeLengthMetres = 240;
  growth.maximumNodes = 2048;
  if (argc >= 3)
  {
    const std::string value = argv[2];
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), growth.maximumNodes);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || growth.maximumNodes < 4 || growth.maximumNodes > 4096 || (argc == 4 && std::string(argv[3]) != "--growth-only" && std::string(argv[3]) != "--headwater-coasts" && !guidedRange))
    {
      std::cerr << "FAIL invalid river budget or mode\n";
      return 2;
    }
  }
  growth.coastClearanceMetres = 180;
  growth.separationMetres = 180;
  growth.maximumCoastalSlope = .3;
  growth.maximumElevationPerTerrainLambda = 1200;
  growth.continuationProbability = .2;
  growth.symmetricProbability = .1;
  if (guidedRange || (argc == 4 && std::string(argv[3]) == "--headwater-coasts"))
  {
    // The paper's mouths must be able to seed an inland drainage tree. Applying
    // the full interior clearance at the first step can strand an entire cove.
    growth.mouthClearanceScale = .3;
    growth.attemptsPerChild = 32;
  }
  RiverGrowthProduct rivers;
  if (!growRiverGraph(land, controls, mouths, growth, rivers, &error))
    return fail();
  if (argc == 4 && std::string(argv[3]) == "--growth-only")
  {
    std::ofstream file(argv[1]);
    file << json({{"nodes", rivers.nodes.size()}, {"budgetReached", rivers.budgetReached}, {"attempts", rivers.attempts}, {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()}})
                .dump(2)
         << '\n';
    return file ? 0 : 2;
  }
  if (rivers.budgetReached)
  {
    error = "Drainage frontier exceeded its node budget; no complete foundation published";
    return fail();
  }
  WatershedProduct watersheds;
  if (!buildWatersheds(land, rivers, controls, false, watersheds, &error))
    return fail();
  WorldReliefSurface surface;
  ReliefSurfaceSettings relief;
  relief.sampleSpacingMetres = 110;
  relief.poissonSamplesPerCell = 50;
  relief.maximumCoastalSlope = .35;
  relief.roughnessFraction = .04;
  relief.shoreTransitionMetres = 100;
  if (!surface.build(land, rivers, watersheds, relief, &error))
    return fail();
  // Terrain input needs conservative runoff and section sizes; confluence
  // surface blending belongs to the later continuous channel-profile stage.
  std::vector<double> runoff(rivers.nodes.size()), discharge(rivers.nodes.size());
  for (const auto &cell : watersheds.cells)
    runoff[cell.riverNode] = discharge[cell.riverNode] = cell.areaMetres2 * 5 * .45 / 3600000.;
  for (size_t i = rivers.nodes.size(); i-- > 0;)
    if (rivers.nodes[i].receiver != UINT32_MAX)
      discharge[rivers.nodes[i].receiver] += discharge[i];
  ChannelSettings channel;
  channel.widthPerSqrtDischarge = 2;
  channel.minimumBottomWidthMetres = 2;
  channel.maximumBottomWidthMetres = 24;

  json out = {{"schema", 1}, {"seed", spec.seed}, {"extent", spec.extentMetres}, {"source", "C++ rewrite land/coast/river/watershed/relief/channel stages"}, {"drainageTruncated", rivers.budgetReached}};
  out["riverGrowthControls"] = {{"edgeLengthMetres", growth.edgeLengthMetres}, {"coastClearanceMetres", growth.coastClearanceMetres},
                               {"mouthClearanceScale", growth.mouthClearanceScale}, {"attemptsPerChild", growth.attemptsPerChild},
                               {"continuationProbability", growth.continuationProbability}, {"symmetricProbability", growth.symmetricProbability}};
  if (guidedRange)
  {
    out["mountainRangeGuide"] = json::array();
    for (auto p : rangeGuide)
      out["mountainRangeGuide"].push_back({p.x, p.z});
    out["mountainRangeControls"] = {{"riverSlope", .18}, {"terrainLambda", .25}, {"strength", 4.}, {"supportRadiusMetres", 560}, {"role", "slope controls before hydrology; not height uplift"}};
  }
  double assignedArea = 0;
  for (const auto &cell : watersheds.cells)
    assignedArea += cell.areaMetres2;
  out["drainageCoverage"] = {
    {"growthFinished", !rivers.budgetReached},
    {"sourceNodes", rivers.nodes.size()},
    {"landAreaMetres2", land.landAreaMetres2},
    {"assignedCatchmentAreaMetres2", assignedArea},
    {"unassignedAreaMetres2", std::abs(land.landAreaMetres2 - assignedArea)},
    {"oceanOutlets", mouths.size()}};
  out["coasts"] = json::array();
  for (auto loop : land.coasts)
  {
    json ring = json::array();
    for (auto use : loop.edges)
    {
      auto verts = land.boundaries.find(use.edgeId)->vertices;
      if (use.reversed)
        std::reverse(verts.begin(), verts.end());
      for (size_t i = 0; i + 1 < verts.size(); ++i)
        ring.push_back({verts[i].position.x, verts[i].position.z});
    }
    out["coasts"].push_back({{"ring", ring}, {"area", loop.signedAreaMetres2}});
  }
  out["nodes"] = json::array();
  for (auto n : rivers.nodes)
    out["nodes"].push_back({{"position", {n.position.x, n.position.y, n.position.z}}, {"receiver", n.receiver}, {"order", n.strahlerOrder}});
  out["reaches"] = json::array();
  out["ledger"] = json::array();
  for (size_t i = 0; i < rivers.nodes.size(); ++i)
  {
    auto node = rivers.nodes[i];
    out["ledger"].push_back({{"id", i + 1}, {"local", runoff[i]}, {"incoming", discharge[i] - runoff[i]}, {"loss", 0}, {"outgoing", discharge[i]}});
    if (node.receiver == UINT32_MAX)
      continue;
    auto down = rivers.nodes[node.receiver].position;
    const double slope = (node.position.y - down.y) / std::hypot(node.position.x - down.x, node.position.z - down.z);
    ChannelSection section;
    if (!solveChannelSection(discharge[i], slope, channel, section, &error))
      return fail();
    out["reaches"].push_back({{"from", i + 1}, {"to", node.receiver + 1}, {"discharge", discharge[i]}, {"width", section.waterWidth}, {"depth", section.depth}, {"velocity", section.velocity}});
  }
  out["planningRegions"] = json::array();
  for (const auto &cell : land.cells)
    if (cell.land)
    {
      json ring = json::array();
      for (auto use : cell.patch.outer)
      {
        auto vertices = land.boundaries.find(use.edgeId)->vertices;
        if (use.reversed)
          std::reverse(vertices.begin(), vertices.end());
        for (size_t i = 0; i + 1 < vertices.size(); ++i)
          ring.push_back({vertices[i].position.x, vertices[i].position.z});
      }
      out["planningRegions"].push_back({{"id", cell.id}, {"ring", ring}});
    }
  // Unstructured scalar probes from the relief primitives. No height grid or
  // presentation mesh: downstream map solvers may query these analysis samples.
  out["terrainSites"] = json::array();
  const auto &primitives = surface.field().primitives();
  const size_t stride = std::max<size_t>(1, (primitives.size() + 4095) / 4096);
  size_t valid = 0;
  for (size_t i = 0; i < primitives.size(); i += stride)
  {
    auto p = primitives[i].centre;
    double height = 0;
    if (surface.sample(p.x, p.z, height))
    {
      out["terrainSites"].push_back({p.x, p.z, height});
      ++valid;
    }
  }
  // Full paper products: no artistic elevation adjustment downstream.
  out["terrainMethod"] = "siggraph2013-watershed-primitives-v4";
  out["terrainRevision"] = 5;
  out["mountainRegions"] = mountainRegions;
  out["slopeControls"] = {{"lowland", {.003, .018}}, {"provinces", json::array()}};
  for (auto p : provinces)
    out["slopeControls"]["provinces"].push_back({{"from", {p.from.x, p.from.z}}, {"to", {p.to.x, p.to.z}}, {"radius", p.radiusMetres}, {"riverSlope", p.riverSlope}, {"terrainLambda", p.terrainLambda}, {"strength", p.strength}});
  out["crestVertices"] = json::array();
  for (auto v : watersheds.vertices)
    out["crestVertices"].push_back({{"position", {v.position.x, v.position.y, v.position.z}}, {"coastal", v.coastal}, {"sites", v.equidistantSites}});
  out["watershedEdges"] = json::array();
  for (auto e : watersheds.edges)
    out["watershedEdges"].push_back({{"a", e.a}, {"b", e.b}, {"left", e.leftCell}, {"right", e.rightCell}, {"riverCrossing", e.riverCrossing}});
  out["terrainPrimitives"] = json::array();
  for (auto p : primitives)
    out["terrainPrimitives"].push_back(
        {{"id", p.id}, {"centre", {p.centre.x, p.centre.y, p.centre.z}}, {"radius", p.radiusMetres}, {"noiseAmplitude", p.noiseAmplitudeMetres}, {"noiseWavelength", p.noiseWavelengthMetres}, {"noiseSeed", p.noiseSeed}});
  // A scalar analysis raster, not a mesh or a replacement for Voronoi regions.
  constexpr int side = 513;
  out["scalarField"] = {{"side", side}, {"rowOrder", "south-to-north"}, {"height", json::array()}, {"supported", json::array()}};
  for (int z = 0; z < side; ++z)
    for (int x = 0; x < side; ++x)
    {
      double h = 0;
      bool supported = surface.field().sample(x * spec.extentMetres / (side - 1), z * spec.extentMetres / (side - 1), h);
      out["scalarField"]["height"].push_back(h);
      out["scalarField"]["supported"].push_back(supported);
    }
  std::cout << "PASS exact compact field: " << primitives.size() << " primitives, " << watersheds.vertices.size() << " crest vertices, 513 x 513 scalar samples\n";
  out["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::ofstream file(argv[1]);
  file << out.dump() << '\n';
  if (!file)
    return 2;
  std::cout << "PASS foundation export: " << rivers.nodes.size() << " river nodes, " << valid << " terrain samples; truncated=" << rivers.budgetReached << " seconds=" << out["seconds"] << '\n';
}
