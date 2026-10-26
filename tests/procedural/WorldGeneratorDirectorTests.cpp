#include "procedural/world/WorldEditablePatches.hpp"
#include "procedural/world/WorldGenerationEdits.hpp"
#include "procedural/world/WorldGenerationSession.hpp"
#include "procedural/world/WorldGeneratorDirector.hpp"
#include "procedural/world/WorldMapPatchAdapter.hpp"
#include "procedural/world/WorldPatchGeneratorAdapters.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldPipelineEditSession.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace procedural::world;
using namespace procedural::world::rewrite;
namespace procedural::world
{
bool generateWorldBeforeDirectorRefactor(const WorldPipelineSettings &, GeneratedWorld &, const WorldPipelineCallbacks &, std::string *);
}
int main()
{
  int failures = 0, checks = 0;
  auto check = [&](bool ok, const char *name)
  {
    ++checks;
    if (!ok)
      ++failures;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
  };
  auto port = [](std::string name)
  {
    return GenerationPort{{name, 1}, typeid(int), 1};
  };
  auto make = [&](std::string id, std::vector<GenerationPort> reads, std::string output)
  {
    GenerationTask t;
    t.descriptor = {id, 1, 1, GenerationScope::Patch, reads, {port(output)}};
    t.generate = [=](const GenerationRequest &r, GenerationSnapshot &d, std::string *)
    {
      int sum = 1;
      for (auto p : reads)
      {
        auto value = r.context.get<int>(p.key);
        if (!value)
          return false;
        sum += *value;
      }
      d.emplace(GenerationKey{output, 1}, GenerationProduct{sum, 1});
      return true;
    };
    return t;
  };
  auto a = make("a", {}, "a"), b = make("b", {port("a")}, "b"), c = make("c", {port("a")}, "c"), join = make("join", {port("b"), port("c")}, "join");
  std::vector<GenerationTask> tasks{join, c, a, b};
  GenerationPlan plan;
  std::string error;
  check(planGenerators(tasks, {}, plan, &error) && plan.levels.size() == 3 && plan.levels[1].size() == 2, "typed fork/join ordering");
  GenerationSnapshot output;
  check(runGenerators(tasks, {}, 7, output, {}, &error) && std::any_cast<int>(output.at({"join", 1}).value) == 5, "execute typed products");
  std::reverse(tasks.begin(), tasks.end());
  GenerationSnapshot reordered;
  check(runGenerators(tasks, {}, 7, reordered) && std::any_cast<int>(reordered.at({"join", 1}).value) == 5, "permutation parity");
  auto bad = tasks;
  bad.push_back(a);
  check(!planGenerators(bad, {}, plan), "duplicate identity rejected");
  bad = {a, make("other", {}, "a")};
  check(!planGenerators(bad, {}, plan), "duplicate writer rejected");
  bad = {make("missing", {port("absent")}, "out")};
  check(!planGenerators(bad, {}, plan), "missing input rejected");
  bad = {make("x", {port("y")}, "x"), make("y", {port("x")}, "y")};
  check(!planGenerators(bad, {}, plan), "cycle rejected");
  bad = {a, b};
  bad[1].descriptor.reads[0].type = typeid(double);
  check(!planGenerators(bad, {}, plan), "type mismatch rejected");
  bad = {a, b};
  bad[1].descriptor.reads[0].schema = 2;
  check(!planGenerators(bad, {}, plan), "schema mismatch rejected");
  check(
      !runGenerators(
          tasks,
          {},
          7,
          output,
          []
          {
            return true;
          }) &&
          std::any_cast<int>(output.at({"join", 1}).value) == 5,
      "cancellation preserves prior output");
  bad = {a, b};
  bad[1].generate = [](auto &, auto &, auto *)
  {
    throw std::runtime_error("fixture");
    return false;
  };
  check(!runGenerators(bad, {}, 7, output) && std::any_cast<int>(output.at({"join", 1}).value) == 5, "failure after predecessor remains atomic");
  bad = {a};
  bad[0].generate = [](auto &, auto &d, auto *)
  {
    d.emplace(GenerationKey{"undeclared", 1}, GenerationProduct{1, 1});
    return true;
  };
  check(!runGenerators(bad, {}, 7, output), "undeclared write rejected");
  GenerationSnapshot input{{{"secret", 1}, {4, 1}}};
  // GenerationReadView stores its ports by reference, so the vector has to
  // outlive the view. Passing a braced temporary here bound a reference to
  // memory that died at the end of the statement; it only appeared to work
  // because the dead stack still looked like an empty vector.
  const std::vector<GenerationPort> noPorts;
  GenerationReadView view(input, noPorts);
  check(!view.get<int>({"secret", 1}), "undeclared context read blocked");
  uint64_t seed1 = 0, seed2 = 0;
  auto seeded = a;
  seeded.generate = [&](auto &r, auto &d, auto *)
  {
    seed1 = r.seed;
    d.emplace(GenerationKey{"a", 1}, GenerationProduct{1, 1});
    return true;
  };
  runGenerators({seeded}, {}, 7, output);
  seed2 = seed1;
  runGenerators({make("unrelated", {}, "other"), seeded}, {}, 7, output);
  check(seed1 == seed2, "seed independent of unrelated task ordering");
  std::vector<size_t> affected;
  check(affectedGenerators(tasks, {}, {{"b", 1}}, affected) && affected.size() == 1 && tasks[affected[0]].descriptor.id == "join", "changed branch excludes unrelated sibling");
  check(affectedGenerators(tasks, {}, {{"a", 1}}, affected) && affected.size() == 3, "upstream edit propagates through fork and join");
  check(affectedGenerators(tasks, {}, {{"other-domain", 2}}, affected) && affected.empty(), "unrelated product does not invalidate tasks");

  GenerationSnapshot projected;
  check(runGeneratorsProjected(tasks, {}, 7, projected, {{"join", 1}}) && projected.size() == 1 && std::any_cast<int>(projected.at({"join", 1}).value) == 5, "one-shot projection releases consumed intermediates");
  check(!runGeneratorsProjected(tasks, {}, 7, projected, {{"missing", 1}}) && projected.size() == 1, "invalid projection preserves prior output");
  GenerationSession session;
  GenerationUpdateReport report;
  check(updateGenerationSession(tasks, {}, 7, 0, {}, session, report) && report.executed.size() == 4, "session initial execution");
  check(updateGenerationSession(tasks, {}, 7, 1, {}, session, report) && report.executed.empty() && report.reused.size() == 4, "unchanged session reuses all products");
  check(updateGenerationSession(tasks, {}, 7, 2, {{"b", 1}}, session, report) && report.executed.size() == 2 && report.reused.size() == 2, "dirty invocation rebuilds only branch and consumers");
  check(!updateGenerationSession(tasks, {}, 7, 1, {}, session, report) && session.revision == 3, "stale session update rejected");
  GenerationSnapshot external{{{"a", 1}, {3, 1, 1}}};
  GenerationSession reader;
  GenerationUpdateReport readerReport;
  check(updateGenerationSession({b}, external, 7, 0, {}, reader, readerReport), "external revision tracked");
  external.at({"a", 1}) = {7, 1, 2};
  check(updateGenerationSession({b}, external, 7, 1, {}, reader, readerReport) && std::any_cast<int>(reader.products.at({"b", 1}).value) == 8, "changed input token rebuilds reader");
  external.at({"a", 1}).revision = 1;
  check(!updateGenerationSession({b}, external, 7, 2, {}, reader, readerReport) && reader.revision == 2, "regressing input token rejected");
  FarmGenerationBrief farm;
  farm.geometry.patch = 44;
  farm.geometry.source = 9;
  farm.geometry.outer = {{0, 0, 0}, {120, 0, 0}, {120, 0, 100}, {0, 0, 100}};
  farm.geometry.access = {{0, 0, 0}, {0, 0, 100}};
  GenerationSnapshot farmInputs{{{"farm.brief", 44}, {farm, 1, 1}}, {{"shared.road", 44}, {123, 1, 1}}};
  GenerationSession domain;
  check(
      updateGenerationSession({farmFieldsGenerator(44)}, farmInputs, 7, 0, {}, domain, report) && std::any_cast<const FarmFieldProduct &>(domain.products.at({"farm.fields", 44}).value).cropArea > 0,
      "real farm adapter generates cultivated fields");
  DwellingBrief home;
  home.id = 44;
  home.shell = {{0, 0, 0}, {10, 0, 0}, {10, 0, 16}, {0, 0, 16}};
  home.entrance = {5, 0, 0};
  GenerationSnapshot urbanInputs{{{"dwelling.brief", 44}, {home, 1, 1}}, {{"shared.road", 44}, {123, 1, 1}}};
  std::vector<GenerationTask> urban{dwellingMeshGenerator(44), dwellingPlanGenerator(44)};
  check(
      updateGenerationSession(urban, urbanInputs, 7, 1, {}, domain, report) && !domain.products.count({"farm.fields", 44}) && domain.products.count({"shared.road", 44}) &&
          !std::any_cast<const std::vector<DwellingTriangle> &>(domain.products.at({"dwelling.mesh", 44}).value).empty(),
      "farm producer retired atomically; real dwelling mesh replaces it and shared road remains");
  GenerationSnapshot clean;
  check(
      runGenerators(urban, urbanInputs, 7, clean) &&
          std::any_cast<const DwellingPlan &>(clean.at({"dwelling.plan", 44}).value).residentCapacity == std::any_cast<const DwellingPlan &>(domain.products.at({"dwelling.plan", 44}).value).residentCapacity &&
          std::any_cast<const std::vector<DwellingTriangle> &>(clean.at({"dwelling.mesh", 44}).value).size() ==
              std::any_cast<const std::vector<DwellingTriangle> &>(domain.products.at({"dwelling.mesh", 44}).value).size(),
      "conversion matches clean native generation capacities and mesh count");
  const auto &cleanMesh = std::any_cast<const std::vector<DwellingTriangle> &>(clean.at({"dwelling.mesh", 44}).value);
  const auto &editedMesh = std::any_cast<const std::vector<DwellingTriangle> &>(domain.products.at({"dwelling.mesh", 44}).value);
  auto equalPoint = [](Position a, Position b)
  {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  };
  check(
      cleanMesh.size() == editedMesh.size() && std::equal(
                                                   cleanMesh.begin(),
                                                   cleanMesh.end(),
                                                   editedMesh.begin(),
                                                   [&](auto &a, auto &b)
                                                   {
                                                     return equalPoint(a.a, b.a) && equalPoint(a.b, b.b) && equalPoint(a.c, b.c) && a.material == b.material;
                                                   }),
      "every converted mesh triangle matches clean generation");
  home.entrance = {500, 0, 500};
  urbanInputs.at({"dwelling.brief", 44}) = {home, 1, 2};
  check(!updateGenerationSession(urban, urbanInputs, 7, 2, {}, domain, report) && domain.revision == 2 && domain.products.count({"dwelling.mesh", 44}), "invalid replacement retains previous world products");

  EditablePatchSet patches;
  patches.vertices = {{1, {0, 0, 0}}, {2, {10, 0, 0}}, {3, {20, 0, 0}}, {4, {0, 0, 10}}, {5, {10, 0, 10}}, {6, {20, 0, 10}}, {7, {0, 0, 20}}, {8, {10, 0, 20}}, {9, {20, 0, 20}}};
  patches.patches = {
    {1, {1, 77, 4, 1, PatchLandUse::Farm, {1, 2, 5, 4}, false}},
    {2, {2, 77, 4, 1, PatchLandUse::Farm, {2, 3, 6, 5}, false}},
    {3, {3, 77, 4, 1, PatchLandUse::Urban, {4, 5, 8, 7}, false}},
    {4, {4, 77, 4, 1, PatchLandUse::Urban, {5, 6, 9, 8}, false}}};
  EditablePatchSet edited;
  PatchEditImpact impact;
  check(
      validateEditablePatches(patches) && movePatchVertex(patches, 5, {11, 0, 11}, 1, edited, impact) && impact.patches.size() == 4 && edited.patches.at(1).owner == 77 && edited.vertices.count(5),
      "shared vertex edit retains IDs and includes every incident cell");
  auto readers = patchSpatialReaders({{"near", 1, {0, 0, 5, 5}}, {"empty-query", 2, {19, 19, 21, 21}}, {"far", 3, {100, 100, 110, 110}}}, impact);
  check(readers.size() == 2, "old/new domain bounds invalidate spatial readers including empty queries");
  auto stableRevision = edited.revision;
  check(!movePatchVertex(patches, 5, {30, 0, 30}, 1, edited, impact) && edited.revision == stableRevision, "inverted/overlapping edit fails atomically");
  check(
      assignPatchLandUse(patches, 1, PatchLandUse::Urban, 1, edited, impact) && edited.patches.at(1).owner == 77 && edited.patches.at(1).biome == 4 && edited.patches.at(1).use == PatchLandUse::Urban,
      "land-use override retains kingdom and biome");
  patches.patches.at(1).occupied = true;
  check(!assignPatchLandUse(patches, 1, PatchLandUse::Urban, 1, edited, impact), "occupied conversion cannot silently erase residents");

  EditableGenerationWorld combined;
  combined.geometry.vertices = {{1, {0, 0, 0}}, {2, {120, 0, 0}}, {3, {120, 0, 100}}, {4, {0, 0, 100}}};
  combined.geometry.patches = {{44, {44, 77, 4, 1, PatchLandUse::Farm, {1, 2, 3, 4}, false}}};
  PatchGenerationFactory factory = [&](const EditablePatchSet &geometry, PatchGenerationBuild &build, std::string *)
  {
    const auto &patch = geometry.patches.at(44);
    build.inputs[GenerationKey{"shared.road", 44}] = {123, 1, 1};
    if (patch.use == PatchLandUse::Farm)
    {
      auto f = farm;
      f.geometry.outer.clear();
      for (auto id : patch.vertices)
        f.geometry.outer.push_back(geometry.vertices.at(id));
      build.inputs[{"farm.brief", 44}] = {f, 1, patch.revision};
      build.tasks = {farmFieldsGenerator(44)};
    }
    else
    {
      auto h = home;
      h.entrance = {5, 0, 0};
      build.inputs[{"dwelling.brief", 44}] = {h, 1, patch.revision};
      build.tasks = urban;
    }
    return true;
  };
  check(editGeneratedWorld({}, 1, 0, 7, factory, combined, report), "combined initial native patch generation");
  PatchGenerationEdit convert;
  convert.kind = PatchGenerationEdit::Kind::AssignLandUse;
  convert.id = 44;
  convert.use = PatchLandUse::Urban;
  check(
      !editGeneratedWorld(
          convert,
          1,
          1,
          7,
          factory,
          combined,
          report,
          [](auto &, auto *)
          {
            return false;
          }) &&
          combined.geometry.patches.at(44).use == PatchLandUse::Farm && combined.generation.products.count({"farm.fields", 44}),
      "publication failure rolls back geometry AND generated products");
  check(
      editGeneratedWorld(convert, 1, 1, 7, factory, combined, report) && combined.geometry.patches.at(44).use == PatchLandUse::Urban && !combined.generation.products.count({"farm.fields", 44}) &&
          combined.generation.products.count({"dwelling.mesh", 44}),
      "assignment and native producer replacement commit together");
  check(!editGeneratedWorld(convert, 1, 1, 7, factory, combined, report), "stale combined edit rejected");

  auto nested = patches;
  nested.patches.at(1).occupied = false;
  nested.patches.emplace(10, EditablePatch{10, 77, 4, 1, PatchLandUse::Unassigned, {1, 3, 9, 7}, false});
  for (uint64_t id : {1, 2, 3, 4})
    nested.patches.at(id).parent = 10;
  check(
      validateEditablePatches(nested, &error) && movePatchVertex(nested, 9, {22, 0, 22}, 1, edited, impact, &error) && impact.patches.size() >= 3 && edited.patches.at(4).parent == 10,
      "hierarchical parent edit deforms descendants and preserves tiling");
  std::atomic<int> entered{0};
  auto worker = [&](std::string id)
  {
    auto t = make(id, {}, id);
    t.parallelSafe = true;
    t.generate = [&, id](auto &, auto &d, auto *)
    {
      entered.fetch_add(1);
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (entered.load() < 2 && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
      d.emplace(GenerationKey{id, 1}, GenerationProduct{entered.load() >= 2 ? 1 : 0, 1});
      return true;
    };
    return t;
  };
  GenerationSnapshot concurrent;
  check(
      runGeneratorsParallel({worker("parallel-a"), worker("parallel-b")}, {}, 7, concurrent, 2) && std::any_cast<int>(concurrent.at({"parallel-a", 1}).value) == 1 &&
          std::any_cast<int>(concurrent.at({"parallel-b", 1}).value) == 1,
      "explicit safe tasks actually execute concurrently");
  entered.store(0);
  GenerationSession parallelSession;
  check(updateGenerationSession({worker("parallel-a"), worker("parallel-b")}, {}, 7, 0, {}, parallelSession, report, {}, &error, 2) && entered.load() == 2, "incremental session uses bounded parallel executor");
  GenerationSnapshot serialNative, parallelNative;
  auto h = home;
  h.entrance = {5, 0, 0};
  auto second = h;
  second.id = 45;
  GenerationSnapshot multiInputs{{{"dwelling.brief", 44}, {h, 1, 1}}, {{"dwelling.brief", 45}, {second, 1, 1}}};
  std::vector<GenerationTask> multi{dwellingPlanGenerator(44), dwellingMeshGenerator(44), dwellingPlanGenerator(45), dwellingMeshGenerator(45)};
  check(
      runGenerators(multi, multiInputs, 7, serialNative) && runGeneratorsParallel(multi, multiInputs, 7, parallelNative, 2) &&
          std::any_cast<const std::vector<DwellingTriangle> &>(serialNative.at({"dwelling.mesh", 45}).value).size() ==
              std::any_cast<const std::vector<DwellingTriangle> &>(parallelNative.at({"dwelling.mesh", 45}).value).size(),
      "native patch fork/join serial and parallel mesh parity");
  WorldPipelineSettings settings;
  settings.brief.extentMetres = 12000;
  settings.brief.seed = 17;
  settings.cellSpacingMetres = 1500;
  settings.hierarchyLevels = 1;
  GeneratedWorld legacy, directed;
  bool old = generateWorld(settings, legacy, {}, &error);
  bool modern = generateWorldDirected(settings, directed, {}, &error);
  check(old && modern, "real pipeline adapter succeeds");
  check(
      old && modern && legacy.graph.signature() == directed.graph.signature() && legacy.landCells == directed.landCells && legacy.landAreaSquareMetres == directed.landAreaSquareMetres, "legacy graph and summary parity");

  check(worldPipelineGenerators().size() == 14, "active pipeline migrated to fourteen typed stages");
  bool originalParity = true;
  for (uint64_t seed : {17ULL, 91ULL, 407ULL})
  {
    auto config = settings;
    config.brief.seed = seed;
    config.plateaus = seed == 91;
    config.stylizeRanges = seed == 407;
    GeneratedWorld reference, migrated;
    bool x = generateWorldBeforeDirectorRefactor(config, reference, {}, &error), y = generateWorldDirected(config, migrated, {}, &error);
    // Both pipelines still run, on the same partition, and still produce a
    // world: land, a coastline, and a land area of the same order.
    //
    // What is deliberately no longer checked is bit parity between them. The
    // frozen reference is a copy of the pipeline as it stood before the stage
    // refactor, and it exists to prove that refactor changed no behaviour -
    // which it did prove, and which is done. The staged pipeline has since
    // changed behaviour on purpose: geology scaled to the map extent, a coastal
    // plain and a moisture decay measured from the landmass instead of assumed,
    // and a basin stage the reference does not have. Asserting the two still
    // agree would be asserting that none of those corrections happened.
    originalParity &= x && y;
    originalParity &= reference.landAreaSquareMetres > 0 && migrated.landAreaSquareMetres > 0;
    originalParity &= !reference.coastline.empty() && !migrated.coastline.empty();
    // Same ground, differently dressed: the landmass is shared code, so the two
    // must still agree on how much of the world is land to within a fifth.
    originalParity &= std::abs(reference.landAreaSquareMetres - migrated.landAreaSquareMetres) <= reference.landAreaSquareMetres * .2;
  }
  check(originalParity, "frozen and staged pipelines both produce a world on the same landmass");

  EditablePatchSet imported;
  check(importWorldPatches(directed.graph, imported, &error) && imported.patches.size() == directed.graph.nodeCount(), ("actual generated map imports into editable patch hierarchy: " + error).c_str());
  auto hierarchySettings = settings;
  hierarchySettings.hierarchyLevels = 2;
  hierarchySettings.brief.extentMetres = 24000;
  GeneratedWorld hierarchyWorld;
  bool hierarchyImported = generateWorldDirected(hierarchySettings, hierarchyWorld, {}, &error) && importWorldPatches(hierarchyWorld.graph, imported, &error);
  check(hierarchyImported, ("two-level actual world hierarchy imports with parent tiling: " + error).c_str());

  WorldPipelineEditSession editableMap;
  bool began = beginWorldPipelineEditing(settings, editableMap, {}, &error);
  check(began && editableMap.world(), ("actual world DAG edit session initializes: " + error).c_str());
  if (began)
  {
    std::map<uint64_t, unsigned> incidence;
    for (auto &[id, p] : editableMap.state.geometry.patches)
      for (auto v : p.vertices)
        ++incidence[v];
    uint64_t chosen = 0;
    for (auto [v, n] : incidence)
      if (n >= 3)
      {
        chosen = v;
        break;
      }
    auto edit = PatchGenerationEdit{};
    edit.kind = PatchGenerationEdit::Kind::MoveVertex;
    edit.id = chosen;
    edit.position = editableMap.state.geometry.vertices.at(chosen);
    edit.position.x += .01;
    auto oldSignature = editableMap.world()->graph.signature();
    bool changed = editWorldPipeline(edit, 1, 1, editableMap, report, {}, &error);
    check(changed && report.executed.size() == 14 && editableMap.world()->graph.signature() != oldSignature, ("real map vertex edit regenerates fourteen dependent native stages atomically: " + error).c_str());
  }
  auto signature = directed.graph.signature();
  WorldPipelineCallbacks cancel;
  cancel.cancelled = []
  {
    return true;
  };
  check(!generateWorldDirected(settings, directed, cancel, &error) && signature == directed.graph.signature(), "adapter cancellation preserves published world");
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
