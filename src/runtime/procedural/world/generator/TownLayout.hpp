#pragma once

#include "Generator.hpp"
#include "TownBlocks.hpp"
#include "TownStreets.hpp"
#include "procedural/world/WorldCityLayout.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// The town, Watabou's Model.build in four stages, each its own generator:
// streets (TownStreets), the wall sections and towers, blocks (TownBlocks)
// and buildings (WardBuildings, or RingLotPlanner for a court ward). The
// result is the CityLayout the fabric, the director and the preview read:
// streets, walls, towers, blocks with their buildings as lots, wards, the
// ground and road contours for the world builder.
struct TownLayoutInput
{
  std::vector<TownPatch> patches;
  ConvexRing border;           // The wall ring (walled) or the core circumference.
  std::vector<Position> gates; // On the border.
  std::vector<Position> exits; // Per gate (optional).
  Position centre;
  std::vector<ConvexRing> compounds;                // Reserved compounds (the plaza's own patch is a patch, not a compound).
  std::vector<rewrite::CityPlanDistrict> districts; // Passed through to the layout.
  bool walled = true;
  double wallSectionMetres = 75, wallTowerRadius = 2, wallTowerHeight = 7, wallSetback = 6;
  double arteryHalfWidth = 2.5;

  // How much arc one facet of a street bend covers. A street is built, not
  // drawn: below about a metre per facet the extra vertices only cost the
  // ribbon, the block cuts and the lot reservations that follow them.
  double bendFacetMetres = 1.6;
  double mainStreet = 5, regularStreet = 2.5, alley = 1.5;
  // How big a block may be before a street is cut through it. A block is
  // the land between streets now, so these are the town's street spacing:
  // at 1800 m2 and 60 m a block held four rows of houses and the middle two
  // had no frontage at all.
  double maximumInteriorBlockArea = 1100, maximumInteriorBlockSpan = 46;
  double unit = 6.25;             // Watabou's square unit in m2 (2.5 m per unit).
  double courtWardShare = .3;     // Common wards rung around a court instead of bisected.
  bool villageFarmsteads = false; // Outskirts use farmstead clusters; the developed core retains dense frontage lots.
  uint64_t seed = 7, revision = 1;
  std::function<double(double, double)> terrain; // Height of the terrace under streets.
  std::function<double(uint64_t, uint64_t)> random;

  // A lot smaller than this is not a lot: it goes to the smallest neighbour
  // it shares an edge with. At thirty square metres a house on it is a hut
  // of four cells, and a fifth of the city's houses were under 16 m2. Last
  // in the struct: the tests build these settings positionally.
  //
  // Raised from thirty, but not as far as it first went. Thirty is a lot
  // wide enough to have a frontage, not one a household lives on - at that
  // size the planner gets a single room. Seventy is a family's worth and cost
  // a third of the town: the urban area is fixed by the site, so every metre
  // added to the minimum comes straight off the house count. Forty five keeps
  // the huts out and most of the houses in; what makes the remaining small
  // ones liveable is the footprint they are allowed, not the plot.
  double minimumLotMetres2 = 45;
};

class TownLayout final : public Generator<TownLayoutInput, rewrite::CityLayout>
{
public:
  explicit TownLayout(uint64_t domain);

  // Repartition adjoining parcels before identities/programmes are assigned.
  static uint32_t packFrontageLots(std::vector<ConvexRing> &lots, const std::vector<rewrite::CityStreet> &streets);

  static bool plan(const TownLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error);

protected:
  bool run(const TownLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
