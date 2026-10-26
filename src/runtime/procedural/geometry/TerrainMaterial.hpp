#pragma once

#include "procedural/PlanetGeology.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapGraph.hpp"
#include "procedural/world/WorldBrief.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geometry
{

// What a face *is*, before anything about where it is.
//
// This is the rule that makes "cliff walls are rock" true regardless of the
// biome the cliff stands in, and it is why the ramp is the case that proves it:
// the same feature carries ground cover on the surface you walk and rock on the
// sides you do not, and neither comes from asking where it is.
enum class SurfaceRole : uint8_t
{
  Ground = 0u,
  // A cliff or terrace face. Always exposed rock, never cover.
  Face,
  // The apron of debris below a face.
  Talus,
  // The walking surface of a ramp: ground, even though it is part of a step.
  RampSurface,
  // The sides of a ramp, which are still face.
  RampShoulder,
  RiverBed,
  RiverBank,
  Water,
  Count,
};

const char *surfaceRoleName(SurfaceRole role);

// One entry of the per-world palette. The mesh carries a byte per face indexing
// this, which at a million faces is a megabyte and is the right price.
struct MaterialEntry
{
  SurfaceRole role = SurfaceRole::Ground;
  PlanetLithology lithology = PlanetLithology::Sedimentary;
  world::Biome cover = world::Biome::Grassland;
  // 0 fresh rock, 1 fully weathered. Multiplies the rock's tabled albedo
  // rather than replacing it: the same sandstone reads differently in a desert
  // and a tundra without becoming a different rock.
  uint8_t weathering = 0u;
  // Which variant of the cover this is: one pasture's green against the next
  // one's. Carried in the palette rather than applied at shading time, because
  // the point of the parcel layer is that a field is *one* material - two
  // neighbours have to differ in the mesh's own data, not only in its colour.
  uint8_t tone = 0u;

  bool operator<(const MaterialEntry &other) const;
  bool operator==(const MaterialEntry &other) const;
};

// Linear sRGB, before lighting.
std::array<float, 3> materialColour(const MaterialEntry &entry);

// De-duplicated palette with stable IDs. Populate in deterministic source order
// before parallel meshing; worker callbacks must only read the palette.
class MaterialPalette
{
public:
  // Index of an entry, adding it if new. At most 256 survive; beyond that the
  // fallback entry is reused, because the byte cannot hold more.
  uint8_t add(const MaterialEntry &entry);
  uint8_t indexOf(const MaterialEntry &entry) const;

  const std::vector<MaterialEntry> &entries() const
  {
    return entries_;
  }
  size_t size() const
  {
    return entries_.size();
  }
  bool saturated() const
  {
    return saturated_;
  }

private:
  std::vector<MaterialEntry> entries_;
  bool saturated_ = false;
};

// Weathering from the climate a face stands in. Biome decides how a rock has
// been *worked on*, not which rock it is - the canyon case is why: a canyon
// crossing three biomes must show the same beds at the same heights on both
// walls, and a biome-keyed rock cannot do that.
uint8_t weatheringFor(world::Biome cover, SurfaceRole role, double slope);

// The rock beneath a cell. The suite is fixed by tectonics and cratons *are*
// tectonics, so a craton index is the honest bridge until a real lithology
// stage exists; the stratum then follows from the surface height, which is what
// makes a canyon wall band and a caprock rim land where it should.
PlanetLithology lithologyForCell(const world::MapNode &cell);

// The 2D generator's own cover classification, as the palette's biome axis.
// It already ran a climate model and classified every cell; re-deriving cover
// from geometry would be a second, worse answer to a settled question.
world::Biome polygonMaterialToBiome(PolygonTerrainMaterial material);

} // namespace procedural::geometry
