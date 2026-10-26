#pragma once
#include "procedural/Geometry.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::geometry
{
// Settlement siting and the road network between the results.
//
// This is stage 0 of the parcel layer, and it comes first because the roads are
// what blocks are cut by: fields exist because somebody farms them from
// somewhere, so the pattern has to radiate from settlements rather than cover
// the map evenly.
//
// Suitability is built the way material weights and Horizon Zero Dawn's
// placement densities are - a response curve per criterion over a map we
// already export, multiplied together - so one map can be read differently by
// each consumer instead of being thresholded once.
enum class SettlementTier : uint8_t { City = 0u, Town, Village, Count };

struct SettlementSite
{
  Point2 position{};
  SettlementTier tier = SettlementTier::Village;
  double suitability = 0.0;
  uint32_t id = 0u;
};
// A road is a ribbon, not a line. Until it had a width it could cut parcels and
// tint the ground it crossed, but it had no area of its own, so there was
// nothing in the mesh to see.
enum class RoadClass : uint8_t { Highway = 0u, Road, Lane, Count };
struct SettlementRoad
{
  std::vector<Point2> path;
  uint32_t from = 0u, to = 0u;
  double lengthMetres = 0.0;
  RoadClass kind = RoadClass::Lane;
  double widthMetres = 5.0;
};
// The built form of one settlement, after Watabou's ordering: outline, then a
// street skeleton, then the districts between the streets. Lots come later, in
// the parcel layer, which already subdivides a settlement block far finer than
// a field.
enum class DistrictKind : uint8_t
{
  Common = 0u,  // ordinary housing
  Market,       // the open ground at the centre
  Craft,        // workshops, usually on the water or the downwind side
  Gentry,       // the good addresses, away from the craft quarter
  Temple,
  Castle,
  Farmstead,    // outside the wall
  Count,
};
struct SettlementDistrict
{
  std::vector<Point2> boundary;
  DistrictKind kind = DistrictKind::Common;
  bool insideWall = false;
};
struct SettlementPlan
{
  uint32_t site = 0u;
  std::vector<Point2> outline;               // the built edge, walled or not
  std::vector<Point2> wall;                  // empty when the place is unwalled
  std::vector<Point2> gates;                 // where a road crosses the outline
  std::vector<std::vector<Point2>> streets;  // every ward boundary: the skeleton
  // The main streets: a path from each gate to the middle, traced along ward
  // boundaries. Watabou builds these with a search over the ward-corner graph
  // rather than drawing them, which is why they bend around blocks instead of
  // converging on a point.
  std::vector<std::vector<Point2>> arteries;
  std::vector<SettlementDistrict> districts;
  double radiusMetres = 0.0;
};

struct SettlementNetwork
{
  std::vector<SettlementSite> sites;
  std::vector<SettlementRoad> roads;
  std::vector<SettlementPlan> plans;
  // Suitability as evaluated, for debugging and for later consumers that want
  // to know how good the ground was rather than only where the towns landed.
  std::vector<float> suitability;
  uint32_t suitabilitySide = 0u;
};

// Everything the siting reads. Taking accessors rather than a TerrainMapSource
// keeps this testable on a synthetic world.
struct SettlementFields
{
  std::function<double(Point2)> elevationMetres;
  std::function<double(Point2)> slope;            // gradient magnitude, rise over run
  std::function<double(Point2)> riverDistanceMetres;
  std::function<double(Point2)> riverDischarge;   // cubic metres per second
  std::function<double(Point2)> coastDistanceMetres;
  std::function<double(Point2)> waterLevelMetres;
  std::function<uint8_t(Point2)> waterType;       // 0 land, 1 ocean, 2 lake
  std::function<double(Point2)> treeCover;        // 0..1
  std::function<double(Point2)> soilQuality;      // 0..1
  std::function<double(Point2)> temperatureCelsius;
  std::function<double(Point2)> moisture;         // 0..1
};

struct SettlementTierSettings
{
  // Nothing of this tier is placed closer than this to another of the same or
  // a larger tier. Largest tier is placed first, so a city is never crowded out
  // by the villages that should be its satellites.
  double exclusionRadiusMetres = 6000.0;
  uint32_t maximumCount = 0u;      // 0 means unlimited
  double minimumSuitability = .25;
};

struct SettlementSettings
{
  bool enabled = true;
  // Grid the suitability field is evaluated on. Sites land on cell centres, so
  // this also sets the positional granularity.
  uint32_t resolution = 512u;
  uint64_t seed = 0u;
  std::array<SettlementTierSettings, 3> tiers{{
      {50000.0, 8u, .45},   // city
      {16000.0, 60u, .35},  // town
      {4500.0, 600u, .22}}};// village

  // Hard gates. Ground that fails one of these is never a site.
  double maximumSlope = .18;
  double minimumFreeboardMetres = 4.0;   // above the local water level
  double minimumTemperature = -6.0;
  double minimumMoisture = .12;

  // Catchment radius for the criteria that are about a hinterland rather than a
  // spot. A town needs land around it; sampling at the site alone puts
  // settlements on a lone fertile texel surrounded by rock.
  double catchmentRadiusMetres = 2500.0;

  // Fresh water. The curve peaks a short way back from the bank, not on it.
  double waterIdealDistanceMetres = 260.0;
  double waterFalloffMetres = 1400.0;
  double waterMinimumDischarge = 3.0;
  // Harbour: a second, separate peak against the coast.
  double harbourFalloffMetres = 1800.0;
  // Defensible ground is a band. The classic site is at the foot of the high
  // ground, never on the summit, so this peaks at a distance.
  double reliefIdealMetres = 120.0;
  double reliefFalloffMetres = 260.0;

  // Roads. Slope is the dominant cost; a crossing is paid once.
  //
  // The routing grid is its own resolution, finer than suitability: a road is a
  // line, and at 500 m cells an eight-connected search can only emit multiples
  // of 45 degrees, so every route came out a staircase. Sixteen neighbours
  // halve the angular quantum again, to 11.25 degrees, which is below what
  // reads as a corner once the path is smoothed.
  uint32_t roadResolution = 1024u;
  // Radius of the pathfinding lattice, in cells. Every coprime offset inside it
  // is one distinct heading: radius 4 gives forty of them, against eight for a
  // touching-neighbour lattice.
  // Galin et al. tabulate the angular resolution against the mask radius: k=1
  // gives 45 degrees and 8 headings, k=2 26.5 and 16, k=3 18.5 and 32, k=4 14.0
  // and 48, k=5 11.3 and 80, k=6 9.5 and 96 - and they find k=5 "a good
  // compromise between speed and the angle resolution to generate realistic
  // paths".
  uint32_t roadNeighbourRadius = 5u;
  // Weight on the change of heading, added to the slope penalty. Roads that
  // cost the same either way will otherwise alternate between two lattice
  // directions, which reads as a twitch every few hundred metres.
  double roadTurnPenalty = 9.0;
  // O1 in the paper's WEIGHT(e) = LENGTH(e) * max(1, O1 * E_max).
  double roadSlopePenalty = 26.0;
  // An edge steeper than this anywhere along it is discarded, so a road cannot
  // cross a mountain at any price. This is the difference between going round a
  // range and going over it.
  double roadMaximumSlope = .34;
  // Cost per metre of water bridged, and the longest span that can be bridged
  // at all. Beyond it the crossing is refused and the road finds a ford.
  double roadWaterCrossingPenalty = 240.0;
  double roadMaximumSpanMetres = 420.0;
  // Chaikin rounds applied to a finished route. Two is enough to take the
  // lattice out without cutting the corners a real road would hold.
  uint32_t roadSmoothing = 3u;
  // A second route is laid to any further site whose Gabriel test passes and
  // which is within this multiple of the nearest one. Strictly connecting each
  // site to its nearest larger neighbour makes a tree, and a road network that
  // is a tree has no bypasses and no triangles - it reads as a dendrite.
  double roadLoopFactor = 1.9;
  // Cost multiplier applied along a road already laid, so later routes prefer
  // to join an existing corridor rather than run beside it.
  double roadReuseDiscount = .25;
  bool buildRoads = true;

  // Built form.
  bool buildPlans = true;
  // Radius of the built area, by tier. A city at 900 m encloses 254 ha, which
  // is Paris inside the wall of Philip Augustus; a town at 430 m is 58 ha,
  // which is a middling English borough. The earlier 620 m city was defensible
  // on its own but left the place only eight fields across, and the reference
  // photographs put a town at twenty to forty field widths.
  std::array<double, 3> builtRadiusMetres{{900.0, 430.0, 165.0}};
  // Walls are worth building only where there is something to defend; below
  // this tier a place is open.
  SettlementTier walledBelow = SettlementTier::Town;   // cities and towns walled
  uint32_t minimumGates = 3u;
  // Carriageway width by class. A trunk route between cities is a made road; a
  // lane between two villages is a cart track. These set both the ribbon cut
  // into the parcels and the material the mesh shows.
  std::array<double, 3> roadWidthMetres{{11.0, 7.0, 4.5}};
  // Dressing the wall into masonry. A curtain wall is built in long straight
  // runs between towers, so the raw ward boundary - which turns every twenty
  // metres - is smoothed towards its chords and then simplified: runs shorter
  // than `wallRunMetres` and turns shallower than `wallTurnDegrees` are
  // dropped, which lets the wall cut across a ward rather than detour round it.
  uint32_t wallSmoothing = 3u;
  double wallRunMetres = 46.0;
  double wallTurnDegrees = 14.0;
  // Target area of one ward. The ward is the unit the plan is made of, the way
  // a field is the unit the country is made of: the streets are the boundaries
  // between wards, so this sets the block size and therefore the street
  // spacing. 120 m blocks is a medieval core.
  // Watabou's city is 15 wards inside a radius of about 47 units, and the
  // reference plan's scale bar puts a walled city at ~820 m and ~13 wards
  // across - so a ward is ~63 m and carries about two dozen plots. At 110 m
  // ours carried four times the ground per ward, which is why a block read as a
  // hexagon with a pattern in it rather than as a city block.
  double wardAreaSquareMetres = 62.0 * 62.0;
  // Lloyd relaxation rounds on the ward seeds. Raw jitter leaves slivers and
  // long thin wards; two rounds is enough to even them without making the
  // pattern read as a honeycomb.
  uint32_t wardRelaxation = 3u;
  // Junction welding, as a fraction of a ward's width. Watabou merges ward
  // vertices closer than 8 units against a city radius of ~47, which is about a
  // third of one ward edge; three cells almost never meet at a point, so a raw
  // Voronoi ward carries a scatter of very short edges around every junction and
  // reads as a fish scale rather than a city block. Welding them leaves a
  // polygon with fewer, longer edges - and every path that follows ward corners
  // stops twitching with it.
  double wardWeldFraction = .17;
  // Fraction of the settlement's wards that the wall encloses. The rest are the
  // suburb outside the gate, which is where a real town's growth goes.
  double wallFraction = .62;
};

// `extentMetres` is the square world side. Deterministic for a given seed and
// field set.
SettlementNetwork planSettlements(double extentMetres, const SettlementFields &fields,
    const SettlementSettings &settings = {});
}
