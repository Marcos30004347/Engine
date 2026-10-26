#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <optional>
#include <string>
#include <vector>

namespace procedural::world::generator {

// NOT `using rewrite::RoomUse` - `ArchitecturalProgram.hpp` in this same
// namespace already has `using Use = rewrite::RoomKind`, so the short name
// means the FINE vocabulary there and the coarse one here. Written out in full
// rather than fought over.
using Use = rewrite::RoomUse;
using Kind = rewrite::RoomKind;

// What a room has to be, for a purpose to be put in it.
//
// Declarative, and that is the point. Watabou ships two alternative room
// programmes - `gothic` and `tavern` - that contain NO CODE: they are JSON
// files compiled into predicates by eleven optional fields. Every one of his
// twenty-six hardcoded validators is expressible in that schema, and he proves
// it by shipping two that are. So the schema is the primary form here and the
// "regular" programme below is written in it, rather than as twenty-six
// lambdas that a new building type would have to be added to.
//
// Every field is optional. Absent means "do not care".
struct RoomTypeSpec
{
  Use use = Use::Living;
  Kind kind = Kind::Count;
  std::string name;

  std::optional<uint32_t> minimumCells;
  std::optional<uint32_t> maximumCells;
  std::optional<double> minimumNarrow; // Share of the room that is narrow.
  std::optional<double> maximumNarrow;
  std::optional<uint32_t> maximumDoors;

  // The exterior wall fraction. Watabou's `lookout` is the only validator in
  // his whole set that measures this, and it generalises: a shop wants street
  // frontage and a scullery wants none.
  std::optional<double> minimumExterior;

  std::optional<int32_t> floor;    // Exactly this level.
  std::optional<bool> entrance;    // Carries the front door, or does not.
  std::optional<bool> topFloor;
  std::optional<bool> groundFloor;
  std::optional<bool> basement;
  std::optional<bool> aboveGround;
  std::optional<bool> singular;    // The only room on its floor.

  // Doors to it are real doors rather than open arches, and how much of its
  // outer wall is glazed. Consumed at phase 8.
  bool solidDoors = false;
  std::optional<double> windowRatio;

  // Doors to a bathroom do not count toward `maximumDoors`. Watabou's, and
  // only his bedroom uses it - a bedroom with its own closet is still a
  // bedroom with one door.
  bool ignoreBathroomDoors = false;
};

// Everything about a room the predicates ask after, measured once.
struct RoomFacts
{
  uint32_t cells = 0;
  uint32_t size = 0;      // Cells less the ones a stair stands in.
  double narrowShare = 0;
  uint32_t doors = 0;     // Doors, plus the front door, plus stairs, plus the spiral.
  uint32_t doorsIgnoringBathrooms = 0;
  double exteriorShare = 0;
  int32_t floor = 0;
  bool entrance = false;
  bool topFloor = false;
  bool groundFloor = false;
  bool basement = false;
  bool aboveGround = false;
  bool singular = false;
  bool reservedStair = false;
};

RoomFacts measureRoom(const CellGrid &, const CellHouse &, const CellFloor &, uint32_t room);

bool accepts(const RoomTypeSpec &, const RoomFacts &, const std::vector<RoomFacts> &all, const std::vector<Kind> &assigned);

// The programme a house wants: what it must have, and what it would like.
struct RoomProgramme
{
  std::vector<RoomTypeSpec> required;
  std::vector<RoomTypeSpec> optional;
};

// The ordinary dwelling, written in the schema.
RoomProgramme regularProgramme(const CellHouse &, Stream &);

// Fits purposes to rooms.
//
// Maximum bipartite matching, not Watabou's search. His `assignSet` shuffles
// the rooms, greedily first-fits, counts what it could not place, repeats that
// `rooms x types` times and replays the best attempt's seed - which is a Monte
// Carlo approximation of exactly this, and the exact answer is microseconds on
// a graph this size. Randomness is kept by shuffling each room's candidate list
// before the matching runs, so among maximum matchings the one taken is still
// seeded.
bool assignProgramme(CellHouse &, Stream &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
