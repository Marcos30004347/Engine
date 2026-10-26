// The second dwelling planner: a programme realised as geometry.
//
// Stage D of .plan/bayesian-architectural-program.md, and its acceptance is
// stated there: "the fraction of the programme's adjacencies the plan actually
// delivers. That last number is the one to watch; it is what distinguishes a
// plan from a partition."
//
// So that is what this measures, per standing and per building class, beside
// the area each room asked for against the area it got. A partition cannot
// fail either test - it has no programme to fail against - which is exactly
// why they are worth running.
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <iostream>
#include <fstream>
#include <map>
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

struct Survey
{
  uint32_t built = 0, refused = 0;
  double areaError = 0, doorsDelivered = 0;
  double rooms = 0, dropped = 0;
  std::map<std::string, uint32_t> undelivered;
};

Survey survey(const BuildingClass klass, const PlotStanding standing, const uint32_t count,
              const ProgrammePlanSettings &settings)
{
  Survey out;

  for (uint32_t i = 0; i < count; ++i)
  {
    ProgrammeBrief brief;
    brief.building = klass;
    brief.standing = standing;

    ProgrammeBuilding building;
    std::string why;

    if (!planProgrammeBuilding(brief, 0xD0012u + i * 2654435761u, settings, {}, building, &why))
    {
      ++out.refused;

      continue;
    }

    ++out.built;
    out.areaError += building.areaError;
    out.doorsDelivered += building.doorsDelivered;
    out.rooms += double(building.roomsPlaced);
    out.dropped += double(building.roomsDropped);

    for (const UndeliveredDoor &missed : building.undelivered)
    {
      std::string a = name(missed.useA), b = name(missed.useB);

      if (b < a)
      {
        std::swap(a, b);
      }

      ++out.undelivered[a + " + " + b];
    }
  }

  if (out.built > 0)
  {
    out.areaError /= out.built;
    out.doorsDelivered /= out.built;
    out.rooms /= out.built;
    out.dropped /= out.built;
  }

  return out;
}

void report(const char *label, const Survey &s)
{
  std::printf("  %-14s %3u built, %4.1f rooms (%.1f dropped), area error %.0f%%, "
              "doors delivered %.0f%%\n",
              label, s.built, s.rooms, s.dropped, 100. * s.areaError,
              100. * s.doorsDelivered);

  std::vector<std::pair<uint32_t, std::string>> worst;

  for (const auto &entry : s.undelivered)
  {
    worst.emplace_back(entry.second, entry.first);
  }

  std::sort(worst.rbegin(), worst.rend());

  if (!worst.empty())
  {
    std::printf("      doors it could not make:");

    for (size_t i = 0; i < worst.size() && i < 4; ++i)
    {
      std::printf(" %s %u;", worst[i].second.c_str(), worst[i].first);
    }

    std::printf("\n");
  }
}

// One building, printed as the plan asks: the programme beside the realisation.
void printOne(const char *label, const ProgrammeBuilding &building)
{
  const Programme &programme = building.programme;

  std::printf("\n  %s - %u storey%s%s, footprint %.1f x %.1f m\n", label,
              programme.storeys, programme.storeys == 1 ? "" : "s",
              programme.basement ? " over a basement" : "",
              programme.footprintX, programme.footprintZ);

  for (const PlacedRoom &room : building.rooms)
  {
    std::printf("    %-16s floor %2d  asked %5.1f  got %5.1f m2  %+4.0f%%\n",
                name(room.use), room.floor, room.askedMetres, room.achievedMetres,
                room.askedMetres > 0
                  ? 100. * (room.achievedMetres - room.askedMetres) / room.askedMetres
                  : 0.);
  }

  std::printf("    %zu doors delivered of %zu asked; area error %.0f%%\n",
              building.doors.size(),
              building.doors.size() + building.undelivered.size(),
              100. * building.areaError);
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

// The gallery: every planned building written out for the renderer.
//
// A number cannot settle an argument about a plan. "77% of doors delivered"
// says the geometry mostly works and says nothing about whether the result
// looks like a building, and the questions that actually come up - is the shop
// on the street, is the hall in the middle, is that room a splinter - are
// questions about the picture.
void writeGallery(const std::string &path,
                  const std::vector<std::pair<std::string, ProgrammeBuilding>> &gallery)
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

    file << "   ]\n  }" << (b + 1 < gallery.size() ? "," : "") << "\n";
  }

  file << " ]\n}\n";

  std::cout << "  wrote " << gallery.size() << " buildings to " << path << "\n";
}

}  // namespace

int main()
{
  // The planner is being tested, not the trained network, and the trained one
  // would change every number here the moment the corpus grows.
  useProgrammeNetworkPrior();

  const ProgrammePlanSettings settings;

  std::cout << "one programme, realised\n";

  ProgrammeBrief brief;
  brief.standing = PlotStanding::Comfortable;

  ProgrammeBuilding building;
  std::string why;

  check(planProgrammeBuilding(brief, 20260915ull, settings, {}, building, &why),
        "a comfortable house is planned: " + why);
  check(building.roomsPlaced > 0, "it has rooms in it");
  check(!building.footprint.empty(), "it has a footprint");

  printOne("comfortable", building);

  // Every placed room must be a real piece of ground, and they must not
  // overlap - a power diagram's cells partition the region, so a failure here
  // is a failure to use it correctly rather than a tuning problem.
  double sum = 0;

  for (const PlacedRoom &room : building.rooms)
  {
    sum += room.achievedMetres;
    check(room.achievedMetres >= settings.minimumRoomMetres
            || room.achievedMetres == 0,
          std::string("the ") + name(room.use) + " is big enough to be a room");
  }

  // Against the outline the planner actually used, not against the
  // programme's footprint variable - which the planner now treats as a shape
  // and not a size, because the two were sampled separately and disagreed.
  double lowGX = 1e9, highGX = -1e9, lowGZ = 1e9, highGZ = -1e9;

  for (const auto &point : building.footprint)
  {
    lowGX = std::min(lowGX, point.first);
    highGX = std::max(highGX, point.first);
    lowGZ = std::min(lowGZ, point.second);
    highGZ = std::max(highGZ, point.second);
  }

  const double ground = (highGX - lowGX) * (highGZ - lowGZ);
  std::printf("  ground %.1f m2, rooms sum to %.1f m2 over %u floors\n",
              ground, sum, building.floorsPlanned);
  check(sum <= ground * double(building.floorsPlanned) * 1.02 + 1.,
        "the rooms fit inside the ground they were given");

  // --- across the standings ---

  std::cout << "\nby standing\n";

  const std::pair<PlotStanding, const char *> standings[] = {
    {PlotStanding::Slum, "slum"},
    {PlotStanding::Modest, "modest"},
    {PlotStanding::Comfortable, "comfortable"},
    {PlotStanding::Wealthy, "wealthy"},
    {PlotStanding::Grand, "grand"},
    {PlotStanding::Rural, "rural"},
  };

  double deliveredTotal = 0, errorTotal = 0;
  uint32_t surveyed = 0;

  for (const auto &entry : standings)
  {
    const Survey s = survey(BuildingClass::Dwelling, entry.first, 60, settings);
    report(entry.second, s);

    check(s.built == 60, std::string("every ") + entry.second + " house is planned");

    deliveredTotal += s.doorsDelivered;
    errorTotal += s.areaError;
    ++surveyed;
  }

  const double delivered = deliveredTotal / surveyed;
  const double areaError = errorTotal / surveyed;

  std::printf("\n  over the standings: %.0f%% of doors delivered, %.0f%% area error\n",
              100. * delivered, 100. * areaError);

  // The fences, set where the planner actually stands so that a change making
  // it worse is a failure rather than a shrug. Where it stood when they were
  // set: 77% of doors delivered, 4% area error.
  //
  // Neither is where §5's floor-plan optimisation would put them. What this
  // does is place sites and let a weighted power diagram find the walls; what
  // that does is move the walls themselves against a cost function. The gap
  // between 77% and 100% is most of the reason to write it.
  check(delivered > .70, "most of the programme's doors survive into the plan");
  check(areaError < .10, "rooms come out near the size the programme asked for");

  // --- the other building classes ---

  std::cout << "\nby building class\n";

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    const Survey s = survey(BuildingClass(k), PlotStanding::Comfortable, 40, settings);
    report(name(BuildingClass(k)), s);

    check(s.built > 0, std::string("the ") + name(BuildingClass(k)) + " is planned");
  }

  // --- the footprint may be imposed ---

  std::cout << "\non a given plot\n";

  const std::vector<std::pair<double, double>> plot = {
    {0, 0}, {11, 0}, {11, 7}, {0, 7}};

  ProgrammeBrief tight;
  tight.standing = PlotStanding::Modest;
  tight.footprintX = 11.;
  tight.footprintZ = 7.;

  ProgrammeBuilding onPlot;
  check(planProgrammeBuilding(tight, 77ull, settings, plot, onPlot, &why),
        "a house is planned on a given plot: " + why);

  double lowX = 1e9, highX = -1e9, lowZ = 1e9, highZ = -1e9;

  for (const PlacedRoom &room : onPlot.rooms)
  {
    for (const auto &point : room.boundary.outer)
    {
      lowX = std::min(lowX, geom2::toMetres(point.x));
      highX = std::max(highX, geom2::toMetres(point.x));
      lowZ = std::min(lowZ, geom2::toMetres(point.y));
      highZ = std::max(highZ, geom2::toMetres(point.y));
    }
  }

  std::printf("  rooms span %.2f..%.2f by %.2f..%.2f on an 11 x 7 plot\n",
              lowX, highX, lowZ, highZ);
  check(lowX >= -1e-6 && highX <= 11. + 1e-6 && lowZ >= -1e-6 && highZ <= 7. + 1e-6,
        "no room leaves the plot it was given");

  printOne("on an 11 by 7 plot", onPlot);

  // --- massed, roofed, and in the shape the rest of the engine reads ---
  //
  // The point of this section is not that the numbers are good; it is that a
  // programme now comes out as a DwellingPlan, so every consumer already
  // written - the mesher, the solids writer, the roof renderer - works on it
  // without knowing a network was involved.

  std::cout << "\nmassed as a building\n";

  const ComplexDwellingSettings massing;

  std::vector<Solid> solids;
  uint32_t massed = 0, withRoof = 0, withStairs = 0, withExteriorWalls = 0;
  double roomsPer = 0, wingsPer = 0;

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    ProgrammeBrief g;
    g.building = BuildingClass(k);
    g.standing = BuildingClass(k) == BuildingClass::Mansion
                     || BuildingClass(k) == BuildingClass::Castle
                   ? PlotStanding::Grand
                   : PlotStanding::Comfortable;

    ProgrammeBuilding built;
    rewrite::DwellingPlan dwelling;
    std::string massWhy;

    if (!planProgrammeDwelling(g, 9090ull + k, settings, massing, {}, built, dwelling,
                               &massWhy))
    {
      std::printf("  %-14s refused: %s\n", name(BuildingClass(k)), massWhy.c_str());

      continue;
    }

    collectSolid(name(BuildingClass(k)), dwelling, solids);
    ++massed;
    roomsPer += double(dwelling.rooms.size());
    wingsPer += double(dwelling.wings.size());
    withRoof += dwelling.roofs.empty() ? 0 : 1;
    withStairs += dwelling.stairs.empty() ? 0 : 1;

    bool anyExterior = false;

    for (const rewrite::DwellingWall &wall : dwelling.walls)
    {
      anyExterior = anyExterior || wall.exterior;
    }

    withExteriorWalls += anyExterior ? 1 : 0;

    std::printf("  %-14s %2zu wings, %2zu rooms, %3zu walls, %3zu roof faces, "
                "%zu doors, %zu stairs\n",
                name(BuildingClass(k)), dwelling.wings.size(), dwelling.rooms.size(),
                dwelling.walls.size(), dwelling.roofs.size(), dwelling.doors.size(),
                dwelling.stairs.size());
  }

  std::printf("  %u of %u classes massed; %.1f rooms and %.1f wings each\n",
              massed, uint32_t(BuildingClass::Count),
              roomsPer / std::max(massed, 1u), wingsPer / std::max(massed, 1u));

  check(massed == uint32_t(BuildingClass::Count), "every class masses into a building");
  check(withRoof == massed, "every massed building has a roof over it");
  check(withExteriorWalls == massed, "every massed building has outside walls");
  check(withStairs > 0, "a building with storeys has a stair in it");

  writeSolids("programme-solids.json", solids);

  // --- the gallery ---

  std::cout << "\ngallery\n";

  std::vector<std::pair<std::string, ProgrammeBuilding>> gallery;

  for (const auto &entry : standings)
  {
    ProgrammeBrief g;
    g.standing = entry.first;

    ProgrammeBuilding built;

    if (planProgrammeBuilding(g, 20260915ull, settings, {}, built, nullptr))
    {
      gallery.emplace_back(std::string("dwelling, ") + entry.second, built);
    }
  }

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    const BuildingClass klass = BuildingClass(k);

    // Each class at the standing its own rooms belong to, so a mansion is
    // shown with its state rooms rather than as a large house.
    const PlotStanding standing =
      klass == BuildingClass::Mansion || klass == BuildingClass::Castle
        ? PlotStanding::Grand
        : PlotStanding::Comfortable;

    ProgrammeBrief g;
    g.building = klass;
    g.standing = standing;

    ProgrammeBuilding built;

    if (planProgrammeBuilding(g, 424242ull + k, settings, {}, built, nullptr))
    {
      gallery.emplace_back(name(klass), built);
    }
  }

  writeGallery("programme-gallery.json", gallery);
  check(gallery.size() >= uint32_t(BuildingClass::Count),
        "the gallery has every class in it");

  std::cout << "\n" << (failures == 0 ? "all passed" : std::to_string(failures) + " failed")
            << "\n";

  return failures == 0 ? 0 : 1;
}
