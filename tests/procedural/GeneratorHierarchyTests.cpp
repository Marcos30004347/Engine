// The generator hierarchy: three interfaces, a registry, a policy and a
// director that fans out over regions as they appear.
//
// What each test is actually asserting:
//
//  - a parent's output is its children's input, and nothing else passes
//    between them;
//  - swapping a generator is a binding, not a boolean;
//  - the choice can be made per region, so one ward is gridded while the ward
//    beside it is not;
//  - a widthed edge yields a road region, not a hole;
//  - concave regions run through the whole chain, because that is what a road
//    network's faces are.

#include "procedural/world/hierarchy/Generators.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

using namespace procedural;
using namespace procedural::world::hierarchy;

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

Point at(const double x, const double y)
{
  return {geom2::toUnit(x), geom2::toUnit(y)};
}

Polygon boxPolygon(const double x0, const double y0, const double x1, const double y1)
{
  Polygon p;
  p.outer = {at(x0, y0), at(x1, y0), at(x1, y1), at(x0, y1)};

  return p;
}

// A concave site, because that is what a settlement region actually is.
Polygon ellPolygon()
{
  Polygon p;
  p.outer = {at(0, 0), at(120, 0), at(120, 60), at(60, 60), at(60, 120), at(0, 120)};

  return p;
}

RegionId seed(RegionTree &tree, Polygon boundary)
{
  Region root = makeRegion(tree.nextRegionId(), kNoRegion, std::move(boundary));
  root.cover = Cover::Urban;
  root.producer = "test";

  return tree.add(std::move(root));
}

Registry standardRegistry()
{
  Registry registry;
  std::string error;

  registry.bind(std::make_shared<GridStreets>(GridStreets::Settings{40, 3, 0}), &error);
  registry.bind(std::make_shared<RadialStreets>(RadialStreets::Settings{6, .55, 3}), &error);
  registry.bind(std::make_shared<StripLots>(StripLots::Settings{200, .25}), &error);
  registry.bind(std::make_shared<RingLots>(RingLots::Settings{12, 120}), &error);
  registry.bind(std::make_shared<HouseFootprints>(), &error);
  registry.bind(std::make_shared<SlopeField>(), &error);

  return registry;
}

double leafArea(const RegionTree &tree, const std::vector<RegionId> &ids)
{
  double area = 0;

  for (const RegionId id : ids)
  {
    const Region *region = tree.find(id);

    if (region != nullptr)
    {
      area += region->areaMetres();
    }
  }

  return area;
}

void registryTests()
{
  std::cout << "Registry and policy\n";

  Registry registry = standardRegistry();

  check(registry.find("streets.grid") != nullptr && registry.find("streets.radial") != nullptr, "two generators bind to the same role");
  check(registry.boundTo(Role::Streets).size() == 2, "and the role lists both");
  check(registry.boundTo(Role::Lots).size() == 2, "as does the lot role");

  std::string error;
  check(!registry.bind(std::make_shared<GridStreets>(), &error), "a duplicate id is refused");
  check(!registry.bind(nullptr, &error), "and so is a null generator");

  Region region = makeRegion(1, kNoRegion, boxPolygon(0, 0, 100, 100));

  Policy policy;
  policy.defaults[Role::Streets] = "streets.grid";

  std::string because;
  check(registry.choose(Role::Streets, region, policy, &because) != nullptr && because == "default", "the default binding is chosen when no rule matches");

  // Selection per region, which is the whole point: a wealthy ward can take a
  // different generator from the quarter beside it without a new flag.
  policy.rules.push_back(
      {Role::Streets,
       "streets.radial",
       [](const Region &r) {
         return r.attributes.demand > 500;
       },
       "high demand ward"});

  check(registry.choose(Role::Streets, region, policy, &because)->descriptor().id == "streets.grid", "a rule that does not match leaves the default");

  region.attributes.demand = 900;

  check(registry.choose(Role::Streets, region, policy, &because)->descriptor().id == "streets.radial", "a rule that matches claims the region");
  check(because == "high demand ward", "and records why, so a render can be explained");

  Policy empty;
  check(registry.choose(Role::Interiors, region, empty, &because) == nullptr, "an unbound role chooses nothing rather than silently doing nothing");
}

void hierarchyTests()
{
  std::cout << "Parent to child\n";

  Registry registry = standardRegistry();
  Context context;
  context.worldSeed = 17;

  RegionTree tree;
  const RegionId root = seed(tree, boxPolygon(0, 0, 120, 120));
  const double rootArea = tree.find(root)->areaMetres();

  Plan plan;
  plan.policy.defaults[Role::Streets] = "streets.grid";
  plan.policy.defaults[Role::Lots] = "lots.strip";
  plan.steps.push_back({Role::Streets, {}, 0});
  plan.steps.push_back(
      {Role::Lots,
       [](const Region &r) {
         return r.cover != Cover::Road;
       },
       0});

  Composer composer(registry, context);
  Ledger ledger;
  RunStats stats;
  std::string error;

  check(composer.run(plan, tree, root, ledger, stats, &error), "the plan runs: " + error);
  check(stats.stepsRun == 2, "both steps ran");
  check(stats.edgesProposed > 0, "streets were proposed");

  // Every region produced names its parent and its producer.
  bool parented = true;
  bool attributed = true;

  for (const auto &region : tree.regions())
  {
    if (region.id == root)
    {
      continue;
    }

    if (tree.find(region.parent) == nullptr)
    {
      parented = false;
    }

    if (region.producer.empty() || region.sources.empty())
    {
      attributed = false;
    }
  }

  check(parented, "every region has a parent in the tree");
  check(attributed, "and names the generator that produced it");

  check(!tree.childrenOf(root).empty(), "the root has children");

  // A widthed edge becomes a road region, not a hole in the district.
  size_t roads = 0;
  double roadArea = 0;

  for (const auto &region : tree.regions())
  {
    if (region.cover == Cover::Road)
    {
      ++roads;
      roadArea += region.areaMetres();
    }
  }

  check(roads > 0, "the streets came back as regions of their own");
  check(roadArea > 0, "with real area, so something can stand on them");

  // Area is conserved down the whole hierarchy.
  std::vector<RegionId> leaves;

  for (const auto &region : tree.regions())
  {
    if (tree.childrenOf(region.id).empty())
    {
      leaves.push_back(region.id);
    }
  }

  check(std::abs(leafArea(tree, leaves) - rootArea) < 0.02, "the leaves tile the root exactly");

  // Every region is a valid polygon, concave and holed ones included.
  bool valid = true;

  for (const auto &region : tree.regions())
  {
    if (!geom2::polygonFault(region.boundary).empty())
    {
      valid = false;
    }
  }

  check(valid, "every region in the tree is a valid polygon");
}

void swapTests()
{
  std::cout << "Swapping a generator is a binding\n";

  Registry registry = standardRegistry();
  Context context;
  context.worldSeed = 3;

  const auto runWith = [&](const std::string &streets, const std::string &lots, RunStats &stats, RegionTree &tree) {
    const RegionId root = seed(tree, boxPolygon(0, 0, 120, 120));

    Plan plan;
    plan.policy.defaults[Role::Streets] = streets;
    plan.policy.defaults[Role::Lots] = lots;
    plan.steps.push_back({Role::Streets, {}, 0});
    plan.steps.push_back(
        {Role::Lots,
         [](const Region &r) {
           return r.cover != Cover::Road;
         },
         0});

    Composer composer(registry, context);
    Ledger ledger;
    std::string error;

    return composer.run(plan, tree, root, ledger, stats, &error);
  };

  RegionTree gridTree;
  RunStats gridStats;
  check(runWith("streets.grid", "lots.strip", gridStats, gridTree), "a grid plan runs");

  RegionTree radialTree;
  RunStats radialStats;
  check(runWith("streets.radial", "lots.strip", radialStats, radialTree), "a radial plan runs");

  // The only difference is one string, and the world is different.
  check(gridTree.regions().size() != radialTree.regions().size(), "changing one binding changes the result");

  bool gridDecided = false;
  bool radialDecided = false;

  for (const auto &decision : gridStats.decisions)
  {
    if (decision.find("streets.grid") != std::string::npos)
    {
      gridDecided = true;
    }
  }

  for (const auto &decision : radialStats.decisions)
  {
    if (decision.find("streets.radial") != std::string::npos)
    {
      radialDecided = true;
    }
  }

  check(gridDecided && radialDecided, "and the ledger records which generator ran");

  RegionTree ringTree;
  RunStats ringStats;
  check(runWith("streets.grid", "lots.ring", ringStats, ringTree), "the ring lot generator runs in the same slot");
  check(ringTree.regions().size() != gridTree.regions().size(), "and gives a different subdivision from the strip one");
}

void perRegionPolicyTests()
{
  std::cout << "Per region selection\n";

  Registry registry = standardRegistry();
  Context context;
  context.worldSeed = 5;

  RegionTree tree;
  const RegionId root = seed(tree, boxPolygon(0, 0, 200, 100));

  Plan plan;
  plan.policy.defaults[Role::Streets] = "streets.grid";
  plan.policy.defaults[Role::Lots] = "lots.strip";

  // The rule a settings boolean cannot express: blocks on the west side take
  // ring lots around a court, the rest take strips. One district, two
  // grammars, decided per region.
  plan.policy.rules.push_back(
      {Role::Lots,
       "lots.ring",
       [](const Region &r) {
         const geom2::Box box = geom2::bounds(r.boundary);

         return geom2::toMetres(box.maxX) < 100.0 && r.areaMetres() > 900.0;
       },
       "west side, large block"});

  plan.steps.push_back({Role::Streets, {}, 0});
  plan.steps.push_back(
      {Role::Lots,
       [](const Region &r) {
         return r.cover != Cover::Road;
       },
       0});

  Composer composer(registry, context);
  Ledger ledger;
  RunStats stats;
  std::string error;

  check(composer.run(plan, tree, root, ledger, stats, &error), "a mixed policy runs: " + error);

  size_t ring = 0;
  size_t strip = 0;

  for (const auto &decision : stats.decisions)
  {
    if (decision.find("lots.ring") != std::string::npos)
    {
      ++ring;
    }

    if (decision.find("lots.strip") != std::string::npos)
    {
      ++strip;
    }
  }

  check(ring > 0, "some blocks took the ring generator");
  check(strip > 0, "and others took the strip generator");
  check(ring + strip > 2, "in the same run, from one plan");
}

void populatorAndFieldTests()
{
  std::cout << "Populators and fields\n";

  Registry registry = standardRegistry();

  Context context;
  context.worldSeed = 11;

  // A hillside: the east side is steep enough to be unbuildable.
  context.terrain = [](const double x, const double) {
    return x < 80.0 ? 0.0 : (x - 80.0) * 0.6;
  };

  RegionTree tree;
  const RegionId root = seed(tree, boxPolygon(0, 0, 120, 120));

  Plan plan;
  plan.policy.defaults[Role::Streets] = "streets.grid";
  plan.policy.defaults[Role::Lots] = "lots.strip";
  plan.policy.defaults[Role::Terrain] = "terrain.slope";
  plan.policy.defaults[Role::Buildings] = "buildings.setback";

  plan.steps.push_back({Role::Streets, {}, 0});
  plan.steps.push_back(
      {Role::Lots,
       [](const Region &r) {
         return r.cover != Cover::Road;
       },
       0});
  plan.steps.push_back({Role::Terrain, {}, 0});
  plan.steps.push_back(
      {Role::Buildings,
       [](const Region &r) {
         return r.cover != Cover::Road && r.attributes.buildable;
       },
       0});

  Composer composer(registry, context);
  Ledger ledger;
  RunStats stats;
  std::string error;

  check(composer.run(plan, tree, root, ledger, stats, &error), "a four step plan runs: " + error);

  // The field generator changed attributes and no geometry.
  size_t measured = 0;
  size_t steep = 0;

  for (const auto &region : tree.regions())
  {
    if (region.attributes.slope > 0.0)
    {
      ++measured;
    }

    if (!region.attributes.buildable)
    {
      ++steep;
    }
  }

  check(measured > 0, "the field generator measured slopes");
  check(steep > 0, "and marked the hillside unbuildable");

  check(stats.objectsPlaced > 0, "the populator placed houses");

  // Nothing was built on a road or on the steep ground.
  bool clear = true;

  for (const auto &object : composer.objects())
  {
    const Region *region = tree.find(object.region);

    if (region == nullptr || region->cover == Cover::Road || !region->attributes.buildable)
    {
      clear = false;
    }
  }

  check(clear, "and no house stands on a road or on unbuildable ground");
  check(ledger.count(LedgerKind::Capacity) > 0, "the ledger records the regions that were too steep");
}

void concaveTests()
{
  std::cout << "Concave regions through the whole chain\n";

  Registry registry = standardRegistry();
  Context context;
  context.worldSeed = 23;

  RegionTree tree;
  const RegionId root = seed(tree, ellPolygon());
  const double rootArea = tree.find(root)->areaMetres();

  check(!tree.find(root)->convex, "the root region is measured as concave, not rejected for it");

  Plan plan;
  plan.policy.defaults[Role::Streets] = "streets.radial";
  plan.policy.defaults[Role::Lots] = "lots.strip";
  plan.steps.push_back({Role::Streets, {}, 0});
  plan.steps.push_back(
      {Role::Lots,
       [](const Region &r) {
         return r.cover != Cover::Road;
       },
       0});

  Composer composer(registry, context);
  Ledger ledger;
  RunStats stats;
  std::string error;

  check(composer.run(plan, tree, root, ledger, stats, &error), "a concave site runs the whole chain: " + error);

  std::vector<RegionId> leaves;
  bool valid = true;
  size_t concaveChildren = 0;

  for (const auto &region : tree.regions())
  {
    if (tree.childrenOf(region.id).empty())
    {
      leaves.push_back(region.id);
    }

    if (!geom2::polygonFault(region.boundary).empty())
    {
      valid = false;
    }

    if (region.id != root && !region.convex)
    {
      ++concaveChildren;
    }
  }

  check(valid, "every region produced from a concave site is valid");
  check(concaveChildren > 0, "and concave children are produced, not subdivided away");
  check(std::abs(leafArea(tree, leaves) - rootArea) < 0.02, "with area conserved through the whole hierarchy");
}

void determinismTests()
{
  std::cout << "Determinism\n";

  Registry registry = standardRegistry();

  const auto build = [&](RegionTree &tree) {
    Context context;
    context.worldSeed = 99;

    const RegionId root = seed(tree, boxPolygon(0, 0, 120, 120));

    Plan plan;
    plan.policy.defaults[Role::Streets] = "streets.radial";
    plan.policy.defaults[Role::Lots] = "lots.strip";
    plan.steps.push_back({Role::Streets, {}, 0});
    plan.steps.push_back(
        {Role::Lots,
         [](const Region &r) {
           return r.cover != Cover::Road;
         },
         0});

    Composer composer(registry, context);
    Ledger ledger;
    RunStats stats;
    std::string error;

    return composer.run(plan, tree, root, ledger, stats, &error);
  };

  RegionTree first;
  RegionTree second;

  check(build(first) && build(second), "two runs of the same plan complete");
  check(first.regions().size() == second.regions().size(), "and produce the same number of regions");

  bool identical = first.regions().size() == second.regions().size();

  for (size_t i = 0; identical && i < first.regions().size(); ++i)
  {
    if (first.regions()[i].boundary.outer != second.regions()[i].boundary.outer)
    {
      identical = false;
    }
  }

  check(identical, "with byte-identical geometry, because the randomness is addressed by purpose and not by order");
}

} // namespace

int main()
{
  std::cout << "generator hierarchy tests\n";

  registryTests();
  hierarchyTests();
  swapTests();
  perRegionPolicyTests();
  populatorAndFieldTests();
  concaveTests();
  determinismTests();

  if (failures != 0)
  {
    std::cerr << failures << " generator hierarchy test(s) failed\n";

    return 1;
  }

  std::cout << "All generator hierarchy tests passed\n";

  return 0;
}
