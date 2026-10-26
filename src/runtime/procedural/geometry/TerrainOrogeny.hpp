#pragma once
#include "procedural/geometry/TerrainRidges.hpp"
#include <memory>

namespace procedural::geometry
{
// How a range was built, and therefore what it looks like.
//
// One mountain generator produced one mountain: a single spine was drawn
// through the whole landmass, every node on it was given the same rise, and a
// cap clipped that rise to a constant - so the result was a plateau in the
// middle of the map with a flat top, which is exactly what a constant height
// over a connected region is. Real ranges differ by the process that raised
// them, and the difference is legible from the air: an active collision belt is
// a narrow line of high summits, an old fold belt is a wide corduroy of
// parallel ridges, an upland massif is a broad dome with radial spurs, and a
// volcanic centre is a cone. These are the four, and each is a class.
enum class OrogenyKind : uint8_t { Cordillera = 0u, FoldBelt, Massif, Volcanic, Count };

// The measurable differences between them. Every figure here is taken from a
// real range rather than tuned by eye - see the comments on each field, and on
// `naturalSummitMetres`, for which range each number came from.
struct OrogenyProfile
{
  const char *name = "";
  // Multiplier on the landmass's natural summit height. Active convergence
  // stands well above what the landmass would otherwise carry (Taiwan: 3,952 m
  // on 36,000 km^2, against 2,450 m natural), an eroded shield well below
  // (Great Britain: 1,345 m on 209,000 km^2, against 3,800 m natural).
  double heightGain = 1.0;
  // Mean slope of the range flank, rise over run - Genevaux's lambda, the
  // slope-magnitude in `q_z = max(a_z,b_z,c_z) + lambda(q)*d`. The paper bounds
  // it at 0.25 and these had been set from photographs of flanks (the Alps at
  // 0.30-0.45) rather than from the crest relation, which is why the land came
  // out with 41% of it steeper than a village can be built on. Half-width
  // follows from the slope rather than being a free multiple of the height.
  double flankSlope = .25;
  // Distance between summits along the crest, and how far the cols between them
  // drop. A crest is not level: in the Alps summits recur every 8-15 km with
  // passes 900-1,500 m below them, which is why a range reads as a row of peaks
  // and a constant rise reads as a mesa.
  double summitSpacingMetres = 11000.0;
  double colFraction = .55;   // a col stands this fraction of the summit height
  // Sub-parallel ridge lines in the belt, and their spacing as a fraction of
  // the belt length. The Appalachian Valley and Ridge is a dozen parallel
  // ridges over 100 km; the Andes are one line with a second behind it.
  uint32_t spines = 1u;
  double spineSpacingFraction = .16;
  // Spurs off the crest: length as a fraction of the spine, and the height they
  // keep. A cordillera throws short steep spurs; a massif is mostly spurs.
  double spurBudgetFraction = .16;
  double spurRiseFraction = .6;
  // Relief carried at the ends. A fold belt plunges under its foreland and dies
  // gradually; a volcanic centre simply stops.
  double endTaperFraction = .18;
};
const OrogenyProfile &orogenyProfile(OrogenyKind kind);

// The terrain slope map: Genevaux's lambda as a *field*, not a constant.
//
// The paper is explicit that lambda "describes which parts of the terrain will
// become hills, plateaus, valleys, or mountains", and that it can be weighted
// by distance to the coast and by node elevation. A single lambda therefore
// gives a single kind of country - which is exactly what a landmass with one
// mountain in the middle of it is. Sampling lambda from a low-frequency field
// gives provinces: a range here, an upland there, a plain between them, and a
// mountain front where they meet.
//
// Returns 0 (flat country) to 1 (the most mountainous province on the map).
double terrainSlopeProvince(Point2 point, double extentMetres, uint64_t seed);

// The natural summit height of a landmass of this area, in metres above its
// own base. Maximum elevation scales with area across real islands and
// continents, because both isostatic support and the time erosion needs to
// remove a summit scale with the mass beneath it. Fitting
//   summit = 1000 * (area / 1000 km^2) ^ 0.25
// against Skye (1,656 km^2, 992 m), Mallorca (3,640, 1,445), Corsica (8,680,
// 2,706), Cyprus (9,251, 1,952), Sardinia (24,000, 1,834), Taiwan (36,000,
// 3,952), Honshu (227,000, 3,776) and Great Britain (209,000, 1,345) puts every
// one of them within a factor of 1.6, and the outliers in both directions are
// explained by the tectonics: Taiwan is an active collision, Britain is a worn
// shield. So this gives the height, and `heightGain` gives the process.
//
// It also removes the last of the absolute metre constants from this file. The
// old code asked for 1,600-2,800 m of rise whatever the world was, which is a
// real mountain on a continent and buries a 48 km island; the cap added to stop
// that is what flattened the tops. Area is the honest input.
double naturalSummitMetres(double landAreaSquareKm);

// One mountain-building process. The shared driver walks the graph, finds the
// components and the crest lines; the strategy decides what is built along
// them. Subclasses override only where they actually differ.
class OrogenyStrategy
{
public:
  virtual ~OrogenyStrategy() = default;
  virtual OrogenyKind kind() const = 0;
  const OrogenyProfile &profile() const { return orogenyProfile(kind()); }
  const char *name() const { return profile().name; }

  // Height of the crest at `along` metres into a spine of `spineLength`, given
  // the landmass's summit height. The default is the one every range shares:
  // summits and cols at the profile's spacing, tapering at both ends.
  virtual double crestRise(double summitMetres, double along, double spineLength,
      uint64_t seed) const;
  // The shape of the crest alone, from 0 at the ends to 1 at the highest
  // summit. Separated from the height so the divide network can use the same
  // profile for a ridge whose height comes from the drainage rather than from
  // the landmass area.
  virtual double crestVariation(double along, double spineLength, uint64_t seed) const;
  // Half-width of the flank at a given rise. Slope, not a multiplier.
  virtual double flankHalfWidth(double riseMetres) const;
  // Whether this process can raise a range on a component with this much
  // upland potential and this much length available.
  virtual bool admits(double potential, double spineLength, double extentMetres) const;
};

// Picks the process for one component from what the map already says about it:
// a craton seam running through it is a collision, high potential over a long
// component is a cordillera, a short high one is volcanic, a broad low one is a
// massif.
std::unique_ptr<OrogenyStrategy> makeOrogeny(OrogenyKind kind);
}
