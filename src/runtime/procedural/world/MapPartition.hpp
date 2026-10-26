#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>

namespace procedural::world {

struct PartitionSettings
{
  // Target spacing between cell sites, in metres. This is the single number
  // that sets the map's resolution: at 361 km extent, 5000 m gives roughly
  // 5200 cells, which is the L1 "region" scale.
  double cellSpacingMetres = 5000.0;

  // Area to partition, in metres, as a square from the origin. Normally the
  // brief's extent; refinement passes a node's bounding box instead.
  double extentMetres = 361000.0;

  uint64_t seed = 0u;
};

// A Voronoi cell before it becomes a graph node. Neighbours are indices into
// the returned vector.
struct VoronoiCell
{
  Point2 site;
  std::vector<Point2> area;
  std::vector<uint32_t> neighbours;
};

// Voronoi cells for an explicit site set, clipped to a convex window.
//
// Split out of buildPartition because the hierarchy needs exactly this: a
// partition of one parent cell from sites the parent chose. One implementation,
// so a level built inside a parent and a level built over the world cannot
// disagree about what a Voronoi cell is.
bool buildVoronoiCells(const std::vector<Point2> &sites, const std::vector<Point2> &window, double toleranceMetres, std::vector<VoronoiCell> &out, std::string *error = nullptr);

// Builds the Voronoi cell partition for one level.
//
// Poisson-disk sites -> Delaunay triangulation -> Voronoi dual. Cells are
// produced as polygons in metres with symmetric neighbour links, ready for a
// raiser to classify.
//
// Sampling deliberately uses PoissonDiskSampler::Method::RandomThrows. That
// method is bit-identical across compiler settings; FillVoids constructs sample
// positions from circle intersections and square roots and is not reproducible
// across targets - measured at 454/448/425 samples under three settings in that
// file's own notes. Determinism is a gate for this system, so the choice is
// load-bearing rather than a default.
bool buildPartition(const PartitionSettings &settings, MapGraph &graph, std::string *error = nullptr);

} // namespace procedural::world
