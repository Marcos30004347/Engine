#pragma once

#include "WorldCityCompletion.hpp"
#include "WorldCityConstruction.hpp"
#include "WorldSettlementFabricAdapter.hpp"
#include "WorldCityRelief.hpp"
#include "WorldCitySurfaces.hpp"
#include "WorldCityVoxelBoundaries.hpp"

#include <functional>
#include <cstddef>

namespace procedural::world::hierarchy { struct DwellingVoxelAssembly; struct Skeleton; }

namespace procedural::world::rewrite {

enum class CityModelBuildingKind : uint8_t
{
  House,
  Workshop,
  Civic,
  Religious,
  CastleRoom,
  Keep,
  Gatehouse,
  DefenceTower,
  Mansion,
  Manor,
  Inn,
  Pub
};

// One room of a generated building, by the use and kind the programme gave
// it, so a person's day can be bound to a room and not only to a building.
struct CityModelRoom
{
  uint64_t id = 0;
  uint32_t floor = 0;
  std::string use, kind;
  std::vector<Position> boundary; // Exact realised room, in settlement coordinates.
  double area = 0;
};

struct CityModelBuilding
{
  uint64_t id = 0, source = 0;
  CityModelBuildingKind kind = CityModelBuildingKind::House;
  std::vector<Position> footprint;
  double base = 0, wallHeight = 0, roofHeight = 0;
  bool central = false, wallAttached = false;
  uint32_t pieces = 0, contacts = 0, rooms = 0, roomConnections = 0;
  uint32_t windows=0, chapels=0, chancels=0;
  EntranceLayout entranceLayout=EntranceLayout::Ground;
  double entranceRise=0;
  std::vector<uint32_t> entranceCells;
  DwellingSiteAccess siteAccess;
  std::vector<Position> exteriorDoors;
  std::vector<Position> interiorViews; // Eye-height samples inside generated room polygons.
  std::vector<double> jettyMetres;
  std::string trade;                   // The lot's trade tag, when it has one.
  std::vector<CityModelRoom> roomList; // Every room of the realised plan.
  std::string architecture="Vernacular";
  uint32_t architecturalAgeYears=0;
  double weathering=0;
  uint32_t colonnadeColumns=0,cofferBeams=0;

};

// The room a person's activity happens in, chosen by what the room is for:
// work in the trade's room, sleep in a bedroom, meals in the kitchen or the
// hall, the rest of the day in the living room. Never guessed from a count:
// a building with no room of the right kind gives no room.
inline std::optional<uint64_t> bindActivityRoom(const CityModelBuilding &building, const std::string &activity)
{
  const auto pick = [&](const std::vector<const char *> &kinds) -> std::optional<uint64_t> {
    for (const char *kind : kinds)
    {
      for (const auto &room : building.roomList)
      {
        if (room.kind == kind)
        {
          return room.id;
        }
      }
    }
    return std::nullopt;
  };
  if (activity == "work" || activity == "apprenticeship")
  {
    return pick({"Forge", "Bakehouse", "Workshop", "Brewhouse", "Shop", "CountingHouse", "Guardroom", "Byre", "Barn", "Dairy", "Bar", "TapRoom", "Office", "Nave", "Kitchen", "GreatHall", "LivingRoom"});
  }
  if (activity == "sleep")
  {
    return pick({"Bedroom", "Dormitory", "Nursery", "Cell", "LivingRoom", "Parlour"});
  }
  if (activity == "meal")
  {
    return pick({"Kitchen", "DiningRoom", "Hall", "GreatHall", "Refectory", "LivingRoom", "Parlour"});
  }
  return pick({"LivingRoom", "Parlour", "Hall", "Kitchen", "Bedroom"});
}

// The deliberately simple third phase of the city pipeline. Completion owns
// topology and contact metadata; developed lots request dwelling capacity
// based on district and fit it through blueprint -> skeleton -> mesh.
// Compound wings are meshed from the
// dwelling plans they already carry, so castles/churches/halls keep their
// rooms, doors, stairs, ceilings and roofs in the structural source of truth.
struct CityBuildingLink {
  uint64_t from=0,to=0;
  Position a,b;
  double width=1.5;
};

struct CityWallAccessLink {
  uint64_t wing=0;
  Position wall,door;
  double clearWidth=.9;
  uint32_t floor=0;
  std::vector<Position> path; // Includes a landing when the wall and door do not face each other.
};

struct CityModelPlan
{
  CityConstructionPlan construction; // Candidate plus explicit phase-2 diagnostics.
  using GroundWeights = std::array<std::array<double,3>,3>;
  uint64_t revision = 0;
  // Four corners of a reusable two-triangle panel, metric UVs, group and material.
  // Kept separate from mesh triangles: no welding, triangle UV or ground-weight copies.
  using QuadInstance = std::array<double,22>;
  std::vector<QuadInstance> quadInstances;
  SplineFaceMesh mesh;
  std::vector<uint32_t> materials;
  // Per-triangle corners preserve UV seams despite position welding.
  std::vector<std::array<std::array<double,2>,3>> triangleUVs;
  std::vector<uint32_t> surfaceMaterials;
  // Fixed palette indices, never interpolate material IDs. Zero weights mean
  // ordinary building material; ground corners sum to one.
  std::array<uint32_t,3> groundMaterialIds{11,12,13};
  std::vector<GroundWeights> groundWeights;
  std::vector<CityModelBuilding> buildings;
  std::vector<uint64_t> functionalProps; // Accepted composition identities emitted in this model.
  std::vector<CityBuildingLink> buildingLinks;
  std::vector<CityWallAccessLink> wallAccessLinks;
  uint32_t wallTriangles = 0, towerTriangles = 0, gatehouseTriangles = 0;
  uint32_t lotTriangles = 0, compoundTriangles = 0, roadTriangles = 0;
  uint32_t lotBlueprints = 0, lotStructures = 0, lotMeshes = 0, lotRejected = 0;
  uint32_t gatewayHouses = 0;
  uint32_t structuralPieces = 0, structuralContacts = 0, structuralRooms = 0, structuralRoomConnections = 0;
  std::vector<std::string> rejections;
  CitySurfaceStructure surfaces;
  std::vector<std::array<Position,2>> elevationLines; // Planned constraints, before CDT.
  // Parallel to mesh triangles; zero denotes a non-road face. A single
  // network mesh can be batched by owner/segment without duplicating junctions.
  std::vector<uint64_t> roadOwners;
  std::vector<uint32_t> roadSegments;
};

// A named, unique fallback for building geometry authored outside the cell
// dwelling system. Triangle indices refer to CityModelPlan::mesh and its
// parallel material arrays while the synchronous sink is executing.
struct CityVoxelUniqueMeshPart
{
  uint64_t buildingId = 0;
  CityModelBuildingKind kind = CityModelBuildingKind::House;
  std::string role;
  size_t firstTriangle = 0;
  size_t triangleCount = 0;
};

struct CityModelSettings
{
  // Called synchronously while the source skeleton is alive. Consume or copy
  // the source-indexed assembly before returning; returning false fails the
  // city build. Keep party boundary faces provisional until voxelBoundarySink
  // supplies the accepted, physically paired contacts. This keeps detailed
  // source memory proportional to one lot, not the city.
  std::function<bool(const hierarchy::DwellingVoxelAssembly &, const hierarchy::Skeleton &,
                     std::string *)> voxelAssemblySink;
  // The mesh span and parallel materials are valid only for this call. The
  // compiler writes a unique asset immediately instead of retaining the city
  // mesh or copying a building-sized subset.
  std::function<bool(const CityVoxelUniqueMeshPart &, const CityModelPlan &,
                     std::string *)> voxelUniqueMeshSink;
  // Perimeter relief samples are streamed per building while its footprint
  // and support field are alive. The unique foundation mesh owns volume above
  // ground; terrain owns the common interface and volume below it.
  std::function<bool(uint64_t, const std::vector<CityVoxelFoundationSample> &,
                     std::string *)> voxelFoundationSink;
  // Called once after all buildings have been accepted. The party graph is
  // final here, so an absent neighbour cannot remove an exterior wall.
  // Source polygons/mesh and `terrainHeight` are valid only during this
  // synchronous callback; sample global integer border vertices with
  // cityVoxelTerrainSample and clip features from their source indices.
  std::function<bool(const CityVoxelBoundaryPlan &, const CityModelPlan &,
                     const CityCompletionPlan &, const CityReliefPlan *,
                     const std::function<double(double,double)> &terrainHeight,
                     std::string *)> voxelBoundarySink;
  bool instanceDwellingPanels = false; // Opt-in for consumers supporting compact instances.
  uint32_t maximumWorkers = 2;
  uint32_t maximumResidentLots = 4; // Bound temporary meshes before merging.
  double lotInset = .9;
  double contourIntervalMetres = 2.;
  bool textureStreets = true; // Paint circulation into the terrain material mask.
  bool polygonTerrain = true; // World Voronoi facets constrained by road and lot polygons.
  bool raisedLotAprons = false; // Optional full-lot decks; house foundations remain supported.
  double houseWallHeight = 4.8;
  double roofRise = 2.2;
  uint32_t towerSides = 10;
  // How far apart ground vertices may be before an edge is split. Every edge
  // longer than this is halved, whichever triangle it belongs to, so the
  // number is a triangle budget as much as a fidelity one: at 1.5 m the city
  // surface is four and a half million triangles, at 6 m it is under half a
  // million and the streets read as the low-poly surfaces they are.
  double groundVertexSpacing = 6.;
  // Height of the urban paving slab above flush streets. The historical
  // configuration name is retained; this no longer excavates the terrain.
  double streetCarveMetres = .09;
  // Optional raised sidewalk inside the connected road reservation. Zero
  // preserves the entire road ribbon and uses the raised block/lot apron.
  double streetPavementMetres = .6; // Raised flagstone margins, bounded by lane width.
  // Enclose substantial private yards, preserving public paths and entrances.
  bool lotBoundaries = false; // Private yard fences remain opt-in until their replacement.
  bool streetDetails = true; // Awnings, signs, benches, barrels and yard props.
  // Set the finished house onto the shape of its lot: the lot's bounding
  // rectangle is mapped onto its four corners and the house goes with it, so
  // a square plan fills a skewed plot and meets its neighbours along the
  // party line. Off leaves every house square and the wedges with it.
  // Bending the finished house onto its lot. Off by default: measured, the
  // map compressed a tenth of the houses it touched to half the dimensions
  // the plan gave them, and a plan is dimensioned in metres - a door is 0.9,
  // a corridor is sized for two abreast. Aligning the PLAN to the lot beats
  // deforming the result, and that is what the shared grid axis below does.
  bool warpToLot = false;
  // How far the lot map may open its quad out to avoid compressing the house.
  // A plan is dimensioned in metres - a door is 0.9, a corridor is sized for
  // two people abreast - and a map that shrinks them is a map that breaks
  // them; but opened too far the house crosses its own lot line.
  double maximumWarpOpening = 1.14;
  // A lot the map would still compress past this keeps a square house. A door
  // is 0.9 m and a corridor is sized for two abreast; shrinking either is not
  // a trade worth making for the shape of a plot.
  double minimumWarpScale = .94;
  // How far a doorway may stand above its street before the lot is refused.
  // A climb past this is a staircase taller than the house it serves.
  double maximumEntranceClimbMetres = 5.;
  bool groundOnly = false; // Streets, blocks and lots; skip all architectural meshes.
  bool infrastructureOnly = false; // Retain fortifications; skip dwelling/compound meshes.
};

// The relief plan, when the settlement stands on a hill, supplies the ground:
// streets graded down it, lot pads horizontal, the retaining band ramped. A
// null plan (or one with no ground) leaves the fabric's own surface in place.
bool buildCityModel(
    const CityCompletionPlan &,
    const SettlementFabric &,
    const CityModelSettings &,
    CityModelPlan &,
    std::string *error = nullptr,
    const CityReliefPlan *relief = nullptr);

const char *cityModelBuildingKindName(CityModelBuildingKind);
// Inspection mesh for one dungeon level; ceilings are deliberately removed.
bool buildCastleDungeonCutaway(const generator::CompoundPlan &, uint32_t level, CityModelPlan &,
                              std::string *error=nullptr, const CityModelSettings *voxelSettings=nullptr);

// Append the portion inside a tile, splitting crossing triangles without
// discarding their surface area. Coordinates stay in the city frame.
void appendCityModelTile(const CityModelPlan &, double minX, double minZ, double maxX, double maxZ,
                        SplineFaceMesh &, std::vector<uint32_t> &materials,
                        std::vector<std::array<std::array<double,2>,3>> *triangleUVs = nullptr,
                        std::vector<uint32_t> *surfaceMaterials = nullptr,
                        std::vector<CityModelPlan::GroundWeights> *groundWeights = nullptr);

} // namespace procedural::world::rewrite
