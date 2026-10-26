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
  double mainStreet = 5, regularStreet = 2.5, alley = 1.5;
  double unit = 6.25;             // Watabou's square unit in m2 (2.5 m per unit).
  double courtWardShare = .3;     // Common wards rung around a court instead of bisected.
  bool villageFarmsteads = false; // Watabou's Farm ward on every patch: one farmstead cluster (a 10 m square cut by createOrthoBuilding) placed between a vertex and the centroid.
  uint64_t seed = 7, revision = 1;
  std::function<double(double, double)> terrain; // Height of the terrace under streets.
  std::function<double(uint64_t, uint64_t)> random;
};

class TownLayout final : public Generator<TownLayoutInput, rewrite::CityLayout>
{
public:
  explicit TownLayout(uint64_t domain);

  static bool plan(const TownLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error);

protected:
  bool run(const TownLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
