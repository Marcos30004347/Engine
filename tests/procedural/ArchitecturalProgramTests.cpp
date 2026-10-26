// The architectural programme, and the network it is sampled from.
//
// Stage B0 of .plan/bayesian-architectural-program.md: a hand-authored
// Bayesian network over the rooms of a 19th-century English house, and the
// sampler that turns a brief into a programme. The paper is Merrell, Schkufza
// and Koltun, "Computer-Generated Residential Building Layouts", SIGGRAPH Asia
// 2010, §4.
//
// What is actually being checked, in the plan's own words:
//
//   "At convergence: no house without a kitchen, no kitchen-bathroom-bedroom
//    path, and the kitchen-scullery and landing-staircase edges present."
//
// Those are the gates. The rest is the machinery underneath them - that
// variable elimination agrees with brute force, that a clamped brief is
// actually honoured, that a seed reproduces a house - because a network that
// samples plausible houses from a broken inference engine is a network that
// will stop doing so the first time anyone clamps anything.
#include "procedural/learn/BayesianNetwork.hpp"
#include "procedural/world/generator/ArchitecturalProgram.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::learn;
using namespace procedural::world::generator;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

// --- the engine ----------------------------------------------------------

// A three-node chain with a known answer, worked out by hand, so that a
// failure here is a failure of variable elimination and not of the house.
//
//   Rain -> WetGrass <- Sprinkler
//
// The interesting query is the explaining-away one: seeing wet grass raises
// the chance of rain, and then seeing the sprinkler on lowers it again. Get
// that wrong and a brief clamping "six bedrooms" will not pull the storey
// count up with it, which is the entire reason this is not forward sampling.
void testInference()
{
  std::cout << "inference\n";

  BayesianNetwork net;

  const uint32_t rain = net.addBoolean("Rain");
  const uint32_t sprinkler = net.addBoolean("Sprinkler");
  const uint32_t wet = net.addBoolean("WetGrass");

  net.setPrior(rain, {.8, .2});
  net.setPrior(sprinkler, {.9, .1});
  net.setParents(wet, {rain, sprinkler});
  net.setDistribution(wet, {
    /* dry, off */ {1.0, 0.0},
    /* dry, on  */ {0.1, 0.9},
    /* wet, off */ {0.2, 0.8},
    /* wet, on  */ {0.01, 0.99},
  });

  std::string why;
  check(net.validate(why), "the chain validates: " + why);

  Evidence none;
  none.resize(net.variableCount());

  const std::vector<double> prior = net.marginal(rain, none);
  check(std::abs(prior[1] - .2) < 1e-9, "an unobserved marginal is the prior");

  // p(wet) = .8*.9*0 + .8*.1*.9 + .2*.9*.8 + .2*.1*.99 = .072+.144+.0198
  const std::vector<double> wetMarginal = net.marginal(wet, none);
  check(std::abs(wetMarginal[1] - .2358) < 1e-9, "a child's marginal is the sum over its parents");

  Evidence sawWet = none;
  sawWet.set(wet, 1);

  // p(rain | wet) = .2*(.9*.8 + .1*.99) / .2358 = .1638/.2358
  const std::vector<double> posterior = net.marginal(rain, sawWet);
  check(std::abs(posterior[1] - .1638 / .2358) < 1e-9, "evidence on a child updates the parent");
  check(posterior[1] > prior[1], "wet grass makes rain more likely");

  Evidence sawBoth = sawWet;
  sawBoth.set(sprinkler, 1);

  const std::vector<double> explained = net.marginal(rain, sawBoth);
  check(explained[1] < posterior[1], "the sprinkler explains the wet grass away");

  // Sampling has to agree with inference, or the fast path is wrong. Only the
  // clamped variable is observed, and it is a *descendant* of Rain, so this
  // exercises the elimination path rather than the CPT read.
  uint32_t rained = 0;
  const uint32_t draws = 40000;

  for (uint32_t i = 0; i < draws; ++i)
  {
    Evidence out;

    if (net.sample(sawWet, 0x1234ull + i, out))
    {
      rained += out.at(rain) == 1 ? 1 : 0;
    }
  }

  const double measured = double(rained) / double(draws);
  check(std::abs(measured - posterior[1]) < .01,
        "conditional sampling matches the posterior (" + std::to_string(measured) + ")");

  // A brief that cannot happen is reported, not quietly relaxed.
  BayesianNetwork impossible;
  const uint32_t a = impossible.addBoolean("A");
  const uint32_t b = impossible.addBoolean("B");
  impossible.setPrior(a, {.5, .5});
  impossible.setParents(b, {a});
  impossible.setDistribution(b, {{1., 0.}, {1., 0.}});

  Evidence cannot;
  cannot.resize(impossible.variableCount());
  cannot.set(b, 1);

  Evidence out;
  check(!impossible.sample(cannot, 7, out), "an impossible brief fails rather than sampling");
}

// --- the network ---------------------------------------------------------

void testNetworkShape()
{
  std::cout << "\nthe programme network\n";

  const BayesianNetwork &net = priorProgrammeNetwork();

  std::string why;
  check(net.validate(why), "the programme network validates: " + why);

  uint32_t edges = 0;

  for (uint32_t v = 0; v < net.variableCount(); ++v)
  {
    edges += uint32_t(net.parentsOf(v).size());
  }

  std::printf("  %u nodes, %u edges over %zu room uses and %zu adjacency pairs\n",
              net.variableCount(), edges, size_t(RoomUse::Count),
              programmeAdjacencyPairs().size());

  // The paper's three networks run 42/82/120 nodes and 33/61/92 edges. Ours
  // is one network covering all three storey counts and a larger vocabulary -
  // scullery, larder, dairy and washhouse have no equivalent in an American
  // suburban catalogue - so it is bigger, as the plan says to expect.
  check(net.variableCount() > 100, "the network is in the paper's size range or above");
  check(!net.topologicalOrder().empty(), "the network is acyclic");

  // The privacy gradient, stated structurally: these pairs must not exist, so
  // no sampled programme can ever ask for the door.
  const auto &pairs = programmeAdjacencyPairs();
  auto joined = [&pairs](const RoomUse a, const RoomUse b) {
    for (const auto &p : pairs)
    {
      if ((p.first == a && p.second == b) || (p.first == b && p.second == a))
      {
        return true;
      }
    }

    return false;
  };

  check(!joined(RoomUse::Kitchen, RoomUse::Bathroom), "no kitchen-bathroom pair exists");
  check(!joined(RoomUse::Scullery, RoomUse::Bedroom), "no scullery-bedroom pair exists");
  check(!joined(RoomUse::Scullery, RoomUse::DrawingRoom), "no scullery-drawing room pair exists");

  // A kitchen-bedroom door does exist in the table, because in a one-storey
  // cottage with no hall the chamber opens off the living room and there is
  // nowhere else for it to open. It is closed by the hall and the landing, so
  // no house that has either can draw it - which is checked where it can be
  // seen, on the sampled houses, rather than here.
  check(joined(RoomUse::Kitchen, RoomUse::Bedroom),
        "the kitchen-bedroom pair exists, conditionally");
  check(joined(RoomUse::Kitchen, RoomUse::Scullery), "the kitchen-scullery pair exists");
  check(joined(RoomUse::Landing, RoomUse::Staircase), "the landing-staircase pair exists");
}

// --- sampling ------------------------------------------------------------

struct Census
{
  uint32_t houses = 0;
  std::map<std::string, uint32_t> rooms;   // plates a room appears on
  std::map<std::string, uint32_t> pairs;   // plates a link appears on
  double meanRooms = 0, meanArea = 0, meanLinks = 0;
  uint32_t noKitchen = 0, stranded = 0, repairs = 0, totalRooms = 0;
  std::map<std::string, uint32_t> repairedUses;
};

// Rooms of the programme that cannot be reached from the entrance by its own
// adjacencies. The yard is excluded: it is outside, and a plate that draws it
// often does not draw the door.
uint32_t unreachableRooms(const Programme &programme)
{
  if (programme.rooms.empty())
  {
    return 0;
  }

  // Start at the front door: the porch, or the hall, or whatever is first.
  uint32_t start = programme.find(RoomUse::Porch);

  if (start == Programme::npos)
  {
    start = programme.find(RoomUse::Hall);
  }

  if (start == Programme::npos)
  {
    start = programme.find(RoomUse::Kitchen);
  }

  if (start == Programme::npos)
  {
    start = 0;
  }

  std::vector<std::vector<uint32_t>> neighbours(programme.rooms.size());

  for (const Adjacency &link : programme.links)
  {
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
  }

  std::vector<bool> seen(programme.rooms.size(), false);
  std::vector<uint32_t> stack = {start};
  seen[start] = true;

  while (!stack.empty())
  {
    const uint32_t r = stack.back();
    stack.pop_back();

    for (const uint32_t n : neighbours[r])
    {
      if (!seen[n])
      {
        seen[n] = true;
        stack.push_back(n);
      }
    }
  }

  uint32_t lost = 0;

  for (size_t i = 0; i < seen.size(); ++i)
  {
    lost += !seen[i] && programme.rooms[i].use != RoomUse::Yard ? 1 : 0;
  }

  return lost;
}

// The paper's own example of what independent per-pair probabilities produce:
//
//   "the frequent occurrence in the data of a bedroom-bathroom adjacency and a
//    kitchen-bathroom adjacency could lead to architectural programs being
//    generated with kitchen-bathroom-bedroom paths, which have very low
//    likelihood."
//
// Here the sanitary room is the water closet. A kitchen that opens into it and
// a bedroom that opens into the same one is the exact failure, and no house
// may have it.
bool kitchenReachesBedThroughSanitary(const Programme &programme)
{
  std::vector<std::vector<uint32_t>> neighbours(programme.rooms.size());

  for (const Adjacency &link : programme.links)
  {
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
  }

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    const RoomUse use = programme.rooms[r].use;

    if (use != RoomUse::WaterCloset && use != RoomUse::Bathroom)
    {
      continue;
    }

    bool kitchen = false, bedroom = false;

    for (const uint32_t n : neighbours[r])
    {
      kitchen = kitchen || programme.rooms[n].use == RoomUse::Kitchen
                || programme.rooms[n].use == RoomUse::Scullery;
      bedroom = bedroom || programme.rooms[n].use == RoomUse::Bedroom;
    }

    if (kitchen && bedroom)
    {
      return true;
    }
  }

  return false;
}

// Bedrooms reached from the kitchen without passing through any circulation
// room. In a two-room cottage that is simply the truth - you cross the kitchen
// to go to bed - so this is only a defect in a house that *has* a hall or a
// landing to route through.
bool kitchenReachesBedPrivately(const Programme &programme)
{
  auto isCirculation = [](const RoomUse u) {
    return u == RoomUse::Hall || u == RoomUse::Passage || u == RoomUse::Landing
           || u == RoomUse::Staircase || u == RoomUse::Lobby || u == RoomUse::Porch;
  };

  if (!programme.has(RoomUse::Hall) && !programme.has(RoomUse::Landing))
  {
    return false;
  }

  const uint32_t kitchen = programme.find(RoomUse::Kitchen);

  if (kitchen == Programme::npos)
  {
    return false;
  }

  std::vector<std::vector<uint32_t>> neighbours(programme.rooms.size());

  for (const Adjacency &link : programme.links)
  {
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
  }

  std::vector<bool> seen(programme.rooms.size(), false);
  std::vector<uint32_t> stack = {kitchen};
  seen[kitchen] = true;

  while (!stack.empty())
  {
    const uint32_t r = stack.back();
    stack.pop_back();

    for (const uint32_t n : neighbours[r])
    {
      if (seen[n] || isCirculation(programme.rooms[n].use))
      {
        continue;
      }

      if (programme.rooms[n].use == RoomUse::Bedroom)
      {
        return true;
      }

      seen[n] = true;
      stack.push_back(n);
    }
  }

  return false;
}

Census survey(const PlotStanding standing, const uint32_t houses)
{
  Census census;
  uint32_t privatePaths = 0, sanitaryPaths = 0;

  for (uint32_t i = 0; i < houses; ++i)
  {
    ProgrammeBrief brief;
    brief.standing = standing;

    Programme programme;

    if (!sampleProgramme(brief, 0xBEEF0000ull + i, programme))
    {
      continue;
    }

    ++census.houses;
    census.meanRooms += double(programme.rooms.size());
    census.meanLinks += double(programme.links.size());
    census.meanArea += programme.totalAreaMetres;
    // Somewhere to cook, which is not the same as a kitchen: the book's
    // cottages have a living room with the fire in it and a scullery behind,
    // and no room labelled kitchen at all.
    census.noKitchen += programme.has(RoomUse::Kitchen)
                            || programme.has(RoomUse::Scullery)
                            || programme.has(RoomUse::LivingRoom)
                          ? 0 : 1;
    census.stranded += unreachableRooms(programme);
    census.repairs += programme.repairedLinks;

    // The repair appends, so the stranded rooms are the far end of the last
    // `repairedLinks` entries. Which rooms they are is the useful half: a
    // room that keeps needing a door is a room with no partner in the table.
    for (uint32_t k = 0; k < programme.repairedLinks; ++k)
    {
      const Adjacency &added = programme.links[programme.links.size() - 1 - k];
      ++census.repairedUses[name(programme.rooms[added.b].use)];
    }

    census.totalRooms += uint32_t(programme.rooms.size());
    privatePaths += kitchenReachesBedPrivately(programme) ? 1 : 0;
    sanitaryPaths += kitchenReachesBedThroughSanitary(programme) ? 1 : 0;

    std::set<std::string> seenRooms;

    for (const RoomSpec &room : programme.rooms)
    {
      seenRooms.insert(name(room.use));
    }

    for (const std::string &r : seenRooms)
    {
      ++census.rooms[r];
    }

    std::set<std::string> seenPairs;

    for (const Adjacency &link : programme.links)
    {
      std::string a = name(programme.rooms[link.a].use);
      std::string b = name(programme.rooms[link.b].use);

      if (b < a)
      {
        std::swap(a, b);
      }

      seenPairs.insert(a + " + " + b);
    }

    for (const std::string &pair : seenPairs)
    {
      ++census.pairs[pair];
    }
  }

  if (census.houses > 0)
  {
    census.meanRooms /= double(census.houses);
    census.meanLinks /= double(census.houses);
    census.meanArea /= double(census.houses);
  }

  check(sanitaryPaths == 0,
        "no kitchen-water closet-bedroom path ("
          + std::to_string(sanitaryPaths) + " of " + std::to_string(census.houses) + ")");
  check(privatePaths * 20 < census.houses,
        "a house with circulation routes its bedrooms through it ("
          + std::to_string(privatePaths) + " of " + std::to_string(census.houses) + ")");

  return census;
}

void report(const char *label, const Census &census)
{
  std::printf("\n  %s: %u houses, %.1f rooms, %.1f links, %.0f m2 mean\n",
              label, census.houses, census.meanRooms, census.meanLinks, census.meanArea);

  std::vector<std::pair<uint32_t, std::string>> byCount;

  for (const auto &entry : census.rooms)
  {
    byCount.emplace_back(entry.second, entry.first);
  }

  std::sort(byCount.rbegin(), byCount.rend());

  std::printf("    rooms:");

  for (size_t i = 0; i < byCount.size() && i < 12; ++i)
  {
    std::printf(" %s %.0f%%", byCount[i].second.c_str(),
                100. * double(byCount[i].first) / double(std::max(census.houses, 1u)));
  }

  std::printf("\n");

  std::vector<std::pair<uint32_t, std::string>> pairsByCount;

  for (const auto &entry : census.pairs)
  {
    pairsByCount.emplace_back(entry.second, entry.first);
  }

  std::sort(pairsByCount.rbegin(), pairsByCount.rend());

  std::vector<std::pair<uint32_t, std::string>> repaired;

  for (const auto &entry : census.repairedUses)
  {
    repaired.emplace_back(entry.second, entry.first);
  }

  std::sort(repaired.rbegin(), repaired.rend());

  std::printf("    stranded:");

  for (size_t i = 0; i < repaired.size() && i < 8; ++i)
  {
    std::printf(" %s %.2f/house", repaired[i].second.c_str(),
                double(repaired[i].first) / double(std::max(census.houses, 1u)));
  }

  std::printf("\n    links:");

  for (size_t i = 0; i < pairsByCount.size() && i < 8; ++i)
  {
    std::printf(" [%s %.0f%%]", pairsByCount[i].second.c_str(),
                100. * double(pairsByCount[i].first) / double(std::max(census.houses, 1u)));
  }

  std::printf("\n");
}

void testSampling()
{
  std::cout << "\nsampling\n";

  const uint32_t houses = 400;

  const Census modest = survey(PlotStanding::Modest, houses);
  const Census grand = survey(PlotStanding::Grand, houses);
  const Census rural = survey(PlotStanding::Rural, houses);

  check(modest.houses == houses && grand.houses == houses && rural.houses == houses,
        "every unclamped brief samples");

  // The plan's gate, as the corpus corrected it: a house must have somewhere
  // to cook. Requiring a *kitchen* would fail the book's own cottages.
  check(modest.noKitchen == 0 && grand.noKitchen == 0 && rural.noKitchen == 0,
        "no house without somewhere to cook");

  auto share = [](const Census &c, const char *room) {
    const auto it = c.rooms.find(room);

    return it == c.rooms.end() ? 0. : double(it->second) / double(std::max(c.houses, 1u));
  };

  auto pairShare = [](const Census &c, const char *pair) {
    const auto it = c.pairs.find(pair);

    return it == c.pairs.end() ? 0. : double(it->second) / double(std::max(c.houses, 1u));
  };

  check(pairShare(grand, "kitchen + scullery") > .8,
        "the kitchen-scullery edge is present in a grand house");
  check(pairShare(grand, "landing + staircase") > .7,
        "the landing-staircase edge is present in a grand house");

  // The standings have to be different kinds of house, not the same house at
  // different scales. These are what separates them.
  check(share(grand, "drawing room") > .8 && share(modest, "drawing room") < .1,
        "a drawing room belongs to a grand house and not a modest one");
  check(share(rural, "dairy") > share(grand, "dairy"),
        "the dairy is a rural room");
  check(share(grand, "scullery") > share(modest, "scullery"),
        "service rooms multiply with standing");
  check(grand.meanArea > 2.5 * modest.meanArea, "a grand house is much bigger than a modest one");

  // A house whose rooms do not join up is a list, not a plan.
  check(modest.stranded == 0 && grand.stranded == 0 && rural.stranded == 0,
        "every room of every programme is reachable from the entrance");

  // How much of that connectivity the network gave and how much the decoder
  // had to put back. Each adjacency is drawn independently given the rooms, so
  // a thirty-room house will strand one now and then whatever the priors are -
  // but this is the number that says whether the priors are carrying the plan
  // or the repair is. It belongs in the output where a change to the table
  // will move it.
  auto repairRate = [](const Census &c) {
    return double(c.repairs) / double(std::max(c.totalRooms, 1u));
  };

  std::printf("  doors the decoder had to add, per room: modest %.3f, grand %.3f, rural %.3f\n",
              repairRate(modest), repairRate(grand), repairRate(rural));
  // A fence, not a target. Every door in this network is drawn on its own
  // given the rooms, so a house whose scullery has three possible partners
  // will now and then draw none of them, and the only way to drive this to
  // zero is to push every probability to one - which would cost the variety
  // the network exists for. What it catches is a room added to the vocabulary
  // with no partner in the pair table, which strands it every single time.
  // Stage B1 should bring these down; it learns the structure instead.
  //
  // Where it stood when the fence was set: modest .106, grand .057, rural .120.
  check(repairRate(grand) < .14 && repairRate(modest) < .14 && repairRate(rural) < .14,
        "the network's own adjacencies carry the plan, not the repair");

  report("modest", modest);
  report("grand", grand);
  report("rural", rural);
}

void testBrief()
{
  std::cout << "\nthe brief\n";

  ProgrammeBrief brief;
  brief.standing = PlotStanding::Wealthy;

  Programme a, b;
  check(sampleProgramme(brief, 99, a) && sampleProgramme(brief, 99, b),
        "a brief samples");
  check(a.rooms.size() == b.rooms.size() && a.storeys == b.storeys
          && std::abs(a.totalAreaMetres - b.totalAreaMetres) < 1e-12,
        "one seed gives one house");

  Programme other;
  check(sampleProgramme(brief, 100, other), "another seed samples");
  check(other.rooms.size() != a.rooms.size() || other.storeys != a.storeys
          || std::abs(other.totalAreaMetres - a.totalAreaMetres) > 1e-12,
        "another seed gives another house");

  // Clamps.
  bool storeysHeld = true, bedroomsHeld = true;

  for (uint32_t i = 0; i < 100; ++i)
  {
    ProgrammeBrief clamped;
    clamped.standing = PlotStanding::Comfortable;
    clamped.storeys = 3;
    clamped.bedrooms = 5;

    Programme programme;

    if (!sampleProgramme(clamped, 500 + i, programme))
    {
      storeysHeld = false;

      continue;
    }

    storeysHeld = storeysHeld && programme.storeys == 3;
    bedroomsHeld = bedroomsHeld && programme.countOf(RoomUse::Bedroom) == 5;
  }

  check(storeysHeld, "a clamped storey count is honoured");
  check(bedroomsHeld, "a clamped bedroom count is honoured");

  // Clamping a descendant has to pull its ancestors with it. Asking for six
  // bedrooms of a modest plot should raise the storey count, because that is
  // the only way a modest house holds six bedrooms - and it is exactly what
  // forward sampling cannot do.
  double storeysWith = 0, storeysWithout = 0;

  for (uint32_t i = 0; i < 300; ++i)
  {
    ProgrammeBrief plain;
    plain.standing = PlotStanding::Modest;

    ProgrammeBrief many = plain;
    many.bedrooms = 6;

    Programme p1, p2;

    if (sampleProgramme(plain, 900 + i, p1))
    {
      storeysWithout += double(p1.storeys);
    }

    if (sampleProgramme(many, 900 + i, p2))
    {
      storeysWith += double(p2.storeys);
    }
  }

  std::printf("  mean storeys: %.2f unclamped, %.2f asking for six bedrooms\n",
              storeysWithout / 300., storeysWith / 300.);
  check(storeysWith > storeysWithout + 30.,
        "asking for six bedrooms makes the house taller");

  // The lot clamps the footprint, and the programme fits it.
  ProgrammeBrief tight;
  tight.standing = PlotStanding::Wealthy;
  tight.footprintX = 11.;
  tight.footprintZ = 7.;

  uint32_t fitted = 0;
  double meanTotal = 0;

  for (uint32_t i = 0; i < 100; ++i)
  {
    Programme programme;

    if (!sampleProgramme(tight, 1300 + i, programme))
    {
      continue;
    }

    ++fitted;
    meanTotal += programme.totalAreaMetres;

    if (std::abs(programme.footprintX - 11.) > 1e-9
        || std::abs(programme.footprintZ - 7.) > 1e-9)
    {
      fitted = 0;

      break;
    }
  }

  check(fitted == 100, "a clamped footprint comes back unchanged");
  std::printf("  a 11x7 m wealthy plot programmes %.0f m2 over its storeys\n",
              meanTotal / std::max(1u, fitted));

  // An impossible brief is refused rather than relaxed.
  ProgrammeBrief silly;
  silly.standing = PlotStanding::Slum;
  silly.storeys = 7;

  Programme nothing;
  check(!sampleProgramme(silly, 1, nothing), "an out-of-range brief is refused");
}

void testAreas()
{
  std::cout << "\nareas\n";

  ProgrammeBrief brief;
  brief.standing = PlotStanding::Comfortable;

  Programme programme;
  check(sampleProgramme(brief, 4242, programme), "a comfortable house samples");

  bool sane = true, likelihoodSane = true;

  for (uint32_t i = 0; i < programme.rooms.size(); ++i)
  {
    const RoomSpec &room = programme.rooms[i];

    sane = sane && room.areaMetres > .5 && room.areaMetres < 400. && room.aspect >= 1.;

    // The area it was given must be more likely than an absurd one. This is
    // the distribution §5.2's cost function scores a candidate plan with, so
    // if it is flat the optimiser has nothing to optimise.
    const double atSampled = areaLogLikelihood(programme, i, room.areaMetres);
    const double atAbsurd = areaLogLikelihood(programme, i, 500.);

    likelihoodSane = likelihoodSane && std::isfinite(atSampled) && atSampled > atAbsurd;
  }

  check(sane, "every room has a believable area and aspect");
  check(likelihoodSane, "the sampled area beats an absurd one on likelihood");

  // A kitchen is bigger than a closet and a drawing room is bigger than both.
  double kitchen = 0, closet = 0, drawing = 0;
  uint32_t kitchens = 0, closets = 0, drawings = 0;

  for (uint32_t i = 0; i < 300; ++i)
  {
    ProgrammeBrief rich;
    rich.standing = PlotStanding::Grand;

    Programme house;

    if (!sampleProgramme(rich, 7000 + i, house))
    {
      continue;
    }

    for (const RoomSpec &room : house.rooms)
    {
      if (room.use == RoomUse::Kitchen) { kitchen += room.areaMetres; ++kitchens; }
      if (room.use == RoomUse::Closet) { closet += room.areaMetres; ++closets; }
      if (room.use == RoomUse::DrawingRoom) { drawing += room.areaMetres; ++drawings; }
    }
  }

  kitchen /= std::max(1u, kitchens);
  closet /= std::max(1u, closets);
  drawing /= std::max(1u, drawings);

  std::printf("  grand house means: kitchen %.1f m2, drawing room %.1f m2, closet %.1f m2\n",
              kitchen, drawing, closet);
  check(closet < kitchen && kitchen < drawing, "the rooms come out in the right order of size");
}

// --- the bubble diagram --------------------------------------------------

// The plan's acceptance figure, in text: the sampled programme as a bubble
// diagram, so a person can read whether it is a house.
void printProgramme(const char *label, const Programme &programme)
{
  std::printf("\n  %s - %u storey%s%s, %.0f m2, footprint %.1f x %.1f m\n",
              label, programme.storeys, programme.storeys == 1 ? "" : "s",
              programme.basement ? " over a basement" : "",
              programme.totalAreaMetres, programme.footprintX, programme.footprintZ);

  for (int32_t floor = -1; floor <= int32_t(programme.storeys) - 1; ++floor)
  {
    std::vector<const RoomSpec *> here;

    for (const RoomSpec &room : programme.rooms)
    {
      if (room.floor == floor)
      {
        here.push_back(&room);
      }
    }

    if (here.empty())
    {
      continue;
    }

    std::printf("    %-8s",
                floor < 0 ? "basement" : floor == 0 ? "ground" : ("floor " + std::to_string(floor)).c_str());

    for (const RoomSpec *room : here)
    {
      std::printf(" %s(%.0f)", name(room->use), room->areaMetres);
    }

    std::printf("\n");
  }

  std::printf("    doors  ");

  for (const Adjacency &link : programme.links)
  {
    std::printf(" %s%s%s", name(programme.rooms[link.a].use), link.door ? "-" : "=",
                name(programme.rooms[link.b].use));
  }

  std::printf("\n           (- is a door, = an open wall)\n");
}

void testGallery()
{
  std::cout << "\na programme per standing\n";

  const std::pair<PlotStanding, const char *> standings[] = {
    {PlotStanding::Slum, "slum"},
    {PlotStanding::Modest, "modest"},
    {PlotStanding::Comfortable, "comfortable"},
    {PlotStanding::Wealthy, "wealthy"},
    {PlotStanding::Grand, "grand"},
    {PlotStanding::Rural, "rural"},
  };

  for (const auto &entry : standings)
  {
    ProgrammeBrief brief;
    brief.standing = entry.first;

    Programme programme;

    if (sampleProgramme(brief, 20260915ull, programme))
    {
      printProgramme(entry.second, programme);
    }
  }
}

}  // namespace

int main()
{
  // This file tests the hand-authored network - stage B0 - and nothing else.
  // The generator picks up the trained network from its asset when there is
  // one, which would quietly change every number below, so pin the prior.
  // What the trained network does is ProgrammeFitTests' business.
  useProgrammeNetworkPrior();

  testInference();
  testNetworkShape();
  testSampling();
  testBrief();
  testAreas();
  testGallery();

  std::cout << "\n" << (failures == 0 ? "all passed" : std::to_string(failures) + " failed")
            << "\n";

  return failures == 0 ? 0 : 1;
}
