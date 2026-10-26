#include "procedural/geometry/TerrainMaterial.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::geometry
{

const char *surfaceRoleName(const SurfaceRole role)
{
  switch (role)
  {
  case SurfaceRole::Ground: return "ground";
  case SurfaceRole::Face: return "face";
  case SurfaceRole::Talus: return "talus";
  case SurfaceRole::RampSurface: return "ramp surface";
  case SurfaceRole::RampShoulder: return "ramp shoulder";
  case SurfaceRole::RiverBed: return "river bed";
  case SurfaceRole::RiverBank: return "river bank";
  case SurfaceRole::Water: return "water";
  default: return "unknown";
  }
}

bool MaterialEntry::operator<(const MaterialEntry &other) const
{
  if (role != other.role)
    return role < other.role;
  if (lithology != other.lithology)
    return lithology < other.lithology;
  if (cover != other.cover)
    return cover < other.cover;
  if (weathering != other.weathering) return weathering < other.weathering;
  return tone < other.tone;
}

bool MaterialEntry::operator==(const MaterialEntry &other) const
{
  return role == other.role && lithology == other.lithology &&
      cover == other.cover && weathering == other.weathering && tone == other.tone;
}

namespace
{

std::array<float, 3> coverColour(const world::Biome cover)
{
  switch (cover)
  {
  case world::Biome::Ocean: return {0.07f, 0.17f, 0.34f};
  case world::Biome::Lake: return {0.15f, 0.33f, 0.52f};
  case world::Biome::Shore: return {0.78f, 0.72f, 0.52f};
  case world::Biome::Glacier: return {0.92f, 0.94f, 0.97f};
  case world::Biome::Tundra: return {0.52f, 0.53f, 0.44f};
  case world::Biome::ColdDesert: return {0.62f, 0.60f, 0.52f};
  case world::Biome::SaltFlat: return {0.86f, 0.85f, 0.80f};
  case world::Biome::Taiga: return {0.20f, 0.31f, 0.24f};
  case world::Biome::TemperateForest: return {0.21f, 0.38f, 0.20f};
  case world::Biome::TemperateRainforest: return {0.16f, 0.35f, 0.22f};
  case world::Biome::MontaneForest: return {0.22f, 0.33f, 0.25f};
  case world::Biome::Mediterranean: return {0.45f, 0.47f, 0.26f};
  case world::Biome::Grassland: return {0.44f, 0.55f, 0.26f};
  case world::Biome::Steppe: return {0.57f, 0.56f, 0.33f};
  case world::Biome::Shrubland: return {0.42f, 0.45f, 0.27f};
  case world::Biome::Savanna: return {0.60f, 0.57f, 0.30f};
  case world::Biome::Desert: return {0.76f, 0.68f, 0.46f};
  case world::Biome::SandDesert: return {0.82f, 0.72f, 0.47f};
  case world::Biome::Wetland: return {0.29f, 0.38f, 0.27f};
  default: return {0.42f, 0.50f, 0.30f};
  }
}

} // namespace

std::array<float, 3> materialColour(const MaterialEntry &entry)
{
  const bool rock = entry.role == SurfaceRole::Face ||
      entry.role == SurfaceRole::RampShoulder || entry.role == SurfaceRole::Talus;
  std::array<float, 3> base = rock
      ? std::array<float, 3>{
            kPlanetLithologyTable[static_cast<uint32_t>(entry.lithology)].albedo[0],
            kPlanetLithologyTable[static_cast<uint32_t>(entry.lithology)].albedo[1],
            kPlanetLithologyTable[static_cast<uint32_t>(entry.lithology)].albedo[2]}
      : coverColour(entry.cover);

  if (entry.role == SurfaceRole::Talus)
    for (float &channel : base)
      channel = std::min(1.0f, channel * 1.12f + 0.03f);
  if (entry.role == SurfaceRole::RiverBed)
    base = {0.30f, 0.33f, 0.29f};
  if (entry.role == SurfaceRole::Water)
    base = {0.16f, 0.34f, 0.52f};

  // Tone shifts the cover, never the rock: it is what makes one field read
  // differently from the next while both stay the same material. Small, and
  // biased green-to-ochre so it reads as crop and cut rather than as noise.
  if (!rock && entry.tone != 0u)
  {
    // The *kind* carries the ochre-against-green contrast; tone separates one
    // field from its neighbour within that. It has to be wide: a district of
    // two hundred parcels drew on thirty-three distinct colours at half this
    // spread, so neighbours matched constantly and merged into blobs several
    // times a field's size. Real fields run from pale straw to deep green, so
    // a wide band is also the truthful one. Brightness moves with hue rather
    // than against it, which is what keeps it reading as crop and cut instead
    // of as a tint applied to one colour.
    const float step = (float(entry.tone % 16u) - 7.5f) / 7.5f;
    const float lift = 1.0f + 0.20f * step;
    base[0] = std::clamp(base[0] * lift * (1.0f + 0.20f * step), 0.0f, 1.0f);
    base[1] = std::clamp(base[1] * lift * (1.0f + 0.02f * step), 0.0f, 1.0f);
    base[2] = std::clamp(base[2] * lift * (1.0f - 0.16f * step), 0.0f, 1.0f);
  }

  // Weathering multiplies the rock rather than replacing it. Dry weathering
  // darkens toward desert varnish; wet weathering greens toward lichen. One
  // scalar, applied to whatever the rock already is.
  const float amount = static_cast<float>(entry.weathering) / 255.0f;
  if (rock && amount > 0.0f)
  {
    base[0] *= 1.0f - 0.35f * amount;
    base[1] *= 1.0f - 0.18f * amount;
    base[2] *= 1.0f - 0.30f * amount;
    base[1] += 0.06f * amount;
  }
  for (float &channel : base)
    channel = std::clamp(channel, 0.0f, 1.0f);
  return base;
}

uint8_t MaterialPalette::add(const MaterialEntry &entry)
{
  const auto found = std::find(entries_.begin(), entries_.end(), entry);
  if (found != entries_.end())
    return static_cast<uint8_t>(std::distance(entries_.begin(), found));
  if (entries_.size() >= 256u)
  {
    saturated_ = true;
    return 0u;
  }
  // Published face IDs must survive later additions (e.g. river bank faces).
  const auto index = static_cast<uint8_t>(entries_.size());
  entries_.push_back(entry);
  return index;
}

uint8_t MaterialPalette::indexOf(const MaterialEntry &entry) const
{
  const auto found = std::find(entries_.begin(), entries_.end(), entry);
  return found != entries_.end()
      ? static_cast<uint8_t>(std::distance(entries_.begin(), found)) : 0u;
}

world::Biome polygonMaterialToBiome(const PolygonTerrainMaterial material)
{
  using world::Biome;
  switch (material)
  {
  case PolygonTerrainMaterial::Ocean: return Biome::Ocean;
  case PolygonTerrainMaterial::Lake: return Biome::Lake;
  case PolygonTerrainMaterial::Beach: return Biome::Shore;
  case PolygonTerrainMaterial::Grass: return Biome::Grassland;
  case PolygonTerrainMaterial::Rock: return Biome::ColdDesert;
  case PolygonTerrainMaterial::Snow: return Biome::Glacier;
  case PolygonTerrainMaterial::River: return Biome::Lake;
  case PolygonTerrainMaterial::Marsh: return Biome::Wetland;
  case PolygonTerrainMaterial::Ice: return Biome::Glacier;
  case PolygonTerrainMaterial::Tundra: return Biome::Tundra;
  case PolygonTerrainMaterial::Bare: return Biome::ColdDesert;
  case PolygonTerrainMaterial::Scorched: return Biome::SaltFlat;
  case PolygonTerrainMaterial::Taiga: return Biome::Taiga;
  case PolygonTerrainMaterial::Shrubland: return Biome::Shrubland;
  case PolygonTerrainMaterial::TemperateDesert: return Biome::Steppe;
  case PolygonTerrainMaterial::TemperateRainForest: return Biome::TemperateRainforest;
  case PolygonTerrainMaterial::TemperateDeciduousForest: return Biome::TemperateForest;
  case PolygonTerrainMaterial::Grassland: return Biome::Grassland;
  case PolygonTerrainMaterial::TropicalRainForest: return Biome::TropicalRainforest;
  case PolygonTerrainMaterial::TropicalSeasonalForest: return Biome::TropicalSeasonalForest;
  case PolygonTerrainMaterial::SubtropicalDesert: return Biome::Desert;
  default: return Biome::Grassland;
  }
}

PlanetLithology lithologyForCell(const world::MapNode &cell)
{
  static constexpr PlanetRockSuite kSuites[4] = {PlanetRockSuite::Platform,
      PlanetRockSuite::Basement, PlanetRockSuite::Platform,
      PlanetRockSuite::Volcanic};
  const uint32_t craton = cell.craton == 0xffffu ? 0u : cell.craton;
  const PlanetRockSuite suite = kSuites[craton % 4u];
  // Bedding tilts and offsets per place, so the strata are not one set of
  // planet-wide horizontal planes.
  const float phase = static_cast<float>((craton % 7u) * 30u);
  return planetStratumLithology(
      suite, static_cast<float>(cell.intent.elevationMetres), phase);
}

uint8_t weatheringFor(
    const world::Biome cover, const SurfaceRole role, const double slope)
{
  if (role == SurfaceRole::Ground || role == SurfaceRole::Water)
    return 0u;
  // Arid air makes desert varnish; wet air makes lichen; cold shatters. All
  // three are the same channel because all three are "how far has this face
  // been worked on", and none of them changes which rock it is.
  double amount = 0.25;
  switch (cover)
  {
  case world::Biome::Desert:
  case world::Biome::SandDesert:
  case world::Biome::SaltFlat:
  case world::Biome::ColdDesert:
    amount = 0.75;
    break;
  case world::Biome::TemperateRainforest:
  case world::Biome::TropicalRainforest:
  case world::Biome::Wetland:
    amount = 0.85;
    break;
  case world::Biome::Glacier:
  case world::Biome::Tundra:
    amount = 0.55;
    break;
  default:
    amount = 0.35;
    break;
  }
  // A steeper face sheds whatever tries to grow on it, so it stays fresher.
  amount *= 1.0 - 0.5 * std::clamp(slope, 0.0, 1.0);
  return static_cast<uint8_t>(std::clamp(amount, 0.0, 1.0) * 255.0);
}

} // namespace procedural::geometry
