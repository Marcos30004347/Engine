#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world {

// What the designer asks for. The generator is a function of this and nothing
// else: same brief, same world, on any machine.
//
// Tags follow the Perilous Shores convention of a small vocabulary combined
// freely rather than a rigid archetype enum, because that is what lets a brief
// say "archipelago" and "barren" at once without a combinatorial enum.
enum class Tag : uint8_t
{
  // Shape. Exactly one of these should be present; the raiser is selected from
  // it. resolveTags() enforces that.
  Island = 0u,
  Archipelago,
  Coast,
  Bay,
  Fjord,
  Headland,
  Lake,
  Landlocked,

  // Character. Any number, and they modulate the generators rather than the
  // land mask.
  Lowland,
  Highland,
  Barren,
  Verdant,
  Arid,
  Frozen,

  Count,
};

const char *tagName(Tag tag);
bool tagIsShape(Tag tag);

// Cell classification. Ocean and Lake are water; everything else is land.
// Deliberately smaller than the 21-class biome enum: this is *landform intent*
// chosen by the layout, not the biome measured from climate afterwards.
enum class CellType : uint8_t
{
  Ocean = 0u,
  Lake,
  Coast,
  Plain,
  Hill,
  Mountain,
  Plateau,
  Valley,
  Wood,
  Swamp,
  Desert,
  Rocks,
  Cliff,
  Canyon,

  Count,
};

const char *cellTypeName(CellType type);
bool cellTypeIsWater(CellType type);

struct WorldBrief
{
  uint64_t seed = 0u;
  std::vector<Tag> tags;

  // The world is square and this is one side, in metres. 361 km is chosen as
  // the default because it is 130,000 km^2 - England - which is a size a game
  // can plausibly fill. Every quantity in this system is in metres: a generator
  // that thinks in pixels produces a world whose features have no real size.
  double extentMetres = 361000.0;

  // Elevation datum. Land is above, bathymetry below.
  double seaLevelMetres = 0.0;

  // Total relief budget, datum to highest summit. Generators scale their
  // amplitude against this rather than carrying absolute constants, so a world
  // can be made flatter or more dramatic without retuning every cell type.
  double reliefMetres = 3000.0;

  bool valid() const;
};

// Normalizes a tag list: ensures exactly one shape tag (defaulting to Island),
// removes duplicates, and sorts into a canonical order so that two briefs that
// mean the same thing hash the same.
void resolveTags(std::vector<Tag> &tags);

// Selects the shape tag, after resolveTags.
Tag shapeTag(const std::vector<Tag> &tags);

// Stable hash of the brief. Two briefs with the same hash must produce the same
// world; this is what the determinism test compares.
uint64_t worldBriefHash(const WorldBrief &brief);

std::string worldBriefDescription(const WorldBrief &brief);

} // namespace procedural::world
