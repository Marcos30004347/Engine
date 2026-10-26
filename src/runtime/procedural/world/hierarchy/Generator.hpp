#pragma once

#include "procedural/world/generator/Ledger.hpp"
#include "procedural/world/hierarchy/Region.hpp"

#include <functional>
#include <memory>
#include <string>

namespace procedural::world::hierarchy {

using generator::Ledger;
using generator::LedgerKind;

// What a generator is for. The role, not the implementation: several
// generators bind to one role and the policy picks between them per region,
// which is what makes "a different block generator for this district" a
// binding rather than a new flag and a new precedence rule.
enum class Role : uint8_t
{
  WorldPartition,  // The map's cells.
  SettlementSites, // Where settlements go.
  Districts,       // A settlement into districts.
  Streets,         // The road network inside a region.
  Blocks,          // Districts into blocks.
  Lots,            // Blocks into lots.
  Buildings,       // Lots into building footprints.
  Fields,          // Hinterland into farm fields.
  Interiors,       // A building into rooms.
  Terrain          // Attributes over regions.
};

const char *roleName(Role);

// How far a generator may look. Already the right distinction: a chord closes
// inside one region, a road crosses many, a river crosses a watershed.
enum class Scope : uint8_t
{
  Region,        // Reads and writes one region. The parallel grain.
  Neighbourhood, // Reads its region and those across its portals.
  Network,       // Spans a connected network; roads, rivers.
  World
};

struct Descriptor
{
  std::string id; // Stable name; what the registry binds and the ledger records.
  Role role = Role::Lots;
  Scope scope = Scope::Region;
  uint32_t version = 1;

  // Explicit opt-in: immutable inputs, no captured mutable state, and a
  // result that does not depend on the order regions are visited.
  bool parallelSafe = false;
};

// The context a generator reads. Passed rather than captured, so that making a
// generator parallel-safe is a statement about this struct and not an audit of
// nine hand-wired lambdas.
struct Context
{
  uint64_t worldSeed = 1;

  // Terrain height in metres, in the region's local frame. May be empty.
  std::function<double(double, double)> terrain;

  std::function<bool()> cancelled;

  // Purpose-addressed randomness. Derived from the world seed, the generator
  // id and the region id rather than from traversal order, so a result does
  // not depend on when it was computed.
  double unitRandom(const std::string &generator, RegionId region, uint64_t purpose, uint64_t sample = 0) const;
};

struct Request
{
  const Region &region;
  const RegionTree &tree;
  const Context &context;
};

// ---------------------------------------------------------------------------
// The three interfaces.
//
// Not one, and not four. An earlier draft separated "partition" from
// "network"; that was wrong. A partition and a network produce the same thing -
// edges - and the child regions are what you get by extracting faces
// afterwards. A chord from a lot splitter and a segment from a road L-system
// are the same operation at different scopes, and the codebase already proves
// it by extracting faces from a road graph and then forgetting that the face
// boundary *is* the road.
// ---------------------------------------------------------------------------

class GeneratorBase
{
public:
  virtual ~GeneratorBase() = default;

  virtual Descriptor descriptor() const = 0;
};

// Proposes edges inside (or across) regions. The faces are derived, not
// authored: the composer turns the proposal into child regions.
//
// Covers lot splitting, ward planning, block cutting, street growth, river
// routing and map partition alike.
class EdgeGenerator : public GeneratorBase
{
public:
  virtual bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) = 0;
};

// Places objects in a region without partitioning it: a farmstead in a field,
// a compound in a precinct, a tree in a wood.
// Three-dimensional geometry for an object that has any.
//
// An Object's footprint is a plan, and a plan cannot say where a roof is. The
// town's 3D view therefore re-derived each building's massing from its
// footprint - wings, storeys, roof, the lot - which meant the rules were
// written twice, in the plot planner and again in whatever was drawing. They
// drifted, as duplicated rules do: narrow houses silently lost a storey in the
// drawing that they had in the plan.
//
// So the geometry is produced once, by the layer that plans the building, and
// carried. Deliberately nothing but points grouped by surface: the hierarchy
// does not need to know what a wing or a ridge is, only that a building has
// walls and roof faces, and a consumer that only wants the plan can ignore it.
struct Solid
{
  // `HalfTimber` is plaster with the frame IN THE TEXTURE. The frame used to
  // be geometry - a post, a plate and a brace per panel, thousands of beams a
  // city - and at any distance it read as a scribble of dark lines on a flat
  // wall rather than as timber. A tile one bay wide by one storey tall does
  // the same job, and the panel keeps its plaster.
  enum class SurfaceMaterial : uint32_t { Unassigned, Stone, RoofTile, Wood, Plaster, Glass, Paving, DarkStone, Door, CarvedStone, Boarding, VaultMasonry = 14, TowerMasonry = 15, HalfTimber = 16 };
  struct Vertex
  {
    double x = 0, y = 0, z = 0;
    // Face-corner UVs in metres. Repeat scale belongs to the material, not
    // to polygon dimensions. Vertices at seams are deliberately duplicated.
    double u = 0, v = 0;
    SurfaceMaterial material = SurfaceMaterial::Unassigned;
    uint32_t uvIsland = 0;
    bool authoredUV = false; // Preserve fitted asset UVs through the generic mapping pass.
  };

  using Face = std::vector<Vertex>;

  std::vector<Face> walls, innerWalls, slopes, gables, soffits, pillars;

  // What fills an opening. Appended at the end, never inserted: a wall split
  // into panels around its windows leaves the window itself unfilled, and a
  // hole in a facade reads as a fault rather than as glass.
  std::vector<Face> joinery;

  // ---- the inside -----------------------------------------------------------
  //
  // A wall with thickness has a RETURN at every hole cut in it - the two jambs,
  // the head and the sill - and without them the wall is a zero-thickness card
  // and every window shows the room behind through a knife edge. They are their
  // own group because they are the only surfaces in a facade that face along
  // the wall rather than across it, and anything shading this wants to know
  // that.
  std::vector<Face> reveals, innerReveals;

  // The floor you stand on, the flights between them, and the timber that is
  // not part of a wall: a ladder, a post under a long span.
  std::vector<Face> decking, stairs, timbers, furniture;

  // Kept separate even in the simple mesh so the next detail pass can replace
  // the shell without trying to separate it from the house roof and walls.
  std::vector<Face> dormerWalls, dormerWindows, dormerRoofs, dormerCeilings;

  // Outdoor ground that belongs to the building: a yard, a court. Drawn as
  // ground and not as floor, which is why it is not `decking`.
  std::vector<Face> yard;
  std::vector<Face> foundations;

  // The planner declined to roof at least one of this building's masses,
  // because its plan has a reflex corner that is not a right angle and the
  // roof is only exact where they are. Carried so that a building standing
  // bare can be told from one whose roof went missing.
  bool roofRefused = false;

  bool empty() const
  {
    return walls.empty() && innerWalls.empty() && slopes.empty() && gables.empty() && soffits.empty() &&
           pillars.empty() && joinery.empty() && reveals.empty() && innerReveals.empty() &&
           decking.empty() && stairs.empty() && timbers.empty() && furniture.empty() &&
           dormerWalls.empty() && dormerWindows.empty() && dormerRoofs.empty() && dormerCeilings.empty() && yard.empty() && foundations.empty();
  }
};

struct Structure;
struct Skeleton;
struct DwellingVoxelAssembly;

struct Object
{
  std::string kind;
  Polygon footprint;
  Point anchor;
  double heightMetres = 0;
  RegionId region = kNoRegion;

  // The built form, when the generator that placed this knew it. Shared
  // because an Object is copied through the composer and a building's
  // surfaces are not small.
  std::shared_ptr<const Solid> solid;

  // The structure the solid was meshed from (P24), when there was one: the
  // typed faces the skeleton view draws beside the mesh (P26).
  std::shared_ptr<const Structure> structure;

  // The complete middle-phase artefact: polygons, pieces, piece contacts and
  // room connectivity. `structure` above aliases this object's structure for
  // old consumers and the P26 renderer.
  std::shared_ptr<const Skeleton> skeleton;
  // Source-indexed voxel part assembly, emitted while the CellHouse and
  // skeleton still coexist. Its face indices address `skeleton` above.
  std::shared_ptr<const DwellingVoxelAssembly> voxelAssembly;
};

class RegionPopulator : public GeneratorBase
{
public:
  virtual bool populate(const Request &, std::vector<Object> &out, Ledger &, std::string *error) = 0;
};

// Writes attributes over regions without changing any geometry: climate,
// elevation, suitability, demand.
class FieldGenerator : public GeneratorBase
{
public:
  virtual bool evaluate(const RegionTree &, const Context &, std::vector<Region *> &regions, Ledger &, std::string *error) = 0;
};

using GeneratorPtr = std::shared_ptr<GeneratorBase>;

} // namespace procedural::world::hierarchy
