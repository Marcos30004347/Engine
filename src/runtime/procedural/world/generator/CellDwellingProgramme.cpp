#include "procedural/world/generator/CellDwellingProgramme.hpp"

#include "procedural/world/generator/CellDwellingClasses.hpp"

#include "procedural/geom2/CellShape.hpp"

#include <algorithm>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;

namespace {

RoomTypeSpec spec(const Use use, const Kind kind, const char *label)
{
  RoomTypeSpec out;
  out.use = use;
  out.kind = kind;
  out.name = label;

  return out;
}

// Watabou's `checkBasic(min, max, maxNarrow, maxDoors)`, which most of his
// types are written in.
RoomTypeSpec basic(const Use use,
                   const Kind kind,
                   const char *label,
                   const uint32_t low,
                   const uint32_t high,
                   const double narrow,
                   const uint32_t doors)
{
  RoomTypeSpec out = spec(use, kind, label);
  out.minimumCells = low;
  out.maximumCells = high;
  out.maximumNarrow = narrow;
  out.maximumDoors = doors;

  return out;
}

} // namespace

RoomFacts measureRoom(const CellGrid &grid, const CellHouse &house, const CellFloor &floor, const uint32_t room)
{
  RoomFacts facts;
  const Board &area = floor.roomAreas[room];

  facts.cells = area.count();
  facts.narrowShare = facts.cells == 0 ? 0. : double(geom2::narrow(grid, area).count()) / double(facts.cells);
  facts.floor = floor.level;

  uint32_t stairs = 0;

  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      if (grid.contains(area, cell))
      {
        ++stairs;
      }
    }
  }

  // Stair cells are not floor you can put anything on, so they do not count
  // toward the room's size - and they DO count as ways in, which is why a room
  // with a flight in it reads as busier than its door count suggests.
  facts.size = facts.cells > stairs ? facts.cells - stairs : 0;

  facts.entrance = std::find(floor.roomContours[room].begin(), floor.roomContours[room].end(), house.entrance) !=
                   floor.roomContours[room].end();

  const bool spiral = house.spiral.has_value() && grid.contains(area, house.spiral->landing);

  uint32_t doors = 0;
  uint32_t ignoring = 0;

  for (const CellDoor &door : floor.doors)
  {
    if (door.a != room && door.b != room)
    {
      continue;
    }

    ++doors;
    const uint32_t other = door.a == room ? door.b : door.a;
    const bool otherIsBath = other < floor.roomKinds.size() && floor.roomKinds[other] == Kind::Bathroom;

    if (!otherIsBath)
    {
      ++ignoring;
    }
  }

  const uint32_t extra = (facts.entrance ? 1u : 0u) + stairs + (spiral ? 1u : 0u);
  facts.doors = doors + extra;
  facts.doorsIgnoringBathrooms = ignoring + extra;

  // How much of the room's wall is the building's wall.
  uint32_t outside = 0;

  for (const uint32_t e : floor.roomContours[room])
  {
    if (exteriorEdge(floor, e))
    {
      ++outside;
    }
  }

  facts.exteriorShare =
    floor.roomContours[room].empty() ? 0. : double(outside) / double(floor.roomContours[room].size());

  facts.basement = floor.level < 0;
  facts.groundFloor = floor.level == 0;
  facts.topFloor = floor.level == int32_t(house.floors.size()) - 1;
  facts.aboveGround = floor.level > 0 || (floor.level == 0 && house.floors.size() == 1);
  facts.singular = floor.roomAreas.size() == 1;
  facts.reservedStair = house.stairwell.has_value() && grid.contains(area, house.stairwell->stair);

  return facts;
}

bool accepts(const RoomTypeSpec &want,
             const RoomFacts &facts,
             const std::vector<RoomFacts> &,
             const std::vector<Kind> &)
{
  if (facts.reservedStair)
  {
    return false;
  }

  if (want.minimumCells.has_value() && facts.size < *want.minimumCells)
  {
    return false;
  }

  if (want.maximumCells.has_value() && facts.size > *want.maximumCells)
  {
    return false;
  }

  if (want.minimumNarrow.has_value() && facts.narrowShare < *want.minimumNarrow)
  {
    return false;
  }

  if (want.maximumNarrow.has_value() && facts.narrowShare > *want.maximumNarrow)
  {
    return false;
  }

  const uint32_t doors = want.ignoreBathroomDoors ? facts.doorsIgnoringBathrooms : facts.doors;

  if (want.maximumDoors.has_value() && doors > *want.maximumDoors)
  {
    return false;
  }

  if (want.minimumExterior.has_value() && facts.exteriorShare < *want.minimumExterior)
  {
    return false;
  }

  if (want.floor.has_value() && facts.floor != *want.floor)
  {
    return false;
  }

  if (want.entrance.has_value() && facts.entrance != *want.entrance)
  {
    return false;
  }

  if (want.topFloor.has_value() && facts.topFloor != *want.topFloor)
  {
    return false;
  }

  if (want.groundFloor.has_value() && facts.groundFloor != *want.groundFloor)
  {
    return false;
  }

  if (want.basement.has_value() && facts.basement != *want.basement)
  {
    return false;
  }

  if (want.aboveGround.has_value() && facts.aboveGround != *want.aboveGround)
  {
    return false;
  }

  if (want.singular.has_value() && facts.singular != *want.singular)
  {
    return false;
  }

  return true;
}

// What a church wants of the rooms the shape did not name: a vestry, and
// then a chapel, a crypt under it, a store. Nothing domestic - the check that
// a church has no kitchen in it is the one that used to fail before the shape
// named every room itself.
RoomProgramme churchProgramme(const CellHouse &house, Stream &stream)
{
  (void)stream;

  RoomTypeSpec vestry = spec(Use::Service, Kind::Vestry, "vestry");
  vestry.minimumCells = 2;
  vestry.maximumCells = 10;
  vestry.aboveGround = false;
  vestry.basement = false;
  vestry.solidDoors = true;
  vestry.windowRatio = .25;

  RoomTypeSpec chapel = spec(Use::Common, Kind::SideChapel, "chapel");
  chapel.minimumCells = 3;
  chapel.basement = false;
  chapel.windowRatio = .5;

  RoomTypeSpec crypt = spec(Use::Store, Kind::Crypt, "crypt");
  crypt.basement = true;
  crypt.minimumCells = 4;

  RoomTypeSpec store = spec(Use::Store, Kind::Store, "store");
  store.maximumCells = 4;
  store.maximumDoors = 1;
  store.solidDoors = true;
  store.windowRatio = 0;

  RoomProgramme out;
  out.required = {vestry};
  out.optional = {chapel, crypt, chapel, store};

  (void)house;

  return out;
}

RoomProgramme regularProgramme(const CellHouse &house, Stream &stream)
{
  const auto tagged=[&](const char *tag){return std::find(house.tags.begin(),house.tags.end(),tag)!=house.tags.end();};
  if (tagged("monastery-refectory") || tagged("monastery-chapter-house") || tagged("monastery-kitchen") || tagged("monastery-dormitory"))
  {
    const bool dorm=tagged("monastery-dormitory"),kitchen=tagged("monastery-kitchen"),chapter=tagged("monastery-chapter-house");
    const auto room=spec(dorm ? Use::Sleeping : kitchen ? Use::Service : Use::Common,
                        dorm ? Kind::Dormitory : kitchen ? Kind::Kitchen : chapter ? Kind::ChapterHouse : Kind::Refectory,
                        dorm ? "dormitory" : kitchen ? "kitchen" : chapter ? "chapter house" : "refectory");
    RoomProgramme out;out.required.push_back(room);
    size_t rooms=0;for (const auto &floor:house.floors) rooms+=floor.roomAreas.size();
    for (size_t i=1;i<rooms;++i) out.optional.push_back(room);
    return out;
  }
  if (house.building == BuildingClass::Church || house.building == BuildingClass::Temple ||
      house.building == BuildingClass::Chapel)
  {
    return churchProgramme(house, stream);
  }

  // The whole of J.1, as data. Each of these is Watabou's own validator read
  // off his source, not a reinterpretation of it.
  RoomTypeSpec salon = spec(Use::Living, Kind::LivingRoom, "living room");
  salon.minimumCells = 5;
  salon.maximumNarrow = .5;
  salon.basement = false;
  salon.windowRatio = .4;

  RoomTypeSpec bathroom = spec(Use::Service, Kind::Bathroom, "bathroom");
  bathroom.maximumCells = 4;
  bathroom.maximumDoors = 1;
  bathroom.entrance = false;
  bathroom.basement = false;
  bathroom.singular = false;
  bathroom.solidDoors = true;

  RoomTypeSpec bedroom = spec(Use::Sleeping, Kind::Bedroom, "bedroom");
  bedroom.aboveGround = true;
  bedroom.minimumCells = 4;
  bedroom.maximumCells = 8;
  bedroom.maximumNarrow = .5;
  bedroom.maximumDoors = 1;
  bedroom.ignoreBathroomDoors = true;
  bedroom.solidDoors = true;
  bedroom.windowRatio = .25;

  RoomTypeSpec kitchen = spec(Use::Service, Kind::Kitchen, "kitchen");
  kitchen.floor = 0;
  kitchen.minimumCells = 4;
  kitchen.maximumCells = 6;
  kitchen.maximumNarrow = .5;
  kitchen.maximumDoors = 2;
  kitchen.solidDoors = true;

  RoomTypeSpec storage = spec(Use::Store, Kind::Store, "storage");
  storage.minimumCells = 2;
  storage.maximumCells = 5;
  storage.maximumDoors = 1;
  storage.entrance = false;
  storage.solidDoors = true;
  storage.windowRatio = 0;

  RoomTypeSpec hall = spec(Use::Hall, Kind::Hall, "hall");
  hall.minimumCells = 2;
  hall.maximumCells = 8;
  hall.entrance = true;

  RoomTypeSpec gallery = spec(Use::Common, Kind::Gallery, "gallery");
  gallery.minimumCells = 6;
  gallery.minimumNarrow = 1;

  RoomTypeSpec cellar = spec(Use::Store, Kind::Cellar, "cellar");
  cellar.minimumCells = 5;
  cellar.maximumNarrow = .99;
  cellar.basement = true;

  RoomTypeSpec attic = spec(Use::Store, Kind::Attic, "attic");
  attic.topFloor = true;
  attic.singular = true;

  RoomTypeSpec study = spec(Use::Living, Kind::Study, "study");
  study.aboveGround = true;
  study.minimumCells = 4;
  study.maximumCells = 6;
  study.maximumNarrow = .5;
  study.maximumDoors = 1;
  study.solidDoors = true;

  RoomTypeSpec library = spec(Use::Living, Kind::Library, "library");
  library.minimumCells = 4;
  library.maximumCells = 9;
  library.maximumNarrow = .8;
  library.maximumDoors = 3;
  library.entrance = false;
  library.basement = false;

  RoomTypeSpec workshop = basic(Use::Service, Kind::Workshop, "workshop", 4, 6, .3, 2);
  workshop.solidDoors = true;

  RoomTypeSpec passage = spec(Use::Corridor, Kind::Passage, "secret passage");
  passage.minimumCells = 3;
  passage.minimumNarrow = 1;
  passage.entrance = false;
  passage.windowRatio = 0;

  RoomTypeSpec observatory = spec(Use::Common, Kind::Observatory, "observatory");
  observatory.topFloor = true;
  observatory.minimumCells = 4;
  observatory.maximumCells = 9;
  observatory.maximumNarrow = .2;
  observatory.maximumDoors = 2;

  RoomTypeSpec laboratory = basic(Use::Service, Kind::Laboratory, "laboratory", 4, 6, .3, 2);
  laboratory.solidDoors = true;

  RoomTypeSpec lookout = spec(Use::Common, Kind::Lookout, "lookout");
  lookout.topFloor = true;
  lookout.maximumCells = 9;
  lookout.minimumExterior = .5;
  lookout.windowRatio = 1;

  RoomTypeSpec greenhouse = spec(Use::Common, Kind::Conservatory, "greenhouse");
  greenhouse.topFloor = true;
  greenhouse.minimumCells = 5;
  greenhouse.maximumCells = 9;
  greenhouse.maximumNarrow = .5;
  greenhouse.maximumDoors = 2;
  greenhouse.windowRatio = 1;

  RoomTypeSpec studio = spec(Use::Living, Kind::Studio, "studio");
  studio.minimumCells = 6;
  studio.maximumCells = 9;
  studio.maximumNarrow = .4;
  studio.maximumDoors = 2;
  studio.entrance = false;
  studio.basement = false;

  RoomTypeSpec vault = spec(Use::Store, Kind::StrongRoom, "vault");
  vault.minimumCells = 4;
  vault.maximumNarrow = .3;
  vault.maximumDoors = 1;
  vault.floor = house.basement.has_value() ? -1 : 0;
  vault.solidDoors = true;
  vault.windowRatio = 0;

  RoomTypeSpec chapel = basic(Use::Common, Kind::SideChapel, "chapel", 4, 6, .2, 1);
  chapel.solidDoors = true;

  RoomTypeSpec trophy = basic(Use::Common, Kind::TrophyRoom, "trophy room", 4, 9, .5, 2);
  RoomTypeSpec armoury = basic(Use::Store, Kind::Armoury, "armoury", 4, 9, .5, 1);
  armoury.windowRatio = 0;

  std::vector<RoomTypeSpec> specials = {study,     library,  workshop, passage, laboratory, observatory,
                                        greenhouse, chapel,  studio,   trophy,  armoury,    lookout,
                                        vault};

  RoomProgramme out;

  // One or two specials, drawn without replacement.
  std::vector<RoomTypeSpec> drawn;
  const uint32_t wanted = 1 + stream.below(2);

  for (uint32_t k = 0; k < wanted && !specials.empty(); ++k)
  {
    drawn.push_back(stream.take(specials));
  }

  out.required = {salon, bathroom, bedroom};

  // What the household's trade needs, as rooms the history named: a smith's
  // house has a forge on the ground floor, a baker's an oven room, a
  // farmstead a byre and a barn, a guardhouse a guardroom, a civic hall a
  // counting house. These are REQUIRED, so the matching gives them ground
  // before it gives any to a second bedroom; without them a smith's house
  // was a cottage with a different label.
  const auto trade = [&](const Use use, const Kind kind, const char *label, const uint32_t low, const uint32_t high) {
    RoomTypeSpec room = basic(use, kind, label, low, high, .5, 3);
    room.floor = 0;
    room.solidDoors = true;
    room.windowRatio = .2;
    room.minimumExterior = .2;
    return room;
  };

  if (tagged("smithy"))
  {
    out.required.push_back(trade(Use::Service, Kind::Forge, "forge", 5, 12));
  }
  if (tagged("bakehouse"))
  {
    out.required.push_back(trade(Use::Service, Kind::Bakehouse, "bakehouse", 4, 10));
  }
  if (tagged("carpentry") || tagged("masonry") || tagged("workshop-trade"))
  {
    out.required.push_back(trade(Use::Service, Kind::Workshop, "workshop", 5, 14));
  }
  if (tagged("brewhouse"))
  {
    out.required.push_back(trade(Use::Service, Kind::Brewhouse, "brewhouse", 4, 10));
  }
  if (tagged("farmstead"))
  {
    out.required.push_back(trade(Use::Service, Kind::Byre, "byre", 5, 16));
    out.optional.push_back(trade(Use::Store, Kind::Barn, "barn", 4, 14));
    out.optional.push_back(trade(Use::Store, Kind::Granary, "granary", 2, 6));
    out.optional.push_back(trade(Use::Service, Kind::Dairy, "dairy", 2, 6));
  }
  if (tagged("guardhouse"))
  {
    out.required.push_back(trade(Use::Common, Kind::Guardroom, "guardroom", 5, 12));
    out.optional.push_back(trade(Use::Store, Kind::Armoury, "armoury", 2, 8));
  }
  if (tagged("hall"))
  {
    out.required.push_back(trade(Use::Living, Kind::CountingHouse, "counting house", 5, 12));
    out.optional.push_back(trade(Use::Common, Kind::CouncilChamber, "council chamber", 6, 16));
  }

  if (!drawn.empty())
  {
    out.required.push_back(drawn.front());
  }

  // What the class has because of what it is.
  //
  // A castle has a chapel and an armoury, and a great house has a chapel: they
  // are not a lucky draw from the same bag a cottage draws a studio from. Added
  // to the REQUIRED list, so the matching will give them ground before it gives
  // any of it to a bedroom.
  const bool fortress = house.building == BuildingClass::Castle;
  const bool great = house.building == BuildingClass::Mansion || house.building == BuildingClass::ManorHouse ||
                     house.building == BuildingClass::Prison;

  if (fortress || great)
  {
    // Loosened from the cottage version, because the cottage version cannot be
    // satisfied here: a chapel that will take only one door has nowhere to go
    // in a building whose rooms all open onto a corridor or a court. Measured,
    // requiring the strict spec placed a chapel in one castle in twenty.
    RoomTypeSpec ownChapel = chapel;
    ownChapel.minimumCells = 4;
    ownChapel.maximumCells = 12;
    ownChapel.maximumNarrow = .5;
    ownChapel.maximumDoors = 3;

    out.required.push_back(ownChapel);
  }

  if (fortress)
  {
    RoomTypeSpec ownArmoury = armoury;
    ownArmoury.minimumCells = 4;
    ownArmoury.maximumCells = 14;
    ownArmoury.maximumNarrow = .5;
    ownArmoury.maximumDoors = 3;

    out.required.push_back(ownArmoury);
  }

  out.optional = {kitchen, storage, hall, gallery};

  for (size_t k = 1; k < drawn.size(); ++k)
  {
    out.optional.push_back(drawn[k]);
  }

  // One of the three ordinary work rooms, if it was not already drawn.
  const std::vector<RoomTypeSpec> ordinary = {study, library, workshop};
  const RoomTypeSpec &pick = ordinary[stream.below(3)];
  const bool already = std::any_of(drawn.begin(), drawn.end(), [&](const RoomTypeSpec &s) { return s.kind == pick.kind; });

  if (!already)
  {
    out.optional.push_back(pick);
  }

  for (size_t k = 1; k < house.floors.size(); ++k)
  {
    out.optional.push_back(bedroom);
  }

  if (house.floors.size() > 1)
  {
    out.optional.push_back(bathroom);
    out.optional.push_back(attic);
  }

  if (house.basement.has_value())
  {
    out.optional.push_back(cellar);
  }

  return out;
}

namespace {

// One room, one purpose: maximum bipartite matching by augmenting paths.
// Kuhn's, which is O(V*E) and on forty rooms against twenty purposes is a
// rounding error next to measuring the rooms in the first place.
bool augment(const uint32_t want,
             const std::vector<std::vector<uint32_t>> &options,
             std::vector<uint32_t> &takenBy,
             std::vector<uint8_t> &visited)
{
  for (const uint32_t room : options[want])
  {
    if (visited[room] != 0)
    {
      continue;
    }

    visited[room] = 1;

    if (takenBy[room] == UINT32_MAX || augment(takenBy[room], options, takenBy, visited))
    {
      takenBy[room] = want;

      return true;
    }
  }

  return false;
}

} // namespace

bool assignProgramme(CellHouse &house, Stream &stream, Ledger &ledger, std::string *error)
{
  std::vector<CellFloor *> all;

  for (CellFloor &floor : house.floors)
  {
    all.push_back(&floor);
  }

  if (house.basement.has_value())
  {
    all.push_back(&house.basement.value());
  }

  // Flatten every room of the house, because the programme is a property of the
  // house and not of a floor: there is one kitchen in a building, not one per
  // storey.
  std::vector<std::pair<CellFloor *, uint32_t>> rooms;

  for (CellFloor *floor : all)
  {
    // A kind the SHAPE settled is not the programme's to overrule. An axial
    // building's nave and chancel are what they are because of where they
    // stand, and a matching that renames the chancel a library has misunderstood
    // what it was given.
    std::vector<Kind> fixed = floor->roomKinds;
    fixed.resize(floor->roomAreas.size(), Kind::Count);

    floor->roomUses.assign(floor->roomAreas.size(), Use::Living);
    floor->roomKinds.assign(floor->roomAreas.size(), Kind::Count);

    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      if (r < floor->roomFixed.size() && floor->roomFixed[r] != 0)
      {
        floor->roomKinds[r] = fixed[r];
      }
    }
    floor->roomWindowRatio.assign(floor->roomAreas.size(), -1.);
    floor->roomSolidDoors.assign(floor->roomAreas.size(), 0);

    if(floor->level==0 && !house.coveredPassage.empty())
      for(uint32_t r=0;r<floor->roomAreas.size();++r)if(floor->roomAreas[r]==house.coveredPassage) {
        floor->roomUses[r]=Use::Corridor;floor->roomKinds[r]=Kind::Passage;floor->roomWindowRatio[r]=0;
      }

    // A court is named before anything is matched and takes no part in the
    // programme. It is a room for the purpose of doors and for nothing else:
    // no household wants a bedroom that is open to the sky, and offering one to
    // the matching is how a castle ends up with its kitchen in the bailey.
    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      if (r < floor->roomOutdoor.size() && floor->roomOutdoor[r] != 0)
      {
        // A yard or a court, read off its edges: a court is enclosed, a yard
        // has a side with nothing beyond it but the lot.
        bool open = false;

        for (const uint32_t e : floor->roomContours[r])
        {
          const uint32_t twin = house.grid.twin(e);
          const uint32_t beyond = twin == geom2::kNoEdge ? geom2::kNoCell : house.grid.edgeCell(twin);

          if (beyond == geom2::kNoCell ||
              (!house.grid.contains(floor->area, beyond) && !house.grid.contains(house.courts, beyond)))
          {
            open = true;
            break;
          }
        }

        const bool pen = !house.pens.empty() && (floor->roomAreas[r] & ~house.pens).empty();
        floor->roomKinds[r] = pen ? Kind::Pen : (open ? Kind::Yard : Kind::Courtyard);
        floor->roomWindowRatio[r] = 0;
      }
    }

    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      if (r < floor->roomOutdoor.size() && floor->roomOutdoor[r] != 0)
      {
        continue;
      }

      if (r < floor->roomFixed.size() && floor->roomFixed[r] != 0)
      {
        continue;
      }

      rooms.emplace_back(floor, r);
    }
  }

  std::vector<RoomFacts> facts;
  facts.reserve(rooms.size());

  for (const auto &[floor, r] : rooms)
  {
    facts.push_back(measureRoom(house.grid, house, *floor, r));
  }

  RoomProgramme programme = regularProgramme(house, stream);
  const bool mixedUse=house.building==BuildingClass::ShopHouse || house.building==BuildingClass::Inn ||
      house.building==BuildingClass::CommerceHouse;
  if (mixedUse)
  {
    auto business=spec(Use::Shop,house.building==BuildingClass::Inn ? Kind::Bar : Kind::Shop,"ground-floor business");
    business.floor=0;
    programme.required.insert(programme.required.begin(),business);
  }

  std::vector<RoomTypeSpec> wanted = programme.required;
  wanted.insert(wanted.end(), programme.optional.begin(), programme.optional.end());

  // Which rooms each purpose could go in, shuffled so that among the maximum
  // matchings the one taken is still drawn from the stream.
  std::vector<std::vector<uint32_t>> options(wanted.size());
  std::vector<uint32_t> order;

  for (uint32_t r = 0; r < uint32_t(rooms.size()); ++r)
  {
    order.push_back(r);
  }

  for (uint32_t w = 0; w < uint32_t(wanted.size()); ++w)
  {
    for (const uint32_t r : stream.shuffled(order))
    {
      if (mixedUse && ((facts[r].floor==0 && (wanted[w].use==Use::Sleeping || wanted[w].use==Use::Living)) ||
                      (facts[r].floor!=0 && wanted[w].use==Use::Shop))) continue;
      if (accepts(wanted[w], facts[r], facts, {}))
      {
        options[w].push_back(r);
      }
    }
  }

  std::vector<uint32_t> takenBy(rooms.size(), UINT32_MAX);
  uint32_t placed = 0;

  // The required set first, so that when there is not room for everything it is
  // the optional purposes that go without.
  for (uint32_t pass = 0; pass < 2; ++pass)
  {
    const uint32_t from = pass == 0 ? 0u : uint32_t(programme.required.size());
    const uint32_t to = pass == 0 ? uint32_t(programme.required.size()) : uint32_t(wanted.size());

    for (uint32_t w = from; w < to; ++w)
    {
      std::vector<uint8_t> visited(rooms.size(), 0);

      if (augment(w, options, takenBy, visited))
      {
        ++placed;
      }
    }
  }

  for (uint32_t r = 0; r < uint32_t(rooms.size()); ++r)
  {
    auto &[floor, index] = rooms[r];

    if (takenBy[r] != UINT32_MAX)
    {
      floor->roomUses[index] = wanted[takenBy[r]].use;
      floor->roomKinds[index] = wanted[takenBy[r]].kind;
      floor->roomSolidDoors[index] = wanted[takenBy[r]].solidDoors ? 1 : 0;

      if (wanted[takenBy[r]].windowRatio.has_value())
      {
        floor->roomWindowRatio[index] = *wanted[takenBy[r]].windowRatio;
      }

      continue;
    }

    // What is left, by what it is rather than by what it was wanted for.
    const RoomFacts &f = facts[r];

    if (f.reservedStair)
    {
      floor->roomUses[index] = Use::Corridor;
      floor->roomKinds[index] = Kind::Staircase;
    }
    else if (f.doors - (f.entrance ? 1u : 0u) > 1 && f.narrowShare >= 1.)
    {
      floor->roomUses[index] = Use::Corridor;
      floor->roomKinds[index] = Kind::Passage;
    }
    else if (f.size <= 2)
    {
      floor->roomUses[index] = Use::Store;
      floor->roomKinds[index] = Kind::Closet;
    }
    else
    {
      floor->roomUses[index] = Use::Living;
      floor->roomKinds[index] = Kind::Count;
    }
  }

  if (mixedUse)
    for (auto &[floor,index]:rooms)
    {
      auto &use=floor->roomUses[index]; auto &kind=floor->roomKinds[index];
      if (floor->level==0 && (use==Use::Living || use==Use::Sleeping))
      { use=Use::Shop;kind=house.building==BuildingClass::Inn ? Kind::Bar : Kind::Shop; }
      else if (floor->level>0 && (use==Use::Shop || (use==Use::Living && kind==Kind::Count)))
      { use=Use::Sleeping;kind=Kind::Bedroom;floor->roomSolidDoors[index]=1; }
    }

  if (placed < programme.required.size())
  {
    ledger.add(LedgerKind::Omitted, 0, "not every required room found a place");
  }

  (void)error;

  return true;
}

} // namespace procedural::world::generator
