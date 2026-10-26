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
  struct Vertex
  {
    double x = 0, y = 0, z = 0;
  };

  using Face = std::vector<Vertex>;

  std::vector<Face> walls, innerWalls, slopes, gables, soffits, pillars;

  bool empty() const
  {
    return walls.empty() && innerWalls.empty() && slopes.empty() && gables.empty() && soffits.empty() && pillars.empty();
  }
};

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
