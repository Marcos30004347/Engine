#include "json.hpp"
#include "procedural/world/WorldRuntimeArchive.hpp"
#include "procedural/world/WorldRuntimeBuild.hpp"
#include "procedural/world/WorldRuntimeCollision.hpp"
#include "procedural/world/WorldRuntimeCover.hpp"
#include "procedural/world/WorldRuntimeDisplay.hpp"
#include "procedural/world/WorldRuntimePublication.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace procedural::world::rewrite;
using nlohmann::json;
SurfaceContour rectangle(uint64_t id, double x, double z, double w, double h, uint32_t material, uint32_t layer = 0)
{
  SurfaceContour c;
  c.id = id;
  c.loop = {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
  c.material = material;
  c.layer = layer;
  c.terrainSurface = false;
  return c;
}
json render(const RuntimeSnapshot &s)
{
  json j;
  j["phase"] = 17;
  j["ticket"] = s.ticket;
  j["revision"] = s.assets->levels[0].world.map.revision;
  j["tileLevels"] = s.tileLevels;
  for (size_t tile = 0; tile < s.tileLevels.size(); ++tile)
  {
    const auto &w = s.assets->levels[s.tileLevels[tile]].world;
    for (auto index : w.tiles[tile].triangles)
    {
      auto t = w.mesh.triangles[index];
      size_t first = j["mesh"]["vertices"].size();
      for (auto v : t.vertices)
      {
        auto p = w.mesh.vertices[v];
        j["mesh"]["vertices"].push_back({p.x, p.y, p.z});
      }
      j["mesh"]["triangles"].push_back({first, first + 1, first + 2});
      j["mesh"]["materials"].push_back(t.material);
      j["tileIds"].push_back(tile);
    }
  }
  return j;
}
int main(int argc, char **argv)
{
  auto start = std::chrono::steady_clock::now();
  int count = 0;
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    if (!ok)
    {
      std::cerr << "FAIL " << name << ": " << error << '\n';
      std::exit(1);
    }
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  // Declared before qualification: <=60 seconds, <=400,000 manager-resident triangles,
  // three LODs, eight tiles, fixed seed 17. No comparison to the unfinished legacy baseline.
  auto terrain = [](double x, double z)
  {
    double phase = z < 22 ? z / 22 : z > 26 ? (z - 26) / 22 : 0;
    return .03 * x + 2 * std::pow(std::sin(3.141592653589793 * x / 16) * std::sin(3.141592653589793 * phase), 2);
  };
  SurfaceMapSettings map;
  map.maxX = 64;
  map.maxZ = 48;
  map.cutsX = {16, 32, 48};
  map.cutsZ = {24};
  auto road = rectangle(1, 0, 22, 64, 4, 1);
  road.role = SurfaceRole::Road;
  road.plane.x = .03;
  auto water = rectangle(2, 46, 0, 4, 48, 9, 1);
  water.role = SurfaceRole::Water;
  water.plane.x = .03;
  water.plane.constant = .1;
  WorldFinalPatches base;
  check(freezeWorldPatches({{1, 0, {road, water}}}, map, {}, terrain, 17, base, &error), "freeze eight-tile relief/road/water fixture");
  RuntimeDependencies deps{1, 1, 1, 1, 1};
  WorldCoverDetail covers;
  check(covers.build(base, {{0, {0}, false, false}, {1, {1}, true, false}}, {}, &error), "build cover dependency");
  std::vector<RuntimeCoverPage> pages;
  for (auto tile : base.tiles)
  {
    CoverPage p;
    check(covers.page(tile.x * 16, tile.z * 24, 17, 25, 1, 1, 1, p, &error), "generate versioned tile cover page");
    pages.push_back({17, deps, std::move(p)});
  }
  PatchSurface surface = [&](const SurfaceMapFace &f, double x, double z)
  {
    return f.terrainSurface ? terrain(x, z) : f.plane.height(x, z);
  };
  std::vector<SurfaceErrorSettings> errors(3);
  std::vector<SurfaceRefinement> density(3);
  for (int i = 0; i < 3; ++i)
  {
    errors[i].heightMetres = std::array<double, 3>{.05, .15, .4}[i];
    errors[i].normalDegrees = 180;
    errors[i].maximumPasses = 24;
    density[i].groundSpacingMetres = std::array<double, 3>{3., 7., 14.}[i];
    density[i].modifiedSpacingMetres = density[i].groundSpacingMetres;
  }
  RuntimeWorldAssets assets;
  check(buildRuntimeLods(base, surface, {}, errors, density, deps, pages, assets, &error), "build three measured LODs with locked interfaces");
  check(buildRuntimeCoverMips(assets, covers, {{0, {0}, false, false}, {1, {1}, true, false}}, 3, &error), "build three source-filtered cover footprint levels");
  CoverSample roadCover, groundCover;
  bool hardCover = true;
  for (double footprint : {1., 2., 4., 32.})
  {
    hardCover &= sampleRuntimeCover(assets, 8, 22.01, 1, footprint, roadCover, &error) && roadCover.hard && roadCover.owner == 1 && roadCover.weights[1] == 1;
    hardCover &= sampleRuntimeCover(assets, 8, 21.99, 0, footprint, groundCover, &error) && !groundCover.hard && groundCover.weights[1] == 0;
  }
  check(hardCover, "road boundary never leaks across mip footprints or triangle owners");
  bool sourceMipParity = true;
  for (const auto &mip : assets.coverMips)
    for (const auto &page : mip)
    {
      const auto &p = page.page;
      CoverSample expected;
      const auto &actual = p.samples[size_t(p.gutter + 1) * (p.width + 2 * p.gutter) + p.gutter + 1];
      sourceMipParity &= covers.sample((p.originX + 1) * p.spacingMetres, (p.originZ + 1) * p.spacingMetres, p.footprintMetres, expected, &error) && expected.weights == actual.weights &&
                         expected.variation == actual.variation && expected.cropRows == actual.cropRows;
    }
  check(sourceMipParity, "coarse cover samples match direct filtered semantic source evaluations");
  std::cout << "PRINT cover-mips levels=" << assets.coverMips.size() + 1 << " tiles-per-level=" << assets.coverPages.size() << " hard-boundary-leaks=0\n";
  auto invalidMip = assets;
  invalidMip.coverMips[0][0].dependencies.cover++;
  check(!validateRuntimeAssets(invalidMip, &error), "stale cover mip dependency rejected");
  invalidMip = assets;
  invalidMip.coverMips[1][1].page.samples[0].variation += .1;
  check(!validateRuntimeAssets(invalidMip, &error), "cover mip gutter disagreement rejected");
  check(!sampleRuntimeCover(assets, NAN, 0, 0, 1, groundCover, &error), "invalid cover query rejected");
  check(assets.levels[2].world.mesh.triangles.size() < assets.levels[0].world.mesh.triangles.size(), "coarse LOD reduces triangle count");
  // Sample actual display triangles, independently of queryRuntimeSurface's
  // deliberate finest-LOD collision policy. Verify submerged/visible classification
  // and geometric depth, including dry parts of this deliberately imperfect river.
  auto displayHeight = [](const RefinedWorldSurfaces &level, double x, double z, uint32_t layer) -> double
  {
    for (const auto &t : level.world.mesh.triangles)
      if (t.layer == layer)
      {
        auto a = level.world.mesh.vertices[t.vertices[0]], b = level.world.mesh.vertices[t.vertices[1]], c = level.world.mesh.vertices[t.vertices[2]];
        double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        double u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / den;
        double v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / den;
        if (u >= -1e-10 && v >= -1e-10 && u + v <= 1 + 1e-10)
          return u * a.y + v * b.y + (1 - u - v) * c.y;
      }
    return NAN;
  };
  size_t waterSamples = 0, wetSamples = 0, drySamples = 0;
  double maximumWaterDepthDelta = 0;
  bool invariant = true;
  json waterProbes = json::array();
  for (double x = 46.125; x < 50; x += .25)
    for (double z = .125; z < 48; z += .25)
    {
      double fineDepth = displayHeight(assets.levels[0], x, z, 1) - displayHeight(assets.levels[0], x, z, 0);
      waterProbes.push_back({x, z, fineDepth});
      ++waterSamples;
      if (fineDepth > 0)
        ++wetSamples;
      else
        ++drySamples;
      for (size_t lod = 1; lod < assets.levels.size(); ++lod)
      {
        double depth = displayHeight(assets.levels[lod], x, z, 1) - displayHeight(assets.levels[lod], x, z, 0);
        waterProbes.back().push_back(depth);
        invariant &= std::isfinite(depth) && depth == fineDepth;
        maximumWaterDepthDelta = std::max(maximumWaterDepthDelta, std::abs(depth - fineDepth));
      }
    }
  check(invariant && wetSamples && drySamples, "display LOD preserves waterline and depth at 3072 wet/dry probes");
  std::cout << "PRINT protected-water samples=" << waterSamples << " wet=" << wetSamples << " dry=" << drySamples << " maximum-depth-delta=" << maximumWaterDepthDelta << '\n';
  bool roadInvariant = true;
  for (double x = .25; x < 64; x += .5)
    for (size_t lod = 1; lod < assets.levels.size(); ++lod)
      roadInvariant &= displayHeight(assets.levels[lod], x, 23.25, 0) == displayHeight(assets.levels[0], x, 23.25, 0);
  check(roadInvariant, "inclined road surface remains exact across display LODs");
  // A perfectly valid standalone coarse triangulation is no longer a valid
  // runtime level if it changes protected physical geometry.
  auto unsafe = assets;
  check(refineWorldSurfaces(base, surface, {}, errors[2], unsafe.levels[2], &error), "construct independently valid unsafe coarse level");
  check(!validateRuntimeAssets(unsafe, &error) && error == "runtime LOD changed protected physical geometry", "publication rejects independently valid waterline-changing LOD");
  size_t unsafeWaterlineChanges = 0;
  double unsafeMaximumDepthDelta = 0;
  for (auto &probe : waterProbes)
  {
    double x = probe[0], z = probe[1], depth = probe[2];
    double coarseDepth = displayHeight(unsafe.levels[2], x, z, 1) - displayHeight(unsafe.levels[2], x, z, 0);
    probe.push_back(coarseDepth);
    unsafeMaximumDepthDelta = std::max(unsafeMaximumDepthDelta, std::abs(coarseDepth - depth));
    if ((depth > 0) != (coarseDepth > 0))
      ++unsafeWaterlineChanges;
  }
  check(unsafeWaterlineChanges > 0, "waterline regression detects visible coverage change without feature protection");
  RuntimeBuildInput recipe;
  recipe.revision = 17;
  recipe.samplingSeed = 17;
  recipe.dependencies = deps;
  recipe.producers = {{1, 0, {road, water}}};
  recipe.map = map;
  recipe.terrain = terrain;
  recipe.errors = errors;
  recipe.densities = density;
  recipe.covers = {{0, {0}, false, false}, {1, {1}, true, false}};
  RuntimeBuildResult rebuilt;
  check(buildRuntimeWorld(recipe, &assets, rebuilt, &error) && rebuilt.changedTiles.empty(), "rebuild identical inputs preserves every tile payload");
  auto edited = recipe;
  edited.revision++;
  edited.dependencies.discharge++;
  edited.producers[0].contours[1].plane.constant += .05;
  check(buildRuntimeWorld(edited, &assets, rebuilt, &error) && !rebuilt.changedTiles.empty() && rebuilt.changedTiles.size() < 8, "water geometry edit rebuilds actual world and identifies affected tiles");
  std::cout << "PRINT edited-water changed-tiles=" << json(rebuilt.changedTiles).dump() << " revision=" << rebuilt.assets.levels[0].world.map.revision << "\n";
  RuntimeSnapshot rebuiltSnapshot{0, std::make_shared<const RuntimeWorldAssets>(rebuilt.assets), std::vector<uint32_t>(8, 2)};
  RuntimeSurfaceHit changedWater;
  check(queryRuntimeSurface(rebuiltSnapshot, 48, 10, 1, changedWater, &error) && std::abs(changedWater.height - (.03 * 48 + .15)) < 1e-10, "rebuilt water reader sees changed physical height, not just a version bump");
  const auto preservedRevision = rebuilt.assets.levels[0].world.map.revision;
  edited.producers[0].contours[1].loop[0].x = NAN;
  check(!buildRuntimeWorld(edited, &assets, rebuilt, &error) && rebuilt.assets.levels[0].world.map.revision == preservedRevision, "invalid edit retains previous complete rebuild result");
  auto bad = assets;
  bad.coverPages[0].dependencies.cover++;
  check(!validateRuntimeAssets(bad, &error), "stale page generation rejected");
  bad = assets;
  bad.levels[2].world.map.vertices.front().y += .01;
  check(!validateRuntimeAssets(bad, &error), "shifted LOD seam rejected");
  bad = assets;
  bad.coverPages[0].page.originX += 100;
  check(!validateRuntimeAssets(bad, &error), "misplaced cover page rejected");
  bad = assets;
  bad.coverPages[1].page.samples[0].variation += .1;
  check(!validateRuntimeAssets(bad, &error), "mismatched page gutter rejected");
  bad = assets;
  bad.levels[0].triangleHeightErrors[0] = NAN;
  check(!validateRuntimeAssets(bad, &error), "invalid error payload rejected");
  WorldRuntimePublication publication;
  uint64_t ticket = 0;
  std::vector<uint32_t> initial(8, 2), mixed{0, 1, 2, 0, 2, 1, 0, 2};
  check(publication.begin(assets, initial, 400000, ticket, &error), "begin complete parent coverage");
  for (uint32_t tile = 0; tile < 7; ++tile)
    check(publication.complete(ticket, tile, deps, &error) && !publication.acquire(), "unready initial generation remains unpublished");
  check(publication.complete(ticket, 7, deps, &error) && publication.acquire(), "complete initial generation publishes once");
  auto parent = publication.acquire();
  std::vector<json> frames;
  frames.push_back(render(*parent));
  RuntimeSurfaceHit before;
  check(queryRuntimeSurface(*parent, 8, 12, 0, before, &error), "collision reader uses finest source mesh");
  uint64_t pending;
  check(publication.begin(assets, mixed, 400000, pending, &error), "begin unequal LOD selection");
  check(publication.complete(pending, 0, deps, &error) && publication.complete(pending, 1, deps, &error) && publication.acquire() == parent, "parent retained while children are incomplete");
  frames.push_back(render(*publication.acquire()));
  auto wrong = deps;
  wrong.discharge++;
  check(!publication.complete(pending, 2, wrong, &error) && publication.acquire() == parent, "stale discharge completion cannot mix readers");
  std::atomic<uint32_t> next{2};
  std::atomic<bool> succeeded{true};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i)
    workers.emplace_back(
        [&]
        {
          for (;;)
          {
            auto t = next.fetch_add(1);
            if (t >= 8)
              break;
            if (!publication.complete(pending, t, deps))
              succeeded = false;
          }
        });
  for (auto &w : workers)
    w.join();
  check(succeeded && publication.acquire()->tileLevels == mixed, "eight workers publish complete deterministic selection");
  auto current = publication.acquire();
  bool seamIdentity = true;
  for (const auto &edge : base.map.edges)
    if (edge.leftFace && edge.rightFace)
    {
      auto a = base.map.vertices[edge.a], b = base.map.vertices[edge.b];
      for (const auto &level : assets.levels)
      {
        auto u = level.world.mesh.vertices[edge.a], v = level.world.mesh.vertices[edge.b];
        seamIdentity &= a.x == u.x && a.y == u.y && a.z == u.z && b.x == v.x && b.y == v.y && b.z == v.z;
      }
    }
  check(seamIdentity, "independent exact XYZ check for every shared LOD edge");
  double coverage = 0;
  for (size_t tile = 0; tile < mixed.size(); ++tile)
  {
    const auto &w = current->assets->levels[mixed[tile]].world;
    for (auto id : w.tiles[tile].triangles)
    {
      auto t = w.mesh.triangles[id];
      if (t.layer)
        continue;
      auto a = w.mesh.vertices[t.vertices[0]], b = w.mesh.vertices[t.vertices[1]], c = w.mesh.vertices[t.vertices[2]];
      coverage += ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;
    }
  }
  check(std::abs(coverage - 64 * 48) < 1e-8, "independent mixed-LOD ground coverage");
  frames.push_back(render(*current));
  RuntimeSurfaceHit after, waterHit;
  check(queryRuntimeSurface(*current, 8, 12, 0, after) && after.height == before.height, "collision height independent of display LOD");
  check(queryRuntimeSurface(*current, 48, 12, 1, waterHit) && waterHit.revision == after.revision && waterHit.height > terrain(48, 12), "water and collision share revision and retain depth");
  check(!publication.complete(ticket, 0, deps, &error), "old job completion rejected after publication");
  uint64_t cancelled;
  check(
      publication.begin(assets, initial, 400000, cancelled, &error) && publication.cancel(cancelled) && !publication.complete(cancelled, 0, deps, &error) && publication.acquire() == current,
      "cancellation retains complete coverage");
  uint64_t capacityTicket = 123;
  check(!publication.begin(assets, initial, 1, capacityTicket, &error) && capacityTicket == 123 && publication.acquire() == current, "capacity exhaustion preserves visible snapshot");
  // Each dependency edit invalidates prior completions, including edits during generation.
  for (int edit = 0; edit < 5; ++edit)
  {
    auto changed = assets;
    auto &d = changed.dependencies;
    switch (edit)
    {
    case 0:
      ++d.road;
      break;
    case 1:
      ++d.discharge;
      break;
    case 2:
      ++d.spline;
      break;
    case 3:
      ++d.settlement;
      break;
    case 4:
      ++d.cover;
      break;
    }
    for (auto &p : changed.coverPages)
      p.dependencies = d;
    for (auto &mip : changed.coverMips)
      for (auto &p : mip)
        p.dependencies = d;
    uint64_t oldJob, newJob;
    check(publication.begin(assets, mixed, 400000, oldJob) && publication.begin(changed, mixed, 400000, newJob), "dependency edit replaces pending generation");
    check(!publication.complete(oldJob, 0, deps) && !publication.complete(newJob, 0, deps), "dependency edit rejects stale jobs and versions");
    check(publication.cancel(newJob) && publication.acquire() == current, "edit cancellation keeps parent readers coherent");
  }
  // One-worker replay in reverse tile order produces the same render payload.
  WorldRuntimePublication serial;
  uint64_t serialTicket;
  check(serial.begin(assets, mixed, 400000, serialTicket), "one-worker reverse order request");
  for (int t = 7; t >= 0; --t)
    check(serial.complete(serialTicket, t, deps), "serial tile completion");
  auto a = render(*serial.acquire()), b = render(*current);
  a.erase("ticket");
  b.erase("ticket");
  check(a == b, "one and eight workers produce identical reader payloads");
  // Validate every supported neighboring LOD difference, with exact frozen seam positions.
  for (uint32_t left = 0; left < 3; ++left)
    for (uint32_t right = 0; right < 3; ++right)
    {
      std::vector<uint32_t> choice(8);
      for (uint32_t t = 0; t < 8; ++t)
        choice[t] = (t % 4 < 2) ? left : right;
      uint64_t request;
      check(serial.begin(assets, choice, 400000, request), "supported unequal LOD pair begins");
      for (uint32_t t = 0; t < 8; ++t)
        serial.complete(request, t, deps);
      check(serial.acquire()->tileLevels == choice, "supported unequal LOD pair preserves full tile coverage");
    }
  RuntimeDisplayMesh display;
  check(buildRuntimeDisplayMesh(*current, 1000000, display, &error), "runtime display adapter selects coherent mixed tile LODs");
  auto native = render(*current);
  bool displayExact = display.triangles.size() == native["mesh"]["triangles"].size();
  for (size_t i = 0; i < display.vertices.size(); ++i)
    displayExact &= native["mesh"]["vertices"][i] == json::array({display.vertices[i].x, display.vertices[i].y, display.vertices[i].z});
  check(displayExact && json(display.materials) == native["mesh"]["materials"], "viewer adapter preserves exact native geometry/material payload");
  auto keptDisplay = display.vertices.size();
  check(!buildRuntimeDisplayMesh(*current, 1, display, &error) && display.vertices.size() == keptDisplay, "display capacity failure preserves previous mesh");
  auto withStructures = *current->assets;
  withStructures.structures.vertices = {{0, 0, 0}, {0, 2, 0}, {0, 0, 2}};
  withStructures.structures.triangles = {{0, 1, 2}};
  withStructures.structures.expectedNormals = {{1, 0, 0}};
  withStructures.structuralMaterials = {12};
  RuntimeSnapshot structureSnapshot{0, std::make_shared<const RuntimeWorldAssets>(withStructures), mixed};
  check(
      buildRuntimeDisplayMesh(structureSnapshot, 1000000, display, &error) && display.triangles.size() == native["mesh"]["triangles"].size() + 1 && display.materials.back() == 12,
      "vertical structure appended without flattening into terrain layer");
  WorldRuntimeCollision collision;
  check(collision.build(structureSnapshot, 1000000, &error), "build finest terrain/water/structure collision BVH");
  RuntimeCollisionHit wallHit, oppositeHit;
  check(
      collision.segment({-2, .5, .5}, {2, .5, .5}, CollisionStructure, wallHit) && wallHit.category == CollisionStructure && wallHit.material == 12 && wallHit.normal.x == 1 && wallHit.distanceMetres == 2,
      "horizontal ray hits vertical wall with preserved outward normal");
  check(
      collision.segment({2, .5, .5}, {-2, .5, .5}, CollisionStructure, oppositeHit) && oppositeHit.normal.x == wallHit.normal.x && oppositeHit.position.x == 0,
      "structural collision is two-sided without flipping the normal");
  RuntimeCollisionHit hit;
  check(collision.segment({8, 10, 12}, {8, -5, 12}, CollisionSurface, hit) && std::abs(hit.position.y - before.height) < 1e-10, "collision BVH agrees with finest terrain height query");
  RuntimeCollisionHit collisionWaterHit, groundHit;
  check(
      collision.segment({48, 10, 10}, {48, -5, 10}, CollisionWater, collisionWaterHit) && collisionWaterHit.material == 9 && collisionWaterHit.category == CollisionWater,
      "water is independently selectable and excluded from solid queries");
  check(collision.segment({48, 10, 10}, {48, -5, 10}, CollisionSolid, groundHit) && groundHit.category != CollisionWater, "solid ray reaches the ground under water");
  const auto keptHit = wallHit.primitive;
  check(!collision.segment({-2, 3, 3}, {2, 3, 3}, CollisionStructure, wallHit) && wallHit.primitive == keptHit, "miss preserves previous hit output");
  check(
      !collision.segment({0, 0, 0}, {0, 0, 0}, CollisionAll, hit) && !collision.segment({NAN, 0, 0}, {1, 0, 0}, CollisionAll, hit) && !collision.segment({0, 0, 0}, {1, 1, 1}, 8, hit),
      "invalid collision segments and masks rejected");
  check(collision.segment({-1, .5, .5}, {0, .5, .5}, CollisionStructure, hit) && hit.fraction == 1, "collision includes exact segment endpoints");
  check(!collision.segment({0, .2, .2}, {0, .8, .2}, CollisionStructure, hit), "coplanar motion has no crossing hit");
  bool parity = true;
  size_t rayHits = 0;
  json collisionProbes = json::array();
  for (uint32_t i = 0; i < 1000; ++i)
  {
    const double x = double(featureSeed(17, 1, i, 1) % 6800) / 100 - 2, z = double(featureSeed(17, 1, i, 2) % 5200) / 100 - 2;
    Position from{x, 8, z}, to{x + double(int(i % 7) - 3), -3, z + double(int(i % 5) - 2)};
    RuntimeCollisionHit a, b;
    const bool indexed = collision.segment(from, to, CollisionAll, a), brute = collision.segment(from, to, CollisionAll, b, false);
    parity &= indexed == brute && (!indexed || (a.primitive == b.primitive && a.fraction == b.fraction && a.revision == 17));
    rayHits += indexed;
    if (i < 80)
      collisionProbes.push_back({{"from", {from.x, from.y, from.z}}, {"to", {to.x, to.y, to.z}}, {"hit", indexed}, {"point", {a.position.x, a.position.y, a.position.z}}, {"category", a.category}});
  }
  check(parity && rayHits > 800, "1000 deterministic oblique segments match brute-force nearest intersections");
  json wallProbes = json::array();
  bool wallCoverage = true;
  size_t wallCrossings = 0;
  for (uint32_t y = 0; y < 20; ++y)
    for (uint32_t z = 0; z < 20; ++z)
    {
      const double py = (y + .5) / 10, pz = (z + .5) / 10;
      RuntimeCollisionHit probe;
      const bool crossed = collision.segment({-2, py, pz}, {2, py, pz}, CollisionStructure, probe);
      wallCoverage &= crossed == (py + pz <= 2 + 1e-12);
      wallCrossings += crossed;
      wallProbes.push_back({py, pz, crossed});
    }
  check(wallCoverage && wallCrossings > 180, "400 wall cross-section probes match analytic triangle coverage");
  std::atomic<bool> collisionReadParity{true};
  std::vector<std::thread> collisionReaders;
  for (uint32_t worker = 0; worker < 8; ++worker)
    collisionReaders.emplace_back(
        [&, worker]
        {
          for (uint32_t i = worker; i < 256; i += 8)
          {
            RuntimeCollisionHit indexed, reference;
            Position a{double(i % 64) + .1, 10, double(i % 48) + .1}, b{a.x, -5, a.z};
            const bool x = collision.segment(a, b, CollisionSolid, indexed), y = collision.segment(a, b, CollisionSolid, reference, false);
            if (x != y || (x && (indexed.primitive != reference.primitive || indexed.fraction != reference.fraction)))
              collisionReadParity = false;
          }
        });
  for (auto &reader : collisionReaders)
    reader.join();
  check(collisionReadParity, "eight simultaneous collision readers match serial primitive queries");
  const auto countBefore = collision.triangleCount();
  check(!collision.build(structureSnapshot, 1, &error) && collision.triangleCount() == countBefore && collision.revision() == 17, "failed collision rebuild retains previous index");
  WorldRuntimeCollision editedCollision;
  check(
      editedCollision.build(rebuiltSnapshot, 1000000, &error) && editedCollision.segment({48, 10, 10}, {48, -5, 10}, CollisionWater, hit) && hit.revision == 18 &&
          std::abs(hit.position.y - collisionWaterHit.position.y - .05) < 1e-10,
      "edited world collision rebuild changes physical water and revision together");
  check(collision.segment({48, 10, 10}, {48, -5, 10}, CollisionWater, hit) && hit.revision == 17 && hit.position.y == collisionWaterHit.position.y, "old collision reader survives replacement without geometry changes");
  auto deckAssets = withStructures;
  deckAssets.structures.vertices.insert(deckAssets.structures.vertices.end(), {{2, 5, 2}, {4, 5, 2}, {2, 5, 4}});
  deckAssets.structures.triangles.push_back({3, 4, 5});
  deckAssets.structures.expectedNormals.push_back({0, -1, 0});
  deckAssets.structuralMaterials.push_back(1);
  RuntimeSnapshot deckSnapshot{0, std::make_shared<const RuntimeWorldAssets>(deckAssets), mixed};
  WorldRuntimeCollision deckCollision;
  check(
      deckCollision.build(deckSnapshot, 1000000, &error) && deckCollision.segment({2.5, 0, 2.5}, {2.5, 10, 2.5}, CollisionStructure, hit) && hit.position.y == 5 && hit.normal.y == -1,
      "bridge underside keeps its downward structural normal");
  std::cout << "PRINT collision triangles=" << collision.triangleCount() << " nodes=" << collision.nodeCount() << " parity-rays=1000 hits=" << rayHits << "\n";
  withStructures.structures.expectedNormals[0].x = -1;
  check(!validateRuntimeAssets(withStructures, &error), "inverted structural payload rejected");
  std::string archive, roundtrip, editArchive, collisionArchive;
  check(saveRuntimeArchive(deckSnapshot, collisionArchive, &error), "persist structural collision fixture in complete runtime archive");
  check(saveRuntimeArchive(rebuiltSnapshot, editArchive, &error), "archive actual rebuilt geometry for live viewer edit replacement");
  RuntimeSnapshot restored;
  check(saveRuntimeArchive(*current, archive, &error) && loadRuntimeArchive(archive, restored, &error), "complete world geometry/cover archive round trip");
  auto restoredRender = render(restored), originalRender = render(*current);
  restoredRender.erase("ticket");
  originalRender.erase("ticket");
  check(restoredRender == originalRender && restored.ticket == 0, "archive restores exact render geometry without reusing process tickets");
  check(saveRuntimeArchive(restored, roundtrip, &error) && archive == roundtrip, "complete archive has deterministic byte round trip");
  auto legacyCoverArchive = json::parse(archive);
  legacyCoverArchive["assets"].erase("coverDefinitions");
  legacyCoverArchive["assets"].erase("coverMips");
  RuntimeSnapshot legacyCoverSnapshot;
  check(loadRuntimeArchive(legacyCoverArchive.dump(), legacyCoverSnapshot, &error) && legacyCoverSnapshot.assets->coverMips.empty(), "legacy compiled archive without mip extension remains readable");
  auto corrupt = json::parse(archive);
  corrupt["assets"]["levels"][0]["world"]["map"]["faces"][0]["vertices"][0] = 999999999;
  auto pinnedRestored = restored.assets;
  check(!loadRuntimeArchive(corrupt.dump(), restored, &error) && restored.assets == pinnedRestored, "corrupt archive index rejected without replacing reader");
  corrupt = json::parse(archive);
  corrupt["assets"]["levels"][0]["world"]["map"]["faces"][0]["vertices"][0] = 4294967296ull;
  check(!loadRuntimeArchive(corrupt.dump(), restored, &error), "archive rejects index overflow instead of wrapping to zero");
  corrupt = json::parse(archive);
  corrupt["assets"]["coverPages"][0]["dependencies"]["discharge"] = 999;
  check(!loadRuntimeArchive(corrupt.dump(), restored, &error), "archive rejects stale water/cover dependencies");
  check(!loadRuntimeArchive(std::string(40, '['), restored, &error), "archive rejects excessive nesting");
  check(!loadRuntimeArchive(archive.substr(0, archive.size() / 2), restored, &error), "archive rejects truncation");
  std::string saved;
  std::vector<uint32_t> loaded;
  check(saveRuntimeSelection(*current, saved, &error) && loadRuntimeSelection(saved, assets, loaded, &error) && loaded == mixed, "save reload selection against validated assets");
  auto stale = assets;
  stale.dependencies.spline++;
  for (auto &p : stale.coverPages)
    p.dependencies = stale.dependencies;
  for (auto &mip : stale.coverMips)
    for (auto &p : mip)
      p.dependencies = stale.dependencies;
  check(!loadRuntimeSelection(saved, stale, loaded, &error) && loaded == mixed, "stale saved selection rejected atomically");
  check(!loadRuntimeSelection("{broken", assets, loaded, &error), "malformed selection rejected");
  // Input is deeply copied: caller mutations after begin cannot change any pinned reader.
  auto mutableAssets = assets;
  uint64_t copyTicket;
  check(serial.begin(mutableAssets, mixed, 400000, copyTicket), "snapshot isolation transaction");
  mutableAssets.levels[0].world.mesh.vertices[0].y += 999;
  for (uint32_t t = 0; t < 8; ++t)
    serial.complete(copyTicket, t, deps);
  check(serial.acquire()->assets->levels[0].world.mesh.vertices[0].y == assets.levels[0].world.mesh.vertices[0].y, "caller mutation cannot alter runtime readers");
  frames.push_back(render(*serial.acquire()));
  frames.push_back(render(*parent));
  frames.push_back(render(*current));
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  check(seconds < 60, "declared 60-second bounded qualification budget");
  json report = {{"assertions", count}, {"passed", true}, {"seconds", seconds}, {"budgetSeconds", 60}, {"residentTriangleBudget", 400000}, {"workers", {1, 8}}, {"tiles", 8}};
  report["waterline"] = {{"samples", waterSamples}, {"wet", wetSamples}, {"dry", drySamples}, {"maximumDepthDelta", maximumWaterDepthDelta}};
  report["waterline"]["unsafeCoverageChanges"] = unsafeWaterlineChanges;
  report["waterline"]["unsafeMaximumDepthDelta"] = unsafeMaximumDepthDelta;
  report["collision"] = {{"rays", 1000}, {"hits", rayHits}, {"nodes", collision.nodeCount()}, {"triangles", collision.triangleCount()}, {"bruteForceParity", parity}, {"wallProbes", 400}, {"wallHits", wallCrossings}};
  for (auto &l : assets.levels)
    report["lods"].push_back({{"triangles", l.world.mesh.triangles.size()}, {"heightError", l.maximumHeightError}});
  if (argc > 1)
  {
    std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    for (size_t i = 0; i < frames.size(); ++i)
    {
      frames[i]["cameraAzimuthDegrees"] = -130 + int(i) * 20;
      std::ofstream(root / ("frame-" + std::to_string(i) + "-mesh.json")) << frames[i].dump(2);
    }
    std::ofstream(root / "water-probes.json") << waterProbes.dump();
    for (uint32_t lod = 0; lod < assets.levels.size(); ++lod)
    {
      RuntimeSnapshot display{1, std::make_shared<const RuntimeWorldAssets>(assets), std::vector<uint32_t>(8, lod)};
      std::ofstream(root / ("lod-" + std::to_string(lod) + "-mesh.json")) << render(display).dump();
    }
    json coverPrints = json::array();
    for (double footprint : {1., 2., 4.})
    {
      json pixels = json::array();
      for (int z = 0; z < 96; ++z)
        for (int x = 0; x < 128; ++x)
        {
          const double px = (x + .5) * .5, pz = (z + .5) * .5;
          CoverSample sample;
          if (!sampleRuntimeCover(assets, px, pz, pz >= 22 && pz <= 26 ? 1 : 0, footprint, sample, &error))
          {
            std::cerr << error;
            return 1;
          }
          pixels.push_back({sample.owner, sample.variation, sample.weights});
        }
      coverPrints.push_back({{"footprint", footprint}, {"width", 128}, {"height", 96}, {"pixels", pixels}});
    }
    std::ofstream(root / "cover-mip-probes.json") << coverPrints.dump();
    std::ofstream(root / "wall-probes.json") << wallProbes.dump(2);
    std::ofstream(root / "collision-probes.json") << collisionProbes.dump(2);
    std::ofstream(root / "collision-world.runtime.json") << collisionArchive;
    std::ofstream(root / "world.runtime.json") << archive;
    std::ofstream(root / "edited-world.runtime.json") << editArchive;
    std::ofstream(root / "selection.json") << saved;
    std::ofstream(root / "validation.json") << report.dump(2);
  }
  std::cout << "PASS Step17 runtime integration " << count << " assertions in " << seconds << "s\n";
}
