#pragma once

#include "procedural/world/hierarchy/Registry.hpp"

namespace procedural::world::hierarchy {

// The director.
//
// It is what was missing, and why every parent in the existing code loops over
// its children with static ::plan() calls instead of registering them: the
// typed director takes a fixed task list and infers dependencies before
// execution, so "one task per region, N of them discovered while running"
// cannot be expressed. Here the work list grows as regions appear.
//
// One generator per role per region, chosen by policy; the edges it proposes
// become the region's children; the children are queued for the next role.
// Nothing reads outside its own region, which is what makes the region the
// unit of parallelism once the context is value-owned.

struct Step
{
  Role role = Role::Districts;

  // Which regions this step applies to. A step runs on every region produced
  // by the previous one that passes this test; an empty test takes them all.
  std::function<bool(const Region &)> when;

  // Children below this area are not refined further by later steps.
  double minimumChildAreaMetres = 0;
};

struct Plan
{
  std::vector<Step> steps;
  Policy policy;
};

struct RunStats
{
  uint32_t regionsIn = 0;
  uint32_t regionsOut = 0;
  uint32_t edgesProposed = 0;
  uint32_t objectsPlaced = 0;
  uint32_t stepsRun = 0;

  // Per step, which generator ran on how many regions. What makes a render
  // explainable: "this ward is gridded because the merchant rule claimed it".
  std::vector<std::string> decisions;
};

class Composer
{
public:
  Composer(const Registry &registry, Context context) : registry_(registry), context_(std::move(context))
  {
  }

  // Runs the plan from `root` downward, adding every region and object it
  // produces to the tree. Returns false on a generator failure; regions
  // produced before the failure stay in the tree, and the ledger says where it
  // stopped.
  bool run(const Plan &, RegionTree &, RegionId root, Ledger &, RunStats &, std::string *error = nullptr);

  const std::vector<Object> &objects() const
  {
    return objects_;
  }

private:
  // Turns one generator's edge proposal into child regions: the edges enter an
  // arrangement together with the parent's own boundary, widthed edges become
  // ribbon regions of their own, and every remaining face becomes a child.
  bool subdivide(const Region &parent, const std::vector<Edge> &, RegionTree &, const std::string &producer, std::vector<RegionId> &children, Ledger &, std::string *error);

  const Registry &registry_;
  Context context_;
  std::vector<Object> objects_;
};

} // namespace procedural::world::hierarchy
