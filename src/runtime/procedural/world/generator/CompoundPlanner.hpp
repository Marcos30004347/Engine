#pragma once

#include "Ledger.hpp"
#include "PatchRegion.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"

#include <functional>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A2 compound structures inside a reserved precinct: castle (keep, curtain
// wall, corner towers, gatehouse, bailey, hall and store ranges), manor (hall
// and service wings around a court behind a garden wall), church (nave,
// tower, low precinct wall) and civic hall. Every inhabited wing is a
// rectangular dwelling brief planned by the dwelling planner, so rooms,
// doors and stairs come from the same rules as houses.
// Fit one convex precinct across adjoining world planning cells. Shared cell
// seams are not streets and must not receive independent building margins.
bool fitCompoundPrecinct(const std::vector<ConvexRing> &pieces,double margin,ConvexRing &out,std::string *error=nullptr,double minimumWidth=26,double minimumDepth=22);

enum class CompoundKind : uint8_t
{
  Castle,
  Manor,
  Church,
  Hall,
  Monastery
};

struct CompoundWall
{
  Position a, b;
  double thickness = 1.5, height = 6;
  bool gate = false; // Opening segment.
};

struct CompoundTower
{
  ConvexRing footprint;
  double height = 9;
};

struct CompoundWing
{
  std::string role; // keep, hall, stable, store, chapel, nave, tower, service...
  rewrite::DwellingBrief brief;
  rewrite::DwellingPlan plan;
  bool planned = false;
  std::string note;
};

struct DungeonRoom
{
  uint32_t id=0, level=0;
  std::string role;
  ConvexRing footprint;
  double floor=0;
};
struct DungeonConnection
{
  uint32_t from=0,to=0; // Zero is the keep entrance; room IDs start at one.
  Position position;
  bool stairs=false;
};

enum class LandmarkLayout : uint8_t { Citadel, GardenManor, Palace };
enum class KeepPlacement : uint8_t { Automatic, Detached, WallAttached };
struct CompoundYard { ConvexRing footprint; std::string role; };
struct CompoundWallAccess { uint64_t wing=0; Position at; double width=3.; };
struct CompoundGarden { ConvexRing footprint; };
struct CompoundPath { Position a,b; double width=4; };

struct CompoundTerrace { ConvexRing footprint; double level=0; };
struct CompoundSiteSolid { ConvexRing footprint; double bottom=0,top=0; bool guard=false; };
struct CompoundTerraceLink { uint32_t from=0,to=0,steps=0; Position foot,head; double width=0; };

struct CompoundPlan
{
  uint64_t id = 0;
  CompoundKind kind = CompoundKind::Manor;
  ConvexRing precinct;  // Local frame of the caller.
  ConvexRing enclosure; // Wall line (castle/manor/church) as a ring.
  std::vector<CompoundWall> walls;
  std::vector<CompoundTower> towers;
  std::vector<CompoundWing> wings;
  std::vector<ConvexRing> courts;
  Position gate;
  uint32_t plannedWings = 0;
  bool solved = false;
  bool fantasyGothic=false;
  bool gothicFacades=false; // Compact precincts retain Gothic architecture without the monumental layout.
  LandmarkLayout landmarkLayout=LandmarkLayout::Citadel;
  bool wallAttachedKeep=false;
  std::vector<CompoundYard> yards;
  std::vector<CompoundWallAccess> wallAccess;
  std::vector<CompoundGarden> gardens;
  std::vector<Position> fountains;
  std::vector<CompoundPath> paths;
  std::vector<CompoundTerrace> terraces;
  std::vector<CompoundSiteSolid> siteSolids;
  std::vector<CompoundTerraceLink> terraceLinks;
  std::vector<DungeonRoom> dungeonRooms;
  std::vector<DungeonConnection> dungeonConnections;
};

struct CompoundBrief
{
  uint64_t id = 0, seed = 1;
  CompoundKind kind = CompoundKind::Manor;
  ConvexRing precinct;   // Convex, counter-clockwise, caller's local frame.
  Position approach;     // Point on or near the precinct edge the street reaches.
  double foundation = 0; // Terrace level for the wings.
  bool fantasyGothic=false;
  LandmarkLayout landmarkLayout=LandmarkLayout::Citadel;
  uint32_t dungeonLevels=3;
  KeepPlacement keepPlacement=KeepPlacement::Automatic;
  uint32_t extraStoreys=0;
  uint32_t terraceBands=0; // 0 retains a single court; 3–5 reserves a climbing route first.
};

bool planCompound(const CompoundBrief &, const std::function<double(double, double)> &terrain, CompoundPlan &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
