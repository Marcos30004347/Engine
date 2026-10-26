#pragma once

#include "procedural/Predicates.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Dendritic pattern field, after:
//
//   Gaillard, Benes, Guerin, Galin, Rohmer and Cani,
//   "Dendry: A Procedural Model for Dendritic Patterns", I3D 2019.
//
// PROVENANCE. This is an independent implementation written from the
// publication's description of the method. The authors' reference
// implementation at github.com/mgaillard/Noise is GPL-3.0 and was **not**
// fetched, read, copied or translated at any point; no line of it, and no
// element of its structure or naming, informed this file. What is reproduced
// here is the algorithm as described: a locally computable function returning
// the distance from a point to an implicitly constructed branching tree, driven
// by a global control function, refined by subdivision.
//
// Clean-room removes the licence obligation, not the academic one: cite
// Gaillard et al. 2019 wherever this is used. Terms in
// docs/map-generator-design.md §11.1.
//
// Where the publication's description leaves a choice open, the choice made
// here is stated in the implementation rather than left to be reverse
// engineered.

namespace procedural::world {

// The paper's `c`: a global control function over the plane, returning [0, 1].
// It "defines the overall shape and can be used to initialize local minima" -
// so for terrain, lower means lower ground, and the tree grows down it. Our map
// is this function.
using ControlFunction = std::function<double(Point2)>;

struct DendrySettings
{
  // The paper's branching level `n`. Each level halves the cell size and adds a
  // generation of tributaries.
  uint32_t levels = 4u;

  // Cell size of the coarsest level, in metres. With `levels`, this sets the
  // finest channel spacing: baseCellMetres / 2^(levels-1).
  double baseCellMetres = 24000.0;

  // The paper's seeding `s`, as a fraction of a cell: how far a node may sit
  // from its lattice position. Zero gives a visible grid.
  double jitter = 0.45;

  // The paper's disturbance `Δ`, applied to the control value. Halved at each
  // level, so it perturbs where a fine channel goes without moving a trunk.
  double disturbance = 0.10;

  // How strongly a finer level is drawn to the channels of the coarser one.
  // This is what makes the levels one tributary hierarchy rather than several
  // independent networks laid over each other, and it is the mechanism behind
  // "the overall shape is inherited through the iterations".
  double inheritance = 0.55;

  // Penalty on a tributary joining its trunk against the flow. Without it the
  // network joins at whatever angle the lattice offers and reads as a lattice.
  double angleWeight = 0.35;

  // The paper's local smoothing `ε`, in metres. Rounds the distance field near
  // a channel so a valley floor is not a crease.
  double smoothingMetres = 0.0;

  uint64_t seed = 0u;

  bool valid(std::string *error = nullptr) const;
};

// One node → parent link of the implicit tree, in world metres. The endpoint
// control values travel with it, because what a terrain wants from the tree is
// not only how far the nearest channel is but how low it lies.
struct DendrySegment
{
  Point2 from;
  Point2 to;
  double fromValue = 0.0;
  double toValue = 0.0;
  uint32_t level = 0u;
};

struct DendrySample
{
  // f(p) = d(p, T), the paper's field, in metres.
  double distanceMetres = 0.0;
  // Control value of the nearest point on the tree, interpolated along the
  // segment. A trunk sits at a low value and a twig at a high one, so a terrain
  // built on this gets deep valleys under its trunks and shallow notches under
  // its twigs - from one field, rather than from a separate depth heuristic.
  double channelValue = 0.0;
  uint32_t level = 0u;
};

// Evaluates f(p) = d(p, T) over a square tile.
//
// Tiles rather than points because the tree near a point is what costs: built
// once for a tile, every sample in it is a nearest-segment query. Point
// evaluation is the same code with a one-sample tile, so there is one
// implementation and not two that can drift.
//
// The result is **independent of how the domain is tiled** - the same world
// evaluated in one tile or in nine gives identical values - which is the
// property that lets a node be refined on demand without agreeing anything with
// its siblings. There is a test for exactly that.
class DendryField
{
public:
  DendryField(DendrySettings settings, ControlFunction control);

  // Samples an axis-aligned square, `resolution` by `resolution`, row 0 at the
  // *bottom* (world y increasing with row). Values are distances in metres.
  bool sample(Point2 origin, double sizeMetres, uint32_t resolution, std::vector<DendrySample> &out, std::string *error = nullptr) const;

  DendrySample sampleAt(Point2 position) const;
  double distanceAt(Point2 position) const;

  // The tree near a tile, for diagnostics and for drawing the network over a
  // map. Same construction the sampler uses.
  std::vector<DendrySegment> network(Point2 origin, double sizeMetres) const;

  const DendrySettings &settings() const
  {
    return settings_;
  }

private:
  struct Node
  {
    Point2 position;
    double value = 0.0;
    uint32_t parent = 0xffffffffu;
  };

  struct Build
  {
    std::vector<DendrySegment> segments;
    // Uniform grid over the segments, so a sample does not test them all.
    std::vector<uint32_t> bucketOffsets;
    std::vector<uint32_t> bucketEntries;
    Point2 gridOrigin;
    double gridCell = 0.0;
    uint32_t gridSide = 0u;
  };

  // Radius, in coarsest cells, of the neighbourhood a node's parent can depend
  // on. Everything a node needs - its own value, its neighbours' values, the
  // coarser tree those values read - lies inside this, so building it around a
  // tile reproduces the global answer exactly.
  static constexpr int32_t kDependencyCells = 3;

  Build build(Point2 origin, double sizeMetres) const;
  DendrySample nearest(const Build &build, Point2 position) const;

  DendrySettings settings_{};
  ControlFunction control_;
};

} // namespace procedural::world
