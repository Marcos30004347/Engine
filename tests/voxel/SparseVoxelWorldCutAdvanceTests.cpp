// Incremental publication of a generated-world spatial cut.
//
// The cut is a partition: every point inside it has exactly one owner. Whatever
// the streamer publishes while a replacement is only partly built has to keep
// that true, or the frame shows a hole where nothing owns an area or a
// double-draw where a page and its descendants both do.
//
// The properties asserted here are the ones the old all-or-nothing barrier got
// for free by never publishing anything intermediate.

#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{

int failures = 0;
constexpr double kBaseEdge = 12.8;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

struct Box
{
  double minimumX = 0.0;
  double minimumZ = 0.0;
  double edge = 0.0;
};

Box boxOf(const svr::WorldVoxelChunkAddress &address)
{
  const double edge = std::ldexp(kBaseEdge, static_cast<int>(address.spatialLOD));
  return {
    static_cast<double>(address.x) * edge,
    static_cast<double>(address.z) * edge, edge};
}

bool overlaps(
    const svr::WorldVoxelChunkAddress &left,
    const svr::WorldVoxelChunkAddress &right)
{
  const Box a = boxOf(left);
  const Box b = boxOf(right);
  const double tolerance = std::max(a.edge, b.edge) * 1.0e-9;
  return a.minimumX < b.minimumX + b.edge - tolerance &&
      b.minimumX < a.minimumX + a.edge - tolerance &&
      a.minimumZ < b.minimumZ + b.edge - tolerance &&
      b.minimumZ < a.minimumZ + a.edge - tolerance;
}

// No two owners may overlap. This is the double-draw check.
bool disjoint(const std::vector<svr::WorldVoxelChunkAddress> &cut)
{
  for (size_t left = 0u; left < cut.size(); ++left)
    for (size_t right = left + 1u; right < cut.size(); ++right)
      if (overlaps(cut[left], cut[right]))
        return false;
  return true;
}

// Everything the reference cut covers must still have an owner. This is the
// hole check: it samples the centre of every reference page, which is enough
// because owners are axis-aligned boxes on a shared quadtree lattice.
bool covers(
    const std::vector<svr::WorldVoxelChunkAddress> &cut,
    const std::vector<svr::WorldVoxelChunkAddress> &reference)
{
  for (const auto &page : reference)
  {
    const Box target = boxOf(page);
    const double centreX = target.minimumX + target.edge * 0.5;
    const double centreZ = target.minimumZ + target.edge * 0.5;
    bool owned = false;
    for (const auto &owner : cut)
    {
      const Box box = boxOf(owner);
      if (centreX >= box.minimumX && centreX < box.minimumX + box.edge &&
          centreZ >= box.minimumZ && centreZ < box.minimumZ + box.edge)
      {
        owned = true;
        break;
      }
    }
    if (!owned)
      return false;
  }
  return true;
}

bool faceAdjacent(
    const svr::WorldVoxelChunkAddress &left,
    const svr::WorldVoxelChunkAddress &right)
{
  const Box a = boxOf(left);
  const Box b = boxOf(right);
  const double tolerance = std::max(a.edge, b.edge) * 1.0e-9;
  const bool overlapX = a.minimumX < b.minimumX + b.edge - tolerance &&
      b.minimumX < a.minimumX + a.edge - tolerance;
  const bool overlapZ = a.minimumZ < b.minimumZ + b.edge - tolerance &&
      b.minimumZ < a.minimumZ + a.edge - tolerance;
  const bool touchX = std::abs(a.minimumX + a.edge - b.minimumX) <= tolerance ||
      std::abs(b.minimumX + b.edge - a.minimumX) <= tolerance;
  const bool touchZ = std::abs(a.minimumZ + a.edge - b.minimumZ) <= tolerance ||
      std::abs(b.minimumZ + b.edge - a.minimumZ) <= tolerance;
  return (touchX && overlapZ) || (touchZ && overlapX);
}

bool balanced(const std::vector<svr::WorldVoxelChunkAddress> &cut)
{
  for (size_t left = 0u; left < cut.size(); ++left)
    for (size_t right = left + 1u; right < cut.size(); ++right)
    {
      const uint32_t delta = cut[left].spatialLOD > cut[right].spatialLOD
          ? cut[left].spatialLOD - cut[right].spatialLOD
          : cut[right].spatialLOD - cut[left].spatialLOD;
      if (delta > 1u && faceAdjacent(cut[left], cut[right]))
        return false;
    }
  return true;
}

bool sameSet(
    std::vector<svr::WorldVoxelChunkAddress> left,
    std::vector<svr::WorldVoxelChunkAddress> right)
{
  const auto order = [](const svr::WorldVoxelChunkAddress &a,
                        const svr::WorldVoxelChunkAddress &b)
  {
    if (a.spatialLOD != b.spatialLOD)
      return a.spatialLOD < b.spatialLOD;
    if (a.x != b.x)
      return a.x < b.x;
    return a.z < b.z;
  };
  std::sort(left.begin(), left.end(), order);
  std::sort(right.begin(), right.end(), order);
  if (left.size() != right.size())
    return false;
  for (size_t index = 0u; index < left.size(); ++index)
    if (!(left[index] == right[index]))
      return false;
  return true;
}

std::vector<svr::WorldVoxelChunkAddress> childrenOf(
    const svr::WorldVoxelChunkAddress &page)
{
  std::vector<svr::WorldVoxelChunkAddress> children;
  for (int32_t z = 0; z != 2; ++z)
    for (int32_t x = 0; x != 2; ++x)
      children.push_back({page.x * 2 + x, page.z * 2 + z, page.spatialLOD - 1u});
  return children;
}

} // namespace

int main()
{
  std::cout << "generated-world incremental cut publication\n";

  for (const uint32_t residentCount : {486u, 489u})
  {
    std::vector<svr::WorldVoxelChunkAddress> resident;
    for (uint32_t i = 0u; i < residentCount; ++i)
      resident.push_back({static_cast<int32_t>(i), 0, 1u});
    auto target = resident;
    target.erase(target.begin(), target.begin() + 8);
    for (uint32_t i = 0u; i < 8u; ++i)
      for (const auto &child : childrenOf(resident[i]))
        target.push_back(child);
    const auto admitted = svr::limitWorldVoxelCutToCapacity(
        resident, target, {}, kBaseEdge, 512u);
    const uint32_t fittingGroups = (512u - residentCount) / 4u;
    check(admitted.size() == residentCount + fittingGroups * 3u,
          "capacity admits fitting sibling groups instead of rejecting the batch");
    check(disjoint(admitted) && covers(admitted, target) && balanced(admitted),
          "capacity-limited refinement keeps a complete balanced cut");
    check(std::find(admitted.begin(), admitted.end(), resident[0]) == admitted.end(),
          "highest-priority requested group advances first");
    auto reversedPriority = target;
    std::reverse(reversedPriority.begin(), reversedPriority.end());
    const auto ranked = svr::limitWorldVoxelCutToCapacity(
        resident, reversedPriority, {}, kBaseEdge, 512u);
    const auto firstNew = std::find_if(ranked.begin(), ranked.end(),
        [&](const auto &page)
        { return std::find(resident.begin(), resident.end(), page) == resident.end(); });
    check(firstNew != ranked.end() && *firstNew == reversedPriority.front(),
          "capacity closure preserves demand order for bounded generation");
    const auto full = svr::limitWorldVoxelCutToCapacity(
        resident, target, {}, kBaseEdge, residentCount);
    check(sameSet(full, resident), "a full pool retains its drawable owners");
  }

  {
    const std::vector<svr::WorldVoxelChunkAddress> parent{{0, 0, 1u}};
    const auto children = childrenOf(parent[0]);
    const auto partial = svr::limitWorldVoxelCutToCapacity(
        parent, children, {children[0]}, kBaseEdge, 4u);
    check(sameSet(partial, parent), "staged children consume transition slots");
    const auto complete = svr::limitWorldVoxelCutToCapacity(
        parent, children, {children[0]}, kBaseEdge, 5u);
    check(sameSet(complete, children), "a staged group completes when its remaining children fit");
    const auto merge = svr::limitWorldVoxelCutToCapacity(
        children, parent, {}, kBaseEdge, 5u);
    check(sameSet(merge, parent), "coarsening reserves the new parent before retiring children");
  }

  {
    const std::vector<svr::WorldVoxelChunkAddress> resident{{1, 0, 1u}, {1, 0, 2u}};
    auto target = childrenOf(resident[0]);
    const auto neighbours = childrenOf(resident[1]);
    target.insert(target.end(), neighbours.begin(), neighbours.end());
    const auto cut = svr::limitWorldVoxelCutToCapacity(
        resident, target, {}, kBaseEdge, 6u);
    check(balanced(target), "capacity balance fixture has a balanced requested cut");
    check(balanced(cut) && disjoint(cut) && covers(cut, target),
          "capacity holds a split when its balancing neighbour cannot fit");
    auto feedback = childrenOf(resident[0]);
    feedback.push_back(resident[1]);
    const auto fitting = svr::limitWorldVoxelCutToCapacity(
        resident, feedback, {}, kBaseEdge, 6u);
    check(sameSet(fitting, feedback),
          "capacity does not reject affordable feedback while neighbours are still converging");
  }

  // A 2x2 block of LOD 1 pages, each requested at LOD 0.
  const std::vector<svr::WorldVoxelChunkAddress> published{
    {0, 0, 1u}, {1, 0, 1u}, {0, 1, 1u}, {1, 1, 1u}};
  std::vector<svr::WorldVoxelChunkAddress> requested;
  for (const auto &page : published)
    for (const auto &child : childrenOf(page))
      requested.push_back(child);

  // Startup may advance three levels at once. Verify that this optimization
  // reaches the requested depth without weakening the ownership invariants.
  const std::vector<svr::WorldVoxelChunkAddress> deepPublished{{0, 0, 3u}};
  std::vector<svr::WorldVoxelChunkAddress> threeLevelRequested =
      deepPublished;
  for (uint32_t level = 0u; level < 3u; ++level)
  {
    std::vector<svr::WorldVoxelChunkAddress> children;
    for (const auto &page : threeLevelRequested)
      for (const auto &child : childrenOf(page))
        children.push_back(child);
    threeLevelRequested = std::move(children);
  }
  const auto threeLevelCut = svr::clampWorldVoxelCutToRefinementLevels(
      deepPublished, threeLevelRequested, 3u);
  check(
      sameSet(threeLevelCut, threeLevelRequested),
      "startup convergence advances three spatial levels in one cut");
  check(
      disjoint(threeLevelCut) && covers(threeLevelCut, deepPublished) &&
          balanced(threeLevelCut),
      "a three-level startup cut stays complete, disjoint, and 2:1 balanced");
  const auto oneLevelCut = svr::clampWorldVoxelCutToOneLevel(
      deepPublished, threeLevelRequested);
  check(
      oneLevelCut.size() == 4u &&
          std::all_of(
              oneLevelCut.begin(), oneLevelCut.end(),
              [](const auto &page) { return page.spatialLOD == 2u; }),
      "steady-state convergence remains limited to one spatial level");

  uint32_t advanced = 0u;
  uint32_t held = 0u;

  // Nothing staged: the published cut has to survive untouched.
  auto cut = svr::selectPublishableWorldVoxelCut(
      published, requested, {}, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, published) && advanced == 0u && held == 4u,
      "with nothing staged the published cut is kept whole");

  // One group fully staged: it advances, the other three stay. This is the
  // case the old barrier refused, and it is what makes refinement incremental.
  auto oneGroup = childrenOf(published[0]);
  cut = svr::selectPublishableWorldVoxelCut(
      published, requested, oneGroup, kBaseEdge, &advanced, &held);
  check(advanced == 1u && held == 3u, "a fully staged group advances alone");
  check(cut.size() == 3u + 4u, "the advanced group replaced one page with four");
  check(disjoint(cut), "the partly advanced cut has no overlapping owners");
  check(covers(cut, requested), "the partly advanced cut leaves no area unowned");
  check(balanced(cut), "the partly advanced cut stays 2:1 balanced");

  // A partially staged group must not advance: publishing two of four children
  // would leave the other half of the parent with no owner at all.
  std::vector<svr::WorldVoxelChunkAddress> halfGroup{oneGroup[0], oneGroup[1]};
  cut = svr::selectPublishableWorldVoxelCut(
      published, requested, halfGroup, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, published) && advanced == 0u,
      "a half-staged group does not advance");

  // Everything staged: the cut becomes the requested one.
  cut = svr::selectPublishableWorldVoxelCut(
      published, requested, requested, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, requested) && advanced == 4u && held == 0u,
      "with everything staged the requested cut is published");
  check(disjoint(cut) && covers(cut, requested), "the final cut is a partition");

  // Coarsening: four published pages absorbed by one requested parent. The
  // group only advances when the parent itself is staged.
  const std::vector<svr::WorldVoxelChunkAddress> fine = childrenOf({0, 0, 1u});
  const std::vector<svr::WorldVoxelChunkAddress> coarse{{0, 0, 1u}};
  cut = svr::selectPublishableWorldVoxelCut(
      fine, coarse, {}, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, fine) && advanced == 0u,
      "a coarsening group holds until its parent is staged");
  cut = svr::selectPublishableWorldVoxelCut(
      fine, coarse, coarse, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, coarse) && advanced == 1u,
      "a coarsening group advances when its parent is staged");
  check(disjoint(cut) && covers(cut, coarse), "the coarsened cut is a partition");

  // New coverage entering render distance has no published counterpart and must
  // not be dropped for that reason.
  const std::vector<svr::WorldVoxelChunkAddress> entering{
    {0, 0, 1u}, {2, 0, 1u}};
  cut = svr::selectPublishableWorldVoxelCut(
      {{0, 0, 1u}}, entering, {{2, 0, 1u}}, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, entering),
      "a requested page with no published counterpart is added when staged");
  cut = svr::selectPublishableWorldVoxelCut(
      {{0, 0, 1u}}, entering, {}, kBaseEdge, &advanced, &held);
  check(
      sameSet(cut, {{0, 0, 1u}}),
      "an unstaged new page is not published early");

  // Balance closure. A row of LOD 2 pages with the middle one requested two
  // levels finer: advancing it alone would leave a two-level step against both
  // neighbours, which is the seam the requested cut is balanced to avoid.
  const std::vector<svr::WorldVoxelChunkAddress> row{
    {0, 0, 2u}, {1, 0, 2u}, {2, 0, 2u}};
  std::vector<svr::WorldVoxelChunkAddress> deepRequest{{0, 0, 2u}, {2, 0, 2u}};
  for (const auto &child : childrenOf({1, 0, 2u}))
    for (const auto &grandchild : childrenOf(child))
      deepRequest.push_back(grandchild);
  cut = svr::selectPublishableWorldVoxelCut(
      row, deepRequest, deepRequest, kBaseEdge, &advanced, &held);
  check(
      balanced(cut),
      "a commit that would break the 2:1 balance is held back");
  check(
      disjoint(cut) && covers(cut, row),
      "the balance-held cut is still a partition");

  // Degenerate inputs must not lose the published cut.
  check(
      sameSet(
          svr::selectPublishableWorldVoxelCut(
              published, {}, {}, kBaseEdge, &advanced, &held),
          published),
      "an empty requested cut keeps the published one");
  check(
      svr::selectPublishableWorldVoxelCut({}, requested, requested, kBaseEdge)
          .empty(),
      "an empty published cut publishes nothing on its own");
  check(
      sameSet(
          svr::selectPublishableWorldVoxelCut(
              published, requested, requested, 0.0, &advanced, &held),
          published),
      "an invalid page edge keeps the published cut rather than guessing");

  return failures == 0 ? 0 : 1;
}
