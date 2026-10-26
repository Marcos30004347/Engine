#include "json.hpp"
#include "procedural/world/WorldGenerationContracts.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <random>
using namespace procedural::world::rewrite;
using nlohmann::json;

json dump(const BoundaryRegistry &registry)
{
  json result = json::array();
  for (const auto &edge : registry.records())
  {
    json vertices = json::array();
    for (const auto &v : edge.vertices)
      vertices.push_back({{"owner", v.owner}, {"index", v.index}, {"position", {v.position.x, v.position.y, v.position.z}}});
    result.push_back({{"edge", edge.id}, {"vertices", vertices}});
  }
  return result;
}
int main(int argc, char **argv)
{
  json assertions = json::array();
  int failures = 0;
  auto check = [&](bool pass, const char *name)
  {
    assertions.push_back({{"name", name}, {"pass", pass}});
    std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
    failures += !pass;
  };
  std::string error;
  WorldSpec spec;
  check(spec.validate(&error), "valid world specification");
  spec.extentMetres = std::numeric_limits<double>::quiet_NaN();
  check(!spec.validate(&error), "nonfinite world extent rejected");
  check(featureSeed(7, 1, 3, 4) != featureSeed(7, 1, 3, 5) && featureSeed(7, 1, 3, 4) != featureSeed(7, 2, 3, 4), "purpose and version change random stream");
  BoundaryEndpoint a{1, {0, 0, 0}}, b{2, {0, 10, 100}}, c{3, {100, 5, 100}}, d{4, {-100, 5, 100}};
  std::vector<BoundaryInput> inputs = {{10, a, b, {.25, .75}}, {10, b, a, {.5, .75}}, {11, b, c, {.5}}, {12, b, d, {.5}}};
  BoundaryRegistry registry;
  check(registry.commit(inputs, &error), "shared boundary commit");
  const auto original = dump(registry);
  check(registry.find(10) && registry.find(10)->vertices.size() == 5, "neighbor split contributions reconciled");
  check(
      registry.find(10)->vertices.back().index == registry.find(11)->vertices.front().index && registry.find(11)->vertices.front().index == registry.find(12)->vertices.front().index,
      "three-region junction has one vertex identity");
  std::vector<std::future<json>> jobs;
  for (unsigned seed = 0; seed < 8; ++seed)
    jobs.push_back(
        std::async(
            std::launch::async,
            [=]
            {
              auto shuffled = inputs;
              std::mt19937 rng(seed);
              std::shuffle(shuffled.begin(), shuffled.end(), rng);
              BoundaryRegistry copy;
              if (!copy.commit(shuffled))
                return json();
              return dump(copy);
            }));
  bool deterministic = true;
  for (auto &job : jobs)
    deterministic &= job.get() == original;
  check(deterministic, "eight worker schedules preserve boundary snapshot");
  check(json::parse(original.dump()) == original, "boundary snapshot JSON round trip");
  auto bad = inputs;
  bad[1].from.position.y += 1;
  check(!registry.commit(bad, &error) && dump(registry) == original, "inconsistent endpoint rejected atomically");
  bad = inputs;
  bad[0].splits.push_back(-.1);
  check(!registry.commit(bad, &error) && dump(registry) == original, "invalid split cannot publish partial state");
  bad = inputs;
  bad[1].to.id = 99;
  check(!registry.commit(bad, &error), "conflicting edge identity rejected");

  std::vector<FlowNode> nodes = {{1, 2, 0}, {2, 3, 0}, {3, .5, 0}, {4, 0, 0}, {5, 0, 0}};
  std::vector<FlowLink> links = {{1, 3, 1}, {2, 3, 1}, {3, 4, .3}, {3, 5, .7}};
  std::vector<FlowResult> flows;
  check(accumulateDischarge(nodes, links, flows, &error), "merge and split flow graph resolves");
  const auto baselineFlows = flows;
  check(std::abs(flows[2].outgoingCumecs - 5.5) < 1e-12, "2 + 3 + 0.5 = 5.5 cumecs");
  check(std::abs(flows[3].outgoingCumecs + flows[4].outgoingCumecs - 5.5) < 1e-12, "30/70 split conserves water");
  nodes[2].lossCumecs = .25;
  check(accumulateDischarge(nodes, links, flows, &error) && std::abs(flows[2].outgoingCumecs - 5.25) < 1e-12, "declared loss produces 5.25 cumecs");
  std::reverse(nodes.begin(), nodes.end());
  std::reverse(links.begin(), links.end());
  std::vector<FlowResult> reordered;
  bool flowSame = accumulateDischarge(nodes, links, reordered, &error);
  for (size_t i = 0; flowSame && i < flows.size(); ++i)
    flowSame &= flows[i].id == reordered[i].id && flows[i].outgoingCumecs == reordered[i].outgoingCumecs;
  check(flowSame, "flow accumulation independent of input order");
  auto cycle = links;
  cycle.push_back({4, 1, 1});
  check(!accumulateDischarge(nodes, cycle, reordered, &error), "cycle rejected");
  auto duplicate = links;
  duplicate.push_back(links.front());
  check(!accumulateDischarge(nodes, duplicate, reordered, &error), "duplicate link rejected");
  auto wrongSplit = links;
  wrongSplit[0].fraction = .2;
  check(!accumulateDischarge(nodes, wrongSplit, reordered, &error), "unbalanced split rejected");
  nodes.back().lossCumecs = 100;
  check(!accumulateDischarge(nodes, links, reordered, &error), "loss exceeding supply rejected");

  RoadDesignProfile road;
  check(road.build({{0, 10, 3, .02}, {100, 15, 3, .02}}, .10, .02, &error), "five percent inclined road accepted");
  double h = 0;
  check(road.height(50, 3, h) && std::abs(h - 12.56) < 1e-12, "road plane has controlled two percent cross slope");
  check(!road.height(50, 3.1, h) && !road.height(-1, 0, h), "road domain cannot extrapolate across hillside");
  check(!road.build({{0, 10, 3, 0}, {100, 30, 3, 0}}, .10, .02, &error), "excess center grade rejected");
  check(!road.build({{0, 10, 3, -.02}, {1, 10.09, 3, .02}}, .10, .02, &error), "changing cross slope checks whole panel grade");
  check(road.height(50, 0, h) && std::abs(h - 12.5) < 1e-12, "failed road replacement preserves previous design");
  check(!road.build({{0, 10, 3, .3}, {100, 15, 3, .3}}, .10, .02, &error), "hillside lateral tilt rejected");

  json sections = json::array();
  for (int d = 0; d <= 100; d += 10)
  {
    json values = json::array();
    for (int l = -3; l <= 3; ++l)
    {
      road.height(d, l, h);
      values.push_back({{"lateral", l}, {"road", h}, {"ground", 10 + .05 * d + .3 * l}});
    }
    sections.push_back({{"distance", d}, {"samples", values}});
  }
  json flowDump = json::array();
  for (const auto &f : baselineFlows)
    flowDump.push_back({{"id", f.id}, {"incoming", f.incomingCumecs}, {"outgoing", f.outgoingCumecs}});
  if (argc == 2)
  {
    std::ofstream out(argv[1]);
    if (!out)
    {
      std::cerr << "cannot open report\n";
      return 2;
    }
    out << json({{"schema", 1}, {"seed", 7}, {"assertions", assertions}, {"boundaries", original}, {"flow", flowDump}, {"roadSections", sections}, {"status", failures ? "FAIL" : "PASS"}}).dump(2) << '\n';
    if (!out)
    {
      std::cerr << "cannot write report\n";
      return 2;
    }
  }
  return failures ? 1 : 0;
}
