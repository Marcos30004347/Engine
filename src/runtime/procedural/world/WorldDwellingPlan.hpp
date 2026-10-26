#pragma once
#include "WorldGenerationContracts.hpp"
#include <functional>
#include <string>
#include <vector>
namespace procedural::world::rewrite
{
enum class DwellingUse
{
  Cottage,
  ShopHouse,
  Lodging,
  Hall,
  Warehouse
};
enum class RoomUse
{
  Living,
  Sleeping,
  Shop,
  Store,
  Common,
  Service,
  Hall
};
struct DwellingBrief
{
  uint64_t id = 0, seed = 1;
  DwellingUse use = DwellingUse::Cottage;
  std::vector<Position> shell; // Accepted rectangular wing, CCW, metres.
  Position entrance;           // Fixed on ground-floor exterior wall.
  double foundation = 0, floorRise = 2.8, availableHeight = 6, wallThickness = .2;
  uint32_t floors = 1;
};
struct DwellingRoom
{
  uint64_t id = 0;
  uint32_t floor = 0;
  RoomUse use = RoomUse::Living;
  std::vector<Position> boundary;
  double usableArea = 0;
};
struct DwellingWall
{
  uint64_t id = 0;
  uint32_t floor = 0;
  Position a, b;
  double thickness = .2;
  bool exterior = false;
};
struct DwellingDoor
{
  uint64_t wall = 0, room = 0;
  uint32_t floor = 0;
  Position center;
  double width = .9;
  bool privateAccess = false, exterior = false;
};
struct DwellingStair
{
  std::vector<Position> opening, lowerLanding, upperLanding;
  uint32_t lowerFloor = 0, risers = 0;
  double rise = 0, tread = .27, width = 1.1;
  bool privateAccess = false;
};
struct DwellingFloor
{
  double elevation = 0, height = 2.8, grossArea = 0, roomArea = 0, circulationArea = 0, coreArea = 0, wallArea = 0;
  std::vector<std::vector<Position>> circulation, slabs;
};
struct DwellingPlan
{
  uint64_t id = 0, seed = 0;
  DwellingUse use = DwellingUse::Cottage;
  std::vector<DwellingRoom> rooms;
  std::vector<DwellingWall> walls;
  std::vector<DwellingDoor> doors;
  std::vector<DwellingStair> stairs;
  std::vector<DwellingFloor> floors;
  uint32_t residentCapacity = 0, workerCapacity = 0;
};
struct DwellingTriangle
{
  Position a, b, c;
  uint32_t material = 0;
};
// Bounded rectangular-wing qualification. Voronoi cells within room bands use
// the same MapPartition implementation as the world map. Unsupported shells,
// fixed doors, programmes or stairs fail atomically; no forced tiny rooms.
bool generateDwellingPlan(const DwellingBrief &, DwellingPlan &, std::string *error = nullptr);
bool meshDwellingPlan(const DwellingPlan &, std::vector<DwellingTriangle> &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
