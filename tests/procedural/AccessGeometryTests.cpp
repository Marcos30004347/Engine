// The way into a building, as planes.
//
// Two kinds of test, and the second is the one that matters.
//
// The first kind builds a stack and asserts it has no faults. That is the
// ordinary thing, and on its own it is worth very little: the last two attempts
// at this area had eight such checks, all green, over renders that were plainly
// wrong. A check nobody has ever seen fail is a check nobody has tested.
//
// So the second kind takes a *correct* model, breaks one thing in it on
// purpose, and asserts that the fault it broke is the fault that comes back.
// That is what says the checks can see. Every fault kind below has one.

#include "procedural/world/generator/AccessGeometry.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace procedural::world::generator;
using access::FaultKind;
using access::Model;
using access::Position;

namespace {

size_t failures = 0;

void check(const bool condition, const std::string &what)
{
  std::cout << (condition ? "  PASSED " : "  FAILED ") << what << "\n";
  failures += condition ? 0 : 1;
}

std::string listOf(const std::vector<access::Fault> &faults)
{
  std::string out;
  std::map<std::string, size_t> counted;

  for (const access::Fault &fault : faults)
  {
    counted[access::nameOf(fault.kind)] += 1;
  }

  for (const auto &entry : counted)
  {
    out += (out.empty() ? "" : ", ") + entry.first + " x" + std::to_string(entry.second);
  }

  return out.empty() ? "none" : out;
}

bool has(const std::vector<access::Fault> &faults, const FaultKind kind)
{
  for (const access::Fault &fault : faults)
  {
    if (fault.kind == kind)
    {
      return true;
    }
  }

  return false;
}

// A stack that is known good, for the breaking tests to start from.
Model soundStack(const uint32_t floors = 4)
{
  access::StackBrief brief;
  brief.wallFrom = {0, 0, 0};
  brief.wallTo = {12, 0, 0};
  brief.floors = floors;
  brief.storeyHeight = 2.8;
  brief.rampWidth = 1.0;
  brief.maximumSlope = .6;

  Model model;
  std::string why;

  if (!access::planStack(brief, model, &why))
  {
    std::cout << "  (the sound stack would not build: " << why << ")\n";
  }

  return model;
}

// ---------------------------------------------------------------------------
// The stacks themselves.
// ---------------------------------------------------------------------------
void stackCases(std::string &document)
{
  std::cout << "\nA stack, one household to a floor, climbed from outside\n";

  struct Case
  {
    const char *name;
    uint32_t floors;
    double wall;
    double storey;
  };

  const std::vector<Case> cases = {
    {"three floors", 3, 11.0, 2.8},
    {"four floors", 4, 12.0, 2.8},
    {"four, tall storeys", 4, 14.0, 3.2},
    {"five floors", 5, 13.0, 2.7},
  };

  for (const Case &one : cases)
  {
    access::StackBrief brief;
    brief.wallFrom = {0, 0, 0};
    brief.wallTo = {one.wall, 0, 0};
    brief.floors = one.floors;
    brief.storeyHeight = one.storey;
    brief.rampWidth = 1.0;
    brief.maximumSlope = .6;

    Model model;
    std::string why;

    if (!access::planStack(brief, model, &why))
    {
      check(false, std::string(one.name) + ": " + why);

      continue;
    }

    const std::vector<access::Fault> faults = access::validate(model);

    std::printf("    %-20s %zu surfaces, %zu ramps, %zu doors; faults: %s\n", one.name, model.surfaces.size(), model.ramps.size(), model.portals.size(), listOf(faults).c_str());

    for (const access::Fault &fault : faults)
    {
      std::printf("      %-24s %s\n", access::nameOf(fault.kind), fault.what.c_str());
    }

    check(model.surfaces.size() == one.floors, std::string(one.name) + ": a surface for the ground and one per floor above it");
    check(model.ramps.size() + 1u == one.floors, std::string(one.name) + ": a ramp between each pair of them");
    check(model.portals.size() == one.floors, std::string(one.name) + ": a door for every household");
    check(faults.empty(), std::string(one.name) + ": and nothing wrong with any of it");

    if (one.floors == 4u && std::abs(one.storey - 2.8) < .01)
    {
      document = access::toJson(model, faults);
    }
  }

  // A wall too short to climb is refused rather than fudged.
  {
    access::StackBrief brief;
    brief.wallFrom = {0, 0, 0};
    brief.wallTo = {3.5, 0, 0};
    brief.floors = 3;

    Model model;
    std::string why;

    check(!access::planStack(brief, model, &why), "a wall too short for a ramp is refused, not squeezed");
    std::printf("      refused: %s\n", why.c_str());
  }
}

// ---------------------------------------------------------------------------
// The tower: a stair that winds instead of doubling back.
// ---------------------------------------------------------------------------
void towerCases(std::string &document)
{
  std::cout << "\nA stair winding up the inside of a tower\n";

  struct Case
  {
    const char *name;
    double side;
    uint32_t floors;
    double width;
  };

  const std::vector<Case> cases = {
    {"a four floor shaft", 4.4, 4, 1.1},
    {"a tall thin one", 3.6, 6, .9},
    {"a wide one", 5.4, 3, 1.3},
  };

  for (const Case &one : cases)
  {
    access::TowerBrief brief;
    brief.centre = {0, 0, 0};
    brief.side = one.side;
    brief.floors = one.floors;
    brief.rampWidth = one.width;
    brief.storeyHeight = 2.8;

    Model model;
    std::string why;

    if (!access::planTower(brief, model, &why))
    {
      check(false, std::string(one.name) + ": " + why);

      continue;
    }

    const std::vector<access::Fault> faults = access::validate(model);

    std::printf("    %-20s %zu surfaces, %zu ramps, %zu doors; faults: %s\n", one.name, model.surfaces.size(), model.ramps.size(), model.portals.size(), listOf(faults).c_str());

    for (const access::Fault &fault : faults)
    {
      std::printf("      %-24s %s\n", access::nameOf(fault.kind), fault.what.c_str());
    }

    // Four flights to a storey, one to a side of the shaft. That is what makes
    // it a newel stair rather than a tall straight one.
    check(model.ramps.size() == (one.floors - 1u) * 4u, std::string(one.name) + ": four flights to a storey, one to a side");
    check(model.portals.size() == one.floors - 1u, std::string(one.name) + ": a door out of the shaft at every floor");
    check(faults.empty(), std::string(one.name) + ": and nothing wrong with any of it");

    // Every storey's flights add up to exactly a storey. Measured off the
    // ramps, not off the numbers that made them.
    double climbed = 0;

    for (const access::Ramp &ramp : model.ramps)
    {
      climbed += ramp.headLeft.y - ramp.footLeft.y;
    }

    const double wanted = double(one.floors - 1u) * brief.storeyHeight;
    std::printf("      climbs %.3f m of a wanted %.3f m\n", climbed, wanted);
    check(std::abs(climbed - wanted) < 1e-6, std::string(one.name) + ": and the flights add up to the height of the tower");

    if (one.floors == 4u)
    {
      document = access::toJson(model, faults);
    }
  }

  // A shaft with no well left in the middle is refused.
  {
    access::TowerBrief brief;
    brief.centre = {0, 0, 0};
    brief.side = 2.6;
    brief.rampWidth = 1.2;

    Model model;
    std::string why;

    check(!access::planTower(brief, model, &why), "a shaft too narrow to leave a well is refused");
    std::printf("      refused: %s\n", why.c_str());
  }
}

// ---------------------------------------------------------------------------
// Triangles.
// ---------------------------------------------------------------------------
void triangleCases()
{
  std::cout << "\nThe model as triangles\n";

  Model model = soundStack();
  std::vector<access::Face> faces;
  std::string why;

  check(access::triangulate(model, faces, &why) && !faces.empty(), "a sound stack triangulates");

  size_t surfaces = 0, ramps = 0, barriers = 0, solids = 0, slivers = 0;

  for (const access::Face &face : faces)
  {
    surfaces += face.kind == access::FaceKind::SurfaceFace ? 1 : 0;
    ramps += face.kind == access::FaceKind::RampFace ? 1 : 0;
    barriers += face.kind == access::FaceKind::BarrierFace ? 1 : 0;
    solids += face.kind == access::FaceKind::SolidFace ? 1 : 0;

    const double ax = face.b.x - face.a.x, ay = face.b.y - face.a.y, az = face.b.z - face.a.z;
    const double bx = face.c.x - face.a.x, by = face.c.y - face.a.y, bz = face.c.z - face.a.z;
    const double area = std::hypot(std::hypot(ay * bz - az * by, az * bx - ax * bz), ax * by - ay * bx) * .5;

    slivers += area < 1e-6 ? 1 : 0;
  }

  std::printf("    %zu faces: %zu surface, %zu ramp, %zu barrier, %zu solid; %zu with no area\n", faces.size(), surfaces, ramps, barriers, solids, slivers);

  check(slivers == 0, "and none of its triangles has no area");
  check(ramps == model.ramps.size() * 2, "a ramp is two triangles, being a quad");

  // The surface triangles have to add up to the surface, holes and all. A
  // triangulation that quietly fills the stairwell is the failure that matters
  // here, and it is an area.
  double drawn = 0;

  for (const access::Face &face : faces)
  {
    if (face.kind != access::FaceKind::SurfaceFace)
    {
      continue;
    }

    drawn += std::abs((face.b.x - face.a.x) * (face.c.z - face.a.z) - (face.c.x - face.a.x) * (face.b.z - face.a.z)) * .5;
  }

  double wanted = 0;

  for (const access::Surface &surface : model.surfaces)
  {
    const auto area = [](const std::vector<Position> &ring) {
      double twice = 0;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        twice += ring[i].x * ring[(i + 1) % ring.size()].z - ring[(i + 1) % ring.size()].x * ring[i].z;
      }

      return std::abs(twice) * .5;
    };

    wanted += area(surface.outer);

    for (const std::vector<Position> &hole : surface.holes)
    {
      wanted -= area(hole);
    }
  }

  std::printf("    surfaces cover %.3f m2 drawn against %.3f m2 planned\n", drawn, wanted);
  check(std::abs(drawn - wanted) < .05, "and the triangles of a surface cover it exactly, holes and all");
}

// ---------------------------------------------------------------------------
// Breaking it on purpose.
// ---------------------------------------------------------------------------
//
// One test per fault kind. Each starts from a model with no faults, breaks the
// single thing that fault is about, and asserts it comes back - so a check that
// silently stops working is a failing test rather than a quiet green one.
void breakingCases()
{
  std::cout << "\nBreaking a sound stack, one fault at a time\n";

  check(access::validate(soundStack()).empty(), "the stack it all starts from has no faults");

  {
    Model model = soundStack();
    model.surfaces[1].outer.resize(2);

    check(has(access::validate(model), FaultKind::SurfaceDegenerate), "a surface with two corners is caught");
  }

  {
    Model model = soundStack();

    // A bow tie: the same four corners wound so the outline crosses itself.
    std::swap(model.surfaces[1].outer[2], model.surfaces[1].outer[3]);

    check(has(access::validate(model), FaultKind::SurfaceDegenerate), "and one whose outline crosses itself");
  }

  {
    Model model = soundStack();
    access::Surface copy = model.surfaces[1];
    copy.id = 99;
    model.surfaces.push_back(std::move(copy));

    check(has(access::validate(model), FaultKind::SurfaceOverlaps), "two surfaces standing on each other are caught");
  }

  {
    Model model = soundStack();

    // Slide the first ramp sideways, off the ground it starts from.
    for (Position *p : {&model.ramps[0].footLeft, &model.ramps[0].footRight})
    {
      p->z -= 8;
    }

    check(has(access::validate(model), FaultKind::RampFootOffSurface), "a ramp whose foot is not on its surface is caught");
  }

  {
    Model model = soundStack();

    for (Position *p : {&model.ramps[0].headLeft, &model.ramps[0].headRight})
    {
      p->z -= 8;
    }

    check(has(access::validate(model), FaultKind::RampHeadOffSurface), "and one whose head reaches nothing");
  }

  {
    Model model = soundStack();

    // Fill in the stairwell the first ramp comes up through: the balcony
    // becomes the solid rectangle it covers. Taken from its own bounds rather
    // than written out, because which way the wall faces is the builder's
    // business and a break that guesses it proves nothing.
    access::Surface &balcony = model.surfaces[1];
    double lowX = 1e30, highX = -1e30, lowZ = 1e30, highZ = -1e30;

    for (const Position &p : balcony.outer)
    {
      lowX = std::min(lowX, p.x);
      highX = std::max(highX, p.x);
      lowZ = std::min(lowZ, p.z);
      highZ = std::max(highZ, p.z);
    }

    const double h = balcony.height;
    balcony.holes.clear();
    balcony.outer = {Position{lowX, h, lowZ}, Position{highX, h, lowZ}, Position{highX, h, highZ}, Position{lowX, h, highZ}};

    check(has(access::validate(model), FaultKind::RampPierces), "a balcony with no hole where the ramp comes through is caught");
  }

  {
    Model model = soundStack();

    // Stand the head of a ramp on top of its own foot.
    model.ramps[0].headLeft.x = model.ramps[0].footLeft.x + .2;
    model.ramps[0].headRight.x = model.ramps[0].footRight.x + .2;

    check(has(access::validate(model), FaultKind::RampTooSteep), "a ramp nobody could climb is caught");
  }

  {
    Model model = soundStack();
    model.portals[0].at.z -= 8;

    check(has(access::validate(model), FaultKind::PortalOffSurface), "a door with no floor outside it is caught");
  }

  {
    Model model = soundStack();

    // Move the first upper door to the ramp arriving there, and push the floor
    // you step onto out into the ramp's own half of the balcony.

    // Onto the ramp, not merely near it. Near it is every door on every
    // landing ever built - see the tower, whose corner landings are a metre
    // square and have nowhere on them that is not near a flight.
    // Halfway along the ramp, not at its head: at the head the balcony is cut
    // away for the stairwell, so a door there is off the surface entirely and
    // trips that fault first. Halfway along, the balcony is solid and the ramp
    // is underneath it - which is exactly the door that opens onto a stair.
    const access::Ramp &ramp = model.ramps[0];
    // A sixth of the way up it, on its centre line.
    //
    // Not halfway: halfway up, the balcony above has been cut away for the
    // stairwell, so a door there is off the surface and trips *that* fault
    // instead. Near the foot the ramp still has solid balcony over it, which
    // is where a door opening onto a staircase actually is.
    const double t = 1. / 6.;
    const double footX = (ramp.footLeft.x + ramp.footRight.x) * .5, footZ = (ramp.footLeft.z + ramp.footRight.z) * .5;
    const double headX = (ramp.headLeft.x + ramp.headRight.x) * .5, headZ = (ramp.headLeft.z + ramp.headRight.z) * .5;
    const double midX = footX + (headX - footX) * t;
    const double midZ = footZ + (headZ - footZ) * t;

    // Back the door itself off the wall by however far out that is.
    access::Portal &door = model.portals[0];
    door.into = 1.5;
    door.at.x = midX - door.outX * door.into;
    door.at.z = midZ - door.outZ * door.into;

    const std::vector<access::Fault> faults = access::validate(model);

    check(has(faults, FaultKind::PortalOnRamp), "a door you step out of onto the ramp is caught");
    check(!has(faults, FaultKind::PortalOffSurface), "and it is still standing on the balcony while it is caught");
  }

  {
    Model model = soundStack();

    // A person wider than the balcony is deep.
    model.clearWidth = 6;

    const std::vector<access::Fault> faults = access::validate(model);

    check(has(faults, FaultKind::PortalPinched) || has(faults, FaultKind::NoWayThrough), "a balcony too narrow to walk along is caught");
  }

  {
    Model model = soundStack();

    // Cut the first balcony clean in two with a hole across its whole depth,
    // leaving the ramp head on one side and the door on the other.
    access::Surface &balcony = model.surfaces[1];
    const double h = balcony.height;
    double lowest = 1e30, highest = -1e30, nearZ = 1e30, farZ = -1e30;

    for (const Position &p : balcony.outer)
    {
      lowest = std::min(lowest, p.x);
      highest = std::max(highest, p.x);
      nearZ = std::min(nearZ, p.z);
      farZ = std::max(farZ, p.z);
    }

    // Between the head of the ramp and the door, so the two end up on opposite
    // sides of it. A hole somewhere else would leave them both reachable and
    // prove nothing.
    const double rampAt = (model.ramps[0].headLeft.x + model.ramps[0].headRight.x) * .5;
    const double doorAt = model.portals[0].at.x;
    const double mid = std::max(lowest + .9, std::min(highest - .9, (rampAt + doorAt) * .5));

    // The full depth of it, but for a hair at each edge: a hole that touches
    // the boundary is not a hole.
    balcony.holes.push_back({Position{mid - .7, h, nearZ + .02}, Position{mid - .7, h, farZ - .02}, Position{mid + .7, h, farZ - .02}, Position{mid + .7, h, nearZ + .02}});

    const std::vector<access::Fault> faults = access::validate(model);

    check(has(faults, FaultKind::NoWayThrough) || has(faults, FaultKind::Unreachable), "a balcony cut in two by its own hole is caught");
  }

  {
    Model model = soundStack();

    // Push the first balcony back through the wall - towards the middle of the
    // building's own mass, rather than in a direction written out here. Which
    // way a wall faces is the builder's business; a break that guesses it
    // proves nothing, and two of these already did.
    double intoX = 0, intoZ = 0;

    for (const Position &p : model.solids.front().footprint)
    {
      intoX += p.x / double(model.solids.front().footprint.size());
      intoZ += p.z / double(model.solids.front().footprint.size());
    }

    double fromX = 0, fromZ = 0;

    for (const Position &p : model.surfaces[1].outer)
    {
      fromX += p.x / double(model.surfaces[1].outer.size());
      fromZ += p.z / double(model.surfaces[1].outer.size());
    }

    const double away = std::hypot(intoX - fromX, intoZ - fromZ);
    const double stepX = (intoX - fromX) / away * 3, stepZ = (intoZ - fromZ) / away * 3;

    for (Position &p : model.surfaces[1].outer)
    {
      p.x += stepX;
      p.z += stepZ;
    }

    for (std::vector<Position> &hole : model.surfaces[1].holes)
    {
      for (Position &p : hole)
      {
        p.x += stepX;
        p.z += stepZ;
      }
    }

    check(has(access::validate(model), FaultKind::SurfaceInSolid), "a balcony reaching inside the building is caught");
  }

  {
    Model model = soundStack();

    // ...and a ramp pushed through it, the same way.
    double intoX = 0, intoZ = 0;

    for (const Position &p : model.solids.front().footprint)
    {
      intoX += p.x / double(model.solids.front().footprint.size());
      intoZ += p.z / double(model.solids.front().footprint.size());
    }

    const double fromX = (model.ramps[0].footLeft.x + model.ramps[0].headRight.x) * .5;
    const double fromZ = (model.ramps[0].footLeft.z + model.ramps[0].headRight.z) * .5;
    const double away = std::hypot(intoX - fromX, intoZ - fromZ);

    for (Position *p : {&model.ramps[0].footLeft, &model.ramps[0].footRight, &model.ramps[0].headLeft, &model.ramps[0].headRight})
    {
      p->x += (intoX - fromX) / away * 3;
      p->z += (intoZ - fromZ) / away * 3;
    }

    check(has(access::validate(model), FaultKind::RampInSolid), "a ramp passing through the building is caught");
  }

  {
    Model model = soundStack();

    // Carry the balcony rail across the head of the ramp that arrives there:
    // a handrail over the top step. The geometry stays faultless in every
    // other way, which is exactly why this needs its own check.
    const access::Ramp &ramp = model.ramps[0];

    access::Barrier across;
    across.id = 900;
    across.a = ramp.headLeft;
    across.b = ramp.headRight;
    across.base = ramp.headLeft.y;
    across.height = 1.0;
    across.wall = false;

    model.barriers.push_back(std::move(across));

    check(has(access::validate(model), FaultKind::EntranceBlocked), "a rail across the top of a ramp is caught");
  }

  {
    Model model = soundStack();

    // ...and the same across a doorway.
    const access::Portal &portal = model.portals[0];

    access::Barrier across;
    across.id = 901;
    across.a = Position{portal.at.x - .6, portal.at.y, portal.at.z + portal.outZ * .4};
    across.b = Position{portal.at.x + .6, portal.at.y, portal.at.z + portal.outZ * .4};
    across.base = portal.at.y;
    across.height = 1.0;
    across.wall = true;

    model.barriers.push_back(std::move(across));

    check(has(access::validate(model), FaultKind::EntranceBlocked), "and a wall across a doorway");
  }

  {
    Model model = soundStack();

    check(!model.barriers.empty(), "a sound stack has rails on it at all");

    // Take them all away: every balcony is then a shelf you walk off.
    model.barriers.clear();

    check(has(access::validate(model), FaultKind::UnguardedEdge), "a balcony with no rail on its edge is caught");
  }

  {
    Model model = soundStack();

    // ...and take away only the rail round a stairwell, leaving the outside
    // edge railed. This is the one a drawing never shows: the balcony looks
    // finished from every direction and has an open hole in the middle of it.
    size_t removed = 0;

    for (size_t i = model.barriers.size(); i-- > 0;)
    {
      const access::Barrier &rail = model.barriers[i];
      const double mx = (rail.a.x + rail.b.x) * .5, mz = (rail.a.z + rail.b.z) * .5;

      for (const access::Surface &surface : model.surfaces)
      {
        if (std::abs(surface.height - rail.base) > .1)
        {
          continue;
        }

        for (const std::vector<Position> &hole : surface.holes)
        {
          double nearest = 1e30;

          for (size_t k = 0; k < hole.size(); ++k)
          {
            const Position &a = hole[k];
            const Position &b = hole[(k + 1) % hole.size()];
            const double ex = b.x - a.x, ez = b.z - a.z;
            const double span = ex * ex + ez * ez;
            const double t = span > 1e-12 ? std::max(0., std::min(1., ((mx - a.x) * ex + (mz - a.z) * ez) / span)) : 0.;
            nearest = std::min(nearest, std::hypot(a.x + ex * t - mx, a.z + ez * t - mz));
          }

          if (nearest < .1 && removed == 0)
          {
            model.barriers.erase(model.barriers.begin() + long(i));
            removed += 1;
          }
        }
      }
    }

    if (removed == 0)
    {
      check(false, "the stack has no rail round a stairwell to take away");
    }
    else
    {
      check(has(access::validate(model), FaultKind::UnguardedEdge), "and an open stairwell with the outside still railed is caught");
    }
  }

  {
    Model model = soundStack();

    // Take the first ramp away: everything above it is then unreachable.
    model.ramps.erase(model.ramps.begin());

    const std::vector<access::Fault> faults = access::validate(model);

    check(has(faults, FaultKind::Unreachable), "a door no walk from the ground reaches is caught");

    size_t stranded = 0;

    for (const access::Fault &fault : faults)
    {
      stranded += fault.kind == FaultKind::Unreachable ? 1 : 0;
    }

    check(stranded == 3, "and taking the bottom ramp away strands every household above it");
  }
}

// ---------------------------------------------------------------------------
// The walkable region, on its own.
// ---------------------------------------------------------------------------
void walkableCases()
{
  std::cout << "\nThe walkable part of a surface\n";

  access::Surface strip;
  strip.id = 1;
  strip.height = 0;
  strip.outer = {{0, 0, 0}, {10, 0, 0}, {10, 0, 2}, {0, 0, 2}};

  std::vector<std::vector<Position>> pieces;

  check(access::walkableRegion(strip, .9, pieces) && pieces.size() == 1, "a plain strip walks in one piece");

  // A hole that leaves a metre either side of it: still one piece, round the
  // hole.
  strip.holes.push_back({{4, 0, .5}, {4, 0, 1.5}, {6, 0, 1.5}, {6, 0, .5}});
  check(access::walkableRegion(strip, .4, pieces) && pieces.size() == 1, "a strip with a hole in the middle of it still walks round the hole");

  // ...and the same hole against a person wide enough that neither side fits.
  check(access::walkableRegion(strip, .9, pieces) && pieces.size() == 2, "and is two pieces when nobody fits either side of it");

  // A strip narrower than a person has no walkable part at all.
  access::Surface ledge;
  ledge.id = 2;
  ledge.height = 0;
  ledge.outer = {{0, 0, 0}, {10, 0, 0}, {10, 0, .5}, {0, 0, .5}};

  check(access::walkableRegion(ledge, .9, pieces) && pieces.empty(), "a ledge narrower than a person has no walkable part");
}

} // namespace

int main(int argc, char **argv)
{
  std::string document;

  stackCases(document);

  std::string tower;
  towerCases(tower);

  triangleCases();
  breakingCases();
  walkableCases();

  if (argc > 1 && !document.empty())
  {
    std::ofstream file(argv[1]);
    file << document;
    std::cout << "\nwrote the four floor stack to " << argv[1] << "\n";
  }

  if (argc > 2 && !tower.empty())
  {
    std::ofstream file(argv[2]);
    file << tower;
    std::cout << "wrote the tower to " << argv[2] << "\n";
  }

  std::cout << "\n" << (failures == 0 ? "all checks passed" : std::to_string(failures) + " checks failed") << "\n";

  return failures == 0 ? 0 : 1;
}
