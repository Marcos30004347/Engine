#pragma once

#include "procedural/world/MapGraph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

// Shared weighted blending between neighbouring cells.
//
// Written once and used twice, deliberately: the map view colours a pixel by
// blending the cell type colours here, and the land generator later blends the
// per-cell-type height fields with the same weights. A blend implemented
// separately in each place is how the map view and the terrain end up
// disagreeing about where a boundary is.

struct BlendWeight
{
  uint32_t node = kInvalidNode;
  double weight = 0.0;
};

struct SiteHit
{
  uint32_t node = kInvalidNode;
  double distanceMetres = 0.0;
};

struct BlendSettings
{
  // Sites that can contribute to a sample. One more than this is always looked
  // up, because the extra one's distance is the support radius that makes the
  // weights fall to zero rather than jump when the contributor set changes.
  uint32_t neighbourCount = 4u;

  // Multiplies every transition's exponent. 1 is the tuned table; larger makes
  // the whole map crisper, which is what the app's "blend sharpness" slider
  // moves.
  double sharpnessScale = 1.0;
};

// Shapes an already continuous spatial influence field using terrain.
// This is deliberately separate from BlendSettings: the continuous elevation
// surface is built from the spatial weights first, then categorical materials
// can consult that surface without introducing a dependency cycle.
struct HeightBlendSettings
{
  // Deterministic per-cell dominance applied before the terrain gate. Zero
  // gives every source cell equal authority; one lets the strongest cell
  // carry twice the influence of the weakest at an otherwise equal sample.
  // This moves categorical thresholds away from uniform Voronoi midpoints
  // without changing the cells or softening the final edge.
  double cellWeightStrength = 0.55;
  // Zero preserves the original region influence; one applies the full height
  // gate before a categorical winner is thresholded.
  double influence = 0.85;
  // Local peak-to-trough variation, as fractions of the world's relief, over
  // which transitions change from broad to terrain-selected.
  double lowVariationFraction = 0.01;
  double highVariationFraction = 0.08;
  // Candidate cells whose intended elevation lies inside this fraction of the
  // relief remain compatible with the sampled terrain height.
  double elevationBandFraction = 0.06;
  // At high variation, concentrate spatial weights by up to this exponent.
  double maximumWeightExponent = 4.0;
};

// Exponent for the transition between two cell types. Higher is a harder edge.
//
// Symmetric by construction, and that is load-bearing rather than tidiness: the
// effective exponent at a sample is a bilinear form over the contributing
// weights, and an asymmetric table would make that form depend on which cell
// happened to be nearest, so the colour would jump as a sample crossed the
// midline between two cells.
double transitionSharpness(CellType first, CellType second);

// Uniform grid over the sites of one level, for nearest-site queries.
//
// Brute force is O(cells) per sample and the map view takes one sample per
// pixel over several thousand cells, which is seconds per frame. The grid makes
// it constant time.
class SiteIndex
{
public:
  bool build(
      const MapGraph &graph,
      uint8_t level,
      double extentMetres,
      std::string *error = nullptr);

  // The `count` nearest sites to `position`, nearest first. Ties break on node
  // id so the result does not depend on grid traversal order.
  void nearest(
      Point2 position, uint32_t count, std::vector<SiteHit> &out) const;

  size_t siteCount() const
  {
    return sites_.size();
  }
  bool empty() const
  {
    return sites_.empty();
  }

private:
  struct Site
  {
    Point2 position;
    uint32_t node = kInvalidNode;
  };

  uint32_t bucketIndex(const Point2 &position) const;

  std::vector<Site> sites_;
  // Node ids per grid cell, as a flat CSR-style array so a lookup does not walk
  // a vector of vectors.
  std::vector<uint32_t> bucketOffsets_;
  std::vector<uint32_t> bucketEntries_;
  uint32_t gridSide_ = 0u;
  double cellSize_ = 0.0;
  double extentMetres_ = 0.0;
};

// Weights of the cells contributing at a position. Sums to one, nearest cell
// heaviest, and continuous everywhere: a contributor's weight reaches zero
// exactly as it leaves the neighbour set.
//
// `out` is cleared and refilled, so a caller sampling per pixel can hand the
// same vector back every time and never allocate.
void blendAt(
    const MapGraph &graph,
    const SiteIndex &index,
    Point2 position,
    const BlendSettings &settings,
    std::vector<BlendWeight> &out);

// Gives every source cell a stable, seed-derived amount of authority over the
// categorical field. This reshapes the eventual threshold contour but remains
// continuous and normalized; it is deliberately not stored in the renderer or
// sampled from a texture, so CPU export and GPU display cannot disagree.
void applyCellInfluenceWeights(
    const MapGraph &graph,
    double strength,
    std::vector<BlendWeight> &weights);

// Keeps contributors on one side of the shoreline and renormalizes them. The
// nearest contributor on the requested surface becomes the fallback, preserving
// a categorical coast even when the support set is unusually sparse.
void restrictBlendToSurface(
    const MapGraph &graph,
    bool land,
    std::vector<BlendWeight> &weights);

// Reweights contributors in place. Flat ground follows the broad original
// region influence; where elevation changes strongly, contributors close to
// the sampled height win. A caller can then threshold the weights to one hard
// categorical land type. The result remains normalized and deterministic.
void shapeBlendByHeight(
    const MapGraph &graph,
    double elevationMetres,
    double localVariationMetres,
    double worldReliefMetres,
    const HeightBlendSettings &settings,
    std::vector<BlendWeight> &weights);

} // namespace procedural::world
