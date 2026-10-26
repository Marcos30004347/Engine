#pragma once

#include <cstdint>

namespace procedural
{

// Rock type of the bedrock beneath a surface cell.
//
// This is substrate, not land cover. The existing PolygonTerrainMaterial values
// are all cover — Grass, Tundra, Taiga and so on — and its two substrate-looking
// entries, Rock and Bare, are assigned by a Whittaker climate lookup rather than
// by geology. Lithology is what erodes; cover is what grows on it.
//
// Erodibility is the reason this exists. A uniform-k landscape cannot produce
// cliffs, structural benches, resistant caprock, mesas, waterfalls at
// lithological contacts, or asymmetric valley cross-sections at any iteration
// count, because all of those are differential erosion.
enum class PlanetLithology : uint8_t
{
  // Weak clastics. Erodes fastest, floors valleys, forms badlands.
  Shale = 0u,
  // Mixed sedimentary sequence: the default cover of stable platforms.
  Sedimentary = 1u,
  // Soluble but mechanically strong. Holds benches and cliff bands.
  Limestone = 2u,
  // The classic caprock: resistant enough to hold a mesa above weak shale.
  Sandstone = 3u,
  // Fresh volcanics: ash and tuff, weak until welded.
  Volcanic = 4u,
  // Flood basalt. Strong, forms stepped plateaus and coastal cliffs.
  Basalt = 5u,
  // Foliated basement raised in collision belts.
  Metamorphic = 6u,
  // Intrusive basement. The most resistant rock here, and what mountain cores
  // are left standing on.
  Granite = 7u,
  Count = 8u,
};

constexpr uint32_t kPlanetLithologyCount = static_cast<uint32_t>(PlanetLithology::Count);

// Physical properties of one rock type.
struct PlanetLithologyProperties
{
  // Multiplier on the stream power erodibility k. Real rock spans well over an
  // order of magnitude, and a narrow range simply reads as uniform terrain, so
  // this table deliberately spans 15x from shale to granite.
  //
  // The values are normalized to a geometric mean of 1, not to a maximum of 1.
  // A table where every rock is at or below 1 silently divides the global
  // erosion rate by its own mean, which leaves uplift unopposed: the first
  // version of this table, topping out at 1.0 for shale, drove peak relief
  // straight into the encoding ceiling.
  float erodibility = 1.0f;
  // Stable hillslope angle in degrees. Weak rock stands at a debris-cone angle;
  // strong rock holds a near-vertical face, which is what a cliff is.
  float talusAngleDegrees = 34.0f;
  // Relative rate at which this rock weathers into regolith. Phase 3 consumes
  // this; it is declared here so the table has one home.
  float regolithYield = 1.0f;
  // Linear sRGB albedo of freshly exposed rock, and a roughness to match.
  float albedo[3] = {0.5f, 0.5f, 0.5f};
  float roughness = 0.85f;
};

// Property table, indexed by PlanetLithology.
//
// Albedos are dry rock in daylight, chosen to stay distinguishable from each
// other after the surface-cover tint is applied on top.
constexpr PlanetLithologyProperties kPlanetLithologyTable[kPlanetLithologyCount] = {
    // Shale: dark grey-brown, crumbles to slopes rather than faces.
    {2.92f, 28.0f, 1.35f, {0.24f, 0.22f, 0.20f}, 0.92f},
    // Sedimentary: pale buff, the neutral platform default.
    {1.98f, 33.0f, 1.10f, {0.44f, 0.40f, 0.34f}, 0.88f},
    // Limestone: light grey, holds benches.
    {1.34f, 45.0f, 0.85f, {0.62f, 0.60f, 0.55f}, 0.80f},
    // Sandstone: warm red-buff caprock.
    {1.11f, 55.0f, 0.75f, {0.58f, 0.42f, 0.30f}, 0.84f},
    // Volcanic ash and tuff: mid grey, weak.
    {2.33f, 32.0f, 1.20f, {0.36f, 0.34f, 0.33f}, 0.90f},
    // Basalt: near-black, strong, stepped.
    {0.64f, 60.0f, 0.55f, {0.16f, 0.16f, 0.17f}, 0.78f},
    // Metamorphic: banded grey-green basement.
    {0.38f, 58.0f, 0.45f, {0.34f, 0.36f, 0.33f}, 0.74f},
    // Granite: pale pink-grey, the resistant core.
    {0.20f, 62.0f, 0.40f, {0.56f, 0.52f, 0.50f}, 0.70f},
};

// Which rock suite a place belongs to. The suite is fixed by tectonics and does
// not change as the surface erodes; what changes is which stratum of that suite
// the surface currently sits in.
enum class PlanetRockSuite : uint8_t
{
  // Ocean floor: basalt, with volcanic centres on active boundaries.
  Oceanic = 0u,
  // Continental basement raised in collision belts.
  Basement = 1u,
  // Stable platform carrying a layered sedimentary sequence.
  Platform = 2u,
  // Volcanic province: stacked flows over their own basement.
  Volcanic = 3u,
  Count = 4u,
};

// Thickness of one sedimentary bed, in metres. Thick enough that a bed spans
// several cells at bake resolution — a stratum thinner than the cell size is
// invisible no matter how much its erodibility differs from its neighbours.
constexpr float kPlanetStratumThicknessMetres = 210.0f;

// Rock at a given height in a given suite.
//
// Stratigraphy is evaluated against the *current* surface elevation rather than
// baked once from the initial one. That is what makes layered ground behave
// like layered ground: as a valley cuts down it exposes the bed below, a
// resistant bed becomes a bench or a caprock rim where the surface reaches it,
// and the same query answers "what is under this voxel" when digging.
//
// bedPhaseMetres tilts and offsets the sequence per place, so bedding is not a
// single set of planet-wide horizontal planes.
inline PlanetLithology planetStratumLithology(
    const PlanetRockSuite suite,
    const float elevationMetres,
    const float bedPhaseMetres)
{
  const float section = elevationMetres + bedPhaseMetres;
  const int32_t bed = static_cast<int32_t>(section / kPlanetStratumThicknessMetres);
  // Floor division, so the cycle does not mirror around sea level.
  const int32_t index = ((bed % 4) + 4) % 4;
  switch (suite)
  {
  case PlanetRockSuite::Oceanic:
    return index == 0 ? PlanetLithology::Volcanic : PlanetLithology::Basalt;
  case PlanetRockSuite::Basement:
    // Basement is not layered, but grade varies: granite cores with
    // metamorphic envelopes.
    return index == 2 ? PlanetLithology::Metamorphic : PlanetLithology::Granite;
  case PlanetRockSuite::Volcanic:
    // Stacked flows: weak ash between welded basalt, which is what makes a
    // volcanic plateau step rather than taper.
    return index == 1 ? PlanetLithology::Volcanic : PlanetLithology::Basalt;
  default:
    break;
  }
  // Platform: the classic sequence. Sandstone directly over shale is the
  // caprock pairing that holds a mesa up.
  switch (index)
  {
  case 0:
    return PlanetLithology::Shale;
  case 1:
    return PlanetLithology::Sandstone;
  case 2:
    return PlanetLithology::Limestone;
  default:
    return PlanetLithology::Sedimentary;
  }
}

inline const PlanetLithologyProperties &planetLithologyProperties(const PlanetLithology lithology)
{
  const uint32_t index = static_cast<uint32_t>(lithology);
  return kPlanetLithologyTable[index < kPlanetLithologyCount ? index : 0u];
}

inline const PlanetLithologyProperties &planetLithologyProperties(const uint8_t lithology)
{
  return kPlanetLithologyTable[lithology < kPlanetLithologyCount ? lithology : 0u];
}

} // namespace procedural
