#pragma once
#include "WorldSuitability.hpp"
#include "WorldSurfaceArrangement.hpp"

namespace procedural::world::rewrite {
struct RuralSource
{
  uint64_t id = 0;
  Position exit;
  double areaBudget = 20000;
};
enum class RuralLandCover
{
  Open,
  Forest,
  Rock,
  Water
};

struct RuralRegion
{
  uint64_t id = 0;
  std::vector<Position> boundary;
  RuralLandCover cover = RuralLandCover::Open;
  // Reserved open land (a suburb belt beyond a town wall): never farmed, but
  // its boundary still carries lanes to the farms beyond it.
  bool reserved = false;
};

struct RuralPatchSettings
{
  // Optional original world Voronoi cells. Preserve their boundaries; never regrow them on the analysis grid.
  std::vector<RuralRegion> regions;
  uint64_t seed = 7;
  double minimumWidth = 24, minimumArea = 1200, maximumArea = 6000;
  double maximumAccessCost = 800, patchSpacing = 16, trackWidth = 3;
  uint32_t maximumPatches = 32;
  bool partialRegions = false; // A region with too many unsuitable cells is farmed on its suitable part (hull of the largest eligible group) instead of dropped.
};

struct AgriculturalPatch
{
  uint64_t id = 0, source = 0, originalRegion = 0;
  std::vector<uint32_t> cells, accessCells;
  std::vector<Position> boundary, access;
  uint32_t boundaryAccessStart = 0; // Imported mode: first boundary-network point after source approach.
  double area = 0, meanSuitability = 0, accessCost = 0;
};

struct RuralBudget
{
  uint64_t source = 0;
  double requested = 0, allocated = 0, shortfall = 0;
};

struct RuralPatchProduct
{
  uint64_t suitabilityFingerprint = 0;
  std::vector<AgriculturalPatch> patches;
  std::vector<RuralBudget> budgets;
  std::vector<std::string> rejections;
  std::vector<double> travelCost;
  std::vector<uint64_t> catchments;
  std::vector<uint8_t> eligible;
  std::vector<SurfaceContour> contours; // Patch masks then track reservations; no crop fields yet.
  double eligibleArea = 0, patchArea = 0, unallocatedArea = 0;
};

// WorldSuitability is the frozen terrain/soil/water input. Additional convex
// reservations exclude their whole intersected analysis cells from growth AND
// travel (urban envelope, infrastructure, protected water, cliffs). Exits must
// lie outside them. Track ribbons follow terrain; road engineering is downstream.
// Without supplied regions, broad grid kernels preserve width. With original
// regions, whole eligible convex cells are retained; adjacent cells share edges.
// Their analysis-cell lists may overlap and never define ownership geometry.
// Boundary lanes preserve those edges; source approaches are explicit and cannot
// cross allocated interiors. Protected cover excludes both farming and travel.
// Bounded conservative allocation reports shortfall; invalid input fails atomically.
bool growRuralPatches(const WorldSuitability &, const std::vector<SurfaceContour> &reservations, const std::vector<RuralSource> &, const RuralPatchSettings &, RuralPatchProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
