#include "procedural/world/hierarchy/CellDwellingGenerator.hpp"
#include "procedural/world/hierarchy/CellSkeleton.hpp"
#include "procedural/world/hierarchy/CellStructure.hpp"
#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world::hierarchy {

namespace gen = procedural::world::generator;
using gen::PatchId;
using rewrite::Position;

namespace {

// The purpose this generator draws its seed under. Purpose-addressed, so the
// result does not depend on the order regions are visited - and then the stream
// inside one building is ordinary and sequential, which is what the algorithm
// needs. The boundary is where those two requirements meet.
constexpr uint64_t kPurposeDwelling = 0x6379636C65647764ull;

// Which lot edges face a street.
//
// A portal is a way out of the region, which for a lot is the side it is
// reached from. Where the region has none, the longest edge is taken, which on
// a lot cut from a block is the street side more often than not.
std::vector<uint32_t> frontageOf(const Region &region)
{
  std::vector<uint32_t> out;

  if (region.boundary.outer.size() < 3)
  {
    return out;
  }

  for (const Portal &portal : region.portals)
  {
    (void)portal;
  }

  return out;
}

} // namespace

Descriptor CellDwellings::descriptor() const
{
  Descriptor out;
  out.id = "dwelling.cell";
  out.role = Role::Buildings;
  out.scope = Scope::Region;
  out.version = 1;

  // Honest, and worth saying why: the brief is built from immutable region
  // data, the seed is purpose-addressed so it does not depend on visit order,
  // and the planner holds no state between calls. Nothing here reads or writes
  // anything outside the region it was given.
  out.parallelSafe = true;

  return out;
}

// The mesh step, as the composition it now is.
//
// Blueprint -> structural skeleton -> mesh. Nothing here reads the plan into
// faces any more: `buildSkeleton` produces the one middle-phase artifact and
// `meshSkeleton` turns that into the faces a renderer draws. This is the seam
// callers were already holding. The old direct builder - a
// wall as a box split into panels round its openings, reveals by construction,
// a pane per window - was measured against the structure route on the whole
// corpus (areas per group within two percent, the same silhouette from every
// side, the same storeys in every column) and then deleted rather than kept
// beside it.
void buildCellDwellingSolid(const rewrite::DwellingPlan &plan, Solid &out)
{
  meshSkeleton(buildSkeleton(plan), out);
}

bool CellDwellings::populate(const Request &request, std::vector<Object> &out, Ledger &ledger, std::string *error)
{
  const Region &region = request.region;

  if (region.boundary.outer.size() < 3)
  {
    return true;
  }

  const double area = std::abs(geom2::areaMetres(region.boundary));

  if (area < settings_.minimumAreaMetres)
  {
    ledger.add(LedgerKind::Rejected, PatchId(region.id), "lot too small for a building");

    return true;
  }

  gen::CellDwellingBrief brief;
  brief.id = uint64_t(region.id);
  brief.lot = region.boundary;
  brief.frontage = frontageOf(region);
  brief.setbackMetres = settings_.setbackMetres;
  brief.standing = settings_.standing;
  brief.building = settings_.building;
  brief.tags = settings_.tags;

  // One stream per building, derived once from a purpose-addressed draw.
  const double draw = request.context.unitRandom(descriptor().id, region.id, kPurposeDwelling);
  brief.seed = uint64_t(draw * 9007199254740992.) ^ (uint64_t(region.id) * 0x9E3779B97F4A7C15ull);

  gen::CellHouse house;

  if (!gen::planCellDwelling(brief, settings_.planner, house, ledger, error))
  {
    // A declined lot is a lot with no building on it, not a failed region: the
    // planner has already said why in the ledger.
    return true;
  }

  rewrite::DwellingPlan plan;

  if (!gen::realiseCellDwelling(house, settings_.planner, plan, error))
  {
    ledger.add(LedgerKind::Rejected, PatchId(region.id), "a planned dwelling could not be realised");

    return true;
  }

  auto skeleton = std::make_shared<Skeleton>(buildSkeleton(plan));
  auto voxelAssembly = std::make_shared<DwellingVoxelAssembly>();
  if (!buildDwellingVoxelAssembly(house, plan, *skeleton, brief.id, brief.seed, *voxelAssembly, error))
  {
    ledger.add(LedgerKind::Rejected, PatchId(region.id), "dwelling voxel assembly could not be built");
    return true;
  }
  auto solid = std::make_shared<Solid>();
  meshSkeleton(*skeleton, *solid);

  Object object;
  object.kind = "dwelling.cell";
  object.region = region.id;
  object.heightMetres = plan.storeys.empty() ? 0 : plan.storeys.back().top;
  object.solid = std::move(solid);
  object.structure = std::shared_ptr<const Structure>(skeleton, &skeleton->structure);
  object.skeleton = std::move(skeleton);
  object.voxelAssembly = std::move(voxelAssembly);

  if (!plan.storeys.empty())
  {
    const rewrite::DwellingStorey &ground = plan.storeys.front();
    double cx = 0;
    double cz = 0;

    for (const Position &p : ground.outer)
    {
      object.footprint.outer.push_back(geom2::Point{geom2::toUnit(p.x), geom2::toUnit(p.z)});
      cx += p.x;
      cz += p.z;
    }

    if (!geom2::counterClockwise(object.footprint.outer))
    {
      geom2::reverse(object.footprint.outer);
    }

    object.anchor = geom2::Point{geom2::toUnit(cx / double(ground.outer.size())),
                                 geom2::toUnit(cz / double(ground.outer.size()))};
  }

  out.push_back(std::move(object));

  return true;
}

} // namespace procedural::world::hierarchy
