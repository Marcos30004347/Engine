#include "json.hpp"
#include "procedural/world/WorldRiverGraph.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural;
using namespace procedural::world;
using namespace procedural::world::rewrite;
using nlohmann::json;
json encode(const RiverGrowthProduct &p)
{
  json nodes = json::array();
  for (auto n : p.nodes)
    nodes.push_back({{"position", {n.position.x, n.position.y, n.position.z}}, {"receiver", n.receiver}, {"priority", n.growthPriority}, {"order", n.strahlerOrder}});
  return {{"nodes", nodes}, {"productions", p.productions}, {"rejections", p.rejections}, {"attempts", p.attempts}, {"exhaustedChildren", p.exhaustedChildren}, {"budgetReached", p.budgetReached}};
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
  std::vector<VoronoiCell> cells;
  for (int z = 0; z < 20; ++z)
    for (int x = 0; x < 20; ++x)
    {
      double a = x * 100., b = z * 100.;
      cells.push_back({{a + 50, b + 50}, {{a, b}, {a + 100, b}, {a + 100, b + 100}, {a, b + 100}}, {}});
    }
  LandSettings ls;
  ls.cores = {210};
  ls.landFraction = .7;
  LandProduct land;
  std::string error;
  check(buildLandProduct(spec, cells, ls, land, &error), "River fixture has validated frozen coast");
  if (land.coasts.empty())
    return 1;
  ReliefControls relief;
  relief.build({}, {.01, .12});
  RiverMouth mouth;
  mouth.coastEdge = land.coasts[0].edges.front().edgeId;
  mouth.priority = 4;
  RiverGrowthSettings settings;
  settings.maximumNodes = 180;
  settings.edgeLengthMetres = 100;
  settings.coastClearanceMetres = 20;
  settings.separationMetres = 25;
  RiverGrowthProduct graph;
  bool ok = growRiverGraph(land, relief, {mouth}, settings, graph, &error);
  check(ok, "Mouth-first river graph builds");
  if (!ok)
  {
    std::cerr << error << '\n';
    return 1;
  }
  check(graph.nodes.size() > 10, "Grammar grows a nontrivial network into land");
  bool drainage = true, lipschitz = true, orders = true, lengths = true;
  for (size_t i = 0; i < graph.nodes.size(); ++i)
  {
    auto n = graph.nodes[i];
    if (n.receiver != UINT32_MAX)
    {
      drainage &= n.receiver < i && n.position.y > graph.nodes[n.receiver].position.y;
      auto p = graph.nodes[n.receiver].position;
      lengths &= std::abs(std::hypot(n.position.x - p.x, n.position.z - p.z) - 100) < 1e-8;
    }
    uint32_t high = 0, count = 0;
    for (auto c : graph.nodes)
      if (c.receiver == i)
      {
        if (c.strahlerOrder > high)
        {
          high = c.strahlerOrder;
          count = 1;
        }
        else if (c.strahlerOrder == high)
          ++count;
      }
    orders &= n.strahlerOrder == (high ? high + (count > 1) : 1);
    for (auto other : graph.nodes)
      lipschitz &= std::abs(n.position.y - other.position.y) <= settings.maximumSlope * std::hypot(n.position.x - other.position.x, n.position.z - other.position.z) + 1e-9;
  }
  check(drainage, "Every upstream edge rises and every receiver reaches an earlier outlet");
  check(lengths, "Generated edges have configured metric length");
  check(lipschitz, "All node pairs satisfy slope compatibility");
  check(orders, "Strahler order repaired from surviving children");
  RiverGrowthProduct repeat;
  check(growRiverGraph(land, relief, {mouth}, settings, repeat, &error) && encode(repeat) == encode(graph), "River graph and rejection counters repeat exactly");
  auto snapshot = encode(repeat);
  RiverMouth invalid = mouth;
  invalid.coastEdge = UINT64_MAX;
  check(!growRiverGraph(land, relief, {invalid}, settings, repeat, &error) && encode(repeat) == snapshot, "Invalid mouth rejected atomically");
  RiverGrowthSettings tiny = settings;
  tiny.maximumNodes = 2;
  check(growRiverGraph(land, relief, {mouth}, tiny, repeat, &error) && repeat.budgetReached && repeat.nodes.size() == 2, "Node budget produces explicitly truncated graph");
  RiverGrowthSettings exhausted = settings;
  exhausted.coastClearanceMetres = 10000;
  check(
      growRiverGraph(land, relief, {mouth}, exhausted, repeat, &error) && repeat.nodes.size() == 1 && !repeat.budgetReached && repeat.exhaustedChildren > 0 && repeat.attempts <= 10 * exhausted.attemptsPerChild,
      "Impossible expansion stops with bounded rejection report");
  RiverGrowthSettings estuary = exhausted;
  estuary.mouthClearanceScale = .001;
  check(
      growRiverGraph(land, relief, {mouth}, estuary, repeat, &error) && repeat.nodes.size() > 1 && !repeat.budgetReached &&
          std::all_of(repeat.nodes.begin() + 1, repeat.nodes.end(), [](const auto &node) { return node.receiver == 0; }),
      "Estuary clearance permits the first step but cannot bypass interior clearance");
  const auto estuarySnapshot = encode(repeat);
  estuary.mouthClearanceScale = 0;
  check(!growRiverGraph(land, relief, {mouth}, estuary, repeat, &error) && encode(repeat) == estuarySnapshot,
        "Invalid estuary clearance rejected atomically");
  estuary.mouthClearanceScale = 1.1;
  check(!growRiverGraph(land, relief, {mouth}, estuary, repeat, &error) && encode(repeat) == estuarySnapshot,
        "Estuary clearance cannot exceed the interior setting");
  RiverGrowthSettings symmetric = settings;
  symmetric.continuationProbability = 0;
  symmetric.symmetricProbability = 1;
  check(
      growRiverGraph(land, relief, {mouth}, symmetric, repeat, &error) && repeat.productions[1] > 0 && repeat.productions[0] == 0 && repeat.productions[2] == 0,
      "Pure symmetric preset uses only symmetric and filling rules");
  RiverGrowthSettings asymmetric = settings;
  asymmetric.continuationProbability = 0;
  asymmetric.symmetricProbability = 0;
  check(
      growRiverGraph(land, relief, {mouth}, asymmetric, repeat, &error) && repeat.productions[2] > 0 && repeat.productions[0] == 0 && repeat.productions[1] == 0,
      "Pure asymmetric preset uses only asymmetric and filling rules");
  // Incremental runoff verifies receiver direction against the conservative core;
  // catchment areas and physical runoff assignment belong to later stages.
  std::vector<FlowNode> flow;
  std::vector<FlowLink> links;
  for (size_t i = 0; i < graph.nodes.size(); ++i)
  {
    flow.push_back({i + 1, 1, 0});
    if (graph.nodes[i].receiver != UINT32_MAX)
      links.push_back({i + 1, graph.nodes[i].receiver + 1, 1});
  }
  std::vector<FlowResult> ledger;
  check(accumulateDischarge(flow, links, ledger, &error) && ledger[0].outgoingCumecs == graph.nodes.size(), "Unit runoff merges conservatively along generated receivers");
  bool noCrossings = true, onLand = true;
  auto cross = [](Position a, Position b, Position c)
  {
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
  };
  for (size_t i = 0; i < graph.nodes.size(); ++i)
  {
    auto a = graph.nodes[i];
    if (a.receiver == UINT32_MAX)
      continue;
    auto b = graph.nodes[a.receiver];
    for (int sample = 1; sample <= 10; ++sample)
    {
      double t = sample / 10.;
      double x = b.position.x + (a.position.x - b.position.x) * t, z = b.position.z + (a.position.z - b.position.z) * t;
      int col = int(std::floor(x / 100)), row = int(std::floor(z / 100));
      onLand &= col >= 0 && col < 20 && row >= 0 && row < 20 && land.cells[row * 20 + col].land;
    }
    for (size_t j = i + 1; j < graph.nodes.size(); ++j)
    {
      auto c = graph.nodes[j];
      if (c.receiver == UINT32_MAX || c.receiver == a.receiver || c.receiver == i || a.receiver == j)
        continue;
      auto d = graph.nodes[c.receiver];
      noCrossings &= !(cross(a.position, b.position, c.position) * cross(a.position, b.position, d.position) < 0 && cross(c.position, d.position, a.position) * cross(c.position, d.position, b.position) < 0);
    }
  }
  check(noCrossings && onLand, "Independent exported-edge check finds no crossings or sampled ocean incursions");
  std::array<uint32_t, 3> totals{};
  for (uint64_t seed = 1; seed <= 24; ++seed)
  {
    LandProduct seeded = land;
    seeded.spec.seed = seed;
    RiverGrowthProduct sample;
    if (!growRiverGraph(seeded, relief, {mouth}, settings, sample, &error))
    {
      ++failures;
      break;
    }
    for (size_t i = 0; i < 3; ++i)
      totals[i] += sample.productions[i];
  }
  double trials = totals[0] + totals[1] + totals[2];
  check(
      trials >= 100 && std::abs(totals[0] / trials - .2) < .15 && std::abs(totals[1] / trials - .4) < .15 && std::abs(totals[2] / trials - .4) < .15,
      "Twenty-four seeds meet predeclared production-frequency interval of 0.15");
  RiverMouth second = mouth;
  second.coastEdge = land.coasts[0].edges[land.coasts[0].edges.size() / 2].edgeId;
  second.priority = 3;
  RiverGrowthProduct multi, reordered;
  check(
      growRiverGraph(land, relief, {mouth, second}, settings, multi, &error) && growRiverGraph(land, relief, {second, mouth}, settings, reordered, &error) && encode(multi) == encode(reordered),
      "Mouth publication order does not change growth");
  json coasts = json::array();
  for (auto &loop : land.coasts)
  {
    json ring = json::array();
    for (auto use : loop.edges)
    {
      auto e = land.boundaries.find(use.edgeId);
      auto p = (use.reversed ? e->vertices.back() : e->vertices.front()).position;
      ring.push_back({p.x, p.z});
    }
    coasts.push_back(ring);
  }
  json report = {{"status", failures ? "FAIL" : "PASS"}, {"fixture", "rivers"}, {"assertions", assertions}, {"graph", encode(graph)}, {"coasts", coasts}, {"frequencyTrials", trials}, {"frequencyCounts", totals}};
  if (argc > 1)
  {
    std::ofstream f(argv[1]);
    f << report.dump(2) << '\n';
    if (!f)
      return 2;
  }
  return failures ? 1 : 0;
}
