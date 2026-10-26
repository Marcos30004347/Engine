#include "json.hpp"
#include "procedural/world/WorldSuitability.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
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
  SplineFeature plateau;
  plateau.id = 1;
  plateau.heightMetres = 8;
  plateau.faceWidthMetres = 0;
  plateau.weatheredWidthMetres = plateau.apronWidthMetres = 2;
  for (int k = 0; k < 12; ++k)
  {
    double t = k * 6.283185307179586 / 12;
    plateau.controls.push_back({215 + 42 * std::cos(t), 0, 160 + 42 * std::sin(t)});
  }
  SplineFeature talus;
  talus.id = 2;
  talus.heightMetres = 12;
  talus.faceWidthMetres = 8;
  talus.weatheredWidthMetres = 8;
  talus.apronWidthMetres = 12;
  for (int k = 0; k < 12; ++k)
  {
    double t = k * 6.283185307179586 / 12;
    talus.controls.push_back({255 + 18 * std::cos(t), 0, 55 + 18 * std::sin(t)});
  }
  WorldSplineLandforms forms;
  check(forms.build({plateau, talus}, {}, &error), "Landform inputs build");
  auto ground = [](double x, double z)
  {
    double d = std::hypot(x - 90, z - 62);
    return 8 * std::exp(-std::pow((d - 12) / 3, 2));
  };
  RiverGrowthProduct graph;
  graph.nodes = {{{45, -2, 0}, UINT32_MAX, 1, 1}, {{45, -1, 240}, 0, 1, 1}};
  // A gentle curved path is consumed by the final surface and flood mask.
  graph.reachPaths.resize(2);
  std::vector<double> lengths{0};
  for (int i = 0; i <= 60; ++i)
  {
    double t = i / 60.;
    Position p{45 + 8 * std::sin(6.283185307179586 * t) * std::pow(std::sin(3.141592653589793 * t), 2), 0, 240 * (1 - t)};
    if (i)
    {
      auto a = graph.reachPaths[1].back();
      lengths.push_back(lengths.back() + std::hypot(p.x - a.x, p.z - a.z));
    }
    graph.reachPaths[1].push_back(p);
  }
  for (size_t i = 0; i < lengths.size(); ++i)
    graph.reachPaths[1][i].y = -1 - lengths[i] / lengths.back();
  graph.reachPaths[1].front() = graph.nodes[1].position;
  graph.reachPaths[1].back() = graph.nodes[0].position;
  RiverReservation river{1, 8, 12, 1, 30};
  AccessRamp ramp;
  ramp.id = 10;
  ramp.fromFeature = 0;
  ramp.toFeature = 1;
  ramp.stations = {{115, 0, 158}, {195, 8, 158}};
  WorldPhysicalTerrain physical, unramped;
  check(physical.build(forms, ground, {ramp}, graph, {river}, &error), "Final physical terrain includes the access ramp and curved river cut");
  check(unramped.build(forms, ground, {}, graph, {river}, &error), "Inaccessible plateau comparison builds");
  WatershedProduct cells;
  cells.landAreaMetres2 = 2000000;
  cells.cells = {{0, 0, {}}, {1, 2000000, {}}};
  WorldRiverChannels channels;
  ChannelSettings cs;
  check(channels.buildFromWatersheds(graph, cells, 3.6, 1, cs, false, &error) && channels.validateReservations(physical, &error), "Final curved hydraulic profiles fit reserved terrain");
  auto soil = [](double x, double z)
  {
    return std::hypot(x - 215, z - 160) < 40 ? .95 : .65;
  };
  auto moisture = [](double, double)
  {
    return .5;
  };
  SuitabilitySettings settings;
  std::vector<Position> access = {{10, 0, 220}, {100, 0, 220}};
  WorldSuitability world, blocked;
  check(world.build(physical, channels, soil, moisture, access, settings, 1, &error), "Final surface suitability and access components build");
  if (world.cells().empty())
    return 1;
  check(blocked.build(unramped, channels, soil, moisture, access, settings, 1, &error), "Unramped suitability comparison builds");
  auto at = [&](const WorldSuitability &w, double x, double z) -> const SuitabilityCell &
  {
    return w.cells().at(w.cellAt(x, z));
  };
  check((at(blocked, 214, 162).reasons & Inaccessible) != 0, "An attractive but inaccessible cap is excluded");
  check(at(world, 214, 162).reasons == 0, "The same suitable cap becomes accessible through its ramp");
  check(at(world, 46, 122).reasons & (Wet | Flood), "Curved water and bank setback override soil suitability");
  bool face = false, apron = false, brink = false, wet = false;
  for (const auto &c : world.cells())
  {
    face |= bool(c.reasons & Cliff);
    apron |= bool(c.reasons & Talus);
    brink |= bool(c.reasons & Brink);
    wet |= bool(c.reasons & Wet);
  }
  check(face && apron && brink && wet, "Per-location reasons retain cliff talus brink and water exclusions");
  SettlementSettings sites;
  sites.townAreaMetres2 = 800;
  sites.villageAreaMetres2 = 320;
  SettlementCandidate candidate;
  check(evaluateSettlement(world, uint32_t(world.cellAt(90, 62)), true, sites, candidate, &error) && candidate.reasons, "Flat centroid enclosed by steep ring cannot reserve a town footprint");
  check(
      evaluateSettlement(world, uint32_t(world.cellAt(214, 162)), true, sites, candidate, &error) && candidate.reasons == 0 && candidate.core.size() * 16 >= 800,
      "Accessible cap has sufficient connected area and expansion space");
  SettlementProduct settlements;
  check(reserveSettlements(world, sites, settlements, &error), "Area-based town and village reservation pass completes");
  check(settlements.unfilledTowns == 0 && settlements.unfilledVillages == 0 && settlements.reservations.size() == 3, "Requested one town and two villages have usable footprints");
  std::set<uint32_t> used;
  bool disjoint = true, safe = true, connected = true, parented = true, crossCells = false;
  for (const auto &r : settlements.reservations)
  {
    std::set<uint32_t> core(r.core.begin(), r.core.end());
    std::set<uint32_t> seen;
    std::vector<uint32_t> stack{r.core.front()};
    seen.insert(stack[0]);
    while (!stack.empty())
    {
      auto id = stack.back();
      stack.pop_back();
      for (auto n : world.neighbors(id))
        if (core.count(n) && seen.insert(n).second)
          stack.push_back(n);
    }
    connected &= seen.size() == core.size();
    std::set<int> planning;
    for (auto id : r.core)
    {
      safe &= world.cells()[id].reasons == 0;
      disjoint &= used.insert(id).second;
      planning.insert(int(world.cells()[id].center.x / 20));
    }
    for (auto id : r.expansion)
    {
      safe &= world.cells()[id].reasons == 0;
      disjoint &= used.insert(id).second;
    }
    crossCells |= planning.size() > 1;
    if (!r.town)
    {
      bool found = false;
      for (auto &t : settlements.reservations)
        found |= t.town && t.id == r.parentTown && t.component == r.component;
      parented &= found;
    }
  }
  check(safe && disjoint, "Core and expansion reservations avoid all hazards and overlaps");
  check(connected, "Every settlement core is connected");
  check(parented, "Villages reference an accessible town in the same travel component");
  check(crossCells, "Footprints cross arbitrary planning-cell strips without inheriting their boundaries");
  WorldSuitability repeat;
  SettlementProduct again;
  check(
      repeat.build(physical, channels, soil, moisture, access, settings, 1, &error) && repeat.fingerprint() == world.fingerprint() && reserveSettlements(repeat, sites, again, &error) &&
          again.reservations[0].core == settlements.reservations[0].core,
      "Repeated final inputs produce stable suitability and reservations");
  auto moved = plateau;
  for (auto &p : moved.controls)
    p.x -= 20;
  WorldSplineLandforms movedForms;
  movedForms.build({moved, talus});
  WorldPhysicalTerrain movedPhysical;
  movedPhysical.build(movedForms, ground, {}, graph, {river});
  WorldSuitability movedWorld;
  check(movedWorld.build(movedPhysical, channels, soil, moisture, access, settings, 1, &error) && !settlements.matches(movedWorld), "Moving a landform invalidates suitability and dependent reservations");
  check(repeat.build(physical, channels, soil, moisture, access, settings, 2, &error) && !settlements.matches(repeat), "Changed dependency revision invalidates even equivalent sampled inputs");
  auto oldHash = repeat.fingerprint();
  auto invalid = [](double, double)
  {
    return std::numeric_limits<double>::quiet_NaN();
  };
  check(!repeat.build(physical, channels, invalid, moisture, access, settings, 1, &error) && repeat.fingerprint() == oldHash, "Invalid fields fail atomically without publishing partial suitability");
  auto bad = settings;
  bad.spacingMetres = .01;
  check(!repeat.build(physical, channels, soil, moisture, access, bad, 1), "Invalid resolution is rejected before allocation");
  check(!repeat.build(physical, channels, soil, moisture, {{46, 0, 122}}, settings, 1), "Water cannot be used as an access seed");
  auto over = sites;
  over.maximumCandidates = 1;
  check(!reserveSettlements(world, over, again, &error), "Candidate budget overflow is explicit");
  auto saturated = [](double, double)
  {
    return 1.;
  };
  WorldSuitability bog;
  check(bog.build(physical, channels, soil, saturated, access, settings, 1, &error) && (at(bog, 130, 220).reasons & Saturated), "Saturated ground overrides attractive soil");
  auto barren = [](double, double)
  {
    return 0.;
  };
  WorldSuitability poor;
  check(poor.build(physical, channels, barren, moisture, access, settings, 1, &error) && (at(poor, 130, 220).reasons & PoorSoil), "Poor soil is separately inspectable");
  auto emptySites = sites;
  emptySites.towns = 0;
  emptySites.villages = 1;
  SettlementProduct orphan;
  check(reserveSettlements(world, emptySites, orphan, &error) && orphan.unfilledVillages == 1 && orphan.reservations.empty(), "No town produces an explicit unfilled village request");
  WorldSplineLandforms noForms;
  noForms.build({});
  WorldPhysicalTerrain flatPhysical;
  WorldRiverChannels noChannels;
  auto flat = [](double, double)
  {
    return 0.;
  };
  flatPhysical.build(noForms, flat, {}, {}, {});
  SuitabilitySettings stripSettings;
  stripSettings.maxX = 40;
  stripSettings.maxZ = 120;
  auto stripSoil = [](double x, double)
  {
    return std::abs(x - 20) <= 4 ? .9 : 0.;
  };
  WorldSuitability strip;
  check(
      strip.build(flatPhysical, noChannels, stripSoil, moisture, {{10, 0, 10}}, stripSettings, 1, &error) && evaluateSettlement(strip, uint32_t(strip.cellAt(18, 62)), true, sites, candidate, &error) &&
          (candidate.reasons & Narrow),
      "Long thin suitable strips fail footprint compactness/aspect checks");
  check(at(world, 90, 62).slope < settings.maximumBuildGrade, "Rejected ring fixture really has a locally flat centroid");
  WorldSuitability ringWorld;
  auto ringAccess = access;
  ringAccess.push_back({90, 0, 62});
  check(
      ringWorld.build(physical, channels, soil, moisture, ringAccess, settings, 1, &error) && evaluateSettlement(ringWorld, uint32_t(ringWorld.cellAt(90, 62)), true, sites, candidate, &error) &&
          (candidate.reasons & InsufficientArea),
      "Even an accessible flat centroid records insufficient connected area inside the steep ring");
  bool accounted = true;
  size_t acceptedCandidates = 0;
  for (const auto &c : settlements.candidates)
  {
    if (!c.reasons)
      ++acceptedCandidates;
    accounted &= c.reasons || std::any_of(
                                  settlements.reservations.begin(),
                                  settlements.reservations.end(),
                                  [&](const auto &r)
                                  {
                                    return r.town == c.town && world.cellAt(r.center.x, r.center.z) == c.seed;
                                  });
  }
  check(accounted && acceptedCandidates == settlements.reservations.size(), "Every unselected candidate has an explicit rejection or quota reason");
  WorldSuitability empty;
  check(empty.cellAt(10, 10) == SIZE_MAX && !SettlementProduct{}.matches(empty), "Empty products cannot masquerade as valid dependency snapshots");
  bool separated = true;
  for (size_t i = 0; i < settlements.reservations.size(); ++i)
    for (size_t j = i + 1; j < settlements.reservations.size(); ++j)
      for (auto a : settlements.reservations[i].core)
        for (auto b : settlements.reservations[j].core)
        {
          auto p = world.cells()[a].center, q = world.cells()[b].center;
          double dx = std::max(0., std::abs(p.x - q.x) - settings.spacingMetres), dz = std::max(0., std::abs(p.z - q.z) - settings.spacingMetres);
          separated &= std::hypot(dx, dz) >= sites.minimumSettlementGapMetres;
        }
  check(separated, "Distinct settlements retain the requested ground gap between their footprints");
  WorldSuitability lakeWorld;
  auto lake = [](double x, double z)
  {
    return std::hypot(x - 130, z - 222) < 12;
  };
  check(lakeWorld.build(physical, channels, soil, moisture, access, settings, 1, &error, lake) && (at(lakeWorld, 130, 222).reasons & Wet), "Additional ocean or lake masks override otherwise usable ground");
  json map = json::array();
  for (auto c : world.cells())
    map.push_back(
        {{"position", {c.center.x, c.center.y, c.center.z}},
         {"reasons", c.reasons},
         {"score", c.score},
         {"slope", c.slope},
         {"relief", c.relief},
         {"soil", c.soil},
         {"moisture", c.moisture},
         {"waterDistance", c.waterDistance},
         {"walkable", c.walkable},
         {"component", c.component}});
  json blockedReasons = json::array();
  for (auto c : blocked.cells())
    blockedReasons.push_back(c.reasons);
  json towns = json::array();
  for (auto &r : settlements.reservations)
    towns.push_back({{"id", r.id}, {"parent", r.parentTown}, {"town", r.town}, {"center", {r.center.x, r.center.y, r.center.z}}, {"area", r.areaMetres2}, {"core", r.core}, {"expansion", r.expansion}});
  json candidates = json::array();
  for (auto &c : settlements.candidates)
    candidates.push_back({{"seed", c.seed}, {"reasons", c.reasons}, {"town", c.town}, {"score", c.score}});
  json report = {
    {"status", failures ? "FAIL" : "PASS"},
    {"assertions", checks},
    {"phase", 8},
    {"columns", world.columns()},
    {"rows", world.rows()},
    {"spacing", settings.spacingMetres},
    {"cells", map},
    {"blockedReasons", blockedReasons},
    {"settlements", towns},
    {"candidates", candidates},
    {"fingerprint", world.fingerprint()},
    {"unfilledTowns", settlements.unfilledTowns},
    {"unfilledVillages", settlements.unfilledVillages}};
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    file << report.dump(2) << '\n';
  }
  std::cout << "METRIC cells " << world.cells().size() << " reservations " << settlements.reservations.size() << " candidates " << settlements.candidates.size() << '\n';
  return failures ? 1 : 0;
}
