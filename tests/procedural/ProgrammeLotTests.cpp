// A programme building on the ground the town actually gave it.
//
// Every other test of this generator hands the planner a rectangle, and a
// rectangle is the one shape a lot allocated by a settlement almost never is:
// the lots here are wedges off bent streets, burgage strips four times as deep
// as they are wide, and the leftover triangles where three roads meet. They
// also vary enormously in size, which is the part that breaks a generator -
// a programme sampled for a merchant's house does not go on forty square
// metres, and the honest answer there is a smaller programme, not a smaller
// house.
//
// So this measures the two things a lot can tell you that a rectangle cannot:
// how much of the building ends up standing off its own ground, and how much
// of the town gets a building at all.
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"
#include "procedural/world/generator/SettlementLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::world::generator;
namespace rewrite = procedural::world::rewrite;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

// --- real lots -----------------------------------------------------------

struct Lot
{
  std::vector<rewrite::Position> ring;
  rewrite::DistrictUse use = rewrite::DistrictUse::Residential;
  double area = 0, aspect = 1;
  uint64_t id = 0;
  bool village = false;
};

double area(const std::vector<rewrite::Position> &ring)
{
  double twice = 0;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    twice += ring[j].x * ring[i].z - ring[i].x * ring[j].z;
  }

  return std::abs(twice) * .5;
}

bool inside(const std::vector<rewrite::Position> &ring, const double x, const double z)
{
  bool in = false;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const bool straddles = (ring[i].z > z) != (ring[j].z > z);

    if (straddles
        && x < (ring[j].x - ring[i].x) * (z - ring[i].z) / (ring[j].z - ring[i].z) + ring[i].x)
    {
      in = !in;
    }
  }

  return in;
}

// The settlement, run for real, with its lots taken out of it. Flat ground and
// a hashed random, which is what the fabric adapter passes it.
std::vector<Lot> lotsOf(const uint64_t seed, const uint32_t population, const bool village)
{
  SettlementLayoutInput input;

  const double half = village ? 150. : 230.;
  input.region = {{-half, 0, -half}, {half, 0, -half}, {half, 0, half}, {-half, 0, half}};
  input.centre = {0, 0, 0};
  input.exits = {{-half * 2, 0, -20}, {half * 2, 0, 40}, {30, 0, half * 2}};
  input.population = population;
  input.farmsteads = village;
  input.seed = seed;
  input.revision = 1;
  input.terrain = [](double, double) { return 0.; };
  input.random = [seed](const uint64_t id, const uint64_t purpose) {
    uint64_t h = (seed + 1) * 0x9E3779B97F4A7C15ull ^ (id + 1) * 0xBF58476D1CE4E5B9ull
                 ^ (purpose + 1) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 29;

    return double(h % 1000000) / 1000000.;
  };

  rewrite::CityLayout layout;
  Ledger ledger;
  std::string error;

  if (!SettlementLayout::plan(input, layout, ledger, &error))
  {
    std::cerr << "  the settlement did not plan: " << error << "\n";

    return {};
  }

  std::vector<Lot> lots;
  uint32_t allocated = 0;

  // The roads-first settlement allocates its ground in blocks, and the lots
  // are Watabou's: a block cut by alleys into strips that each front a street.
  // That is the ground a building is actually given, and it is the input this
  // whole test is about.
  for (const rewrite::CityStreetBlock &block : layout.blocks)
  {
    for (const std::vector<rewrite::Position> &ring : block.lots)
    {
      ++allocated;

      if (ring.size() < 3)
      {
        continue;
      }

      Lot lot;
      lot.ring = ring;
      lot.use = block.use;
      lot.village = village;
      lot.id = block.id * 64ull + allocated;
      lot.area = area(ring);

      double lowX = ring.front().x, highX = lowX;
      double lowZ = ring.front().z, highZ = lowZ;

      for (const rewrite::Position &point : ring)
      {
        lowX = std::min(lowX, point.x);
        highX = std::max(highX, point.x);
        lowZ = std::min(lowZ, point.z);
        highZ = std::max(highZ, point.z);
      }

      const double a = std::max(highX - lowX, highZ - lowZ);
      const double b = std::max(std::min(highX - lowX, highZ - lowZ), 1e-6);
      lot.aspect = a / b;

      if (lot.area > 8.)
      {
        lots.push_back(std::move(lot));
      }
    }
  }

  std::printf("    settlement %llu: %zu blocks, %u lots allocated, %zu worth building on\n",
              (unsigned long long)seed, layout.blocks.size(), allocated, lots.size());

  return lots;
}

// The district the lot is in says what stands on it. The town already decides
// this - a guild ward, a craftsmen's ward, a temple precinct - and every one of
// those has a network of its own, so the class does not have to be guessed.
BuildingClass classOf(const Lot &lot)
{
  // A village is not a small town. Its houses are longhouses: one range with
  // the family at one end and the beasts at the other, a croft behind, and no
  // kitchen because the house-place is the kitchen. Handing those lots to the
  // town's dwelling network would furnish a labourer's holding with a parlour.
  if (lot.village && lot.use != rewrite::DistrictUse::Manor)
  {
    return lot.area > 260. ? BuildingClass::Farmstead : BuildingClass::PeasantCottage;
  }

  switch (lot.use)
  {
    case rewrite::DistrictUse::Merchant: return BuildingClass::CommerceHouse;
    case rewrite::DistrictUse::Craftsmen: return BuildingClass::ShopHouse;
    case rewrite::DistrictUse::Guild: return BuildingClass::GuildHall;
    case rewrite::DistrictUse::Temple: return BuildingClass::Temple;
    case rewrite::DistrictUse::Church: return BuildingClass::Church;
    case rewrite::DistrictUse::Manor: return BuildingClass::ManorHouse;
    case rewrite::DistrictUse::Prison: return BuildingClass::Prison;
    case rewrite::DistrictUse::Harbor: return BuildingClass::Warehouse;
    case rewrite::DistrictUse::Civic: return BuildingClass::School;
    default: return BuildingClass::Dwelling;
  }
}

PlotStanding standingOf(const double lotArea, const bool village = false)
{
  if (village)
  {
    // On a holding, the size of the plot is the size of the holding, and even
    // a large one is a rural household rather than a wealthy one.
    return lotArea > 300. ? PlotStanding::Rural
           : lotArea > 60. ? PlotStanding::Rural
                           : PlotStanding::Slum;
  }

  // The lot is the only evidence of standing a town object carries, and it is
  // a good one: a merchant's plot is four times a cottager's.
  return lotArea > 420. ? PlotStanding::Wealthy
         : lotArea > 190. ? PlotStanding::Comfortable
         : lotArea > 90.  ? PlotStanding::Modest
                          : PlotStanding::Slum;
}

// --- what a fitting measures ---------------------------------------------

struct Fitting
{
  uint32_t offered = 0, built = 0, refused = 0;
  double doorsDelivered = 0, areaError = 0;

  // The share of built floor area standing outside its own lot. The number
  // this test exists for.
  double spilled = 0;
  double worstSpill = 0;
  uint32_t spilling = 0;

  double rooms = 0;

  // The plan's dimension cost against the one the programme's own sampled
  // areas would score. §5.2's actual criterion, and the only fair one: the
  // sampled area is a draw from a distribution and not a target, so a plan
  // can be a long way from it and still be more likely than it.
  double dimensionCost = 0, sampledCost = 0;

  // The shell against the plan. §6 builds the walls from the floor plan, so
  // the storey plates and the rooms must be the same building.
  double shellGap = 0;

  // Rooms standing on the same ground as another room. A plan is a partition
  // of its floor: whatever else is wrong with one, two rooms cannot both have
  // the same square metre.
  double overlap = 0;
  uint32_t overlapping = 0;

  // A building nobody can get into is not a building.
  uint32_t withEntrance = 0;

  // The lot, once the building and its outdoor ground are both on it. A
  // cottage covering a fifth of its plot is right; a cottage covering a fifth
  // of its plot with the other four fifths unallocated is a cottage in a field.
  double lotUsed = 0;
  uint32_t grounds = 0;

  uint32_t residents = 0, workers = 0;
  uint32_t overlappingGround = 0;

  // Multi-storey buildings, and the ones whose stair arrives somewhere. The
  // paper's own failure case is a staircase "blocked on the second story".
  uint32_t storeyed = 0, connected = 0, missingClimb = 0, climbs = 0;
  double climbOverlap = 0;
};

// Room area outside the lot, by sampling the room on a fine lattice. A room is
// rectilinear and the lot is not, so there is no closed form worth writing for
// an answer that only has to be right to a hand's breadth.
double outsideShare(const std::vector<rewrite::Position> &lot, const geom2::Polygon &room)
{
  if (room.outer.size() < 3)
  {
    return 0;
  }

  double lowX = geom2::toMetres(room.outer.front().x), highX = lowX;
  double lowZ = geom2::toMetres(room.outer.front().y), highZ = lowZ;

  for (const geom2::Point &point : room.outer)
  {
    lowX = std::min(lowX, geom2::toMetres(point.x));
    highX = std::max(highX, geom2::toMetres(point.x));
    lowZ = std::min(lowZ, geom2::toMetres(point.y));
    highZ = std::max(highZ, geom2::toMetres(point.y));
  }

  std::vector<rewrite::Position> ring;

  for (const geom2::Point &point : room.outer)
  {
    ring.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
  }

  uint32_t in = 0, out = 0;

  for (uint32_t j = 0; j < 24; ++j)
  {
    const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 24.;

    for (uint32_t i = 0; i < 24; ++i)
    {
      const double x = lowX + (highX - lowX) * (double(i) + .5) / 24.;

      if (!inside(ring, x, z))
      {
        continue;
      }

      (inside(lot, x, z) ? in : out) += 1;
    }
  }

  return in + out > 0 ? double(out) / double(in + out) : 0.;
}

// The same gallery ProgrammeDwellingTests writes, with the lot the building
// was given drawn around it - which is the whole question here.
void writeGallery(const std::string &path,
                  const std::vector<std::pair<std::string, ProgrammeBuilding>> &gallery,
                  const std::vector<std::vector<rewrite::Position>> &lots)
{
  std::ofstream file(path);

  if (!file)
  {
    std::cerr << "  could not write " << path << "\n";

    return;
  }

  const auto escape = [](const std::string &text) {
    std::string out;

    for (const char ch : text)
    {
      if (ch == '"' || ch == '\\')
      {
        out.push_back('\\');
      }

      out.push_back(ch);
    }

    return out;
  };

  file << "{\n \"buildings\": [\n";

  for (size_t b = 0; b < gallery.size(); ++b)
  {
    const ProgrammeBuilding &building = gallery[b].second;
    const Programme &programme = building.programme;

    file << "  {\n";
    file << "   \"label\": \"" << escape(gallery[b].first) << "\",\n";
    file << "   \"class\": \"" << name(programme.building) << "\",\n";
    file << "   \"storeys\": " << programme.storeys << ",\n";
    file << "   \"basement\": " << (programme.basement ? "true" : "false") << ",\n";
    file << "   \"footprintX\": " << programme.footprintX << ",\n";
    file << "   \"footprintZ\": " << programme.footprintZ << ",\n";
    file << "   \"areaError\": " << building.areaError << ",\n";
    file << "   \"doorsDelivered\": " << building.doorsDelivered << ",\n";

    file << "   \"lot\": [";

    if (b < lots.size())
    {
      for (size_t i = 0; i < lots[b].size(); ++i)
      {
        file << (i ? ", " : "") << "[" << lots[b][i].x << ", " << lots[b][i].z << "]";
      }
    }

    file << "],\n";

    file << "   \"outline\": [";

    for (size_t i = 0; i < building.footprint.size(); ++i)
    {
      file << (i ? ", " : "") << "[" << building.footprint[i].first << ", "
           << building.footprint[i].second << "]";
    }

    file << "],\n   \"rooms\": [\n";

    for (size_t r = 0; r < building.rooms.size(); ++r)
    {
      const PlacedRoom &room = building.rooms[r];

      file << "    {\"use\": \"" << name(room.use) << "\", \"floor\": " << room.floor
           << ", \"asked\": " << room.askedMetres
           << ", \"got\": " << room.achievedMetres
           << ", \"street\": "
           << (programme.rooms[room.programmeRoom].frontsStreet ? "true" : "false")
           << ", \"outline\": [";

      for (size_t i = 0; i < room.boundary.outer.size(); ++i)
      {
        file << (i ? ", " : "") << "[" << geom2::toMetres(room.boundary.outer[i].x)
             << ", " << geom2::toMetres(room.boundary.outer[i].y) << "]";
      }

      file << "]}" << (r + 1 < building.rooms.size() ? "," : "") << "\n";
    }

    file << "   ],\n   \"doors\": [\n";

    for (size_t d = 0; d < building.doors.size(); ++d)
    {
      const PlacedDoor &door = building.doors[d];

      file << "    {\"a\": " << door.a << ", \"b\": " << door.b
           << ", \"door\": " << (door.door ? "true" : "false")
           << ", \"x\": " << door.x << ", \"z\": " << door.z << "}"
           << (d + 1 < building.doors.size() ? "," : "") << "\n";
    }

    file << "   ],\n   \"grounds\": [\n";

    for (size_t g = 0; g < building.grounds.size(); ++g)
    {
      const PlacedGround &ground = building.grounds[g];

      file << "    {\"use\": \"" << name(ground.use) << "\", \"got\": "
           << ground.achievedMetres << ", \"outline\": [";

      for (size_t i = 0; i < ground.boundary.outer.size(); ++i)
      {
        file << (i ? ", " : "") << "[" << geom2::toMetres(ground.boundary.outer[i].x) << ", "
             << geom2::toMetres(ground.boundary.outer[i].y) << "]";
      }

      file << "]}" << (g + 1 < building.grounds.size() ? "," : "") << "\n";
    }

    file << "   ],\n   \"entrances\": [\n";

    for (size_t e = 0; e < building.entrances.size(); ++e)
    {
      const PlacedEntrance &entrance = building.entrances[e];

      file << "    {\"room\": " << entrance.room << ", \"x\": " << entrance.x
           << ", \"z\": " << entrance.z << "}"
           << (e + 1 < building.entrances.size() ? "," : "") << "\n";
    }

    file << "   ],\n   \"household\": \"" << escape(building.household.description)
         << "\",\n   \"residents\": " << building.household.residents
         << ",\n   \"workers\": " << building.household.workers
         << ",\n   \"wealth\": " << building.household.wealth << "\n";

    file << "  }" << (b + 1 < gallery.size() ? "," : "") << "\n";
  }

  file << " ]\n}\n";

  std::cout << "  wrote " << gallery.size() << " buildings to " << path << "\n";
}

}  // namespace

int main()
{
  std::cout << "a programme building on a lot the town allocated\n";

  // --- the lots themselves --------------------------------------------------

  std::vector<Lot> lots = lotsOf(20260915ull, 900, false);
  const std::vector<Lot> village = lotsOf(4771ull, 260, true);

  lots.insert(lots.end(), village.begin(), village.end());

  std::cout << "\n  the ground the town handed over\n";
  check(lots.size() >= 40, "the settlement allocated lots to build on");

  if (lots.empty())
  {
    return 1;
  }

  std::vector<double> areas, aspects;

  for (const Lot &lot : lots)
  {
    areas.push_back(lot.area);
    aspects.push_back(lot.aspect);
  }

  std::sort(areas.begin(), areas.end());
  std::sort(aspects.begin(), aspects.end());

  const auto at = [](const std::vector<double> &sorted, const double q) {
    return sorted[std::min(sorted.size() - 1, size_t(q * double(sorted.size())))];
  };

  std::printf("    %zu lots: area %.0f / %.0f / %.0f m2 (least, median, most),"
              " aspect %.1f / %.1f / %.1f\n",
              lots.size(), areas.front(), at(areas, .5), areas.back(), aspects.front(),
              at(aspects, .5), aspects.back());

  // They have to vary, or this test is measuring rectangles again with more
  // steps.
  check(areas.back() > areas.front() * 4., "the lots vary in size by more than a factor of four");
  check(at(aspects, .9) > 1.6, "a tenth of the lots are long and narrow");

  // --- build on them ---------------------------------------------------------

  ProgrammePlanSettings settings;
  ComplexDwellingSettings massing;

  FloorPlanSettings optimise;
  // The paper's count. Figure 6 runs to a hundred thousand and shows
  // twenty thousand still visibly short of it.
  optimise.iterations = 100000;

  Fitting fitting;
  std::vector<std::pair<std::string, ProgrammeBuilding>> gallery;
  std::vector<std::vector<rewrite::Position>> galleryLots;
  std::map<std::string, uint32_t> refusals;

  // Sampled across the whole range, and the village lots are at the end of the
  // list, so a plain stride would take a town survey and call it a settlement.
  std::map<std::string, uint32_t> byClass, dropped;

  const size_t stride = std::max<size_t>(1, lots.size() / 48);

  for (size_t i = 0; i < lots.size(); i += stride)
  {
    const Lot &lot = lots[i];

    ProgrammeBrief brief;
    brief.building = classOf(lot);
    brief.standing = standingOf(lot.area, lot.village);

    ProgrammeBuilding building;
    rewrite::DwellingPlan plan;
    std::string error;

    ++fitting.offered;

    if (!planProgrammeDwelling(brief, 0x51D5u + lot.id * 31ull, settings, massing, lot.ring,
                               building, plan, &error, &optimise))
    {
      ++fitting.refused;
      refusals[error] += 1;

      continue;
    }

    double asked = 0, got = 0, aboveGround = 0;

    for (const PlacedRoom &room : building.rooms)
    {
      asked += room.askedMetres;
      got += room.achievedMetres;
      aboveGround += room.floor >= 0 ? room.achievedMetres : 0.;
    }

    byClass[name(building.programme.building)] += 1;

    // Which of the programme's rooms did not make it onto the plan.
    {
      std::set<uint32_t> placedRooms;

      for (const PlacedRoom &room : building.rooms)
      {
        placedRooms.insert(room.programmeRoom);
      }

      for (uint32_t r = 0; r < building.programme.rooms.size(); ++r)
      {
        if (!placedRooms.count(r) && enclosesFloorArea(building.programme.rooms[r].use))
        {
          dropped[name(building.programme.rooms[r].use)] += 1;
        }
      }
    }

    ++fitting.built;
    fitting.doorsDelivered += building.doorsDelivered;
    fitting.areaError += building.areaError;
    fitting.rooms += double(building.roomsPlaced);
    fitting.dimensionCost += building.layoutCost.dimensions;
    fitting.withEntrance += building.entrances.empty() ? 0 : 1;
    fitting.grounds += uint32_t(building.grounds.size());
    fitting.residents += building.household.residents;
    fitting.workers += building.household.workers;

    {
      double allocated = 0;

      for (const PlacedGround &ground : building.grounds)
      {
        allocated += ground.achievedMetres;
      }

      for (const rewrite::DwellingStorey &storey : plan.storeys)
      {
        // The lowest storey standing above ground. A cellar is a storey too
        // and numbers itself level zero, so the level alone no longer tells
        // the ground floor from the one dug under it - the height does.
        allocated += storey.base >= 0 && storey.base < 1.5 ? area(storey.outer) : 0.;
      }

      fitting.lotUsed += std::min(1., allocated / std::max(lot.area, 1.));

      // The ground outside is outside. If the leftover was taken as the whole
      // lot - because the difference failed - the yard runs under the house,
      // and "the lot is allocated" would be true only by double counting it.
      if (allocated > lot.area * 1.05)
      {
        ++fitting.overlappingGround;
      }
    }
    fitting.sampledCost += building.sampledDimensionCost;

    double total = 0, out = 0;

    for (const PlacedRoom &room : building.rooms)
    {
      const double share = outsideShare(lot.ring, room.boundary);
      total += room.achievedMetres;
      out += room.achievedMetres * share;
    }

    const double spill = total > 1e-6 ? out / total : 0.;
    fitting.spilled += spill;
    fitting.worstSpill = std::max(fitting.worstSpill, spill);

    if (spill > .02)
    {
      ++fitting.spilling;
    }

    // The ground floor only. An upper storey is jettied - it oversails the
    // floor below by a hand's breadth on every side - so its plate is larger
    // than its rooms on purpose, and comparing it to them would report the
    // jetty as a defect.
    double plates = 0, groundRooms = 0;

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      plates += storey.base >= 0 && storey.base < 1.5 ? area(storey.outer) : 0.;
    }

    for (const PlacedRoom &room : building.rooms)
    {
      groundRooms += room.floor == 0 ? room.achievedMetres : 0.;
    }

    // How much of the ground floor's own plate stands off the lot, by
    // sampling. The rooms are inside the plate by construction now, so if the
    // plate leaves the lot so do they, and the two have to be told apart.
    // Does any room stand on another? Sampled, because the polygons are
    // rectilinear but not axis-aligned once the plan is turned onto its lot.
    {
      uint32_t shared = 0, tested = 0;

      for (size_t a = 0; a < building.rooms.size(); ++a)
      {
        const geom2::Polygon &first = building.rooms[a].boundary;

        if (first.outer.size() < 3)
        {
          continue;
        }

        std::vector<rewrite::Position> ringA;

        for (const geom2::Point &point : first.outer)
        {
          ringA.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
        }

        for (size_t b = 0; b < building.rooms.size(); ++b)
        {
          if (a == b || building.rooms[a].floor != building.rooms[b].floor
              || building.rooms[b].boundary.outer.size() < 3)
          {
            continue;
          }

          std::vector<rewrite::Position> ringB;

          for (const geom2::Point &point : building.rooms[b].boundary.outer)
          {
            ringB.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
          }

          // Sampled over B's own extent: a point inside B that is also
          // inside A is ground both rooms claim. Sampling B's corners or its
          // centroid instead reports overlaps that are not there, because a
          // rectilinear room can be L-shaped and its centroid outside it.
          double lowX = ringB.front().x, highX = lowX;
          double lowZ = ringB.front().z, highZ = lowZ;

          for (const rewrite::Position &point : ringB)
          {
            lowX = std::min(lowX, point.x);
            highX = std::max(highX, point.x);
            lowZ = std::min(lowZ, point.z);
            highZ = std::max(highZ, point.z);
          }

          for (uint32_t j = 0; j < 14; ++j)
          {
            const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 14.;

            for (uint32_t i = 0; i < 14; ++i)
            {
              const double x = lowX + (highX - lowX) * (double(i) + .5) / 14.;

              if (!inside(ringB, x, z))
              {
                continue;
              }

              ++tested;
              shared += inside(ringA, x, z) ? 1 : 0;
            }
          }
        }
      }

      const double share = tested > 0 ? double(shared) / double(tested) : 0.;
      fitting.overlap += share;
      fitting.overlapping += share > 0 ? 1 : 0;
    }

    // Does the stair arrive? For every floor above the ground, the landing or
    // staircase on it has to stand over the one below.
    {
      std::map<int32_t, const PlacedRoom *> climb;

      for (const PlacedRoom &room : building.rooms)
      {
        if (room.use != RoomUse::Staircase && room.use != RoomUse::Landing)
        {
          continue;
        }

        const auto found = climb.find(room.floor);

        if (found == climb.end()
            || (room.use == RoomUse::Staircase && found->second->use == RoomUse::Landing))
        {
          climb[room.floor] = &room;
        }
      }

      int32_t highest = 0;

      for (const PlacedRoom &room : building.rooms)
      {
        highest = std::max(highest, room.floor);
      }

      if (highest > 0)
      {
        ++fitting.storeyed;
        uint32_t missingClimb = 0;

        bool reaches = true;

        for (int32_t level = 1; level <= highest; ++level)
        {
          const auto above = climb.find(level);
          const auto below = climb.find(level - 1);

          if (above == climb.end() || below == climb.end())
          {
            reaches = false;
            ++missingClimb;

            break;
          }

          std::vector<rewrite::Position> ringBelow;

          for (const geom2::Point &point : below->second->boundary.outer)
          {
            ringBelow.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
          }

          // How much of the *flight below* is covered by the landing above it.
          // That is the question a stair asks: the hole in the upper floor has
          // to be over the steps, and you arrive on the landing through it.
          // Asking the reverse - how much of the landing is over the flight -
          // scores a generous landing as a failure for being larger than the
          // staircase under it, which is what a landing is.
          std::vector<rewrite::Position> ringAbove;

          for (const geom2::Point &point : above->second->boundary.outer)
          {
            ringAbove.push_back({geom2::toMetres(point.x), 0, geom2::toMetres(point.y)});
          }

          double lowX = ringBelow.front().x, highX = lowX;
          double lowZ = ringBelow.front().z, highZ = lowZ;

          for (const rewrite::Position &point : ringBelow)
          {
            lowX = std::min(lowX, point.x);
            highX = std::max(highX, point.x);
            lowZ = std::min(lowZ, point.z);
            highZ = std::max(highZ, point.z);
          }

          uint32_t on = 0, all = 0;

          for (uint32_t sj = 0; sj < 16; ++sj)
          {
            const double z = lowZ + (highZ - lowZ) * (double(sj) + .5) / 16.;

            for (uint32_t si = 0; si < 16; ++si)
            {
              const double x = lowX + (highX - lowX) * (double(si) + .5) / 16.;

              if (!inside(ringBelow, x, z))
              {
                continue;
              }

              ++all;
              on += inside(ringAbove, x, z) ? 1 : 0;
            }
          }

          const double share = all > 0 ? double(on) / double(all) : 0.;
          fitting.climbOverlap += share;
          ++fitting.climbs;

          reaches = reaches && share > .5;
        }

        fitting.connected += reaches ? 1 : 0;
        fitting.missingClimb += missingClimb > 0 ? 1 : 0;
      }
    }

    fitting.shellGap += plates > 1e-6 ? std::abs(plates - groundRooms) / plates : 0.;

    double plateOut = 0;

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      if (storey.base < 0 || storey.base >= 1.5 || storey.outer.size() < 3)
      {
        continue;
      }

      double lowX = storey.outer.front().x, highX = lowX;
      double lowZ = storey.outer.front().z, highZ = lowZ;

      for (const rewrite::Position &point : storey.outer)
      {
        lowX = std::min(lowX, point.x);
        highX = std::max(highX, point.x);
        lowZ = std::min(lowZ, point.z);
        highZ = std::max(highZ, point.z);
      }

      uint32_t in = 0, off = 0;

      for (uint32_t j = 0; j < 40; ++j)
      {
        const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 40.;

        for (uint32_t i = 0; i < 40; ++i)
        {
          const double x = lowX + (highX - lowX) * (double(i) + .5) / 40.;

          if (inside(storey.outer, x, z))
          {
            (inside(lot.ring, x, z) ? in : off) += 1;
          }
        }
      }

      plateOut = in + off > 0 ? double(off) / double(in + off) : 0.;
    }

    std::printf("      lot %6.0f m2 %-14s %u storey, %2u rooms, asked %5.0f got %5.0f"
                " (%+4.0f%%) of %5.0f standing, doors %3.0f%%, off %4.1f%%\n",
                lot.area, name(brief.building), building.programme.storeys,
                building.roomsPlaced, asked, got,
                100. * (got - asked) / std::max(asked, 1.), plates,
                100. * building.doorsDelivered, 100. * spill);
    std::printf("        Ca %.2f  Cd %.2f (sampled %.2f)  Cf %.2f  Cs %.2f\n",
                building.layoutCost.accessibility, building.layoutCost.dimensions,
                building.sampledDimensionCost, building.layoutCost.floors,
                building.layoutCost.shape);

    if (plates > 1e-6 && std::abs(plates - groundRooms) / plates > .02)
    {
      std::printf("        ground shell %.1f m2 against %.1f m2 of rooms\n", plates, groundRooms);
    }

    if (spill > .02)
    {
      std::printf("        off the lot by %.1f%%, and the optimiser thinks %.2f m2 is off\n",
                  100. * spill, building.layoutCost.outsideGround);
    }

    if (plateOut > .005)
    {
      std::printf("        the ground plate itself is %.1f%% off the lot\n", 100. * plateOut);
    }

    if (gallery.size() < 8)
    {
      char label[96];
      std::snprintf(label, sizeof(label), "%.0f m2 lot", lot.area);
      gallery.emplace_back(label, building);
      galleryLots.push_back(lot.ring);
    }
  }

  std::cout << "\n  fitting the programme to the ground\n";

  const double built = std::max(1u, fitting.built);

  std::printf("    %u offered, %u built, %u refused; %.1f rooms each,"
              " doors %.0f%%, area error %.0f%%\n",
              fitting.offered, fitting.built, fitting.refused, fitting.rooms / built,
              100. * fitting.doorsDelivered / built, 100. * fitting.areaError / built);
  std::printf("    off its own ground: %.2f%% of floor area on average,"
              " worst building %.1f%%, %u buildings over 2%%\n",
              100. * fitting.spilled / built, 100. * fitting.worstSpill, fitting.spilling);

  for (const auto &entry : refusals)
  {
    std::printf("    refused %u x %s\n", entry.second, entry.first.c_str());
  }

  std::printf("    dimension cost %.0f against %.0f for the programme's own draw (%.2f),"
              " shell to plan %.2f%%\n",
              fitting.dimensionCost, fitting.sampledCost,
              fitting.dimensionCost / std::max(fitting.sampledCost, 1.),
              100. * fitting.shellGap / built);

  std::printf("    rooms standing on other rooms: %u buildings, %.2f%% of samples\n",
              fitting.overlapping, 100. * fitting.overlap / built);

  std::printf("    %u of %u have a way in; %.1f pieces of outdoor ground each,"
              " %.0f%% of the lot allocated\n",
              fitting.withEntrance, fitting.built, double(fitting.grounds) / built,
              100. * fitting.lotUsed / built);
  for (const auto &entry : byClass)
  {
    std::printf("    %2u x %s\n", entry.second, entry.first.c_str());
  }

  for (const auto &entry : dropped)
  {
    std::printf("    dropped %2u x %s\n", entry.second, entry.first.c_str());
  }

  std::printf("    %u of %u buildings with an upper floor have a stair that arrives"
              " (%u have no vertical circulation on some floor at all);"
              " %.0f%% of each landing stands over the flight below it\n",
              fitting.connected, fitting.storeyed, fitting.missingClimb,
              100. * fitting.climbOverlap / std::max(fitting.climbs, 1u));

  std::printf("    housing %u people and employing %u across %u buildings\n",
              fitting.residents, fitting.workers, fitting.built);

  check(fitting.built >= fitting.offered * 9 / 10, "nine lots in ten get a building");
  check(fitting.withEntrance >= fitting.built * 95 / 100, "the buildings have a way in");
  check(fitting.lotUsed / built > .8, "the lot is allocated, not left as a field");
  check(fitting.overlappingGround == 0, "the ground outside the building is outside it");
  // Where it stands, and what the paper reports for comparison: it sees these
  // failures in 1 of 20 two-storey optimisations and 1 of 5 three-storey ones,
  // and half of these buildings are three-storey. The mean coverage is the
  // number to watch - a building at 70% has a stair you can climb, and one at
  // 20% has a hole in the ceiling next to the steps.
  check(fitting.storeyed == 0 || fitting.connected >= fitting.storeyed * 3 / 4,
        "the stair arrives on the floor above, rather than into a wall");
  check(fitting.climbs == 0 || fitting.climbOverlap / fitting.climbs > .7,
        "a landing covers most of the flight that climbs to it");
  check(fitting.overlapping == 0, "no room stands on another: the floor is a partition");
  // Half as likely again as an unconstrained draw, and that is the lot's
  // doing rather than the optimiser's: on open ground the same code scores
  // 0.91 (FloorPlanOptimiserTests), and a town lot is not open ground.
  check(fitting.dimensionCost < fitting.sampledCost * 1.6,
        "the rooms it fits on the lot stay close to the ones it drew");
  check(fitting.shellGap / built < .01, "the walls that get built are the walls of the plan");
  check(fitting.spilled / built < .01, "on average under 1% of the floor stands off the lot");
  check(fitting.worstSpill < .05, "no building stands appreciably off its lot");
  check(fitting.doorsDelivered / built > .8, "the programme's doors survive the fitting");

  writeGallery("programme-lots.json", gallery, galleryLots);

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");

  return failures == 0 ? 0 : 1;
}
