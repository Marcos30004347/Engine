#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>
#include <vector>

namespace procedural::world
{

struct HierarchySettings
{
  // Cell spacing per level, coarsest first, in metres. Each entry is its own
  // Voronoi partition at its own scale; level k's cells are cut inside level
  // k-1's, so the levels nest exactly rather than merely looking similar.
  //
  // The default is the ~50 km / ~12 km / ~4 km ladder: the coarsest is the
  // scale a realm or a province is drawn at, the middle one a district, the
  // finest the terrain cell everything downstream measures. Ratios matter more
  // than absolutes - each step is about four, which is enough that a parent has
  // a dozen or so children rather than three.
  std::vector<double> cellSpacingMetres{50000.0, 12000.0, 4000.0};

  double extentMetres = 361000.0;
  uint64_t seed = 0u;
};

// Builds the whole nested partition: level 0 over the world, then each deeper
// level inside its parent's polygon.
//
// A child level is generated **per parent**, from sites the parent's own seed
// chose and clipped to the parent's polygon. Two consequences, both wanted:
//
// - The children of a cell tile it exactly, so the levels are a true hierarchy
//   and not three partitions that happen to overlap.
// - The parent owns every shared boundary, which is the contract §3.3 requires
//   and which refinement was always going to need. A child never has to agree
//   anything with a child of another parent, because the boundary between them
//   was decided one level up.
//
// Refinement of a node is therefore a pure function of (node, seed) - the
// property that lets a node be refined on demand, in any order, on any machine.
bool buildHierarchy(
    const HierarchySettings &settings,
    MapGraph &graph,
    std::string *error = nullptr);

// Refines one leaf into a finer partition, in place. Same construction the
// eager builder uses, so refining on demand and building the whole ladder up
// front produce identical cells.
bool refineNode(
    uint32_t node,
    double cellSpacingMetres,
    uint64_t seed,
    MapGraph &graph,
    std::string *error = nullptr);

// Fills every refined node from its children: land flag, dominant landform and
// content, mean elevation and uplift, coastal and shallow flags.
//
// This is what makes a coarse cell a *place* rather than a bookkeeping node.
// Picking a huge section of land and calling it a kingdom means picking coarse
// cells, and that only works if a coarse cell knows whether it is land, how
// high it stands and what is on it.
//
// Called after the leaf stages, since it summarises what they decided.
void aggregateToParents(MapGraph &graph);

// Area-weighted share of a node's leaves that are land. Meaningful on a refined
// node; 0 or 1 on a leaf.
double landFractionOf(const MapGraph &graph, uint32_t node);

// Leaves of the hierarchy: the cells that carry terrain. Every stage after the
// partition works on these, because a refined cell is a container and not a
// place.
std::vector<uint32_t> leafNodes(const MapGraph &graph);

} // namespace procedural::world
