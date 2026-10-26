// Mansions.
//
// The corpus has them - a first class town mansion of forty-three rooms on
// four floors, an Italian villa with a tower, a mansion in the Old English
// style - and they are the hardest thing the generator is asked for: twenty
// rooms and more, three and four storeys, a service wing that is a household
// of its own, and a plan that has to hold together across all of it.
//
// A cottage can be wrong and still look like a cottage. A mansion cannot: the
// state rooms have to be on the ground floor and reach each other, the service
// rooms have to be kept away from them and still serve the dining room, the
// stair has to arrive on every floor, and the whole thing has to stand on a
// piece of ground somebody drew. So this measures those, and writes the
// blueprints and the solids out to be looked at, because the numbers cannot
// settle whether it reads as a great house.
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"

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
  std::cout << "mansions: the largest thing the programme generator is asked for\n";

  ProgrammePlanSettings settings;
  ComplexDwellingSettings massing;

  FloorPlanSettings optimise;
  // The paper's count. Figure 6 runs to a hundred thousand and shows
  // twenty thousand still visibly short of it.
  optimise.iterations = 100000;

  // The grounds. A mansion is not fitted to a burgage strip: it stands in a
  // park, and what bounds it is the ground the estate laid out rather than a
  // neighbour's wall.
  const auto grounds = [](const double side, const double aspect) {
    const double halfX = side * std::sqrt(aspect) * .5;
    const double halfZ = side / std::sqrt(aspect) * .5;

    return std::vector<rewrite::Position>{{-halfX, 0, -halfZ},
                                          {halfX, 0, -halfZ},
                                          {halfX, 0, halfZ},
                                          {-halfX, 0, halfZ}};
  };

  struct Ask
  {
    const char *label;
    BuildingClass klass;
    PlotStanding standing;
    uint32_t storeys;
    double side, aspect;
  };

  const std::vector<Ask> asks{
    {"first class town mansion", BuildingClass::Mansion, PlotStanding::Grand, 4, 34, 1.35},
    {"mansion in its park", BuildingClass::Mansion, PlotStanding::Grand, 3, 46, 1.6},
    {"Italian villa with a tower", BuildingClass::Mansion, PlotStanding::Wealthy, 3, 38, 1.2},
    {"manor house", BuildingClass::ManorHouse, PlotStanding::Wealthy, 3, 40, 1.45},
    {"the old manor", BuildingClass::ManorHouse, PlotStanding::Grand, 2, 44, 1.8},
    {"guild hall", BuildingClass::GuildHall, PlotStanding::Wealthy, 3, 30, 1.3},
  };

  std::vector<std::pair<std::string, ProgrammeBuilding>> gallery;
  std::vector<std::vector<rewrite::Position>> lots;
  std::vector<rewrite::DwellingPlan> plans;
  std::vector<Solid> solids;

  uint32_t built = 0, tall = 0, roofed = 0, staired = 0;
  double roomsPer = 0, floorAreaPer = 0;

  for (uint32_t i = 0; i < asks.size(); ++i)
  {
    const Ask &ask = asks[i];

    ProgrammeBrief brief;
    brief.building = ask.klass;
    brief.standing = ask.standing;
    brief.storeys = ask.storeys;

    const std::vector<rewrite::Position> lot = grounds(ask.side, ask.aspect);

    ProgrammeBuilding building;
    rewrite::DwellingPlan plan;
    std::string error;

    if (!planProgrammeDwelling(brief, 0x9A5701u + i * 7919ull, settings, massing, lot,
                               building, plan, &error, &optimise))
    {
      std::printf("  %-28s refused: %s\n", ask.label, error.c_str());

      continue;
    }

    ++built;
    tall += building.programme.storeys >= 3 ? 1 : 0;
    roofed += plan.roofs.empty() ? 0 : 1;
    staired += plan.stairs.empty() ? 0 : 1;
    roomsPer += double(building.roomsPlaced);

    double floorArea = 0;
    std::map<int32_t, uint32_t> perFloor;

    for (const PlacedRoom &room : building.rooms)
    {
      floorArea += room.achievedMetres;
      perFloor[room.floor] += 1;
    }

    floorAreaPer += floorArea;

    std::printf("  %-28s %u storeys, %2u rooms over %zu floors, %4.0f m2 of floor,"
                " %2zu wings, %3zu roof faces, %2zu stairs\n",
                ask.label, building.programme.storeys, building.roomsPlaced,
                perFloor.size(), floorArea, plan.wings.size(), plan.roofs.size(),
                plan.stairs.size());
    std::printf("      %s\n", building.household.description.c_str());

    std::printf("      ceilings:");

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      std::printf(" %d at %.2f m", storey.level, storey.top - storey.base);
    }

    std::printf("\n      rooms by floor:");

    for (const auto &entry : perFloor)
    {
      std::printf(" %d:%u", entry.first, entry.second);
    }

    std::printf("\n");

    gallery.emplace_back(ask.label, building);
    lots.push_back(lot);
    plans.push_back(plan);
    collectSolid(ask.label, plan, solids);
  }

  std::cout << "\n";
  std::printf("  %u of %zu built; %.0f rooms and %.0f m2 of floor each\n", built,
              asks.size(), roomsPer / std::max(built, 1u),
              floorAreaPer / std::max(built, 1u));

  check(built == asks.size(), "every mansion asked for is built");
  check(tall >= 4, "most of them are three storeys or more");
  check(roofed == built, "every one of them is roofed");
  check(staired == built, "every one of them has a stair");
  check(roomsPer / std::max(built, 1u) > 14., "a mansion has the rooms of a mansion");

  writeGallery("mansion-gallery.json", gallery, lots, plans);
  writeSolids("mansion-solids.json", solids);

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");

  return failures == 0 ? 0 : 1;
}
