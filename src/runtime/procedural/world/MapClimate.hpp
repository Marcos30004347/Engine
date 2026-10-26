#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapRivers.hpp"

#include <string>
#include <vector>

namespace procedural::world {

// Land types, in the vocabulary a physical atlas uses. Deliberately richer than
// `CellType`, which mixes landform and cover and has no vocabulary for cold or
// for rainforest.
enum class Biome : uint8_t
{
  Ocean = 0u,
  Lake,
  Shore,

  Glacier,
  Tundra,
  ColdDesert,
  SaltFlat,

  Taiga,
  TemperateForest,
  TemperateRainforest,
  MontaneForest,
  Mediterranean,

  Grassland,
  Steppe,
  Shrubland,
  Savanna,

  TropicalSeasonalForest,
  TropicalRainforest,
  // Warm and wet, between temperate forest and tropical rainforest - the
  // laurel forests and, at altitude, the cloud forests. Exists because without
  // it the classifier stepped straight from one to the other across the 21 C
  // threshold, and the compatibility gate reported it 97 times on one map.
  SubtropicalForest,

  Desert,
  SandDesert,
  Wetland,
  Swamp,
  Mangrove,
  AlpineMeadow,
  BareRock,

  Count,
};

const char *biomeName(Biome biome);
bool biomeIsWater(Biome biome);

// MapNode stores the biome as a plain byte so the graph does not have to know
// about this module; these are the only places that byte should be read or
// written.
inline Biome biomeOf(const MapNode &node)
{
  return static_cast<Biome>(node.biome);
}

inline void setBiome(MapNode &node, const Biome biome)
{
  node.biome = static_cast<uint8_t>(biome);
}

struct ClimateSettings
{
  double extentMetres = 361000.0;

  // The latitude band the world occupies. A world is a window onto a globe, and
  // where that window sits is what decides whether it has tundra or rainforest;
  // without it every world is the same temperate middle.
  double southLatitudeDegrees = 34.0;
  double latitudeSpanDegrees = 9.0;

  // Sea-level temperature at the equator and at the pole, in Celsius. The
  // profile between them is cosine, which is a decent fit to the real zonal
  // mean and costs nothing.
  double equatorTemperature = 28.0;
  double poleTemperature = -22.0;

  // Environmental lapse rate, Celsius per kilometre of elevation. 6.5 is the
  // standard atmosphere; this is what puts snow on a tropical summit.
  double lapseRatePerKilometre = 6.5;

  // Where the weather comes from, degrees clockwise from north. 270 is a
  // westerly, which is what the mid-latitudes actually get.
  double prevailingWindBearingDegrees = 270.0;

  // How far inland maritime moisture reaches, and how much of it a mountain
  // barrier removes.
  double coastalMoistureRangeMetres = 240000.0;
  double rainShadowStrength = 0.75;

  // The subtropical high, centred near 27 degrees and about 9 degrees wide.
  // Descending air, no rain: this belt is why the Sahara, the Kalahari, the
  // Atacama and the Australian interior are where they are, and without it a
  // world has no deserts unless a mountain happens to shade one.
  double dryBeltStrength = 0.72;
  double dryBeltCentreDegrees = 27.0;
  double dryBeltWidthDegrees = 9.0;
  // Barrier height, in metres, that halves the moisture crossing it.
  double rainShadowHalfHeightMetres = 900.0;

  // Extra moisture beside a channel or a lake. Riparian strips are why a desert
  // has a green line through it.
  double riparianStrength = 0.25;

  uint64_t seed = 0u;
};

struct ClimateCell
{
  double temperatureCelsius = 0.0;
  // 0..1, where 0 is hyper-arid and 1 is perhumid.
  double moisture = 0.0;
  // 0..1, how much of the maritime moisture a barrier upwind removed.
  double rainShadow = 0.0;
  double latitudeDegrees = 0.0;
};

struct ClimateMap
{
  // Indexed by node id.
  std::vector<ClimateCell> cells;
};

// Builds the temperature and moisture fields and classifies every leaf.
//
// The point of doing it this way rather than picking a type per region: both
// fields are **continuous**, so neighbouring cells get near-identical values and
// therefore compatible types, by construction rather than by repair. A
// rainforest cannot appear beside a desert unless the moisture field really
// falls off a cliff between them - which happens exactly where it should, at a
// rain shadow, and is not a defect.
//
// Writes `biome` on every leaf. `rivers` may be empty, in which case the
// riparian term is skipped.
bool buildClimate(const ClimateSettings &settings, const RiverNetwork &rivers, MapGraph &graph, ClimateMap &climate, std::string *error = nullptr);

// Whittaker classification: what grows at this temperature and moisture.
Biome classifyBiome(double temperatureCelsius, double moisture, double elevationMetres, double slope);

// Can these two share a border?
//
// Used as a gate rather than as a fixer. If the climate fields are continuous
// this is always true, so a failure means a field has a discontinuity in it -
// which is the bug worth finding, not the biome pair.
bool biomesCompatible(Biome first, Biome second);

// Pairs that fail `biomesCompatible`, over every leaf boundary. Zero is the
// expected answer; a non-zero count is a diagnostic, and the count is returned
// rather than the pass silently patching them.
uint32_t countIncompatibleBorders(const MapGraph &graph);

struct IncompatiblePair
{
  Biome first = Biome::Ocean;
  Biome second = Biome::Ocean;
  uint32_t count = 0u;
};

// The offending pairs, commonest first. A count alone says something is wrong;
// this says what, which is the difference between fixing the axis that is
// actually broken and guessing at one that is not.
std::vector<IncompatiblePair> incompatibleBorderPairs(const MapGraph &graph);

} // namespace procedural::world
