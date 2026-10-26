#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>
#include <vector>

namespace procedural::world
{

struct ElevationSettings
{
  // Datum and total relief budget, normally taken from the brief. Generators
  // scale against these rather than carrying absolute constants, so a world can
  // be made flatter without retuning every stage.
  double seaLevelMetres = 0.0;
  double reliefMetres = 3000.0;
  double extentMetres = 361000.0;

  // Share of the summit budget used by broad plains, plateaus and range
  // flanks. The remaining height is reserved for the cores of authored ranges,
  // allowing tall summits without raising the whole country with them.
  double broadReliefFraction = 1.0;

  // Fraction of the relief budget the ranges carry; the remainder is the broad
  // rise from the coast inland. At 0 the world is a tilted plain.
  double rangeStrength = 0.55;

  // Ranges per landmass, and how far a landmass has to be to earn more than
  // one. Ranges used to be one per 400 cells with no ceiling, which on a large
  // continent grew a range system that filled it - the map was mountain from
  // coast to coast and there was nowhere for a plain to be.
  uint32_t maximumRangesPerLandmass = 2u;
  double cellsPerRange = 900.0;

  // Orogenic plates: a diagram of its own whose seams are the ranges.
  //
  // Ranges used to sit on the seams between the blocks the *landmass* grew
  // from, which was geologically right and practically wrong: that diagram also
  // controls whether the land holds together, so spreading its blocks to spread
  // the ranges fragmented the continent into lobes with inland seas - 5 lakes
  // became 120. Range layout and land cohesion are different questions and now
  // have different diagrams.
  //
  // The spacing is how far apart the ranges are, which is the dial that says
  // "more mountain ranges".
  double orogenSpacingMetres = 220000.0;
  // Fraction of plate pairs whose seam is a range. Marking every seam draws a
  // closed loop around each plate and walls the interior into pockets.
  double orogenFraction = 0.55;

  // How far a range's flank reaches from its spine, in metres. This is the one
  // number that decides whether the world reads as a few big cordilleras or as
  // scattered massifs.
  double rangeWidthMetres = 22000.0;

  // Distance inland over which the coastal plain rises to full height.
  double coastalPlainMetres = 60000.0;

  // Large landform provinces break the old coast-to-centre elevation ramp into
  // coherent areas with different character. Plains stay low and quiet,
  // rolling country carries modest relief, plateaus are high but locally flat,
  // and highlands amplify ranges. The fractions are draws per province; any
  // remainder is rolling country.
  double landformSpacingMetres = 140000.0;
  double landformEdgeMetres = 18000.0;
  double plainProvinceFraction = 0.42;
  double plateauProvinceFraction = 0.20;
  double highlandProvinceFraction = 0.18;

  // Zero makes every province use the same neutral profile; one applies the
  // full differences above. This is an art-direction control, not noise
  // amplitude: province sites and types remain deterministic.
  double landformContrast = 0.80;

  // Peak uplift, metres per year, at a range spine. Real orogenic rates are
  // 0.1-10 mm/yr; 1 mm/yr is a mature range. Carried so a later erosion pass
  // has something to push against - elevation alone lets erosion flatten
  // everything the layout asked for.
  double upliftRateMetresPerYear = 1.0e-3;

  // Continental shelf: how deep and how far out before the abyssal floor.
  double shelfDepthFraction = 0.06;
  double abyssalDepthFraction = 0.30;
  double shelfWidthMetres = 50000.0;

  // Sub-range roughness, as a fraction of the relief budget, and its wavelength
  // in metres. §5.3's first legitimate use of noise: seeding the surface so
  // structure has something to sit on.
  //
  // Kept small, and deliberately not used as a drainage dial. Sweeping it from
  // 0.04 to 0.40 across three wavelengths on a 3,003 cell world moved the river
  // mouth count by less than 10% and the Strahler order not at all; what it did
  // do was manufacture depressions, 0 pits at 0.04 and 42 at 0.40. Drainage
  // depth is set by how much of the network the cell size resolves, not by how
  // rough the surface is - see the notes on channel threshold in the spec.
  double detailReliefFraction = 0.05;
  double detailWavelengthMetres = 22000.0;

  // Tectonic blocks: a Voronoi diagram of its own, at its own scale, whose
  // cells are raised or lowered.
  //
  // This is where basins and plateaus come from, and it replaced placing them
  // as radial features. Two reasons. The elevation this stage otherwise builds
  // is *monotone* - a sum of distance-to-coast and distance-to-spine terms -
  // so it has no interior minimum anywhere, no depression to fill and therefore
  // no lakes. And a radial feature is a circle: stamping bowls and mesas
  // produced round basins and round plateaus, which is not what either looks
  // like. A block diagram gives them polygon edges and, where two blocks differ,
  // an escarpment - which is what a plateau edge actually is.
  //
  // The diagram is independent of the cell hierarchy on purpose, so tectonic
  // boundaries do not fall on province boundaries.
  // A continent has a handful of tectonic blocks, not hundreds. At 55 km this
  // was a block every 55 km whatever the world's size, so a bigger continent
  // got proportionally more subsided blocks and therefore more lakes -
  // measured at 0, 42 and 140 lakes for continents of 700, 950 and 1,500 km.
  // It is a length and it scales with the world.
  double blockSpacingMetres = 200000.0;
  // How far a block may move, as a fraction of the relief budget. A closed
  // basin needs the block to move further than the ground already falls across
  // it: at 70 km spacing and a regional slope near 6 m/km that is about 400 m,
  // so this has to be comfortably more.
  double blockReliefFraction = 0.30;
  // Width of the transition at a block boundary. Narrow is an escarpment; wide
  // is a swell.
  // At 14 km against a 70 km spacing most of every block was in transition and
  // the field came out as one broad swell rather than as blocks. A few cells is
  // an escarpment.
  double blockEdgeMetres = 5000.0;

  // A cell on the high side of a macro-height discontinuity is a cliff when
  // both its drop, as a share of the world's relief budget, and its physical
  // gradient clear these thresholds. Requiring both keeps small rough patches
  // and long mountain flanks from being mislabeled as escarpments.
  double cliffMinimumDropFraction = 0.06;
  double cliffMinimumGradient = 0.08;

  // Positive tilts the draw toward subsidence, which makes basin-and-range
  // country; negative makes plateau country.
  double blockSubsidenceBias = 0.0;

  // Basins are shallower than plateaus are high. Not symmetry for its own sake:
  // a block that subsides fills with sediment, and one that rises does not, so
  // real basins are shallow relative to the ranges beside them. It is also what
  // stops every subsided block closing into a lake.
  double blockBasinFraction = 0.45;

  double roughness = 0.5;
  uint64_t seed = 0u;
};

// Splits water into ocean and lakes.
//
// Water that cannot reach the world border by walking water neighbours is
// enclosed, and enclosed water is a lake. Returns the number of cells
// reclassified. Idempotent, and safe to call before elevation exists: it is
// pure topology.
uint32_t classifyWaterBodies(MapGraph &graph, double extentMetres);

// Stage 5: elevation and uplift intent, and the landform cell types that follow
// from them.
//
// Structure comes from the layout, never from noise. Range spines are laid out
// explicitly on selected orogenic seams. An independent coarse diagram assigns
// plain, rolling, plateau and highland character, so broad elevation stops being
// a single distance-to-coast ramp. Noise only modulates flank width and
// sub-range detail; it cannot place a range or choose a landform province.
//
// Assigns, for every cell: `intent` (elevation, uplift, relief amplitude,
// roughness) and `type` (Coast / Plain / Hill / Mountain / Plateau / Cliff for
// land, Ocean / Lake for water). Valley and Canyon are assigned later, by the
// river stage, because both are measured from a channel and its banks.
bool assignElevation(
    const std::vector<Tag> &tags,
    const ElevationSettings &settings,
    MapGraph &graph,
    std::string *error = nullptr);

// Metric distance from each land cell to the nearest water cell, along the
// neighbour graph. Exposed because the region and settlement stages want the
// same field, and because it is the clearest thing to assert about in a test.
// Water cells get 0; unreachable cells get infinity.
std::vector<double> coastDistances(const MapGraph &graph);

// Signed area of a cell polygon, in square metres. Always positive; the sign of
// the winding is not the caller's problem.
double cellArea(const MapNode &node);

} // namespace procedural::world
