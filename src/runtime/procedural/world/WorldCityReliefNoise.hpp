#pragma once

#include <cstdint>
#include <functional>

namespace procedural::world::rewrite {

// Low-frequency relief under a settlement.
//
// The site selector picks flat ground and the world's own surface under a town
// is close to a plane, so a town built on it has no verticality to express. The
// noise here is the hill the town stands on: one or two long wavelengths, tens
// of metres of amplitude, faded to nothing outside the settlement's reach so
// the hinterland still reads the world map's own height. Zero amplitude uses
// the world surface exactly as it comes, which is what a town on real relief
// wants.
struct CityReliefNoise
{
  double amplitudeMetres = 0;
  double wavelengthMetres = 190;
  uint32_t octaves = 3;
  uint64_t seed = 1;
  double terraceRiseMetres = 0; // Zero keeps continuous hills.
};

std::function<double(double, double)> cityReliefSurface(
    std::function<double(double, double)> base,
    const CityReliefNoise &,
    double centreX,
    double centreZ,
    double reachMetres,
    double fadeMetres);

struct CityReliefSettings
{
  // Broad shared terrain ramps before circulation grading; zero retains the source.
  double terrainFacetMetres = 8.;
  // A way steeper than this is not a way a cart goes up. The road solve pulls
  // the network below it; what stays steeper becomes a stepped way.
  double maximumStreetGrade = .12;
  // Minimum excavation below the lowest sampled point across a road section.
  // The road solve can lower a bed further to satisfy its longitudinal grade.
  double streetExcavationMetres = .3;
  bool excavateStreets = true;
  bool stairStreets = false;
  // Steeper still, and no amount of grading makes it a road.
  double steppedWayGrade = .2;
  double stepRiseMetres = .16, stepGoingMetres = .34;
  uint32_t stepsPerFlight = 11;
  double landingMetres = 1.3;
  // Lot pads are horizontal and snap to this, so neighbours share levels and
  // the town reads as terraced rather than as sampled noise.
  double levelStepMetres = .3;
  // How far the flattened region grades back into the hill around it.
  //
  // This was half a metre, and half a metre carrying three is a wall. Built as
  // one it needed masonry standing in it, the boolean that cut it out of every
  // lot left slivers - the cracks - and the ground texture, which is laid in
  // plan, smeared into vertical streaks down a face that steep. None of that
  // is worth having: the region a house stands on is simply flattened, and the
  // cut grades back into the ground over a bank you could walk up.
  double padSkirtMetres = 2.5;
  // The steepest the bank around a flattened region may be.
  //
  // A fixed skirt cannot work: two and a half metres is a gentle bank under a
  // one metre cut and a wall under a three metre one, and the deep ones are
  // exactly where it matters. Each region's skirt is sized from its own drop
  // instead, so the bank is always one you could walk up - which is also what
  // joins the terrace to the ground beside it.
  double maximumBankGrade = .45;
  // How far a doorway may stand above or below the street it opens onto.
  //
  // This is the whole of the walkability contract: a pad is pinned to within
  // this of the street that serves the lot, so every entrance is a step or a
  // short slope from a way you can walk. Anchoring the pad to the lot's own
  // ground instead - which the first version did - puts doors metres above
  // their street and there is no way in.
  double doorstepMetres = .45;
  // How far a way can be and still be the street a lot is levelled onto.
  // Beyond this the lot is back land and keeps its own ground.
  double streetServiceMetres = 26;
  // The stone base a building stands on where the ground falls away from a
  // pad that is level with its street. This is a reported budget now, not a
  // clamp: what the pad may do is set by `doorstepMetres`.
  double maximumPlinthMetres = 3.2;
  // Beyond this a lot cannot be terraced at all and is left to the ground.
  double maximumRetainedMetres = 7;
  // A wall walk steeper than this is stepped rather than ramped. The Great
  // Wall is the reference: the rampart follows the ridge it stands on, and
  // where the ridge is too steep to walk the walk becomes a stair inside the
  // parapet rather than the wall becoming a flight of level terraces.
  double maximumWalkGrade = .16;
  uint32_t solverIterations = 96;
};

} // namespace procedural::world::rewrite
