#pragma once

#include "procedural/world/WorldRuralPatches.hpp"
#include "procedural/world/rural/RuralGeometry.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace procedural::world::rural {

using rewrite::Position;
using rewrite::RuralPatchSettings;
using rewrite::RuralRegion;
using rewrite::RuralSource;
using rewrite::WorldSuitability;

// Deciding what a farm is.
//
// growRuralPatches was 1,072 lines, and a third of it was one
// `if (regions.empty())` against its `else`: two different answers to that
// question, chosen by whether the caller supplied the world's own cells.
//
// The same shape as the city's housing paths, and the same fix - they are
// strategies with a name each, not two arms of a branch.

// One farm claimed out of the analysis grid, before its fields and tracks
// exist.
//
// `cells` is what it holds; `visited` is what its growth front has already
// considered, so two farms competing outward cannot claim the same ground
// twice. `boundary` is its outline: the world's own cell for an imported
// region, and the hull of what the growth claimed otherwise.
struct Growth
{
  uint32_t seed = 0;
  uint64_t source = 0;
  double budget = 0;
  std::set<uint32_t> cells, visited;
  Poly boundary;
  uint64_t region = 0;
};

// Everything a growth strategy reads, and the two things it writes besides the
// farms themselves.
//
// `eligible`, `catchments` and `travelCost` are decided before either strategy
// runs, because both need them and neither should decide them: whether a cell
// may be farmed at all, which source it belongs to, and what it costs to reach.
struct GrowthContext
{
  const WorldSuitability &suitability;
  const RuralPatchSettings &settings;
  const std::vector<RuralSource> &sources;

  // The world's own cells, when the caller supplied them. Empty selects grid
  // growth instead.
  const std::vector<RuralRegion> &regions;

  // The product under construction. A strategy reads its per-cell verdicts -
  // which cells may be farmed, which source each belongs to, what each costs
  // to reach - and charges each farm it grows against its source's budget.
  rewrite::RuralPatchProduct &product;

  // Distance in cells from the nearest ineligible ground, which is what keeps
  // a farm from being grown as a sliver along an edge.
  const std::vector<uint32_t> &clearance;

  // Cells inside a caller reservation: never farmed, and never crossed by a
  // track either.
  const std::vector<uint8_t> &reservedCell;

  // Half the analysis cell spacing, in metres.
  double halfSpacing = 0;

  // Which farm owns each cell, written as the strategies claim ground.
  std::vector<uint32_t> &owner;
};

// One way of deciding what a farm is.
class GrowthStrategy
{
public:
  virtual ~GrowthStrategy() = default;

  virtual const char *name() const = 0;

  // Appends the farms it grew. A hinterland that cannot hold another farm is a
  // full hinterland, not an error: `false` means a budget was exceeded or the
  // input was invalid, nothing else.
  virtual bool grow(const GrowthContext &, std::vector<Growth> &out, std::string *error) = 0;
};

using GrowthStrategyPtr = std::shared_ptr<GrowthStrategy>;

// Seeds a bounded number of broad cores and lets them compete outward.
//
// Used when the caller supplied no cells of its own. Farms come out roughly
// even in size and separated by the configured spacing - a pattern rather than
// a map, which is what you want when there is no map to follow.
class GridGrowthStrategy final : public GrowthStrategy
{
public:
  const char *name() const override
  {
    return "rural.grid";
  }

  bool grow(const GrowthContext &, std::vector<Growth> &, std::string *error) override;
};

// Makes each supplied world cell a farm.
//
// Whole source cells form adjoining countryside with no artificial spacing
// between estates; the grid only audits the terrain and supplies access paths.
// This is what keeps a holding's boundary on the world's own cell edge, so a
// field and the map agree about where it ends.
class RegionGrowthStrategy final : public GrowthStrategy
{
public:
  const char *name() const override
  {
    return "rural.regions";
  }

  bool grow(const GrowthContext &, std::vector<Growth> &, std::string *error) override;
};

// Picks the strategy the context calls for and runs it.
bool growPatches(const GrowthContext &, std::vector<Growth> &out, std::string *error);

} // namespace procedural::world::rural
