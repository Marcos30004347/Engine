// A street of the town, built by the programme generator.
//
// The last join. `PlotPlanner` decides what a lot does with its ground - how
// much of it the house takes, where the yard and the garden are, which edge is
// the street - and composes the house's exterior; its own comment says the
// interior "is the next layer's problem". The programme generator is that
// layer, and until this test nothing connected the two: a town's houses were
// still cut out of their footprints by the recursive planner, and every
// programme building in the engine stood on ground invented for it.
//
// So: a settlement, its blocks, its lots; the plot planner on each lot; the
// programme inside the house it placed. What comes out is what a street of the
// town would actually be, and it is written out as blueprints and as solids
// because that is the only way to see whether it reads as a street.
#include "procedural/world/generator/PlotPlanner.hpp"
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"
#include "procedural/world/generator/SettlementLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
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

// The solids: the same buildings in the shape the roof renderer already reads.
//
// Written in exactly the form DwellingGalleryTests writes, so
// tools/terrain/render_roof_skeleton.py draws a programme building without
// knowing it came from a network. That is the point of massing into a
// DwellingPlan rather than inventing a second pipeline.
struct Solid
{
  std::string name;
  std::vector<std::vector<rewrite::Position>> walls, innerWalls, slopes, gables,
    soffits, pillars, garden, yard;
};

void collectSolid(const std::string &label, const rewrite::DwellingPlan &plan,
                  std::vector<Solid> &solids)
{
  Solid solid;
  solid.name = label;

  // Walls come from the storey plates and not from the wings: a wing's own
  // outline includes the wall it shares with its neighbour, so extruding every
  // wing puts a duplicate face inside every building where two wings meet.
  const auto band = [](const std::vector<rewrite::Position> &ring, const double base,
                       const double top,
                       std::vector<std::vector<rewrite::Position>> &out) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const rewrite::Position &a = ring[i];
      const rewrite::Position &b = ring[(i + 1) % ring.size()];

      if (std::hypot(b.x - a.x, b.z - a.z) < 1e-6)
      {
        continue;
      }

      out.push_back({{a.x, base, a.z}, {b.x, base, b.z}, {b.x, top, b.z}, {a.x, top, a.z}});
    }
  };

  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    band(storey.outer, storey.base, storey.top, solid.walls);

    for (const auto &hole : storey.holes)
    {
      band(hole, storey.base, storey.top, solid.innerWalls);
    }
  }

  // ...and the partitions, which is what a programme building has that a
  // massed shell does not.
  for (const rewrite::DwellingWall &wall : plan.walls)
  {
    if (wall.exterior)
    {
      continue;
    }

    solid.innerWalls.push_back({{wall.a.x, wall.a.y, wall.a.z},
                                {wall.b.x, wall.b.y, wall.b.z},
                                {wall.b.x, wall.b.y + 2.4, wall.b.z},
                                {wall.a.x, wall.a.y + 2.4, wall.a.z}});
  }

  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    (face.soffit ? solid.soffits : face.gable ? solid.gables : solid.slopes)
      .push_back(face.polygon);
  }

  solids.push_back(std::move(solid));
}

void writeSolids(const std::string &path, const std::vector<Solid> &solids)
{
  std::ofstream file(path);

  if (!file)
  {
    std::cerr << "  could not write " << path << "\n";

    return;
  }

  file.setf(std::ios::fixed);
  file.precision(4);

  const auto faces = [&file](const char *key,
                             const std::vector<std::vector<rewrite::Position>> &list,
                             const bool last) {
    file << "      \"" << key << "\": [";

    for (size_t f = 0; f < list.size(); ++f)
    {
      file << "[";

      for (size_t v = 0; v < list[f].size(); ++v)
      {
        file << "[" << list[f][v].x << "," << list[f][v].y << "," << list[f][v].z << "]"
             << (v + 1 < list[f].size() ? "," : "");
      }

      file << "]" << (f + 1 < list.size() ? "," : "");
    }

    file << "]" << (last ? "\n" : ",\n");
  };

  file << "{\n  \"buildings\": [\n";

  for (size_t i = 0; i < solids.size(); ++i)
  {
    file << "    {\n      \"name\": \"" << solids[i].name << "\",\n";
    faces("garden", solids[i].garden, false);
    faces("yard", solids[i].yard, false);
    faces("walls", solids[i].walls, false);
    faces("innerwalls", solids[i].innerWalls, false);
    faces("pillars", solids[i].pillars, false);
    faces("slopes", solids[i].slopes, false);
    faces("soffits", solids[i].soffits, false);
    faces("gables", solids[i].gables, true);
    file << "    }" << (i + 1 < solids.size() ? ",\n" : "\n");
  }

  file << "  ]\n}\n";

  std::cout << "  wrote " << solids.size() << " solids to " << path << "\n";
}

// The gallery, with the ground each building stands on and the height of every
// storey in it.
void writeGallery(const std::string &path,
                  const std::vector<std::pair<std::string, ProgrammeBuilding>> &gallery,
                  const std::vector<std::vector<rewrite::Position>> &lots,
                  const std::vector<rewrite::DwellingPlan> &plans)
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

    file << "   ],\n   \"ceilings\": [\n";

    if (b < plans.size())
    {
      for (size_t st = 0; st < plans[b].storeys.size(); ++st)
      {
        const rewrite::DwellingStorey &storey = plans[b].storeys[st];

        file << "    {\"floor\": " << storey.level << ", \"base\": " << storey.base
             << ", \"height\": " << storey.top - storey.base << "}"
             << (st + 1 < plans[b].storeys.size() ? "," : "") << "\n";
      }
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
  std::cout << "a street of the town, planned as programmes\n";

  std::vector<Lot> lots = lotsOf(20260916ull, 700, false);
  const std::vector<Lot> village = lotsOf(881ull, 240, true);

  lots.insert(lots.end(), village.begin(), village.end());

  check(lots.size() >= 40, "the settlement allocated lots to build on");

  if (lots.empty())
  {
    return 1;
  }

  ProgrammePlanSettings settings;
  ComplexDwellingSettings massing;

  // The paper's own budget. It costs about two thirds of a second a house and
  // the street is planned across threads, so it is affordable here in a way it
  // is not in a single-threaded world generation.
  FloorPlanSettings optimise;
  // The paper's count. Figure 6 runs to a hundred thousand and shows
  // twenty thousand still visibly short of it.
  optimise.iterations = 100000;

  std::vector<std::pair<std::string, ProgrammeBuilding>> gallery;
  std::vector<std::vector<rewrite::Position>> galleryLots;
  std::vector<rewrite::DwellingPlan> plans;
  std::vector<Solid> solids;

  uint32_t offered = 0, plotted = 0, built = 0, housed = 0, working = 0;
  double houseShare = 0, doorsDelivered = 0, roomsPer = 0;
  std::map<std::string, uint32_t> byClass;

  // Every lot. The optimisation costs about two thirds of a second a house,
  // which across ten threads is a minute for a settlement - and a street drawn
  // from a sample of one lot in twenty is not a street.
  const size_t stride = 1;

  std::vector<const Lot *> chosen;

  for (size_t i = 0; i < lots.size(); i += stride)
  {
    chosen.push_back(&lots[i]);
  }

  // Planned across threads, because the paper's §5 optimisation costs about
  // two thirds of a second a house and a street is not one house.
  //
  // They are independent - a house is planned from its own lot and its own
  // seed and shares nothing with its neighbours - so the only thing that has
  // to be arranged is the network, whose lazy choice of trained-or-prior is
  // settled under a lock the first time any thread asks for a class.
  struct Planned
  {
    bool built = false;
    std::string line;
    ProgrammeBuilding building;
    rewrite::DwellingPlan plan;
    std::vector<rewrite::Position> lot;
    double houseShare = 0;
    bool plotted = false;
  };

  std::vector<Planned> results(chosen.size());
  std::atomic<size_t> next{0};

  const uint32_t threads =
    std::max(1u, std::min(std::thread::hardware_concurrency(), 12u));

  const auto work = [&]() {
    for (;;)
    {
      const size_t i = next.fetch_add(1);

      if (i >= chosen.size())
      {
        return;
      }

      const Lot &lot = *chosen[i];
      Planned &result = results[i];
      result.lot = lot.ring;

      // --- what the lot does with its ground -------------------------------

      PlotBrief brief;
      brief.id = lot.id;
      brief.seed = 0x51D5u + lot.id * 31ull;
      brief.lot = lot.ring;
      brief.standing = standingOf(lot.area, lot.village);
      describeStanding(brief);

      PlotPlan plot;
      std::string why;

      if (!planPlot(brief, massing, plot, &why) || plot.house.size() < 3)
      {
        result.line = "  lot " + std::to_string(int(lot.area)) + " m2: no house ("
                      + (plot.note.empty() ? why : plot.note) + ")";

        return;
      }

      result.plotted = true;
      result.houseShare = plot.lotArea > 1e-6 ? plot.houseArea / plot.lotArea : 0.;

      // --- and what stands inside it ---------------------------------------

      ProgrammeBrief programme;
      programme.building = classOf(lot);
      programme.standing = brief.standing;
      programme.storeys = std::max(1u, plot.storeys);

      std::string error;

      if (!planProgrammeInside(plot.house, programme, brief.seed, settings, massing,
                               result.building, result.plan, &error, &optimise))
      {
        result.line = "  lot " + std::to_string(int(lot.area)) + " m2: no programme ("
                      + error + ")";

        continue;
      }

      result.built = true;

      char line[240];
      std::snprintf(line, sizeof(line),
                    "  lot %5.0f m2  %-14s house on %2.0f%% of it, %u storeys, %2u rooms;"
                    " %s",
                    lot.area, name(result.building.programme.building),
                    100. * result.houseShare, result.building.programme.storeys,
                    result.building.roomsPlaced,
                    result.building.household.description.c_str());
      result.line = line;
    }
  };

  const auto started = std::chrono::steady_clock::now();

  {
    std::vector<std::thread> pool;

    for (uint32_t t = 0; t < threads; ++t)
    {
      pool.emplace_back(work);
    }

    for (std::thread &thread : pool)
    {
      thread.join();
    }
  }

  const double seconds =
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  for (Planned &result : results)
  {
    ++offered;
    plotted += result.plotted ? 1 : 0;
    houseShare += result.houseShare;

    if (!result.line.empty() && (!result.built || offered <= 12))
    {
      std::printf("%s\n", result.line.c_str());
    }

    if (!result.built)
    {
      continue;
    }

    ++built;
    housed += result.building.household.residents;
    working += result.building.household.workers;
    doorsDelivered += result.building.doorsDelivered;
    roomsPer += double(result.building.roomsPlaced);
    byClass[name(result.building.programme.building)] += 1;

    char label[96];
    std::snprintf(label, sizeof(label), "%.0f m2 lot", std::abs(area(result.lot)));

    // Every house goes into the solids, because the town has to be drawn as a
    // town: buildings one to a tile cannot show whether they crowd a street or
    // stand off it. Only the first few go into the blueprints, which are read
    // one at a time.
    collectSolid(label, result.plan, solids);

    if (gallery.size() < 8)
    {
      gallery.emplace_back(label, result.building);
      galleryLots.push_back(result.lot);
      plans.push_back(result.plan);
    }
  }

  std::printf("\n  %u houses on %u threads in %.1f s (%.2f s each)\n", built, threads,
              seconds, seconds / std::max(built, 1u));

  std::cout << "\n";

  for (const auto &entry : byClass)
  {
    std::printf("  %2u x %s\n", entry.second, entry.first.c_str());
  }

  const double each = std::max(built, 1u);

  std::printf("  %u lots offered, %u planned, %u built; the house takes %.0f%% of its lot,"
              " %.1f rooms each, %.0f%% of doors\n",
              offered, plotted, built, 100. * houseShare / std::max(plotted, 1u),
              roomsPer / each, 100. * doorsDelivered / each);
  std::printf("  the street houses %u people and employs %u\n", housed, working);

  check(plotted >= offered * 3 / 4, "most lots get a house from the plot planner");
  check(built == plotted, "every house the plot planner places gets a programme inside it");
  check(doorsDelivered / each > .8, "the programme's doors survive being fitted to the plot");
  check(houseShare / std::max(plotted, 1u) < .95,
        "the house does not cover its whole lot: there is ground around it");

  writeGallery("town-gallery.json", gallery, galleryLots, plans);
  writeSolids("town-solids.json", solids);

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");

  return failures == 0 ? 0 : 1;
}
