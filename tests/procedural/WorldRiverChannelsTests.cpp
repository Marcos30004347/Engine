#include "json.hpp"
#include "procedural/world/WorldChannelMesh.hpp"
#include "procedural/world/WorldPhysicalTerrain.hpp"
#include "procedural/world/WorldRiverChannels.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
json encode(const ChannelMeshProduct &m)
{
  json v = json::array();
  for (auto p : m.vertices)
    v.push_back({p.x, p.y, p.z});
  return {{"vertices", v}, {"triangles", m.triangles}, {"roles", m.roles}};
}
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
  ChannelSettings settings;
  settings.confluenceBlendMetres = 8;
  std::vector<ChannelNode> nodes = {{1, {-30, 3.4, 40}, 0}, {2, {30, 3.4, 40}, 0}, {3, {0, 3, 0}, 0}, {4, {0, 2.4, -60}, 0}};
  std::vector<FlowLink> links = {{1, 3, 1}, {2, 3, 1}, {3, 4, 1}};
  std::vector<CatchmentRunoff> runoff = {{1, 1, 2000000, 3.6, 1}, {2, 2, 3000000, 3.6, 1}, {3, 3, 500000, 3.6, 1}};
  WorldRiverChannels channel;
  check(channel.build(nodes, links, runoff, settings, &error), "Incremental catchments produce complete channel network");
  if (!error.empty())
    std::cerr << error << '\n';
  auto outlet = [&](const WorldRiverChannels &c)
  {
    return c.ledger().back().outgoing;
  };
  check(std::abs(outlet(channel) - 5.5) < 1e-10, "Two plus three plus local half conserves 5.5 cubic metres per second");
  auto lossNodes = nodes;
  lossNodes[2].lossCumecs = .25;
  WorldRiverChannels loss;
  check(loss.build(lossNodes, links, runoff, settings, &error) && std::abs(outlet(loss) - 5.25) < 1e-10, "Declared quarter cubic metre loss gives 5.25 outlet");
  auto splitNodes = nodes;
  splitNodes.push_back({5, {30, 2.4, -60}, 0});
  auto splitLinks = links;
  splitLinks.back().fraction = .3;
  splitLinks.push_back({3, 5, .7});
  WorldRiverChannels split;
  check(split.build(splitNodes, splitLinks, runoff, settings, &error), "Explicit 30 70 bifurcation builds");
  if (!error.empty())
    std::cerr << "split: " << error << "\n";
  double splitQ = 0;
  for (auto row : split.ledger())
    if (row.nodeId >= 4)
      splitQ += row.outgoing;
  check(std::abs(splitQ - 5.5) < 1e-10, "Split outlet totals preserve incoming volume");
  auto duplicate = runoff;
  duplicate.push_back(runoff[0]);
  check(!channel.build(nodes, links, duplicate, settings), "Duplicate catchment ownership rejected without double counting");
  check(std::abs(outlet(channel) - 5.5) < 1e-10, "Failed channel build preserves previous product");
  auto cycle = links;
  cycle.push_back({4, 1, 1});
  check(!channel.build(nodes, cycle, runoff, settings), "Cyclic river topology rejected");
  auto uphill = nodes;
  uphill.back().water.y = 5;
  check(!channel.build(uphill, links, runoff, settings), "Uphill receiver rejected");
  auto badLoss = nodes;
  badLoss[0].lossCumecs = 3;
  check(!channel.build(badLoss, links, runoff, settings), "Loss exceeding available water rejected");
  ChannelSection low, high;
  auto confined = settings;
  confined.minimumBottomWidthMetres = confined.maximumBottomWidthMetres = 4;
  check(
      solveChannelSection(2, .005, confined, low) && solveChannelSection(8, .005, confined, high) && high.depth > low.depth && high.capacity > low.capacity,
      "Confined channel gets deeper and greater capacity as runoff increases");
  check(std::abs(high.discharge - high.area * high.velocity) < 1e-10 && std::abs(high.capacity - high.discharge) < 1e-8, "Manning capacity and Q equals area times velocity agree");
  auto tooSmall = confined;
  tooSmall.maximumDepthMetres = .01;
  check(!solveChannelSection(8, .005, tooSmall, high), "Unsupported discharge depth is rejected");
  check(!solveChannelSection(-1, .005, confined, high), "Negative discharge rejected");
  RiverGrowthProduct graph;
  graph.nodes = {{{0, 2.4, -60}, UINT32_MAX, 1, 1}, {{0, 3, 0}, 0, 1, 1}, {{-30, 3.4, 40}, 1, 1, 1}, {{30, 3.4, 40}, 1, 1, 1}};
  WatershedProduct watershed;
  watershed.landAreaMetres2 = 5500000;
  watershed.cells = {{0, 0, {}}, {1, 500000, {}}, {2, 2000000, {}}, {3, 3000000, {}}};
  WorldRiverChannels adapted;
  check(adapted.buildFromWatersheds(graph, watershed, 3.6, 1, settings, false, &error) && std::abs(adapted.ledger()[0].outgoing - 5.5) < 1e-10, "Production watershed graph adapter uses incremental exclusive areas once");
  auto repeated = watershed;
  repeated.cells[3].riverNode = 2;
  check(!adapted.buildFromWatersheds(graph, repeated, 3.6, 1, settings), "Duplicate watershed cell rejected");
  repeated = watershed;
  repeated.landAreaMetres2 += 1e6;
  check(!adapted.buildFromWatersheds(graph, repeated, 3.6, 1, settings), "Cumulative catchment area mismatch rejected");
  graph.budgetReached = true;
  check(!adapted.buildFromWatersheds(graph, watershed, 3.6, 1, settings), "Truncated drainage requires explicit opt in");
  bool continuous = true, positive = true, transport = true;
  json sections = json::array();
  for (size_t i = 0; i < channel.reaches().size(); ++i)
  {
    auto r = channel.reaches()[i];
    double previous = 1e30;
    for (int k = 0; k <= 100; ++k)
    {
      ChannelSection p;
      double water;
      channel.section(i, r.length * k / 100., p, water);
      positive &= p.depth >= 0 && p.velocity >= 0;
      transport &= p.capacity + 1e-8 >= p.discharge && std::abs(p.area * p.velocity - p.discharge) < 1e-10;
      continuous &= water - p.depth <= previous + 1e-8;
      previous = water - p.depth;
    }
    ChannelSection p;
    double water;
    channel.section(i, r.length * .5, p, water);
    sections.push_back(
        {{"from", r.from}, {"to", r.to}, {"Q", p.discharge}, {"width", p.waterWidth}, {"bottomWidth", p.bottomWidth}, {"depth", p.depth}, {"water", water}, {"bed", water - p.depth}, {"velocity", p.velocity}});
  }
  check(positive && transport, "All blended reach stations retain positive depths and sufficient capacity");
  check(continuous, "Blended junction beds remain downhill without a single vertex step");
  auto base = [](double, double)
  {
    return 5.;
  };
  bool fieldOK = true;
  for (int z = -60; z <= 40; ++z)
  {
    ChannelSample sample;
    fieldOK &= channel.sample(0, z, base, sample);
    if (sample.wet)
      fieldOK &= sample.terrainHeight < sample.waterHeight;
  }
  check(fieldOK, "Meshing sampler resolves bed below water and continuous finite union");
  std::reverse(nodes.begin(), nodes.end());
  std::reverse(links.begin(), links.end());
  std::reverse(runoff.begin(), runoff.end());
  WorldRiverChannels reordered;
  bool same = reordered.build(nodes, links, runoff, settings);
  for (int z = -60; z <= 40; z += 2)
    for (int x = -40; x <= 40; x += 2)
    {
      ChannelSample a, b;
      channel.sample(x, z, base, a);
      reordered.sample(x, z, base, b);
      same &= a.terrainHeight == b.terrainHeight && a.waterHeight == b.waterHeight;
    }
  check(same, "Input order cannot change flow or channel geometry");
  ChannelMeshSettings meshSettings;
  meshSettings.spacingMetres = 1;
  ChannelMeshProduct lowMesh, highMesh;
  check(buildChannelMesh(channel, base, meshSettings, lowMesh, &error), "Confluence bed banks and single water union compile through CDT");
  if (!error.empty())
    std::cerr << error << '\n';
  auto highRunoff = runoff;
  for (auto &r : highRunoff)
    r.rainfallMmPerHour *= 4;
  WorldRiverChannels highFlow;
  check(highFlow.build(nodes, links, highRunoff, settings, &error) && buildChannelMesh(highFlow, base, meshSettings, highMesh, &error), "High flow changes actual bed bank and water geometry");
  check(highMesh.waterAreaMetres2 > lowMesh.waterAreaMetres2, "Four times runoff increases meshed wetted footprint");
  check(std::abs(lowMesh.terrainAreaMetres2 - 16800) < 1e-6, "Clipped bed land patches preserve total map area");
  std::map<std::pair<uint32_t, uint32_t>, int> waterEdges;
  std::set<uint32_t> waterVertices;
  int waterTriangles = 0;
  for (size_t i = 0; i < lowMesh.triangles.size(); ++i)
    if (lowMesh.roles[i])
    {
      auto t = lowMesh.triangles[i];
      ++waterTriangles;
      for (int j = 0; j < 3; ++j)
      {
        waterVertices.insert(t[j]);
        waterEdges[std::minmax(t[j], t[(j + 1) % 3])]++;
      }
    }
  bool manifold = true, bankSeams = true;
  for (auto [edge, count] : waterEdges)
  {
    manifold &= count <= 2;
    if (count == 1)
      for (auto id : {edge.first, edge.second})
      {
        auto p = lowMesh.vertices[id];
        bool found = false;
        for (size_t j = 0; j < lowMesh.vertices.size(); ++j)
        {
          auto q = lowMesh.vertices[j];
          if (j != id && p.x == q.x && p.z == q.z && std::abs(p.y - q.y) < 1e-7)
          {
            found = true;
            break;
          }
        }
        bankSeams &= found;
      }
  }
  check(manifold && int(waterVertices.size()) - int(waterEdges.size()) + waterTriangles == 1, "Confluence water is a single manifold disk without overlapping ribbons or holes");
  check(bankSeams, "Every water-bank boundary vertex meets the terrain mesh");
  // Integration of both phases: a reserved canyon through a plateau, then
  // conservative runoff and authoritative bed/water replacement on that surface.
  SplineFeature plateau;
  plateau.heightMetres = 20;
  for (int k = 0; k < 12; ++k)
  {
    double a = k * 6.283185307179586 / 12;
    plateau.controls.push_back({300 + 100 * std::cos(a), 0, 300 + 100 * std::sin(a)});
  }
  WorldSplineLandforms forms;
  forms.build({plateau});
  RiverGrowthProduct fixed;
  fixed.nodes = {{{600, -1.2, 300}, UINT32_MAX, 1, 1}, {{0, 0, 300}, 0, 1, 1}};
  RiverReservation reserve;
  reserve.upstreamNode = 1;
  WorldPhysicalTerrain physical;
  auto ground = [](double, double)
  {
    return 0.;
  };
  check(physical.build(forms, ground, {}, fixed, {reserve}), "Step 06 plateau canyon product available to final hydrology");
  WatershedProduct cells;
  cells.cells = {{0, 0, {}}, {1, 2000000, {}}};
  cells.landAreaMetres2 = 2000000;
  WorldRiverChannels finalChannels;
  check(finalChannels.buildFromWatersheds(fixed, cells, 3.6, 1, settings, false, &error) && finalChannels.validateReservations(physical, &error), "Step 07 final channels validate fixed canyon and outlet reservations");
  bool finalDownhill = true;
  double prior = 1e30;
  for (int x = 0; x <= 600; ++x)
  {
    ChannelSample p;
    finalChannels.sample(
        x,
        300,
        [&](double x, double z)
        {
          PhysicalSample p;
          physical.sample(x, z, p);
          return p.height;
        },
        p);
    finalDownhill &= p.wet && p.terrainHeight < p.waterHeight && p.terrainHeight <= prior + 1e-8;
    prior = p.terrainHeight;
  }
  check(finalDownhill, "Combined terrain and hydrology have an open downhill bed through the plateau to the outlet");
  auto huge = settings;
  huge.minimumBottomWidthMetres = huge.maximumBottomWidthMetres = 30;
  WorldRiverChannels tooWide;
  tooWide.buildFromWatersheds(fixed, cells, 3.6, 1, huge);
  check(!tooWide.validateReservations(physical), "Runoff or profile growth outside reserved canyon must rebuild terrain");
  json ledger = json::array();
  for (auto r : channel.ledger())
    ledger.push_back({{"id", r.nodeId}, {"area", r.incrementalArea}, {"local", r.localRunoff}, {"incoming", r.incoming}, {"loss", r.loss}, {"outgoing", r.outgoing}});
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", checks},
    {"phase", 7},
    {"mesh", encode(lowMesh)},
    {"highMesh", encode(highMesh)},
    {"ledger", ledger},
    {"sections", sections},
    {"lowWaterArea", lowMesh.waterAreaMetres2},
    {"highWaterArea", highMesh.waterAreaMetres2}};
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    file << report.dump(2) << '\n';
  }
  std::cout << "METRIC low_water_area " << lowMesh.waterAreaMetres2 << " high_water_area " << highMesh.waterAreaMetres2 << " triangles " << lowMesh.triangles.size() << '\n';
  return failures ? 1 : 0;
}
